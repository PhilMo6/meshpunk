#pragma once

// ─── Meshpunk firmware version identities (single bump site) ────────────────
//
// MESHPUNK_FW_API: monotonic integer describing the contract installable apps
// can depend on (Lua bindings, ELF host_exports, firmware-side behaviors like
// the Alt+Backspace exit chord). Registered at Lua boot as the _FW_API global
// (with MESHPUNK_FW_VERSION as _FW_VERSION for display); the App Library
// refuses catalog entries whose min_fw exceeds it. Firmware that predates the
// global reads as 0 on the Lua side, so every gated entry blocks there — the
// safe default. Bump this whenever a release adds/changes anything a store
// app could require.
// Bump at most ONCE per release cycle: check the level the latest release
// tag shipped (`git show <latest-tag>:src/version.h`) — if the current level
// hasn't shipped yet, fold new contract changes into it instead of bumping.
//   1  2026-07-13  first exposure (Alt+Backspace ELF exit chord release)
//   2  2026-07-17  everything unreleased since the v0.2.6 tag (which shipped
//                  level 1): dynamic USB drivers (_usb_drivers binding, L:/S:
//                  driver dirs, driver pool — Tools/USB Drivers manager),
//                  T-Deck peer link (tdeck_link bridge, host_link_* ELF
//                  exports, driver-ABI link socket — tdeck driver +
//                  link-cable GameBoy), shared emoji picker (lib/emoji_popup
//                  + _emoji_popup_insert, alt+mic insert popup —
//                  Settings/Emoji store app), alt+backspace home chord
//                  (documented by the Read Me store app)
//   3  2026-07-19  unreleased since the v0.2.7 tag (which shipped level 2):
//                  USB drive mode (_usbdrive_* bindings, usb_msc_dev.cpp —
//                  Tools/"USB Drive" store app) + apps.set_on_close /
//                  apps.close_all_backgrounds in lib/apps.lua
//   4  2026-07-22  unreleased since the v0.2.8 tag (which shipped level 3):
//                  module worker tasks (host_spawn_task / host_task_join ELF
//                  exports, session-cleanup force-delete — NGPC Core-1
//                  render worker)
//   5  2026-07-23  NEXT RELEASE (v0.3.0), unreleased since the v0.2.9 tag
//                  (which shipped level 4): baked-bubble Messenger chat —
//                  lv_snapshot render (LV_USE_SNAPSHOT + _snapshot_take /
//                  _snapshot_free / _snapshot_attach_free), disk-paged chat
//                  window (_mesh_chat_page_channel / _mesh_chat_page_dm +
//                  the luavgl obj:update_layout patch), counted channel
//                  repeat-until-heard indicator (3-value _mesh_get_repeat_status
//                  = status, remaining, total), gridnav edge-lock (lv_gridnav.c
//                  meshpunk_gridnav_edge_lock + _gridnav_edge_lock binding),
//                  and _touch_pressed (defer chat paging until touch release)
//   6  2026-07-29  unreleased since the v0.3.0 tag (which shipped level 5):
//                  the Dos (386) emulator's host contract — ELF exports
//                  host_trackball_button (live trackball button LEVEL, for
//                  real press/hold/drag), host_key_mods (matrix shift/alt/sym
//                  levels, which never reach a module as key events),
//                  host_kb_blink (one notification-style keyboard-backlight
//                  blink as toggle feedback) and truncate (the FAT VFS's, so
//                  a module can shrink a file without a copy-and-swap) —
//                  plus the ALT+Enter binding-layer toggle a module opts into
//                  with -kbtoggle N and Shift+Backspace = Esc in ELF modules
//   7  2026-07-30  unreleased since the v0.3.1 tag (which shipped level 6):
//                  legacy ASCII keyboard mode for pre-250620 keyboard-MCU
//                  firmware — _kb_legacy_get / _kb_legacy_set bindings
//                  (Settings/Device toggle), old-firmware auto-detection +
//                  auto-switch with notification + toast, single-byte input in
//                  both the LVGL reader and the ELF host (no exit chord in
//                  legacy mode — restart the device to leave a module)
//   8  2026-07-31  unreleased since the v0.3.2 tag (which shipped level 7):
//                  lib/keybind.lua — the shared binding system every ELF
//                  launcher now requires (key table, Controls + picker +
//                  trackball Input screens, config lines, -keymap/-trkball
//                  strings, Detect-a-keypress capture); a bindable QUIT
//                  (keymap output 0xFF, swallowed host-side, exits through
//                  host_should_exit) which replaces "restart the device" as
//                  the legacy-keyboard exit; and the legacy binding-layer
//                  sequence 'p', Backspace, Enter — the twin of ALT+Enter for
//                  keyboards that report no modifiers, same -kbtoggle opt-in;
//                  a launcher's -stackkb N as a CEILING on the module task
//                  stack, then descending through the built-in rungs below it
//                  (it previously appended one rung UNDER the fixed
//                  64/48/32KB ladder, so a module that declared its depth was
//                  still handed the larger stack whenever one fit), which
//                  leaves internal SRAM for a module's own worker tasks; and
//                  an optional second argument on _wifi_set_enabled /
//                  _ble_set_enabled, persist (default true) — false applies
//                  the change to the live radio and the in-RAM pref but skips
//                  firmware_prefs_save(), so the next boot restores the saved
//                  state (the Snes launcher uses both to fit the Speed
//                  renderer's Core-1 worker into internal SRAM); and the raw
//                  packet capture ring — _mesh_pkt_capture(on) arms/frees it
//                  (48 entries in PSRAM, allocated only while armed) and
//                  _mesh_pkt_poll(max) -> frames, dropped drains it oldest
//                  first, each frame { seq, ts, ms, dir, parsed, snr, rssi,
//                  score, len, hash, raw } with dir "rx"/"tx"/"txfail" and
//                  raw the full wire frame as hex (Tools/Packets store app);
//                  and two NEW ELF host_exports for band renderers —
//                  host_blit_rect_async(buf,x,y,w,h) (the async form of
//                  host_blit_rect: the Core-1 push task now carries an x/y
//                  rect, so a module can hand over one finished band and
//                  rasterise the next into a second buffer while this one
//                  goes out; one-deep back-pressure, it returns when the
//                  PREVIOUS push completed, so two alternating buffers are
//                  enough) and host_blit_wait() (block until no push is in
//                  flight — a module MUST call it before freeing a buffer
//                  the push task may still be reading). A module importing
//                  either fails to LOAD on level 7 with an unresolved
//                  symbol, so min_fw=8 is mandatory for it (Jet 3D)
//   9  2026-08-12  unreleased since the v0.3.4 tag (which shipped level 8):
//                  the in-memory IMAGE BRIDGE — _img_info(path),
//                  _img_open(path[,opts]) -> dsc,w,h,div, _img_scale(dsc,w,h)
//                  and _img_close([dsc]) (src/img_bridge.cpp) decode a PNG,
//                  baseline JPEG or RGB565 .bin into an app-owned PSRAM RGB565
//                  buffer and hand Lua an lv_image_dsc_t* that LVGL draws
//                  through its use-directly path — which is what lifts the
//                  512KB LV_CACHE_DEF_SIZE ceiling that silently FAILS the
//                  decode of any larger image; paired with lib/imgview.lua
//                  (fit / 1:1-pan viewer widget) and LV_USE_TJPGD 1 plus the
//                  JD_USE_SCALE vendored patch that lets TJpgDec descale while
//                  decoding, so a multi-megapixel JPEG lands in a screen-sized
//                  buffer. An app using either fails at require/nil-call time
//                  on level 8, so min_fw=9 is mandatory for it (Tools/Images);
//                  the ON-SCREEN KEYBOARD bridge for keyboardless boards —
//                  _osk_initial_text / _osk_commit / _osk_set_active /
//                  _osk_release (lib/osk.lua) connect the OSK's own preview
//                  textarea to the app textarea input_ui captured at focus
//                  time, re-validating both pointers on every use; and
//                  CONTROLLER-MODE TOUCH ZONES — _zones_set / _zones_enable /
//                  _zones_enabled / _zones_clear (lib/touchlayout.lua) load
//                  and arm an on-screen button layout that intercepts all
//                  touch while armed, with _elf_touch_layout(zones) staging
//                  the same for the NEXT module launch (OUT codes are module
//                  keycodes, 0xFF = quit; armed in elf_input_start, cleared
//                  when the module exits); the SHARED TOUCH MODE that decides
//                  when those arm — _touch_mode() and
//                  _touch_mode_cycle(has_pad) expose the OFF / PAD /
//                  PAD_HIDDEN / KB state, which also governs whether the
//                  OSK opens on textarea focus. It is NOT
//                  persisted: re-derived every boot (no touch -> OFF, keyboard
//                  -> OFF, keyboardless -> PAD) and advanced by one trigger,
//                  the board aux button or Shift+Alt; the INPUT CAPABILITY
//                  probe _input_caps() -> { keyboard, trackball, touch,
//                  kbd_backlight }, the runtime answer to what hardware this
//                  is — a board may have touch and no keyboard, so an app
//                  offering keyboard-only affordances should gate on this
//                  rather than on board identity; _touch_raw() -> { x0, y0,
//                  x1, y1, points, drops }, the panel's own coordinates
//                  BEFORE the board's raw->screen transform plus a count of
//                  frames the backend rejected (bad checksum, invalid slot,
//                  short I2C read) — feeds Tools/Touch Test; and
//                  lib/padlayout, per-launcher on-screen controller presets
//                  carrying the user's own drag/resize edits (persisted to
//                  L:/touch_layouts/<app>.cfg) into _elf_touch_layout;
//                  lib/touchlayout's app-facing API — touchlayout.set(zones)
//                  arms a Lua app's OWN on-screen controller layout (a
//                  zone's `out` is the key code it sends, which is why only
//                  the app authors it) and touchlayout.clear() drops it,
//                  registered through the normal apps.set_on_close hook
//                  AFTER the app's set_root (set_root clears callbacks
//                  registered before it); the lib holds zone plumbing and
//                  the overlay only, no per-app data (Snake, Scorched
//                  Earth); and the HELP SYSTEM — lib/helpdocs discovers
//                  guide pages (lua/help on both drives, L: wins) plus a
//                  readme.lua inside any installed app (title = the app's
//                  registry name; the file only runs when its page is
//                  opened), executing each page in a restricted env (copied
//                  string/table/math, no io/os/_G/require, text-only load)
//                  with the _input_caps and _device_caps snapshots passed
//                  as chunk arguments — a help page therefore calls NO
//                  firmware binding and carries no min_fw of its own. The
//                  Read Me app is a viewer over this lib and requires it at
//                  load, so Read Me >= 1.0.5 needs min_fw=9 (mandatory).
//                  An app calling the bindings above unguarded fails at
//                  nil-call time on level 8, so it needs min_fw=9; the ELF
//                  launchers deliberately stay at min_fw=8 by loading
//                  lib/padlayout through pcall and calling the binding
//                  behind an "if _elf_touch_layout and pad" guard, and
//                  Snake / Scorched Earth stay ungated the same way (their
//                  pcall'd require of lib/touchlayout fails on older
//                  firmware and the touch pad is simply absent). NO new ELF
//                  host_exports this cycle, so no module gains a load-time
//                  dependency on level 9
#define MESHPUNK_FW_API 9

// BLE companion protocol identity (reported in the DEVICE_INFO frame — see
// ble_companion.cpp). Versioned separately from MESHPUNK_FW_API on purpose:
// this tracks what BLE client apps understand, not what store apps need.
#define MESHPUNK_FW_VER_CODE     11
#define MESHPUNK_FW_VERSION      "v1.15.0"
#define MESHPUNK_FW_BUILD_DATE   "16 May 2026"
