// input_heltec.cpp — Heltec V4 Expansion Kit input backend
// (contract: input_dev.h).
//
// Input hardware: CHSC6X capacitive touch (I2C1 on SDA 47 / SCL 48, addr
// 0x2E, polled — the INT line is unused, same query model as the T-Deck's
// GT911) and TWO side buttons, both hw-probed 2026-08-11:
//   USER = GPIO0, active LOW (shares the line with the mainboard PRG/boot
//          button) — wired below as select/click.
//   IO   = GPIO46, ACTIVE HIGH with an external pull-down (the line the R8
//          pinmap labels LED_Write) — reserved for the input-mode cycle
//          (controller/OSK mode manager); wired when that lands.
// No keyboard, no trackball: the keyboard facet reports absent; the
// legacy-ASCII facet is a T-Deck keyboard-MCU concept and stubs out
// entirely. Navigation is touch; USER feeds the shared nav pulse counters
// (the same channel USB arrow keys use on any board) as select/click.
//
// CHSC6X protocol (from Heltec's V4_Touch_TFT example driver): read 16
// bytes from register 0x00; buf[2]&0x07 = point count; point 0 at
// buf[3..6]: x = (buf[3]&0x0F)<<8|buf[4], y = (buf[5]&0x0F)<<8|buf[6];
// (buf[3]&0xC0)==0xC0 marks an invalid slot; all-zero or all-0xFF frames
// mean no touch.
//
// MULTI-TOUCH (hw-probed 2026-08-13, two fingers on the panel): the vendor
// example only ever reads point 0, but the controller reports TWO. Points
// repeat on a 6-byte stride — 4 coordinate bytes then a 2-byte trailer that
// is byte-identical between slots (the tell that identified the stride):
//   point k at buf[3 + 6*k .. ], so point 1 sits at buf[9..12].
// A probe frame decoded to (31,220) and (278,53) with the fingers at
// opposite corners. 16 bytes holds exactly two points; a third would start
// at byte 15, so 2 is the panel ceiling here.

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <Wire.h>

#include "input_dev.h"
#include "../meshpunk_sync.h"  // SLog

// R8-EX kit wiring (schematic-decoded; base-V4 kit pins do not apply):
// touch I2C rides the mainboard's OLED I2C lines 17/18, and the touch RESET
// is the SAME GPIO21 line as the LCD reset — the display backend cycles it
// during panel init, so this backend must NEVER drive it (a pulse here
// would reset the LCD mid-run). The CHSC6X needs no host config after
// reset; reads simply resume.
#define HELTEC_TOUCH_SDA   17
#define HELTEC_TOUCH_SCL   18
#define HELTEC_TOUCH_ADDR  0x2E
#define HELTEC_TOUCH_P0     3   // byte offset of point 0's coordinates
#define HELTEC_TOUCH_STRIDE 6   // bytes per point (4 coords + 2 trailer)
#define HELTEC_TOUCH_POINTS 2   // what fits in the 16-byte frame

#define HELTEC_BTN_PRG     0    // USER key (shares the PRG/boot line), active LOW
#define HELTEC_BTN_IO      46   // IO key, ACTIVE HIGH (external pull-down)

// ── Buttons ─────────────────────────────────────────────────────────────────
// ISR-per-edge with a debounce window (mechanical buttons bounce far more
// than the T-Deck trackball's optical pulses).
#define BTN_DEBOUNCE_MS 180

static void IRAM_ATTR ISR_prg_btn() {
  static uint32_t last_ms = 0;
  uint32_t now = millis();
  if (now - last_ms < BTN_DEBOUNCE_MS) return;
  last_ms = now;
  trackball_click = 1;       // select
}

// IO = the input-mode button (consumed by loop()'s dispatcher).
static volatile bool s_io_pending = false;

static void IRAM_ATTR ISR_io_btn() {
  static uint32_t last_ms = 0;
  uint32_t now = millis();
  if (now - last_ms < BTN_DEBOUNCE_MS) return;
  last_ms = now;
  s_io_pending = true;
}

// ── Capabilities ───────────────────────────────────────────────────────────
bool input_dev_has_keyboard(void)      { return false; }
bool input_dev_has_trackball(void)     { return false; }
bool input_dev_has_touch(void)         { return true; }
bool input_dev_has_kbd_backlight(void) { return false; }

// ── Lifecycle ──────────────────────────────────────────────────────────────

void input_dev_preinit(void) {
  pinMode(HELTEC_BTN_PRG, INPUT_PULLUP);
  attachInterrupt(HELTEC_BTN_PRG, ISR_prg_btn, FALLING);
  pinMode(HELTEC_BTN_IO, INPUT);   // external pull-down; press drives HIGH
  attachInterrupt(HELTEC_BTN_IO, ISR_io_btn, RISING);
}

void input_dev_init(uint8_t kbd_backlight_boot) {
  (void)kbd_backlight_boot;   // no keyboard, no backlight

  // 100kHz. The frame read is 16 bytes at a poll rate that leaves the bus
  // idle almost all the time, so nothing here needs the faster clock.
  Wire1.begin(HELTEC_TOUCH_SDA, HELTEC_TOUCH_SCL, 100000);

  // No reset pulse here: the reset line (GPIO21) is shared with the LCD and
  // is cycled by display_dev_init(). This probe may report not-responding
  // if it runs while that reset is held — reads recover on their own.
  Wire1.beginTransmission(HELTEC_TOUCH_ADDR);
  if (Wire1.endTransmission() == 0) {
    SLog.println("[TOUCH] CHSC6X found (0x2E)");
  } else {
    SLog.println("[TOUCH] CHSC6X not responding at 0x2E (may appear after display init)");
  }
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
  return digitalRead(HELTEC_BTN_PRG) == LOW;
}

bool input_dev_aux_btn_take(void) {
  if (!s_io_pending) return false;
  s_io_pending = false;
  return true;
}

// ── Touch ──────────────────────────────────────────────────────────────────

// Raw CHSC6X frame -> screen coordinates for rotation 1 (landscape 320x240;
// controller reports panel-native portrait 240x320): screen_x = raw_y,
// screen_y = (239 - raw_x). If the first on-device test shows a flipped
// axis, the auto-printed raw/mapped pairs below name the correct transform
// in one boot.
bool input_dev_touch_read(int16_t* tx, int16_t* ty) {
  int16_t x[HELTEC_TOUCH_POINTS], y[HELTEC_TOUCH_POINTS];
  if (input_dev_touch_read_multi(x, y, HELTEC_TOUCH_POINTS) <= 0) return false;
  if (tx) *tx = x[0];
  if (ty) *ty = y[0];
  return true;
}

// Last accepted frame before the raw->screen transform — read by the Touch
// Test app through input_dev_touch_raw(). Both slots are kept so the app can
// show a second contact as the panel reported it.
static volatile int16_t  s_raw_x  = -1, s_raw_y  = -1;
static volatile int16_t  s_raw_x1 = -1, s_raw_y1 = -1;
static volatile uint8_t  s_raw_n  = 0;
// Frames rejected since boot: a failed checksum, a slot flagged invalid, or
// a short I2C read. Surfaced by the Touch Test app as a panel-health figure.
static volatile uint32_t s_raw_drops = 0;

void input_dev_touch_raw(InputTouchRaw* out) {
  if (!out) return;
  out->x0     = s_raw_x;
  out->y0     = s_raw_y;
  out->x1     = s_raw_x1;
  out->y1     = s_raw_y1;
  out->points = s_raw_n;
  out->drops  = s_raw_drops;
}

#define HELTEC_TOUCH_RAW_X_MAX 239   // panel short axis
#define HELTEC_TOUCH_RAW_Y_MAX 319   // panel long axis
#define HELTEC_TOUCH_CSUM_MAX  20    // bounded: cannot flood the log

static int     s_csum_prints = 0;

// The controller's INT line (GPIO43 per wadamesh's R8 environment) is not
// connected on this board: probed at boot by driving the pad's pull both
// ways, it followed the pull in each direction. Reads are polled.
int input_dev_touch_read_multi(int16_t* xs, int16_t* ys, int max) {
  uint8_t buf[16];
  if (!xs || !ys || max <= 0) return 0;

  Wire1.beginTransmission(HELTEC_TOUCH_ADDR);
  Wire1.write((uint8_t)0x00);
  if (Wire1.endTransmission(false) != 0) { s_raw_drops++; return 0; }
  if (Wire1.requestFrom((int)HELTEC_TOUCH_ADDR, 16) != 16) { s_raw_drops++; return 0; }
  for (int i = 0; i < 16; i++) buf[i] = Wire1.read();

  // No-touch signatures per the vendor driver.
  if ((buf[2] == 0 && buf[3] == 0 && buf[4] == 0 && buf[6] == 0) ||
      (buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF && buf[6] == 0xFF)) {
    return 0;
  }

  int n = buf[2] & 0x07;
  if (n <= 0) { return 0; }
  if (n > HELTEC_TOUCH_POINTS) n = HELTEC_TOUCH_POINTS;
  if (n > max) n = max;

  int out = 0;
  // Diagnostic snapshot of THIS frame: reset so a frame carrying one point
  // can't leave the previous frame's slot 1 lying around and read as a
  // phantom that was never there.
  int16_t dx0 = -1, dy0 = -1, dx1 = -1, dy1 = -1;
  for (int k = 0; k < n; k++) {
    const uint8_t* p = &buf[HELTEC_TOUCH_P0 + k * HELTEC_TOUCH_STRIDE];
    if ((p[0] & 0xC0) == 0xC0) { s_raw_drops++; continue; }   // slot invalid

    // buf[7] is a checksum over buf[2..6] — the point count followed by point
    // 0's four coordinate bytes — confirmed on hardware against captured
    // one- and two-point frames. Point 1's matching byte is a copy of it: it
    // holds the same value and does not change when point 1's own bytes do,
    // so the second point carries no checksum and cannot be validated.
    if (k == 0) {
      uint8_t sum = (uint8_t)(buf[2] + p[0] + p[1] + p[2] + p[3]);
      if (sum != p[4]) {
        s_raw_drops++;
        if (s_csum_prints < HELTEC_TOUCH_CSUM_MAX) {
          s_csum_prints++;
          SLog.printf("[TCSUM] want=%02X got=%02X frame:", sum, p[4]);
          for (int b = 0; b < 16; b++) SLog.printf(" %02X", buf[b]);
          SLog.println();
        }
        continue;
      }
    }

    uint16_t raw_x = ((uint16_t)(p[0] & 0x0F) << 8) | p[1];
    uint16_t raw_y = ((uint16_t)(p[2] & 0x0F) << 8) | p[3];

    int16_t sx = (int16_t)raw_y;
    int16_t sy = (int16_t)(HELTEC_TOUCH_RAW_X_MAX - raw_x);
    if (sx < 0) sx = 0; if (sx > 319) sx = 319;
    if (sy < 0) sy = 0; if (sy > 239) sy = 239;
    if (out == 0)      { dx0 = (int16_t)raw_x; dy0 = (int16_t)raw_y; }
    else if (out == 1) { dx1 = (int16_t)raw_x; dy1 = (int16_t)raw_y; }
    xs[out] = sx;
    ys[out] = sy;
    out++;
  }

  if (out == 0) { return 0; }

  s_raw_x  = dx0; s_raw_y  = dy0;
  s_raw_x1 = dx1; s_raw_y1 = dy1;
  s_raw_n  = (uint8_t)n;

  return out;
}

#endif // BOARD_HELTEC_V4
