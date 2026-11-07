// display_wio_l2.cpp — Seeed Wio Tracker L2 display backend
// (contract: display_dev.h).
//
// NV3031B 240x320 over quad-SPI on SPI3 — a bus of its own (the radio has
// FSPI, the microSD slot is SDMMC) — driven through LovyanGFX (panel
// definition in src/boards/wio_l2_lgfx.h). Native landscape is 320x240, the
// same logical geometry as the T-Deck and Heltec, so the Lua UI and the
// 320x240 module video contract carry over unchanged.
//
// Every panel access takes SPI_LOCK, as on the other boards: the bus is not
// shared here, but the lock is what serializes the LVGL flush (loopTask)
// against the ELF blit task (Core 1) — LovyanGFX's own transaction state is
// not safe to enter from two tasks at once.
//
// Pixels: LV_COLOR_16_SWAP=1, so LVGL buffers (and the splash bitmap, stored
// in the flush path's word order) are byte-swapped RGB565 — pushed as
// lgfx::swap565_t, which LovyanGFX sends unconverted.
//
// Panel power and reset are expander lines (LCD_PWR P05, LCD_RST P06,
// P04), sequenced by wio_l2_board_init() from power_dev_init() before this
// backend runs. Backlight: LP5814 LED driver on I2C (wio_l2_board.cpp).

#if defined(BOARD_WIO_L2)

#include <Arduino.h>

#include "display_dev.h"
#include "splash_logo.h"       // generated boot logo bitmap
#include "../boards/wio_l2_board.h"
#include "../boards/wio_l2_lgfx.h"
#include "../meshpunk_sync.h"  // SPI_LOCK/SPI_UNLOCK, SLog

static WioL2Lgfx lcd;

// Orientation state (contract: display_dev.h). The panel config puts native
// landscape at LovyanGFX rotation 0, so the quarter-turn count IS the
// rotation.
static uint8_t s_orient = 0;
static bool    s_module_video = false;

static uint8_t eff_orient(void) { return s_module_video ? 0 : s_orient; }

static void apply_rotation(void) {
  SPI_LOCK();
  lcd.setRotation(eff_orient());
  lcd.fillScreen(0);
  SPI_UNLOCK();
}

void display_dev_set_orientation(uint8_t o) {
  s_orient = o & 3;
  if (!s_module_video) apply_rotation();
}

uint8_t display_dev_orientation(void) { return eff_orient(); }

void display_dev_module_video(bool active) {
  if (active == s_module_video) return;
  s_module_video = active;
  apply_rotation();
}

void display_dev_orient_point(int16_t* x, int16_t* y) {
  // Same quarter-turn algebra the panel rotation applies: one step maps
  // (x, y) in a WxH space to (y, W-1-x). Applied k times from the native
  // landscape 320x240 space.
  int w = 320, h = 240;
  for (uint8_t k = eff_orient(); k > 0; k--) {
    int16_t nx = *y;
    int16_t ny = (int16_t)(w - 1 - *x);
    *x = nx; *y = ny;
    int t = w; w = h; h = t;
  }
}

static void lcd_init_job(void* ok) { *(bool*)ok = lcd.init(); }

// lcd.init() allocates the SPI3 bus interrupt through the ESP-IDF SPI
// driver, so it runs on core 1 (wio_l2_run_on_core1, wio_l2_board.h).
void display_dev_init(void) {
  bool ok = false;
  SPI_LOCK();
  wio_l2_run_on_core1(lcd_init_job, &ok);
  lcd.setRotation(0);
  lcd.fillScreen(0);
  SPI_UNLOCK();
  if (ok) SLog.println("[DISPLAY] NV3031B up (QSPI on SPI3, 75 MHz write)");
  else    SLog.println("[DISPLAY] FAIL: LovyanGFX init() returned false for the NV3031B");
}

// The logo bitmap (splash_logo.h, generated from meshpunk-logo.svg) is
// stored in the flush path's word order, so it goes out exactly like an
// LVGL buffer.
void display_dev_splash(void) {
  SPI_LOCK();
  lcd.fillScreen(0);
  lcd.pushImage((320 - SPLASH_LOGO_W) / 2, (240 - SPLASH_LOGO_H) / 2,
                SPLASH_LOGO_W, SPLASH_LOGO_H,
                (const lgfx::swap565_t*)kSplashLogo);
  SPI_UNLOCK();
}

// Landscape orientations report 320x240, portraits 240x320.
int display_dev_width(void)  { return (eff_orient() & 1) ? 240 : 320; }
int display_dev_height(void) { return (eff_orient() & 1) ? 320 : 240; }

void display_dev_flush_rect(int x, int y, int w, int h, const uint16_t* px) {
  SPI_LOCK();
  lcd.pushImage(x, y, w, h, (const lgfx::swap565_t*)px);
  SPI_UNLOCK();
}

void display_dev_blit(int x, int y, int w, int h, const uint16_t* px) {
  SPI_LOCK();
  lcd.pushImage(x, y, w, h, (const lgfx::swap565_t*)px);
  SPI_UNLOCK();
}

void display_dev_fill_black(void) {
  SPI_LOCK();
  lcd.fillScreen(0);
  SPI_UNLOCK();
}

// value 0-16 (the persisted scale every board shares): 0 = off, 16 = full,
// linear onto the LP5814's 0-255 PWM duty.
static uint8_t level_to_pwm(uint8_t value) {
  if (value > 16) value = 16;
  return (uint8_t)(((uint32_t)value * 255u) / 16u);
}

void display_dev_backlight_init(void) {
  wio_l2_backlight_init(0);
}

// Full register re-init of the LED driver, then the level.
void display_dev_backlight_reset(uint8_t value) {
  wio_l2_backlight_init(level_to_pwm(value));
}

void display_dev_brightness(uint8_t value) {
  wio_l2_backlight_set(level_to_pwm(value));
}

// Sleep = SLPIN. Wake = LovyanGFX's Panel_NV3031B::setSleep(false): a
// software reset, the full init table and MADCTL (its fix for a 1-pixel
// column shift a plain SLPOUT leaves; its reset and SLPOUT waits run
// inside). The reset leaves frame memory undefined, so the panel is
// cleared to black here and the caller repaints the UI (display_dev.h).
void display_dev_sleep(bool sleep) {
  SPI_LOCK();
  if (sleep) {
    lcd.sleep();
  } else {
    lcd.wakeup();
    lcd.fillScreen(0);
  }
  SPI_UNLOCK();
  if (sleep) delay(5);
}

#endif // BOARD_WIO_L2
