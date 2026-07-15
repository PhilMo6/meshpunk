// USB-OTG host CORE — task, pump, enumeration, registry, pipes, sockets.
//
// The class drivers (audio/kbd/msc, src/usb/usb_drv_*.cpp) attach through the
// UsbDriverDesc registry; everything device-class-specific lives in them.
// This file owns the invariants (see memory: usb-host-experiments):
//   - install the host stack from a task pinned to CORE 1 (esp_intr_alloc is
//     calling-core-local and core 0's slots are full), intr_flags = 0;
//   - the internal PHY select is an RTC-domain register that SURVIVES warm
//     resets — restore it to Serial-JTAG on teardown and on boot, or USB
//     serial stays dead (Windows Code 43) until a full power cycle;
//   - EP0 control-transfer completions route through the LIBRARY event
//     handler first, so every wait loop must pump BOTH lib and client
//     handlers or control requests silently time out;
//   - flash-write guard: internal-flash writes stall both cores' caches; the
//     flash-resident host stack must have no deadline-bearing transfers in
//     flight across the stall (per-driver busy/park/resume hooks; pure-bulk
//     drivers are exempt — completions just latch).
//
// Task/locking model: all driver lifecycle hooks and transfer completions
// run in usb_task (core 1). api->control and api->pipe_xfer are dual-context
// (foreign tasks block on a waiter semaphore that usb_task's pump gives;
// usb_task-context callers self-pump instead).

#include "usb_manager.h"
#include "usb_driver_abi.h"
#include "usb_core_int.h"

#include <Arduino.h>
#include <usb/usb_host.h>
#include <esp_heap_caps.h>
#include <soc/rtc_cntl_struct.h>
#include <soc/usb_serial_jtag_struct.h>
#include "elf_host.h"               // elf_input_inject + elf_usb_driver_load/unload
#include "usb_fs.h"                 // usb_fs_mounted — Lua bridge status field
#include "meshpunk_sync.h"          // sd_spi_take — SD-base driver-dir scans
#include <dirent.h>                 // POSIX dir walk over the driver bases
#include <sys/stat.h>               // .disabled marker probe

extern void sd_spi_release();       // main.cpp (take is inline in meshpunk_sync.h)

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// ── Log line ring (usb task -> Lua poll; serial is dead in host mode) ───────

#define ULOG_LINES    48
#define ULOG_LINE_LEN 96

static char     s_log[ULOG_LINES][ULOG_LINE_LEN];
static uint8_t  s_log_head = 0;
static uint8_t  s_log_tail = 0;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void ulog_line(const char* line) {
    portENTER_CRITICAL(&s_log_mux);
    strncpy(s_log[s_log_head], line, ULOG_LINE_LEN - 1);
    s_log[s_log_head][ULOG_LINE_LEN - 1] = 0;
    s_log_head = (s_log_head + 1) % ULOG_LINES;
    if (s_log_head == s_log_tail) s_log_tail = (s_log_tail + 1) % ULOG_LINES;
    portEXIT_CRITICAL(&s_log_mux);
}

void usbcore_log(const char* fmt, ...) {
    char line[ULOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ulog_line(line);
}
#define ulog usbcore_log

// Public wrapper (usb_fs.cpp and other siblings).
void usb_ulog(const char* fmt, ...) {
    char line[ULOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ulog_line(line);
}

static bool ulog_pop(char* out) {
    bool got = false;
    portENTER_CRITICAL(&s_log_mux);
    if (s_log_tail != s_log_head) {
        memcpy(out, s_log[s_log_tail], ULOG_LINE_LEN);
        s_log_tail = (s_log_tail + 1) % ULOG_LINES;
        got = true;
    }
    portEXIT_CRITICAL(&s_log_mux);
    return got;
}

// ── Host + device state ──────────────────────────────────────────────────────

static volatile bool            s_running    = false;
static volatile bool            s_stop_req   = false;
static usb_host_client_handle_t s_client     = NULL;
static usb_device_handle_t      s_dev        = NULL;
static volatile uint8_t         s_new_addr   = 0;
static volatile bool            s_dev_gone   = false;
static TaskHandle_t             s_usb_task_handle = nullptr;

static void (*s_prefs_save)()   = nullptr;

// Cached descriptors of the open device (valid open..close; drivers read
// them through the api during probe/start).
static const usb_device_desc_t* s_dd  = NULL;
static const usb_config_desc_t* s_cfg = NULL;

enum UsbKind { UKIND_NONE, UKIND_AUDIO, UKIND_HID, UKIND_MSC, UKIND_CDC, UKIND_HUB, UKIND_OTHER };

struct UsbDeviceInfo {
    bool     connected;
    uint16_t vid, pid;
    char     product[32];
    UsbKind  kind;
    uint32_t rate;      // AUDIO: chosen sample rate (via usbcore_note_audio)
    uint8_t  bits;      // AUDIO: bit depth
};
static UsbDeviceInfo s_info;   // zero-initialized

static const char* kind_name(UsbKind k) {
    switch (k) {
        case UKIND_AUDIO: return "audio";
        case UKIND_HID:   return "HID";
        case UKIND_MSC:   return "storage";
        case UKIND_CDC:   return "serial";
        case UKIND_HUB:   return "hub";
        case UKIND_OTHER: return "other";
        default:          return "none";
    }
}

usb_host_client_handle_t usbcore_client(void) { return s_client; }
usb_device_handle_t      usbcore_dev(void)    { return s_dev; }

void usbcore_note_audio(uint32_t rate, uint8_t bits) {
    s_info.kind = UKIND_AUDIO;
    s_info.rate = rate;
    s_info.bits = bits;
}

// ── Flash-write guard state (impl at the bottom; drivers read the flag) ─────

static SemaphoreHandle_t s_flash_mux      = nullptr;
static volatile bool     s_flash_pause_req = false;
static volatile bool     s_flash_paused    = false;

bool usbcore_flash_pause_req(void) { return s_flash_pause_req; }

// ── Driver registry ──────────────────────────────────────────────────────────
// Fixed slots; built-ins register from usb_manager_init in lifecycle order
// (audio, kbd, msc — the monolith's want-block order). Iteration is always
// forward, matching the old explicit call order for start/stop/DEV_GONE/
// teardown alike. Mutations happen pre-task (built-ins) or in usb_task
// (dynamic drivers, M2); Lua-facing reads snapshot booleans only.

#define USB_MAX_DRIVERS 8

struct DrvSlot {
    const UsbDriverDesc* d;
    bool  probed;
    bool  running;
    bool  builtin;
    void* mod;              // elf module handle (dynamic drivers only)
    // Per-driver config blob (the install dir's `conf` file, re-read each
    // attach; heap-caps alloc, freed on slot removal) + the publish channel.
    uint8_t* conf;
    uint32_t conf_len;
    uint8_t  pub[64];
    uint8_t  pub_len;
    uint32_t pub_seq;
};

#define DRV_CONF_MAX 1024
static DrvSlot s_drv[USB_MAX_DRIVERS];
static int     s_ndrv = 0;
// Guards slot add/remove/compaction (usb_task) vs the Lua-side snapshot
// (_usb_drivers, core 0). Flag flips alone are atomic; the array shape isn't.
static portMUX_TYPE s_reg_mux = portMUX_INITIALIZER_UNLOCKED;

void usb_registry_add_builtin(const UsbDriverDesc* d) {
    if (!d || s_ndrv >= USB_MAX_DRIVERS) return;
    if (d->abi != USB_DRIVER_ABI_VERSION) return;
    portENTER_CRITICAL(&s_reg_mux);
    s_drv[s_ndrv].d        = d;
    s_drv[s_ndrv].probed   = false;
    s_drv[s_ndrv].running  = false;
    s_drv[s_ndrv].builtin  = true;
    s_drv[s_ndrv].mod      = NULL;
    s_drv[s_ndrv].conf     = NULL;
    s_drv[s_ndrv].conf_len = 0;
    s_drv[s_ndrv].pub_len  = 0;
    s_drv[s_ndrv].pub_seq  = 0;
    s_ndrv++;
    portEXIT_CRITICAL(&s_reg_mux);
}

static DrvSlot* slot_by_name(const char* name) {
    for (int i = 0; i < s_ndrv; i++)
        if (strcmp(s_drv[i].d->name, name) == 0) return &s_drv[i];
    return NULL;
}

bool usb_registry_probed(const char* name) {
    DrvSlot* s = slot_by_name(name);
    return s && s->probed;
}

static int drv_total_busy() {
    int n = 0;
    for (int i = 0; i < s_ndrv; i++)
        if (s_drv[i].running && s_drv[i].d->busy) n += s_drv[i].d->busy();
    return n;
}

// ── Dynamic drivers: manifest matching + attach-time load/unload ────────────
// Installed drivers live in per-driver dirs under L:/usb_drivers/ and
// S:/meshpunk/usb_drivers/, each holding a *.drv.elf, a `match` manifest
// (one `class/subclass/protocol` hex pattern per line, `*` wildcards), the
// store's .version, and optionally a `.disabled` marker. At device attach —
// after the interface walk, before probes — every enabled dir whose manifest
// matches an interface triple of the device is loaded into the driver pool
// and registered; probe-false and detach unload again. Nothing is ever
// loaded for devices that don't match, and installs activate on the next
// plug (no restart). All of this runs in usb_task.

#define DYN_MAX_TRIPLES 16

struct IfTriple { uint8_t cls, sub, proto; };

static bool match_field(const char* tok, uint8_t v) {
    while (*tok == ' ') tok++;
    if (tok[0] == '*') return true;
    return (uint8_t)strtoul(tok, NULL, 16) == v;
}

// One manifest line "cc/ss/pp" vs the device's interface triples.
static bool match_line(char* line, const IfTriple* t, int nt) {
    char* c1 = strchr(line, '/'); if (!c1) return false;
    char* c2 = strchr(c1 + 1, '/'); if (!c2) return false;
    *c1 = 0; *c2 = 0;
    for (int i = 0; i < nt; i++)
        if (match_field(line, t[i].cls) &&
            match_field(c1 + 1, t[i].sub) &&
            match_field(c2 + 1, t[i].proto)) return true;
    return false;
}

// Structural validation of a module's exported ops.
static bool dyn_desc_valid(const UsbDriverDesc* d) {
    if (!d || d->abi != USB_DRIVER_ABI_VERSION) return false;
    if (!d->name || !d->name[0] || !d->probe || !d->start || !d->stop) return false;
    // busy+resume are a pair; park is optional; all-NULL = guard-exempt.
    return (d->busy && d->resume) || (!d->busy && !d->resume && !d->park);
}

// Remove one dynamic slot (must not be running) and unload its module.
static void dyn_remove_slot(int i) {
    void*    mod  = s_drv[i].mod;
    uint8_t* conf = s_drv[i].conf;
    portENTER_CRITICAL(&s_reg_mux);
    for (int j = i; j < s_ndrv - 1; j++) s_drv[j] = s_drv[j + 1];
    s_ndrv--;
    portEXIT_CRITICAL(&s_reg_mux);
    if (conf) heap_caps_free(conf);
    elf_usb_driver_unload(mod);
}

static void dyn_unload_all(void) {
    for (int i = s_ndrv - 1; i >= 0; i--)
        if (!s_drv[i].builtin) dyn_remove_slot(i);
}

static void dyn_unload_unprobed(void) {
    for (int i = s_ndrv - 1; i >= 0; i--)
        if (!s_drv[i].builtin && !s_drv[i].probed) dyn_remove_slot(i);
}

// Scan both driver bases and load every enabled, manifest-matching module.
static void dyn_scan_and_load(const IfTriple* triples, int ntriples) {
    struct Base { const char* vfs; const char* drv; bool sd; };
    static const Base kBases[] = {
        { "/littlefs/usb_drivers",    "L:/usb_drivers",          false },
        { "/sd/meshpunk/usb_drivers", "S:/meshpunk/usb_drivers", true  },
    };
    char loaded[USB_MAX_DRIVERS][32];
    int  nloaded = 0;

    for (size_t b = 0; b < sizeof(kBases) / sizeof(kBases[0]); b++) {
        const Base& base = kBases[b];
        if (base.sd) sd_spi_take();              // POSIX /sd access needs the bus
        DIR* dir = opendir(base.vfs);
        if (!dir) { if (base.sd) sd_spi_release(); continue; }   // base absent

        struct dirent* de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;

            bool dup = false;                    // L: beats S: by dir name
            for (int i = 0; i < nloaded; i++)
                if (strncmp(loaded[i], de->d_name, 32) == 0) { dup = true; break; }
            if (dup) { ulog("drv: %s on both drives — L: wins", de->d_name); continue; }
            if (s_ndrv >= USB_MAX_DRIVERS) {
                ulog("drv: registry full — skipping %s", de->d_name);
                continue;
            }

            char p[192];
            struct stat st;
            snprintf(p, sizeof(p), "%s/%s/.disabled", base.vfs, de->d_name);
            if (stat(p, &st) == 0) continue;     // disabled: never load

            // Manifest match (no manifest = never matches).
            snprintf(p, sizeof(p), "%s/%s/match", base.vfs, de->d_name);
            FILE* mf = fopen(p, "r");
            if (!mf) continue;
            bool hit = false;
            char line[48];
            while (!hit && fgets(line, sizeof(line), mf))
                hit = match_line(line, triples, ntriples);
            fclose(mf);
            if (!hit) continue;

            // Find the module file.
            char elfname[64] = {0};
            snprintf(p, sizeof(p), "%s/%s", base.vfs, de->d_name);
            DIR* sub = opendir(p);
            if (sub) {
                struct dirent* fe;
                while ((fe = readdir(sub)) != NULL) {
                    size_t n = strlen(fe->d_name);
                    if (n > 8 && strcmp(fe->d_name + n - 8, ".drv.elf") == 0) {
                        strncpy(elfname, fe->d_name, sizeof(elfname) - 1);
                        break;
                    }
                }
                closedir(sub);
            }
            if (!elfname[0]) { ulog("drv: %s has no .drv.elf", de->d_name); continue; }

            char epath[192];
            snprintf(epath, sizeof(epath), "%s/%s/%s", base.drv, de->d_name, elfname);
            const void* ops = NULL;
            void* mod = elf_usb_driver_load(epath, &ops);   // re-takes the SD lock
            if (!mod) continue;                              // (recursive) as needed
            const UsbDriverDesc* d = (const UsbDriverDesc*)ops;
            if (!dyn_desc_valid(d)) {
                ulog("drv: %s invalid ops (abi %u, need %u)", de->d_name,
                     d ? (unsigned)d->abi : 0, (unsigned)USB_DRIVER_ABI_VERSION);
                elf_usb_driver_unload(mod);
                continue;
            }

            // Optional per-driver config (the `conf` file beside the elf).
            // Re-read on every attach: replug applies new config.
            uint8_t* conf = NULL;
            uint32_t conf_len = 0;
            snprintf(p, sizeof(p), "%s/%s/conf", base.vfs, de->d_name);
            FILE* cf = fopen(p, "r");
            if (cf) {
                conf = (uint8_t*)heap_caps_malloc(DRV_CONF_MAX,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (conf) {
                    conf_len = fread(conf, 1, DRV_CONF_MAX - 1, cf);
                    conf[conf_len] = 0;   // NUL for text-parsing drivers
                }
                fclose(cf);
            }

            portENTER_CRITICAL(&s_reg_mux);
            s_drv[s_ndrv].d        = d;
            s_drv[s_ndrv].probed   = false;
            s_drv[s_ndrv].running  = false;
            s_drv[s_ndrv].builtin  = false;
            s_drv[s_ndrv].mod      = mod;
            s_drv[s_ndrv].conf     = conf;
            s_drv[s_ndrv].conf_len = conf_len;
            s_drv[s_ndrv].pub_len  = 0;
            s_drv[s_ndrv].pub_seq  = 0;
            s_ndrv++;
            portEXIT_CRITICAL(&s_reg_mux);
            strncpy(loaded[nloaded], de->d_name, 31);
            loaded[nloaded][31] = 0;
            if (nloaded < USB_MAX_DRIVERS - 1) nloaded++;
        }
        closedir(dir);
        if (base.sd) sd_spi_release();
    }
}

// ── PHY restore (RTC-domain; survives warm reset) ───────────────────────────
// Hand-inlined usb_phy_ll_int_jtag_enable() — hal/usb_phy_ll.h can't be
// included from C++ (an unrelated inline copies a volatile struct).
static void restore_serial_jtag_phy() {
    USB_SERIAL_JTAG.conf0.phy_sel           = 0;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
    USB_SERIAL_JTAG.conf0.dp_pullup         = 1;
    USB_SERIAL_JTAG.conf0.usb_pad_enable    = 1;
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
    RTCCNTL.usb_conf.sw_usb_phy_sel    = 0;
}

// ── Enumeration events + helpers ─────────────────────────────────────────────

static volatile bool s_ctrl_done = false;
static void ctrl_cb(usb_transfer_t*) { s_ctrl_done = true; }

static void client_event_cb(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)      s_new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) s_dev_gone = true;
}

static void str_desc_ascii(const usb_str_desc_t* sd, char* out, int out_len) {
    int n = 0;
    if (sd) {
        int chars = (sd->bLength - 2) / 2;
        for (int i = 0; i < chars && n < out_len - 1; i++) {
            uint16_t c = sd->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = 0;
}

static const char* class_name(uint8_t cls) {
    switch (cls) {
        case USB_CLASS_PER_INTERFACE: return "per-interface";
        case USB_CLASS_AUDIO:         return "AUDIO";
        case USB_CLASS_COMM:          return "comm";
        case USB_CLASS_HID:           return "HID";
        case USB_CLASS_MASS_STORAGE:  return "mass storage";
        case USB_CLASS_HUB:           return "hub";
        case USB_CLASS_CDC_DATA:      return "cdc data";
        case USB_CLASS_VENDOR_SPEC:   return "vendor";
        default:                      return "other";
    }
}

static UsbKind class_to_kind(uint8_t cls) {
    switch (cls) {
        case USB_CLASS_AUDIO:        return UKIND_AUDIO;
        case USB_CLASS_HID:          return UKIND_HID;
        case USB_CLASS_MASS_STORAGE: return UKIND_MSC;
        case USB_CLASS_COMM:
        case USB_CLASS_CDC_DATA:     return UKIND_CDC;
        case USB_CLASS_HUB:          return UKIND_HUB;
        default:                     return UKIND_OTHER;
    }
}

// ── EP0 control request — usb_task context ONLY (self-pumping) ──────────────

bool usbcore_ctrl_req(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                      uint16_t wIndex, const uint8_t* data, uint16_t wLength) {
    usb_transfer_t* t = NULL;
    if (usb_host_transfer_alloc(8 + wLength, 0, &t) != ESP_OK) return false;
    uint8_t* b = t->data_buffer;
    b[0] = bmReqType; b[1] = bReq;
    b[2] = wValue & 0xFF;  b[3] = wValue >> 8;
    b[4] = wIndex & 0xFF;  b[5] = wIndex >> 8;
    b[6] = wLength & 0xFF; b[7] = wLength >> 8;
    if (data && wLength) memcpy(b + 8, data, wLength);
    t->num_bytes        = 8 + wLength;
    t->device_handle    = s_dev;
    t->bEndpointAddress = 0;
    t->callback         = ctrl_cb;
    t->context          = NULL;
    s_ctrl_done = false;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
        usb_host_transfer_free(t);
        return false;
    }
    // Pump BOTH handlers — EP0 completes on the default pipe (library-owned).
    for (int i = 0; i < 100 && !s_ctrl_done; i++) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    bool ok = s_ctrl_done && t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (!ok)
        ulog("ctrl %02X/%u: %s", bmReqType, bReq,
             !s_ctrl_done ? "timeout"
             : t->status == USB_TRANSFER_STATUS_STALL ? "STALL" : "error");
    usb_host_transfer_free(t);
    return ok;
}

// ── Dual-context wait primitive ──────────────────────────────────────────────
// On usb_task the pump isn't running (we ARE usb_task) — pump events until
// the flag. Foreign tasks block on the semaphore the usb_task pump gives.

static bool core_wait(volatile bool* flag, SemaphoreHandle_t sem, uint32_t timeout_ms) {
    if (xTaskGetCurrentTaskHandle() == s_usb_task_handle) {
        uint32_t t0 = millis();
        while (!*flag) {
            uint32_t f = 0;
            usb_host_lib_handle_events(0, &f);
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            if (millis() - t0 > timeout_ms) return false;
        }
        return true;
    }
    return xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// ── Pipe layer (the generalized MSC transaction engine) ─────────────────────

#define PIPE_IO_TIMEOUT_FLOOR 1

struct UsbPipe {
    usb_transfer_t*   xf;
    uint8_t           ep;
    uint16_t          mps;
    volatile int      in_flight;      // 0/1
    volatile bool     done_flag;      // sync wait
    SemaphoreHandle_t done_sem;
    bool              async_mode;
    void (*acb)(UsbPipe*, UsbXferResult, uint32_t, void*);
    void* actx;
};

static UsbXferResult map_status(usb_transfer_status_t st) {
    switch (st) {
        case USB_TRANSFER_STATUS_COMPLETED: return USB_XFER_OK;
        case USB_TRANSFER_STATUS_STALL:     return USB_XFER_STALL;
        case USB_TRANSFER_STATUS_TIMED_OUT: return USB_XFER_TIMEOUT;
        case USB_TRANSFER_STATUS_NO_DEVICE: return USB_XFER_NO_DEVICE;
        case USB_TRANSFER_STATUS_CANCELED:  return USB_XFER_CANCELED;
        default:                            return USB_XFER_ERROR;
    }
}

static void pipe_cb(usb_transfer_t* t) {
    UsbPipe* p = (UsbPipe*)t->context;
    p->in_flight = 0;                 // before the callback so it can resubmit
    if (p->async_mode) {
        if (p->acb) p->acb(p, map_status(t->status), t->actual_num_bytes, p->actx);
    } else {
        p->done_flag = true;
        xSemaphoreGive(p->done_sem);
    }
}

static UsbPipe* core_pipe_open(uint8_t ep_addr, uint16_t mps, uint32_t buf_len) {
    UsbPipe* p = (UsbPipe*)calloc(1, sizeof(UsbPipe));
    if (!p) return NULL;
    p->ep  = ep_addr;
    p->mps = mps ? mps : 64;
    p->done_sem = xSemaphoreCreateBinary();
    if (!p->done_sem) { free(p); return NULL; }
    if (usb_host_transfer_alloc(buf_len, 0, &p->xf) != ESP_OK) {
        vSemaphoreDelete(p->done_sem);
        free(p);
        return NULL;
    }
    return p;
}

// usb_task context only (drivers close pipes from stop()).
static void core_pipe_close(UsbPipe* p) {
    if (!p) return;
    if (p->in_flight && s_dev) {
        usb_host_endpoint_halt(s_dev, p->ep);
        usb_host_endpoint_flush(s_dev, p->ep);
    }
    // Drain the forced completion so the transfer object is idle before free.
    for (int i = 0; i < 100 && p->in_flight; i++) {
        uint32_t f = 0;
        usb_host_lib_handle_events(0, &f);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    if (p->in_flight) {
        // Wedged past 1s: leak rather than free under a pending completion.
        ulog("pipe %02X: close timeout — leaking", p->ep);
        return;
    }
    usb_host_transfer_free(p->xf);
    vSemaphoreDelete(p->done_sem);
    free(p);
}

static uint8_t* core_pipe_buf(UsbPipe* p)      { return p ? p->xf->data_buffer : NULL; }
static int      core_pipe_in_flight(UsbPipe* p){ return p ? p->in_flight : 0; }
static uint32_t core_pipe_actual(UsbPipe* p)   { return p ? p->xf->actual_num_bytes : 0; }
static void     core_pipe_set_ep(UsbPipe* p, uint8_t ep) {
    if (p && !p->in_flight) p->ep = ep;
}

static UsbXferResult core_pipe_xfer(UsbPipe* p, uint32_t len, uint32_t timeout_ms) {
    if (!p || !s_dev) return USB_XFER_NO_DEVICE;
    if (timeout_ms < PIPE_IO_TIMEOUT_FLOOR) timeout_ms = PIPE_IO_TIMEOUT_FLOOR;
    p->async_mode       = false;
    p->xf->device_handle    = s_dev;
    p->xf->bEndpointAddress = p->ep;
    p->xf->num_bytes        = (int)len;
    p->xf->callback         = pipe_cb;
    p->xf->context          = p;
    p->done_flag = false;
    xSemaphoreTake(p->done_sem, 0);   // drain a stale give
    p->in_flight = 1;
    if (usb_host_transfer_submit(p->xf) != ESP_OK) {
        p->in_flight = 0;
        return USB_XFER_ERROR;
    }
    if (!core_wait(&p->done_flag, p->done_sem, timeout_ms)) {
        // Still in flight past the timeout — force completion so the pipe is
        // reusable before we return (host side only; wire recovery is the
        // caller's call via pipe_reset).
        usb_host_endpoint_halt(s_dev, p->ep);
        usb_host_endpoint_flush(s_dev, p->ep);
        core_wait(&p->done_flag, p->done_sem, 1000);
        usb_host_endpoint_clear(s_dev, p->ep);
        return USB_XFER_TIMEOUT;
    }
    return map_status(p->xf->status);
}

static bool core_pipe_submit(UsbPipe* p, uint32_t len,
                             void (*cb)(UsbPipe*, UsbXferResult, uint32_t, void*),
                             void* ctx) {
    if (!p || !s_dev || p->in_flight) return false;
    p->async_mode       = true;
    p->acb              = cb;
    p->actx             = ctx;
    p->xf->device_handle    = s_dev;
    p->xf->bEndpointAddress = p->ep;
    p->xf->num_bytes        = (int)len;
    p->xf->callback         = pipe_cb;
    p->xf->context          = p;
    p->in_flight = 1;
    if (usb_host_transfer_submit(p->xf) != ESP_OK) {
        p->in_flight = 0;
        return false;
    }
    return true;
}

static void core_pipe_cancel(UsbPipe* p) {
    if (!p || !s_dev) return;
    usb_host_endpoint_halt(s_dev, p->ep);
    usb_host_endpoint_flush(s_dev, p->ep);
}

// ── Dual-context control transfer (api->control) ────────────────────────────
// Serialized by a mutex (control transactions are short); its own waiter so a
// foreign task's control can't tangle with a pipe wait.

static SemaphoreHandle_t s_ctrlx_mutex = nullptr;
static SemaphoreHandle_t s_ctrlx_sem   = nullptr;
static volatile bool     s_ctrlx_flag  = false;

static void ctrlx_cb(usb_transfer_t*) {
    s_ctrlx_flag = true;
    xSemaphoreGive(s_ctrlx_sem);
}

static bool core_control(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                         uint16_t wIndex, uint8_t* data, uint16_t wLength) {
    if (!s_dev || !s_ctrlx_mutex) return false;
    if (xSemaphoreTake(s_ctrlx_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    usb_transfer_t* t = NULL;
    bool ok = false;
    if (usb_host_transfer_alloc(8 + wLength, 0, &t) == ESP_OK) {
        uint8_t* b = t->data_buffer;
        b[0] = bmReqType; b[1] = bReq;
        b[2] = wValue & 0xFF;  b[3] = wValue >> 8;
        b[4] = wIndex & 0xFF;  b[5] = wIndex >> 8;
        b[6] = wLength & 0xFF; b[7] = wLength >> 8;
        if (data && wLength && !(bmReqType & 0x80)) memcpy(b + 8, data, wLength);
        t->num_bytes        = 8 + wLength;
        t->device_handle    = s_dev;
        t->bEndpointAddress = 0;
        t->callback         = ctrlx_cb;
        t->context          = NULL;
        s_ctrlx_flag = false;
        xSemaphoreTake(s_ctrlx_sem, 0);
        if (usb_host_transfer_submit_control(s_client, t) == ESP_OK) {
            ok = core_wait(&s_ctrlx_flag, s_ctrlx_sem, 5000)
              && t->status == USB_TRANSFER_STATUS_COMPLETED;
            if (ok && data && wLength && (bmReqType & 0x80)) memcpy(data, b + 8, wLength);
        }
        usb_host_transfer_free(t);   // freed-after-timeout exposure matches
                                     // ctrl_req: EP0 completes or NO_DEVICEs fast
    }
    xSemaphoreGive(s_ctrlx_mutex);
    return ok;
}

static void core_pipe_reset(UsbPipe* p) {
    if (!p || !s_dev) return;
    core_control(0x02, 0x01 /*CLEAR_FEATURE(ENDPOINT_HALT)*/, 0, p->ep, NULL, 0);
    usb_host_endpoint_clear(s_dev, p->ep);
}

// ── Input sockets ────────────────────────────────────────────────────────────
// UI held-image (consumed by keyboard_read_cb via usb_kbd_snapshot), module
// key edges (elf_input_inject) and nav counters (trackball ISR channel).

static bool         s_usb_held[128];
static volatile int s_usb_nheld = 0;
static portMUX_TYPE s_kbd_mux = portMUX_INITIALIZER_UNLOCKED;

extern volatile int trackball_up, trackball_down, trackball_left,
                    trackball_right, trackball_click;

static void sock_input_set_held(const bool held[128], int n) {
    portENTER_CRITICAL(&s_kbd_mux);
    memcpy(s_usb_held, held, sizeof(s_usb_held));
    s_usb_nheld = n;
    portEXIT_CRITICAL(&s_kbd_mux);
}

static void sock_input_key(uint8_t code, bool pressed) {
    elf_input_inject(code, pressed ? 1 : 0);
}

static void sock_input_nav(int up, int down, int left, int right, int click) {
    if (up    > 0) trackball_up    += up;
    if (down  > 0) trackball_down  += down;
    if (left  > 0) trackball_left  += left;
    if (right > 0) trackball_right += right;
    if (click > 0) trackball_click += click;
}

static bool sock_input_module_active(void) {
    return elf_input_active();
}

// Pipeline-A consumer (keyboard_read_cb, core 0): copy the USB-held key set.
bool usb_kbd_snapshot(bool out[128]) {
    if (s_usb_nheld == 0) return false;       // fast path: nothing held
    portENTER_CRITICAL(&s_kbd_mux);
    memcpy(out, s_usb_held, sizeof(s_usb_held));
    portEXIT_CRITICAL(&s_kbd_mux);
    return true;
}

// ── Block-device socket ──────────────────────────────────────────────────────
// One binding. In M1 usb_fs still calls the msc sector API directly; the
// socket is registered/unregistered by the msc driver so the plumbing is
// exercised, and becomes load-bearing when storage drivers go dynamic.

static const UsbBlockOps* s_block_ops = NULL;

static bool sock_block_register(const UsbBlockOps* ops) {
    if (s_block_ops || !ops) return false;
    s_block_ops = ops;
    return true;
}
static void sock_block_unregister(void) { s_block_ops = NULL; }

// ── Misc api glue ────────────────────────────────────────────────────────────

static const void* api_device_desc(void) { return s_dd; }
static const void* api_config_desc(void) { return s_cfg; }

static bool api_claim_interface(uint8_t ifnum, uint8_t alt) {
    if (!s_dev) return false;
    esp_err_t err = usb_host_interface_claim(s_client, s_dev, ifnum, alt);
    if (err != ESP_OK) ulog("claim IF%u alt%u failed: %s", ifnum, alt, esp_err_to_name(err));
    return err == ESP_OK;
}

static void api_release_interface(uint8_t ifnum) {
    if (s_dev) usb_host_interface_release(s_client, s_dev, ifnum);
}

static uint32_t api_ticks_ms(void) { return millis(); }
static void     api_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static bool     api_guard_pending(void) { return s_flash_pause_req; }

static void api_pump(uint32_t ms) {
    if (xTaskGetCurrentTaskHandle() != s_usb_task_handle) return;
    uint32_t f = 0;
    usb_host_lib_handle_events(0, &f);
    usb_host_client_handle_events(s_client, pdMS_TO_TICKS(ms));
}

static DrvSlot* slot_by_desc(const void* self) {
    for (int i = 0; i < s_ndrv; i++)
        if ((const void*)s_drv[i].d == self) return &s_drv[i];
    return NULL;
}

static const uint8_t* api_config(const void* self, uint32_t* len) {
    DrvSlot* s = slot_by_desc(self);
    if (!s || !s->conf) { if (len) *len = 0; return NULL; }
    if (len) *len = s->conf_len;
    return s->conf;
}

static void api_publish(const void* self, const void* data, uint32_t len) {
    DrvSlot* s = slot_by_desc(self);
    if (!s || !data) return;
    if (len > sizeof(s->pub)) len = sizeof(s->pub);
    portENTER_CRITICAL(&s_reg_mux);
    memcpy(s->pub, data, len);
    s->pub_len = (uint8_t)len;
    s->pub_seq++;
    portEXIT_CRITICAL(&s_reg_mux);
}

static const UsbHostApi s_api = {
    USB_DRIVER_ABI_VERSION,
    &usbcore_log,
    &api_device_desc,
    &api_config_desc,
    &api_claim_interface,
    &api_release_interface,
    &core_control,
    &core_pipe_open,
    &core_pipe_close,
    &core_pipe_buf,
    &core_pipe_xfer,
    &core_pipe_submit,
    &core_pipe_in_flight,
    &core_pipe_cancel,
    &core_pipe_reset,
    &core_pipe_actual,
    &core_pipe_set_ep,
    &sock_input_key,
    &sock_input_set_held,
    &sock_input_nav,
    &sock_input_module_active,
    &sock_block_register,
    &sock_block_unregister,
    &api_ticks_ms,
    &api_delay_ms,
    &api_guard_pending,
    &api_pump,
    &api_config,
    &api_publish,
};

const UsbHostApi* usbcore_api(void) { return &s_api; }

// ── Enumeration ──────────────────────────────────────────────────────────────
// Open the device, log its identity + interface map, classify, then let every
// registered driver probe. Keeps the device open so DEV_GONE fires on unplug.

static void describe_device(uint8_t addr) {
    ulog("Device connected, addr %u — opening...", addr);

    esp_err_t err = usb_host_device_open(s_client, addr, &s_dev);
    if (err != ESP_OK) { ulog("open failed: %s", esp_err_to_name(err)); s_dev = NULL; return; }

    memset(&s_info, 0, sizeof(s_info));
    s_info.connected = true;

    usb_device_info_t info;
    if (usb_host_device_info(s_dev, &info) == ESP_OK) {
        char mfg[32];
        str_desc_ascii(info.str_desc_manufacturer, mfg, sizeof(mfg));
        str_desc_ascii(info.str_desc_product, s_info.product, sizeof(s_info.product));
        ulog("speed: %s", info.speed == USB_SPEED_FULL ? "full (12M)" : "low (1.5M)");
        if (mfg[0])            ulog("mfg:  %s", mfg);
        if (s_info.product[0]) ulog("prod: %s", s_info.product);
    }

    s_dd = NULL;
    if (usb_host_get_device_descriptor(s_dev, &s_dd) == ESP_OK && s_dd) {
        s_info.vid = s_dd->idVendor;
        s_info.pid = s_dd->idProduct;
        ulog("VID %04X  PID %04X  dev class %02X",
             s_dd->idVendor, s_dd->idProduct, s_dd->bDeviceClass);
        if (s_dd->bDeviceClass && s_dd->bDeviceClass != USB_CLASS_PER_INTERFACE)
            s_info.kind = class_to_kind(s_dd->bDeviceClass);
    }

    s_cfg = NULL;
    err = usb_host_get_active_config_descriptor(s_dev, &s_cfg);
    if (err != ESP_OK || !s_cfg) {
        ulog("config desc failed: %s", esp_err_to_name(err));
        s_cfg = NULL;
        return;
    }

    // Generic interface map (drivers do their own walks in probe); also
    // collect the interface triples the dynamic-driver manifests match on.
    IfTriple triples[DYN_MAX_TRIPLES];
    int ntriples = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)s_cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, s_cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType != USB_B_DESCRIPTOR_TYPE_INTERFACE) continue;
        const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
        ulog("IF %u alt %u: class %02X/%02X (%s) eps %u",
             i->bInterfaceNumber, i->bAlternateSetting,
             i->bInterfaceClass, i->bInterfaceSubClass,
             class_name(i->bInterfaceClass), i->bNumEndpoints);
        if (s_info.kind == UKIND_NONE && i->bInterfaceClass != USB_CLASS_PER_INTERFACE)
            s_info.kind = class_to_kind(i->bInterfaceClass);
        if (ntriples < DYN_MAX_TRIPLES) {
            triples[ntriples].cls   = i->bInterfaceClass;
            triples[ntriples].sub   = i->bInterfaceSubClass;
            triples[ntriples].proto = i->bInterfaceProtocol;
            ntriples++;
        }
    }

    // Load any installed dynamic drivers whose manifest matches this device
    // (into the boot-reserved pool), then probe: dynamics first, built-ins
    // after — a probed dynamic suppresses a built-in of the same name (the
    // dogfood/override rule). Each probe fully resets its driver's state and
    // logs its own findings (audio prints its ">>> Audio device ready" line).
    dyn_scan_and_load(triples, ntriples);

    bool any = false;
    for (int i = 0; i < s_ndrv; i++) {
        if (s_drv[i].builtin) continue;
        s_drv[i].probed  = s_drv[i].d->probe && s_drv[i].d->probe(&s_api);
        s_drv[i].running = false;
        any = any || s_drv[i].probed;
    }
    for (int i = 0; i < s_ndrv; i++) {
        if (!s_drv[i].builtin) continue;
        bool suppressed = false;
        for (int j = 0; j < s_ndrv; j++)
            if (!s_drv[j].builtin && s_drv[j].probed &&
                strcmp(s_drv[j].d->name, s_drv[i].d->name) == 0) {
                suppressed = true;
                break;
            }
        if (suppressed) {
            ulog("drv: builtin %s suppressed by module", s_drv[i].d->name);
            s_drv[i].probed  = false;
            s_drv[i].running = false;
            continue;
        }
        s_drv[i].probed  = s_drv[i].d->probe && s_drv[i].d->probe(&s_api);
        s_drv[i].running = false;
        any = any || s_drv[i].probed;
    }

    // Modules that matched by manifest but declined at probe: unload now —
    // nothing stays in the pool for a device it doesn't serve.
    dyn_unload_unprobed();

    // Classification tail — same messages/priority as the monolith.
    if (!usb_registry_probed("audio")) {
        if (usb_registry_probed("msc"))
            ulog(">>> Storage detected — mounting.");
        else if (usb_registry_probed("kbd"))
            ulog(">>> Keyboard detected — driver starting.");
        else if (!any) {
            if (s_info.kind == UKIND_HUB)
                ulog(">>> Hub — unsupported until IDF5 (single device only).");
            else
                ulog(">>> Device kind: %s (no driver yet).", kind_name(s_info.kind));
        }
    }
}

// ── Host task ────────────────────────────────────────────────────────────────

static void usb_task(void*) {
    s_usb_task_handle = xTaskGetCurrentTaskHandle();   // dual-context wait test
    // Install from THIS task so esp_intr_alloc binds a core-1 interrupt slot.
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags     = 0;
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ulog("usb_host_install failed: %s", esp_err_to_name(err));
        s_running = false; vTaskDelete(NULL); return;
    }

    usb_host_client_config_t client_cfg = {};
    client_cfg.is_synchronous              = false;
    client_cfg.max_num_event_msg           = 5;
    client_cfg.async.client_event_callback = client_event_cb;
    client_cfg.async.callback_arg          = NULL;
    err = usb_host_client_register(&client_cfg, &s_client);
    if (err != ESP_OK) {
        ulog("client_register failed: %s", esp_err_to_name(err));
        usb_host_uninstall(); s_client = NULL; s_running = false; vTaskDelete(NULL); return;
    }

    ulog("Host running — plug in a powered dongle.");

    while (!s_stop_req) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));

        // Flash-write guard: park every running driver's deadline-bearing
        // transfers, drain them to zero in flight, then idle until released.
        // The caller wakes us promptly via usb_host_client_unblock().
        if (s_flash_pause_req && !s_flash_paused) {
            for (int i = 0; i < s_ndrv; i++) {
                const UsbDriverDesc* d = s_drv[i].d;
                if (s_drv[i].running && d->park && d->busy && d->busy() > 0)
                    d->park();
            }
            for (int i = 0; i < 100 && drv_total_busy() > 0; i++) {
                uint32_t f = 0;
                usb_host_lib_handle_events(0, &f);
                usb_host_client_handle_events(s_client, pdMS_TO_TICKS(2));
            }
            s_flash_paused = true;
        } else if (!s_flash_pause_req && s_flash_paused) {
            for (int i = 0; i < s_ndrv; i++)
                if (s_drv[i].running && s_drv[i].d->resume) s_drv[i].d->resume();
            s_flash_paused = false;
        }

        if (s_new_addr) {
            uint8_t addr = s_new_addr;
            s_new_addr = 0;
            if (!s_dev) describe_device(addr);
        }
        if (s_dev_gone) {
            s_dev_gone = false;
            ulog("Device disconnected.");
            for (int i = 0; i < s_ndrv; i++) {
                if (s_drv[i].running) {
                    s_drv[i].d->stop(&s_api, false);
                    s_drv[i].running = false;
                }
                s_drv[i].probed = false;
            }
            dyn_unload_all();   // stopped above; pool space returns here
            if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
            s_dd = NULL; s_cfg = NULL;
            memset(&s_info, 0, sizeof(s_info));
        }

        // Per-driver want/start/stop + tick. want() folds in driver prefs
        // (audio routing/tone) and driver give-up (a start failure that
        // invalidates the driver's own profile drops want to false — the
        // monolith's no-retry-spin semantics). Don't START mid-guard.
        for (int i = 0; i < s_ndrv; i++) {
            DrvSlot* st = &s_drv[i];
            const UsbDriverDesc* d = st->d;
            if (!st->probed) continue;
            bool w = d->want ? d->want() : true;
            if (w && !st->running && !s_flash_pause_req)
                st->running = d->start(&s_api);
            else if (!w && st->running) {
                d->stop(&s_api, true);
                st->running = false;
            }
            if (st->running && d->tick) d->tick(&s_api);
        }
    }

    // Teardown in the order usb_host.h requires.
    for (int i = 0; i < s_ndrv; i++) {
        if (s_drv[i].running) {
            s_drv[i].d->stop(&s_api, true);
            s_drv[i].running = false;
        }
        s_drv[i].probed = false;
    }
    dyn_unload_all();
    if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
    s_dd = NULL; s_cfg = NULL;
    usb_host_client_deregister(s_client);
    s_client = NULL;
    usb_host_device_free_all();
    for (int i = 0; i < 40; i++) {
        uint32_t f = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(50), &f);
        if (f & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
    }
    esp_err_t uerr = usb_host_uninstall();
    restore_serial_jtag_phy();
    memset(&s_info, 0, sizeof(s_info));
    ulog("Host stopped (%s). USB serial restored", uerr == ESP_OK ? "clean" : esp_err_to_name(uerr));
    ulog("(replug the PC cable if it doesn't show).");

    s_running = false;
    vTaskDelete(NULL);
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void usb_manager_init(void (*prefs_save_fn)()) {
    s_prefs_save = prefs_save_fn;
    // Recursive: guarded call chains nest (e.g. a guarded prefs save calling
    // meshpunk_open, which guards its own LittleFS truncate). Only the
    // OUTERMOST level pauses/resumes the stream — see s_flash_guard_depth.
    s_flash_mux   = xSemaphoreCreateRecursiveMutex();
    s_ctrlx_mutex = xSemaphoreCreateMutex();          // priority inheritance
    s_ctrlx_sem   = xSemaphoreCreateBinary();

    // Built-ins, in lifecycle order (start/stop/DEV_GONE iterate forward).
    usb_registry_add_builtin(usbaud_desc());
    usb_registry_add_builtin(usbkbd_desc());
    usb_registry_add_builtin(usbmsc_desc());
}

bool usb_manager_start() {
    if (s_running) return true;

    if (!usbaud_session_reset()) {
        ulog("sink alloc failed (no PSRAM)");
        return false;
    }

    ulog("Switching USB port to HOST mode.");
    ulog("(USB serial is OFF until Stop or reboot)");

    s_stop_req = false; s_new_addr = 0; s_dev_gone = false;
    s_client = NULL; s_running = true;
    for (int i = 0; i < s_ndrv; i++) { s_drv[i].probed = false; s_drv[i].running = false; }
    // 8KB stack. A 6KB trim caused crashes ("works then dies after a short
    // time") — the USB host library's enumeration/transfer callbacks run on
    // THIS stack and go deeper than 6KB. Driver hooks share it too (<=2KB
    // per call, per the ABI budget).
    if (xTaskCreatePinnedToCore(usb_task, "usb_mgr", 8192, NULL, 4, NULL, 1) != pdPASS) {
        ulog("task create failed");
        s_running = false;
        return false;
    }
    return true;
}

void usb_manager_stop() { if (s_running) s_stop_req = true; }
bool usb_manager_running() { return s_running; }

// ── Flash-write guard (see usb_manager.h) ────────────────────────────────────

// Nesting depth of the current holder. Mutated only while s_flash_mux is held,
// so no extra locking needed.
static int s_flash_guard_depth = 0;

void usb_flash_guard_begin() {
    if (!s_flash_mux) return;                 // pre-init: USB can't be up yet
    xSemaphoreTakeRecursive(s_flash_mux, portMAX_DELAY);   // held until end()
    if (++s_flash_guard_depth > 1) return;    // nested: outer level already paused
    if (!s_running) return;                   // no usb_task — nothing to coordinate
    // Always coordinate with the task when it's alive (even if not streaming
    // yet): the pause_req also blocks a stream from STARTING mid-write.
    s_flash_pause_req = true;
    if (s_client) usb_host_client_unblock(s_client);   // wake the usb_task now
    // Wait for it to drain + park (bounded; drain is ~<=30ms). Also unblocks
    // early if the task is tearing down (s_running clears).
    for (int i = 0; i < 300 && !s_flash_paused && s_running; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    // The usb_task can sit in a >300ms control-transfer wait during
    // enumeration and miss the park request. The ISO callbacks still see
    // pause_req and stop resubmitting, so wait (bounded) for the audio
    // in-flight count itself before letting the caller stall the flash cache.
    if (s_running && !s_flash_paused) {
        for (int i = 0; i < 1700 && usbaud_iso_busy() > 0 && s_running; i++)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (usbaud_iso_busy() > 0)
            ulog("flash guard: ISO drain timed out — write proceeding");
    }
}

void usb_flash_guard_end() {
    if (!s_flash_mux) return;
    if (s_flash_guard_depth > 0 && --s_flash_guard_depth == 0) {
        if (s_flash_pause_req) {
            s_flash_pause_req = false;
            if (s_running && s_client) usb_host_client_unblock(s_client);
        }
    }
    xSemaphoreGiveRecursive(s_flash_mux);
}

// ── Lua bridge ───────────────────────────────────────────────────────────────

void usb_manager_register_lua(lua_State* L) {
    // Boot self-heal: a reset while host mode was active leaves the RTC PHY
    // select on OTG (USB serial dead). Guard so re-registration after an ELF
    // Lua re-init doesn't stomp an active session.
    if (!s_running) restore_serial_jtag_phy();

    lua_register(L, "_usb_start",   [](lua_State* L) -> int { lua_pushboolean(L, usb_manager_start());   return 1; });
    lua_register(L, "_usb_stop",    [](lua_State* L) -> int { usb_manager_stop();                        return 0; });
    lua_register(L, "_usb_running", [](lua_State* L) -> int { lua_pushboolean(L, usb_manager_running()); return 1; });

    // nil, or a table describing the connected device for the UI.
    lua_register(L, "_usb_device", [](lua_State* L) -> int {
        if (!s_info.connected) { lua_pushnil(L); return 1; }
        lua_newtable(L);
        lua_pushinteger(L, s_info.vid);          lua_setfield(L, -2, "vid");
        lua_pushinteger(L, s_info.pid);          lua_setfield(L, -2, "pid");
        lua_pushstring(L, s_info.product);       lua_setfield(L, -2, "product");
        lua_pushstring(L, kind_name(s_info.kind)); lua_setfield(L, -2, "kind");
        lua_pushboolean(L, usb_registry_probed("kbd")); lua_setfield(L, -2, "kbd");
        lua_pushboolean(L, usb_fs_mounted());    lua_setfield(L, -2, "msc");
        lua_pushnumber(L, usbmsc_capacity_mb()); lua_setfield(L, -2, "msc_mb");
        lua_pushinteger(L, s_info.rate);         lua_setfield(L, -2, "rate");
        lua_pushinteger(L, s_info.bits);         lua_setfield(L, -2, "bits");
        lua_pushboolean(L, usb_audio_active());  lua_setfield(L, -2, "streaming");
        return 1;
    });

    lua_register(L, "_usb_audio_get", [](lua_State* L) -> int { lua_pushboolean(L, usb_audio_pref_get()); return 1; });
    lua_register(L, "_usb_audio_set", [](lua_State* L) -> int {
        usb_audio_pref_set(lua_toboolean(L, 1));
        if (s_prefs_save) s_prefs_save();
        lua_pushboolean(L, usb_audio_pref_get());
        return 1;
    });
    lua_register(L, "_usb_speaker_get", [](lua_State* L) -> int { lua_pushboolean(L, usb_speaker_pref_get()); return 1; });
    lua_register(L, "_usb_speaker_set", [](lua_State* L) -> int {
        usb_speaker_pref_set(lua_toboolean(L, 1));
        if (s_prefs_save) s_prefs_save();
        lua_pushboolean(L, usb_speaker_pref_get());
        return 1;
    });

    // Debug tone toggle.
    lua_register(L, "_usb_tone", [](lua_State* L) -> int {
        lua_pushboolean(L, usbaud_tone_toggle());
        return 1;
    });

    // One log line per call, nil when drained.
    lua_register(L, "_usb_poll", [](lua_State* L) -> int {
        char line[ULOG_LINE_LEN];
        if (ulog_pop(line)) lua_pushstring(L, line);
        else                lua_pushnil(L);
        return 1;
    });

    // Registry snapshot for the Drivers UI: array of {name, builtin, active,
    // running [, status]}, plus the driver pool's free bytes. Everything a
    // dynamic slot needs is COPIED under the registry mux — after it, the
    // module could unload at any moment, so no dynamic pointers escape
    // (status() is only invoked for built-ins, which never unload).
    // NOTE: lua_register is a MACRO — commas inside this lambda must all sit
    // inside parentheses or they split the macro arguments (braces don't
    // protect). Hence the one-per-line member declarations.
    lua_register(L, "_usb_drivers", [](lua_State* L) -> int {
        struct Snap {
            char name[24];
            bool builtin;
            bool probed;
            bool running;
            const UsbDriverDesc* d;   // built-ins only (safe to call later)
        } snap[USB_MAX_DRIVERS];
        portENTER_CRITICAL(&s_reg_mux);
        int n = s_ndrv;
        for (int i = 0; i < n; i++) {
            strncpy(snap[i].name, s_drv[i].d->name, sizeof(snap[i].name) - 1);
            snap[i].name[sizeof(snap[i].name) - 1] = 0;
            snap[i].builtin = s_drv[i].builtin;
            snap[i].probed  = s_drv[i].probed;
            snap[i].running = s_drv[i].running;
            snap[i].d       = s_drv[i].builtin ? s_drv[i].d : NULL;
        }
        portEXIT_CRITICAL(&s_reg_mux);
        lua_newtable(L);
        for (int i = 0; i < n; i++) {
            lua_newtable(L);
            lua_pushstring(L, snap[i].name);     lua_setfield(L, -2, "name");
            lua_pushboolean(L, snap[i].builtin); lua_setfield(L, -2, "builtin");
            lua_pushboolean(L, snap[i].probed);  lua_setfield(L, -2, "active");
            lua_pushboolean(L, snap[i].running); lua_setfield(L, -2, "running");
            if (snap[i].d && snap[i].d->status) {
                char st[24];
                snap[i].d->status(st, sizeof(st));
                lua_pushstring(L, st);
                lua_setfield(L, -2, "status");
            }
            lua_rawseti(L, -2, i + 1);
        }
        lua_pushnumber(L, (lua_Number)usb_pool_free_bytes());
        return 2;
    });

    // Latest published blob from a named dynamic driver: blob, seq — or nil.
    // (The gamepad setup app's learning feed.) Copied under the mux; commas
    // inside this lambda must stay inside parentheses (lua_register macro).
    lua_register(L, "_usb_drv_read", [](lua_State* L) -> int {
        const char* name = luaL_checkstring(L, 1);
        uint8_t buf[64];
        uint8_t len = 0;
        uint32_t seq = 0;
        bool found = false;
        portENTER_CRITICAL(&s_reg_mux);
        for (int i = 0; i < s_ndrv; i++) {
            if (strcmp(s_drv[i].d->name, name) == 0) {
                len = s_drv[i].pub_len;
                seq = s_drv[i].pub_seq;
                memcpy(buf, s_drv[i].pub, len);
                found = true;
                break;
            }
        }
        portEXIT_CRITICAL(&s_reg_mux);
        if (!found || seq == 0) { lua_pushnil(L); return 1; }
        lua_pushlstring(L, (const char*)buf, len);
        lua_pushinteger(L, (lua_Integer)seq);
        return 2;
    });
}
