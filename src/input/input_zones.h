// input_zones.h — touch-zone input layer (controller mode), board-agnostic.
//
// A layout is a set of screen rectangles, each mapped to a key code. While
// enabled, EVERY touch is intercepted before LVGL's pointer indev (Phil's
// spec: controller mode owns all touch); a touch inside a zone holds that
// zone's key down, sliding between zones releases/presses, and the held key
// is merged into the same kb_key_state[] + keypad-indev stream physical
// keys use — so apps reading LVGL KEY events or the _kb_* polling APIs work
// unmodified.
//
// MULTI-TOUCH: one zone per touch point can be held at once (d-pad plus a
// face button), bounded by the panel — GT911 5, CHSC6X 2. A single finger
// sliding between zones still reads as release+press. Layouts come from Lua
// (lib/touchlayout.lua) via the _zones_* bindings, from launchers via
// _elf_touch_layout, or from the host OSK (whose 45 keys size the cap).
//
// Out codes 0xFD and above are RESERVED and never reach an app or a module:
//   0xFD  SHOT zone — queues a screenshot request (drained by loop()'s
//         dispatcher in the Lua world, by the ELF input task during a run).
//         The same code is the out of lib/keybind's bindable Screenshot
//         action, so a key and a pad tap arrive as one thing.
//   0xFE  MODE zone — queues a mode-button event (same channel as the
//         hardware IO button).
//   0xFF  QUIT (HOST_KEY_QUIT, elf_host.cpp) — ends the module.
// Use zone_out_is_key() rather than comparing against one of them: that is
// the single place the boundary moves when another code is reserved.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define INPUT_ZONES_MAX   48
#define INPUT_ZONE_SHOT   0xFD   // reserved out: screenshot pseudo-zone
#define INPUT_ZONE_MODE   0xFE   // reserved out: mode-toggle pseudo-zone

// True when an out code is a real key to deliver (non-zero, not reserved).
static inline bool zone_out_is_key(uint8_t out) {
  return out != 0 && out < INPUT_ZONE_SHOT;
}

typedef struct {
  int16_t x, y;
  int16_t w, h;
  uint8_t out;        // key char / code merged into kb_key_state
  char    label[8];   // overlay indicator text (Lua renders it)
} InputZone;

// Layout management (Lua world).
void input_zones_set(const InputZone* z, int n);   // copies; n<=MAX
void input_zones_clear(void);
void input_zones_enable(bool on);
bool input_zones_enabled(void);                    // on AND layout present

// Touch hook — called from the pointer indev callback with every sample.
// Returns true when the touch belongs to the zone layer (caller must not
// forward it to LVGL). pressed=false (release) always clears the held zones.
bool input_zones_touch(int16_t x, int16_t y, bool pressed);

// Multi-point form: n=0 means all fingers lifted. Same return meaning.
bool input_zones_touch_multi(const int16_t* xs, const int16_t* ys, int n);

// First held zone's out code (0 = none). Kept for single-key callers.
uint8_t input_zones_held(void);

// All held out codes, up to `max`; returns the count. Order is layout order,
// so it is stable across polls no matter which finger landed first.
int input_zones_held_all(uint8_t* outs, int max);

// True when `out` is currently held by any point.
bool input_zones_out_held(uint8_t out);

// Mode-zone tap event (0xFE): true once per tap.
bool input_zones_mode_toggle_take(void);

// Shot-zone tap event (0xFD): true once per tap.
bool input_zones_shot_take(void);
