// sd_wio_l2.cpp — Seeed Wio Tracker L2 microSD backend (contract: sd_dev.h).
//
// 1-bit SDMMC on CLK 2 / CMD 3 / D0 1 (Seeed wiki; Meshtastic's variant:
// HAS_SD_MMC, SD_SCLK_PIN 2 / SD_MOSI_PIN 3 = CMD / SD_MISO_PIN 1 = D0) —
// the S3's SDMMC host reaches any GPIO through the matrix. Card power is
// expander P16 (SD_PWR, parked OFF by wio_l2_board_init); card-detect is
// expander P02.
//
// Mounted through ESP-IDF's esp_vfs_fat_sdmmc_mount rather than the Arduino
// SD_MMC wrapper, which keeps the card handle private: USB drive mode needs
// it for raw sectors. The fs::FS is the SD/SD_MMC library pattern (usb_fs.cpp
// does the same): a VFSImpl whose mountpoint is set once the volume is up.

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <FS.h>
#include <vfs_api.h>            // VFSImpl

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"       // ff_diskio_get_pdrv_card
#include "ff.h"

#include "sd_dev.h"
#include "../boards/wio_l2_board.h"
#include "../meshpunk_sync.h"    // SLog

#define WIO_SD_CLK         GPIO_NUM_2
#define WIO_SD_CMD         GPIO_NUM_3
#define WIO_SD_D0          GPIO_NUM_1
#define WIO_SD_MOUNTPOINT  "/sd"
#define WIO_SD_MAX_FILES   5      // the SPI SD library's default (sd_spi.cpp)

class WioL2SdFS : public fs::FS {
 public:
  WioL2SdFS() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  void set_mountpoint(const char* mp) { _impl->mountpoint(mp); }
};

static WioL2SdFS     s_fs;
static sdmmc_card_t* s_card = nullptr;

// Descending clock caps, the SPI backend's probe order: high-speed
// (40 MHz), default speed (20 MHz), then a 4 MHz floor. Runs on core 1:
// the SDMMC host allocates its interrupt on the calling core
// (wio_l2_run_on_core1, wio_l2_board.h).
static void mount_job(void* mounted) {
  static const int freqs_khz[] = {SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT, 4000};
  for (int khz : freqs_khz) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags        = SDMMC_HOST_FLAG_1BIT;
    host.slot         = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = 1;
    slot.clk    = WIO_SD_CLK;
    slot.cmd    = WIO_SD_CMD;
    slot.d0     = WIO_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;
    mcfg.max_files              = WIO_SD_MAX_FILES;
    mcfg.allocation_unit_size   = 0;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(WIO_SD_MOUNTPOINT, &host, &slot,
                                            &mcfg, &s_card);
    if (err == ESP_OK) {
      s_fs.set_mountpoint(WIO_SD_MOUNTPOINT);
      SLog.printf("[SD] Mounted (SDMMC 1-bit) at a %d kHz cap: '%s', card max %d kHz\n",
                  khz, s_card->cid.name, (int)s_card->max_freq_khz);
      *(bool*)mounted = true;
      return;
    }
    s_card = nullptr;
    SLog.printf("[SD] Mount failed at a %d kHz cap: %s\n", khz, esp_err_to_name(err));
  }
}

bool sd_dev_mount(void) {
  if (s_card) return true;

  if (!wio_l2_exp_set(WIO_L2_EXP_SD_PWR, true)) {
    SLog.println("[SD] FAIL: expander write for SD power (P16)");
    return false;
  }
  delay(10);   // card supply ramp before the first command

  bool detect = true;
  if (wio_l2_exp_get(WIO_L2_EXP_SD_DETECT, &detect))
    SLog.printf("[SD] card-detect (P02): %s\n", detect ? "HIGH" : "LOW");

  bool mounted = false;
  wio_l2_run_on_core1(mount_job, &mounted);
  return mounted;
}

// The unmount frees the SDMMC host interrupt, so it runs on core 1 too.
static void unmount_job(void*) {
  esp_vfs_fat_sdcard_unmount(WIO_SD_MOUNTPOINT, s_card);
}

void sd_dev_unmount(void) {
  if (!s_card) return;
  wio_l2_run_on_core1(unmount_job, nullptr);
  s_fs.set_mountpoint(NULL);
  s_card = nullptr;
}

fs::FS& sd_dev_fs(void) { return s_fs; }

uint64_t sd_dev_card_size(void) {
  if (!s_card) return 0;
  return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

// FAT geometry of the mounted volume, by the FatFs drive this card holds.
static bool fat_info(FATFS** fs, DWORD* free_clusters) {
  if (!s_card) return false;
  char drv[3] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
  return f_getfree(drv, free_clusters, fs) == FR_OK;
}

static uint64_t fat_sector_size(const FATFS* fs) {
#if FF_MAX_SS != FF_MIN_SS
  return fs->ssize;
#else
  (void)fs;
  return FF_MAX_SS;
#endif
}

uint64_t sd_dev_total_bytes(void) {
  FATFS* fs;
  DWORD free_clusters;
  if (!fat_info(&fs, &free_clusters)) return 0;
  return (uint64_t)fs->csize * (fs->n_fatent - 2) * fat_sector_size(fs);
}

uint64_t sd_dev_used_bytes(void) {
  FATFS* fs;
  DWORD free_clusters;
  if (!fat_info(&fs, &free_clusters)) return 0;
  return (uint64_t)fs->csize * ((fs->n_fatent - 2) - free_clusters) * fat_sector_size(fs);
}

uint32_t sd_dev_num_sectors(void) { return s_card ? (uint32_t)s_card->csd.capacity : 0; }
uint32_t sd_dev_sector_size(void) { return s_card ? (uint32_t)s_card->csd.sector_size : 0; }

bool sd_dev_read_raw(uint8_t* buf, uint32_t sector) {
  return s_card && sdmmc_read_sectors(s_card, buf, sector, 1) == ESP_OK;
}

bool sd_dev_write_raw(const uint8_t* buf, uint32_t sector) {
  return s_card && sdmmc_write_sectors(s_card, buf, sector, 1) == ESP_OK;
}

#endif // BOARD_WIO_L2
