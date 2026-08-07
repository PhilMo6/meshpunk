#include "elf_host.h"
#include "elf_loader.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"
#include "sound.h"
#include "notify.h"
#include "ble_companion.h"
#include "punkmesh.h"
#include "usb_manager.h"   // usb_pool_alloc/free — dynamic USB driver segments
#include "tdeck_link.h"    // peer link: gblink veneers + module-exit detach

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
    if (!s_input_task) return;
    s_input_task_run = false;
    for (int i = 0; i < 100 && s_input_task; i++) {  // ~200ms safety cap
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_input_task = nullptr;
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
    display_dev_blit(x_offset, y_offset, w, h, rgb565);
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

    // Dump mesh object pointer region to detect corruption
    extern PunkMesh* the_mesh;
    SLog.printf("[elf_host] the_mesh=%p, first 16 bytes:", the_mesh);
    if (the_mesh) {
        uint8_t* p = (uint8_t*)the_mesh;
        for (int i = 0; i < 16; i++) SLog.printf(" %02x", p[i]);
    }
    SLog.println();

    int result = -1;
    if (mod) {
        SLog.printf("[elf_host] heap OK before run: %s\n",
                      heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? "yes" : "NO!");
        SLog.printf("[elf_host] the_mesh at: 0x%08x\n", (uint32_t)the_mesh);

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
        char msg[160];
        if (!mod) {
            snprintf(msg, sizeof(msg),
                     "App launch failed: %s didn't load (PSRAM largest %uKB)",
                     base,
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        } else if (result == -2) {
            snprintf(msg, sizeof(msg),
                     "App launch failed: not enough internal RAM for %s "
                     "(largest %uKB). USB host mode uses RAM - try Stop USB.",
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

void elf_host_register_lua(lua_State* L) {
    lua_register(L, "_launch_elf", lua_launch_elf);
}
