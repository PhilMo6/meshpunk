// The USB host subsystem lives in src/usb/:
//
//   usb/usb_core.cpp      — host task, pump, enumeration, driver registry,
//                           pipe layer, input/block sockets, flash guard,
//                           lifecycle + Lua bridge
//   usb/usb_driver_abi.h  — the UsbHostApi/UsbDriverDesc contract (shared
//                           verbatim with dynamic .drv.elf module builds)
//   usb/usb_core_int.h    — internal surface for the in-tree drivers
//   usb/usb_drv_audio.cpp — UAC 1.0 audio out (core-privileged: ISO + sink)
//   usb/usb_drv_hid.cpp   — HID boot keyboard (vtable-clean)
//   usb/usb_drv_msc.cpp   — MSC thumb drive (vtable-clean)
//   usb/usb_pool.cpp      — boot-reserved PSRAM pool for dynamic driver segments
//
// The public surface (usb_manager.h) is unchanged — consumers never see the
// split. This file is intentionally empty: it is a signpost from the old
// usb_manager.cpp name to that layout.
