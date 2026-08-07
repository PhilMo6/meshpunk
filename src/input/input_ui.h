// input_ui.h — device-agnostic input policy layer (see input_ui.cpp).
//
// Sits between the per-device backend (input_dev.h) and LVGL/Lua: the LVGL
// indev callbacks, sym/alt layer + tap-toggle latches, the alt emoji layer,
// WASD nav, the home/topbar/emoji-popup shortcuts, the Gamepad input-capture
// wizard, and the pure-input Lua bindings (registered by
// input_ui_register_lua, same house pattern as sound_register_lua /
// elf_host_register_lua — binding NAMES are unchanged).

#pragma once

#include <stdint.h>
#include <lvgl.h>

typedef struct lua_State lua_State;

// ── Lifecycle ──────────────────────────────────────────────────────────────
// init stores the prefs-save callback used by bindings/mode switches (same
// pattern as sound_init). Call before any binding can fire; the pref
// setters below work before init (plain assignments) so the boot prefs
// loader may run first.
void input_ui_init(void (*prefs_save)(void));

// Create + register the LVGL indevs (touch pointer always; keypad when the
// backend reports a keyboard). Call from setupLvgl() after the display exists.
void input_ui_setup_indevs(lv_display_t* disp);

// Register the input Lua bindings: _kb_*, _input_capture_*, _trackball_*,
// _kb_emoji_*, _emoji_popup_insert/release, _kb_legacy_*, _input_caps.
void input_ui_register_lua(lua_State* L);

// ── Legacy ASCII keyboard mode (T-Deck facet) ──────────────────────────────
// Orchestrates a live mode switch: backend mode command + reset of every
// policy-layer state derived under the old mode; save=true persists prefs.
void input_ui_set_legacy(bool on, bool save);

// ── Deferred-shortcut flags (set by the indev callback, consumed by loop())
// Each returns true exactly once per pending event.
bool input_ui_take_topbar_shortcut(void);
bool input_ui_take_emoji_popup(void);
bool input_ui_take_home_shortcut(void);

// ── Pref accessors (prefs writer/loader + Settings bindings state) ─────────
uint16_t input_ui_trackball_sens_get(void);
void     input_ui_trackball_sens_set(uint16_t v);
uint16_t input_ui_trackball_roll_get(void);
void     input_ui_trackball_roll_set(uint16_t v);
bool     input_ui_sym_toggle_get(void);
void     input_ui_sym_toggle_set(bool on);
bool     input_ui_alt_toggle_get(void);
void     input_ui_alt_toggle_set(bool on);

// Load /emoji_keymap over the compiled defaults (boot, after FS mount).
void input_ui_emoji_map_load(void);

// ── Hooks PROVIDED BY the firmware (main.cpp) to this layer ────────────────
// Activity/timeout policy and the nav-scope stack stay in main.cpp; the
// callbacks reach them only through these seams.
void firmware_note_activity(void);      // bump the inactivity timer
void firmware_wake_restore(void);       // un-dim screen/kbd if timed out
void nav_input_tick_begin(void);        // flush pending gridnav + drop dead scopes
void nav_rearm_on_trackball(void);      // re-arm gridnav on the top scope
void nav_disarm_on_touch(void);         // disarm gridnav so the finger scrolls
