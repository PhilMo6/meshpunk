#pragma once

// OTA update, main-firmware side (ota_update.cpp). The image is staged on the
// SD card or LittleFS and written by the updater partition after a reboot;
// the job-file contract and the updater's exit paths are documented in
// src/updater/main_updater.cpp.

struct lua_State;

// Boot report: partition layout mode, running/main/updater labels, installed
// release (L:/.pack_version) and any staged image. Removes a leftover
// L:/.ota_job. Call after LittleFS and the SD card are mounted.
void ota_init_report(void);

// Registers the _ota_* Lua bindings (Settings/Firmware).
void ota_register_lua(lua_State* L);

// Release channel: "stable" or "dev", selecting which GitHub repo _ota_check
// polls and _ota_begin downloads from. Persisted by main.cpp in
// /firmware_prefs as `ota_channel=`; set sanitizes, and any id that is not a
// known channel becomes "stable".
const char* ota_channel_requested(void);
void        ota_channel_set_requested(const char* id);
