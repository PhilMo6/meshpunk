// display_tdeck.cpp — T-Deck display backend (contract: display_dev.h).
//
// ST7789 320x240 over the shared SPI bus (TFT_eSPI, vendored config
// User_Setups/Setup210_LilyGo_T_Deck.h), scanline tear-sync on flush, and
// the pulse-counted 16-level backlight chip on BOARD_BL_PIN. All code moved
// verbatim from main.cpp/elf_host.cpp when the display seam was carved out.
//
// Bus arbitration lives HERE: the T-Deck shares one SPI bus between the TFT,
// the SX1262 radio, and the SD card, so every panel access is bracketed by
// SPI_LOCK/SPI_UNLOCK (meshpunk_sync.h). The lock scope per call is exactly
// what the pre-split call sites held.

#if defined(BOARD_TDECK)

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "display_dev.h"
#include "splash_logo.h"       // generated boot logo bitmap
#include "../boards/board_pins.h"
#include "../utilities.h"      // BOARD_BL_PIN
#include "../meshpunk_sync.h"  // SPI_LOCK/SPI_UNLOCK

static TFT_eSPI tft;

// The T-Deck's SD slot lives on the same shared FSPI bus as the panel and
// radio — the global SPI object main.cpp begins at boot.
SPIClass& board_sd_spi(void) { return SPI; }

void display_dev_init(void) {
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
}

// The logo bitmap (splash_logo.h, generated from meshpunk-logo.svg) is
// stored in the flush path's word order, so it goes out exactly like an
// LVGL buffer.
void display_dev_splash(void) {
  SPI_LOCK();
  tft.fillScreen(TFT_BLACK);
  tft.startWrite();
  tft.setAddrWindow((320 - SPLASH_LOGO_W) / 2, (240 - SPLASH_LOGO_H) / 2,
                    SPLASH_LOGO_W, SPLASH_LOGO_H);
  tft.pushColors((uint16_t*)kSplashLogo, SPLASH_LOGO_W * SPLASH_LOGO_H, false);
  tft.endWrite();
  SPI_UNLOCK();
}

// Rotation 1 (landscape): the panel's native 240x320 becomes 320 wide by
// 240 tall. Stated as literals on purpose: TFT_WIDTH/TFT_HEIGHT are defined
// with OPPOSITE values by utilities.h (320x240, landscape) and TFT_eSPI's
// Setup210 (240x320, panel-native) — whichever include comes last silently
// wins, and that ordering trap already shipped one 240-wide UI. Never size
// anything off those macros in this file.
int display_dev_width(void)  { return 320; }
int display_dev_height(void) { return 240; }

// Helper: read the ILI9341 current scanline position via command 0x45.
// Returns 0–319 indicating the gate line the panel is currently refreshing.
static uint16_t ili9341_get_scanline() {
  uint8_t hi = tft.readcommand8(0x45, 1); // GTS[8]
  uint8_t lo = tft.readcommand8(0x45, 2); // GTS[7:0]
  return ((hi & 0x01) << 8) | lo;
}

// Scanline-tracking flush.
//
// The ILI9341 physically scans gate lines 0→319 regardless of MADCTL
// rotation settings. In landscape rotation 1 (MADCTL MV|MX), the gate
// scan sweeps horizontally across the screen, so the scanline value
// approximately maps to the LVGL x-coordinate.
//
// Strategy: before writing pixels, read the current scanline. If it is
// inside (or just ahead of) the flush area, busy-wait for it to pass.
// This makes our SPI writes trail behind the panel's read pointer,
// preventing the display from showing a mix of old and new data.
//
// The SCANLINE_MARGIN adds a safety buffer — we wait until the scanline
// is at least this many lines past the end of our flush area before
// writing, to account for SPI transaction setup time.

#define SCANLINE_MARGIN 8

void display_dev_flush_rect(int x, int y, int w, int h, const uint16_t* px) {
  SPI_LOCK();

  // Read current scanline position.
  // In rotation 1 the gate scan maps to the y-axis of the flush area
  // (the ILI9341's 320 native rows become the 240-pixel vertical axis
  // after MV swap + rotation). Try y1/y2 first; if tearing persists,
  // switch flush_start/flush_end to use x1/x2 instead.
  uint16_t scanline = ili9341_get_scanline();
  uint16_t flush_start = (uint16_t)y;
  uint16_t flush_end   = (uint16_t)(y + h - 1 + SCANLINE_MARGIN);

  // Busy-wait if the scanline is inside (or about to enter) the flush
  // area.  Timeout after ~8 ms to avoid blocking the system forever
  // if readcommand8 returns garbage (e.g. MISO not connected).
  int wait_us = 0;
  while (scanline >= flush_start && scanline <= flush_end && wait_us < 8000) {
    delayMicroseconds(10);
    wait_us += 10;
    scanline = ili9341_get_scanline();
  }
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
  pinMode(BOARD_BL_PIN, OUTPUT);
}

// SLPIN/SLPOUT. The panel needs 120ms after SLPOUT before it accepts
// further commands (datasheet minimum for both ST7789 and ILI9341).
void display_dev_sleep(bool sleep) {
  SPI_LOCK();
  tft.writecommand(sleep ? 0x10 : 0x11);
  SPI_UNLOCK();
  delay(sleep ? 5 : 120);
}

// LilyGo T-Deck control backlight chip has 16 levels of adjustment range
// The adjustable range is 0~15, 0 is the minimum brightness, 15 is the maximum
// brightness
static uint8_t s_bl_level = 0;
static const uint8_t s_bl_steps = 16;
static portMUX_TYPE s_bl_mux = portMUX_INITIALIZER_UNLOCKED;

void display_dev_brightness(uint8_t value) {
  if (value == 0) {
    digitalWrite(BOARD_BL_PIN, 0);
    delay(3);
    s_bl_level = 0;
    return;
  }
  // The pulse-counter chip shuts down (and zeroes its counter) if the line
  // sits LOW for more than a few milliseconds. A preemption or ISR landing
  // between a pulse's LOW and HIGH writes stretches it past that threshold:
  // the chip goes dark while the level tracking still says lit, and every
  // later call with the same target computes zero pulses — a stuck-black
  // backlight. The whole train is <150us, so run it with interrupts off.
  taskENTER_CRITICAL(&s_bl_mux);
  if (s_bl_level == 0) {
    digitalWrite(BOARD_BL_PIN, 1);
    s_bl_level = s_bl_steps;
    delayMicroseconds(100);   // turn-on settle from shutdown before pulsing
  }
  int from = s_bl_steps - s_bl_level;
  int to = s_bl_steps - value;
  int num = (s_bl_steps + to - from) % s_bl_steps;
  for (int i = 0; i < num; i++) {
    digitalWrite(BOARD_BL_PIN, 0);
    digitalWrite(BOARD_BL_PIN, 1);
  }
  s_bl_level = value;
  taskEXIT_CRITICAL(&s_bl_mux);
}

// Guaranteed shutdown (>=3ms LOW clears the chip's counter whatever state it
// was in), then a fresh turn-on with a generous settle, then pulse down to
// the target — chip and driver state agree afterward no matter what happened
// before. See display_dev.h.
void display_dev_backlight_reset(uint8_t value) {
  // Re-latch the pad configuration first: a pad coming out of a hold or a
  // sleep state can silently ignore writes until reconfigured, which would
  // swallow the whole train below.
  pinMode(BOARD_BL_PIN, OUTPUT);
  digitalWrite(BOARD_BL_PIN, 0);
  delay(4);
  s_bl_level = 0;
  if (value == 0) return;
  taskENTER_CRITICAL(&s_bl_mux);
  digitalWrite(BOARD_BL_PIN, 1);
  delayMicroseconds(100);
  for (int i = 0; i < (int)(s_bl_steps - value); i++) {
    digitalWrite(BOARD_BL_PIN, 0);
    digitalWrite(BOARD_BL_PIN, 1);
  }
  s_bl_level = value;
  taskEXIT_CRITICAL(&s_bl_mux);
}

#endif // BOARD_TDECK
