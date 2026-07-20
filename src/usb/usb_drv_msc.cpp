// USB class driver: MSC Bulk-Only Transport (thumb drives) — VTABLE-CLEAN.
//
// SCSI over a bulk IN/OUT pair, entirely through the UsbHostApi pipe layer
// (which is this driver's old dual-context engine, promoted into the core).
// Two pipes, retargeted between the endpoints the way the monolith reused
// its transfer objects: s_pipe_cmd (64B — CBW out, CSW in) and s_pipe_data
// (MSC_CHUNK_BYTES — data phases, either direction). Same 16KB+64B internal
// DMA footprint as before.
//
// Task/locking model unchanged: SCSI transactions are serialized by
// s_msc_mutex and may originate on ANY task (Lua file I/O on core 0,
// sound_task, ...); pipe_xfer's dual-context wait handles both sides.
// FLASH-GUARD EXEMPT by design (busy/park/resume all NULL): bulk has no
// isochronous deadline — a transfer in flight across a flash cache-stall
// just has its completion latched until the cores resume, and staying off
// the guard mutex lets a guard HOLDER do USB-drive I/O mid-guard (fs copy
// U:→L: wraps the whole op in UsbFlashGuardIf) with no deadlock cycle.

#include "../usb_manager.h"
#include "usb_core_int.h"
#include "../usb_fs.h"              // mount/unmount the FatFs volume

#include <Arduino.h>

#define ulog usbcore_log

// ── Profile + state ──────────────────────────────────────────────────────────

struct MscProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep_in, ep_out;       // bulk endpoints (0x8x / 0x0x)
    uint16_t mps_in, mps_out;
};
static MscProfile s_msc_prof;

#define MSC_CHUNK_BYTES   16384       // data-phase ceiling (32 × 512B sectors)
#define MSC_IO_TIMEOUT_MS 5000        // per-phase completion timeout

static const UsbHostApi* s_mapi      = NULL;    // captured at probe
static UsbPipe*          s_pipe_cmd  = NULL;    // 64B: CBW out / CSW in
static UsbPipe*          s_pipe_data = NULL;    // 16KB: data phases
static volatile bool     s_msc_on    = false;   // claimed + unit ready
static SemaphoreHandle_t s_msc_mutex = nullptr; // serializes transactions
static uint32_t          s_msc_tag   = 1;       // CBW tag counter
static uint32_t          s_msc_sectors = 0;     // capacity (last LBA + 1)
static uint32_t          s_msc_ssize   = 0;     // logical sector size

// ── BOT plumbing ─────────────────────────────────────────────────────────────

// Clear a bulk-endpoint stall on BOTH sides: pipe_cancel = host halt+flush,
// pipe_reset = wire CLEAR_FEATURE + host clear (data toggles back to DATA0).
// The pipe must already be targeted at the stalled endpoint.
static void msc_clear_stall(UsbPipe* p) {
    s_mapi->pipe_cancel(p);
    s_mapi->pipe_reset(p);
}

// BOT §5.3.4 reset recovery: Bulk-Only Mass Storage Reset + both stalls.
static void msc_bot_reset() {
    ulog("msc: BOT reset");
    s_mapi->control(0x21, 0xFF /*Bulk-Only Reset*/, 0, s_msc_prof.ifnum, NULL, 0);
    s_mapi->pipe_set_ep(s_pipe_cmd, s_msc_prof.ep_in);
    msc_clear_stall(s_pipe_cmd);
    s_mapi->pipe_set_ep(s_pipe_data, s_msc_prof.ep_out);
    msc_clear_stall(s_pipe_data);
}

// One full SCSI transaction: CBW → optional data phase (≤ MSC_CHUNK_BYTES via
// pipe_buf(s_pipe_data); OUT payloads pre-loaded by the caller, IN payloads
// read out by the caller) → CSW. True only on CSW GOOD with a clean data
// phase. `data_in_len` receives the actual IN byte count when non-NULL.
// Callers hold s_msc_mutex, except the pre-mount init sequence in msc_start
// (no foreign access can exist before the volume registers).
static bool msc_scsi(const uint8_t* cdb, uint8_t cdb_len,
                     bool dir_in, uint32_t data_len, uint32_t* data_in_len) {
    if (!s_mapi || !s_pipe_cmd || !s_pipe_data) return false;

    // CBW (31 bytes)
    uint8_t* b = s_mapi->pipe_buf(s_pipe_cmd);
    uint32_t tag = s_msc_tag++;
    memset(b, 0, 31);
    b[0]='U'; b[1]='S'; b[2]='B'; b[3]='C';
    b[4] = (uint8_t)tag; b[5] = (uint8_t)(tag>>8);
    b[6] = (uint8_t)(tag>>16); b[7] = (uint8_t)(tag>>24);
    b[8]  = (uint8_t)data_len;        b[9]  = (uint8_t)(data_len>>8);
    b[10] = (uint8_t)(data_len>>16);  b[11] = (uint8_t)(data_len>>24);
    b[12] = dir_in ? 0x80 : 0x00;
    b[13] = 0;                        // LUN 0
    b[14] = cdb_len;
    memcpy(b + 15, cdb, cdb_len);
    s_mapi->pipe_set_ep(s_pipe_cmd, s_msc_prof.ep_out);
    if (s_mapi->pipe_xfer(s_pipe_cmd, 31, MSC_IO_TIMEOUT_MS) != USB_XFER_OK) {
        msc_bot_reset();
        return false;
    }

    // Data phase
    uint32_t in_got  = 0;
    bool     data_ok = true;
    if (data_len) {
        uint32_t wire = data_len;
        if (dir_in) {                 // IN wire length must be MPS-multiple
            uint32_t mps = s_msc_prof.mps_in ? s_msc_prof.mps_in : 64;
            wire = ((wire + mps - 1) / mps) * mps;
        }
        s_mapi->pipe_set_ep(s_pipe_data, dir_in ? s_msc_prof.ep_in : s_msc_prof.ep_out);
        UsbXferResult res = s_mapi->pipe_xfer(s_pipe_data, wire, MSC_IO_TIMEOUT_MS);
        if (res == USB_XFER_OK) {
            in_got = s_mapi->pipe_actual(s_pipe_data);
        } else if (res == USB_XFER_STALL) {
            // Device flags the error in the data phase; clear and read the
            // CSW, which carries the real status.
            msc_clear_stall(s_pipe_data);
            data_ok = false;
        } else {
            msc_bot_reset();
            return false;
        }
    }

    // CSW (13 bytes; submit MPS-sized, completes short)
    uint32_t csw_wire = s_msc_prof.mps_in ? s_msc_prof.mps_in : 64;
    s_mapi->pipe_set_ep(s_pipe_cmd, s_msc_prof.ep_in);
    UsbXferResult res = s_mapi->pipe_xfer(s_pipe_cmd, csw_wire, MSC_IO_TIMEOUT_MS);
    if (res == USB_XFER_STALL) {                  // one retry per BOT spec
        msc_clear_stall(s_pipe_cmd);
        res = s_mapi->pipe_xfer(s_pipe_cmd, csw_wire, MSC_IO_TIMEOUT_MS);
    }
    uint8_t* c = s_mapi->pipe_buf(s_pipe_cmd);
    uint32_t rtag = (uint32_t)c[4] | ((uint32_t)c[5]<<8)
                  | ((uint32_t)c[6]<<16) | ((uint32_t)c[7]<<24);
    if (res != USB_XFER_OK ||
        s_mapi->pipe_actual(s_pipe_cmd) < 13 ||
        c[0]!='U' || c[1]!='S' || c[2]!='B' || c[3]!='S' || rtag != tag) {
        msc_bot_reset();
        return false;
    }
    if (c[12] == 2) { msc_bot_reset(); return false; }   // phase error
    if (data_in_len) *data_in_len = in_got;
    return data_ok && c[12] == 0;                        // 0 = GOOD, 1 = failed
}

// REQUEST SENSE: fetch + log why the last command failed (clears the unit's
// pending sense). Caller holds the same context as the failed command.
static void scsi_request_sense() {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint32_t got = 0;
    if (msc_scsi(cdb, 6, true, 18, &got) && got >= 14) {
        uint8_t* d = s_mapi->pipe_buf(s_pipe_data);
        ulog("msc: sense key %X asc %02X/%02X", d[2] & 0x0F, d[12], d[13]);
    }
}

// ── Public sector API (any task; see usb_manager.h) ─────────────────────────

bool     usb_msc_ready()        { return s_msc_on; }
uint32_t usb_msc_sector_count() { return s_msc_sectors; }
uint32_t usb_msc_sector_size()  { return s_msc_ssize; }

bool usb_msc_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (!s_msc_on || !s_msc_ssize) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint32_t per = MSC_CHUNK_BYTES / s_msc_ssize;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > per ? per : count;
        uint32_t len = n * s_msc_ssize;
        uint8_t cdb[10] = { 0x28, 0,
            (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8), (uint8_t)lba,
            0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        uint32_t got = 0;
        ok = s_msc_on && msc_scsi(cdb, 10, true, len, &got) && got >= len;
        if (ok) {
            memcpy(buf, s_mapi->pipe_buf(s_pipe_data), len);
            buf += len; lba += n; count -= n;
        } else if (s_msc_on) {
            scsi_request_sense();
        }
    }
    xSemaphoreGive(s_msc_mutex);
    return ok;
}

bool usb_msc_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (!s_msc_on || !s_msc_ssize) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint32_t per = MSC_CHUNK_BYTES / s_msc_ssize;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > per ? per : count;
        uint32_t len = n * s_msc_ssize;
        if (!s_msc_on || !s_pipe_data) { ok = false; break; }
        memcpy(s_mapi->pipe_buf(s_pipe_data), buf, len);
        uint8_t cdb[10] = { 0x2A, 0,
            (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8), (uint8_t)lba,
            0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        ok = msc_scsi(cdb, 10, false, len, NULL);
        if (ok) { buf += len; lba += n; count -= n; }
        else if (s_msc_on) scsi_request_sense();
    }
    xSemaphoreGive(s_msc_mutex);
    return ok;
}

bool usb_msc_sync() {
    if (!s_msc_on) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    bool ok = msc_scsi(cdb, 10, false, 0, NULL);
    if (!ok && s_msc_on) scsi_request_sense();
    xSemaphoreGive(s_msc_mutex);
    return true;   // tolerated: many sticks stub SYNCHRONIZE CACHE
}

// Block-device socket ops (registered at start; usb_fs still calls the
// public sector API directly — the socket becomes load-bearing when
// storage drivers go dynamic).
static const UsbBlockOps s_block_ops = {
    &usb_msc_ready,
    &usb_msc_sector_count,
    &usb_msc_sector_size,
    &usb_msc_read,
    &usb_msc_write,
    &usb_msc_sync,
};

// ── Lifecycle hooks ──────────────────────────────────────────────────────────

static void msc_close_pipes(const UsbHostApi* api) {
    if (s_pipe_data) { api->pipe_close(s_pipe_data); s_pipe_data = NULL; }
    if (s_pipe_cmd)  { api->pipe_close(s_pipe_cmd);  s_pipe_cmd  = NULL; }
}

static bool msc_probe(const UsbHostApi* api) {
    s_mapi = api;
    if (!s_msc_mutex) s_msc_mutex = xSemaphoreCreateMutex();  // priority inheritance
    memset(&s_msc_prof, 0, sizeof(s_msc_prof));
    s_msc_on      = false;
    s_msc_sectors = 0;
    s_msc_ssize   = 0;
    msc_close_pipes(api);   // defensive (a crashed session's leftovers)

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg || !s_msc_mutex) return false;

    bool cur_msc = false;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            // MSC thumb drive (subclass 6 = SCSI transparent, proto 0x50 =
            // BOT). First matching interface wins; ignore any later one.
            cur_msc = (i->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                       i->bInterfaceSubClass == 0x06 &&
                       i->bInterfaceProtocol == 0x50 &&
                       i->bAlternateSetting == 0 &&
                       !s_msc_prof.valid);
            if (cur_msc) s_msc_prof.ifnum = i->bInterfaceNumber;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            bool is_bulk = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_BULK;
            bool is_out  = USB_EP_DESC_GET_EP_DIR(e) == 0;
            if (is_bulk && cur_msc) {
                if (is_out && !s_msc_prof.ep_out) {
                    s_msc_prof.ep_out  = e->bEndpointAddress;
                    s_msc_prof.mps_out = USB_EP_DESC_GET_MPS(e);
                } else if (!is_out && !s_msc_prof.ep_in) {
                    s_msc_prof.ep_in  = e->bEndpointAddress;
                    s_msc_prof.mps_in = USB_EP_DESC_GET_MPS(e);
                }
                if (s_msc_prof.ep_in && s_msc_prof.ep_out && !s_msc_prof.valid) {
                    s_msc_prof.valid = true;
                    ulog("  msc: IF %u in %02X/%u out %02X/%u",
                         s_msc_prof.ifnum, s_msc_prof.ep_in, s_msc_prof.mps_in,
                         s_msc_prof.ep_out, s_msc_prof.mps_out);
                }
            }
        }
    }
    return s_msc_prof.valid;
}

static bool msc_want(void) { return s_msc_prof.valid; }

static void msc_abort_start(const UsbHostApi* api) {
    msc_close_pipes(api);
    api->release_interface(s_msc_prof.ifnum);
    s_msc_prof.valid = false;      // don't retry-spin
}

static bool msc_start(const UsbHostApi* api) {
    if (s_msc_on) return true;
    if (!api->claim_interface(s_msc_prof.ifnum, 0)) {
        s_msc_prof.valid = false;
        return false;
    }
    s_pipe_data = api->pipe_open(s_msc_prof.ep_in, s_msc_prof.mps_in, MSC_CHUNK_BYTES);
    s_pipe_cmd  = api->pipe_open(s_msc_prof.ep_out, s_msc_prof.mps_out, 64);
    if (!s_pipe_data || !s_pipe_cmd) {
        ulog("msc pipe alloc failed (%dK internal)", MSC_CHUNK_BYTES / 1024);
        msc_abort_start(api);
        return false;
    }

    // Get Max LUN — informational, we drive LUN 0 only; STALL means "1 LUN".
    uint8_t maxlun = 0;
    if (api->control(0xA1, 0xFE, 0, s_msc_prof.ifnum, &maxlun, 1) && maxlun > 0)
        ulog("msc: %u LUNs (using 0)", maxlun + 1);

    // INQUIRY (informational: vendor/product)
    {
        uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
        uint32_t got = 0;
        if (msc_scsi(cdb, 6, true, 36, &got) && got >= 32) {
            char v[9] = {0}, p[17] = {0};
            memcpy(v, api->pipe_buf(s_pipe_data) + 8, 8);
            memcpy(p, api->pipe_buf(s_pipe_data) + 16, 16);
            ulog("msc: %s %s", v, p);
        }
    }

    // TEST UNIT READY — sticks spin up / raise a unit-attention that a
    // REQUEST SENSE clears; retry with backoff.
    bool ready = false;
    for (int i = 0; i < 10 && !ready; i++) {
        uint8_t cdb[6] = { 0, 0, 0, 0, 0, 0 };
        ready = msc_scsi(cdb, 6, false, 0, NULL);
        if (!ready) { scsi_request_sense(); api->delay_ms(500); }
    }
    if (!ready) { ulog("msc: unit never became ready"); msc_abort_start(api); return false; }

    // READ CAPACITY(10)
    {
        uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        uint32_t got = 0;
        if (!msc_scsi(cdb, 10, true, 8, &got) || got < 8) {
            ulog("msc: read capacity failed"); msc_abort_start(api); return false;
        }
        const uint8_t* d = api->pipe_buf(s_pipe_data);
        uint32_t last = ((uint32_t)d[0]<<24) | ((uint32_t)d[1]<<16)
                      | ((uint32_t)d[2]<<8)  | d[3];
        uint32_t ss   = ((uint32_t)d[4]<<24) | ((uint32_t)d[5]<<16)
                      | ((uint32_t)d[6]<<8)  | d[7];
        if (last == 0xFFFFFFFF) { ulog("msc: >2TB unsupported"); msc_abort_start(api); return false; }
        if (ss < 512 || ss > 4096) {
            ulog("msc: sector size %u unsupported", (unsigned)ss);
            msc_abort_start(api); return false;
        }
        s_msc_sectors = last + 1;
        s_msc_ssize   = ss;
    }

    s_msc_on = true;
    api->block_register(&s_block_ops);
    ulog(">>> Storage ready: %u MB (%uB sectors)",
         (unsigned)(((uint64_t)s_msc_sectors * s_msc_ssize) / (1024 * 1024)),
         (unsigned)s_msc_ssize);

    // FatFs mount (usb_fs.cpp) — its sector reads land back in usb_msc_read
    // on THIS task and self-pump through the pipe layer. Failure (exFAT/
    // unformatted) is logged there; the device stays listed either way.
    usb_fs_mount();
    return true;
}

static void msc_stop(const UsbHostApi* api, bool dev_present) {
    (void)dev_present;
    if (!s_pipe_cmd && !s_pipe_data && !s_msc_on) return;
    s_msc_on = false;                 // new transactions refuse from here on
    // Force any in-flight phase to complete so its owner fails fast.
    if (s_pipe_cmd)  api->pipe_cancel(s_pipe_cmd);
    if (s_pipe_data) api->pipe_cancel(s_pipe_data);
    // Acquire the transaction mutex while PUMPING (the owner's completion
    // and its failure-path control transfers need usb_task's event handling
    // to keep flowing). Hold it across the teardown so no late caller
    // touches closed pipes.
    bool have_mutex = false;
    for (int i = 0; i < 600 && !have_mutex; i++) {
        have_mutex = (xSemaphoreTake(s_msc_mutex, 0) == pdTRUE);
        if (!have_mutex) api->pump(10);
    }
    usb_fs_unmount();
    api->block_unregister();
    if (have_mutex) {
        msc_close_pipes(api);
    } else {
        // Pathological: a transaction owner is still wedged after ~6s of
        // pumping. Leak the pipes (16KB internal) rather than free them
        // under a task that may still dereference them.
        ulog("msc: teardown timeout — leaking pipes");
        s_pipe_cmd  = NULL;
        s_pipe_data = NULL;
    }
    api->release_interface(s_msc_prof.ifnum);
    s_msc_sectors = 0;
    s_msc_ssize   = 0;
    if (have_mutex) xSemaphoreGive(s_msc_mutex);
    ulog("Storage unmounted.");
}

static void msc_status(char* out, uint32_t n) {
    snprintf(out, n, "%uMB",
             (unsigned)(((uint64_t)s_msc_sectors * s_msc_ssize) / (1024 * 1024)));
}

static const UsbDriverDesc s_msc_desc = {
    USB_DRIVER_ABI_VERSION,
    "msc",
    &msc_probe,
    &msc_want,
    &msc_start,
    &msc_stop,
    NULL,           // busy \ FLASH-GUARD EXEMPT: bulk has no ISO deadline;
    NULL,           // park  ) in-flight completions latch across the stall
    NULL,           // resume/ (see the header comment)
    NULL,           // tick
    &msc_status,
};

const UsbDriverDesc* usbmsc_desc(void) { return &s_msc_desc; }

// ── Core-internal hook (Lua bridge msc_mb field) ────────────────────────────

double usbmsc_capacity_mb(void) {
    if (!s_msc_on) return 0;
    return ((double)s_msc_sectors * s_msc_ssize) / (1024.0 * 1024.0);
}
