#include "elf_host.h"
#include "elf_loader.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"
#include "sound.h"
#include "notify.h"
#include "usb_manager.h"   // usb_pool_alloc/free — dynamic USB driver segments
#include "radio/proto_pool.h"  // proto_pool_alloc/free — protocol module segments
#include "radio/radio_capture.h"  // rcap:: (meshcore package capture exports)
#include "mesh_store.h"           // mstore:: + normalize_smart_quotes (proto_exports)
#include "emoji_font.h"           // emoji_compose (meshcore package export)
#include "../lib/ed25519/ed_25519.h"  // ed25519_key_exchange (proto_exports)
#include "tdeck_link.h"    // peer link: gblink veneers + module-exit detach
#include "screenshot.h"    // screen capture staging + PNG writer

#include <Arduino.h>
#include <Ticker.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_rom_sys.h>   // esp_rom_printf — safe to call from exception context
#include <esp_attr.h>      // IRAM_ATTR — the ELF fault handler runs from IRAM
#include <soc/timer_group_reg.h>  // TG1 MWDT (interrupt watchdog) register access
#include <lvgl.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <errno.h>
#include <time.h>
#include <locale.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// From main.cpp
extern Ticker lvgl_ticker;
extern void wake_activity();  // reset inactivity timer + restore backlights

// Panel access goes through the display backend (module video contract:
// 320x240 RGB565 — see display_dev.h).
#include "display/display_dev.h"

// GCC runtime builtins (soft-float, 64-bit division, double-precision)
extern "C" {
    long long __divdi3(long long, long long);
    float __divsf3(float, float);
    double __extendsfdf2(float);
    int __ltdf2(double, double);
    float __truncdfsf2(double);
    // Double-precision arithmetic
    double __adddf3(double, double);
    double __subdf3(double, double);
    double __muldf3(double, double);
    double __divdf3(double, double);
    int    __gtdf2(double, double);
    int    __gedf2(double, double);
    int    __ledf2(double, double);
    int    __nedf2(double, double);
    int    __eqdf2(double, double);
    double __floatsidf(int);
    double __floatunsidf(unsigned);
    int    __fixdfsi(double);
    unsigned __fixunsdfsi(double);
    long long __fixdfdi(double);
    double __floatdidf(long long);
    unsigned long long __udivdi3(unsigned long long, unsigned long long);
    unsigned long long __umoddi3(unsigned long long, unsigned long long);
    long long __moddi3(long long, long long);
}

// Device input backend: keyboard sampling/decode, trackball counters + click
// level, legacy-mode flag. The matrix tables and I2C protocol that used to be
// mirrored here live in input/input_tdeck.cpp now.
#include "input/input_dev.h"

// Touch controller mode: the shared zone layer + pre-rendered indicator
// chips. Armed on keyboardless boards when the launcher passed a layout via
// _elf_touch_layout; the Core-1 input task samples touch and feeds zone
// edges into the key queue (see poll_input).
#include "input/input_zones.h"
#include "input/zone_overlay.h"
#include "input/host_osk.h"
#include "input/input_ui.h"   // shared touch-input mode (TOUCH_MODE_*)

// Alt combo layer for ELF modules, keyed by the key's SYM-layer char (which
// uniquely names a physical key). Every entry exists because the T-Deck
// matrix has no such key at all. F1-F10 (Alt + digit-position keys) are a
// range, handled separately in the scan.
static const struct { char sym; unsigned char out; } kb_alt_combos[] = {
    { '/', '\\' },   // Alt+G: backslash -- DOS paths
    { '(', '<'  },   // Alt+( / Alt+): angle brackets -- DOS redirection
    { ')', '>'  },
    { '+', '='  },   // Alt+O: equals -- DOS SET syntax
};

// ---------------------------------------------------------------------------
// Key queue for host_get_key()
// ---------------------------------------------------------------------------

#define KEY_QUEUE_SIZE 32

struct KeyEvent {
    unsigned char key;
    int pressed;
};

static KeyEvent key_queue[KEY_QUEUE_SIZE];
static volatile int kq_head = 0;
static volatile int kq_tail = 0;

// Core 1 ELF input task: samples the keyboard at a fixed rate independent of
// the game loop (which monopolizes Core 0). See elf_input_start/stop.
static TaskHandle_t   s_input_task     = nullptr;
static volatile bool  s_input_task_run = false;

// Key ring: producers are the Core 1 input task (poll_input) and the USB HID
// keyboard driver (usb_task, also Core 1, via elf_input_inject) — the spinlock
// serializes them. Consumer is the game's scanInput on Core 0 (kq_pop). The
// barriers make the slot's data writes visible before kq_head advances (and
// vice-versa on the read side), which matters with the ends on different cores.
static portMUX_TYPE s_kq_mux = portMUX_INITIALIZER_UNLOCKED;

static void kq_push(unsigned char key, int pressed) {
    portENTER_CRITICAL(&s_kq_mux);
    int next = (kq_head + 1) % KEY_QUEUE_SIZE;
    if (next != kq_tail) {      // else full: drop
        key_queue[kq_head].key = key;
        key_queue[kq_head].pressed = pressed;
        __sync_synchronize();   // publish the slot before advancing head
        kq_head = next;
    }
    portEXIT_CRITICAL(&s_kq_mux);
}

static bool kq_pop(KeyEvent* out) {
    if (kq_head == kq_tail) return false;
    __sync_synchronize();       // see the slot writes that preceded head's advance
    *out = key_queue[kq_tail];
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

// Previous key states for edge detection (136 to cover trackball pseudo-codes 0x80-0x87)
// 256 so the full byte range is addressable — the module extension codes for
// F1-F10 live at 0xB0-0xB9 (Alt+digit), above the old 136 ceiling.
#define INPUT_STATE_SIZE 256
static bool prev_key_state[INPUT_STATE_SIZE] = {0};
static bool esc_held = false;
static uint32_t esc_hold_start = 0;
#define ESC_EXIT_HOLD_MS 1500

// Bindable quit: a keymap entry whose OUTPUT is this code exits the module
// instead of reaching it. A launcher binds it from its Controls screen like any
// other action, so one tap replaces the Alt+Backspace hold — the only exit
// reachable in legacy ASCII mode, which reports no modifiers and no holds.
// 0xFF sits above every code in use (ASCII, 0x80 shift, 0x81-0x85 trackball,
// 0x8C USB alt, 0x91-0x99 Dos extensions, 0xB0-0xB9 F-keys) and is never an
// input code, so it can only ever arrive as a keymap output.
#define HOST_KEY_QUIT 0xFF
static volatile bool s_quit_requested = false;

// USB keyboard Backspace/Alt held (set by elf_input_inject, pre-keymap) —
// OR'd into the exit-hold check so a USB keyboard can leave a module too.
static volatile bool s_usb_bs_down  = false;
static volatile bool s_usb_alt_down = false;

// ── Touch controller layout (set by _elf_touch_layout, per launch) ─────────
// Zone OUT codes are MODULE keycodes (the launcher generates the layout from
// its keybind actions), so zone edges push straight into the key queue and
// never pass through the keymap. OUT 0xFF is the quit zone: a screen corner
// is far easier to hit than a bound key, so it must be HELD to fire.
static InputZone s_elf_zones[INPUT_ZONES_MAX];
static int              s_elf_zone_count  = 0;
static volatile bool    s_zone_overlay_on = false;
static uint8_t          s_zone_prev_out   = 0;   // first held (OSK path)
// Held-zone SET from the previous poll, diffed to emit press/release edges
// so a d-pad direction and a face button can be down at the same time.
static uint8_t          s_zone_prev[INPUT_DEV_TOUCH_MAX] = {0};
static int              s_zone_prev_n     = 0;
static uint32_t         s_zone_quit_since = 0;
// Key under the finger while the host OSK is open, committed when it lifts.
static uint8_t          s_osk_hover       = 0;
#define ZONE_QUIT_HOLD_MS 600

// ── Screenshot capture (SHOT zone 0xFD / the bindable Screenshot key) ──────
// Both triggers set s_shot_requested; shot_service() (input task) claims the
// staging buffer, and the blit path fills it from the module's OWN pixels
// BEFORE the zone/OSK overlays are stamped into them, so a captured frame
// carries neither. A full-frame push completes a capture on its own; a band
// renderer's strips are accumulated until every row has been seen, and the
// timeout ends captures for renderers that never cover the whole panel (a
// letterboxed band renderer never writes the border rows).
#define SHOT_ACCUM_MS   250
#define SHOT_MAX_ROWS   240
// PSRAM can dip below the frame size for a moment while an emulator works —
// hw-observed on SNES: one shot in several failed to claim the buffer and the
// next succeeded. A request that lands at the wrong instant is retried for
// this long rather than lost.
#define SHOT_RETRY_MS   1000
static volatile bool s_shot_requested = false;   // a trigger fired
static volatile bool s_shot_arming    = false;   // buffer claimed, collecting
static volatile bool s_shot_full      = false;   // a whole frame arrived
static volatile bool s_shot_writing   = false;   // writer task alive
static volatile bool s_shot_feeding   = false;   // a push is inside shot_feed
static uint8_t       s_shot_rows[(SHOT_MAX_ROWS + 7) / 8];
static uint32_t      s_shot_start_ms  = 0;
static bool          s_shot_retrying   = false; // begin() failed, still trying
static uint32_t      s_shot_first_try  = 0;
static void shot_service();   // defined with the blit path it feeds from
// Chips inside the module's frame are stamped into every pushed frame; chip
// parts in the letterbox borders are pushed directly ONCE (the module never
// redraws there) — re-armed whenever the module clears the screen. The last
// frame rect lets the toggle erase border chips when hiding the overlay.
static volatile bool s_zone_border_pushed = false;
static volatile bool s_zone_frame_seen    = false;
static volatile int  s_zone_fx = 0, s_zone_fy = 0, s_zone_fw = 0, s_zone_fh = 0;
// Shift+Alt mode-cycle chord, this task's own edge state (loop()'s detector
// doesn't run during a module).
#define TOUCH_CHORD_MS 300
static bool     s_elf_chord_prev    = false;
static uint32_t s_elf_chord_last_ms = 0;

// Raw-delta mode: set by host_trackball_read() (module emulates a mouse and
// owns the ISR counters); reset before each module run.
static volatile bool s_trk_raw_mode = false;

// Trackball momentum state
static float trk_vel_x = 0, trk_vel_y = 0;
static float trk_impulse  = 1.5f;   // velocity added per ISR tick
static float trk_friction = 0.82f;  // multiplied each poll cycle
static float trk_thresh   = 0.4f;   // below this, key released
static bool  trk_momentum = true;   // false = legacy one-tick-per-poll

// ---------------------------------------------------------------------------
// Data-driven keymap: T-Deck physical key → module keycode.
// Runtime binding-layer switch: ALT+ENTER toggles it while a module runs.
// A module's -keymap turns T-Deck keys into keys the guest needs (arrows,
// Ctrl, F-keys), which STEALS those keys from typing -- fine while playing,
// useless at a DOS prompt. With the layer off the keyboard types normally
// and the mouse/trackball are unaffected, so "arrows plus mouse plus typing"
// is a chord away in either direction.
//
// OPT-IN per module, via `-kbtoggle N` from its launcher: without the arg
// the chord does not exist at all and ALT+ENTER stays an ordinary Enter, so
// modules that never need it cannot lose that keypress. N is the INITIAL
// layer state -- DOS passes 0 (start typing, toggle on for the game),
// anything else that opts in passes 1 (start playing).
static bool s_kb_toggle_enabled = false;
static bool s_kb_layer_on = true;

// One keyboard-backlight blink = "a toggle just changed state". Deliberately
// an EVENT, not a state indicator: holding the light off to mean "bindings
// active" fights the user's own brightness setting and the inactivity
// dimmer, and says nothing at the moment it matters.
// This is the NOTIFICATION blink (notify.cpp) -- the proven one, stepped by
// notify_tick on the mesh task, which keeps running during an ELF module. An
// earlier open-coded version here drove the backlight from the Core-1 input
// task and never visibly blinked.
void host_kb_blink(void) { notify_kbd_blink(); }

// Live matrix modifier levels for modules (bit0 shift, bit1 alt, bit2 sym).
// Modifiers are consumed here to build the key layers, so they never reach a
// module as key events -- but a module that wants a modifier+trackball chord
// (DOS: alt+click toggles mouse latch mode) has no other way to see them.
static volatile uint8_t s_key_mods = 0;
#define HOST_MOD_SHIFT 1
#define HOST_MOD_ALT   2
#define HOST_MOD_SYM   4
int host_key_mods(void) { return s_key_mods; }

// Populated by parse_keymap_arg() when the launcher passes -keymap.
// PURE REMAPPER: a zero entry means "no remap" and the key passes through
// unchanged (modules ignore codes they don't know). Only mapped keys are
// translated. Default is passthrough mode (no table lookup at all).
// ---------------------------------------------------------------------------
static uint8_t keymap_table[INPUT_STATE_SIZE];
static bool keymap_passthrough = true;   // default: all keys pass through as-is

// Parse a keymap string: "AD=77+81,AF=73+82,A0=61,..."
// Format: OUTPUT_HEX=INPUT_HEX[+ALT_INPUT_HEX], comma-separated.
static void parse_keymap_arg(const char* str) {
    memset(keymap_table, 0, sizeof(keymap_table));
    if (!str || !*str) return;

    const char* p = str;
    while (*p) {
        // Parse output keycode and primary input key (2 hex digits each)
        unsigned out = 0, key1 = 0, key2 = 0;
        if (sscanf(p, "%2x=%2x", &out, &key1) < 2) break;
        if (key1 < INPUT_STATE_SIZE)
            keymap_table[key1] = (uint8_t)out;

        // Advance past "OO=KK"
        const char* plus = strchr(p, '+');
        const char* comma = strchr(p, ',');

        // Check for +ALT before the next comma (or end of string)
        if (plus && (!comma || plus < comma)) {
            if (sscanf(plus + 1, "%2x", &key2) == 1) {
                if (key2 < INPUT_STATE_SIZE)
                    keymap_table[key2] = (uint8_t)out;
            }
        }

        // Advance to next entry
        if (comma) p = comma + 1;
        else break;
    }
}

// ---------------------------------------------------------------------------
// Poll keyboard + trackball, push key events onto queue for loaded module
// ---------------------------------------------------------------------------

// kb_only=true: read the keyboard matrix and queue key edges, but skip the
// trackball velocity integration (and preserve its previous state so no
// spurious trackball edge is generated). Used by the high-rate keyboard
// pump in host_sleep_ms — the trackball momentum model is tuned for the
// once-per-frame poll rate and must not be advanced ~200 times a second.
// Legacy ASCII mode pulse: each received byte is a discrete press with no
// matching release, so it is held in cur_state for a fixed window (re-arrival
// extends it) and the edge loop emits the down/up pair to the module.
#define LEGACY_ELF_PULSE_MS 120
static uint8_t  s_legacy_down_ch    = 0;
static uint32_t s_legacy_release_at = 0;

// Legacy binding-layer sequence: 'p', Backspace, Enter as the last three
// presses, in that order, with nothing between them. The legacy twin of the
// ALT+ENTER chord, which needs a modifier level legacy mode never reports.
// Deliberately untimed — the keys are recorded whenever they arrive.
#define LEGACY_SEQ_1 'p'
#define LEGACY_SEQ_2 0x08
#define LEGACY_SEQ_3 0x0D
static uint8_t s_legacy_hist[3]  = {0, 0, 0};
static bool    s_legacy_flip_req = false;

// Apply a touch-input mode to the running module: controller layout, host
// keyboard, or nothing. Owns the panel cleanup each transition needs — the
// module never redraws the letterbox borders, so indicator parts out there
// must be erased explicitly the moment they stop being shown.
static void elf_apply_touch_mode(uint8_t mode) {
    bool has_pad    = (s_elf_zone_count > 0);
    bool want_pad   = has_pad && (mode == TOUCH_MODE_PAD ||
                                  mode == TOUCH_MODE_PAD_HIDDEN);
    bool want_chips = has_pad && (mode == TOUCH_MODE_PAD);
    bool want_kb    = (mode == TOUCH_MODE_KB);

    if (s_zone_overlay_on && !want_chips) {
        int ffx = s_zone_frame_seen ? s_zone_fx : 0;
        int ffy = s_zone_frame_seen ? s_zone_fy : 0;
        int ffw = s_zone_frame_seen ? s_zone_fw : 0;
        int ffh = s_zone_frame_seen ? s_zone_fh : 0;
        zone_overlay_push_outside(ffx, ffy, ffw, ffh, true, display_dev_blit);
    }
    s_zone_overlay_on    = want_chips;
    s_zone_border_pushed = false;

    if (want_kb) {
        const InputZone* oz = nullptr;
        int on = 0;
        host_osk_zones(&oz, &on);
        if (host_osk_open(display_dev_blit)) {
            input_zones_set(oz, on);
            input_zones_enable(true);
            return;
        }
        // Frame allocation failed — fall through and arm nothing.
    } else if (host_osk_active()) {
        host_osk_close(display_dev_blit);
    }

    if (want_pad) {
        input_zones_set(s_elf_zones, s_elf_zone_count);
        input_zones_enable(true);
    } else {
        input_zones_enable(false);
        input_zones_clear();
    }
}

static void poll_input(bool kb_only = false) {
    bool cur_state[INPUT_STATE_SIZE] = {0};

    // Sample the keyboard via the input backend — one ASCII byte per press in
    // legacy mode (old keyboard-MCU firmware: no release/repeat/modifier
    // info, so the alt combos, F-keys, Shift+Backspace-Esc, ALT+ENTER layer
    // chord and the Alt+Backspace exit chord cannot fire. A legacy user
    // leaves a module with a bound quit key, reaching the binding layer via
    // the sequence below where a module opted into -kbtoggle; a USB
    // keyboard's chord still exits too). detect_legacy_fw=false: the
    // old-firmware heuristic stays on the interactive UI reader, exactly as
    // before the input split.
    input_dev_kbd_poll(/*detect_legacy_fw=*/false);
    if (input_dev_kbd_legacy_get()) {
        uint8_t v = input_dev_kbd_legacy_byte();
        if (v) {
            s_legacy_down_ch    = v;
            s_legacy_release_at = millis() + LEGACY_ELF_PULSE_MS;
            // Each byte is exactly one physical press (this firmware has no
            // repeat). Recorded RAW, before any keymap lookup, so binding one
            // of the three keys to a game action cannot break the sequence.
            s_legacy_hist[0] = s_legacy_hist[1];
            s_legacy_hist[1] = s_legacy_hist[2];
            s_legacy_hist[2] = v;
            if (s_legacy_hist[0] == LEGACY_SEQ_1 &&
                s_legacy_hist[1] == LEGACY_SEQ_2 &&
                s_legacy_hist[2] == LEGACY_SEQ_3) {
                s_legacy_flip_req = true;
                s_legacy_hist[0] = s_legacy_hist[1] = s_legacy_hist[2] = 0;
            }
        } else if (s_legacy_down_ch &&
                   (int32_t)(millis() - s_legacy_release_at) >= 0) {
            s_legacy_down_ch = 0;
        }
        if (s_legacy_down_ch) cur_state[s_legacy_down_ch] = true;
        // The backend's matrix stays zero in legacy mode: the modifier bools
        // and the decode loop below read "nothing pressed" and are inert.
    }

    bool lshift = false, rshift = false, sym = false, alt = false;
    input_dev_kbd_mods(&lshift, &rshift, &sym, &alt);
    bool shift = lshift || rshift;

    bool alt_enter = false;
    s_key_mods = (shift ? HOST_MOD_SHIFT : 0) | (alt ? HOST_MOD_ALT : 0)
               | (sym ? HOST_MOD_SYM : 0);

    // Decode the backend's pressed-key sample into character states.
    // Enter/Backspace positions arrive as base==0x0D/0x08 (see input_dev.h).
    InputKeyEv evs[INPUT_DEV_KEYS_MAX];
    int ev_n = input_dev_kbd_decode(evs, INPUT_DEV_KEYS_MAX);
    for (int i = 0; i < ev_n; i++) {
        if (evs[i].base == 0x0D) {
            // ALT+ENTER is the binding-layer chord where a module opted
            // in; everywhere else it stays a plain Enter.
            if (alt && s_kb_toggle_enabled) { alt_enter = true; continue; }
            cur_state[0x0D] = true;
        } else if (evs[i].base == 0x08) {
            // Shift+Backspace = Esc: the T-Deck has no Esc key and DOS
            // tools (FDISK menus etc.) require one. Plain Backspace is
            // unchanged, and the Alt+Backspace exit chord still sees
            // 0x08 because shift is not held during it.
            cur_state[shift ? 0x1B : 0x08] = true;
        } else {
            // Layer select: Alt + a digit-position key -> F1..F10 (module
            // maps 0xB0-0xB9); Sym -> number/symbol layer; else base.
            char ch;
            if (alt) {
                char s = (char)evs[i].sym;
                if (s >= '1' && s <= '9') {
                    ch = (char)(0xB0 + (s - '1'));       // F1-F9
                } else if (s == '0') {
                    ch = (char)0xB9;                     // F10
                } else {
                    ch = (char)evs[i].base;
                    for (auto &m : kb_alt_combos) {
                        if (m.sym == s) { ch = (char)m.out; break; }
                    }
                }
            } else if (sym) {
                ch = (char)evs[i].sym;
            } else {
                ch = (char)evs[i].base;
            }
            if (ch) cur_state[(uint8_t)ch] = true;
        }
    }

    // Shift state
    if (shift) cur_state[0x80] = true; // pseudo-code for shift

    if (!kb_only && s_trk_raw_mode) {
        // Module reads raw deltas via host_trackball_read() (e.g. PC-XT's
        // serial mouse) — leave the ISR counters alone, emit no 0x81-0x85.
    } else if (!kb_only) {
        // Trackball — momentum or legacy mode
        if (trk_momentum) {
            // Accumulate all pending ISR ticks into velocity
            int up = trackball_up;    trackball_up = 0;
            int dn = trackball_down;  trackball_down = 0;
            int lt = trackball_left;  trackball_left = 0;
            int rt = trackball_right; trackball_right = 0;

            trk_vel_y -= up * trk_impulse;
            trk_vel_y += dn * trk_impulse;
            trk_vel_x -= lt * trk_impulse;
            trk_vel_x += rt * trk_impulse;

            // Apply friction
            trk_vel_x *= trk_friction;
            trk_vel_y *= trk_friction;

            // Snap to zero below threshold
            if (fabsf(trk_vel_x) < trk_thresh) trk_vel_x = 0;
            if (fabsf(trk_vel_y) < trk_thresh) trk_vel_y = 0;

            // Report as held keys while velocity is above threshold
            if (trk_vel_y < -trk_thresh) cur_state[0x81] = true;  // up
            if (trk_vel_y >  trk_thresh) cur_state[0x82] = true;  // down
            if (trk_vel_x < -trk_thresh) cur_state[0x83] = true;  // left
            if (trk_vel_x >  trk_thresh) cur_state[0x84] = true;  // right
        } else {
            // Legacy: one tick per poll cycle
            if (trackball_up > 0)    { trackball_up--;    cur_state[0x81] = true; }
            if (trackball_down > 0)  { trackball_down--;  cur_state[0x82] = true; }
            if (trackball_left > 0)  { trackball_left--;  cur_state[0x83] = true; }
            if (trackball_right > 0) { trackball_right--; cur_state[0x84] = true; }
        }
        if (trackball_click > 0) { trackball_click = 0; cur_state[0x85] = true; }
    } else {
        // Keyboard-only tick: carry the trackball's previous state forward so
        // edge detection sees no change (no spurious press/release) and the
        // momentum integration stays exclusively on the lower-rate full poll.
        for (int i = 0x81; i <= 0x85; i++) cur_state[i] = prev_key_state[i];
    }

    // ALT+ENTER (rising edge) flips the binding layer. Anything held across
    // the flip is released under the OLD mapping first, or the guest would
    // get a release for a key it never saw pressed and latch it down; the
    // edge loop below then re-presses it under the new mapping.
    // Rising edge, plus a debounce window. The matrix is a polled LEVEL so it
    // does not suffer the trackball's every-falling-edge problem -- but a key
    // that bounces across two 10ms polls, or a single dropped I2C matrix read
    // (which reads as "all keys up" for one poll), still fabricates a second
    // rising edge, and a toggle that fires twice lands back where it started
    // while blinking only once. Nobody flips this deliberately inside 300ms.
    static bool alt_enter_prev = false;
    static uint32_t alt_enter_last_ms = 0;
    // The legacy sequence asks for the same flip. Consume the request even when
    // the module has not opted in, so it can never fire later out of context.
    bool flip_req = (alt_enter && !alt_enter_prev) || s_legacy_flip_req;
    s_legacy_flip_req = false;
    if (flip_req && s_kb_toggle_enabled
        && !keymap_passthrough
        && (uint32_t)(millis() - alt_enter_last_ms) >= 300) {
        alt_enter_last_ms = millis();
        for (int i = 0; i < INPUT_STATE_SIZE; i++) {
            if (!prev_key_state[i]) continue;
            uint8_t out = s_kb_layer_on ? keymap_table[i] : 0;
            if (!out) out = (uint8_t)i;
            kq_push(out, 0);
        }
        memset(prev_key_state, 0, INPUT_STATE_SIZE);
        s_kb_layer_on = !s_kb_layer_on;
        host_kb_blink();        // one blink acknowledges the flip
        SLog.printf("[elf_host] key bindings %s\n", s_kb_layer_on ? "ON" : "OFF");
    }
    alt_enter_prev = alt_enter;

    // Edge detection: generate press/release events for changed keys.
    // In passthrough mode (default), all keys are pushed as-is. In keymap
    // mode the table is a pure remapper: mapped keys translate, everything
    // else passes through unchanged (unknown codes are ignored by modules).
    for (int i = 0; i < INPUT_STATE_SIZE; i++) {
        if (cur_state[i] != prev_key_state[i]) {
            uint8_t out;
            if (keymap_passthrough || !s_kb_layer_on) {
                out = (uint8_t)i;  // pass through as-is
            } else {
                out = keymap_table[i];
                if (!out) out = (uint8_t)i;   // unmapped: pass through
            }
            // Quit binding: swallowed on BOTH edges, so no module ever sees a
            // 0xFF it does not know or a release without a press. Reachable
            // only while the table is consulted, which is why the bound key
            // still types normally with the binding layer off.
            if (out == HOST_KEY_QUIT) {
                if (cur_state[i]) s_quit_requested = true;
                continue;
            }
            // Screenshot binding: swallowed on both edges like quit, so the
            // module never sees a 0xFD or a release without a press.
            if (out == INPUT_ZONE_SHOT) {
                if (cur_state[i]) s_shot_requested = true;
                continue;
            }
            if (out)
                kq_push(out, cur_state[i] ? 1 : 0);
        }
    }
    memcpy(prev_key_state, cur_state, INPUT_STATE_SIZE);

    // Exit hold detection: hold Alt+Backspace for 1.5s to return to the
    // launcher. Plain backspace stays an ordinary key (DOS et al. use it for
    // editing); requiring the Alt chord makes exits deliberate. The timer
    // starts only once BOTH are down; releasing either resets it. s_usb_*
    // fold in a USB keyboard's Backspace/Alt (this poll runs at 100Hz and
    // would otherwise clear esc_held from the matrix every tick).
    if ((cur_state[0x08] || s_usb_bs_down) && (alt || s_usb_alt_down)) {
        if (!esc_held) { esc_held = true; esc_hold_start = millis(); }
    } else {
        esc_held = false;
    }

    // Touch controller zones + host OSK, keyboardless boards only — sampled
    // at the trackball cadence (~33Hz; a touch read is an I2C transaction,
    // too heavy for the 100Hz keyboard tick). Zone edges bypass the keymap:
    // OUT already IS the module keycode — or plain ASCII while the OSK is
    // open. The quit zone (0xFF) fires only after a deliberate hold and is
    // swallowed like the bound quit key — the module never sees it.
    if (!kb_only) {
        if (input_zones_enabled()) {
            int16_t xs[INPUT_DEV_TOUCH_MAX], ys[INPUT_DEV_TOUCH_MAX];
            int np = input_dev_touch_read_multi(xs, ys, INPUT_DEV_TOUCH_MAX);
            input_zones_touch_multi(xs, ys, np);

            uint8_t cur[INPUT_DEV_TOUCH_MAX];
            int cn = input_zones_held_all(cur, INPUT_DEV_TOUCH_MAX);

            // A SHOT zone tap is a plain edge (the zone layer already
            // debounced it); only quit needs a hold.
            if (input_zones_shot_take()) s_shot_requested = true;

            // Quit needs a deliberate unbroken hold; lifting cancels it.
            if (input_zones_out_held(HOST_KEY_QUIT)) {
                if (!s_zone_quit_since) s_zone_quit_since = millis();
                else if (millis() - s_zone_quit_since >= ZONE_QUIT_HOLD_MS)
                    s_quit_requested = true;
            } else {
                s_zone_quit_since = 0;
            }

            // Typing commits on RELEASE, controller zones on the press edge.
            // A keyboard key is ~30px: a finger that lands between two and
            // settles crosses both, and a press-edge send types both. Only
            // the key under the finger when it LIFTS is sent, so a sloppy
            // tap is one character and sliding off the keys before lifting
            // is none. Games need the opposite — key-down has a duration —
            // so the hold model stays for every non-OSK layout.
            bool osk = host_osk_active();
            if (osk) {
                // The keyboard stays single-point: two-finger typing has no
                // meaning and the commit-on-lift model tracks ONE key.
                uint8_t zout = cn > 0 ? cur[0] : 0;
                if (zout != s_zone_prev_out) {
                    uint8_t prev = s_zone_prev_out;
                    s_zone_prev_out = zout;
                    if (zone_out_is_key(prev))
                        host_osk_key_feedback(prev, false, display_dev_blit);
                    if (zone_out_is_key(zout))
                        host_osk_key_feedback(zout, true, display_dev_blit);
                }
                if (np > 0) {
                    s_osk_hover = zout;  // 0 once the finger leaves the keys
                } else if (s_osk_hover) {
                    uint8_t k = s_osk_hover; // the key it lifted on
                    s_osk_hover = 0;
                    if (zone_out_is_key(k)) { kq_push(k, 1); kq_push(k, 0); }
                }
            } else {
                // Controller: diff the held SET so a direction and a button
                // can be down together (and a third finger is just another
                // member). Release-then-press order keeps a slide between
                // two zones looking the same as it did single-touch.
                s_osk_hover = 0;
                for (int i = 0; i < s_zone_prev_n; i++) {
                    uint8_t o = s_zone_prev[i];
                    bool still = false;
                    for (int j = 0; j < cn && !still; j++) still = (cur[j] == o);
                    if (!still && zone_out_is_key(o)) kq_push(o, 0);
                }
                for (int j = 0; j < cn; j++) {
                    uint8_t o = cur[j];
                    bool was = false;
                    for (int i = 0; i < s_zone_prev_n && !was; i++)
                        was = (s_zone_prev[i] == o);
                    if (!was && zone_out_is_key(o)) kq_push(o, 1);
                }
                s_zone_prev_out = cn > 0 ? cur[0] : 0;
            }
            memcpy(s_zone_prev, cur, (size_t)cn);
            s_zone_prev_n = cn;
        }

        // Mode trigger — the board's aux button (Heltec IO key) or the
        // Shift+Alt chord where a keyboard exists. loop()'s own chord
        // detector is dormant while a module owns the device, so this is
        // the second half of that pair; both advance the SAME shared mode.
        // Requires both modifiers and no character key held, so it cannot
        // fire inside a shift- or alt-layer keystroke.
        bool chord_edge = false;
        {
            bool other = false;
            for (int i = 1; i < 0x80 && !other; i++)
                if (cur_state[i]) other = true;
            bool chord = shift && alt && !other;
            if (chord && !s_elf_chord_prev &&
                (uint32_t)(millis() - s_elf_chord_last_ms) >= TOUCH_CHORD_MS) {
                s_elf_chord_last_ms = millis();
                chord_edge = true;
            }
            s_elf_chord_prev = chord;
        }
        if (input_dev_aux_btn_take() || chord_edge) {
            // Nothing is held down under the OSK (its keys commit on lift),
            // so there is no release to emit — and a tap still in progress
            // must not fire into the mode being switched to.
            if (!host_osk_active()) {
                for (int i = 0; i < s_zone_prev_n; i++)   // release every held
                    if (zone_out_is_key(s_zone_prev[i]))
                        kq_push(s_zone_prev[i], 0);        // across table swaps
            }
            s_zone_prev_out = 0;
            s_zone_prev_n   = 0;
            s_osk_hover     = 0;
            elf_apply_touch_mode(
                input_ui_touch_mode_cycle(s_elf_zone_count > 0));
        }
    }
}

// ---------------------------------------------------------------------------
// Core 1 ELF input task
// ---------------------------------------------------------------------------
// While an ELF module runs, the game owns Core 0 entirely (the elf_run task,
// and loopTask blocked behind it), so the only way to sample input is once
// per game frame — which on a heavy cart collapses to ~10 reads/sec and drops
// any keypress shorter than a frame. This task runs on Core 1 and reads the
// keyboard at a fixed 100Hz regardless of what the game is doing, queuing
// edges into the same kq ring host_get_key drains. A tap is now caught and
// latched even while Core 0 is mid-frame.
//
// I2C safety: _launch_elf blocks loopTask for the whole module run, so the
// firmware's own keyboard scanning (the loop() path — untouched) is dormant;
// this task is the only keyboard-matrix READER between elf_input_start/stop,
// and stop() returns only once it has fully exited, before loopTask resumes.
// The notification blink (notify.cpp, mesh task) may WRITE the backlight
// brightness concurrently — Wire's per-transaction HAL lock serializes it
// against the matrix reads here.
//
// The keyboard is read every tick (100Hz); the trackball's momentum model is
// integrated only every 3rd tick (~33Hz) so its tuned feel is unchanged by
// the higher keyboard rate (poll_input's kb_only path skips the trackball).

#define ELF_INPUT_PERIOD_MS   10   // 100 Hz keyboard sampling
#define ELF_INPUT_TRK_EVERY   3    // integrate trackball every Nth tick (~33 Hz)

static void elf_input_task_body(void* param) {
    (void)param;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;
    while (s_input_task_run) {
        bool do_trackball = (tick % ELF_INPUT_TRK_EVERY) == 0;
        poll_input(/*kb_only=*/!do_trackball);
        shot_service();
        tick++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(ELF_INPUT_PERIOD_MS));
    }
    s_input_task = nullptr;   // signal stop() that we've exited (no more I2C)
    vTaskDelete(nullptr);
}

// Start the Core 1 input task. Resets the queue + edge state so the module
// starts from a clean slate. Called from _launch_elf just before the module
// runs; idempotent if already running.
static void elf_input_start() {
    if (s_input_task) return;
    kq_head = kq_tail = 0;
    memset(prev_key_state, 0, INPUT_STATE_SIZE);
    esc_held = false;
    s_usb_bs_down = false;
    s_usb_alt_down = false;
    s_legacy_down_ch = 0;
    s_legacy_release_at = 0;
    s_legacy_hist[0] = s_legacy_hist[1] = s_legacy_hist[2] = 0;
    s_legacy_flip_req = false;
    s_quit_requested = false;
    s_shot_requested = false;
    s_osk_hover = 0;
    // The zone layer may still hold a LUA layout: Lua teardown is C-side and
    // never ran touchlayout's disarm if the launching app had zones armed.
    // Those out-codes are Lua key codes, not module keycodes — force-disarm
    // before applying the mode below.
    input_zones_enable(false);
    input_zones_clear();
    s_zone_prev_out      = 0;
    s_zone_prev_n        = 0;
    s_zone_quit_since    = 0;
    s_zone_overlay_on    = false;
    s_zone_border_pushed = false;
    s_zone_frame_seen    = false;
    s_elf_chord_prev     = false;
    // Indicator chips are rendered up front (cheap, and the mode can turn
    // them on later); the shared touch mode then decides what is armed —
    // controller layout, host keyboard, or nothing. It carries over from
    // the Lua side, so a board that came up with touch controls live starts
    // the module the same way.
    if (s_elf_zone_count > 0) zone_overlay_build(s_elf_zones, s_elf_zone_count);
    elf_apply_touch_mode(input_ui_touch_mode());
    s_input_task_run = true;
    // Priority 5: ABOVE usb_mgr (4), sound_task (3) and elf_blit (3). The
    // keyboard poll is a tiny, latency-critical task (one I2C read every 10ms);
    // when USB host is streaming, those higher-priority core-1 tasks were
    // starving it at priority 2 — the game saw laggy/missed/doubled keys. Input
    // responsiveness beats a few ms of audio/blit jitter (absorbed by their
    // buffers), and the poll yields immediately so it can't starve them.
    if (xTaskCreatePinnedToCore(elf_input_task_body, "elf_input", 3072,
                                nullptr, 5, &s_input_task, 1 /* Core 1 */) != pdPASS) {
        // Couldn't spawn — fall back to host_get_key's own polling.
        s_input_task = nullptr;
        s_input_task_run = false;
        SLog.println("[elf_host] WARN: input task spawn failed, using per-frame polling");
    }
}

// Stop the Core 1 input task and wait for it to fully exit before returning,
// so no I2C read is in flight when loopTask resumes its own keyboard scan.
static void elf_input_stop() {
    if (s_input_task) {
        s_input_task_run = false;
        for (int i = 0; i < 100 && s_input_task; i++) {  // ~200ms safety cap
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        s_input_task = nullptr;
    }
    // Zone/OSK teardown does NOT happen here: the blit task may still be
    // pushing chip/OSK pixels until blit_drain() — elf_zones_teardown() runs
    // after it in _launch_elf.
}

// Disarm the touch layout and free the overlay chips + OSK frame. Only safe
// once no blit can be in flight (after blit_drain): the blit paths read
// those buffers, which is exactly why host_osk_close() does NOT free — this
// is the single point that does. No panel work: LVGL repaints on resume.
static void elf_zones_teardown() {
    // A capture still collecting when the module exits would strand the
    // staging buffer AND make every later screenshot_begin refuse, so cancel
    // it here — safe for the same reason the zone buffers are freed here: no
    // blit, and therefore no shot_feed, can be in flight. A capture already
    // handed to the writer task is left alone; that task owns the buffer.
    s_shot_requested = false;
    s_shot_retrying  = false;
    if (s_shot_arming) {
        s_shot_arming = false;
        screenshot_end();
        SLog.println("[shot] capture cancelled: module exited");
    }
    host_osk_free();
    input_zones_enable(false);
    input_zones_clear();
    zone_overlay_clear();
    s_elf_zone_count  = 0;
    s_zone_overlay_on = false;
    s_zone_prev_out   = 0;
    s_zone_prev_n     = 0;
}

// ── Firmware-internal injection (USB HID keyboard; see elf_host.h) ─────────

bool elf_input_active(void) { return s_input_task_run; }

void elf_input_inject(unsigned char key, int pressed) {
    if (key == 0x08) s_usb_bs_down  = (pressed != 0);  // exit-hold, pre-keymap
    if (key == 0x8C) s_usb_alt_down = (pressed != 0);  // exit-hold Alt (USB kbd)
    if (!s_input_task_run) return;
    uint8_t out;
    // !s_kb_layer_on mirrors poll_input: with the binding layer off a USB
    // keyboard types raw too, so a bound key (quit included) cannot fire while
    // the user is at a prompt.
    if (keymap_passthrough || !s_kb_layer_on) {
        out = key;
    } else {
        out = keymap_table[key];
        if (!out) out = key;    // unmapped: pass through (as poll_input)
    }
    if (out == HOST_KEY_QUIT) {
        if (pressed) s_quit_requested = true;
        return;
    }
    if (out == INPUT_ZONE_SHOT) {
        if (pressed) s_shot_requested = true;
        return;
    }
    if (out) kq_push(out, pressed ? 1 : 0);
}

// ── Dynamic USB driver modules (loaded by usb_core at device attach) ────────
// A .drv.elf is event-driven — no main(), no run loop, no game lifecycle. It
// exports one symbol, `usbdrv_ops` (a const UsbDriverDesc — the function
// pointers inside are relocation-remapped to instruction-side addresses by
// the loader, same mechanism as the game modules' C++ vtables). Its segments
// come from the boot-reserved low-PSRAM pool (usb_pool.cpp) so load/unload
// at any session time never fragments the region games coalesce.
//
// The export table is DELIBERATELY tiny: no malloc (the game allocator's
// exit-time tracked-free would tear a resident driver's memory out from
// under it — by construction drivers can't allocate), no stdio, no floats.
// Everything USB goes through the UsbHostApi vtable the core passes in.

static const elf_symbol_t driver_exports[] = {
    { "memcpy",    (void*)memcpy },
    { "memset",    (void*)memset },
    { "memcmp",    (void*)memcmp },
    { "memmove",   (void*)memmove },
    { "strlen",    (void*)strlen },
    { "strcmp",    (void*)strcmp },
    { "strncmp",   (void*)strncmp },
    { "strchr",    (void*)strchr },
    { "snprintf",  (void*)snprintf },
    { "vsnprintf", (void*)vsnprintf },
    ELF_SYMBOL_END
};

static void* drv_seg_alloc(size_t size, void*) { return usb_pool_alloc(size); }
static void  drv_seg_free(void* p, void*)      { usb_pool_free(p); }

// Load one driver module from a drive-prefixed path ("L:/usb_drivers/kbd/
// kbd.drv.elf"). Returns the module handle (NULL on any failure, reason
// logged to the USB ring) and the exported ops struct via out_ops. usb_task
// context (called from enumeration).
void* elf_usb_driver_load(const char* path, const void** out_ops) {
    *out_ops = NULL;
    uint32_t size = 0;
    void* buf = meshpunk_read_all(path, &size);
    if (!buf) { usb_ulog("drv: read failed: %s", path); return NULL; }

    elf_module_t* mod = elf_load_ex(buf, size, driver_exports,
                                    drv_seg_alloc, drv_seg_free, NULL);
    heap_caps_free(buf);
    if (!mod) {
        usb_ulog("drv: load failed (%s) — pool free %uB",
                 path, (unsigned)usb_pool_free_bytes());
        return NULL;
    }

    const void* ops = elf_lookup(mod, "usbdrv_ops");
    if (!ops) {
        usb_ulog("drv: no usbdrv_ops export: %s", path);
        elf_unload(mod);
        return NULL;
    }

    uint32_t ts = 0, te = 0;
    elf_text_range(mod, &ts, &te);
    usb_ulog("drv: loaded %s text %08X-%08X pool %uB free",
             path, (unsigned)ts, (unsigned)te, (unsigned)usb_pool_free_bytes());
    *out_ops = ops;
    return mod;
}

void elf_usb_driver_unload(void* mod) {
    if (!mod) return;
    elf_unload((elf_module_t*)mod);
    usb_ulog("drv: unloaded (pool %uB free)", (unsigned)usb_pool_free_bytes());
}

// ── LoRa-protocol modules (.loraproto.elf) ──────────────────────────────────
// Symbols a protocol module may import. Richer than driver_exports
// (protocols persist NodeDB/config via stdio-over-VFS and format text), but
// still no malloc — module memory comes from MeshHostApi mem_alloc (the
// protocol pool). APPEND-ONLY: fielded protocol elfs resolve against this
// table; the module build.ps1 UND audit is what grows it (e.g. mbedtls
// entry points for the vendor crypto).

// src/radio/proto_crypto.cpp — the meshtastic-lite crypto seam.
extern "C" {
void mesh_aes_block_encrypt(const uint8_t*, int, const uint8_t*, uint8_t*);
void mesh_sha256(const uint8_t*, size_t, uint8_t*);
bool mesh_ccm_encrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*, uint8_t*, size_t);
bool mesh_ccm_decrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*);
bool mesh_x25519_dh(const uint8_t*, const uint8_t*, uint8_t*);
bool mesh_generate_keypair(uint8_t*, uint8_t*);
}

// Defined further down (with the game-module stdio wrappers they share
// bounce/lock machinery with); the table needs the names now. Their
// definitions sit in the file's extern "C" region — linkage must match.
extern "C" {
FILE*  elf_fopen(const char* path, const char* mode);
int    elf_fclose(FILE* f);
int    elf_fseek(FILE* f, long offset, int whence);
long   elf_ftell(FILE* f);
size_t elf_fread(void* dst, size_t size, size_t nmemb, FILE* f);
size_t elf_fwrite(const void* src, size_t size, size_t nmemb, FILE* f);
static FILE* proto_fopen(const char* path, const char* mode);
static int   proto_fclose(FILE* f);
static char* proto_fgets(char* s, int n, FILE* f);
static int   proto_fprintf(FILE* f, const char* fmt, ...);
static int   proto_fputs(const char* s, FILE* f);
static int   proto_fflush(FILE* f);
static int   proto_remove(const char* path);
static int   proto_rename(const char* a, const char* b);
static int   proto_mkdir(const char* path);
}
extern "C" void mesh_lock(void);
extern "C" void mesh_unlock(void);
extern "C" int  mcs_channel_msg_path(const char* name, char* out, int out_sz);
extern "C" int  mcs_dm_msg_path(const char* peer, char* out, int out_sz);
extern "C" int  mcs_messages_dir_path(char* out, int out_sz);
extern "C" void mcs_append_extra_path(const char* msg_log_path,
                                      const uint8_t* hash,
                                      const ObservedPath* op);
extern "C" int  mcs_read_one_stored_msg(const char* path, uint32_t offset,
                                        StoredMsg* m);

static const elf_symbol_t proto_exports[] = {
    { "memcpy",    (void*)memcpy },
    { "memset",    (void*)memset },
    { "memcmp",    (void*)memcmp },
    { "memmove",   (void*)memmove },
    { "strlen",    (void*)strlen },
    { "strcmp",    (void*)strcmp },
    { "strncmp",   (void*)strncmp },
    { "strcasecmp",  (void*)strcasecmp },
    { "strncasecmp", (void*)strncasecmp },
    { "strcpy",    (void*)strcpy },
    { "strncpy",   (void*)strncpy },
    { "strchr",    (void*)strchr },
    { "strrchr",   (void*)strrchr },
    { "strstr",    (void*)strstr },
    { "strtol",    (void*)strtol },
    { "atoi",      (void*)atoi },
    // Vendor packet-id/CSMA randomness (mtlite srand()s from the host TRNG
    // at init — this rand feeds protocol jitter, not key material).
    { "rand",      (void*)rand },
    { "srand",     (void*)srand },
    { "snprintf",  (void*)snprintf },
    { "vsnprintf", (void*)vsnprintf },
    { "sscanf",    (void*)sscanf },
    // stdio over VFS: config/NodeDB persistence on the protocol's storage.
    // All SPI-locked (shared bus with TFT/SD) via the proto_ wrappers above;
    // fread/fwrite additionally bounce PSRAM buffers through internal RAM
    // (the pool is PSRAM, SD DMA is not PSRAM-safe).
    { "fopen",     (void*)proto_fopen },
    { "fclose",    (void*)proto_fclose },
    { "fread",     (void*)elf_fread },   // locked + PSRAM bounce, no tracking inside
    { "fwrite",    (void*)elf_fwrite },  // locked + PSRAM bounce, no tracking inside
    { "fseek",     (void*)elf_fseek },
    { "ftell",     (void*)elf_ftell },
    { "fgets",     (void*)proto_fgets },
    { "fprintf",   (void*)proto_fprintf },
    { "fputs",     (void*)proto_fputs },   // gcc rewrites constant fprintf into this
    { "fflush",    (void*)proto_fflush },
    { "remove",    (void*)proto_remove },
    { "rename",    (void*)proto_rename },
    // newlib errno accessor (libm error paths reference it).
    { "__errno",   (void*)__errno },
    // Lua C API (ABI v2 lua_open surface): the protocol registers its own
    // bindings into the firmware's lua_State. The common macro forms resolve
    // to these (lua_pushcfunction→pushcclosure, lua_pcall→pcallk,
    // lua_tostring→tolstring, luaL_checkstring→checklstring,
    // lua_newtable→createtable, lua_tonumber/integer→*x).
    { "lua_gettop",        (void*)lua_gettop },
    { "lua_settop",        (void*)lua_settop },
    { "lua_pushvalue",     (void*)lua_pushvalue },
    { "lua_checkstack",    (void*)lua_checkstack },
    { "lua_pushnil",       (void*)lua_pushnil },
    { "lua_pushboolean",   (void*)lua_pushboolean },
    { "lua_pushinteger",   (void*)lua_pushinteger },
    { "lua_pushnumber",    (void*)lua_pushnumber },
    { "lua_pushstring",    (void*)lua_pushstring },
    { "lua_pushlstring",   (void*)lua_pushlstring },
    { "lua_pushcclosure",  (void*)lua_pushcclosure },
    { "lua_toboolean",     (void*)lua_toboolean },
    { "lua_tointegerx",    (void*)lua_tointegerx },
    { "lua_tonumberx",     (void*)lua_tonumberx },
    { "lua_tolstring",     (void*)lua_tolstring },
    { "lua_type",          (void*)lua_type },
    { "lua_createtable",   (void*)lua_createtable },
    { "lua_getfield",      (void*)lua_getfield },
    { "lua_setfield",      (void*)lua_setfield },
    { "lua_gettable",      (void*)lua_gettable },
    { "lua_settable",      (void*)lua_settable },
    { "lua_rawgeti",       (void*)lua_rawgeti },
    { "lua_rawseti",       (void*)lua_rawseti },
    { "lua_next",          (void*)lua_next },
    { "lua_getglobal",     (void*)lua_getglobal },
    { "lua_setglobal",     (void*)lua_setglobal },
    { "lua_pcallk",        (void*)lua_pcallk },
    { "lua_error",         (void*)lua_error },
    { "lua_rotate",        (void*)lua_rotate },   // lua_remove/insert macros
    { "luaL_error",        (void*)luaL_error },
    { "luaL_ref",          (void*)luaL_ref },
    { "luaL_unref",        (void*)luaL_unref },
    { "luaL_checklstring", (void*)luaL_checklstring },
    { "luaL_optlstring",   (void*)luaL_optlstring },
    { "luaL_checkinteger", (void*)luaL_checkinteger },
    { "luaL_optinteger",   (void*)luaL_optinteger },
    { "luaL_checknumber",  (void*)luaL_checknumber },
    { "luaL_optnumber",    (void*)luaL_optnumber },
    // Identity crypto (lib/ed25519, stays firmware — ABI doc D-3). The
    // meshcore package's mesh::Identity resolves these; mtlite uses the
    // mesh_* primitives below instead (raw-Curve25519 format).
    { "ed25519_key_exchange",   (void*)ed25519_key_exchange },
    { "ed25519_create_keypair", (void*)ed25519_create_keypair },
    { "ed25519_derive_pub",     (void*)ed25519_derive_pub },
    { "ed25519_sign",           (void*)ed25519_sign },
    // ── meshcore package surface ─────────────────────────────────────────
    // libc stragglers its punkmesh port pulls (free pairs emoji_compose's
    // firmware malloc), locks, FreeRTOS queue API (kernel objects work
    // cross-boundary; the RxEvent queue is module-owned), emoji composer.
    { "qsort",     (void*)qsort },
    { "strtoul",   (void*)strtoul },
    { "atof",      (void*)atof },
    { "sprintf",   (void*)sprintf },
    { "malloc",    (void*)malloc },
    { "free",      (void*)free },
    // L: filesystem stats (the phone app's storage figures — real values).
    { "_Z14mp_littlefs_dfPjS_", (void*)&mp_littlefs_df },
    { "mkdir",     (void*)proto_mkdir },   // SPI-locked (VFS metadata write)
    { "mesh_lock",   (void*)mesh_lock },
    { "mesh_unlock", (void*)mesh_unlock },
    { "vTaskDelay",         (void*)vTaskDelay },
    { "xQueueGenericSend",  (void*)xQueueGenericSend },
    { "xQueueReceive",      (void*)xQueueReceive },
    { "xQueueGenericCreate",(void*)xQueueGenericCreate },
    { "emoji_compose",      (void*)emoji_compose },
    { "emoji_decompose",    (void*)emoji_decompose },
    // mstore + capture + text utils, exported by MANGLED name (both sides
    // are the same xtensa g++; the package build's UND audit hard-fails on
    // any drift). String/fs::FS-typed mstore calls do NOT cross this way —
    // shim types are not layout-compatible — they use the mcs_* C bridges.
    { "_Z22normalize_smart_quotesPKcPcj", (void*)&normalize_smart_quotes },
    { "_ZN4rcap4pushEh",   (void*)static_cast<PktCapture*(*)(uint8_t)>(&rcap::push) },
    { "_ZN4rcap6newestEv", (void*)&rcap::newest },
    { "_ZN4rcap5startEv",  (void*)&rcap::start },
    { "_ZN4rcap4stopEv",   (void*)&rcap::stop },
    { "_ZN4rcap10pop_oldestEP10PktCapture", (void*)&rcap::pop_oldest },
    { "_ZN4rcap12take_droppedEv",           (void*)&rcap::take_dropped },
    { "_ZN6mstore16set_max_messagesEi",     (void*)&mstore::set_max_messages },
    { "_ZN6mstore22append_channel_messageEPKciS1_S1_jffhbtPKhS3_j", (void*)&mstore::append_channel_message },
    { "_ZN6mstore17append_dm_messageEPKcS1_S1_jffhbtPKhS3_S3_j",    (void*)&mstore::append_dm_message },
    { "_ZN6mstore19unread_bump_channelEPKc",  (void*)&mstore::unread_bump_channel },
    { "_ZN6mstore14unread_bump_dmEPKc",       (void*)&mstore::unread_bump_dm },
    { "_ZN6mstore14unread_channelEPKc",       (void*)&mstore::unread_channel },
    { "_ZN6mstore9unread_dmEPKc",             (void*)&mstore::unread_dm },
    { "_ZN6mstore20unread_clear_channelEPKc", (void*)&mstore::unread_clear_channel },
    { "_ZN6mstore15unread_clear_dmEPKc",      (void*)&mstore::unread_clear_dm },
    { "_ZN6mstore12unread_totalEv",           (void*)&mstore::unread_total },
    { "_ZN6mstore21push_channel_messagesEP9lua_StatePKci", (void*)&mstore::push_channel_messages },
    { "_ZN6mstore16push_dm_messagesEP9lua_StatePKci",      (void*)&mstore::push_dm_messages },
    { "_ZN6mstore20push_dm_thread_namesEP9lua_State",      (void*)&mstore::push_dm_thread_names },
    { "_ZN6mstore22push_chat_page_channelEP9lua_StatePKciji", (void*)&mstore::push_chat_page_channel },
    { "_ZN6mstore17push_chat_page_dmEP9lua_StatePKciji",      (void*)&mstore::push_chat_page_dm },
    { "_ZN6mstore18push_msg_summariesEP9lua_StatePK13MStoreChanRefi", (void*)&mstore::push_msg_summaries },
    { "_ZN6mstore18push_routing_queryEP9lua_StatePKcjj",   (void*)&mstore::push_routing_query },
    { "_ZN6mstore20push_routing_sendersEP9lua_StatePKci",  (void*)&mstore::push_routing_senders },
    { "_ZN6mstore15push_path_tableEP9lua_StatetPKh",       (void*)&mstore::push_path_table },
    { "_ZN6mstore22lookup_persisted_pathsEP9lua_StatePKcS3_S3_", (void*)&mstore::lookup_persisted_paths },
    { "_ZN6mstore21read_stored_msgs_fromEPKcjP9StoredMsgPjiS4_S4_", (void*)&mstore::read_stored_msgs_from },
    { "_ZN6mstore20read_all_stored_msgsEPKcP9StoredMsgi",  (void*)&mstore::read_all_stored_msgs },
    { "_ZN6mstore23enumerate_message_filesEP11MsgFileInfoi", (void*)&mstore::enumerate_message_files },
    { "_ZN6mstore24offset_of_newest_recordsEPKcji",        (void*)&mstore::offset_of_newest_records },
    // Type-boundary bridges (definitions above the table).
    { "mcs_channel_msg_path",     (void*)mcs_channel_msg_path },
    { "mcs_dm_msg_path",          (void*)mcs_dm_msg_path },
    { "mcs_messages_dir_path",    (void*)mcs_messages_dir_path },
    { "mcs_append_extra_path",    (void*)mcs_append_extra_path },
    { "mcs_read_one_stored_msg",  (void*)mcs_read_one_stored_msg },
    // meshtastic-lite crypto seam (src/radio/proto_crypto.cpp): the vendor
    // headers' software-fallback externs, implemented host-side with the
    // firmware's mbedtls (hw AES) + esp_random TRNG.
    { "mesh_aes_block_encrypt", (void*)mesh_aes_block_encrypt },
    { "mesh_sha256",            (void*)mesh_sha256 },
    { "mesh_ccm_encrypt",       (void*)mesh_ccm_encrypt },
    { "mesh_ccm_decrypt",       (void*)mesh_ccm_decrypt },
    { "mesh_x25519_dh",         (void*)mesh_x25519_dh },
    { "mesh_generate_keypair",  (void*)mesh_generate_keypair },
    ELF_SYMBOL_END
};

static void* proto_seg_alloc(size_t size, void*) { return proto_pool_alloc(size); }
static void  proto_seg_free(void* p, void*)      { proto_pool_free(p); }

// Load one LoRa-protocol module from a drive-prefixed path
// ("L:/meshpunk/lora_protos/mtlite/mtlite.loraproto.elf"). Returns the
// module handle (NULL on any failure, reason logged) and the exported ops
// struct via out_ops. Boot context (setup(), before the mesh task exists).
// The loaded LoRa-protocol module (dependent BLE-protocol elfs resolve their
// leftover imports against its exports — see ble_import_fallback below).
static void* s_lora_proto_mod = nullptr;

void* elf_loraproto_load(const char* path, const void** out_ops) {
    *out_ops = NULL;
    uint32_t size = 0;
    void* buf = meshpunk_read_all(path, &size);
    if (!buf) { SLog.printf("[PROTO] read failed: %s\n", path); return NULL; }

    elf_module_t* mod = elf_load_ex(buf, size, proto_exports,
                                    proto_seg_alloc, proto_seg_free, NULL);
    heap_caps_free(buf);
    if (!mod) {
        SLog.printf("[PROTO] elf load failed (%s) — pool free %uB\n",
                    path, (unsigned)proto_pool_free_bytes());
        return NULL;
    }

    const void* ops = elf_lookup(mod, "loraproto_ops");
    if (!ops) {
        SLog.printf("[PROTO] no loraproto_ops export: %s\n", path);
        elf_unload(mod);
        return NULL;
    }
    s_lora_proto_mod = mod;

    // C++ statics with vtables need their constructors run (no crt in
    // module land); host-independent by contract — MeshHostApi arrives
    // later, at init().
    elf_run_ctors(mod);

    uint32_t ts = 0, te = 0;
    elf_text_range(mod, &ts, &te);
    SLog.printf("[PROTO] loaded %s text %08X-%08X pool %uB free\n",
                path, (unsigned)ts, (unsigned)te, (unsigned)proto_pool_free_bytes());
    *out_ops = ops;
    return mod;
}

void elf_loraproto_unload(void* mod) {
    if (!mod) return;
    if (mod == s_lora_proto_mod) s_lora_proto_mod = nullptr;
    elf_unload((elf_module_t*)mod);
    SLog.printf("[PROTO] unloaded (pool %uB free)\n", (unsigned)proto_pool_free_bytes());
}

// ── BLE-slot protocol modules (.bleproto.elf) ───────────────────────────────
// Same pool, same export table, plus ONE addition: unresolved imports fall
// back to the LOADED LoRa-protocol elf's own exports (instruction-side).
// That is how a coupled protocol (the meshcore companion importing PunkMesh)
// links against its LoRa protocol at load time — and how the dependency
// enforces itself: under any other LoRa protocol those imports miss and the
// load is refused, loudly. Standalone BLE protocols import nothing extra.

static void* ble_import_fallback(const char* name) {
    return s_lora_proto_mod
               ? elf_lookup_remapped((elf_module_t*)s_lora_proto_mod, name)
               : nullptr;
}

void* elf_bleproto_load(const char* path, const void** out_ops) {
    *out_ops = NULL;
    uint32_t size = 0;
    void* buf = meshpunk_read_all(path, &size);
    if (!buf) { SLog.printf("[BLEPROTO] read failed: %s\n", path); return NULL; }

    elf_set_symbol_fallback(ble_import_fallback);
    elf_module_t* mod = elf_load_ex(buf, size, proto_exports,
                                    proto_seg_alloc, proto_seg_free, NULL);
    elf_set_symbol_fallback(NULL);
    heap_caps_free(buf);
    if (!mod) {
        SLog.printf("[BLEPROTO] elf load failed (%s) — pool free %uB\n",
                    path, (unsigned)proto_pool_free_bytes());
        return NULL;
    }

    const void* ops = elf_lookup(mod, "bleproto_ops");
    if (!ops) {
        SLog.printf("[BLEPROTO] no bleproto_ops export: %s\n", path);
        elf_unload(mod);
        return NULL;
    }

    elf_run_ctors((elf_module_t*)mod);   // static vptrs (the 5f boot loop)

    SLog.printf("[BLEPROTO] loaded %s, pool %uB free\n",
                path, (unsigned)proto_pool_free_bytes());
    *out_ops = ops;
    return mod;
}

void elf_bleproto_unload(void* mod) {
    if (!mod) return;
    elf_unload((elf_module_t*)mod);
    SLog.printf("[BLEPROTO] unloaded (pool %uB free)\n",
                (unsigned)proto_pool_free_bytes());
}

// ── Screenshot capture ──────────────────────────────────────────────────────

// Copy one push into the staging buffer and note the rows it covered. Called
// from the module task (host_blit_*) and the blit task, never from both at
// once: a module hands over a frame through one path or the other.
static void shot_feed(const uint16_t* px, int x, int y, int w, int h, bool whole) {
    // Claim first, THEN re-test: shot_service clears s_shot_arming and waits
    // for this flag before handing the buffer to the writer task, so a push
    // that started before the hand-off finishes and one that starts after it
    // does nothing. Testing first would leave a window where both run.
    s_shot_feeding = true;
    if (s_shot_arming) {
        screenshot_feed_be565(px, x, y, w, h);
        for (int r = y; r < y + h; r++)
            if (r >= 0 && r < SHOT_MAX_ROWS)
                s_shot_rows[r >> 3] |= (uint8_t)(1 << (r & 7));
        if (whole) s_shot_full = true;
    }
    s_shot_feeding = false;
}

static void elf_shot_task(void* param) {
    (void)param;
    char path[96];
    if (screenshot_finish_to_disk(path, sizeof(path))) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Screenshot saved: %s", path);
        notify_post(msg);
        notify_kbd_blink();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Screenshot failed: %s", path);
        notify_post(msg);
    }
    s_shot_writing = false;
    vTaskDelete(nullptr);
}

// Drive one capture from the input task: claim the buffer on a request, then
// decide when enough of the frame has arrived and hand the write to its own
// task so the module keeps running.
static void shot_service() {
    if (s_shot_arming) {
        bool rows_done = true;
        int rows = display_dev_height();
        if (rows > SHOT_MAX_ROWS) rows = SHOT_MAX_ROWS;
        for (int r = 0; r < rows && rows_done; r++)
            if (!(s_shot_rows[r >> 3] & (1 << (r & 7)))) rows_done = false;
        bool timeout = (uint32_t)(millis() - s_shot_start_ms) >= SHOT_ACCUM_MS;
        if (!s_shot_full && !rows_done && !timeout) return;
        s_shot_arming = false;
        while (s_shot_feeding) vTaskDelay(1);   // let an in-flight push finish
        s_shot_writing = true;
        // Priority 1 and Core 1: below the mesh task, off the module's core.
        if (xTaskCreatePinnedToCore(elf_shot_task, "elf_shot", 6144, nullptr,
                                    1, nullptr, 1) != pdPASS) {
            s_shot_writing = false;
            screenshot_end();
            notify_post("Screenshot failed: no task memory");
        }
        return;
    }
    if (!s_shot_requested) return;
    if (s_shot_writing) {                // previous shot still going to disk
        s_shot_requested = false;
        s_shot_retrying  = false;
        return;
    }
    if (!screenshot_begin()) {
        if (!s_shot_retrying) { s_shot_retrying = true; s_shot_first_try = millis(); }
        if (millis() - s_shot_first_try < SHOT_RETRY_MS) return;   // keep the request
        s_shot_requested = false;
        s_shot_retrying  = false;
        notify_post(screenshot_busy() ? "Screenshot skipped: previous one still saving"
                                      : "Screenshot failed: low memory");
        return;
    }
    s_shot_requested = false;
    s_shot_retrying  = false;
    memset(s_shot_rows, 0, sizeof(s_shot_rows));
    s_shot_full = false;
    s_shot_start_ms = millis();
    s_shot_arming = true;
}

// ---------------------------------------------------------------------------
// Host function implementations
// ---------------------------------------------------------------------------

extern "C" {

// Count of fread/fwrite transfers we bounced because dst/src was in PSRAM.
static volatile uint32_t g_bounce_reads = 0;

void host_blit_frame(const uint16_t* rgb565, int w, int h) {
    int x_offset = (display_dev_width() - w) / 2;
    if (x_offset < 0) x_offset = 0;
    int y_offset = (display_dev_height() - h) / 2;
    if (y_offset < 0) y_offset = 0;
    // Screenshot: the module's pixels as it drew them, taken before the
    // stamps below write indicator pixels into this same buffer.
    if (s_shot_arming) shot_feed(rgb565, x_offset, y_offset, w, h, true);
    // Controller-mode indicators: stamped into the module's own buffer just
    // before the push (transparent — only outline/label pixels are written).
    // Writing into it is safe on this path — a full-frame renderer rewrites
    // every pixel next frame, so the stamp is transient (and idempotent for
    // a module re-pushing a static frame). Chip colors are byte-swap-
    // invariant, so byte-swapped module frames render them correctly too.
    if (s_zone_overlay_on) {
        zone_overlay_stamp((uint16_t*)rgb565, x_offset, y_offset, w, h);
        s_zone_fx = x_offset; s_zone_fy = y_offset;
        s_zone_fw = w;        s_zone_fh = h;
        s_zone_frame_seen = true;
    }
    // Open OSK: stamped INTO the buffer so one push carries game + keyboard
    // — a post-push re-push alternated game/keyboard pixels on the panel,
    // a visible blink at the module's frame rate. (The game deliberately
    // keeps rendering while the user types.)
    if (host_osk_active())
        host_osk_stamp((uint16_t*)rgb565, x_offset, y_offset, w, h);
    display_dev_blit(x_offset, y_offset, w, h, rgb565);
    // Chip parts in the letterbox borders never ride a frame — push them
    // once per screen-clear, AFTER the frame so a clear can't wipe them.
    if (s_zone_overlay_on && !s_zone_border_pushed) {
        s_zone_border_pushed = true;
        zone_overlay_push_outside(x_offset, y_offset, w, h, false,
                                  display_dev_blit);
    }
}

// Blit a rectangle at absolute screen coordinates. A module that composites
// in internal RAM pushes finished strips straight to the panel with this,
// so it needs no PSRAM frame buffer: that removes both the buffer write and
// the blit task's read-back of it. The bus lock is taken per call, so the
// radio and SD still get the SPI bus between strips.
void host_blit_rect(const uint16_t* rgb565, int x, int y, int w, int h) {
    if (!rgb565 || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 ||
        x + w > display_dev_width() || y + h > display_dev_height()) return;
    if (s_shot_arming) shot_feed(rgb565, x, y, w, h, false);
    // Controller-mode indicators: transparent-stamp the overlapping chip
    // parts into the strip before it goes out, exactly like the frame path
    // (band renderers re-rasterize every strip, so the stamp is transient).
    if (s_zone_overlay_on)
        zone_overlay_stamp((uint16_t*)rgb565, x, y, w, h);
    if (host_osk_active())
        host_osk_stamp((uint16_t*)rgb565, x, y, w, h);
    display_dev_blit(x, y, w, h, rgb565);
}

// ── Async blit ───────────────────────────────────────────────────────────────
// A Core-1 task owns the (blocking) SPI push so the module keeps running on
// Core 0 during the transfer. The module double-buffers and hands over a
// frame pointer; back-pressure is one frame deep.
//
// Priority 3 (tied with the sound task, above the mesh task at 2). At the old
// priority 2 the push lost CPU to both synth (3) and the radio task (2) and to
// the shared spi_bus_mutex, so an ~11.5 ms transfer couldn't finish inside a
// 50-72 ms Core-0 step — Core 0 then stalled ~a full transfer every frame
// (blitwait ~= 11.5 ms). At 3 it preempts the radio task and, via the mutex's
// priority inheritance, gets the SPI bus released to it sooner; tied with synth
// (which is mostly blocked waiting on I2S) keeps audio fed. DMA isn't an option
// here: the TFT shares one register-level SPI bus with the radio and SD, so
// TFT_eSPI initDMA() would install the esp-idf driver on it and break them.
// If audio underruns/crackles after this, drop back to 2.
static TaskHandle_t      s_blit_task = nullptr;
static SemaphoreHandle_t s_blit_idle = nullptr;   // given when no push in flight
static const uint16_t* volatile s_blit_buf = nullptr;
static volatile int s_blit_w = 0, s_blit_h = 0;
static volatile int s_blit_x = 0, s_blit_y = 0;
static volatile bool s_blit_is_rect = false;

static void blit_task_body(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t* buf = s_blit_buf;
        if (buf) {
            if (s_blit_is_rect)
                host_blit_rect(buf, s_blit_x, s_blit_y, s_blit_w, s_blit_h);
            else
                host_blit_frame(buf, s_blit_w, s_blit_h);
        }
        xSemaphoreGive(s_blit_idle);
    }
}

// Bring the push task up on first use. False means the async path is
// unavailable and the caller should push synchronously instead.
static bool blit_async_begin(void) {
    if (s_blit_task) return true;
    if (!s_blit_idle) {
        s_blit_idle = xSemaphoreCreateBinary();
        if (!s_blit_idle) return false;
        xSemaphoreGive(s_blit_idle);
    }
    xTaskCreatePinnedToCore(blit_task_body, "elf_blit", 4096, nullptr,
                            3, &s_blit_task, 1 /* Core 1 */);
    return s_blit_task != nullptr;
}

void host_blit_frame_async(const uint16_t* rgb565, int w, int h) {
    if (!blit_async_begin()) { host_blit_frame(rgb565, w, h); return; }
    // Wait until the previous frame is on the wire — after this the caller's
    // other buffer is free to render into.
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    s_blit_buf     = rgb565;
    s_blit_x       = 0;
    s_blit_y       = 0;
    s_blit_w       = w;
    s_blit_h       = h;
    s_blit_is_rect = false;
    xTaskNotifyGive(s_blit_task);
}

// Async form of host_blit_rect, for modules that rasterise a band at a time
// into internal RAM and double-buffer: hand one band over and rasterise the
// next into the other buffer while this one goes out on the wire. The push
// itself is CPU-driven (there is no DMA on this bus) but it spends nearly all
// of its time spinning on the SPI busy flag, so on Core 1 it costs the module's
// Core-0 rasteriser very little.
//
// Same one-deep back-pressure as host_blit_frame_async: this returns once the
// PREVIOUS push has finished, which is exactly when the buffer handed over
// before that one becomes safe to touch again. Two buffers are therefore
// enough, and the caller must alternate them.
void host_blit_rect_async(const uint16_t* rgb565, int x, int y, int w, int h) {
    if (!rgb565 || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > 320 || y + h > 240) return;
    if (!blit_async_begin()) { host_blit_rect(rgb565, x, y, w, h); return; }
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    s_blit_buf     = rgb565;
    s_blit_x       = x;
    s_blit_y       = y;
    s_blit_w       = w;
    s_blit_h       = h;
    s_blit_is_rect = true;
    xTaskNotifyGive(s_blit_task);
}

// Block until no push is in flight. A module MUST call this before freeing or
// reusing a buffer it handed to an async blit: the session-cleanup drain runs
// only after the module has already returned, which is too late for memory the
// module frees itself.
void host_blit_wait(void) {
    if (!s_blit_task || !s_blit_idle) return;
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    xSemaphoreGive(s_blit_idle);
}

// Wait for any in-flight async push. Must be called before the module's
// memory is unmapped — the blit task reads the frame straight from module BSS.
static void blit_drain(void) {
    if (!s_blit_task) return;
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    s_blit_buf = nullptr;
    xSemaphoreGive(s_blit_idle);
}

void host_clear_screen(void) {
    display_dev_fill_black();
    // The clear just wiped any border chips — repaint them on the next frame.
    s_zone_border_pushed = false;
    // An open OSK was wiped with them: repaint it whole, right now.
    if (host_osk_active())
        host_osk_maintain(0, 0, display_dev_width(), display_dev_height(),
                          display_dev_blit);
}

uint32_t host_get_ticks_ms(void) {
    return (uint32_t)millis();
}

uint32_t host_get_ticks_us(void) {
    return (uint32_t)micros();
}

void host_sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

int host_get_key(int* pressed, unsigned char* key) {
    // Normally the Core 1 input task fills the queue; only self-poll if that
    // task isn't running (spawn failed), so the queue never has two producers.
    if (!s_input_task) poll_input();
    KeyEvent ev;
    if (kq_pop(&ev)) {
        *pressed = ev.pressed;
        *key = ev.key;
        return 1;
    }
    return 0;
}

// Raw trackball deltas for modules that emulate a pointing device (PC-XT's
// serial mouse). First call opts the module in: the Core 1 input task stops
// consuming the ISR counters and stops emitting 0x81-0x85 pseudo-keys.
// Subtract-what-was-read (not =0) so ticks landing between the read and the
// write survive — same tolerance as the input task's own consumption.
// Live trackball button level (1 = pressed). The click ISR only counts press
// EDGES, so modules that want real press/release semantics (held mouse
// buttons, dragging) read the level here each poll instead of inferring a
// duration from the edge count.
int host_trackball_button(void) {
    return input_dev_nav_click_held() ? 1 : 0;
}

void host_trackball_read(int* dx, int* dy, int* click) {
    s_trk_raw_mode = true;
    int up = trackball_up;    trackball_up    -= up;
    int dn = trackball_down;  trackball_down  -= dn;
    int lt = trackball_left;  trackball_left  -= lt;
    int rt = trackball_right; trackball_right -= rt;
    int ck = trackball_click; trackball_click -= ck;
    if (dx)    *dx = rt - lt;
    if (dy)    *dy = dn - up;
    if (click) *click = ck;
}

// Track the sample rate across calls so we only reconfigure the mixer when
// the rate changes. Reset to 0 by the cleanup path after module exit so the
// next module's first push always sets the rate correctly.
static int s_audio_last_rate = 0;

void host_audio_push(const int16_t* samples, int count, int sample_rate) {
    // Route through the firmware's sound mixer — samples are mixed alongside
    // notification tones, with firmware volume/mute applied automatically.
    // Set the upsample factor so the mixer resamples correctly for this
    // module's rate (e.g. 11025 Hz, 22050 Hz, or 44100 Hz).
    if (sample_rate != s_audio_last_rate) {
        sound_extern_set_rate(sample_rate);
        s_audio_last_rate = sample_rate;
    }
    sound_extern_push(samples, count);
}

// Pull-model audio: the module registers a synth callback that the sound
// task (Core 1) invokes for exactly the samples the I2S pipeline needs,
// replacing per-frame host_audio_push() pacing from the game loop (Core 0).
// Constraints on the callback: it runs on the firmware's sound task, so it
// must not block and must not call the module's malloc/free (the alloc
// tracker is single-task). Pass cb=NULL to unregister; that blocks until
// the mixer is outside the callback, making it safe to free module memory.
void host_audio_set_pull(void (*cb)(int16_t* out, int count), int sample_rate) {
    // Relocations targeting .text arrive instruction-side (0x42/0x43) from
    // the loader, but remap defensively in case the pointer came through a
    // data-side path — a 0x3C/0x3D PSRAM alias is never executable.
    uint32_t addr = (uint32_t)cb;
    if (addr >= 0x3C000000 && addr < 0x3E000000)
        cb = (void (*)(int16_t*, int))(addr + 0x06000000);
    sound_extern_set_pull(cb, sample_rate);
    // Force the next host_audio_push() to reprogram the mixer rate —
    // unregistering resets the upsample factor behind s_audio_last_rate.
    s_audio_last_rate = 0;
}

// ---------------------------------------------------------------------------
// Module worker tasks. A module may spawn a small number of helper tasks
// (e.g. the NGPC Core-1 renderer). Handles are tracked so the session
// cleanup path can force-delete anything left running — a worker executing
// module code after the module's memory is freed would fault. Spawn and
// join are called from the module task only.
// ---------------------------------------------------------------------------
#define ELF_MAX_WORKERS 2

struct elf_worker {
    TaskHandle_t     task;      // nullptr = slot free
    void           (*fn)(void*);
    void            *arg;
    volatile bool    done;
};
static elf_worker s_elf_workers[ELF_MAX_WORKERS];

static void elf_worker_tramp(void* p) {
    elf_worker* w = (elf_worker*)p;
    w->fn(w->arg);
    w->done = true;
    vTaskSuspend(nullptr);   // parked here until join (or cleanup) deletes us
}

void* host_spawn_task(void (*fn)(void*), void* arg, int core, int prio, int stackkb) {
    // Function pointers can arrive data-side (0x3C..) from the loader's
    // relocations — remap to the instruction bus, same as host_audio_set_pull.
    uint32_t addr = (uint32_t)fn;
    if (addr >= 0x3C000000 && addr < 0x3E000000)
        fn = (void (*)(void*))(addr + 0x06000000);
    if (core < 0 || core > 1) core = 1;
    if (prio < 1) prio = 1;
    if (prio > 4) prio = 4;
    if (stackkb < 2) stackkb = 2;
    if (stackkb > 16) stackkb = 16;
    for (int i = 0; i < ELF_MAX_WORKERS; i++) {
        if (s_elf_workers[i].task) continue;
        s_elf_workers[i].fn   = fn;
        s_elf_workers[i].arg  = arg;
        s_elf_workers[i].done = false;
        BaseType_t ok = xTaskCreatePinnedToCore(elf_worker_tramp, "elf_worker",
                                                (uint32_t)stackkb * 1024,
                                                &s_elf_workers[i], prio,
                                                &s_elf_workers[i].task, core);
        if (ok != pdPASS) {
            s_elf_workers[i].task = nullptr;
            SLog.printf("[elf_host] worker spawn FAILED (stack=%dKB core=%d)\n",
                        stackkb, core);
            return nullptr;
        }
        SLog.printf("[elf_host] worker %d spawned (core=%d prio=%d stack=%dKB)\n",
                    i, core, prio, stackkb);
        return &s_elf_workers[i];
    }
    return nullptr;
}

// Wait for a worker's fn to return, then delete the task. timeout_ms < 0
// waits forever. Returns 0 on join, -1 on bad handle or timeout.
int host_task_join(void* handle, int timeout_ms) {
    elf_worker* w = (elf_worker*)handle;
    if (!w || w < s_elf_workers || w >= s_elf_workers + ELF_MAX_WORKERS || !w->task)
        return -1;
    uint32_t start = millis();
    while (!w->done) {
        if (timeout_ms >= 0 && (int)(millis() - start) > timeout_ms)
            return -1;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelete(w->task);
    w->task = nullptr;
    return 0;
}

// Session-cleanup safety net for the exit()-longjmp path. Returns how many
// workers had to be force-deleted.
static int elf_workers_cleanup(void) {
    int killed = 0;
    for (int i = 0; i < ELF_MAX_WORKERS; i++) {
        if (!s_elf_workers[i].task) continue;
        SLog.printf("[elf_host] force-deleting leftover worker %d\n", i);
        vTaskDelete(s_elf_workers[i].task);
        s_elf_workers[i].task = nullptr;
        killed++;
    }
    return killed;
}

int host_should_exit(void) {
    if (s_quit_requested) return 1;
    return (esc_held && (millis() - esc_hold_start > ESC_EXIT_HOLD_MS)) ? 1 : 0;
}

void* host_read_file(const char* path, uint32_t* out_size) {
    // Default to SD for ELF module file access
    return meshpunk_read_all(path, out_size, /*default_sd=*/true);
}

int host_write_file(const char* path, const void* data, uint32_t size) {
    MeshpunkFile mf = meshpunk_open(path, "w", /*default_sd=*/true);
    if (!mf.valid) {
        // First write into a directory that doesn't exist yet (e.g. a cart's
        // cdata/ folder) — create the parents and retry once.
        meshpunk_mkdirs(path, /*default_sd=*/true);
        mf = meshpunk_open(path, "w", /*default_sd=*/true);
        if (!mf.valid) return -1;
    }
    size_t written = mf.file.write((const uint8_t*)data, size);
    meshpunk_close(mf);
    return (written == size) ? 0 : -1;
}

void host_log(const char* msg) {
    SLog.printf("[elf_mod] %s\n", msg);
}

// Module-callable internal-heap integrity probe. Prints the tag FIRST, so if
// the heap is corrupt and the walk loops/faults (silent TG1WDT), the last
// "[heapchk] <tag>:" line on the wire names the step that did the damage.
void host_check_heap(const char* tag) {
    // Also report this task's remaining stack — if it plummets toward 0, the
    // module is overflowing the task stack (spilling into adjacent internal-RAM
    // heap = the "corruption").
    esp_rom_printf("[heapchk] %s (stk_free=%u): int=", tag ? tag : "?",
                   (unsigned)uxTaskGetStackHighWaterMark(NULL));
    // Print incrementally: if a walk hangs/faults on a corrupt heap, the last
    // token on the wire names which heap broke.
    bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, false);
    esp_rom_printf("%s psram=", ok ? "ok" : "CORRUPT");
    // The module heap (Lua's entire world) lives in PSRAM — this is the one
    // that matters for cart-corruption hunts.
    bool ok_ps = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false);
    esp_rom_printf("%s\n", ok_ps ? "ok" : "CORRUPT");
}

// File I/O wrappers for loaded modules.
//
// CRITICAL: SD transfers use SPI DMA, and on the ESP32-S3 DMA CANNOT target
// PSRAM. Modules that stream large files into PSRAM-allocated buffers via fread
// would have the SD DMA write to a bogus address, corrupting internal RAM (heap
// metadata, DMA descriptors). We bounce any PSRAM-destined read/write through
// an internal DMA-capable buffer so the DMA only ever touches internal RAM.
static inline bool elf_is_psram(const void* p) {
    uint32_t a = (uint32_t)p;
    return a >= 0x3C000000u && a < 0x3E000000u;   // PSRAM data bus window
}

static uint8_t* s_io_bounce = nullptr;            // lazily allocated, reused
#define ELF_IO_BOUNCE_SZ 8192

static uint8_t* elf_io_bounce() {
    if (!s_io_bounce)
        s_io_bounce = (uint8_t*)heap_caps_malloc(ELF_IO_BOUNCE_SZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return s_io_bounce;
}

// fread that never DMAs into PSRAM (called only by the loaded module, which is
// single-threaded on the elf_run task — so the shared bounce buffer is safe).
// SPI-locked file wrappers — SD card shares the SPI bus with the radio,
// so every file operation must hold the bus mutex.

// Module file descriptor tracker — same pattern as the PSRAM allocation tracker.
// Modules may keep files open for streaming reads; if the module exits via exit()
// or abort(), those handles leak and exhaust FATFS file descriptors.
#define MAX_MODULE_FILES 16
static FILE* s_mod_files[MAX_MODULE_FILES];
static int   s_mod_file_count = 0;

static void mod_track_file(FILE* f) {
    if (!f) return;
    if (s_mod_file_count < MAX_MODULE_FILES)
        s_mod_files[s_mod_file_count++] = f;
    else
        printf("[elf_host] WARNING: module file tracker full\n");
}

static void mod_untrack_file(FILE* f) {
    if (!f) return;
    for (int i = s_mod_file_count - 1; i >= 0; i--) {
        if (s_mod_files[i] == f) {
            s_mod_files[i] = s_mod_files[--s_mod_file_count];
            return;
        }
    }
}

static void mod_close_tracked_files() {
    int n = s_mod_file_count;
    for (int i = 0; i < n; i++) {
        if (s_mod_files[i])
            fclose(s_mod_files[i]);
    }
    s_mod_file_count = 0;
    if (n > 0)
        printf("[elf_host] closed %d leaked module file descriptors\n", n);
}

FILE* elf_fopen(const char* path, const char* mode) {
    SPI_LOCK();
    FILE* f = fopen(path, mode);
    SPI_UNLOCK();
    mod_track_file(f);
    return f;
}

// ── Protocol-module stdio (SPI-locked, UNTRACKED) ───────────────────────────
// Protocol modules run for the whole boot on the mesh task, with their data
// on the shared-SPI storage (SD when the user selected it) and their buffers
// in the PSRAM pool — so their stdio needs the same SPI locking (and fread/
// fwrite PSRAM bouncing) as game modules. They must NOT use the tracked
// fopen/fclose above: the tracking table feeds the game-exit leak sweep,
// which would close a protocol's file mid-write from another core.
// Protocols never unload, so there is nothing to sweep.
static FILE* proto_fopen(const char* path, const char* mode) {
    SPI_LOCK();
    FILE* f = fopen(path, mode);
    SPI_UNLOCK();
    return f;
}
static int proto_fclose(FILE* f) {
    SPI_LOCK();
    int r = fclose(f);
    SPI_UNLOCK();
    return r;
}
static char* proto_fgets(char* s, int n, FILE* f) {
    SPI_LOCK();
    char* r = fgets(s, n, f);
    SPI_UNLOCK();
    return r;
}
static int proto_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SPI_LOCK();
    int r = vfprintf(f, fmt, ap);
    SPI_UNLOCK();
    va_end(ap);
    return r;
}
static int proto_fputs(const char* s, FILE* f) {
    SPI_LOCK();
    int r = fputs(s, f);
    SPI_UNLOCK();
    return r;
}
static int proto_fflush(FILE* f) {
    SPI_LOCK();
    int r = fflush(f);
    SPI_UNLOCK();
    return r;
}
static int proto_remove(const char* path) {
    SPI_LOCK();
    int r = remove(path);
    SPI_UNLOCK();
    return r;
}
static int proto_rename(const char* a, const char* b) {
    SPI_LOCK();
    int r = rename(a, b);
    SPI_UNLOCK();
    return r;
}
static int proto_mkdir(const char* path) {
    SPI_LOCK();
    int r = mkdir(path, 0777);
    SPI_UNLOCK();
    return r;
}

// The firmware mesh mutex for protocol packages (their bindings run on Core 0
// against protocol state the mesh task mutates — same recursive mutex the
// firmware bindings take).
extern "C" void mesh_lock(void)   { MESH_LOCK(); }
extern "C" void mesh_unlock(void) { MESH_UNLOCK(); }

// ── meshcore package type-boundary bridges (mcs_*) ──────────────────────────
// The String/fs::FS-typed mstore calls re-expressed in C: the package's shim
// String/FS classes are NOT layout-compatible with the firmware's, so those
// values never cross raw. Paths cross as char*; the storage backend is the
// firmware's own (mstore::storage()).
extern "C" int mcs_channel_msg_path(const char* name, char* out, int out_sz) {
    String p = mstore::channel_msg_path_for(name);
    int n = snprintf(out, out_sz, "%s", p.c_str());
    return (n > 0 && n < out_sz) ? n : 0;
}
extern "C" int mcs_dm_msg_path(const char* peer, char* out, int out_sz) {
    String p = mstore::dm_msg_path_for(peer);
    int n = snprintf(out, out_sz, "%s", p.c_str());
    return (n > 0 && n < out_sz) ? n : 0;
}
extern "C" int mcs_messages_dir_path(char* out, int out_sz) {
    String p = mstore::messages_dir_path();
    int n = snprintf(out, out_sz, "%s", p.c_str());
    return (n > 0 && n < out_sz) ? n : 0;
}
extern "C" void mcs_append_extra_path(const char* msg_log_path,
                                      const uint8_t* hash,
                                      const ObservedPath* op) {
    if (!msg_log_path || !hash || !op) return;
    mstore::append_extra_path(String(msg_log_path), hash, *op);
}
extern "C" int mcs_read_one_stored_msg(const char* path, uint32_t offset,
                                       StoredMsg* m) {
    if (!path || !m) return 0;
    return mstore::read_one_stored_msg(mstore::storage(), path, offset, *m);
}

int elf_fclose(FILE* f) {
    mod_untrack_file(f);
    SPI_LOCK();
    int r = fclose(f);
    SPI_UNLOCK();
    return r;
}

int elf_fseek(FILE* f, long offset, int whence) {
    SPI_LOCK();
    int r = fseek(f, offset, whence);
    SPI_UNLOCK();
    return r;
}

long elf_ftell(FILE* f) {
    SPI_LOCK();
    long r = ftell(f);
    SPI_UNLOCK();
    return r;
}

size_t elf_fread(void* dst, size_t size, size_t nmemb, FILE* f) {
    size_t total = size * nmemb;
    uint8_t* bb;
    if (total == 0) return 0;
    if (!elf_is_psram(dst) || !(bb = elf_io_bounce())) {
        SPI_LOCK();
        size_t r = fread(dst, size, nmemb, f);
        SPI_UNLOCK();
        return r;
    }
    if (++g_bounce_reads == 1)
        esp_rom_printf("[elf_host] elf_fread: bouncing PSRAM reads (first dst=0x%x total=%u)\n",
                       (unsigned)(uint32_t)dst, (unsigned)total);
    uint8_t* d = (uint8_t*)dst;
    size_t done = 0;
    while (done < total) {
        size_t chunk = (total - done < ELF_IO_BOUNCE_SZ) ? (total - done) : ELF_IO_BOUNCE_SZ;
        SPI_LOCK();
        size_t r = fread(bb, 1, chunk, f);         // SD DMA lands in internal RAM
        SPI_UNLOCK();
        if (r) { memcpy(d + done, bb, r); done += r; }
        if (r < chunk) break;                      // short read / EOF
    }
    return (size ? done / size : 0);
}

size_t elf_fwrite(const void* src, size_t size, size_t nmemb, FILE* f) {
    size_t total = size * nmemb;
    uint8_t* bb;
    if (total == 0) return 0;
    if (!elf_is_psram(src) || !(bb = elf_io_bounce())) {
        SPI_LOCK();
        size_t r = fwrite(src, size, nmemb, f);
        SPI_UNLOCK();
        return r;
    }
    const uint8_t* s = (const uint8_t*)src;
    size_t done = 0;
    while (done < total) {
        size_t chunk = (total - done < ELF_IO_BOUNCE_SZ) ? (total - done) : ELF_IO_BOUNCE_SZ;
        memcpy(bb, s + done, chunk);
        SPI_LOCK();
        size_t w = fwrite(bb, 1, chunk, f);
        SPI_UNLOCK();
        done += w;
        if (w < chunk) break;
    }
    return (size ? done / size : 0);
}

// ---------------------------------------------------------------------------
// Module PSRAM allocation tracker.
// The loaded module allocates PSRAM through our psram_malloc/calloc/realloc
// wrappers.  When the module exits — often via exit() → longjmp, skipping
// normal cleanup — those allocations leak.  We track every psram_*
// allocation and bulk-free survivors after module exit.
// ---------------------------------------------------------------------------
// Allocation tracker — an open-addressing hash set in PSRAM. Lua-based
// modules (PICO-8) route their entire GC heap through these wrappers:
// tens of thousands of live allocations and thousands of track/untrack
// calls per frame, so both operations must be O(1). The previous linear
// array degraded to a full-table scan per free() once it filled, and
// printed a warning per allocation — both visibly dragged the frame rate.
#define MOD_ALLOC_HASH_SIZE 65536              // power of 2, 256KB PSRAM
#define MOD_ALLOC_MAX_LIVE  (MOD_ALLOC_HASH_SIZE / 2)
#define MOD_ALLOC_TOMB      ((void*)1)

static void**   s_mod_allocs = nullptr;        // hash table, lazily allocated
static int      s_mod_alloc_count = 0;         // live entries
static uint32_t s_mod_alloc_occupied = 0;      // live + tombstones
static bool     s_mod_tracking = false;
static bool     s_mod_track_warned = false;

static inline uint32_t mod_hash(void* p) {
    uint32_t x = (uint32_t)p;
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x & (MOD_ALLOC_HASH_SIZE - 1);
}

static void mod_track_insert(void* ptr) {
    uint32_t i = mod_hash(ptr);
    uint32_t first_tomb = UINT32_MAX;
    for (;;) {
        void* v = s_mod_allocs[i];
        if (v == NULL) {
            if (first_tomb != UINT32_MAX) {
                s_mod_allocs[first_tomb] = ptr;   // reuse tombstone
            } else {
                s_mod_allocs[i] = ptr;
                s_mod_alloc_occupied++;
            }
            s_mod_alloc_count++;
            return;
        }
        if (v == MOD_ALLOC_TOMB && first_tomb == UINT32_MAX) first_tomb = i;
        if (v == ptr) return;                     // already tracked
        i = (i + 1) & (MOD_ALLOC_HASH_SIZE - 1);
    }
}

// Tombstones eventually exhaust the NULL slots probes terminate on;
// rebuild when 3/4 of the table is live+tomb (live alone is capped at 1/2).
static void mod_track_rebuild(void) {
    if (s_mod_alloc_count == 0) {   // all tombstones — just wipe
        memset(s_mod_allocs, 0, MOD_ALLOC_HASH_SIZE * sizeof(void*));
        s_mod_alloc_occupied = 0;
        return;
    }
    void** live = (void**)heap_caps_malloc(
        (size_t)s_mod_alloc_count * sizeof(void*), MALLOC_CAP_SPIRAM);
    if (!live) {            // can't rebuild — stop tracking rather than risk
        s_mod_tracking = false;  // unterminated probe loops
        printf("[elf_host] WARNING: alloc tracker rebuild failed, tracking disabled\n");
        return;
    }
    int n = 0;
    for (uint32_t i = 0; i < MOD_ALLOC_HASH_SIZE; i++) {
        void* v = s_mod_allocs[i];
        if (v && v != MOD_ALLOC_TOMB) live[n++] = v;
    }
    memset(s_mod_allocs, 0, MOD_ALLOC_HASH_SIZE * sizeof(void*));
    s_mod_alloc_count = 0;
    s_mod_alloc_occupied = 0;
    for (int i = 0; i < n; i++) mod_track_insert(live[i]);
    heap_caps_free(live);
}

static void mod_track(void* ptr) {
    if (!s_mod_tracking || !ptr) return;
    if (!s_mod_allocs) {
        s_mod_allocs = (void**)heap_caps_calloc(
            MOD_ALLOC_HASH_SIZE, sizeof(void*), MALLOC_CAP_SPIRAM);
        if (!s_mod_allocs) {
            printf("[elf_host] WARNING: can't allocate alloc tracker\n");
            return;
        }
    }
    if (s_mod_alloc_count >= MOD_ALLOC_MAX_LIVE) {
        if (!s_mod_track_warned) {
            s_mod_track_warned = true;
            printf("[elf_host] WARNING: module alloc tracker full (%d live) — "
                   "further allocations leak if the module aborts\n", s_mod_alloc_count);
        }
        return;
    }
    if (s_mod_alloc_occupied > (MOD_ALLOC_HASH_SIZE / 4) * 3) {
        mod_track_rebuild();
        if (!s_mod_tracking) return;
    }
    mod_track_insert(ptr);
}

static void mod_untrack(void* ptr) {
    if (!ptr || !s_mod_allocs) return;
    uint32_t i = mod_hash(ptr);
    for (;;) {
        void* v = s_mod_allocs[i];
        if (v == NULL) return;                    // not tracked
        if (v == ptr) {
            s_mod_allocs[i] = MOD_ALLOC_TOMB;
            s_mod_alloc_count--;
            return;
        }
        i = (i + 1) & (MOD_ALLOC_HASH_SIZE - 1);
    }
}

static void mod_free_tracked() {
    int n = s_mod_alloc_count;
    if (s_mod_allocs) {
        for (uint32_t i = 0; i < MOD_ALLOC_HASH_SIZE; i++) {
            void* v = s_mod_allocs[i];
            if (v && v != MOD_ALLOC_TOMB)
                heap_caps_free(v);
        }
        heap_caps_free(s_mod_allocs);
        s_mod_allocs = nullptr;
    }
    s_mod_alloc_count = 0;
    s_mod_alloc_occupied = 0;
    s_mod_tracking = false;
    s_mod_track_warned = false;
    if (n > 0)
        printf("[elf_host] freed %d leaked module allocations\n", n);
}

// PSRAM-only memory allocation for loaded modules.
// All module memory goes to PSRAM — never internal RAM.
// Allocations are tracked so they can be freed on module exit.
void* psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mod_track(p);
    return p;
}

// Deliberate exception to the PSRAM-only rule above, for the one case where it
// pays: a SMALL, extremely hot structure that would otherwise fight the
// module's own multi-MB working set for the 32KB data cache. The DOS module's
// CPUI386 (~400 bytes) is touched several times by every emulated instruction
// while a 4MB guest RAM streams past it, so every eviction turns a register
// read into an 80MHz PSRAM round-trip.
//
// Internal RAM is the scarce pool (BLE, WiFi, TLS, task stacks all draw on it),
// so this is NOT for buffers — callers must keep it to a few hundred bytes and
// fall back gracefully. Returns NULL if internal RAM cannot satisfy the request;
// the caller is expected to retry with plain malloc rather than fail.
// Tracked like every other module allocation, so the leak sweep still frees it.
void* internal_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) mod_track(p);
    return p;
}

void* psram_calloc(size_t nmemb, size_t size) {
    void* p = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mod_track(p);
    return p;
}

void* psram_realloc(void* ptr, size_t size) {
    mod_untrack(ptr);
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) mod_track(p);
    // Failed grow: the old block is still live and the module keeps using it —
    // re-track it or it escapes the leak sweep if the module exits via longjmp.
    // (size == 0 means realloc freed the block; it must stay untracked.)
    else if (ptr && size) mod_track(ptr);
    return p;
}

void psram_free(void* ptr) {
    mod_untrack(ptr);
    free(ptr);
}

// Query how much PSRAM is available for the module to allocate.
uint32_t host_psram_largest_free(void) {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

// T-Deck peer link, gblink service — thin veneers over src/tdeck_link.cpp.
// Called from the module's Core-0 task; the bridge's rings make them cheap
// enough for gnuboy's per-instruction-batch polling.
int host_link_status(void) {
    return tdeck_link_status();
}

int host_link_gb_send(int cmd, int data_ctrl, unsigned int ts) {
    // data_ctrl = (BGB control byte << 8) | data byte; ts = 2MiHz timestamp.
    return tdeck_link_gb_send((uint8_t)cmd, (uint16_t)data_ctrl, ts) ? 1 : 0;
}

int host_link_gb_poll(unsigned int* ts_out) {
    return tdeck_link_gb_poll(ts_out);
}

int host_link_gb_wait(unsigned int timeout_ms) {
    return tdeck_link_gb_wait(timeout_ms);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Stubs for functions that don't make sense or are unsafe on ESP32
// ---------------------------------------------------------------------------
static FILE* stub_tmpfile(void) { return NULL; }
static char* stub_tmpnam(char* s) { (void)s; return NULL; }
static FILE* stub_freopen(const char* p, const char* m, FILE* f) {
    (void)p; (void)m; (void)f; return NULL;
}
static char* stub_getenv(const char* n) { (void)n; return NULL; }
static int   stub_atexit(void(*f)(void)) { (void)f; return 0; }

// ---------------------------------------------------------------------------
// Symbol export table — these are the functions the loaded ELF can call
// ---------------------------------------------------------------------------

static const elf_symbol_t host_exports[] = {
    { "host_blit_frame",    (void*)host_blit_frame },
    { "host_blit_frame_async", (void*)host_blit_frame_async },
    { "host_blit_rect",     (void*)host_blit_rect },
    { "host_blit_rect_async", (void*)host_blit_rect_async },
    { "host_blit_wait",     (void*)host_blit_wait },
    { "host_clear_screen",  (void*)host_clear_screen },
    { "host_get_ticks_ms",  (void*)host_get_ticks_ms },
    { "host_get_ticks_us",  (void*)host_get_ticks_us },
    { "host_sleep_ms",      (void*)host_sleep_ms },
    { "host_get_key",       (void*)host_get_key },
    { "host_trackball_read", (void*)host_trackball_read },
    { "host_trackball_button", (void*)host_trackball_button },
    { "host_key_mods",         (void*)host_key_mods },
    { "host_kb_blink",         (void*)host_kb_blink },
    { "host_audio_push",    (void*)host_audio_push },
    { "host_audio_set_pull", (void*)host_audio_set_pull },
    { "host_spawn_task",    (void*)host_spawn_task },
    { "host_task_join",     (void*)host_task_join },
    { "host_should_exit",   (void*)host_should_exit },
    { "host_read_file",     (void*)host_read_file },
    { "host_write_file",    (void*)host_write_file },
    { "host_log",           (void*)host_log },
    { "host_check_heap",    (void*)host_check_heap },
    { "host_link_status",   (void*)host_link_status },
    { "host_link_gb_send",  (void*)host_link_gb_send },
    { "host_link_gb_poll",  (void*)host_link_gb_poll },
    { "host_link_gb_wait",  (void*)host_link_gb_wait },

    // C library: stdio
    { "printf",             (void*)printf },
    { "fprintf",            (void*)fprintf },
    { "sprintf",            (void*)sprintf },
    { "snprintf",           (void*)snprintf },
    { "vsnprintf",          (void*)vsnprintf },
    { "vfprintf",           (void*)vfprintf },
    { "puts",               (void*)puts },
    { "putchar",            (void*)putchar },
    { "fputc",              (void*)fputc },
    { "fputs",              (void*)fputs },
    { "fopen",              (void*)elf_fopen },   // SPI-locked
    { "fclose",             (void*)elf_fclose },  // SPI-locked
    { "fread",              (void*)elf_fread },   // SPI-locked + PSRAM bounce
    { "fwrite",             (void*)elf_fwrite },  // SPI-locked + PSRAM bounce
    { "fseek",              (void*)elf_fseek },   // SPI-locked
    { "ftell",              (void*)elf_ftell },   // SPI-locked
    { "fflush",             (void*)fflush },
    { "sscanf",             (void*)sscanf },
    { "fscanf",             (void*)fscanf },
    { "fgets",              (void*)fgets },
    { "getc",               (void*)getc },
    { "ungetc",             (void*)ungetc },
    { "feof",               (void*)feof },
    { "ferror",             (void*)ferror },
    { "clearerr",           (void*)clearerr },
    { "setvbuf",            (void*)setvbuf },
    { "remove",             (void*)remove },
    { "rename",             (void*)rename },
    // ESP-IDF's FAT VFS implements this; without it a module that has to
    // shrink a file must copy the part it keeps to a temp file and swap --
    // which after a big DOS install meant rewriting every file that had
    // sector padding, on the exit path, with the user watching.
    { "truncate",           (void*)truncate },
    { "tmpfile",            (void*)stub_tmpfile },
    { "tmpnam",             (void*)stub_tmpnam },
    { "freopen",            (void*)stub_freopen },

    // C library: string
    { "memcpy",             (void*)memcpy },
    { "memset",             (void*)memset },
    { "memmove",            (void*)memmove },
    { "memcmp",             (void*)memcmp },
    { "strcmp",              (void*)strcmp },
    { "strncmp",            (void*)strncmp },
    { "strcasecmp",         (void*)strcasecmp },
    { "strncasecmp",        (void*)strncasecmp },
    { "strlen",             (void*)strlen },
    { "strcpy",             (void*)strcpy },
    { "strncpy",            (void*)strncpy },
    { "strcat",             (void*)strcat },
    { "strchr",             (void*)strchr },
    { "strrchr",            (void*)strrchr },
    { "strstr",             (void*)strstr },
    { "strdup",             (void*)strdup },
    { "strtol",             (void*)strtol },
    { "strtoul",            (void*)strtoul },
    { "strtod",             (void*)strtod },
    { "atoi",               (void*)atoi },
    { "atof",               (void*)atof },
    { "memchr",             (void*)memchr },
    { "strpbrk",            (void*)strpbrk },
    { "strspn",             (void*)strspn },
    { "strcspn",            (void*)strcspn },
    { "strcoll",            (void*)strcoll },
    { "strerror",           (void*)strerror },
    { "strtok",             (void*)strtok },

    // C library: memory (PSRAM-aware wrappers)
    { "malloc",             (void*)psram_malloc },
    { "host_malloc_internal", (void*)internal_malloc },
    { "free",               (void*)psram_free },
    { "calloc",             (void*)psram_calloc },
    { "realloc",            (void*)psram_realloc },
    { "host_psram_largest_free", (void*)host_psram_largest_free },

    // C library: math/utility
    { "abs",                (void*)(int(*)(int))abs },
    { "qsort",              (void*)qsort },
    { "rand",               (void*)rand },
    { "srand",              (void*)srand },

    // C library: math (float + double)
    { "sinf",               (void*)sinf },
    { "cosf",               (void*)cosf },
    { "tanf",               (void*)tanf },
    { "asinf",              (void*)asinf },
    { "acosf",              (void*)acosf },
    { "atan2f",             (void*)atan2f },
    { "powf",               (void*)powf },
    { "sqrtf",              (void*)sqrtf },
    { "fmodf",              (void*)fmodf },
    { "fabsf",              (void*)fabsf },
    { "floorf",             (void*)floorf },
    { "ceilf",              (void*)ceilf },
    { "roundf",             (void*)roundf },
    { "logf",               (void*)logf },
    { "log2f",              (void*)log2f },
    { "log10f",             (void*)log10f },
    { "expf",               (void*)expf },
    { "exp2f",              (void*)exp2f },
    { "sin",                (void*)(double(*)(double))sin },
    { "cos",                (void*)(double(*)(double))cos },
    { "tan",                (void*)(double(*)(double))tan },
    { "asin",               (void*)(double(*)(double))asin },
    { "acos",               (void*)(double(*)(double))acos },
    { "atan2",              (void*)(double(*)(double,double))atan2 },
    { "pow",                (void*)(double(*)(double,double))pow },
    { "sqrt",               (void*)(double(*)(double))sqrt },
    { "fmod",               (void*)(double(*)(double,double))fmod },
    { "fabs",               (void*)(double(*)(double))fabs },
    { "floor",              (void*)(double(*)(double))floor },
    { "ceil",               (void*)(double(*)(double))ceil },
    { "round",              (void*)(double(*)(double))round },
    { "log",                (void*)(double(*)(double))log },
    { "log2",               (void*)(double(*)(double))log2 },
    { "log10",              (void*)(double(*)(double))log10 },
    { "exp",                (void*)(double(*)(double))exp },
    { "ldexp",              (void*)(double(*)(double,int))ldexp },
    { "ldexpf",             (void*)ldexpf },
    { "frexp",              (void*)(double(*)(double,int*))frexp },
    { "frexpf",             (void*)frexpf },
    { "modf",               (void*)(double(*)(double,double*))modf },

    // C library: ctype
    { "_ctype_",            (void*)_ctype_ },
    { "toupper",            (void*)toupper },
    { "tolower",            (void*)tolower },
    { "isalpha",            (void*)isalpha },
    { "isdigit",            (void*)isdigit },
    { "isalnum",            (void*)isalnum },
    { "isspace",            (void*)isspace },
    { "isupper",            (void*)isupper },
    { "islower",            (void*)islower },
    { "ispunct",            (void*)ispunct },
    { "isxdigit",           (void*)isxdigit },
    { "isgraph",            (void*)isgraph },
    { "iscntrl",            (void*)iscntrl },
    { "isprint",            (void*)isprint },

    // C library: POSIX
    { "mkdir",              (void*)mkdir },
    { "system",             (void*)system },

    // C library: newlib internals
    { "__errno",            (void*)__errno },
    { "__getreent",         (void*)__getreent },

    // GCC soft-float / 64-bit math builtins
    { "__divdi3",           (void*)__divdi3 },
    { "__divsf3",           (void*)__divsf3 },
    { "__extendsfdf2",      (void*)__extendsfdf2 },
    { "__ltdf2",            (void*)__ltdf2 },
    { "__truncdfsf2",       (void*)__truncdfsf2 },
    // Double-precision builtins
    { "__adddf3",           (void*)__adddf3 },
    { "__subdf3",           (void*)__subdf3 },
    { "__muldf3",           (void*)__muldf3 },
    { "__divdf3",           (void*)__divdf3 },
    { "__gtdf2",            (void*)__gtdf2 },
    { "__gedf2",            (void*)__gedf2 },
    { "__ledf2",            (void*)__ledf2 },
    { "__nedf2",            (void*)__nedf2 },
    { "__eqdf2",            (void*)__eqdf2 },
    { "__floatsidf",        (void*)__floatsidf },
    { "__floatunsidf",      (void*)__floatunsidf },
    { "__fixdfsi",          (void*)__fixdfsi },
    { "__fixunsdfsi",       (void*)__fixunsdfsi },
    { "__fixdfdi",          (void*)__fixdfdi },
    { "__floatdidf",        (void*)__floatdidf },
    { "__udivdi3",          (void*)__udivdi3 },
    { "__umoddi3",          (void*)__umoddi3 },
    { "__moddi3",           (void*)__moddi3 },

    // C library: time
    { "time",               (void*)time },
    { "clock",              (void*)clock },
    { "mktime",             (void*)mktime },
    { "gmtime",             (void*)gmtime },
    { "localtime",          (void*)localtime },
    { "difftime",           (void*)difftime },
    { "strftime",           (void*)strftime },

    // C library: misc stubs
    { "getenv",             (void*)stub_getenv },
    { "atexit",             (void*)stub_atexit },
    { "setlocale",          (void*)setlocale },
    { "localeconv",         (void*)localeconv },

    // C library: setjmp (for exit() trap)
    { "setjmp",             (void*)setjmp },
    { "longjmp",            (void*)longjmp },

    ELF_SYMBOL_END
};

// ---------------------------------------------------------------------------
// Dedicated execution task for loaded modules.
//
// Heavy native modules use far more stack than the 16KB Arduino
// loopTask provides — and _launch_elf is itself invoked deep inside the Lua
// VM's C call chain, so even less is actually free. Overflowing the loopTask
// stack corrupts it; the CPU then faults and the panic handler cannot unwind
// the trashed stack, so instead of a Guru Meditation backtrace we get a
// silent TG1 (interrupt) watchdog reset. Running the module on its own large,
// freshly-allocated stack removes that failure mode entirely.
// ---------------------------------------------------------------------------

// Module task stack. Internal RAM, not PSRAM: a task stack must stay
// accessible while the cache is disabled (e.g. during flash writes) and the
// module does file I/O. We try 64KB first for maximum headroom, falling back
// to 48KB or 32KB if not enough contiguous internal RAM is available.
// A launcher's "-stackkb N" caps this ladder at N (elf_host_run_pending).
// (ESP-IDF stack sizes are in bytes — StackType_t is 1 byte.)
static const uint32_t ELF_TASK_STACK_CANDIDATES[] = { 64*1024, 48*1024, 32*1024 };

struct elf_run_ctx {
    elf_module_t*     mod;
    int               argc;
    char**            argv;
    int               result;
    SemaphoreHandle_t done;   // given by the module task when it returns
};

// ---------------------------------------------------------------------------
// CPU-exception interception for loaded modules.
//
// The default ESP-IDF dual-core panic handler deadlocks in this context — it
// hangs in panic_handler.c (stalling the other core) and the raw TG1 hardware
// watchdog then resets the chip before any Guru Meditation backtrace prints.
// So a fault inside the module is invisible: all we see is "TG1WDT_SYS_RST".
//
// While the module runs we install our own handler for the common fatal
// exception causes. It prints the exact cause / PC / faulting address via the
// ROM console (the same path the boot log uses, so it reaches the USB console)
// then spins so the watchdog resets cleanly — at least now WITH the diagnostic
// info on the wire. This pinpoints a bad relocation or wild pointer.
//
// XtExcFrame, xt_exc_handler and xt_set_exception_handler come from
// <xtensa/xtensa_api.h>, already pulled in transitively via FreeRTOS.h.
// ---------------------------------------------------------------------------

// Xtensa EXCCAUSE values a misbehaving native module is likely to trigger.
// (Excludes 1=Syscall and 4=Level1Interrupt — those are NORMAL operations and
// hooking them would break the system.) Includes the PIF/cache-bus error causes
// 12-15, which PSRAM access can raise and which we were previously missing.
static const int kElfFaultCauses[] = {
    0,   // IllegalInstruction
    2,   // InstructionFetchError
    3,   // LoadStoreError
    6,   // IntegerDivideByZero
    8,   // Privileged
    9,   // LoadStoreAlignment
    12,  // InstrPIFDataError
    13,  // LoadStorePIFDataError
    14,  // InstrPIFAddrError
    15,  // LoadStorePIFAddrError
    20,  // InstrFetchProhibited
    26,  // LoadStorePrivilege
    28,  // LoadProhibited
    29,  // StoreProhibited
};
#define ELF_FAULT_NCAUSES (sizeof(kElfFaultCauses) / sizeof(kElfFaultCauses[0]))

static void IRAM_ATTR elf_exception_handler(XtExcFrame* f) {
    esp_rom_printf("\n\n*** [ELF FAULT] core=%d exccause=%d PC=0x%x excvaddr=0x%x PS=0x%x\n",
                   (int)xPortGetCoreID(), (int)f->exccause, (unsigned)f->pc,
                   (unsigned)f->excvaddr, (unsigned)f->ps);
    esp_rom_printf("*** a0 =0x%x a1 =0x%x a2 =0x%x a3 =0x%x\n",
                   (unsigned)f->a0, (unsigned)f->a1, (unsigned)f->a2, (unsigned)f->a3);
    esp_rom_printf("*** a4 =0x%x a5 =0x%x a6 =0x%x a7 =0x%x\n",
                   (unsigned)f->a4, (unsigned)f->a5, (unsigned)f->a6, (unsigned)f->a7);
    esp_rom_printf("*** a8 =0x%x a9 =0x%x a10=0x%x a11=0x%x\n",
                   (unsigned)f->a8, (unsigned)f->a9, (unsigned)f->a10, (unsigned)f->a11);
    esp_rom_printf("*** a12=0x%x a13=0x%x a14=0x%x a15=0x%x\n",
                   (unsigned)f->a12, (unsigned)f->a13, (unsigned)f->a14, (unsigned)f->a15);
    // Raw stack words near SP — code-looking values (0x4000../0x4037../0x42....)
    // can be addr2line'd to reconstruct the interrupt call chain.
    const uint32_t* sp = (const uint32_t*)f->a1;
    for (int i = 0; i < 32; i += 4) {
        esp_rom_printf("*** sp+%02d: 0x%x 0x%x 0x%x 0x%x\n", i * 4,
                       (unsigned)sp[i], (unsigned)sp[i+1], (unsigned)sp[i+2], (unsigned)sp[i+3]);
    }
    esp_rom_printf("*** spinning; interrupt watchdog will reset shortly\n");
    // MUST NOT return: a returning handler retries the faulting instruction,
    // which would just re-fault forever. Spin until the watchdog resets.
    while (1) { }
}

// Exception handlers are PER-CORE. The module runs on Core 0, but DMA/interrupt
// faults can land on Core 1 (radio SPI, etc.) — those escape a Core-0-only
// handler and hit the deadlocking default panic handler (silent TG1WDT). So we
// install on BOTH cores. Saved originals are restored on clean module exit.
static xt_exc_handler s_saved_exc[2][ELF_FAULT_NCAUSES];

static void elf_install_handlers(int core) {
    for (size_t i = 0; i < ELF_FAULT_NCAUSES; i++)
        s_saved_exc[core][i] = xt_set_exception_handler(kElfFaultCauses[i], elf_exception_handler);
}
static void elf_restore_handlers(int core) {
    for (size_t i = 0; i < ELF_FAULT_NCAUSES; i++)
        xt_set_exception_handler(kElfFaultCauses[i], s_saved_exc[core][i]);
}

// One-shot helper task to (un)install handlers on Core 1 (handlers install on
// the core that calls xt_set_exception_handler).
struct elf_c1_op { bool install; SemaphoreHandle_t done; };
static void elf_c1_op_task(void* arg) {
    elf_c1_op* op = (elf_c1_op*)arg;
    if (op->install) elf_install_handlers(1); else elf_restore_handlers(1);
    xSemaphoreGive(op->done);
    vTaskDelete(NULL);
}
static void elf_set_core1_handlers(bool install) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return;
    elf_c1_op op = { install, done };
    TaskHandle_t t = nullptr;
    bool ok = (xTaskCreatePinnedToCore(elf_c1_op_task, "elf_c1op", 2560, &op, 10, &t, 1) == pdPASS);
    if (ok) xSemaphoreTake(done, portMAX_DELAY);
    SLog.printf("[elf_host] Core1 fault handlers %s (%s)\n",
                  install ? "installed" : "restored", ok ? "ok" : "TASK-CREATE-FAILED");
    vSemaphoreDelete(done);
}

static void elf_run_task(void* param) {
    elf_run_ctx* ctx = (elf_run_ctx*)param;

    // Report current SP so a fault address can be compared against the stack range.
    uint32_t sp_top = (uint32_t)__builtin_frame_address(0);
    SLog.printf("[elf_host] elf_run task started, SP~0x%x\n", sp_top);

    // Intercept module faults on Core 0 (Core 1 is set up separately by caller).
    elf_install_handlers(0);

    ctx->result = elf_run(ctx->mod, ctx->argc, ctx->argv);

    // Clean return — restore the normal handlers.
    elf_restore_handlers(0);

    // High-water mark tells us how close we came to overflowing (for tuning).
    SLog.printf("[elf_host] module task finished (result=%d), min stack free: %u bytes\n",
                  ctx->result, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// _launch_elf(path [, arg1, arg2, ...])  — DEFERRED launch.
//
// The binding is called from a Lua game, so we cannot tear Lua down here (Lua is
// on the C stack). It deep-copies the args — the Lua strings will NOT survive the
// lua_close — sets s_elf_pending, and returns. The main loop (Core 0) then sees
// the flag, calls luaTearDown() (freeing the whole fragmented Lua heap so the
// module gets a clean contiguous PSRAM block), runs the module via
// elf_host_run_pending(), and recreates Lua + the launcher. Because Lua is gone
// before the module runs, _launch_elf returns no meaningful result — the user
// lands on the launcher home when the module exits.
// ---------------------------------------------------------------------------
#define ELF_MAX_ARGS 16
#define ELF_ARG_MAX  256
static volatile bool s_elf_pending = false;
static volatile bool s_elf_running = false;
static int  s_elf_argc = 0;
static char s_elf_args[ELF_MAX_ARGS][ELF_ARG_MAX];

static int lua_launch_elf(lua_State* L) {
    if (s_elf_pending || s_elf_running) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "launch already in progress");
        return 2;
    }
    int nargs = lua_gettop(L);
    if (nargs < 1) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no ELF path");
        return 2;
    }
    if (nargs > ELF_MAX_ARGS) nargs = ELF_MAX_ARGS;
    s_elf_argc = nargs;
    for (int i = 0; i < nargs; i++) {
        const char* a = lua_tostring(L, i + 1);
        strlcpy(s_elf_args[i], a ? a : "", ELF_ARG_MAX);
    }
    s_elf_pending = true;
    lua_pushboolean(L, 1);   // queued; the real load happens after teardown
    lua_pushinteger(L, 0);
    return 2;
}

// Called from loop() (Core 0): true if a launch was requested; marks it running
// so a second tap is ignored until the module exits.
bool elf_host_pending_take(void) {
    if (!s_elf_pending) return false;
    s_elf_pending = false;
    s_elf_running = true;
    return true;
}

// Run the stashed module to completion. MUST be called only after luaTearDown()
// — Lua is down and this never touches lua_State. The caller recreates Lua after
// it returns. Returns the module result (negative on load/spawn failure).
int elf_host_run_pending(void) {
    const char* path = s_elf_args[0];
    int argc = s_elf_argc;
    char* argv[ELF_MAX_ARGS + 1];
    for (int i = 0; i < argc; i++) argv[i] = s_elf_args[i];
    argv[argc] = NULL;

    SLog.printf("[elf_host] loading %s\n", path);
    SLog.printf("[elf_host] PSRAM free: %u, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Read ELF file from SD
    uint32_t elf_size = 0;
    void* elf_data = host_read_file(path, &elf_size);
    if (!elf_data) {
        SLog.printf("[elf_host] failed to read ELF file: %s\n", path);
        // Launch failures are otherwise INVISIBLE (Lua is torn down, the app
        // "just closes") — surface every one through the notification bell.
        {
            const char* base = strrchr(path, '/');
            base = base ? base + 1 : path;
            char msg[160];   // notify.cpp truncates to its slot size anyway
            snprintf(msg, sizeof(msg), "App launch failed: can't read %s", base);
            notify_post(msg);
        }
        s_elf_running = false;
        return -3;
    }
    SLog.printf("[elf_host] read %u bytes\n", elf_size);

    // Suspend LVGL, mesh task, BLE companion, and watchdog for module execution.
    lvgl_ticker.detach();
    // Widen the task watchdog timeout so long module init doesn't trip it.
    // Also set panic=false so even if it triggers, it just warns instead of rebooting.
    esp_task_wdt_init(120, false);
    // Disable the interrupt watchdog (TG1 MWDT). The first 3D frame from PSRAM
    // with cold caches can exceed the default 300ms timeout.
    {
        uint32_t before = REG_READ(TIMG_WDTCONFIG0_REG(1));
        REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0x50D83AA1);   // unlock write-protect
        REG_WRITE(TIMG_WDTCONFIG0_REG(1), before & ~(1U << 31));  // clear EN bit 31
        uint32_t after = REG_READ(TIMG_WDTCONFIG0_REG(1));
        REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0);            // re-lock
        SLog.printf("[elf_host] TG1 WDT: before=0x%08x after=0x%08x\n", before, after);
    }
    // Sound stays active — module audio is routed through the firmware's mixer
    // via host_audio_push() → sound_extern_push().
    elf_set_core1_handlers(true); // also catch module faults that land on Core 1
    host_clear_screen();

    // Reset input state and load keymap
    memset(prev_key_state, 0, INPUT_STATE_SIZE);
    kq_head = kq_tail = 0;
    esc_held = false;
    s_kb_toggle_enabled = false;   // opt-in: set below by -kbtoggle
    s_kb_layer_on = true;
    s_trk_raw_mode = false; // modules opt in via host_trackball_read()

    // Check for -keymap argument; default is passthrough (raw key codes).
    // Modules that need translated keycodes (e.g. Doom) pass their own
    // keymap string from their Lua launcher.
    const char* keymap_str = NULL;
    const char* trkball_str = NULL;
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "-keymap") == 0) {
            keymap_str = argv[i + 1];
        } else if (strcmp(argv[i], "-trkball") == 0) {
            trkball_str = argv[i + 1];
        } else if (strcmp(argv[i], "-kbtoggle") == 0) {
            // Opt in to the ALT+ENTER binding-layer chord; the value is the
            // layer's initial state (0 = start typing, 1 = start bound).
            s_kb_toggle_enabled = true;
            s_kb_layer_on = (atoi(argv[i + 1]) != 0);
        }
    }
    if (s_kb_toggle_enabled)
        SLog.printf("[elf_host] ALT+Enter binding toggle enabled (start %s)\n",
                    s_kb_layer_on ? "bound" : "typing");
    if (keymap_str && strcmp(keymap_str, "passthrough") == 0) {
        keymap_passthrough = true;
        memset(keymap_table, 0, sizeof(keymap_table));
        SLog.println("[elf_host] keymap: passthrough (raw key codes)");
    } else if (keymap_str) {
        keymap_passthrough = false;
        parse_keymap_arg(keymap_str);
        SLog.printf("[elf_host] keymap: custom (%d chars)\n", (int)strlen(keymap_str));
    } else {
        // No -keymap argument: default to passthrough
        keymap_passthrough = true;
        memset(keymap_table, 0, sizeof(keymap_table));
        SLog.println("[elf_host] keymap: passthrough (default)");
    }

    // Parse trackball momentum settings: "enabled,impulse*10,friction*100,threshold*10"
    // e.g. "1,15,82,4" → enabled=true, impulse=1.5, friction=0.82, threshold=0.4
    trk_vel_x = trk_vel_y = 0;
    if (trkball_str) {
        int en = 1, imp = 15, fri = 82, thr = 4;
        sscanf(trkball_str, "%d,%d,%d,%d", &en, &imp, &fri, &thr);
        trk_momentum = (en != 0);
        trk_impulse  = imp / 10.0f;
        trk_friction = fri / 100.0f;
        trk_thresh   = thr / 10.0f;
        SLog.printf("[elf_host] trackball: momentum=%d impulse=%.1f friction=%.2f thresh=%.1f\n",
                      trk_momentum, trk_impulse, trk_friction, trk_thresh);
    } else {
        // Defaults
        trk_momentum = true;
        trk_impulse  = 1.5f;
        trk_friction = 0.82f;
        trk_thresh   = 0.4f;
    }

    // Start tracking module PSRAM allocations so we can free them on exit
    s_mod_alloc_count = 0;
    s_mod_tracking = true;

    // Load and relocate
    elf_module_t* mod = elf_load(elf_data, elf_size, host_exports);
    heap_caps_free(elf_data); // raw ELF data no longer needed
    elf_data = NULL;

    int result = -1;
    if (mod) {
        SLog.printf("[elf_host] heap OK before run: %s\n",
                      heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? "yes" : "NO!");

        // Run the module on a dedicated large-stack task pinned to Core 0
        // (the UI core; LVGL is idle and Lua is torn down, so Core 0 is
        // free). The mesh task keeps running on Core 1 the whole time —
        // messages still arrive, persist, and raise the C-side notification
        // alert (notify.cpp). This loopTask blocks until the module returns —
        // IDLE0 still runs between the module's per-frame yields, feeding the
        // watchdog.
        // Try progressively smaller stacks until one fits in available RAM.
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        elf_run_ctx ctx = { mod, argc, argv, -1, done };
        TaskHandle_t task = nullptr;
        BaseType_t ok = pdFAIL;
        uint32_t stack_used = 0;
        if (done) {
            // Per-app ceiling: "-stackkb N" (from the app's launcher) caps the
            // ladder at N for THIS launch only, then descends through the
            // built-in rungs smaller than N. This stack is internal SRAM, the
            // same pool a module's own worker tasks draw on, so a module that
            // declares its depth is not handed more than it asked for.
            // Values outside 16..64 are ignored: the built-in ladder applies.
            uint32_t req_stack = 0;
            for (int i = 0; i < argc - 1; i++) {
                if (strcmp(argv[i], "-stackkb") == 0) {
                    int kb = atoi(argv[i + 1]);
                    if (kb >= 16 && kb <= 64) req_stack = (uint32_t)kb * 1024;
                }
            }
            uint32_t ladder[1 + (sizeof(ELF_TASK_STACK_CANDIDATES) /
                                 sizeof(ELF_TASK_STACK_CANDIDATES[0]))];
            int nladder = 0;
            if (req_stack) {
                ladder[nladder++] = req_stack;
                SLog.printf("[elf_host] stack ceiling %uKB (launcher -stackkb)\n",
                            (unsigned)(req_stack / 1024));
            }
            for (uint32_t c : ELF_TASK_STACK_CANDIDATES)
                if (!req_stack || c < req_stack) ladder[nladder++] = c;

            for (int ci = 0; ci < nladder; ci++) {
                uint32_t candidate = ladder[ci];
                ok = xTaskCreatePinnedToCore(
                    elf_run_task, "elf_run", candidate,
                    &ctx, 1 /* priority == loopTask */, &task, 0 /* Core 0 */);
                if (ok == pdPASS) {
                    stack_used = candidate;
                    SLog.printf("[elf_host] task created with %uKB stack\n", candidate / 1024);
                    break;
                }
                SLog.printf("[elf_host] %uKB stack failed, trying smaller...\n", candidate / 1024);
            }
        }
        if (ok == pdPASS) {
            // Start the Core 1 keyboard sampler AFTER the big stack landed:
            // its own ~4KB stack, allocated first, was exactly the margin
            // that made the 32KB rung fail beside USB host mode (largest
            // internal block 35KB). Order big-contiguous-first; if the
            // sampler can't spawn now, elf_input_start falls back to
            // per-frame polling (input still works).
            elf_input_start();
            xSemaphoreTake(done, portMAX_DELAY); // wait for module to return
            result = ctx.result;
            SLog.printf("[elf_host] module returned %d\n", result);
        } else {
            result = -2; // distinct from -1 (module ran but exit() called)
            SLog.printf("[elf_host] FAILED to create module task — largest internal block: %u bytes\n",
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        if (done) vSemaphoreDelete(done);
        // Stop the Core 1 keyboard sampler and wait for it to exit before any
        // further cleanup, so its I2C reads can't overlap loopTask's once the
        // firmware's own keyboard scan resumes.
        elf_input_stop();
        // Peer-link safety: the gameboy module DETACHes its gblink service
        // itself, but the exit()-longjmp path can skip that — make sure the
        // peer never sees a ghost game.
        tdeck_link_gb_send(TDL_GB_DETACH, 0, 0);
        // The pull callback (and the Audio state it reads) lives in module
        // memory; unregister before any of it is freed. Blocks until the
        // mixer is outside the callback. The module normally does this in
        // its own cleanup, but the exit()-longjmp path skips that.
        sound_extern_set_pull(NULL, 0);
        // Force-delete any worker tasks the module left running — they
        // execute module code, which is about to be freed. The module
        // normally joins its workers itself; the exit()-longjmp path can
        // skip that. A worker killed inside host_blit_frame_async can leave
        // s_blit_idle taken, which would hang blit_drain — give it back in
        // that case only (a spurious give on a binary semaphore is a no-op),
        // so the normal path keeps blit_drain's strict wait.
        if (elf_workers_cleanup() > 0 && s_blit_idle)
            xSemaphoreGive(s_blit_idle);
        blit_drain();  // an async push may still be reading module BSS
        elf_unload(mod);
    } else {
        SLog.println("[elf_host] elf_load failed");
    }

    // Touch layout/OSK teardown — after blit_drain: no push reads the
    // overlay buffers past this point. Also clears a layout stashed by
    // _elf_touch_layout when the launch failed before arming it.
    elf_zones_teardown();

    // Close any file descriptors the module left open
    mod_close_tracked_files();

    // Free any PSRAM the module leaked on exit()
    mod_free_tracked();

    // Free the DMA bounce buffer used during this session
    if (s_io_bounce) {
        heap_caps_free(s_io_bounce);
        s_io_bounce = nullptr;
    }

    // Reset audio rate tracking so the next module's first push sets it fresh
    s_audio_last_rate = 0;

    SLog.printf("[elf_host] PSRAM after cleanup: %u free, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Resume LVGL and watchdog
    host_clear_screen();
    wake_activity();             // reset inactivity timer + restore backlights
    elf_set_core1_handlers(false); // restore Core 1 exception handlers
    sound_extern_flush();        // drain any leftover module audio
    // Re-enable interrupt watchdog
    REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0x50D83AA1);
    REG_SET_BIT(TIMG_WDTCONFIG0_REG(1), (1U << 31));
    REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0);
    esp_task_wdt_init(5, true);  // restore normal watchdog (5s timeout, panic on trigger)
    lvgl_ticker.attach_ms(5, []() { lv_tick_inc(5); });
    // Force full redraw — LVGL doesn't know the display was wiped
    lv_obj_invalidate(lv_screen_active());

    SLog.printf("[elf_host] module session done (loaded=%d result=%d)\n",
                mod != NULL, result);

    // Surface failures through the notification bell — with Lua torn down
    // during the run, a failed launch otherwise just drops the user back at
    // the launcher with zero explanation. notify.cpp's ring survives the
    // teardown by design, so the bell shows this after Lua returns.
    if (!mod || result != 0) {
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char msg[192];   // == NOTIFY_LOG_TEXT_MAX (notify.cpp truncates above it)
        if (!mod) {
            snprintf(msg, sizeof(msg),
                     "App launch failed: %s didn't load - low RAM "
                     "(PSRAM largest %uKB). Restart and try again; if it keeps "
                     "failing see Read Me > Freeing up RAM.",
                     base,
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        } else if (result == -2) {
            snprintf(msg, sizeof(msg),
                     "App launch failed: not enough free RAM for %s "
                     "(largest block %uKB). Restart and try again; if it keeps "
                     "failing see Read Me > Freeing up RAM.",
                     base,
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        } else if (result == -1) {
            snprintf(msg, sizeof(msg),
                     "%s exited early (aborted - missing ROM/file?)", base);
        } else {
            snprintf(msg, sizeof(msg), "%s exited with error %d", base, result);
        }
        notify_post(msg);
    }
    s_elf_running = false;
    return result;
}

// _elf_touch_layout({ {x=,y=,w=,h=,out=,label=}, ... }) — controller zones
// for the NEXT module launch. OUT codes are module keycodes (0xFF = quit);
// lib/keybind.lua generates the table from a launcher's action list. Armed
// in elf_input_start on keyboardless boards, cleared when the module exits —
// call it before every _launch_elf. Returns the zone count accepted.
static int lua_elf_touch_layout(lua_State* L) {
    s_elf_zone_count = 0;
    if (!lua_istable(L, 1)) { lua_pushinteger(L, 0); return 1; }
    int len = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= len && s_elf_zone_count < INPUT_ZONES_MAX; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            InputZone* z = &s_elf_zones[s_elf_zone_count];
            memset(z, 0, sizeof(*z));
            lua_getfield(L, -1, "x");   z->x   = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "y");   z->y   = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "w");   z->w   = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "h");   z->h   = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "out"); z->out = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "label");
            const char* lb = lua_tostring(L, -1);
            if (lb) { strncpy(z->label, lb, sizeof(z->label) - 1); }
            lua_pop(L, 1);
            s_elf_zone_count++;
        }
        lua_pop(L, 1);
    }
    lua_pushinteger(L, s_elf_zone_count);
    return 1;
}

void elf_host_register_lua(lua_State* L) {
    lua_register(L, "_launch_elf", lua_launch_elf);
    lua_register(L, "_elf_touch_layout", lua_elf_touch_layout);
}
