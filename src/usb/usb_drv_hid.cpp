// USB class driver: HID boot-protocol keyboard — VTABLE-CLEAN.
//
// Everything goes through the UsbHostApi: control requests (SET_PROTOCOL /
// SET_IDLE / LED SET_REPORT / CLEAR_FEATURE via pipe_reset), the interrupt-IN
// report loop (async pipe with resubmit-in-callback), and the three input
// sockets. This file is also the dogfood source for the dynamic-module
// build (modules/usbdrv_kbd).
//
// INPUT CONTRACT (load-bearing): input_key carries RAW host pseudo-codes,
// pre-keymap — including modifier edges 0x80/0x8B/0x8C/0x8D. 0x8C (Alt) is
// consumed by elf_host's Alt+Backspace module-exit chord; stop()/unplug must
// sweep release edges for held keys AND modifiers or a stuck flag degrades
// the exit chord for the rest of the run.

// DUAL-BUILD: this file compiles (a) into firmware as the built-in driver
// and (b) unchanged as the dogfood dynamic module (modules/usbdrv_kbd/,
// -DUSB_DRV_MODULE). The module build has no Arduino/IDF headers:
// descriptor structs come from the local usb_shim.h and logging goes
// through the vtable.
#ifdef USB_DRV_MODULE
  #include <string.h>
  #include <stdio.h>
  #include "usb_shim.h"                       // packed descriptor structs + walker
  #include "../../src/usb/usb_driver_abi.h"
  #define ulog(...) do { if (s_hapi) s_hapi->log(__VA_ARGS__); } while (0)
#else
  #include "../usb_manager.h"
  #include "usb_core_int.h"
  #include <Arduino.h>
  #define ulog usbcore_log
#endif

// ── Profile + state ──────────────────────────────────────────────────────────

struct HidKbdProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep;          // interrupt-IN endpoint address (0x8x)
    uint16_t mps;
    uint8_t  binterval;
};
static HidKbdProfile s_kbd_prof;

static const UsbHostApi* s_hapi  = NULL;   // captured at probe (session-stable)
static UsbPipe*          s_pipe  = NULL;   // interrupt-IN report pipe
static volatile bool     s_hid_on = false; // interface claimed + pipe armed
static uint8_t           s_hid_prev[8];    // last boot report (edge diffs)

// UI arrow-nav repeat: held USB arrows re-fire the trackball nav counters
// through the input_nav socket (rate-limited by keyboard_read_cb).
static uint8_t  s_arrow_state = 0;          // bit0..3 = up/down/left/right held
static uint32_t s_arrow_t0 = 0, s_arrow_last = 0;
#define HID_ARROW_DELAY_MS  400
#define HID_ARROW_REPEAT_MS 150

// Lock-key state — HOST-tracked (the keyboard only reports the keypress; the
// host owns the state and drives the LEDs). NumLock defaults ON at connect
// (keypad types digits; OFF = nav cluster, UI only). The LED report is sent
// from tick(), never from the report callback (control transfers must not
// run inside a transfer completion).
static bool          s_caps_lock   = false;
static bool          s_num_lock    = true;
static volatile bool s_led_pending = false;

// HID usage → ASCII, US QWERTY boot layout (usages 0x04-0x38). Enter/Backspace
// use the matrix pipeline's 0x0D/0x08 codes; Esc 0x1B, Tab 0x09. Usage 0x32
// (non-US #~) unmapped. F-keys and arrows are handled outside the tables.
static const uint8_t kUsage2Char[0x39] = {
    0, 0, 0, 0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',        // 0x04-0x10
    'n','o','p','q','r','s','t','u','v','w','x','y','z',        // 0x11-0x1D
    '1','2','3','4','5','6','7','8','9','0',                    // 0x1E-0x27
    0x0D, 0x1B, 0x08, 0x09, ' ',                                // 0x28-0x2C
    '-','=','[',']','\\', 0, ';','\'','`',',','.','/',          // 0x2D-0x38
};
static const uint8_t kUsage2CharShift[0x39] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0x0D, 0x1B, 0x08, 0x09, ' ',
    '_','+','{','}','|', 0, ':','"','~','<','>','?',
};

// ── Usage translation ────────────────────────────────────────────────────────
// usage_to_game: unshifted base chars (modules get separate modifier
// pseudo-key edges, matching the matrix convention). PURE function of the
// usage — deliberately ignores NumLock/Caps so a release always emits the
// same code as its press (no stuck keys if a lock toggles mid-hold); keypad
// is always digits for modules. 0 = not mapped.
//
// Pseudo-code allocation (kq/keymap space; 0x80-0x85 + 0xB0-0xB9 pre-date USB):
//   0x80 shift  0x81-0x84 up/down/left/right  0x85 click (trackball)
//   0x86 Home   0x87 End   0x88 PgUp   0x89 PgDn   0x8A Insert
//   0x8B Ctrl   0x8C Alt   0x8D GUI    0x8E CapsLock   0x8F NumLock
//   0x90-0x9F RESERVED — PC-XT launcher binding outputs live at 0x91-0x99
//   0xA0 PrtSc  0xA1 ScrLk 0xA2 Pause  0xA3 Menu
//   0xB0-0xB9 F1-F10   0xBA F11   0xBB F12   0x7F Delete (ASCII DEL)
static uint8_t usage_to_game(uint8_t u) {
    if (u < 0x39) return kUsage2Char[u];
    if (u >= 0x3A && u <= 0x43) return (uint8_t)(0xB0 + (u - 0x3A));  // F1-F10
    if (u >= 0x59 && u <= 0x61) return (uint8_t)('1' + (u - 0x59));   // KP1-KP9
    switch (u) {
        case 0x39: return 0x8E;   // CapsLock (state/LED handled host-side too)
        case 0x44: return 0xBA;   // F11
        case 0x45: return 0xBB;   // F12
        case 0x46: return 0xA0;   // PrintScreen
        case 0x47: return 0xA1;   // ScrollLock
        case 0x48: return 0xA2;   // Pause
        case 0x49: return 0x8A;   // Insert
        case 0x4A: return 0x86;   // Home
        case 0x4B: return 0x88;   // PgUp
        case 0x4C: return 0x7F;   // Delete
        case 0x4D: return 0x87;   // End
        case 0x4E: return 0x89;   // PgDn
        case 0x4F: return 0x84;   // right
        case 0x50: return 0x83;   // left
        case 0x51: return 0x82;   // down
        case 0x52: return 0x81;   // up
        case 0x53: return 0x8F;   // NumLock
        case 0x54: return '/';    // keypad ─ always the digit/operator layer
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return 0x0D;   // KP Enter
        case 0x62: return '0';
        case 0x63: return '.';
        case 0x65: return 0xA3;   // Menu
    }
    return 0;
}

// usage_to_ui: chars for the 128-slot UI held image. Shift picks the shifted
// table; Caps flips letter case only (shift XOR caps); NumLock picks the
// keypad layer. Home/End/Delete land on LV_KEY_HOME/END/DEL (2/3/0x7F).
// Arrows are NOT chars — they ride the nav socket.
static uint8_t usage_to_ui(uint8_t u, bool shift) {
    if (u >= 0x04 && u <= 0x1D)                           // letters
        return (shift ^ s_caps_lock) ? kUsage2CharShift[u] : kUsage2Char[u];
    if (u < 0x39) return shift ? kUsage2CharShift[u] : kUsage2Char[u];
    switch (u) {
        case 0x4A: return 2;      // Home   -> LV_KEY_HOME
        case 0x4C: return 0x7F;   // Delete -> LV_KEY_DEL (textarea fwd-delete)
        case 0x4D: return 3;      // End    -> LV_KEY_END
        case 0x58: return 0x0D;   // KP Enter
    }
    if (s_num_lock) {                                     // keypad: digits
        if (u >= 0x59 && u <= 0x61) return (uint8_t)('1' + (u - 0x59));
        switch (u) {
            case 0x54: return '/';
            case 0x55: return '*';
            case 0x56: return '-';
            case 0x57: return '+';
            case 0x62: return '0';
            case 0x63: return '.';
        }
    } else {                                              // keypad: nav layer
        switch (u) {
            case 0x5F: return 2;      // KP7 Home
            case 0x59: return 3;      // KP1 End
            case 0x63: return 0x7F;   // KP. Delete
        }
    }
    return 0;
}

// Modifier byte (report[0]) → module pseudo-key edges, L|R combined like the
// matrix's single shift. The UI consumes shift/caps only (via usage_to_ui).
static const struct { uint8_t mask; uint8_t code; } kModGame[] = {
    { 0x22, 0x80 },   // shift
    { 0x11, 0x8B },   // ctrl
    { 0x44, 0x8C },   // alt  — the module exit chord consumes this edge
    { 0x88, 0x8D },   // gui
};
#define MODGAME_N (sizeof(kModGame) / sizeof(kModGame[0]))

// ── Report parsing ───────────────────────────────────────────────────────────
// One 8-byte boot report: [0]=modifiers [1]=reserved [2..7]=usages. Reports
// are ABSOLUTE (all currently-held keys), so both consumers are rebuilt/
// diffed from scratch each time — nothing can stick.

static void hid_parse_report(const uint8_t* r) {
    // Phantom rollover (>6 keys): all usage slots 0x01 — keep previous state.
    bool phantom = true;
    for (int i = 2; i < 8; i++) if (r[i] != 0x01) { phantom = false; break; }
    if (phantom) return;

    bool game  = s_hapi->input_module_active();
    bool shift = (r[0] & 0x22) != 0;            // L/R shift modifier bits

    // Lock keys toggle host state on press edge; the LED report is deferred
    // to tick() (s_led_pending) — see the state block comment.
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u != 0x39 && u != 0x53) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) if (s_hid_prev[j] == u) { was = true; break; }
        if (!was) {
            if (u == 0x39) s_caps_lock = !s_caps_lock;
            else           s_num_lock  = !s_num_lock;
            s_led_pending = true;
        }
    }

    // UI image: shift/caps/numlock applied at parse time.
    bool held[128] = {};
    int  nheld = 0;
    for (int i = 2; i < 8; i++) {
        uint8_t ch = usage_to_ui(r[i], shift);
        if (ch && ch < 128 && !held[ch]) { held[ch] = true; nheld++; }
    }
    s_hapi->input_set_held(held, nheld);

    // Module edges: usage-set diff vs the previous report, unshifted codes.
    if (game) {
        for (int i = 2; i < 8; i++) {           // releases: in prev, gone now
            uint8_t u = s_hid_prev[i];
            if (!u) continue;
            bool still = false;
            for (int j = 2; j < 8; j++) if (r[j] == u) { still = true; break; }
            if (!still) { uint8_t c = usage_to_game(u); if (c) s_hapi->input_key(c, false); }
        }
        for (int i = 2; i < 8; i++) {           // presses: new this report
            uint8_t u = r[i];
            if (!u) continue;
            bool was = false;
            for (int j = 2; j < 8; j++) if (s_hid_prev[j] == u) { was = true; break; }
            if (!was) { uint8_t c = usage_to_game(u); if (c) s_hapi->input_key(c, true); }
        }
        for (size_t m = 0; m < MODGAME_N; m++) {   // modifier edges (shift/ctrl/alt/gui)
            bool now = (r[0] & kModGame[m].mask) != 0;
            bool was = (s_hid_prev[0] & kModGame[m].mask) != 0;
            if (now != was) s_hapi->input_key(kModGame[m].code, now);
        }
    }

    // UI arrow nav via the nav socket (rising edges; kbd_tick repeats).
    // Keypad 8/2/4/6 join in when NumLock is off. Suppressed in-game — there
    // arrows are 0x81-0x84 edges above, and counter increments would fight
    // poll_input's momentum integration.
    uint8_t arrows = 0;
    for (int i = 2; i < 8; i++) {
        switch (r[i]) {
            case 0x52: arrows |= 1; break;      // up
            case 0x51: arrows |= 2; break;      // down
            case 0x50: arrows |= 4; break;      // left
            case 0x4F: arrows |= 8; break;      // right
            case 0x60: if (!s_num_lock) arrows |= 1; break;   // KP8
            case 0x5A: if (!s_num_lock) arrows |= 2; break;   // KP2
            case 0x5C: if (!s_num_lock) arrows |= 4; break;   // KP4
            case 0x5E: if (!s_num_lock) arrows |= 8; break;   // KP6
        }
    }
    if (!game) {
        uint8_t newly = arrows & (uint8_t)~s_arrow_state;
        s_hapi->input_nav((newly & 1) ? 1 : 0, (newly & 2) ? 1 : 0,
                          (newly & 4) ? 1 : 0, (newly & 8) ? 1 : 0, 0);
        if (newly) { s_arrow_t0 = s_hapi->ticks_ms(); s_arrow_last = s_arrow_t0; }
        s_arrow_state = arrows;
    } else {
        s_arrow_state = 0;
    }

    memcpy(s_hid_prev, r, 8);
}

// Release everything (unplug/stop): clear the UI image and, mid-game, inject
// release edges for whatever the last report showed held — no stuck keys and
// no stuck exit-chord flags.
static void hid_all_keys_up() {
    static const bool none[128] = {};
    s_hapi->input_set_held(none, 0);
    if (s_hapi->input_module_active()) {
        for (int i = 2; i < 8; i++) {
            uint8_t c = usage_to_game(s_hid_prev[i]);
            if (c) s_hapi->input_key(c, false);
        }
        for (size_t m = 0; m < MODGAME_N; m++)
            if (s_hid_prev[0] & kModGame[m].mask)
                s_hapi->input_key(kModGame[m].code, false);
    }
    s_arrow_state = 0;
    memset(s_hid_prev, 0, sizeof(s_hid_prev));
}

// ── Report pipe (async interrupt IN, resubmit-in-callback) ──────────────────

static void kbd_pipe_cb(UsbPipe* p, UsbXferResult res, uint32_t actual, void*) {
    if (res == USB_XFER_OK && actual >= 8)
        hid_parse_report(s_hapi->pipe_buf(p));
    // Same drain discipline as the ISO stream: no resubmit while a flash
    // write is pending or once the device/driver is gone. busy() reads the
    // pipe's in-flight count, so declining here drains it to zero.
    if (s_hid_on && !s_hapi->flash_guard_pending()
                 && res != USB_XFER_NO_DEVICE
                 && res != USB_XFER_CANCELED) {
        if (!s_hapi->pipe_submit(p, s_kbd_prof.mps, kbd_pipe_cb, NULL))
            ulog("hid: resubmit failed");
    }
}

// ── Lifecycle hooks ──────────────────────────────────────────────────────────

static bool kbd_probe(const UsbHostApi* api) {
    s_hapi = api;
    memset(&s_kbd_prof, 0, sizeof(s_kbd_prof));
    s_hid_on = false;
    memset(s_hid_prev, 0, sizeof(s_hid_prev));
    s_arrow_state = 0;
    if (s_pipe) { api->pipe_close(s_pipe); s_pipe = NULL; }   // defensive

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool cur_kbd = false;
    uint8_t cur_if = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            // HID boot-protocol keyboard (subclass 1 = boot, protocol 1 = kbd).
            cur_kbd = (i->bInterfaceClass == USB_CLASS_HID &&
                       i->bInterfaceSubClass == 1 &&
                       i->bInterfaceProtocol == 1 &&
                       i->bAlternateSetting == 0);
            cur_if = i->bInterfaceNumber;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            bool is_int = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR;
            bool is_in  = USB_EP_DESC_GET_EP_DIR(e) != 0;
            if (is_int && is_in && cur_kbd && !s_kbd_prof.valid
                       && USB_EP_DESC_GET_MPS(e) >= 8) {   // boot report = 8B
                s_kbd_prof.valid     = true;
                s_kbd_prof.ifnum     = cur_if;
                s_kbd_prof.ep        = e->bEndpointAddress;
                s_kbd_prof.mps       = USB_EP_DESC_GET_MPS(e);
                s_kbd_prof.binterval = e->bInterval;
                ulog("  boot kbd: IF %u ep %02X mps %u intv %u",
                     cur_if, s_kbd_prof.ep, s_kbd_prof.mps, s_kbd_prof.binterval);
            }
        }
    }
    return s_kbd_prof.valid;
}

static bool kbd_want(void) { return s_kbd_prof.valid; }

static bool kbd_start(const UsbHostApi* api) {
    if (s_hid_on) return true;
    if (!api->claim_interface(s_kbd_prof.ifnum, 0)) {
        s_kbd_prof.valid = false;               // don't retry-spin
        return false;
    }
    // Boot protocol + report-on-change (idle 0). Both non-fatal on STALL:
    // reports are absolute snapshots, so periodic idle re-sends of an
    // unchanged report just produce zero diffs.
    if (!api->control(0x21, 0x0B /*SET_PROTOCOL*/, 0 /*boot*/, s_kbd_prof.ifnum, NULL, 0))
        ulog("SET_PROTOCOL boot failed (continuing)");
    if (!api->control(0x21, 0x0A /*SET_IDLE*/, 0, s_kbd_prof.ifnum, NULL, 0))
        ulog("SET_IDLE failed (continuing)");

    memset(s_hid_prev, 0, sizeof(s_hid_prev));
    s_pipe = api->pipe_open(s_kbd_prof.ep, s_kbd_prof.mps, s_kbd_prof.mps);
    if (!s_pipe) {
        ulog("kbd pipe alloc failed");
        api->release_interface(s_kbd_prof.ifnum);
        s_kbd_prof.valid = false;
        return false;
    }
    s_hid_on = true;   // before submit: the callback consults it
    if (!api->pipe_submit(s_pipe, s_kbd_prof.mps, kbd_pipe_cb, NULL)) {
        ulog("kbd transfer submit failed");
        s_hid_on = false;
        api->pipe_close(s_pipe); s_pipe = NULL;
        api->release_interface(s_kbd_prof.ifnum);
        s_kbd_prof.valid = false;
        return false;
    }
    // Fresh keyboard = fresh lock state; push the initial LEDs (NumLock on)
    // from tick() on its next pass.
    s_caps_lock = false;
    s_num_lock  = true;
    s_led_pending = true;
    ulog(">>> Keyboard ready.");
    return true;
}

static void kbd_stop(const UsbHostApi* api, bool dev_present) {
    (void)dev_present;
    s_hid_on = false;                 // callback declines resubmit from here
    if (s_pipe) {
        // pipe_close cancels a pending transfer (halt+flush — a quiet
        // keyboard's IN never completes on its own), drains it, and frees.
        api->pipe_close(s_pipe);
        s_pipe = NULL;
    }
    api->release_interface(s_kbd_prof.ifnum);
    hid_all_keys_up();
    ulog("Keyboard stopped.");
}

// ── Flash-guard hooks ────────────────────────────────────────────────────────
// An idle interrupt-IN never completes on its own — park forces a CANCELED
// completion (halt+flush) so busy() drains fast. Resume clears the halt on
// BOTH sides (pipe_reset = wire CLEAR_FEATURE + host clear, data toggles
// back in sync) and re-arms.

static int kbd_busy(void) {
    return (s_pipe && s_hapi) ? s_hapi->pipe_in_flight(s_pipe) : 0;
}

static void kbd_park(void) {
    if (s_pipe && s_hapi) s_hapi->pipe_cancel(s_pipe);
}

static void kbd_resume(void) {
    if (!s_pipe || !s_hapi) return;
    if (s_hapi->pipe_in_flight(s_pipe)) return;   // park drain timed out
    s_hapi->pipe_reset(s_pipe);
    if (!s_hapi->pipe_submit(s_pipe, s_kbd_prof.mps, kbd_pipe_cb, NULL))
        ulog("hid: resume resubmit failed");
}

// ── Per-loop tick: arrow repeat + deferred LED report ───────────────────────

static void kbd_tick(const UsbHostApi* api) {
    if (s_arrow_state && !api->input_module_active()) {
        uint32_t now = api->ticks_ms();
        if (now - s_arrow_t0 >= HID_ARROW_DELAY_MS &&
            now - s_arrow_last >= HID_ARROW_REPEAT_MS) {
            s_arrow_last = now;
            api->input_nav((s_arrow_state & 1) ? 1 : 0, (s_arrow_state & 2) ? 1 : 0,
                           (s_arrow_state & 4) ? 1 : 0, (s_arrow_state & 8) ? 1 : 0, 0);
        }
    }
    // Push the lock-key LED state (boot output report: bit0 Num, bit1 Caps).
    // tick() context — control transfers must never run inside the report
    // callback, and never start one mid-flash-guard.
    if (s_led_pending && !api->flash_guard_pending()) {
        s_led_pending = false;
        uint8_t led = (uint8_t)((s_num_lock ? 1 : 0) | (s_caps_lock ? 2 : 0));
        if (!api->control(0x21, 0x09 /*SET_REPORT*/, 0x0200 /*Output, report 0*/,
                          s_kbd_prof.ifnum, &led, 1))
            ulog("kbd LED report failed (continuing)");
    }
}

static void kbd_status(char* out, uint32_t n) {
    snprintf(out, n, "kbd");
}

#ifdef USB_DRV_MODULE
extern "C" const UsbDriverDesc usbdrv_ops =
#else
static const UsbDriverDesc s_kbd_desc =
#endif
{
    USB_DRIVER_ABI_VERSION,
    "kbd",
    &kbd_probe,
    &kbd_want,
    &kbd_start,
    &kbd_stop,
    &kbd_busy,
    &kbd_park,
    &kbd_resume,
    &kbd_tick,
    &kbd_status,
};

#ifndef USB_DRV_MODULE
const UsbDriverDesc* usbkbd_desc(void) { return &s_kbd_desc; }
#endif
