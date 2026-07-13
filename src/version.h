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
//   1  2026-07-13  first exposure (Alt+Backspace ELF exit chord release)
#define MESHPUNK_FW_API 1

// BLE companion protocol identity (reported in the DEVICE_INFO frame — see
// ble_companion.cpp). Versioned separately from MESHPUNK_FW_API on purpose:
// this tracks what BLE client apps understand, not what store apps need.
#define MESHPUNK_FW_VER_CODE     11
#define MESHPUNK_FW_VERSION      "v1.15.0"
#define MESHPUNK_FW_BUILD_DATE   "16 May 2026"
