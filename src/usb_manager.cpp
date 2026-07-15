// The USB host subsystem moved to src/usb/ (M1 of the downloadable-drivers
// migration, 2026-07-13):
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
//
// The public surface (usb_manager.h) is unchanged — consumers never see the
// split. This stub exists so old references to "usb_manager.cpp" in notes
// and memory land somewhere useful.
