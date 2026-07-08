// Unified drive-aware filesystem bridge: Lua -> C++.
//
// One binding family for BOTH storages, routed by the same L:/S: path prefix
// io.open() already uses (no prefix = LittleFS):
//
//   _fs_list(path[, sz])  -> { {name=, type="file"|"dir", size=}, ... } | nil, err
//                            (sz defaults true; false skips per-entry sizes —
//                             much faster on big directories, size comes back 0)
//   _fs_stat(path)        -> { type="file"|"dir", size= } | nil
//   _fs_exists(path)      -> bool
//   _fs_mkdir(path)       -> bool                 (creates missing parents too)
//   _fs_remove(path)      -> ok, err              (file, or EMPTY directory)
//   _fs_rename(src, dst)  -> ok, err              (same drive only)
//   _fs_copy(src, dst)    -> ok, err              (one file, any drive combo)
//   _fs_df(drive)         -> total, used | nil    (drive = "L" | "S")
//
// Sizes are pushed as Lua numbers (doubles), not integers, so multi-GB SD
// values can't overflow a 32-bit lua_Integer build.
//
// Recursive operations (copy/delete a tree) are deliberately Lua-side —
// lib/fileman.lua walks trees incrementally from an LVGL timer so the UI stays
// alive and the task watchdog is never starved; every call here is bounded
// work. SD access follows the meshpunk_fs.cpp convention: sd_spi_take/release
// around bus use, released periodically inside long loops so the Core-1 mesh
// task can use the radio between our transactions.

#include "fs_bridge.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"
#include "usb_manager.h"   // UsbFlashGuardIf — pause USB audio around flash writes

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <dirent.h>
#include <sys/stat.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

extern bool sd_mounted;
extern void sd_spi_release();

// A resolved path: which filesystem it lives on and the prefix-stripped path.
// `ok` is false when the path routes to SD but no card is mounted.
struct FsTarget {
    fs::FS*     fs;
    const char* path;
    bool        is_sd;
    bool        ok;
};

static FsTarget fs_resolve(const char* raw) {
    FsTarget t = { &LittleFS, raw, false, true };
    bool use_sd = false;
    t.path = meshpunk_parse_prefix(raw, &use_sd, /*default_sd=*/false);
    if (t.path[0] == '\0') t.path = "/";
    t.is_sd = use_sd;
    if (use_sd) {
        t.fs = &SD;
        t.ok = sd_mounted;
    }
    return t;
}

// _fs_list(path [, want_sizes]) -> array | nil, err
// want_sizes defaults to true; pass false to skip per-entry sizes (size = 0).
//
// Enumeration is POSIX opendir/readdir on the VFS path, NOT Arduino
// openNextFile(): the latter re-opens every entry by full path, and both FAT
// and LittleFS do a linear directory lookup per open — a whole listing was
// quadratic in the entry count and starved the task watchdog on ROM folders
// with hundreds of files. readdir walks the directory once. Sizes still cost
// a stat() (a path lookup) per file, which is why callers that only need
// names — the ELF launchers, fileman's tree scans — pass want_sizes=false.
//
// Two-phase: the directory walk collects entries into a growable C array with
// the SPI lock held, then the Lua table is built with the lock released and the
// dir handle closed. lua_push* can longjmp on a true OOM — escaping with the
// SPI lock held would stall the mesh task forever; leaking the transient names
// on that path is the accepted trade. Names are strdup'd exactly (no length
// cap): the Files app operates on them, so truncation would corrupt ops.
static int lua_fs_list(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    bool want_sizes = lua_isnoneornil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    FsTarget t = fs_resolve(raw);
    if (!t.ok) {
        lua_pushnil(L);
        lua_pushstring(L, "SD not mounted");
        return 2;
    }

    // VFS-level base path for opendir/stat ("/sd/..." or "/littlefs/...").
    char base[384];
    snprintf(base, sizeof(base), "%s%s", t.is_sd ? "/sd" : "/littlefs", t.path);
    size_t base_len = strlen(base);

    if (t.is_sd) sd_spi_take();
    DIR* dir = opendir(base);
    if (!dir) {
        if (t.is_sd) sd_spi_release();
        lua_pushnil(L);
        lua_pushstring(L, "not a directory");
        return 2;
    }

    struct Ent {
        char*  name;
        bool   is_dir;
        double size;
    };
    int cap = 32, n = 0;
    Ent* ents = (Ent*)heap_caps_malloc(sizeof(Ent) * cap,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    int iter = 0;
    struct dirent* de;
    while (ents && (de = readdir(dir)) != NULL) {
        const char* name = de->d_name;
        if (name[0] == '\0') continue;
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) continue;   // "." / ".."

        bool   is_dir = (de->d_type == DT_DIR);
        double size   = 0;
        // stat() only when the caller needs sizes (or d_type is missing) —
        // it is exactly the per-entry path lookup readdir lets us avoid.
        if ((want_sizes && !is_dir) || de->d_type == DT_UNKNOWN) {
            char full[576];
            snprintf(full, sizeof(full), "%s%s%s", base,
                     (base_len && base[base_len - 1] == '/') ? "" : "/", name);
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                if (!is_dir) size = (double)st.st_size;
            }
        }

        if (n >= cap) {
            Ent* grown = (Ent*)heap_caps_realloc(
                ents, sizeof(Ent) * cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) break;   // partial listing beats a deadlock
            ents = grown;
            cap *= 2;
        }
        char* dup = strdup(name);
        if (!dup) break;
        ents[n].name   = dup;
        ents[n].is_dir = is_dir;
        ents[n].size   = size;
        n++;

        // Yield periodically so IDLE0 feeds the task watchdog even on huge
        // directories; on SD also give the bus back so the Core-1 mesh task
        // can use the radio between our transactions.
        if (++iter % 32 == 0) {
            if (t.is_sd) sd_spi_release();
            vTaskDelay(1);
            if (t.is_sd) sd_spi_take();
        }
    }
    closedir(dir);
    if (t.is_sd) sd_spi_release();

    if (!ents) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        lua_pushstring(L, ents[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, ents[i].is_dir ? "dir" : "file");
        lua_setfield(L, -2, "type");
        lua_pushnumber(L, (lua_Number)ents[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, i + 1);
    }
    for (int i = 0; i < n; i++) free(ents[i].name);
    heap_caps_free(ents);
    return 1;
}

// _fs_stat(path) -> { type=, size= } | nil
static int lua_fs_stat(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    FsTarget t = fs_resolve(raw);
    if (!t.ok) {
        lua_pushnil(L);
        return 1;
    }

    bool is_root = (strcmp(t.path, "/") == 0);
    bool is_dir = false;
    lua_Number size = 0;

    if (is_root) {
        is_dir = true;   // a mounted drive's root always exists
    } else {
        if (t.is_sd) sd_spi_take();
        if (!t.fs->exists(t.path)) {
            if (t.is_sd) sd_spi_release();
            lua_pushnil(L);
            return 1;
        }
        File f = t.fs->open(t.path);
        if (!f) {
            if (t.is_sd) sd_spi_release();
            lua_pushnil(L);
            return 1;
        }
        is_dir = f.isDirectory();
        if (!is_dir) size = (lua_Number)f.size();
        f.close();
        if (t.is_sd) sd_spi_release();
    }

    lua_newtable(L);
    lua_pushstring(L, is_dir ? "dir" : "file");
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, size);
    lua_setfield(L, -2, "size");
    return 1;
}

// _fs_exists(path) -> bool
static int lua_fs_exists(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    FsTarget t = fs_resolve(raw);
    if (!t.ok) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (strcmp(t.path, "/") == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (t.is_sd) sd_spi_take();
    bool ex = t.fs->exists(t.path);
    if (t.is_sd) sd_spi_release();
    lua_pushboolean(L, ex ? 1 : 0);
    return 1;
}

// _fs_mkdir(path) -> bool. Creates missing parents. Returns true if the
// directory exists on return (already-a-dir is success; a same-named FILE is
// failure).
static int lua_fs_mkdir(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    FsTarget t = fs_resolve(raw);
    if (!t.ok || strcmp(t.path, "/") == 0) {
        lua_pushboolean(L, t.ok ? 1 : 0);   // mkdir "/" is a no-op success
        return 1;
    }

    // LittleFS directory creation is an internal-flash (metadata) write —
    // pause USB audio around the whole thing (one pause spans the nested
    // meshpunk_mkdirs guard too; the guard is recursive).
    UsbFlashGuardIf _g(!t.is_sd);

    // meshpunk_mkdirs treats the last segment as a file name, so it creates
    // exactly the parents; then we create the directory itself.
    meshpunk_mkdirs(raw, /*default_sd=*/false);

    if (t.is_sd) sd_spi_take();
    bool ok;
    if (t.fs->exists(t.path)) {
        File f = t.fs->open(t.path);
        ok = f && f.isDirectory();
        if (f) f.close();
    } else {
        ok = t.fs->mkdir(t.path);
    }
    if (t.is_sd) sd_spi_release();
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// _fs_remove(path) -> ok, err. Removes a file, or an EMPTY directory
// (recursion is the Lua library's job).
static int lua_fs_remove(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    FsTarget t = fs_resolve(raw);
    if (!t.ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "SD not mounted");
        return 2;
    }
    if (strcmp(t.path, "/") == 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "cannot remove drive root");
        return 2;
    }

    UsbFlashGuardIf _g(!t.is_sd);   // LittleFS remove/rmdir writes flash metadata
    if (t.is_sd) sd_spi_take();
    if (!t.fs->exists(t.path)) {
        if (t.is_sd) sd_spi_release();
        lua_pushboolean(L, 0);
        lua_pushstring(L, "not found");
        return 2;
    }
    File f = t.fs->open(t.path);
    bool is_dir = f && f.isDirectory();
    if (f) f.close();
    bool ok = is_dir ? t.fs->rmdir(t.path) : t.fs->remove(t.path);
    if (t.is_sd) sd_spi_release();

    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, is_dir ? "rmdir failed (not empty?)" : "remove failed");
    return 2;
}

// _fs_rename(src, dst) -> ok, err. Same drive only — a cross-drive "rename"
// must be a copy + delete, which the Lua library handles.
static int lua_fs_rename(lua_State* L) {
    const char* raw_src = luaL_checkstring(L, 1);
    const char* raw_dst = luaL_checkstring(L, 2);
    FsTarget s = fs_resolve(raw_src);
    FsTarget d = fs_resolve(raw_dst);
    if (!s.ok || !d.ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "SD not mounted");
        return 2;
    }
    if (s.is_sd != d.is_sd) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "cross-drive rename (copy + delete instead)");
        return 2;
    }

    UsbFlashGuardIf _g(!s.is_sd);   // LittleFS rename writes flash metadata
    if (s.is_sd) sd_spi_take();
    bool ok = s.fs->rename(s.path, d.path);
    if (s.is_sd) sd_spi_release();

    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, "rename failed");
    return 2;
}

// _fs_copy(src, dst) -> ok, err. One FILE, any drive combination, chunked and
// binary-safe. Overwrites dst. The SPI lock is held only per chunk, with a
// periodic yield, so the mesh task keeps running during multi-MB copies.
static int lua_fs_copy(lua_State* L) {
    const char* raw_src = luaL_checkstring(L, 1);
    const char* raw_dst = luaL_checkstring(L, 2);
    FsTarget s = fs_resolve(raw_src);
    FsTarget d = fs_resolve(raw_dst);
    if (!s.ok || !d.ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "SD not mounted");
        return 2;
    }
    if (s.fs == d.fs && strcmp(s.path, d.path) == 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "source = destination");
        return 2;
    }
    bool any_sd = s.is_sd || d.is_sd;

    // LittleFS destination: every chunk write below is an internal-flash
    // write. Hold the guard across the whole copy — pausing USB audio for a
    // user-initiated file copy beats crashing the host stack mid-transfer.
    UsbFlashGuardIf _g(!d.is_sd);

    if (any_sd) sd_spi_take();
    File fsrc = s.fs->open(s.path, "r");
    if (!fsrc || fsrc.isDirectory()) {
        if (fsrc) fsrc.close();
        if (any_sd) sd_spi_release();
        lua_pushboolean(L, 0);
        lua_pushstring(L, "cannot open source file");
        return 2;
    }
    // create=true also creates missing parent directories.
    File fdst = d.fs->open(d.path, "w", true);
    if (!fdst) {
        fsrc.close();
        if (any_sd) sd_spi_release();
        lua_pushboolean(L, 0);
        lua_pushstring(L, "cannot open destination");
        return 2;
    }
    if (any_sd) sd_spi_release();

    const uint32_t PSRAM_CHUNK = 32 * 1024;
    uint32_t chunk = PSRAM_CHUNK;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(PSRAM_CHUNK,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        chunk = 4096;
        buf = (uint8_t*)malloc(chunk);
    }

    const char* err = buf ? NULL : "out of memory";
    int iter = 0;
    while (!err) {
        if (any_sd) sd_spi_take();
        size_t n = fsrc.read(buf, chunk);
        size_t w = (n > 0) ? fdst.write(buf, n) : 0;
        if (any_sd) sd_spi_release();
        if (n == 0) break;   // EOF
        if (w != n) {
            err = "write failed (disk full?)";
            break;
        }
        // Yield every 64KB so the idle task feeds the watchdog and Core-1
        // gets bus time even on huge files.
        if (++iter % 2 == 0) vTaskDelay(1);
    }

    if (any_sd) sd_spi_take();
    fsrc.close();
    fdst.close();
    if (err && buf) d.fs->remove(d.path);   // don't leave a truncated file
    if (any_sd) sd_spi_release();
    if (buf) free(buf);

    if (err) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// _fs_df(drive) -> total, used | nil. drive = "L" or "S".
// NOTE: the first SD call can take a moment on big cards (FAT free-cluster
// scan); it is cached by the FS driver afterwards.
static int lua_fs_df(lua_State* L) {
    const char* drv = luaL_checkstring(L, 1);
    if (drv[0] == 'S' || drv[0] == 's') {
        if (!sd_mounted) {
            lua_pushnil(L);
            return 1;
        }
        sd_spi_take();
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        sd_spi_release();
        lua_pushnumber(L, (lua_Number)total);
        lua_pushnumber(L, (lua_Number)used);
        return 2;
    }
    lua_pushnumber(L, (lua_Number)LittleFS.totalBytes());
    lua_pushnumber(L, (lua_Number)LittleFS.usedBytes());
    return 2;
}

void fs_bridge_register(lua_State* L) {
    lua_register(L, "_fs_list",   lua_fs_list);
    lua_register(L, "_fs_stat",   lua_fs_stat);
    lua_register(L, "_fs_exists", lua_fs_exists);
    lua_register(L, "_fs_mkdir",  lua_fs_mkdir);
    lua_register(L, "_fs_remove", lua_fs_remove);
    lua_register(L, "_fs_rename", lua_fs_rename);
    lua_register(L, "_fs_copy",   lua_fs_copy);
    lua_register(L, "_fs_df",     lua_fs_df);
}
