// sd_spi.cpp — microSD over SPI (contract: sd_dev.h): the T-Deck (the
// shared FSPI bus) and the Heltec kit (the display's HSPI bus). The board's
// display backend supplies the bus (board_sd_spi()); PIN_SD_CS selects the
// card. The Arduino SD library mounts at /sd by default.

#if defined(BOARD_TDECK) || defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <SD.h>

#include "sd_dev.h"
#include "../boards/board_pins.h"   // PIN_SD_CS, board_sd_spi()
#include "../meshpunk_sync.h"        // SLog

// The SPI bus is shared with the TFT (80 MHz) and SX1262, but every device
// sets its own per-transaction SPISettings, so this clock only applies to
// SD transfers. 40 MHz cuts a 131KB map-tile read from ~400ms (4 MHz
// Arduino default) to ~50ms. Probe descending; 4 MHz floor = old behavior.
bool sd_dev_mount(void) {
  static const uint32_t freqs[] = {40000000U, 25000000U, 4000000U};
  for (uint32_t freq : freqs) {
    if (SD.begin(PIN_SD_CS, board_sd_spi(), freq)) {
      SLog.printf("[SD] Mounted at %lu Hz\n", (unsigned long)freq);
      return true;
    }
    SD.end();
    SLog.printf("[SD] Mount failed at %lu Hz\n", (unsigned long)freq);
  }
  return false;
}

void sd_dev_unmount(void) { SD.end(); }

fs::FS& sd_dev_fs(void) { return SD; }

uint64_t sd_dev_card_size(void)   { return SD.cardSize(); }
uint64_t sd_dev_total_bytes(void) { return SD.totalBytes(); }
uint64_t sd_dev_used_bytes(void)  { return SD.usedBytes(); }

uint32_t sd_dev_num_sectors(void) { return (uint32_t)SD.numSectors(); }
uint32_t sd_dev_sector_size(void) { return (uint32_t)SD.sectorSize(); }

bool sd_dev_read_raw(uint8_t* buf, uint32_t sector) {
  return SD.readRAW(buf, sector);
}

bool sd_dev_write_raw(const uint8_t* buf, uint32_t sector) {
  return SD.writeRAW((uint8_t*)buf, sector);
}

#endif // BOARD_TDECK || BOARD_HELTEC_V4
