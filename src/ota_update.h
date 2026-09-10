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
