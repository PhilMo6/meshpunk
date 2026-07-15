// Dynamic USB driver: HID boot-protocol MOUSE (match 03/01/02).
//
// The first genuinely out-of-tree Meshpunk driver — no firmware counterpart.
// Boot report: [0]=buttons (bit0 L, bit1 R), [1]=dx, [2]=dy (both int8),
// [3]=wheel (ignored). Routing:
//   - UI (no module running): deltas accumulate into focus-nav steps through
//     input_nav (one step per NAV_STEP counts — a mouse is a coarse focus
//     mover, not a cursor, on this UI); left click = input_nav click
//     (→ LV_KEY_ENTER via the trackball channel).
//   - Module running: RAW deltas each report through input_nav — PC-XT's
//     serial-mouse raw mode consumes the trackball counters as deltas, so a
//     USB mouse drives DOS directly. Buttons ride the key queue as the
//     established pseudo-codes: left 0x85, right 0x95 (press/release edges).
//     (Caveat: modules NOT in raw-trackball mode see accumulated counters as
//     nav pseudo-keys; that's the pre-existing meaning of those counters.)
//
// Flash-guard: interrupt-IN loop → busy/park/resume, same discipline as the
// keyboard driver. Stop/unplug releases held buttons (no stuck edges).

#include <string.h>
#include <stdio.h>
#include "usb_shim.h"
#include "../../src/usb/usb_driver_abi.h"

#define ulog(...) do { if (s_hapi) s_hapi->log(__VA_ARGS__); } while (0)

#define NAV_STEP 24    // delta counts per UI focus step

struct MouseProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep;
    uint16_t mps;
};
static MouseProfile s_prof;

static const UsbHostApi* s_hapi  = 0;
static UsbPipe*          s_pipe  = 0;
static volatile bool     s_on    = false;
static uint8_t           s_btn_prev = 0;   // held buttons (for edges + sweep)
static int               s_acc_x = 0, s_acc_y = 0;   // UI step accumulators

// ── Report handling ──────────────────────────────────────────────────────────

static void mouse_report(const uint8_t* r, uint32_t len) {
    if (len < 3) return;
    uint8_t btn = r[0];
    int8_t  dx  = (int8_t)r[1];
    int8_t  dy  = (int8_t)r[2];
    bool game = s_hapi->input_module_active();

    // Buttons: edges. Left doubles as UI click (Enter) when no module runs.
    uint8_t changed = btn ^ s_btn_prev;
    if (changed & 0x01) {
        if (game) s_hapi->input_key(0x85, (btn & 0x01) != 0);
        else if (btn & 0x01) s_hapi->input_nav(0, 0, 0, 0, 1);
    }
    if (changed & 0x02) {
        if (game) s_hapi->input_key(0x95, (btn & 0x02) != 0);
    }
    s_btn_prev = btn;

    if (dx == 0 && dy == 0) return;
    if (game) {
        // Raw deltas: PC-XT's raw-trackball mode reads the counters as dx/dy.
        s_hapi->input_nav(dy < 0 ? -dy : 0, dy > 0 ? dy : 0,
                          dx < 0 ? -dx : 0, dx > 0 ? dx : 0, 0);
        s_acc_x = 0; s_acc_y = 0;
    } else {
        // Focus nav: one step per NAV_STEP counts.
        s_acc_x += dx;
        s_acc_y += dy;
        int up = 0, down = 0, left = 0, right = 0;
        while (s_acc_y <= -NAV_STEP) { up++;    s_acc_y += NAV_STEP; }
        while (s_acc_y >=  NAV_STEP) { down++;  s_acc_y -= NAV_STEP; }
        while (s_acc_x <= -NAV_STEP) { left++;  s_acc_x += NAV_STEP; }
        while (s_acc_x >=  NAV_STEP) { right++; s_acc_x -= NAV_STEP; }
        if (up | down | left | right)
            s_hapi->input_nav(up, down, left, right, 0);
    }
}

static void mouse_pipe_cb(UsbPipe* p, UsbXferResult res, uint32_t actual, void*) {
    if (res == USB_XFER_OK && actual >= 3)
        mouse_report(s_hapi->pipe_buf(p), actual);
    if (s_on && !s_hapi->flash_guard_pending()
             && res != USB_XFER_NO_DEVICE
             && res != USB_XFER_CANCELED) {
        if (!s_hapi->pipe_submit(p, s_prof.mps, mouse_pipe_cb, 0))
            ulog("mouse: resubmit failed");
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

static bool mouse_probe(const UsbHostApi* api) {
    s_hapi = api;
    memset(&s_prof, 0, sizeof(s_prof));
    s_on = false;
    s_btn_prev = 0;
    s_acc_x = s_acc_y = 0;
    if (s_pipe) { api->pipe_close(s_pipe); s_pipe = 0; }

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool cur_mouse = false;
    uint8_t cur_if = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != 0) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            cur_mouse = (i->bInterfaceClass == USB_CLASS_HID &&
                         i->bInterfaceSubClass == 1 &&
                         i->bInterfaceProtocol == 2 &&        // boot MOUSE
                         i->bAlternateSetting == 0);
            cur_if = i->bInterfaceNumber;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            if (cur_mouse && !s_prof.valid &&
                USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR &&
                USB_EP_DESC_GET_EP_DIR(e) != 0 &&
                USB_EP_DESC_GET_MPS(e) >= 3) {                // boot report = 3B+
                s_prof.valid = true;
                s_prof.ifnum = cur_if;
                s_prof.ep    = e->bEndpointAddress;
                s_prof.mps   = USB_EP_DESC_GET_MPS(e);
                ulog("  boot mouse: IF %u ep %02X mps %u",
                     cur_if, s_prof.ep, s_prof.mps);
            }
        }
    }
    return s_prof.valid;
}

static bool mouse_want(void) { return s_prof.valid; }

static bool mouse_start(const UsbHostApi* api) {
    if (s_on) return true;
    if (!api->claim_interface(s_prof.ifnum, 0)) {
        s_prof.valid = false;
        return false;
    }
    if (!api->control(0x21, 0x0B /*SET_PROTOCOL*/, 0 /*boot*/, s_prof.ifnum, 0, 0))
        ulog("mouse SET_PROTOCOL failed (continuing)");

    s_pipe = api->pipe_open(s_prof.ep, s_prof.mps, s_prof.mps);
    if (!s_pipe) {
        ulog("mouse pipe alloc failed");
        api->release_interface(s_prof.ifnum);
        s_prof.valid = false;
        return false;
    }
    s_on = true;
    if (!api->pipe_submit(s_pipe, s_prof.mps, mouse_pipe_cb, 0)) {
        ulog("mouse submit failed");
        s_on = false;
        api->pipe_close(s_pipe); s_pipe = 0;
        api->release_interface(s_prof.ifnum);
        s_prof.valid = false;
        return false;
    }
    ulog(">>> Mouse ready.");
    return true;
}

static void mouse_stop(const UsbHostApi* api, bool) {
    s_on = false;
    if (s_pipe) { api->pipe_close(s_pipe); s_pipe = 0; }
    api->release_interface(s_prof.ifnum);
    // Release held buttons — no stuck edges in a running module.
    if (s_hapi->input_module_active()) {
        if (s_btn_prev & 0x01) s_hapi->input_key(0x85, false);
        if (s_btn_prev & 0x02) s_hapi->input_key(0x95, false);
    }
    s_btn_prev = 0;
    ulog("Mouse stopped.");
}

static int mouse_busy(void) {
    return (s_pipe && s_hapi) ? s_hapi->pipe_in_flight(s_pipe) : 0;
}

static void mouse_park(void) {
    if (s_pipe && s_hapi) s_hapi->pipe_cancel(s_pipe);
}

static void mouse_resume(void) {
    if (!s_pipe || !s_hapi) return;
    if (s_hapi->pipe_in_flight(s_pipe)) return;
    s_hapi->pipe_reset(s_pipe);
    if (!s_hapi->pipe_submit(s_pipe, s_prof.mps, mouse_pipe_cb, 0))
        ulog("mouse: resume resubmit failed");
}

static void mouse_status(char* out, uint32_t n) {
    snprintf(out, n, "mouse");
}

extern "C" const UsbDriverDesc usbdrv_ops = {
    USB_DRIVER_ABI_VERSION,
    "mouse",
    &mouse_probe,
    &mouse_want,
    &mouse_start,
    &mouse_stop,
    &mouse_busy,
    &mouse_park,
    &mouse_resume,
    0,              // tick
    &mouse_status,
};
