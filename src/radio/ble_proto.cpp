#include "ble_proto.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <FS.h>
#include <LittleFS.h>

#include "radio_hal.h"          // lora_proto_active (requires_lora checks)
#include "proto_pool.h"
#include "../elf_host.h"        // elf_bleproto_load/unload
#include "../meshpunk_sync.h"   // SLog, MESH_LOCK
#include "../notify.h"
#include <helpers/ArduinoHelpers.h>   // VolatileRTCClock

extern VolatileRTCClock* host_rtc;

// ── Installed BLE-protocol modules (.bleproto.elf) ──────────────────────────
// ONE mechanism for every BLE protocol: its own elf in
// L:/meshpunk/ble_protos/<id>/, loaded into the protocol pool. A BLE
// protocol coupled to a LoRa protocol (the meshcore companion importing the
// LoRa elf) resolves those imports against the LOADED LoRa elf — under any
// other LoRa protocol the imports miss and the load is refused, which IS
// the dependency enforced at link level.
static void* s_ble_mod = nullptr;

static void unload_module(void) {
    if (s_ble_mod) {
        elf_bleproto_unload(s_ble_mod);
        s_ble_mod = nullptr;
    }
}

static const BleProtoOps* try_load_module(const char* id) {
    char dir[96];
    snprintf(dir, sizeof(dir), "/meshpunk/ble_protos/%s", id);
    char elfname[64] = {0};
    File root = LittleFS.open(dir);
    if (root && root.isDirectory()) {
        File e = root.openNextFile();
        while (e) {
            if (!e.isDirectory()) {
                String base = e.name();
                int slash = base.lastIndexOf('/');
                if (slash >= 0) base = base.substring(slash + 1);
                if (base.endsWith(".bleproto.elf")) {
                    strncpy(elfname, base.c_str(), sizeof(elfname) - 1);
                    break;
                }
            }
            e = root.openNextFile();
        }
        root.close();
    }
    if (!elfname[0]) return nullptr;   // not installed as a module

    char path[160];
    snprintf(path, sizeof(path), "L:%s/%s", dir, elfname);
    const void* ops_raw = nullptr;
    void* mod = elf_bleproto_load(path, &ops_raw);
    if (!mod) return nullptr;          // load refused (logged; a coupled
                                       // protocol under the wrong LoRa
                                       // protocol lands here via
                                       // unresolved imports)
    const BleProtoOps* ops = (const BleProtoOps*)ops_raw;
    if (ops->abi < 1 || ops->abi > BLE_PROTO_ABI_VERSION ||
        !ops->id || strcmp(ops->id, id) != 0) {
        SLog.printf("[BLEPROTO] %s: abi %u / id '%s' mismatch\n", path,
                    (unsigned)ops->abi, ops->id ? ops->id : "(null)");
        elf_bleproto_unload(mod);
        return nullptr;
    }
    s_ble_mod = mod;
    return ops;
}

static const BleProtoOps* acquire_proto(const char* id) {
    return try_load_module(id);
}

// ── Selection state ──────────────────────────────────────────────────────────
// Default matches the pre-slot behavior: the companion on (when compiled in).
#if BLE_COMPANION_ENABLED
#define BLE_PROTO_DEFAULT "meshcore_companion"
#else
#define BLE_PROTO_DEFAULT "none"
#endif

static char s_requested[24] = BLE_PROTO_DEFAULT;
static char s_active[24]    = "none";
static const BleProtoOps* s_ops = nullptr;
static bool s_running = false;
static char s_notice[96] = {0};

void ble_proto_set_requested(const char* id) {
    // Same character rules as LoRa protocol ids; anything invalid = default.
    char buf[24];
    size_t n = 0;
    if (id) {
        for (const char* c = id; *c && n < sizeof(buf) - 1; c++) {
            char ch = (char)tolower((unsigned char)*c);
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
                buf[n++] = ch;
            } else { n = 0; break; }
        }
    }
    buf[n] = '\0';
    strncpy(s_requested, n ? buf : BLE_PROTO_DEFAULT, sizeof(s_requested) - 1);
    s_requested[sizeof(s_requested) - 1] = '\0';
}

const char* ble_proto_requested(void) { return s_requested; }
const char* ble_proto_active(void)    { return s_active; }
bool        ble_proto_running(void)   { return s_running; }
const char* ble_proto_boot_notice(void) { return s_notice[0] ? s_notice : nullptr; }

// ── Host API (module BLE protocols; built-ins ignore it) ─────────────────────
static uint32_t bh_clock_now(void) { return host_rtc ? host_rtc->getCurrentTime() : 0; }
static uint32_t bh_millis32(void)  { return (uint32_t)millis(); }
static void bh_log(const char* fmt, ...) {
    char buf[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SLog.printf("[BLEPROTO] %s\n", buf);
}

// User-initiated restart carried by the protocol (phone app command).
static void bh_device_reboot(void) {
    SLog.println("[BLEPROTO] device reboot (user request via BLE protocol)");
    delay(500);
    ESP.restart();
}

static const BleHostApi s_host_api = {
    BLE_PROTO_ABI_VERSION,
    // Serial transport (ble_transport.cpp — the NimBLE service).
    ble_transport_open,
    ble_transport_close,
    ble_transport_enable,
    ble_transport_disable,
    ble_transport_connected,
    ble_transport_write_busy,
    ble_transport_write,
    ble_transport_read,
    notify_post,
    bh_clock_now,
    bh_millis32,
    proto_pool_alloc,
    proto_pool_free,
    bh_log,
    bh_device_reboot,
};

// ── Boot ─────────────────────────────────────────────────────────────────────

void ble_proto_select_and_init(void) {
    strncpy(s_active, "none", sizeof(s_active));
    if (strcmp(s_requested, "none") == 0) return;

    const BleProtoOps* ops = acquire_proto(s_requested);
    if (!ops) {
        SLog.printf("[BLEPROTO] '%s' requested but not available — BLE off\n", s_requested);
        snprintf(s_notice, sizeof(s_notice),
                 "BLE protocol '%s' is not available - BLE off", s_requested);
        return;
    }
    if (ops->requires_lora && strcmp(ops->requires_lora, lora_proto_active()) != 0) {
        SLog.printf("[BLEPROTO] '%s' needs LoRa protocol '%s' (active: '%s') — BLE off\n",
                    ops->id, ops->requires_lora, lora_proto_active());
        snprintf(s_notice, sizeof(s_notice),
                 "BLE '%s' needs the %s protocol - BLE off this boot",
                 ops->id, ops->requires_lora);
        unload_module();
        return;
    }
    if (!ops->init || !ops->init(&s_host_api)) {
        SLog.printf("[BLEPROTO] '%s' init failed — BLE off\n", ops->id);
        snprintf(s_notice, sizeof(s_notice),
                 "BLE protocol '%s' init failed - BLE off", ops->id);
        unload_module();
        return;
    }
    s_ops = ops;
    strncpy(s_active, ops->id, sizeof(s_active) - 1);
    s_active[sizeof(s_active) - 1] = '\0';
    SLog.printf("[BLEPROTO] active: %s (%s)\n", ops->id, ops->name ? ops->name : "");
}

void ble_proto_start(void) {
    if (!s_ops) return;
    MESH_LOCK();
    bool ok = s_ops->start && s_ops->start();
    MESH_UNLOCK();
    if (ok) {
        s_running = true;
    } else {
        SLog.printf("[BLEPROTO] '%s' start failed\n", s_ops->id);
        char nbuf[96];
        snprintf(nbuf, sizeof(nbuf), "BLE protocol '%s' failed to start", s_ops->id);
        notify_post(nbuf);
    }
}

void ble_proto_loop(void)  { if (s_running && s_ops && s_ops->loop)  s_ops->loop(); }
void ble_proto_flush(void) { if (s_ops && s_ops->flush) s_ops->flush(); }

void ble_proto_stop(void) {
    if (!s_ops) return;
    MESH_LOCK();
    if (s_ops->stop) s_ops->stop();
    MESH_UNLOCK();
    s_running = false;
}

void ble_proto_resume(void) {
    if (!s_ops || s_running) return;
    MESH_LOCK();
    bool ok = s_ops->init && s_ops->init(&s_host_api) &&
              s_ops->start && s_ops->start();
    MESH_UNLOCK();
    if (ok) s_running = true;
    else    SLog.printf("[BLEPROTO] '%s' resume failed\n", s_ops->id);
}

// ── Live selection ───────────────────────────────────────────────────────────

bool ble_proto_apply(const char* id, const char** reason) {
    static const char* kUnknown = "unknown BLE protocol";
    static const char* kNeeds   = "needs a different LoRa protocol running";
    static const char* kInit    = "init failed";
    static const char* kStart   = "start failed";
    if (reason) *reason = nullptr;

    // Sanitize via the setter, but restore on a validation refusal — a
    // refused selection changes NOTHING.
    char saved[24];
    strncpy(saved, s_requested, sizeof(saved));
    ble_proto_set_requested(id);
    char want[24];
    strncpy(want, s_requested, sizeof(want));

    // Idempotent tap: re-selecting what is already in its proper state does
    // nothing — no BLE bounce, a connected phone stays connected. A matching
    // selection that is NOT running (boot-refused, LoRa protocol has changed
    // since) falls through and gets its transition.
    if (strcmp(want, saved) == 0) {
        bool want_none = (strcmp(want, "none") == 0);
        if ((want_none && !s_ops) || (!want_none && s_running)) return true;
    }

    // Acquire the target first (may load its elf); the old protocol keeps
    // running until the new one is validated. Same-module-id was handled by
    // the idempotence check above, so a fresh load here is a DIFFERENT
    // module than s_ble_mod.
    const BleProtoOps* ops = nullptr;
    void* old_mod = s_ble_mod;
    s_ble_mod = nullptr;
    if (strcmp(want, "none") != 0) {
        ops = acquire_proto(want);
        if (!ops) {
            s_ble_mod = old_mod;
            strncpy(s_requested, saved, sizeof(s_requested));
            if (reason) *reason = kUnknown;
            return false;
        }
        if (ops->requires_lora && strcmp(ops->requires_lora, lora_proto_active()) != 0) {
            unload_module();          // the freshly loaded target, if any
            s_ble_mod = old_mod;
            strncpy(s_requested, saved, sizeof(s_requested));
            if (reason) *reason = kNeeds;
            return false;
        }
    }
    void* new_mod = s_ble_mod;

    MESH_LOCK();
    if (s_ops && s_ops->stop) s_ops->stop();
    s_running = false;
    s_ops = nullptr;
    strncpy(s_active, "none", sizeof(s_active));
    if (old_mod) elf_bleproto_unload(old_mod);

    bool ok = true;
    if (ops) {
        if (ops->init && ops->init(&s_host_api)) {
            s_ops = ops;
            strncpy(s_active, ops->id, sizeof(s_active) - 1);
            s_active[sizeof(s_active) - 1] = '\0';
            if (ops->start && ops->start()) {
                s_running = true;
            } else {
                ok = false;
                if (reason) *reason = kStart;
            }
        } else {
            ok = false;
            if (reason) *reason = kInit;
            s_ble_mod = new_mod;      // keep handle consistent for unload
            unload_module();
            s_ops = nullptr;
        }
    }
    MESH_UNLOCK();
    return ok;
}
