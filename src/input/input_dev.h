// input_dev.h — per-device input backend contract.
//
// Exactly ONE backend implementation is compiled per build, selected by the
// board define in platformio.ini (-DBOARD_TDECK -> input_tdeck.cpp). The
// functions are plain free functions on purpose: no vtable, no runtime
// dispatch — a board's firmware image only ever contains its own backend.
//
// The backend owns everything that touches input HARDWARE: buses, controller
// init, matrix layout tables, protocol quirks, ISRs, backlight commands.
// Everything above it (layer/latch policy, LVGL indev feed, the ELF host's
// key queue, Lua bindings) is device-agnostic and lives in input_ui.cpp /
// elf_host.cpp, talking to the hardware only through this header.
//
// Matrix geometry never crosses this interface: consumers see decoded
// per-key {base, sym} character pairs and modifier levels, so a backend with
// a different matrix (or no keyboard at all) fits without header changes.

#pragma once

#include <stdint.h>

// Decoded characters for one pressed physical key. `base` is the key's
// normal-layer char, `sym` its symbol-layer char; either may be 0 when that
// layer has no character on the key (the T-Deck mic key is {0, '0'}).
// Enter/Backspace positions report 0x0D/0x08 in BOTH fields — consumers
// special-case them off `base` (chords, Esc synthesis) before layer logic.
// Shift is NOT applied: the UI path upcases letters itself, the ELF path
// forwards a 0x80 shift pseudo-key instead.
struct InputKeyEv {
    uint8_t base;
    uint8_t sym;
};

// Upper bound on simultaneously reported keys per poll (modifiers excluded).
// The 5x7 matrix can't produce more without ghosting long past usability.
#define INPUT_DEV_KEYS_MAX 16

// ── Capabilities ────────────────────────────────────────────────────────────
bool input_dev_has_keyboard(void);       // probe result (set by input_dev_init)
bool input_dev_has_trackball(void);
bool input_dev_has_touch(void);
bool input_dev_has_kbd_backlight(void);

// ── Lifecycle ───────────────────────────────────────────────────────────────
// preinit: pin modes + ISR attach for input lines that need no bus/power
// (called early in setup(), before peripheral power-on). init: bus + touch
// controller + keyboard probe/mode; kbd_backlight_boot is the user's
// persisted backlight level to apply once the keyboard is found.
void input_dev_preinit(void);
void input_dev_init(uint8_t kbd_backlight_boot);

// ── Keyboard sampling ───────────────────────────────────────────────────────
// poll performs ONE bus transaction and stores the sample; the query calls
// below read that stored sample. Single-reader by construction: the LVGL
// indev callback (loopTask) polls while the UI runs, the ELF input task
// (Core 1) polls during a module run — never both (loopTask is blocked for
// the whole module run).
// detect_legacy_fw: run the old-keyboard-firmware heuristic on this sample.
// Passed true only by the interactive UI reader — matching the pre-split
// behavior where detection lived only in keyboard_read_cb.
void    input_dev_kbd_poll(bool detect_legacy_fw);
uint8_t input_dev_kbd_legacy_byte(void);   // legacy mode: byte fresh THIS poll
                                           // (raw, unbounded; 0 = none)
void    input_dev_kbd_mods(bool* lshift, bool* rshift, bool* sym, bool* alt);
int     input_dev_kbd_decode(InputKeyEv* out, int max);  // pressed non-modifier keys
bool    input_dev_kbd_mic_edge(void);      // mic key: down this poll, up last poll

// ── Legacy ASCII mode facet ────────────────────────────────────────────────
// T-Deck-specific: keyboard-MCU firmware older than LilyGo's 250620 build has
// no raw matrix mode (I2C cmd 0x03 is ACKed but ignored) and only sends one
// final ASCII byte per press. This is a runtime mode INSIDE the T-Deck
// backend — old and new T-Decks share this backend — not a separate device.
// Backends for hardware without the concept stub these (get() == false).
bool input_dev_kbd_legacy_get(void);
void input_dev_kbd_legacy_load(bool on);   // prefs restore only: set the flag
                                           // without touching the bus (pre-init)
void input_dev_kbd_legacy_set(bool on);    // live switch: mode command + reset
                                           // of backend sampling state
bool input_dev_kbd_legacy_autoswitch_pending(void);  // detection verdict;
                                                     // clears on read
void input_dev_kbd_autoswitch_close(void); // manual legacy-OFF blocks detection
                                           // until reboot (don't fight the user)

// ── Keyboard backlight ─────────────────────────────────────────────────────
// Callers: input_ui wake/restore, main.cpp timeout dimmer + _kbd_* bindings,
// notify.cpp blink state machine (mesh task — the bus driver's per-transaction
// lock serializes it against the matrix reads, exactly as before the split).
void input_dev_kbd_backlight(uint8_t value);
void input_dev_kbd_backlight_default(uint8_t value);

// ── Nav pulse counters (device-NEUTRAL, defined in input_ui.cpp) ───────────
// Shared accumulator channel for discrete nav events. Producers: the T-Deck
// trackball ISRs (this backend) and the USB HID driver's arrow/mouse
// injection (usb_core.cpp, any board). Consumers: the UI nav model
// (input_ui.cpp) and the ELF host's momentum/raw models (elf_host.cpp), each
// keeping its own consumption discipline on the raw counters.
extern volatile int trackball_up;
extern volatile int trackball_down;
extern volatile int trackball_left;
extern volatile int trackball_right;
extern volatile int trackball_click;

// Live level of the nav click line (true while physically held). The click
// ISR counts press EDGES only; LVGL release detection and module mouse
// buttons need the level.
bool input_dev_nav_click_held(void);

// Auxiliary (mode) button: one event per press, consumed by loop()'s
// dispatcher to cycle the input mode (controller/OSK). Boards without a
// second button return false.
bool input_dev_aux_btn_take(void);

// ── Touch ──────────────────────────────────────────────────────────────────
// Latest touch point; true while touched (first contact wins). What LVGL's
// pointer indev uses — it is single-point by nature.
bool input_dev_touch_read(int16_t* tx, int16_t* ty);

// All current touch points, up to `max`; returns the count (0 = not
// touched). Controller mode uses this so a d-pad direction and a face
// button can be held at once. Panel limits: GT911 (T-Deck) reports up to 5,
// CHSC6X (Heltec) 2 — hw-verified, not datasheet-derived. Backends fill
// point 0 identically to input_dev_touch_read.
#define INPUT_DEV_TOUCH_MAX 5
int input_dev_touch_read_multi(int16_t* xs, int16_t* ys, int max);

// Diagnostics for the touch measurement app (Tools/Touch Test): the last
// accepted frame, BEFORE the board's raw->screen transform, so the app can
// compare what the controller reported against where the UI thinks the
// finger is. Slot 1 is carried because a phantom contact occupying slot 0
// while the real one sits in slot 1 would mis-locate every touch that
// happens near an edge — the theory this exists to test. Boards whose
// driver already returns screen coordinates report those unchanged.
typedef struct {
    int16_t  x0, y0;    // slot 0 raw, -1 when the frame carried none
    int16_t  x1, y1;    // slot 1 raw, -1 when absent
    uint8_t  points;    // point count the controller reported
    uint32_t drops;     // frames/slots rejected as invalid since boot
} InputTouchRaw;

void input_dev_touch_raw(InputTouchRaw* out);
