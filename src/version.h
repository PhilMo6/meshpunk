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
#define MESHPUNK_FW_API 7

// BLE companion protocol identity (reported in the DEVICE_INFO frame — see
// ble_companion.cpp). Versioned separately from MESHPUNK_FW_API on purpose:
// this tracks what BLE client apps understand, not what store apps need.
#define MESHPUNK_FW_VER_CODE     11
#define MESHPUNK_FW_VERSION      "v1.15.0"
#define MESHPUNK_FW_BUILD_DATE   "16 May 2026"
