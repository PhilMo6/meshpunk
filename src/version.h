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
#define MESHPUNK_FW_API 3

// BLE companion protocol identity (reported in the DEVICE_INFO frame — see
// ble_companion.cpp). Versioned separately from MESHPUNK_FW_API on purpose:
// this tracks what BLE client apps understand, not what store apps need.
#define MESHPUNK_FW_VER_CODE     11
#define MESHPUNK_FW_VERSION      "v1.15.0"
#define MESHPUNK_FW_BUILD_DATE   "16 May 2026"
