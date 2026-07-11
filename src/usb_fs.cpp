// FatFs/VFS glue for the USB MSC thumb drive — see usb_fs.h.
//
// Modeled 1:1 on the framework's SD template (libraries/SD/src/sd_diskio.cpp
// + SD.cpp): grab a FatFs drive slot, register a custom diskio impl whose
// read/write land in usb_manager's SCSI sector API, register the VFS at
// /usb, f_mount, then point a generic VFSImpl-backed fs::FS at the mount.
// Teardown runs the exact reverse order so neither the drive slot nor the
// VFS registration leaks (FF_VOLUMES budget is 2 and SD holds one).

#include "usb_fs.h"
#include "usb_manager.h"

#include <vfs_api.h>          // VFSImpl — generic POSIX-over-VFS FSImpl
#include <diskio_impl.h>      // ff_diskio_get_drive / ff_diskio_register
#include <esp_vfs_fat.h>
#include <ff.h>

// ── Diskio callbacks (any task; FatFs serializes per-volume) ────────────────

static DSTATUS usbfs_init(unsigned char) {
    return usb_msc_ready() ? 0 : STA_NOINIT;
}

static DSTATUS usbfs_status(unsigned char) {
    return usb_msc_ready() ? 0 : STA_NOINIT;
}

static DRESULT usbfs_read(unsigned char, unsigned char* buff,
                          uint32_t sector, unsigned count) {
    if (!usb_msc_ready()) return RES_NOTRDY;
    return usb_msc_read(sector, count, buff) ? RES_OK : RES_ERROR;
}

static DRESULT usbfs_write(unsigned char, const unsigned char* buff,
                           uint32_t sector, unsigned count) {
    if (!usb_msc_ready()) return RES_NOTRDY;
    return usb_msc_write(sector, count, buff) ? RES_OK : RES_ERROR;
}

static DRESULT usbfs_ioctl(unsigned char, unsigned char cmd, void* buff) {
    switch (cmd) {
        case CTRL_SYNC:                       // flush device write cache
            usb_msc_sync();
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD*)buff) = usb_msc_sector_count();
            return RES_OK;
        case GET_SECTOR_SIZE:                 // REQUIRED: FF_MIN_SS != FF_MAX_SS
            *((WORD*)buff) = (WORD)usb_msc_sector_size();
            return RES_OK;
        case GET_BLOCK_SIZE:
            *((DWORD*)buff) = 1;
            return RES_OK;
    }
    return RES_PARERR;
}

// ── Mount state + the Arduino FS wrapper ────────────────────────────────────

class UsbFS : public fs::FS {
public:
    UsbFS(fs::FSImplPtr impl) : fs::FS(impl) {}
    void setMountPoint(const char* mp) { _impl->mountpoint(mp); }
};

static UsbFS  s_usbfs(fs::FSImplPtr(new VFSImpl()));   // same pattern as SD.cpp:135
static BYTE   s_pdrv    = 0xFF;
static FATFS* s_fatfs   = nullptr;
static volatile bool s_mounted = false;

fs::FS& usb_fs()        { return s_usbfs; }
bool    usb_fs_mounted() { return s_mounted; }

bool usb_fs_mount() {
    if (s_mounted) return true;
    if (ff_diskio_get_drive(&s_pdrv) != ESP_OK || s_pdrv == 0xFF) {
        usb_ulog("usbfs: no free FatFs volume slot");
        s_pdrv = 0xFF;
        return false;
    }
    static const ff_diskio_impl_t impl = {
        &usbfs_init, &usbfs_status, &usbfs_read, &usbfs_write, &usbfs_ioctl
    };
    ff_diskio_register(s_pdrv, &impl);

    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    esp_err_t err = esp_vfs_fat_register("/usb", drv, 8 /*max open files*/, &s_fatfs);
    if (err != ESP_OK) {
        usb_ulog("usbfs: vfs register failed: %s", esp_err_to_name(err));
        ff_diskio_register(s_pdrv, NULL);
        s_pdrv = 0xFF;
        return false;
    }

    FRESULT fr = f_mount(s_fatfs, drv, 1);      // 1 = mount now (reads the disk)
    if (fr != FR_OK) {
        if (fr == FR_NO_FILESYSTEM)
            usb_ulog("usbfs: no FAT found (exFAT/NTFS?) — reformat FAT32");
        else
            usb_ulog("usbfs: mount failed (FatFs err %d)", (int)fr);
        esp_vfs_fat_unregister_path("/usb");
        ff_diskio_register(s_pdrv, NULL);
        s_pdrv  = 0xFF;
        s_fatfs = nullptr;
        return false;
    }

    s_usbfs.setMountPoint("/usb");
    s_mounted = true;
    usb_ulog(">>> Drive U: mounted.");
    return true;
}

void usb_fs_unmount() {
    if (s_pdrv == 0xFF) return;                 // never mounted / already rolled back
    s_mounted = false;
    s_usbfs.setMountPoint(NULL);
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    f_mount(NULL, drv, 0);                      // f_unmount idiom
    esp_vfs_fat_unregister_path("/usb");
    ff_diskio_register(s_pdrv, NULL);
    s_pdrv  = 0xFF;
    s_fatfs = nullptr;
    usb_ulog("Drive U: unmounted.");
}

bool usb_fs_df(uint64_t* total, uint64_t* free_bytes) {
    if (!s_mounted || !s_fatfs) return false;
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    DWORD  nfree = 0;
    FATFS* fsp   = nullptr;
    if (f_getfree(drv, &nfree, &fsp) != FR_OK || !fsp) return false;
    uint64_t cluster_bytes = (uint64_t)fsp->csize * fsp->ssize;   // ssize: variable-SS build
    if (total)      *total      = (uint64_t)(fsp->n_fatent - 2) * cluster_bytes;
    if (free_bytes) *free_bytes = (uint64_t)nfree * cluster_bytes;
    return true;
}
