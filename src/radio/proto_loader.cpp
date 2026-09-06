#include "proto_loader.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <esp_random.h>
#include <stdarg.h>

#include "radio_hal.h"
#include "radio_capture.h"
#include "proto_pool.h"
#include "../mesh_store.h"
#include "../meshpunk_sync.h"   // SLog
#include "../meshpunk_fs.h"     // meshpunk_read_all
#include "../notify.h"
#include "../usb_manager.h"     // UsbFlashGuardIf
#include "../elf_host.h"        // elf_loraproto_load/elf_loraproto_unload
#include <helpers/ArduinoHelpers.h>   // VolatileRTCClock

extern VolatileRTCClock* host_rtc;
extern void sd_spi_release();     // defined in main.cpp (legacy TFT-recovery wrapper)
extern uint16_t firmware_batt_mv();   // main.cpp — board battery reading
extern bool sd_mounted;           // main.cpp — the card mounted at boot

extern const LoraProtoOps none_proto_ops;           // defined below (the no-radio floor)
static const LoraProtoOps* s_ops = &none_proto_ops; // replaced at select_and_load
static void*               s_mod = nullptr;         // elf module handle (module path)
static char                s_boot_notice[96] = {0};

// ── MeshHostApi adapters ─────────────────────────────────────────────────────

static bool host_radio_config(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr,
                              int8_t tx_dbm_radiated, bool crc,
                              uint8_t sync_word, uint16_t preamble_len) {
    RadioParams p;
    p.freq_mhz        = freq_mhz;
    p.bw_khz          = bw_khz;
    p.sf              = sf;
    p.cr              = cr;
    p.tx_dbm_radiated = tx_dbm_radiated;
    p.crc             = crc;
    p.sync_word       = sync_word;
    p.preamble_len    = preamble_len;
    return radio_hal_config(p);
}

static void host_store_channel_msg(const char* ch_name, int channel_idx,
                                   const char* from, const char* text,
                                   uint32_t timestamp, float snr, float rssi,
                                   uint8_t hops, bool direct) {
    mstore::append_channel_message(ch_name, channel_idx, from, text,
                                   timestamp, snr, rssi, hops, direct);
}

static void host_store_dm_msg(const char* peer, const char* from, const char* text,
                              uint32_t timestamp, float snr, float rssi,
                              uint8_t hops, bool direct) {
    mstore::append_dm_message(peer, from, text, timestamp, snr, rssi, hops, direct);
}

static uint32_t host_clock_now(void) {
    return host_rtc ? host_rtc->getCurrentTime() : 0;
}

static uint32_t host_millis32(void) { return (uint32_t)millis(); }

static void host_random_bytes(uint8_t* out, int len) {
    if (out && len > 0) esp_fill_random(out, (size_t)len);
}

static void host_log(const char* fmt, ...) {
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SLog.printf("[PROTO] %s\n", buf);
}

// The protocol's data home on the chosen mesh storage, VFS form (set at
// select time, before init runs). Empty until a protocol module is selected.
static char s_data_dir_vfs[128] = {0};
static const char* host_data_dir(void) { return s_data_dir_vfs; }

// Mesh-derived time → the firmware clock authority (meshpunk_set_clock:
// higher tier wins, equal is forward-only, stale tiers decay). quality maps
// 1:1 onto the CLOCK_TIER_* table, capped at MANUAL.
static bool host_clock_suggest(uint32_t epoch, uint8_t quality) {
    if (quality > CLOCK_TIER_MANUAL) quality = CLOCK_TIER_MANUAL;
    return meshpunk_set_clock(quality, epoch, "protocol");
}

static const MeshHostApi s_host_api = {
    LORA_PROTO_ABI_VERSION,
    host_radio_config,
    radio_hal_start_receive,
    radio_hal_poll_irq,
    radio_hal_read_packet,
    radio_hal_start_send,
    radio_hal_send_finished,
    radio_hal_last_rssi,
    radio_hal_last_snr,
    radio_hal_standby,
    radio_hal_sleep,
    host_store_channel_msg,
    host_store_dm_msg,
    mstore::unread_bump_channel,
    mstore::unread_bump_dm,
    notify_message_alert,
    notify_post,
    host_clock_now,
    host_millis32,
    host_random_bytes,
    host_log,
    proto_pool_alloc,
    proto_pool_free,
    rcap::frame,
    host_data_dir,
    meshpunk_gps_last_fix,
    // v2 appends (a v1 module never reads past its known prefix)
    host_clock_suggest,
    // v3 appends (same rule; the handed copy is stamped at the module's abi)
    radio_hal_time_on_air_ms,
    radio_hal_current_rssi,
    radio_hal_reset_agc,
    radio_hal_set_rx_boost,
    firmware_batt_mv,
};

// ── Selection + module load ──────────────────────────────────────────────────

static void set_notice(const char* fmt, const char* id) {
    snprintf(s_boot_notice, sizeof(s_boot_notice), fmt, id);
}

// The no-radio protocol: a user selection in Settings > Lora, for running the
// device with the LoRa chip parked (and the boot state once every protocol
// package is uninstalled). Every universal surface runs; protocol apps show
// their protocol notice. start() parks the chip — radio_hal_begin has already
// run at that point. All other surfaces are NULL: the _lora_proto_* bindings
// return clean failures/empties on NULL vtable members.
static bool none_start(void) { radio_hal_sleep(); return true; }
const LoraProtoOps none_proto_ops = {
    LORA_PROTO_ABI_VERSION,
    "none",
    "No radio",
    nullptr,                     // init: nothing to initialize
    none_start,
    nullptr, nullptr,            // loop, stop
    nullptr, nullptr,            // send_channel_text, send_text
    nullptr,                     // get_peers
    nullptr, nullptr,            // get_config, set_config
    nullptr, nullptr, nullptr,   // prepare_sleep, note_wake, flush
    nullptr, nullptr, nullptr,   // lua_open, lua_close, lua_tick (v2)
};

// First <name>.loraproto.elf inside fs:<dir>; false when the dir or the file
// is absent. The caller holds the SPI lock for the SD filesystem.
static bool find_package_elf(FS& fs, const char* dir, char* out, size_t out_sz) {
    out[0] = '\0';
    File root = fs.open(dir);
    if (!root || !root.isDirectory()) return false;
    File e = root.openNextFile();
    while (e) {
        if (!e.isDirectory()) {
            String base = e.name();
            int slash = base.lastIndexOf('/');
            if (slash >= 0) base = base.substring(slash + 1);
            if (base.endsWith(".loraproto.elf")) {
                strncpy(out, base.c_str(), out_sz - 1);
                out[out_sz - 1] = '\0';
                break;
            }
        }
        e = root.openNextFile();
    }
    root.close();
    return out[0] != '\0';
}

void lora_proto_select_and_load(void) {
    const char* req = lora_proto_requested();
    // meshcore is a package like any other; its one privilege is the store
    // ROOT as data home (the zero-migration rule, below).
    bool meshcore_req = (strcmp(req, "meshcore") == 0);
    if (strcmp(req, "none") == 0) {
        s_ops = &none_proto_ops;
        lora_proto_mark_active("none");
        SLog.println("[PROTO] active: none (no radio) — LoRa chip parks at start");
        return;
    }

    // Find <something>.loraproto.elf in <drive>:/meshpunk/lora_protos/<req>/
    // — internal flash first, then the SD card when it mounted. An SD-hosted
    // package with no card at boot is simply "not installed": radio off with
    // the notice, never a substitute.
    char dir[96];
    snprintf(dir, sizeof(dir), "/meshpunk/lora_protos/%s", req);
    char elfname[64] = {0};
    const char* drv = "L";
    const char* pkg_mount = "/littlefs";
    if (!find_package_elf(LittleFS, dir, elfname, sizeof(elfname)) && sd_mounted) {
        sd_spi_take();
        bool on_sd = find_package_elf(SD, dir, elfname, sizeof(elfname));
        sd_spi_release();
        if (on_sd) {
            drv = "S";
            pkg_mount = "/sd";
        }
    }
    if (!elfname[0]) {
        SLog.printf("[PROTO] '%s' requested but no .loraproto.elf in L:%s or S:%s — radio off\n",
                    req, dir, dir);
        set_notice("LoRa protocol '%s' is not installed - radio off", req);
        s_ops = &none_proto_ops;
        lora_proto_mark_active("none");
        return;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s:%s/%s", drv, dir, elfname);
    const void* ops_raw = nullptr;
    void* mod = elf_loraproto_load(path, &ops_raw);
    const LoraProtoOps* ops = (const LoraProtoOps*)ops_raw;

    bool ok = (mod != nullptr) && (ops != nullptr);
    if (!ok)
        set_notice("LoRa protocol '%s': elf load failed - radio off", req);
    // Append-only ABI: any older module loads (appended fields are gated on
    // its declared abi); a module built against a NEWER abi is refused.
    if (ok && (ops->abi < 1 || ops->abi > LORA_PROTO_ABI_VERSION)) {
        SLog.printf("[PROTO] %s: abi %u, firmware supports up to %u\n", path,
                    (unsigned)ops->abi, (unsigned)LORA_PROTO_ABI_VERSION);
        set_notice("LoRa protocol '%s': needs newer firmware - radio off", req);
        ok = false;
    }
    if (ok && (!ops->id || strcmp(ops->id, req) != 0)) {
        SLog.printf("[PROTO] %s: id '%s' does not match install dir '%s'\n",
                    path, ops->id ? ops->id : "(null)", req);
        set_notice("LoRa protocol '%s': package id mismatch, reinstall it - radio off", req);
        ok = false;
    }
    if (ok) {
        const char* mount = (mstore::storage() == &SD) ? "/sd" : "/littlefs";
        if (meshcore_req) {
            // The meshcore PACKAGE owns the store ROOT — every file exactly
            // where it has always lived (the zero-migration promise). No
            // namespace, no per-id subfolder.
            snprintf(s_data_dir_vfs, sizeof(s_data_dir_vfs), "%s%s",
                     mount, mstore::prefix().c_str());
            mstore::set_namespace("");
        } else {
            // Per-protocol folder on the chosen mesh storage: <prefix>/<id>/
            // holds this protocol's peers/config/identity copy AND (via the
            // store namespace) its messages + route dirs — protocols never
            // share conversation files. Set BEFORE init: the module loads
            // from it.
            char rel[96];
            snprintf(rel, sizeof(rel), "%s/%s", mstore::prefix().c_str(), req);
            if (mstore::storage()) mstore::storage()->mkdir(rel);
            snprintf(s_data_dir_vfs, sizeof(s_data_dir_vfs), "%s%s",
                     mount, rel);
            mstore::set_namespace(req);
        }
        SLog.printf("[PROTO] data home: %s\n", s_data_dir_vfs);

        char vfs_dir[128];
        snprintf(vfs_dir, sizeof(vfs_dir), "%s%s", pkg_mount, dir);
        // Hand the host API stamped at the MODULE's declared abi: an older
        // module's strict version check passes, and the stamp is the truth —
        // it must not read past that version's fields anyway.
        static MeshHostApi handed;
        handed = s_host_api;
        handed.abi = ops->abi;
        if (!ops->init || !ops->init(&handed, vfs_dir)) {
            SLog.printf("[PROTO] %s: init failed\n", path);
            set_notice("LoRa protocol '%s': init failed - radio off", req);
            ok = false;
        }
    }

    if (!ok) {
        if (mod) elf_loraproto_unload(mod);
        mstore::set_namespace("");   // back to the root namespace
        s_ops = &none_proto_ops;
        lora_proto_mark_active("none");
        return;
    }

    s_mod = mod;
    s_ops = ops;
    lora_proto_mark_active(req);
    SLog.printf("[PROTO] active: %s (%s) from %s:, pool %uB free\n",
                ops->id, ops->name ? ops->name : "", drv, (unsigned)proto_pool_free_bytes());
}

const LoraProtoOps* lora_proto_ops(void) { return s_ops; }

bool lora_proto_start(void) {
    if (s_ops->start && s_ops->start()) return true;
    // Radio bring-up failed. The protocol stays loaded and selected; the radio
    // is not receiving. Loud and visible (log + bell notice), nothing rerouted.
    SLog.printf("[PROTO] '%s' start failed — radio not running\n", s_ops->id);
    set_notice("LoRa protocol '%s' failed to start - radio not running", s_ops->id);
    return false;
}

void lora_proto_loop(void)  { if (s_ops->loop)  s_ops->loop(); }
void lora_proto_stop(void)  { if (s_ops->stop)  s_ops->stop(); }
void lora_proto_flush(void) { if (s_ops->flush) s_ops->flush(); }

// ── Protocol Lua surface (ABI v2) ────────────────────────────────────────────
// Appended-field rule: a v1 module's ops struct physically ends before
// lua_open — reading those slots is only legal when the module declared
// abi >= 2.
void lora_proto_lua_open(void* L) {
    if (s_ops->abi >= 2 && s_ops->lua_open) s_ops->lua_open(L);
}
void lora_proto_lua_close(void) {
    if (s_ops->abi >= 2 && s_ops->lua_close) s_ops->lua_close();
}
void lora_proto_lua_tick(void* L) {
    if (s_ops->abi >= 2 && s_ops->lua_tick) s_ops->lua_tick(L);
}

const char* lora_proto_boot_notice(void) {
    return s_boot_notice[0] ? s_boot_notice : nullptr;
}

// ── Offline protocol settings ────────────────────────────────────────────────
// Settings apps are never gated on the active protocol: an INACTIVE
// protocol's settings edit its files here, and the protocol re-validates
// every value at its next boot (each protocol's cfg loader range-checks on
// read — the ABI file convention). Files, on the chosen mesh storage under
// <prefix>/<id>/:
//   cfg     key=value lines               (keys are the protocol's own schema)
//   notify  <channel name> \t <mode>      ("notify_ch:<name>" keys; no entry
//                                          = mode 1, the shared default)
// The ACTIVE protocol must never be edited this way — its RAM state clobbers
// the file at its next save; the _lora_proto_config_* bindings route by id.

static void offline_path(const char* id, bool notify_file, char* out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s/%s", mstore::prefix().c_str(), id,
             notify_file ? "notify" : "cfg");
}

// One line from an Arduino File (no fgets on FS::File). False at EOF with
// nothing read.
static bool offline_read_line(File& f, char* line, size_t line_sz) {
    size_t len = 0;
    bool any = false;
    while (f.available()) {
        char c = (char)f.read();
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len < line_sz - 1) line[len++] = c;
    }
    line[len] = '\0';
    return any;
}

int lora_proto_offline_config_get(const char* id, const char* key,
                                  char* out, int out_sz) {
    if (!id || !id[0] || !key || !key[0] || !out || out_sz <= 0) return 0;
    FS* fs = mstore::storage();
    if (!fs) return 0;
    bool notify_file = (strncmp(key, "notify_ch:", 10) == 0);
    const char* want = notify_file ? key + 10 : key;
    if (!want[0]) return 0;

    char path[128];
    offline_path(id, notify_file, path, sizeof(path));
    bool is_sd = (fs == &SD);
    if (is_sd) sd_spi_take();

    int n = 0;
    File f = fs->open(path, "r");
    if (f) {
        char line[128];
        size_t want_len = strlen(want);
        while (offline_read_line(f, line, sizeof(line))) {
            char sep = notify_file ? '\t' : '=';
            char* p = strchr(line, sep);
            if (!p) continue;
            if ((size_t)(p - line) != want_len) continue;
            if (strncmp(line, want, want_len) != 0) continue;
            n = snprintf(out, out_sz, "%s", p + 1);
            if (n >= out_sz) n = out_sz - 1;
            break;
        }
        f.close();
    }
    if (is_sd) sd_spi_release();

    // No notify entry = the protocols' shared default mode.
    if (n == 0 && notify_file) n = snprintf(out, out_sz, "1");
    return n;
}

bool lora_proto_offline_config_set(const char* id, const char* key, const char* val) {
    if (!id || !id[0] || !key || !key[0] || !val) return false;
    FS* fs = mstore::storage();
    if (!fs) return false;
    // Line-format integrity; schema validation is the protocol's, at its load.
    if (strchr(key, '\n') || strchr(val, '\n') || strchr(val, '\r')) return false;
    bool notify_file = (strncmp(key, "notify_ch:", 10) == 0);
    const char* want = notify_file ? key + 10 : key;
    if (!want[0]) return false;
    if (notify_file) {
        // The notify FILE format only holds 0/1/2 by definition.
        if (val[0] < '0' || val[0] > '2' || val[1]) return false;
        if (strchr(want, '\t')) return false;
    } else {
        if (strchr(want, '=') || strchr(want, '\t')) return false;
    }

    char path[128];
    char tmp[136];
    offline_path(id, notify_file, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    bool is_sd = (fs == &SD);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    // Make sure the protocol's folder exists — settings can be written before
    // the protocol's first boot (mkdir on an existing dir is a harmless false).
    {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/%s", mstore::prefix().c_str(), id);
        fs->mkdir(dir);
    }

    bool ok = false;
    File w = fs->open(tmp, "w", true);
    if (w) {
        // Keep every line except the key being replaced (unknown keys belong
        // to the protocol's schema and must survive round-trips).
        File r = fs->open(path, "r");
        if (r) {
            char line[128];
            size_t want_len = strlen(want);
            char sep = notify_file ? '\t' : '=';
            while (offline_read_line(r, line, sizeof(line))) {
                if (!line[0]) continue;
                char* p = strchr(line, sep);
                if (p && (size_t)(p - line) == want_len &&
                    strncmp(line, want, want_len) == 0) continue;
                w.printf("%s\n", line);
            }
            r.close();
        }
        // notify mode 1 = the default = no entry (the protocols' own rule).
        if (notify_file) {
            if (val[0] != '1') w.printf("%s\t%s\n", want, val);
        } else {
            w.printf("%s=%s\n", want, val);
        }
        w.close();
        fs->remove(path);
        ok = fs->rename(tmp, path);
        if (!ok) fs->remove(tmp);
    }
    if (is_sd) sd_spi_release();
    if (ok) SLog.printf("[PROTO] offline config %s: %s=%s\n", id, key, val);
    return ok;
}
