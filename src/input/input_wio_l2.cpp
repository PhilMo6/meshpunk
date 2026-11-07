// input_wio_l2.cpp — Seeed Wio Tracker L2 input backend
// (contract: input_dev.h).
//
// Input hardware:
//   GT911 capacitive touch on the shared I2C bus (SDA 47 / SCL 48). Its RST
//   and INT lines are I/O-expander bits (P10 / P03) sequenced by
//   wio_l2_board_init(); INT stays driven LOW there, so reads are polled.
//   USER button = GPIO0, active LOW — select/click (same role as the
//   Heltec's USER key).
//   WAKE button = expander P00, active LOW (Meshtastic WakeKey.cpp: pressed
//   = !digitalRead) — the aux (input-mode) button. It reaches the ESP32 only
//   through the expander's open-drain INT on GPIO45, which goes low when an
//   expander input changes.
// No keyboard, no trackball: the keyboard facet reports absent and the
// legacy-ASCII facet stubs out, as on the Heltec kit.
//
// Touch geometry: the controller reports panel-native portrait, x 0-239 and
// y 0-319 (the range Meshtastic's and wadamesh's LovyanGFX touch configs
// declare for this panel). Their config — panel offset_rotation 1, touch
// offset_rotation 2, rotation 0 — resolves in LovyanGFX's convertRawXY to
// screen_x = 319 - raw_y, screen_y = raw_x on the 320x240 landscape.

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>
#include "TouchDrvGT911.hpp"

#include "input_dev.h"
#include "../boards/board_pins.h"     // PIN_BOOT_BTN
#include "../boards/wio_l2_board.h"
#include "../display/display_dev.h"   // display_dev_orient_point
#include "../meshpunk_sync.h"         // SLog

// ── Buttons ─────────────────────────────────────────────────────────────────
// USER: ISR-per-edge with a debounce window, as on the Heltec.
#define BTN_DEBOUNCE_MS 180

static void IRAM_ATTR ISR_user_btn() {
  static uint32_t last_ms = 0;
  uint32_t now = millis();
  if (now - last_ms < BTN_DEBOUNCE_MS) return;
  last_ms = now;
  trackball_click = 1;       // select
}

// WAKE: a GT911 touch read on the shared bus also returns the expander INT
// (GPIO45) high, so the INT level does not hold a pending change. A falling-
// edge interrupt latches the time of every drop, and the input port is read
// WAKE_DEBOUNCE_MS after that edge (the Meshtastic WakeKey timing). A poll
// that finds INT low with no latched edge arms the same read, timed from the
// poll.
#define WAKE_DEBOUNCE_MS 25

static bool              s_wake_armed  = false;   // port read pending
static uint32_t          s_wake_t0     = 0;       // time the read counts from
static bool              s_wake_down   = false;   // last level read: pressed
static volatile bool     s_int_fell    = false;   // ISR: falling edge latched
static volatile uint32_t s_int_fell_ms = 0;

static void IRAM_ATTR ISR_exp_int() {
  s_int_fell_ms = millis();
  s_int_fell    = true;
}

// ── Touch ──────────────────────────────────────────────────────────────────
static TouchDrvGT911 touch;
static bool s_touch_ok = false;

#define WIO_TOUCH_RAW_Y_MAX 319   // panel long axis (raw x spans 0-239)

// ── Capabilities ───────────────────────────────────────────────────────────
bool input_dev_has_keyboard(void)      { return false; }
bool input_dev_has_trackball(void)     { return false; }
bool input_dev_has_touch(void)         { return true; }
bool input_dev_has_kbd_backlight(void) { return false; }

// ── Lifecycle ──────────────────────────────────────────────────────────────

// GPIO45 (expander INT) is configured by wio_l2_board_init().
void input_dev_preinit(void) {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  attachInterrupt(PIN_BOOT_BTN, ISR_user_btn, FALLING);
}

void input_dev_init(uint8_t kbd_backlight_boot) {
  (void)kbd_backlight_boot;   // no keyboard, no backlight

  // Explicit SDA/SCL: TouchDrvGT911's defaults are the variant's SDA/SCL
  // (8/9 on the generic esp32s3 variant) — the radio's BUSY and DIO1 here.
  // The bus is already up (wio_l2_board_init), so the library's
  // Wire.begin() is a no-op; its probe tries 0x5D, then 0x14.
  s_touch_ok = touch.begin(Wire, WIO_L2_TOUCH_ADDR, WIO_L2_I2C_SDA, WIO_L2_I2C_SCL);
  if (s_touch_ok) {
    int16_t rx = 0, ry = 0;
    touch.getResolution(&rx, &ry);
    SLog.printf("[TOUCH] GT911 found: fw 0x%04X, resolution %dx%d\n",
                (unsigned)touch.getFwVersion(), rx, ry);
  } else {
    SLog.printf("[TOUCH] FAIL: GT911 not answering at 0x5D or 0x14 on SDA %d / SCL %d\n",
                WIO_L2_I2C_SDA, WIO_L2_I2C_SCL);
  }

  // Read the expander input port once: releases any INT the power-up
  // sequence's direction changes latched, and seeds the WAKE level.
  bool level = true;
  if (wio_l2_exp_get(WIO_L2_EXP_WAKE_BTN, &level)) {
    s_wake_down = !level;
    SLog.printf("[WIO] WAKE button idle level: %s\n", level ? "HIGH" : "LOW (pressed?)");
  }
  attachInterrupt(WIO_L2_EXP_INT_PIN, ISR_exp_int, FALLING);
}

// GT911 sleep (command 0x05 with INT low — the expander already holds it
// LOW). A reset pulse wakes it; wio_l2_board_init() sends one every boot.
void input_dev_shutdown_prepare(void) {
  if (s_touch_ok) touch.sleep();
}

void input_dev_wake_pin_release(void) {
  detachInterrupt(PIN_BOOT_BTN);
}

void input_dev_wake_pin_restore(void) {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  attachInterrupt(PIN_BOOT_BTN, ISR_user_btn, FALLING);
}

// ── Keyboard facet: no keyboard on this board ──────────────────────────────

void    input_dev_kbd_poll(bool detect_legacy_fw) { (void)detect_legacy_fw; }
uint8_t input_dev_kbd_legacy_byte(void) { return 0; }

void input_dev_kbd_mods(bool* lshift, bool* rshift, bool* sym, bool* alt) {
  if (lshift) *lshift = false;
  if (rshift) *rshift = false;
  if (sym)    *sym    = false;
  if (alt)    *alt    = false;
}

int  input_dev_kbd_decode(InputKeyEv* out, int max) { (void)out; (void)max; return 0; }
bool input_dev_kbd_mic_edge(void) { return false; }

bool input_dev_kbd_legacy_get(void)  { return false; }
void input_dev_kbd_legacy_load(bool on) { (void)on; }
void input_dev_kbd_legacy_set(bool on)  { (void)on; }
bool input_dev_kbd_legacy_autoswitch_pending(void) { return false; }
void input_dev_kbd_autoswitch_close(void) { }

void input_dev_kbd_backlight(uint8_t value)         { (void)value; }
void input_dev_kbd_backlight_default(uint8_t value) { (void)value; }

// ── Nav click level ────────────────────────────────────────────────────────
// USER is the "select" button; LVGL release detection reads the live level.
bool input_dev_nav_click_held(void) {
  return digitalRead(PIN_BOOT_BTN) == LOW;
}

// WAKE press edge. Callers: loop()'s mode dispatcher, and the ELF input task
// every 30 ms while a module runs (never both at once).
bool input_dev_aux_btn_take(void) {
  if (!s_wake_armed) {
    bool fell = s_int_fell;
    if (!fell && digitalRead(WIO_L2_EXP_INT_PIN) == HIGH) return false;   // no change
    s_int_fell   = false;
    s_wake_armed = true;
    s_wake_t0    = fell ? s_int_fell_ms : millis();
  }
  if ((int32_t)(millis() - s_wake_t0) < WAKE_DEBOUNCE_MS) return false;
  s_wake_armed = false;

  bool level = true;
  if (!wio_l2_exp_get(WIO_L2_EXP_WAKE_BTN, &level)) return false;
  bool down  = !level;
  bool press = down && !s_wake_down;
  s_wake_down = down;
  return press;
}

// ── Touch ──────────────────────────────────────────────────────────────────

bool input_dev_touch_read(int16_t* tx, int16_t* ty) {
  int16_t x[INPUT_DEV_TOUCH_MAX], y[INPUT_DEV_TOUCH_MAX];
  if (input_dev_touch_read_multi(x, y, INPUT_DEV_TOUCH_MAX) <= 0) return false;
  if (tx) *tx = x[0];
  if (ty) *ty = y[0];
  return true;
}

// Last accepted frame before the raw->screen transform — read by the Touch
// Test app through input_dev_touch_raw().
static volatile int16_t s_raw_x  = -1, s_raw_y  = -1;
static volatile int16_t s_raw_x1 = -1, s_raw_y1 = -1;
static volatile uint8_t s_raw_n  = 0;

void input_dev_touch_raw(InputTouchRaw* out) {
  if (!out) return;
  out->x0     = s_raw_x;
  out->y0     = s_raw_y;
  out->x1     = s_raw_x1;
  out->y1     = s_raw_y1;
  out->points = s_raw_n;
  out->drops  = 0;          // the driver does its own validation
}

// Raw/mapped pairs for the first touches after boot print themselves, so
// the first session on the device confirms (or names the fix for) the
// transform above without any app. Bounded: cannot flood the log.
#define WIO_TOUCH_LOG_MAX 12
static int s_touch_logs = 0;

int input_dev_touch_read_multi(int16_t* xs, int16_t* ys, int max) {
  if (!xs || !ys || max <= 0 || !s_touch_ok) return 0;
  // The driver's getPoint() fills `size` slots and then transforms as many
  // slots as the controller reported (up to 5), so both arrays are full
  // size regardless of `max`.
  int16_t x[INPUT_DEV_TOUCH_MAX], y[INPUT_DEV_TOUCH_MAX];
  int n = (int)touch.getPoint(x, y, INPUT_DEV_TOUCH_MAX);
  if (n <= 0) return 0;
  if (n > INPUT_DEV_TOUCH_MAX) n = INPUT_DEV_TOUCH_MAX;
  if (n > max) n = max;

  for (int i = 0; i < n; i++) {
    int16_t sx = (int16_t)(WIO_TOUCH_RAW_Y_MAX - y[i]);
    int16_t sy = x[i];
    if (sx < 0) sx = 0; if (sx > 319) sx = 319;
    if (sy < 0) sy = 0; if (sy > 239) sy = 239;
    if (i == 0 && s_touch_logs < WIO_TOUCH_LOG_MAX) {
      s_touch_logs++;
      SLog.printf("[TOUCH] raw (%d,%d) -> screen (%d,%d)\n", x[0], y[0], sx, sy);
    }
    // Landscape coords from the board transform above; the user-orientation
    // mapping happens on top. Identity during module video sessions.
    display_dev_orient_point(&sx, &sy);
    xs[i] = sx;
    ys[i] = sy;
  }

  s_raw_x  = x[0];
  s_raw_y  = y[0];
  s_raw_x1 = (n > 1) ? x[1] : -1;
  s_raw_y1 = (n > 1) ? y[1] : -1;
  s_raw_n  = (uint8_t)n;
  return n;
}

#endif // BOARD_WIO_L2
