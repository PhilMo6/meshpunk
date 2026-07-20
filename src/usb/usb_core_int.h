#pragma once

// ── USB core INTERNAL surface ────────────────────────────────────────────────
// Shared only among src/usb/*.cpp. Everything a built-in driver needs beyond
// the public ABI vtable lives here; nothing outside src/usb/ may include it.
// The audio driver is "core-privileged" (ISO + the sink ring + descriptor
// patching are welded to the firmware's sound path), so it gets the raw
// client/device handles and the usb_task-only ctrl_req. The HID and MSC
// drivers are deliberately vtable-clean — they include this header ONLY for
// the descriptor-getter declarations and registration types.

#include "usb_driver_abi.h"
#include <usb/usb_host.h>

// ── Core accessors (usb_core.cpp) ────────────────────────────────────────────
void usbcore_log(const char* fmt, ...);              // the ulog ring
usb_host_client_handle_t usbcore_client(void);
usb_device_handle_t      usbcore_dev(void);
const UsbHostApi*        usbcore_api(void);
bool usbcore_flash_pause_req(void);

// EP0 control request — usb_task context ONLY (self-pumps both event
// handlers). Drivers off usb_task use api->control instead.
bool usbcore_ctrl_req(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                      uint16_t wIndex, const uint8_t* data, uint16_t wLength);

// Audio probe reports the negotiated stream so the core's device info (and
// the Lua `_usb_device` table) can carry rate/bits + kind=audio.
void usbcore_note_audio(uint32_t rate, uint8_t bits);

// ── Registry (usb_core.cpp) ──────────────────────────────────────────────────
// Built-ins register once from usb_manager_init (order = lifecycle order:
// audio, kbd, msc — preserved from the monolith). Dynamic add/remove (the
// .drv.elf loader) runs in usb_task only.
void usb_registry_add_builtin(const UsbDriverDesc* d);
bool usb_registry_probed(const char* name);          // Lua bridge status

// ── Built-in driver descriptors ──────────────────────────────────────────────
const UsbDriverDesc* usbaud_desc(void);              // usb_drv_audio.cpp
const UsbDriverDesc* usbkbd_desc(void);              // usb_drv_hid.cpp
const UsbDriverDesc* usbmsc_desc(void);              // usb_drv_msc.cpp

// ── Audio internals the core lifecycle/bridge needs (usb_drv_audio.cpp) ─────
bool usbaud_session_reset(void);   // alloc/reset the PSRAM sink ring + clear
                                   // the tone flag; false = PSRAM alloc failed
bool usbaud_tone_toggle(void);     // Lua debug-tone toggle; returns new state
int  usbaud_iso_busy(void);        // in-flight ISO count (guard-begin fallback)

// ── MSC internals the core bridge needs (usb_drv_msc.cpp) ───────────────────
double usbmsc_capacity_mb(void);   // 0 when no unit is ready

// ── Shared with device-mode drive sessions (usb_msc_dev.cpp) ────────────────
// Muxes the USB pads back to the Serial-JTAG peripheral (defined in
// usb_core.cpp; used by both the host-stop path and drive-mode stop).
void restore_serial_jtag_phy(void);
