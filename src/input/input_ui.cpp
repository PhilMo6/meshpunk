// input_ui.cpp — device-agnostic input policy layer (contract: input_ui.h).
//
// Everything here moved verbatim from main.cpp's input stack when the
// per-device input layer was carved out; hardware access now goes through
// the compiled-in backend (input_dev.h). The nav-scope stack and the
// activity/timeout policy stayed in main.cpp — reached via the firmware_* /
// nav_* hooks declared in input_ui.h.

#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "input_dev.h"
#include "input_ui.h"
#include "input_zones.h"        // controller-mode touch zones
#include "../meshpunk_sync.h"   // SLog
#include "../usb_manager.h"     // usb_kbd_snapshot + UsbFlashGuard
#include "../emoji_font.h"      // emoji_preload

// ── Nav pulse counters (declared in input_dev.h) ────────────────────────────
// Neutral accumulators: produced by the board backend's trackball ISRs and by
// the USB HID driver's arrow/mouse injection (usb_core.cpp), consumed by the
// nav model below and by the ELF host's momentum/raw models (elf_host.cpp).
volatile int trackball_up = 0;
volatile int trackball_down = 0;
volatile int trackball_left = 0;
volatile int trackball_right = 0;
volatile int trackball_click = 0;

// ── Prefs-save callback (stored by input_ui_init) ──────────────────────────
static void (*s_prefs_save)(void) = nullptr;

// ── Keyboard sym behavior ───────────────────────────────────────────────────
// false = sym is a plain hold modifier. true = a clean tap of sym (press and
// release with nothing typed in between) latches the symbol layer until the
// next tap, while holding sym still works as a momentary modifier. Persisted.
static bool kb_sym_toggle_pref = false;
static bool kb_sym_latched = false;
static bool kb_sym_phys_prev = false;
static bool kb_sym_used_while_held = false;

// ── Keyboard alt emoji layer ────────────────────────────────────────────────
// alt+key types an emoji while a textarea is focused (the layer is inert
// outside text fields, so a latched alt never hijacks WASD nav). Same
// tap-toggle latch machinery as sym above, behind its own persisted pref.
// The per-key map is keyed by the key's normal-layer char and persisted to
// /emoji_keymap on LittleFS (kb_emoji_map_load/save below).
static bool kb_alt_toggle_pref = false;
static bool kb_alt_latched = false;
static bool kb_alt_phys_prev = false;
static bool kb_alt_used_while_held = false;
static bool kb_alt_layer_active = false;

// ── Trackball nav tuning ───────────────────────────────────────────────────
static uint16_t trackball_sensitivity_ms = 75;  // ms between accepted direction pulses, persisted
static uint16_t trackball_roll_ms = 0;          // drain interval: 0 = instant (no momentum), persisted

// ── Alt emoji layer map ─────────────────────────────────────────────────────
// Default emoji per key (normal-layer char -> Unicode codepoint), roughly the
// most-used emojis with a few mnemonics (z=sleep, $=money, h=haha). Space is
// deliberately absent so latched emoji runs can still be space-separated.
// Sequence emojis are assignable too: the picker stores their PUA codepoint
// and the send path decomposes it to real Unicode (see prepare_outgoing_text).
struct KbEmojiDefault { char key; uint32_t cp; };
static const KbEmojiDefault kb_emoji_defaults[] = {
  {'q',0x1F923},{'w',0x1F609},{'e',0x1F60D},{'r',0x1F917},{'t',0x1F44D},
  {'y',0x1F642},{'u',0x1F937},{'i',0x1F60A},{'o',0x1F618},{'p',0x1F970},
  {'a',0x2764}, {'s',0x263A}, {'d',0x1F62D},{'f',0x1F525},{'g',0x1F601},
  {'h',0x1F602},{'j',0x1F605},{'k',0x1F64F},{'l',0x1F606},
  {'z',0x1F634},{'x',0x1F926},{'c',0x1F97A},{'v',0x1F495},{'b',0x1F382},
  {'n',0x1F644},{'m',0x1F914},{'$',0x1F4B0},
};

// Active map, indexed by the key's normal-layer char. 0 = no emoji (the key
// falls through to its normal char under alt).
static uint32_t kb_emoji_map[128] = {0};

static void kb_emoji_apply_defaults() {
  memset(kb_emoji_map, 0, sizeof(kb_emoji_map));
  for (auto &d : kb_emoji_defaults) kb_emoji_map[(uint8_t)d.key] = d.cp;
}

// /emoji_keymap on LittleFS: one "c=1F602" line per key (hex codepoint; 0
// clears the key). Defaults apply first, then the file overrides — so a
// missing file or a key the file doesn't mention means the compiled default.
void input_ui_emoji_map_load(void) {
  kb_emoji_apply_defaults();
  File f = LittleFS.open("/emoji_keymap", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3 || line[1] != '=') continue;
    uint8_t c = (uint8_t)line[0];
    if (c >= 128) continue;
    kb_emoji_map[c] = (uint32_t)strtoul(line.c_str() + 2, nullptr, 16);
  }
  f.close();
}

static void kb_emoji_map_save() {
  UsbFlashGuard _g;   // internal-flash write — pause USB audio around it (crash-safe)
  File f = LittleFS.open("/emoji_keymap", "w", true);
  if (!f) { SLog.println("[KB_EMOJI] cannot write /emoji_keymap"); return; }
  for (int c = 32; c < 128; c++) {
    bool is_default_key = false;
    for (auto &d : kb_emoji_defaults) {
      if ((uint8_t)d.key == c) { is_default_key = true; break; }
    }
    // Write every assigned key, plus explicit "=0" lines for cleared default
    // keys so a cleared key doesn't resurrect its default on the next boot.
    if (kb_emoji_map[c] || is_default_key)
      f.printf("%c=%lX\n", (char)c, (unsigned long)kb_emoji_map[c]);
  }
  f.close();
}

// Pack a codepoint's UTF-8 bytes into a uint32 with the FIRST byte in the
// LOW byte (U+1F602 = F0 9F 98 82 -> 0x82989FF0). lv_textarea_add_char()
// reinterprets the uint32's memory as the char sequence
// (letter_buf = (char *)&u32_buf), and the ESP32-S3 is little-endian, so
// memory order = low byte first.
static uint32_t utf8_pack_key(uint32_t cp) {
  if (cp < 0x80u) return cp;
  if (cp < 0x800u)
    return  (0xC0u | (cp >> 6)) |
           ((0x80u | (cp & 0x3Fu)) << 8);
  if (cp < 0x10000u)
    return  (0xE0u | (cp >> 12)) |
           ((0x80u | ((cp >> 6) & 0x3Fu)) << 8) |
           ((0x80u | (cp & 0x3Fu)) << 16);
  return  (0xF0u | (cp >> 18)) |
         ((0x80u | ((cp >> 12) & 0x3Fu)) << 8) |
         ((0x80u | ((cp >> 6) & 0x3Fu)) << 16) |
         ((0x80u | (cp & 0x3Fu)) << 24);
}

// ── Keyboard state tracking ────────────────────────────────────────────────
static uint32_t last_key_code = 0;
static bool kb_key_state[128] = {0};
static bool kb_key_prev[128] = {0};
static uint32_t kb_key_press_time[128] = {0};
static const uint32_t KEY_HOLD_THRESHOLD_MS = 400;
static bool kb_shift_active = false;
static bool kb_lshift_active = false;
static bool kb_rshift_active = false;
static bool kb_sym_active = false;
static bool kb_alt_active = false;

// ── Legacy ASCII mode runtime state ─────────────────────────────────────────
// One byte arrives per physical press (no release/repeat), so each byte is
// synthesized into a press: kb_key_state[ch] stays true for LEGACY_PULSE_MS
// (feeds the _kb_* polling APIs and input capture), while the LVGL emit is a
// one-poll edge per byte so double letters register.
#define LEGACY_PULSE_MS 120
static uint8_t  kb_legacy_down_ch  = 0;   // char currently pulsed, 0 = none
static uint32_t kb_legacy_deadline = 0;   // millis() when the pulse releases

// Orchestrate a live keyboard-mode switch: the backend sends the MCU mode
// command and drops its sampling state; every policy state derived under the
// previous mode is dropped here.
void input_ui_set_legacy(bool on, bool save) {
  input_dev_kbd_legacy_set(on);
  memset(kb_key_state, 0, sizeof(kb_key_state));
  memset(kb_key_prev, 0, sizeof(kb_key_prev));
  last_key_code = 0;
  kb_shift_active = kb_lshift_active = kb_rshift_active = false;
  kb_sym_active = kb_alt_active = kb_alt_layer_active = false;
  kb_sym_latched = kb_alt_latched = false;
  kb_sym_phys_prev = kb_alt_phys_prev = false;
  kb_legacy_down_ch = 0;
  kb_legacy_deadline = 0;
  if (save && s_prefs_save) s_prefs_save();
}

// ── Deferred shortcuts (flags set here, dispatched from loop()) ────────────
// Bare mic press = notifications shortcut: the keyboard reader (LVGL indev
// callback) only sets the flag; loop() dispatches it into Lua
// (topbar.on_shortcut) outside of indev processing.
static bool s_topbar_shortcut_pending = false;

// Alt+mic while typing = emoji search popup (lib/emoji_popup). Same flag/
// dispatch split as the topbar shortcut; the textarea focused at press time is
// captured as the insert target for _emoji_popup_insert, which re-validates the
// pointer before every insert (the app underneath can rebuild its views while
// the popup is up).
static bool      s_emoji_popup_pending = false;
static lv_obj_t *s_emoji_popup_target  = NULL;

// Alt+Backspace held ~1.5s = home shortcut (the Lua-land twin of the ELF exit
// chord, same hold time): close the current app and return to the launcher
// home page (apps.home_shortcut via loop()). Physical alt only — a LATCHED
// alt while holding backspace to delete text must not count.
#define HOME_CHORD_HOLD_MS 1500
static uint32_t s_home_chord_start   = 0;      // 0 = chord not held
static bool     s_home_chord_fired   = false;  // fired once for this hold
static bool     s_home_shortcut_pending = false;

bool input_ui_take_topbar_shortcut(void) {
  if (!s_topbar_shortcut_pending) return false;
  s_topbar_shortcut_pending = false;
  return true;
}

bool input_ui_take_emoji_popup(void) {
  if (!s_emoji_popup_pending) return false;
  s_emoji_popup_pending = false;
  return true;
}

bool input_ui_take_home_shortcut(void) {
  if (!s_home_shortcut_pending) return false;
  s_home_shortcut_pending = false;
  return true;
}

// Input capture for the Gamepad mapping wizard (_input_capture_* bindings):
// armed by Lua, consumed by the capture block inside keyboard_read_cb.
static volatile bool     s_input_capture_armed = false;
static volatile uint32_t s_input_captured      = 0;

// Shift+Alt (mode-cycle chord) edge state, set by the keyboard reader below
// and consumed by loop()'s dispatcher. Physical modifier levels only (an alt
// LATCH must not arm it) with a debounce window, matching the ELF binding
// chord: a bouncing key or one dropped matrix read would otherwise fabricate
// a second edge and cycle twice.
#define TOUCH_CHORD_DEBOUNCE_MS 300
static bool          s_touch_chord_prev    = false;
static uint32_t      s_touch_chord_last_ms = 0;
static volatile bool s_touch_chord_pending = false;

// ── LVGL keyboard read callback ────────────────────────────────────────────
static bool trackball_btn_pressed = false;

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  nav_input_tick_begin();

  static uint32_t kb_mapped_key = 0;
  bool any_new = false;
  bool key_from_trackball = false;

  // ── Sample the keyboard (raw matrix — or one ASCII byte in legacy mode) ──
  // detect_legacy_fw=true: this interactive reader is where the
  // old-keyboard-firmware heuristic runs (see input_dev.h).
  input_dev_kbd_poll(/*detect_legacy_fw=*/true);
  uint8_t legacy_byte = 0;   // legacy mode: char received this poll (one per press)
  if (input_dev_kbd_legacy_get()) {
    legacy_byte = input_dev_kbd_legacy_byte();
    if (legacy_byte >= 128) legacy_byte = 0;   // kb_key_state bounds
  }

  // ── Decode modifier key states ──
  bool sym_phys = false, alt_phys = false;
  input_dev_kbd_mods(&kb_lshift_active, &kb_rshift_active, &sym_phys, &alt_phys);
  kb_shift_active = kb_lshift_active || kb_rshift_active;

  // alt: same tap-toggle latch as sym below, behind its own pref. The emoji
  // layer it drives only substitutes while typing in a textarea (see the
  // substitution block after the resolver), so a latched alt never pauses
  // WASD nav the way a latched sym does.
  kb_alt_active = alt_phys;
  if (kb_alt_toggle_pref) {
    if (alt_phys && !kb_alt_phys_prev) {
      kb_alt_used_while_held = false;             // new hold begins
    } else if (!alt_phys && kb_alt_phys_prev && !kb_alt_used_while_held) {
      kb_alt_latched = !kb_alt_latched;           // clean tap — toggle latch
    }
    kb_alt_layer_active = alt_phys || kb_alt_latched;
  } else {
    kb_alt_layer_active = alt_phys;
    kb_alt_latched = false;
  }
  kb_alt_phys_prev = alt_phys;

  // sym: plain hold by default. In toggle mode a clean tap (press + release
  // with nothing typed during the hold) latches the symbol layer until the
  // next tap; holding sym while typing still works as a momentary modifier.
  if (kb_sym_toggle_pref) {
    if (sym_phys && !kb_sym_phys_prev) {
      kb_sym_used_while_held = false;             // new hold begins
    } else if (!sym_phys && kb_sym_phys_prev && !kb_sym_used_while_held) {
      kb_sym_latched = !kb_sym_latched;           // clean tap — toggle latch
    }
    kb_sym_active = sym_phys || kb_sym_latched;
  } else {
    kb_sym_active = sym_phys;
    kb_sym_latched = false;
  }
  kb_sym_phys_prev = sym_phys;

  // ── Bare mic press = notifications shortcut ──
  // The mic key has no normal-layer character (its bare press is dead in the
  // key resolver below), so it's free as a global hotkey: raise the topbar
  // over a running app / toggle the notification drop-down
  // (topbar.on_shortcut, dispatched from loop()). Gated on !kb_sym_active so
  // sym+mic still types '0' through the symbol layer. Alt+mic instead opens
  // the emoji search popup — only while a textarea is focused (dead press
  // otherwise), capturing that textarea as the insert target.
  {
    if (input_dev_kbd_mic_edge() && !kb_sym_active) {
      if (kb_alt_layer_active) {
        // The mic key never reaches the char resolver (normal-layer ch==0),
        // so mark the alt hold as used here — otherwise a held-alt+mic reads
        // as a clean alt tap on release and flips the tap-toggle latch.
        if (alt_phys) kb_alt_used_while_held = true;
        lv_obj_t *foc = lv_group_get_focused(lv_group_get_default());
        if (foc && lv_obj_is_valid(foc) &&
            lv_obj_check_type(foc, &lv_textarea_class)) {
          s_emoji_popup_target  = foc;
          s_emoji_popup_pending = true;
        }
      } else {
        s_topbar_shortcut_pending = true;
      }
    }
  }

  // ── Snapshot previous key state and clear current ──
  memcpy(kb_key_prev, kb_key_state, 128);
  memset(kb_key_state, 0, 128);

  // ── Legacy mode: synthesize the received byte into key state ──
  // Each byte is a discrete press (the MCU sends nothing on release), pulsed
  // in kb_key_state for LEGACY_PULSE_MS so the _kb_* polling APIs and input
  // capture see it. A re-arrival restarts the pulse as a fresh press.
  bool legacy_new = false;
  if (input_dev_kbd_legacy_get()) {
    if (legacy_byte) {
      kb_legacy_down_ch  = legacy_byte;
      kb_legacy_deadline = millis() + LEGACY_PULSE_MS;
      kb_key_press_time[legacy_byte] = millis();
      legacy_new = true;
    } else if (kb_legacy_down_ch &&
               (int32_t)(millis() - kb_legacy_deadline) >= 0) {
      kb_legacy_down_ch = 0;
    }
    if (kb_legacy_down_ch) kb_key_state[kb_legacy_down_ch] = true;
  }

  // ── Resolve ALL pressed keys from the backend's decoded sample ──
  // Enter/Backspace positions arrive as base==0x0D/0x08; the mic key is
  // {0,'0'} so its bare press stays dead (ch==0) while sym+mic types '0'.
  uint32_t resolved_key = 0;

  InputKeyEv evs[INPUT_DEV_KEYS_MAX];
  int ev_n = input_dev_kbd_decode(evs, INPUT_DEV_KEYS_MAX);
  for (int i = 0; i < ev_n; i++) {
    uint8_t ch = 0;
    if (evs[i].base == 0x0D) {
      ch = 0x0D;
    } else if (evs[i].base == 0x08) {
      ch = 0x08;
    } else {
      ch = kb_sym_active ? evs[i].sym : evs[i].base;
      if (ch == 0) continue;
      if (kb_shift_active && ch >= 'a' && ch <= 'z') ch -= 32;
    }

    kb_key_state[ch] = true;
    if (!kb_key_prev[ch]) {
      kb_key_press_time[ch] = millis();
    }
    if (sym_phys) kb_sym_used_while_held = true;  // hold was used — not a tap
    if (alt_phys) kb_alt_used_while_held = true;

    if (resolved_key == 0) {
      if (ch == 0x0D) resolved_key = LV_KEY_ENTER;
      else if (ch == 0x08) resolved_key = LV_KEY_BACKSPACE;
      else resolved_key = ch;
    }
  }

  // ── Legacy mode: the received byte is the resolved key ──
  // Only on the poll it arrived: the next poll resolves 0, which resets
  // last_key_code below and reports RELEASED, so LVGL sees one clean press
  // per byte and a repeated character re-registers.
  if (input_dev_kbd_legacy_get() && legacy_new) {
    if      (legacy_byte == 0x0D) resolved_key = LV_KEY_ENTER;
    else if (legacy_byte == 0x08) resolved_key = LV_KEY_BACKSPACE;
    else                          resolved_key = legacy_byte;
  }

  // ── Merge USB HID keyboard held keys (usb_task, Core 1 → here, Core 0) ──
  // Shift/layout already resolved at parse time (usb_manager.cpp); chars OR
  // into the same level-based state as matrix keys, so LVGL, nav, and the
  // Lua _kb_* bindings see USB keys identically. Arrows arrive separately
  // via the trackball counters. Matrix keys win the resolved_key slot.
  {
    bool usb_held[128];
    if (usb_kbd_snapshot(usb_held)) {
      for (int ch = 1; ch < 128; ch++) {
        if (!usb_held[ch]) continue;
        kb_key_state[ch] = true;
        if (!kb_key_prev[ch]) {
          kb_key_press_time[ch] = millis();
        }
        if (resolved_key == 0) {
          if (ch == 0x0D)      resolved_key = LV_KEY_ENTER;
          else if (ch == 0x08) resolved_key = LV_KEY_BACKSPACE;
          else                 resolved_key = ch;
        }
      }
    }
  }

  // ── Controller-mode zone key (touch zones, lib/touchlayout.lua) ──
  // The held zone's code rides the same level-based state as matrix/USB
  // keys, so LVGL key events (with native auto-repeat) and the _kb_*
  // polling APIs see it identically. Physical keys win the resolved slot.
  {
    uint8_t zs[INPUT_DEV_TOUCH_MAX];
    int zn = input_zones_held_all(zs, INPUT_DEV_TOUCH_MAX);
    for (int i = 0; i < zn; i++) {
      uint8_t z = zs[i];
      if (!z || z >= 128) continue;
      kb_key_state[z] = true;
      if (!kb_key_prev[z]) {
        kb_key_press_time[z] = millis();
      }
      // LVGL's keypad indev carries ONE key per cycle, so the first held
      // zone wins the resolved slot; the rest are still live in
      // kb_key_state[] for the _kb_* pollers (how games read a d-pad
      // direction and a button together).
      if (resolved_key == 0) {
        if (z == 0x0D)      resolved_key = LV_KEY_ENTER;
        else if (z == 0x08) resolved_key = LV_KEY_BACKSPACE;
        else                resolved_key = z;
      }
    }
  }

  // ── Alt+Backspace held = home shortcut ──
  // Same chord + hold time as the ELF exit chord, applied to Lua apps: close
  // the current app, land on the launcher home page. Physical alt only (the
  // ELF chord's rule too) so an alt-LATCH user holding backspace to delete
  // text can't trigger it. Backspace keeps deleting during the hold — the
  // same trade-off the ELF chord made. Fires once per hold.
  {
    bool chord = alt_phys && kb_key_state[0x08];
    if (!chord) {
      s_home_chord_start = 0;
      s_home_chord_fired = false;
    } else if (s_home_chord_start == 0) {
      s_home_chord_start = millis();
    } else if (!s_home_chord_fired &&
               millis() - s_home_chord_start >= HOME_CHORD_HOLD_MS) {
      s_home_chord_fired = true;
      s_home_shortcut_pending = true;
    }
  }

  // ── Shift+Alt = cycle the touch input mode ──
  // The trigger on boards that have a keyboard (a keyboardless board uses
  // its aux button). Requires BOTH modifiers and nothing else held, so it
  // can't fire inside a shift- or alt-layer keystroke; the actual cycling
  // happens in loop()'s dispatcher, which knows whether the running app has
  // a controller layout.
  {
    bool other = false;
    for (int i = 1; i < 128 && !other; i++)
      if (kb_key_state[i]) other = true;
    bool chord = kb_shift_active && alt_phys && !other;
    if (chord && !s_touch_chord_prev &&
        millis() - s_touch_chord_last_ms >= TOUCH_CHORD_DEBOUNCE_MS) {
      s_touch_chord_last_ms = millis();
      s_touch_chord_pending = true;
    }
    s_touch_chord_prev = chord;
  }

  bool kb_active = (resolved_key != 0);

  // ── Input capture (Gamepad app mapping wizard; _input_capture_*) ──
  // While armed, the FIRST input this reader resolves is recorded as a
  // module/driver code (chars as themselves incl. 0x0D/0x08; trackball →
  // 0x81-0x84, click → 0x85) and ALL input is swallowed — the captured
  // press must not also navigate the UI. Matrix and USB keys both land in
  // kb_key_state, so both are capturable.
  if (s_input_capture_armed) {
    uint32_t got = 0;
    for (int ch = 1; ch < 128 && !got; ch++)
      if (kb_key_state[ch] && !kb_key_prev[ch]) got = ch;
    if (!got) {
      if      (trackball_click > 0) got = 0x85;
      else if (trackball_up > 0)    got = 0x81;
      else if (trackball_down > 0)  got = 0x82;
      else if (trackball_left > 0)  got = 0x83;
      else if (trackball_right > 0) got = 0x84;
    }
    if (got) {
      s_input_captured     = got;
      s_input_capture_armed = false;
    }
    trackball_click = 0;
    trackball_up = trackball_down = trackball_left = trackball_right = 0;
    resolved_key = 0;
    kb_active = false;
  }

  // ── WASD intercept — treat as direction, not character (unless typing) ──
  uint32_t wasd_dir = 0;
  lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
  bool typing = focused && lv_obj_is_valid(focused) && lv_obj_check_type(focused, &lv_textarea_class);

  // ── Alt emoji layer — substitute AFTER resolution, only while typing ──
  // kb_key_state[] above stays indexed by the base char (it's a 128-slot
  // array); outside a textarea the layer is inert. Sym wins when both are
  // active, shift is ignored (alt+shift+A = same emoji as alt+a), and an
  // unmapped key falls through to its normal char. The key value carries the
  // emoji's UTF-8 bytes packed low-byte-first (see utf8_pack_key).
  if (typing && kb_alt_layer_active && !kb_sym_active &&
      resolved_key >= 0x20 && resolved_key < 0x80) {
    uint32_t base = resolved_key;
    if (base >= 'A' && base <= 'Z') base += 32;
    uint32_t cp = kb_emoji_map[base];
    // emoji_preload gates on the ACTIVE blob: a mapped emoji the current set
    // can't render (e.g. a sequence PUA after the SD extended set was
    // removed) falls through to the plain char instead of emitting a
    // codepoint that would draw tofu here and on the receiving device.
    if (cp && emoji_preload(cp)) resolved_key = utf8_pack_key(cp);
  }

  if (!typing) {
    if      (resolved_key == 'w') wasd_dir = LV_KEY_UP;
    else if (resolved_key == 'a') wasd_dir = LV_KEY_LEFT;
    else if (resolved_key == 's') wasd_dir = LV_KEY_DOWN;
    else if (resolved_key == 'd') wasd_dir = LV_KEY_RIGHT;
    if (wasd_dir) kb_active = false;
  }

  // ── LVGL state tracking (single-key for LVGL reporting) ──
  if (kb_active) {
    if (resolved_key != last_key_code) {
      last_key_code = resolved_key;
      kb_mapped_key = resolved_key;
      any_new = true;
      firmware_note_activity();
    }
  } else {
    if (last_key_code != 0) last_key_code = 0;
  }

  // ── Direction navigation — WASD + trackball, shared sensitivity ──
  if (!kb_active) {
    if (trackball_click > 0) {
      trackball_click = 0;
      last_key_code = LV_KEY_ENTER;
      any_new = true;
      key_from_trackball = true;
      trackball_btn_pressed = true;
    } else {
      uint32_t nav_dir = wasd_dir;
      if (!nav_dir) {
        if      (trackball_up > 0)    nav_dir = LV_KEY_UP;
        else if (trackball_down > 0)  nav_dir = LV_KEY_DOWN;
        else if (trackball_left > 0)  nav_dir = LV_KEY_LEFT;
        else if (trackball_right > 0) nav_dir = LV_KEY_RIGHT;
      }

      if (nav_dir) {
        static uint32_t last_nav_ms = 0;
        uint32_t now = millis();
        uint16_t interval = (!wasd_dir && trackball_roll_ms > 0)
                            ? trackball_roll_ms : trackball_sensitivity_ms;
        if (now - last_nav_ms >= interval) {
          last_nav_ms = now;
          if (!wasd_dir) {
            if (trackball_roll_ms > 0) {
              if      (nav_dir == LV_KEY_UP)    trackball_up--;
              else if (nav_dir == LV_KEY_DOWN)  trackball_down--;
              else if (nav_dir == LV_KEY_LEFT)  trackball_left--;
              else if (nav_dir == LV_KEY_RIGHT) trackball_right--;
            } else {
              if      (nav_dir == LV_KEY_UP)    trackball_up = 0;
              else if (nav_dir == LV_KEY_DOWN)  trackball_down = 0;
              else if (nav_dir == LV_KEY_LEFT)  trackball_left = 0;
              else if (nav_dir == LV_KEY_RIGHT) trackball_right = 0;
            }
          }
          last_key_code = nav_dir;
          kb_mapped_key = nav_dir;
          any_new = true;
          key_from_trackball = true;
        }
      }
    }
    if (key_from_trackball) firmware_note_activity();
  }

  // ── Re-enable gridnav on trackball input (top scope only) ──
  if (key_from_trackball) nav_rearm_on_trackball();

  // ── Wake from timeout ──
  if (any_new) firmware_wake_restore();

  // ── Report to LVGL ──
  if (kb_active) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->key = kb_mapped_key;
  } else if (any_new) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->key = last_key_code;
  } else if (trackball_btn_pressed) {
    if (input_dev_nav_click_held()) {
      data->state = LV_INDEV_STATE_PRESSED;
      data->key = LV_KEY_ENTER;
    } else {
      data->state = LV_INDEV_STATE_RELEASED;
      trackball_btn_pressed = false;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  nav_input_tick_begin();

  // All points, so controller mode can hold a direction and a button at
  // once; LVGL's pointer only ever uses point 0 (it is single-point).
  int16_t xs[INPUT_DEV_TOUCH_MAX], ys[INPUT_DEV_TOUCH_MAX];
  int n = input_dev_touch_read_multi(xs, ys, INPUT_DEV_TOUCH_MAX);
  if (n > 0) {
    // Controller mode intercepts EVERY touch before LVGL sees a pointer:
    // zone hits become held keys (merged in keyboard_read_cb), everything
    // else is swallowed.
    if (input_zones_touch_multi(xs, ys, n)) {
      data->state = LV_INDEV_STATE_RELEASED;
      firmware_note_activity();
      firmware_wake_restore();
      return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = xs[0];
    data->point.y = ys[0];
    firmware_note_activity();
    firmware_wake_restore();

    // Touch disarms gridnav on the top scope so the finger scrolls instead of
    // moving focus; trackball re-arms it (above). Safe point: indev callback,
    // not inside a gridnav event dispatch.
    nav_disarm_on_touch();
  } else {
    input_zones_touch_multi(nullptr, nullptr, 0);   // release every held zone
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void input_ui_init(void (*prefs_save)(void)) {
  s_prefs_save = prefs_save;
}

// ── On-screen keyboard trigger (keyboardless boards) ───────────────────────
// A textarea gaining focus queues the OSK (for a touch: on the lift, see
// osk_group_focus_cb); loop()'s dispatch_osk() opens lib/osk.lua — same
// pending-flag pattern as the emoji popup. The captured target pointer is
// consumed by the _osk_* bindings in main.cpp, which re-validate it before
// every use.
static volatile bool s_osk_pending = false;
static lv_obj_t *s_osk_target = NULL;
static bool s_osk_active = false;

bool input_ui_take_osk(void) {
  if (!s_osk_pending) return false;
  s_osk_pending = false;
  return true;
}

lv_obj_t* input_ui_osk_target(void) { return s_osk_target; }
void input_ui_osk_set_active(bool on) { s_osk_active = on; }
void input_ui_osk_release(void) { s_osk_target = NULL; }

// ── Touch input mode (contract: input_ui.h) ────────────────────────────────
static uint8_t s_touch_mode = TOUCH_MODE_OFF;

void input_ui_touch_mode_init(void) {
  // No touch panel: the mode can never do anything, so leave it OFF. With a
  // keyboard the user opts in via the chord; without one, touch controls are
  // the only input there is, so they start live.
  if (!input_dev_has_touch())          s_touch_mode = TOUCH_MODE_OFF;
  else if (input_dev_has_keyboard())   s_touch_mode = TOUCH_MODE_OFF;
  else                                 s_touch_mode = TOUCH_MODE_PAD;
}

uint8_t input_ui_touch_mode(void) { return s_touch_mode; }

void input_ui_touch_mode_set(uint8_t mode) {
  if (mode < TOUCH_MODE_COUNT) s_touch_mode = mode;
}

uint8_t input_ui_touch_mode_cycle(bool has_pad) {
  if (!input_dev_has_touch()) return s_touch_mode;   // stays OFF
  uint8_t m = s_touch_mode;
  for (int i = 0; i < TOUCH_MODE_COUNT; i++) {
    m = (uint8_t)((m + 1) % TOUCH_MODE_COUNT);
    if (!has_pad && (m == TOUCH_MODE_PAD || m == TOUCH_MODE_PAD_HIDDEN)) continue;
    break;
  }
  s_touch_mode = m;
  return m;
}

bool input_ui_take_touch_chord(void) {
  if (!s_touch_chord_pending) return false;
  s_touch_chord_pending = false;
  return true;
}

// A touch press focuses the textarea on the PRESS edge (LVGL runs click-focus
// right after LV_EVENT_PRESSED), and opening the OSK from that focus puts the
// modal over the textarea while the finger is still down. So for a touch the
// focus callback only plants a SHORT_CLICKED hook on the target. Objects carry
// LV_OBJ_FLAG_PRESS_LOCK by default, so the textarea keeps the press through
// any popup its LONG_PRESSED handler spawns and receives CLICKED on the lift
// regardless; SHORT_CLICKED is sent only for a press that never reached the
// long-press threshold, and never after a scroll. A tap opens the OSK on the
// lift; a hold never does. Focus from any other indev (keypad / nav buttons /
// programmatic) has no lift to wait for and queues the OSK at once. The hook
// is planted remove+add: one per object, no address memory (a rebuilt
// textarea can reuse a freed one's address). It dies with the object.
static void osk_target_clicked_cb(lv_event_t *e) {
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  if (s_osk_active || s_touch_mode == TOUCH_MODE_OFF) return;
  if (obj && lv_obj_is_valid(obj) && lv_obj_check_type(obj, &lv_textarea_class)) {
    s_osk_target  = obj;
    s_osk_pending = true;
  }
}

static void osk_hook(lv_obj_t *obj) {
  lv_obj_remove_event_cb(obj, osk_target_clicked_cb);
  lv_obj_add_event_cb(obj, osk_target_clicked_cb, LV_EVENT_SHORT_CLICKED, NULL);
}

static void osk_group_focus_cb(lv_group_t *g) {
  if (s_osk_active || s_touch_mode == TOUCH_MODE_OFF) return;
  lv_obj_t *obj = lv_group_get_focused(g);
  if (!(obj && lv_obj_is_valid(obj) && lv_obj_check_type(obj, &lv_textarea_class))) return;
  osk_hook(obj);
  lv_indev_t *act = lv_indev_active();
  bool touch_press = act && lv_indev_get_type(act) == LV_INDEV_TYPE_POINTER &&
                     lv_indev_get_active_obj() == obj;
  if (touch_press) return;   // opens from osk_target_clicked_cb on a short tap's lift
  s_osk_target  = obj;
  s_osk_pending = true;
}

void input_ui_setup_indevs(lv_display_t* disp) {
  // Register a touchscreen input device
  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, touchpad_read_cb);
  lv_indev_set_display(touch_indev, disp);

  // Register the keypad indev on EVERY board: with no physical keyboard the
  // same callback still delivers USB-keyboard keys, nav pulses (buttons /
  // USB arrows) and — with controller mode — synthesized zone keys, so apps
  // reading LVGL key events work identically on keyboardless boards.
  lv_indev_t *kb_indev = lv_indev_create();
  lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(kb_indev, keyboard_read_cb);

  // Connect keyboard to the default group
  lv_indev_set_group(kb_indev, lv_group_get_default());

  SLog.println("Keyboard input device registered with LVGL");

  // Textarea focus opens the on-screen keyboard. Registered on every board
  // with a touch panel: the callback itself is inert while the touch-input
  // mode is OFF (the boot default wherever a physical keyboard exists), so
  // the Shift+Alt chord can enable it at runtime with no re-registration.
  if (input_dev_has_touch()) {
    lv_group_set_focus_cb(lv_group_get_default(), osk_group_focus_cb);
  }
}

// ── Pref accessors ─────────────────────────────────────────────────────────

uint16_t input_ui_trackball_sens_get(void)       { return trackball_sensitivity_ms; }
void     input_ui_trackball_sens_set(uint16_t v) { trackball_sensitivity_ms = v; }
uint16_t input_ui_trackball_roll_get(void)       { return trackball_roll_ms; }
void     input_ui_trackball_roll_set(uint16_t v) { trackball_roll_ms = v; }
bool     input_ui_sym_toggle_get(void)           { return kb_sym_toggle_pref; }
void     input_ui_sym_toggle_set(bool on)        { kb_sym_toggle_pref = on;
                                                   if (!on) kb_sym_latched = false; }
bool     input_ui_alt_toggle_get(void)           { return kb_alt_toggle_pref; }
void     input_ui_alt_toggle_set(bool on)        { kb_alt_toggle_pref = on;
                                                   if (!on) kb_alt_latched = false; }

// ── Lua bindings (names unchanged from the pre-split main.cpp) ─────────────

void input_ui_register_lua(lua_State *L) {
  // ── Controller-mode zones (lib/touchlayout.lua) ─────────────────────────
  // _zones_set{ {x=,y=,w=,h=,out=,label=}, ... } loads a layout;
  // _zones_enable arms/disarms it (all touch intercepted while armed).
  lua_register(L, "_zones_set", [](lua_State *L) -> int {
    luaL_checktype(L, 1, LUA_TTABLE);
    InputZone zs[INPUT_ZONES_MAX];
    int n = 0;
    int len = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= len && n < INPUT_ZONES_MAX; i++) {
      lua_rawgeti(L, 1, i);
      if (lua_istable(L, -1)) {
        InputZone *z = &zs[n];
        memset(z, 0, sizeof(*z));
        lua_getfield(L, -1, "x");     z->x = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "y");     z->y = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "w");     z->w = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "h");     z->h = (int16_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "out");   z->out = (uint8_t)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "label");
        const char *lb = lua_tostring(L, -1);
        if (lb) { strncpy(z->label, lb, sizeof(z->label) - 1); }
        lua_pop(L, 1);
        n++;
      }
      lua_pop(L, 1);
    }
    input_zones_set(zs, n);
    lua_pushinteger(L, n);
    return 1;
  });
  lua_register(L, "_zones_clear", [](lua_State *L) -> int {
    input_zones_clear();
    return 0;
  });
  lua_register(L, "_zones_enable", [](lua_State *L) -> int {
    input_zones_enable(lua_toboolean(L, 1));
    return 0;
  });
  lua_register(L, "_zones_enabled", [](lua_State *L) -> int {
    lua_pushboolean(L, input_zones_enabled() ? 1 : 0);
    return 1;
  });

  // ── Touch diagnostics (Tools/Touch Test) ────────────────────────────────
  // _touch_raw() -> rx, ry, drops. rx/ry are the controller's own numbers
  // for the last accepted sample, BEFORE the board's raw->screen transform;
  // drops counts frames the backend threw away as invalid. Lets the
  // measurement app show what the panel reported next to where the UI put
  // the finger, without the app needing to know the board's geometry.
  lua_register(L, "_touch_raw", [](lua_State *L) -> int {
    // NOTE: lua_register is a MACRO. A comma at this brace level splits its
    // argument list — braces do not protect commas, only parentheses do —
    // so every declaration here stays on its own line.
    InputTouchRaw t;
    memset(&t, 0, sizeof(t));
    input_dev_touch_raw(&t);
    lua_newtable(L);
    lua_pushinteger(L, t.x0);              lua_setfield(L, -2, "x0");
    lua_pushinteger(L, t.y0);              lua_setfield(L, -2, "y0");
    lua_pushinteger(L, t.x1);              lua_setfield(L, -2, "x1");
    lua_pushinteger(L, t.y1);              lua_setfield(L, -2, "y1");
    lua_pushinteger(L, t.points);          lua_setfield(L, -2, "points");
    lua_pushinteger(L, (lua_Integer)t.drops); lua_setfield(L, -2, "drops");
    return 1;
  });

  // ── Touch input mode (lib/touchlayout.lua applies it) ───────────────────
  // _touch_mode() -> current mode; _touch_mode_cycle(has_pad) -> next mode.
  // The Lua side passes has_pad because only it knows whether the running
  // app ships a controller layout.
  lua_register(L, "_touch_mode", [](lua_State *L) -> int {
    lua_pushinteger(L, input_ui_touch_mode());
    return 1;
  });
  lua_register(L, "_touch_mode_cycle", [](lua_State *L) -> int {
    lua_pushinteger(L, input_ui_touch_mode_cycle(lua_toboolean(L, 1)));
    return 1;
  });

  // Device capability table for Lua (Settings/apps adapt per device).
  lua_register(L, "_input_caps", [](lua_State *L) -> int {
    lua_newtable(L);
    lua_pushboolean(L, input_dev_has_keyboard());      lua_setfield(L, -2, "keyboard");
    lua_pushboolean(L, input_dev_has_trackball());     lua_setfield(L, -2, "trackball");
    lua_pushboolean(L, input_dev_has_touch());         lua_setfield(L, -2, "touch");
    lua_pushboolean(L, input_dev_has_kbd_backlight()); lua_setfield(L, -2, "kbd_backlight");
    return 1;
  });

  // ── Trackball sensitivity ───────────────────────────────────────────────
  lua_register(L, "_trackball_sensitivity_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 500) v = 500;
    trackball_sensitivity_ms = (uint16_t)v;
    if (s_prefs_save) s_prefs_save();
    lua_pushinteger(L, trackball_sensitivity_ms);
    return 1;
  });
  lua_register(L, "_trackball_sensitivity_get", [](lua_State* L) -> int {
    lua_pushinteger(L, trackball_sensitivity_ms);
    return 1;
  });
  lua_register(L, "_trackball_roll_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 500) v = 500;
    trackball_roll_ms = (uint16_t)v;
    if (s_prefs_save) s_prefs_save();
    lua_pushinteger(L, trackball_roll_ms);
    return 1;
  });
  lua_register(L, "_trackball_roll_get", [](lua_State* L) -> int {
    lua_pushinteger(L, trackball_roll_ms);
    return 1;
  });

  // Sym key behavior: false = hold modifier (default), true = tap toggles
  // the symbol layer (hold still works as momentary). Persisted.
  lua_register(L, "_kb_sym_toggle_set", [](lua_State* L) -> int {
    kb_sym_toggle_pref = lua_toboolean(L, 1);
    if (!kb_sym_toggle_pref) kb_sym_latched = false;
    if (s_prefs_save) s_prefs_save();
    return 0;
  });
  lua_register(L, "_kb_sym_toggle_get", [](lua_State* L) -> int {
    lua_pushboolean(L, kb_sym_toggle_pref ? 1 : 0);
    return 1;
  });

  // Alt key behavior: false = hold modifier (default), true = tap toggles
  // the emoji layer (hold still works as momentary). Persisted.
  lua_register(L, "_kb_alt_toggle_set", [](lua_State* L) -> int {
    kb_alt_toggle_pref = lua_toboolean(L, 1);
    if (!kb_alt_toggle_pref) kb_alt_latched = false;
    if (s_prefs_save) s_prefs_save();
    return 0;
  });
  lua_register(L, "_kb_alt_toggle_get", [](lua_State* L) -> int {
    lua_pushboolean(L, kb_alt_toggle_pref ? 1 : 0);
    return 1;
  });

  // Legacy ASCII keyboard mode (old keyboard-MCU firmware without raw matrix
  // support). Live-flips the MCU mode command both directions; no reboot
  // needed. A manual OFF also blocks the auto-switch until the next boot so
  // detection doesn't fight the user's choice. Persisted.
  lua_register(L, "_kb_legacy_set", [](lua_State* L) -> int {
    bool on = lua_toboolean(L, 1);
    if (!on) input_dev_kbd_autoswitch_close();
    input_ui_set_legacy(on, /*save=*/true);
    lua_pushboolean(L, input_dev_kbd_legacy_get() ? 1 : 0);
    return 1;
  });
  lua_register(L, "_kb_legacy_get", [](lua_State* L) -> int {
    lua_pushboolean(L, input_dev_kbd_legacy_get() ? 1 : 0);
    return 1;
  });

  // Alt emoji layer keymap. Keys are identified by their normal-layer char
  // ("a".."z", "$"); codepoints may be blob singles OR sequence PUAs.
  lua_register(L, "_kb_emoji_get", [](lua_State* L) -> int {
    const char *k = luaL_checkstring(L, 1);
    uint8_t c = (uint8_t)k[0];
    lua_pushinteger(L, (c && c < 128) ? (lua_Integer)kb_emoji_map[c] : 0);
    return 1;
  });
  lua_register(L, "_kb_emoji_set", [](lua_State* L) -> int {
    const char *k = luaL_checkstring(L, 1);
    uint32_t cp = (uint32_t)luaL_checkinteger(L, 2);   // 0 clears the key
    uint8_t c = (uint8_t)k[0];
    if (!c || c >= 128) { lua_pushboolean(L, 0); return 1; }
    kb_emoji_map[c] = cp;
    kb_emoji_map_save();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_kb_emoji_reset", [](lua_State* L) -> int {
    kb_emoji_apply_defaults();
    LittleFS.remove("/emoji_keymap");
    return 0;
  });

  // _emoji_popup_insert(cp) -> bool. Insert one emoji at the cursor of the
  // textarea captured at alt+mic time (s_emoji_popup_target). Re-validates the
  // pointer — the app underneath can rebuild its views while the popup is up —
  // and gates on emoji_preload like the alt layer, so a stale target or an
  // unrenderable codepoint returns false instead of emitting tofu.
  lua_register(L, "_emoji_popup_insert", [](lua_State* L) -> int {
    uint32_t cp = (uint32_t)luaL_checkinteger(L, 1);
    lv_obj_t *ta = s_emoji_popup_target;
    bool ok = cp > 0 && ta && lv_obj_is_valid(ta) &&
              lv_obj_check_type(ta, &lv_textarea_class) && emoji_preload(cp);
    if (ok) {
      uint32_t packed = utf8_pack_key(cp);   // UTF-8 bytes, low byte first
      char buf[5] = {0};
      memcpy(buf, &packed, sizeof(packed));
      lv_textarea_add_text(ta, buf);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  });

  // _emoji_popup_release(): drop the captured insert target (popup closed).
  lua_register(L, "_emoji_popup_release", [](lua_State* L) -> int {
    s_emoji_popup_target = NULL;
    return 0;
  });

  // Input capture (Gamepad mapping wizard): arm, poll until a code arrives,
  // stop to disarm. While armed keyboard_read_cb swallows ALL keyboard and
  // trackball input and reports the first press as a driver code.
  lua_register(L, "_input_capture_start", [](lua_State *L) -> int {
    s_input_captured      = 0;
    s_input_capture_armed = true;
    return 0;
  });
  lua_register(L, "_input_capture_poll", [](lua_State *L) -> int {
    if (s_input_captured) {
      lua_pushinteger(L, (lua_Integer)s_input_captured);
      s_input_captured = 0;
    } else {
      lua_pushnil(L);
    }
    return 1;
  });
  lua_register(L, "_input_capture_stop", [](lua_State *L) -> int {
    s_input_capture_armed = false;
    s_input_captured      = 0;
    return 0;
  });

  lua_register(L, "_kb_is_down", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && kb_key_state[key]);
    return 1;
  });

  lua_register(L, "_kb_just_pressed", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && kb_key_state[key] && !kb_key_prev[key]);
    return 1;
  });

  lua_register(L, "_kb_just_released", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && !kb_key_state[key] && kb_key_prev[key]);
    return 1;
  });

  lua_register(L, "_kb_is_held", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    bool held = false;
    if (key >= 0 && key < 128 && kb_key_state[key] && kb_key_press_time[key] > 0) {
      held = (millis() - kb_key_press_time[key]) > KEY_HOLD_THRESHOLD_MS;
    }
    lua_pushboolean(L, held);
    return 1;
  });

  lua_register(L, "_kb_hold_duration", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    if (key >= 0 && key < 128 && kb_key_state[key] && kb_key_press_time[key] > 0) {
      lua_pushinteger(L, millis() - kb_key_press_time[key]);
    } else {
      lua_pushinteger(L, 0);
    }
    return 1;
  });

  lua_register(L, "_kb_shift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_shift_active);
    return 1;
  });

  lua_register(L, "_kb_lshift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_lshift_active);
    return 1;
  });

  lua_register(L, "_kb_rshift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_rshift_active);
    return 1;
  });

  lua_register(L, "_kb_sym", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_sym_active);
    return 1;
  });

  lua_register(L, "_kb_alt", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_alt_active);
    return 1;
  });
}
