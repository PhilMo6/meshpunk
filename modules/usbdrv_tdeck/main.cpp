// Dynamic USB driver: TDECK peer link (match 0A/*/*, Espressif VID only).
// The ESP32-S3 USB-Serial-JTAG presents its CDC-data interface as 0A/02
// (non-standard subclass), so the manifest wildcards subclass/protocol; the
// probe's idVendor==0x303A gate is what actually keeps us to real T-Decks.
//
// Connects two T-Decks over one USB cable: this side runs USB host mode and
// enumerates the OTHER T-Deck's native USB-Serial-JTAG CDC device port. The
// driver is a thin transport — it claims the CDC interfaces, runs a bulk-IN
// resubmit loop feeding every received byte to the firmware's peer-link
// bridge (api->link_rx), and registers the link socket whose send() carries
// the bridge's frames out over bulk OUT. All protocol (framing, sessions,
// services — GameBoy link etc.) lives in src/tdeck_link.cpp.
//
// The probe requires idVendor == 0x303A (Espressif) so random CDC gadgets
// are left alone. Flash guard: the bulk-IN resubmit loop is periodic, so
// busy/park/resume are provided (kbd discipline).

#include <string.h>
#include <stdio.h>
#include "usb_shim.h"
#include "../../src/usb/usb_driver_abi.h"

extern "C" const UsbDriverDesc usbdrv_ops;

#define ulog(...) do { if (s_hapi) s_hapi->log(__VA_ARGS__); } while (0)

#define VID_ESPRESSIF 0x303A

struct LinkProfile {
    bool     valid;
    uint8_t  data_if;      // CDC-data interface (bulk pipes)
    uint8_t  comm_if;      // CDC-comm interface (line-state control target)
    bool     have_comm;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
};
static LinkProfile s_prof;

static const UsbHostApi* s_hapi     = 0;
static UsbPipe*          s_pipe_in  = 0;
static UsbPipe*          s_pipe_out = 0;
static volatile bool     s_on       = false;
static volatile bool     s_linked   = false;   // socket registered

// ── Link socket: bridge -> wire ──────────────────────────────────────────────
// CONTRACT: the bridge (src/tdeck_link.cpp) SERIALIZES every call to this
// send() under its own mutex — REQUIRED, not optional. There is ONE shared
// pipe buffer + ONE transfer object here; two unserialized callers (the ELF
// module task and tdl_task on different cores) once memcpy'd the same buffer
// and double-submitted the same usb_transfer_t, corrupting the host stack's
// transfer list and hard-freezing the deck (hw runs 6-8). Do not call this
// from anywhere that isn't the bridge's serialized send path. pipe_xfer is
// dual-context (blocks a foreign task on a waiter; pumps inline on usb_task).

static bool link_send(const uint8_t* d, uint32_t n) {
    if (!s_on || !s_pipe_out || !s_hapi || n == 0) return false;
    if (n > s_prof.mps_out) return false;        // frames are tiny (< 24B)
    memcpy(s_hapi->pipe_buf(s_pipe_out), d, n);
    return s_hapi->pipe_xfer(s_pipe_out, n, 250) == USB_XFER_OK;
}

static const UsbLinkOps s_link_ops = { &link_send };

// ── Bulk-IN resubmit loop: wire -> bridge ────────────────────────────────────

static void in_pipe_cb(UsbPipe* p, UsbXferResult res, uint32_t actual, void*) {
    if (res == USB_XFER_OK && actual > 0)
        s_hapi->link_rx(s_hapi->pipe_buf(p), actual);
    if (s_on && !s_hapi->flash_guard_pending()
             && res != USB_XFER_NO_DEVICE
             && res != USB_XFER_CANCELED) {
        if (!s_hapi->pipe_submit(p, s_prof.mps_in, in_pipe_cb, 0))
            ulog("tdeck: resubmit failed");
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

static bool tdl_probe(const UsbHostApi* api) {
    s_hapi = api;
    memset(&s_prof, 0, sizeof(s_prof));
    s_on = false;
    if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
    if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }

    // Only pair with another Espressif device (the other T-Deck's
    // USB-Serial-JTAG CDC) — leave real CDC gadgets to their own drivers.
    const usb_device_desc_t* dd = (const usb_device_desc_t*)api->device_desc();
    if (!dd || dd->idVendor != VID_ESPRESSIF) return false;

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool    cur_data = false;
    uint8_t cur_if   = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != 0) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            cur_if   = i->bInterfaceNumber;
            cur_data = false;
            if (i->bAlternateSetting != 0) continue;
            if (i->bInterfaceClass == 0x02) {          // CDC comm
                if (!s_prof.have_comm) {
                    s_prof.comm_if   = cur_if;
                    s_prof.have_comm = true;
                }
            } else if (i->bInterfaceClass == 0x0A) {   // CDC data
                cur_data = true;
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            if (!cur_data || s_prof.valid) continue;
            if ((e->bmAttributes & 0x03) != 2 /*bulk*/) continue;
            uint16_t mps = USB_EP_DESC_GET_MPS(e);
            if (mps > 64) mps = 64;
            if (USB_EP_DESC_GET_EP_DIR(e) != 0) {
                s_prof.ep_in  = e->bEndpointAddress;
                s_prof.mps_in = mps;
            } else {
                s_prof.ep_out  = e->bEndpointAddress;
                s_prof.mps_out = mps;
            }
            if (s_prof.ep_in && s_prof.ep_out) {
                s_prof.data_if = cur_if;
                s_prof.valid   = true;
            }
        }
    }
    if (!s_prof.valid) return false;

    ulog("  tdeck: peer T-Deck IF %u ep in %02X out %02X (comm IF %u)",
         s_prof.data_if, s_prof.ep_in, s_prof.ep_out,
         s_prof.have_comm ? s_prof.comm_if : 0xFF);
    return true;
}

static bool tdl_want(void) { return s_prof.valid; }

static bool tdl_start(const UsbHostApi* api) {
    if (s_on) return true;
    if (!api->claim_interface(s_prof.data_if, 0)) {
        s_prof.valid = false;
        return false;
    }
    if (s_prof.have_comm) {
        api->claim_interface(s_prof.comm_if, 0);   // tolerated-fail
        // CDC SET_CONTROL_LINE_STATE: DTR|RTS — opens the port so the peer's
        // HWCDC considers itself connected and lets its TX flow.
        if (!api->control(0x21, 0x22, 0x0003, s_prof.comm_if, 0, 0))
            ulog("tdeck: line-state failed (continuing)");
    }

    s_pipe_in  = api->pipe_open(s_prof.ep_in,  s_prof.mps_in,  s_prof.mps_in);
    s_pipe_out = api->pipe_open(s_prof.ep_out, s_prof.mps_out, s_prof.mps_out);
    if (!s_pipe_in || !s_pipe_out) {
        ulog("tdeck: pipe alloc failed");
        if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
        if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }
        api->release_interface(s_prof.data_if);
        if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
        s_prof.valid = false;
        return false;
    }
    s_on = true;
    if (!api->pipe_submit(s_pipe_in, s_prof.mps_in, in_pipe_cb, 0)) {
        ulog("tdeck: submit failed");
        s_on = false;
        api->pipe_close(s_pipe_in);  s_pipe_in = 0;
        api->pipe_close(s_pipe_out); s_pipe_out = 0;
        api->release_interface(s_prof.data_if);
        if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
        s_prof.valid = false;
        return false;
    }

    s_linked = api->link_register(&s_link_ops);
    if (!s_linked) ulog("tdeck: link socket busy — transport idle");
    ulog(">>> T-Deck peer link up.");
    return true;
}

static void tdl_stop(const UsbHostApi* api, bool) {
    if (s_linked) { api->link_unregister(); s_linked = false; }
    s_on = false;
    if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
    if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }
    api->release_interface(s_prof.data_if);
    if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
    ulog("T-Deck peer link stopped.");
}

static int tdl_busy(void) {
    return (s_pipe_in && s_hapi) ? s_hapi->pipe_in_flight(s_pipe_in) : 0;
}

static void tdl_park(void) {
    if (s_pipe_in && s_hapi) s_hapi->pipe_cancel(s_pipe_in);
}

static void tdl_resume(void) {
    if (!s_pipe_in || !s_hapi) return;
    if (s_hapi->pipe_in_flight(s_pipe_in)) return;
    s_hapi->pipe_reset(s_pipe_in);
    if (!s_hapi->pipe_submit(s_pipe_in, s_prof.mps_in, in_pipe_cb, 0))
        ulog("tdeck: resume resubmit failed");
}

static void tdl_status(char* out, uint32_t n) {
    snprintf(out, n, s_linked ? "peer link" : "idle");
}

extern "C" const UsbDriverDesc usbdrv_ops = {
    USB_DRIVER_ABI_VERSION,
    "tdeck",
    &tdl_probe,
    &tdl_want,
    &tdl_start,
    &tdl_stop,
    &tdl_busy,
    &tdl_park,
    &tdl_resume,
    0,              // tick: none
    &tdl_status,
};
