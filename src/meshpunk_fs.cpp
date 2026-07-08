#include "meshpunk_fs.h"
#include "meshpunk_sync.h"
#include "usb_manager.h"   // UsbFlashGuardIf — pause USB audio around flash writes

#include <SD.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

extern bool sd_mounted;
extern void sd_spi_release();

// Strip S: or L: prefix, set *use_sd accordingly.
// If no prefix, *use_sd = default_sd.
// Also strips leading /sd/ when targeting SD, since SD.open() already
// operates relative to the SD mount point.
const char* meshpunk_parse_prefix(const char* path, bool* use_sd, bool default_sd) {
    *use_sd = default_sd;
    if (path[0] != '\0' && path[1] == ':') {
        if (path[0] == 'S' || path[0] == 's') {
            *use_sd = true;
            return path + 2;
        } else if (path[0] == 'L' || path[0] == 'l') {
            *use_sd = false;
            return path + 2;
        }
    }
    // Strip /sd/ prefix for SD paths — SD.open() adds the mount point itself
    if (*use_sd && strncmp(path, "/sd/", 4) == 0) {
        return path + 3; // keep the leading /
    }
    return path;
}

MeshpunkFile meshpunk_open(const char* path, const char* mode, bool default_sd) {
    MeshpunkFile mf = { {}, false, false };

    bool use_sd;
    const char* actual = meshpunk_parse_prefix(path, &use_sd, default_sd);
    mf.is_sd = use_sd;

    if (use_sd) {
        if (!sd_mounted) return mf;
        sd_spi_take();
        mf.file = SD.open(actual, mode);
    } else {
        // A "w"/"a"/"r+" open of a LittleFS file writes internal flash
        // (truncate / create updates metadata) — that stalls the cache, which
        // crashes an active USB host audio stream. Guarded; no-op when USB
        // isn't running.
        UsbFlashGuardIf _g(mode[0] != 'r' || strchr(mode, '+') != nullptr);
        mf.file = LittleFS.open(actual, mode);
    }

    if (!mf.file) {
        if (use_sd) sd_spi_release();
        return mf;
    }

    mf.valid = true;
    return mf;
}

void meshpunk_close(MeshpunkFile& mf) {
    if (!mf.valid) return;
    mf.file.close();
    if (mf.is_sd) sd_spi_release();
    mf.valid = false;
}

bool meshpunk_mkdirs(const char* path, bool default_sd) {
    bool use_sd;
    const char* actual = meshpunk_parse_prefix(path, &use_sd, default_sd);
    if (use_sd && !sd_mounted) return false;

    char buf[160];
    size_t n = strlen(actual);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, actual, n + 1);

    bool ok = true;
    if (use_sd) sd_spi_take();
    // LittleFS mkdir is an internal-flash (metadata) write — see meshpunk_open.
    UsbFlashGuardIf _g(!use_sd);
    // Create each directory prefix; the final segment is the file name and
    // is not created.
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (use_sd) {
            if (!SD.exists(buf)) ok = SD.mkdir(buf) && ok;
        } else {
            if (!LittleFS.exists(buf)) ok = LittleFS.mkdir(buf) && ok;
        }
        *p = '/';
    }
    if (use_sd) sd_spi_release();
    return ok;
}

void* meshpunk_read_all(const char* path, uint32_t* out_size, bool default_sd) {
    MeshpunkFile mf = meshpunk_open(path, "r", default_sd);
    if (!mf.valid) {
        SLog.printf("[meshpunk_fs] failed to open %s\n", path);
        return NULL;
    }

    uint32_t sz = mf.file.size();
    void* buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        meshpunk_close(mf);
        SLog.printf("[meshpunk_fs] failed to alloc %u bytes for %s\n", sz, path);
        return NULL;
    }

    size_t rd = mf.file.read((uint8_t*)buf, sz);
    meshpunk_close(mf);

    if (rd != sz) {
        SLog.printf("[meshpunk_fs] short read: %u/%u for %s\n", (uint32_t)rd, sz, path);
        heap_caps_free(buf);
        return NULL;
    }

    *out_size = sz;
    return buf;
}
