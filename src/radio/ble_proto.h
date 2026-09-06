#pragma once

#include "ble_proto_abi.h"

// ── BLE protocol slot: selection + dispatch ──────────────────────────────────
// Mirrors the LoRa slot's loader (proto_loader.h). Boot flow:
//   1. firmware_prefs_load resolves ble_protocol= via ble_proto_set_requested
//   2. ble_proto_select_and_init() — early in setup(), AFTER the LoRa
//      protocol selection (requires_lora checks read lora_proto_active())
//      and before LVGL/Lua (BLE-stack allocs land low in internal SRAM)
//   3. ble_proto_start() — after the LoRa protocol is up
//   4. mesh_task ticks ble_proto_loop() (Core 1)
// stop/resume manage the RUNTIME of the current selection (standby, module
// runs borrowing internal SRAM); apply changes the SELECTION live.
// All runtime transitions take MESH_LOCK internally (recursive — callers
// already holding it compose fine).

void        ble_proto_set_requested(const char* id);   // sanitized; persisted by firmware_prefs
const char* ble_proto_requested(void);
const char* ble_proto_active(void);      // selected this boot ("none" when empty)
bool        ble_proto_running(void);

void ble_proto_select_and_init(void);
void ble_proto_start(void);
void ble_proto_loop(void);
void ble_proto_stop(void);               // full stop of the current selection
void ble_proto_resume(void);             // init+start the current selection again
void ble_proto_flush(void);

// Live selection change (picker/bindings): validates the target against the
// ACTIVE LoRa protocol (requires_lora — refused, never degraded), stops the
// current protocol, starts the new one. On refusal nothing changes and
// *reason names why. The caller persists via firmware_prefs_save.
bool ble_proto_apply(const char* id, const char** reason);

// One-shot boot notice (dependency refused / init failed), for notify_post
// once the notify system is up. NULL when the boot was clean.
const char* ble_proto_boot_notice(void);

// The NimBLE serial transport backing BleHostApi (ble_transport.cpp).
bool ble_transport_open(const char* name_prefix, const char* dev_name,
                        uint32_t pin);
void ble_transport_close(void);
void ble_transport_enable(void);
void ble_transport_disable(void);
bool ble_transport_connected(void);
bool ble_transport_write_busy(void);
int  ble_transport_write(const uint8_t* frame, int len);
int  ble_transport_read(uint8_t* buf);
