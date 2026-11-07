// sd_dev.h — per-device microSD backend contract.
//
// Same selection model as the other *_dev.h seams: exactly one backend
// compiles per build via the board define (sd_spi.cpp for the T-Deck and
// the Heltec kit, sd_wio_l2.cpp for the Wio L2). The backend owns the
// card's bus, power and mount; callers never see the card library.
//
// Every backend mounts the card's FAT volume at VFS "/sd": the S:/ drive,
// Lua io paths, fs_bridge's directory walks and the protocol data root all
// depend on that mountpoint.
//
// Callers keep the firmware's existing card discipline around these calls:
// sd_spi_take()/sd_spi_release() and the sd_mounted flag (main.cpp).

#pragma once

#include <stdint.h>
#include <FS.h>

// Mount the card at /sd (bus bring-up included); true when mounted. Each
// attempt is logged.
bool sd_dev_mount(void);

// Unmount the volume. Safe when nothing is mounted.
void sd_dev_unmount(void);

// The volume as an fs::FS — the same object for the whole run; file calls
// fail cleanly while nothing is mounted.
fs::FS& sd_dev_fs(void);

// Card capacity, and the FAT volume's total/used bytes; 0 when unmounted.
uint64_t sd_dev_card_size(void);
uint64_t sd_dev_total_bytes(void);
uint64_t sd_dev_used_bytes(void);

// Raw sector access for USB drive mode (usb_msc_dev.cpp), one sector per
// call. The volume stays mounted underneath while the firmware leaves it
// alone for the session.
uint32_t sd_dev_num_sectors(void);
uint32_t sd_dev_sector_size(void);
bool     sd_dev_read_raw(uint8_t* buf, uint32_t sector);
bool     sd_dev_write_raw(const uint8_t* buf, uint32_t sector);
