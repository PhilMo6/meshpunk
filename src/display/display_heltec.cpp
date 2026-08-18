// display_heltec.cpp — Heltec V4 Expansion Kit display backend
// (contract: display_dev.h).
//
// ST7789 240x320 over HSPI (SCK 17 / MOSI 33, no MISO wired), rotation 1 ->
// 320x240 landscape — the same logical geometry as the T-Deck, so the Lua UI
// and the 320x240 module video contract carry over unchanged. Panel config
// comes from the env's USER_SETUP_LOADED build flags (platformio.ini), not a
// vendored TFT_eSPI Setup file.
//
// Differences from the T-Deck backend:
//  - NO scanline tear-sync: the panel's MISO line is not connected to the
//    ESP32 as a readback path, so readcommand8() is impossible. Flush is a
//    plain push.
//  - The panel SHARES its HSPI bus with the microSD slot (SCK 16 / MOSI 15
//    / MISO 45, SD CS 3), so every panel access takes SPI_LOCK — the same
//    global bus mutex the SD helpers (sd_spi_take) already use everywhere.
//    The radio has FSPI to itself; its wrapper's use of the same mutex just
//    costs a little needless cross-bus serialization.
//
// Backlight: plain LEDC PWM (no pulse-counted chip here), mapped from the
// same persisted 0-16 brightness scale the prefs use.

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "display_dev.h"
#include "../boards/board_pins.h"
#include "../meshpunk_sync.h"  // SPI_LOCK/SPI_UNLOCK

// The SD slot's bus: HSPI, shared with the panel. Lazily begun because the
// SD mount runs earlier in setup() than display_dev_init(); TFT_eSPI's own
// HSPI attach later coexists with this (same pattern as the T-Deck's shared
// FSPI global).
SPIClass& board_sd_spi(void) {
  static SPIClass hspi(HSPI);
  static bool begun = false;
  if (!begun) {
    hspi.begin(16, 45, 15, -1);
    begun = true;
  }
  return hspi;
}

// R8-EX kit: backlight (LCD_LEDK) = GPIO44, NOT the base-V4 kit's GPIO21 —
// on this board 21 is the shared LCD+touch RESET line (TFT_RST).
#define HELTEC_TFT_BL_PIN   44
#define HELTEC_BL_LEDC_CH   1        // LEDC channel (0 is unused too, but keep clear of libs)
// 44kHz: above the ~20kHz band where backlight switching noise couples into
// the capacitive touch sensor. The fundamental and its odd harmonics are the
// interfering components, so a 5kHz carrier puts 15/25/35kHz into that band.
// wadamesh drives this same GPIO44 backlight at 44kHz. At 8-bit resolution
// this uses 11.3MHz of the LEDC's 80MHz budget.
#define HELTEC_BL_FREQ      44000
#define HELTEC_BL_RES_BITS  8

static TFT_eSPI tft;

void display_dev_init(void) {
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
}

// Rotation 1 (landscape): the panel's native 240x320 becomes 320 wide by
// 240 tall. Literals on purpose — never size anything off TFT_WIDTH/
// TFT_HEIGHT macros in a backend (see display_tdeck.cpp for the collision
// this avoids).
int display_dev_width(void)  { return 320; }
int display_dev_height(void) { return 240; }

void display_dev_flush_rect(int x, int y, int w, int h, const uint16_t* px) {
  SPI_LOCK();
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  tft.pushColors((uint16_t *)px, w * h, false);
  tft.endWrite();
  SPI_UNLOCK();
}

void display_dev_blit(int x, int y, int w, int h, const uint16_t* px) {
  SPI_LOCK();
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  tft.pushColors((uint16_t*)px, w * h, false);
  tft.endWrite();
  SPI_UNLOCK();
}

void display_dev_fill_black(void) {
  SPI_LOCK();
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);
  tft.endWrite();
  SPI_UNLOCK();
}

void display_dev_backlight_init(void) {
  ledcSetup(HELTEC_BL_LEDC_CH, HELTEC_BL_FREQ, HELTEC_BL_RES_BITS);
  ledcAttachPin(HELTEC_TFT_BL_PIN, HELTEC_BL_LEDC_CH);
}

// value 0-16 (same persisted scale as the T-Deck's 16-level chip): 0 = off,
// 16 = full. Linear duty mapping.
void display_dev_brightness(uint8_t value) {
  if (value > 16) value = 16;
  uint32_t duty = ((uint32_t)value * 255u) / 16u;
  ledcWrite(HELTEC_BL_LEDC_CH, duty);
}

#endif // BOARD_HELTEC_V4
