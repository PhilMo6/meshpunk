#pragma once

// FatFs/VFS layer for the USB MSC thumb drive (drive "U:"). Sits on the
// sector API usb_manager.h exports and mounts the stick as a FATFS volume at
// VFS path /usb, wrapped in an Arduino fs::FS so fs_bridge / io.open / the
// Audio library use it exactly like SD and LittleFS.
//
// Lifecycle is driven by the MSC driver in usb_manager.cpp: usb_fs_mount()
// from msc_start (usb_task context — its sector reads self-pump through the
// MSC dual-context wait), usb_fs_unmount() from msc_stop. Everything else
// may be called from any task.
//
// FAT12/16/32 only — exFAT is compiled out of the prebuilt FatFs, so sticks
// >32GB (which ship exFAT) must be reformatted FAT32. FF_VOLUMES is 2 and SD
// owns one slot, so exactly one USB volume can be mounted (enough: the host
// is single-device anyway).

#include <FS.h>
#include <stdint.h>

bool    usb_fs_mount();      // usb_task only; logs the failure reason
void    usb_fs_unmount();    // usb_task only; safe when not mounted
bool    usb_fs_mounted();
fs::FS& usb_fs();            // rooted at /usb; ops fail cleanly when unmounted

// Volume totals in bytes (false when unmounted). Sizes can exceed 4GB —
// callers pushing to Lua must use numbers, not integers (fs_bridge rule).
bool usb_fs_df(uint64_t* total, uint64_t* free_bytes);
