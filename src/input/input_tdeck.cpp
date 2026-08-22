// input_tdeck.cpp — T-Deck input backend (see input_dev.h for the contract).
//
// Owns the T-Deck's input hardware: the ESP32-C3 keyboard MCU on I2C 0x55
// (raw 5x7 matrix protocol, or the legacy single-ASCII-byte protocol on
// keyboard firmware older than LilyGo's 250620 build), the GT911 capacitive
// touch controller, the trackball's five GPIO edge lines, and the keyboard
// backlight commands. All code here moved verbatim from main.cpp's input
// stack when the per-device input layer was carved out.

#if defined(BOARD_TDECK)

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>   // esp_reset_reason (GT911 wake pulse gate)
#include "TouchDrvGT911.hpp"

#include "input_dev.h"
#include "../utilities.h"      // BOARD_* pin defines (LilyGo T-Deck)
#include "../tdeck-pins.h"     // TDECK_* pin defines
#include "../meshpunk_sync.h"  // SLog

// ── Keyboard I2C protocol ──────────────────────────────────────────────────
#define LILYGO_KB_SLAVE_ADDRESS 0x55
#define LILYGO_KB_BRIGHTNESS_CMD 0x01
#define LILYGO_KB_ALT_B_BRIGHTNESS_CMD 0x02
#define LILYGO_KB_MODE_RAW_CMD 0x03
#define LILYGO_KB_MODE_KEY_CMD 0x04

// Keyboard matrix dimensions (from stock ESP32-C3 firmware)
#define KB_COLS 5
#define KB_ROWS 7

// Modifier key positions in the matrix
#define KB_MOD_SYM_COL    0
#define KB_MOD_SYM_ROW    2
#define KB_MOD_ALT_COL    0
#define KB_MOD_ALT_ROW    4
#define KB_MOD_LSHIFT_COL 1
#define KB_MOD_LSHIFT_ROW 6
#define KB_MOD_RSHIFT_COL 2
#define KB_MOD_RSHIFT_ROW 3
#define KB_KEY_ENTER_COL  3
#define KB_KEY_ENTER_ROW  3
#define KB_KEY_BS_COL     4
#define KB_KEY_BS_ROW     3
#define KB_KEY_SPACE_COL  0
#define KB_KEY_SPACE_ROW  5
#define KB_KEY_MIC_COL    0
#define KB_KEY_MIC_ROW    6

// Normal character layer (col × row) — from stock C3 firmware Keyboard_ESP32C3.ino
static const char kb_matrix[KB_COLS][KB_ROWS] = {
  {'q','w',  0, 'a',  0, ' ',  0 },
  {'e','s','d','p','x','z',  0 },
  {'r','g','t',  0, 'v','c','f'},
  {'u','h','y',  0, 'b','n','j'},
  {'o','l','i',  0, '$','m','k'},
};

// Symbol character layer
static const char kb_matrix_symbol[KB_COLS][KB_ROWS] = {
  {'#','1',  0, '*',  0,   0, '0'},
  {'2','4','5','@','8','7',  0 },
  {'3','/',  '(',  0, '?','9','6'},
  {'_',':',')',  0, '!',',',';'},
  {'+','"','-',  0,   0, '.','\''},
};

// ── Backend state ──────────────────────────────────────────────────────────
static bool s_keyboard_available = false;

// Raw matrix sample: cur = this poll, prev = the poll before (edge queries —
// mic key, legacy-firmware detection — compare the two).
static uint8_t s_cur_matrix[KB_COLS]  = {0};
static uint8_t s_prev_matrix[KB_COLS] = {0};

// Legacy mode flag + the byte received on THIS poll (0 = none). The byte is
// raw and unbounded — the UI path bounds it to <128 for its state arrays,
// the ELF path uses the full range — exactly as before the split.
static bool    s_kb_legacy_mode = false;
static uint8_t s_legacy_byte    = 0;

// ── Old-keyboard-firmware detection (raw mode only) ─────────────────────────
// A keyboard MCU without raw-mode support keeps sending single ASCII bytes,
// which land in cur_matrix[0] with cols 1-4 zero. Lowercase ASCII (0x61-0x7A)
// as a matrix byte means space+mic+letter pressed simultaneously — a state no
// real typing produces — while such firmware can never light cols 1-4. The
// verdict latches on the first decisive input: a col 1-4 byte proves raw
// firmware and closes detection for the session; three signature events =
// old firmware, and loop() auto-enables legacy mode. A manual Settings
// toggle-off also closes detection so the user's choice isn't fought.
static uint8_t s_kb_suspect_events = 0;
static bool    s_kb_legacy_autoswitch_pending = false;
static bool    s_kb_autoswitch_done = false;

// ── Trackball ──────────────────────────────────────────────────────────────
// Counters are the shared neutral accumulators defined in input_ui.cpp
// (declared in input_dev.h); these ISRs are their T-Deck producer.

static void IRAM_ATTR ISR_click() {
  static uint32_t last_click_ms = 0;
  uint32_t now = millis();
  if (now - last_click_ms < 1) return;
  last_click_ms = now;
  trackball_click = 1;
}

// Trackball direction counters — each ISR fires on a FALLING edge pulse
// from the T-Deck trackball.
static void IRAM_ATTR ISR_trackball_up()    { trackball_up++; }
static void IRAM_ATTR ISR_trackball_down()  { trackball_down++; }
static void IRAM_ATTR ISR_trackball_left()  { trackball_left++; }
static void IRAM_ATTR ISR_trackball_right() { trackball_right++; }

// ── Touch ──────────────────────────────────────────────────────────────────
static TouchDrvGT911 touch;

// ── Capabilities ───────────────────────────────────────────────────────────
bool input_dev_has_keyboard(void)      { return s_keyboard_available; }
bool input_dev_has_trackball(void)     { return true; }
bool input_dev_has_touch(void)         { return true; }
bool input_dev_has_kbd_backlight(void) { return s_keyboard_available; }

// ── Lifecycle ──────────────────────────────────────────────────────────────

void input_dev_preinit(void) {
  // Connect trackball / home button
  pinMode(TDECK_TRACKBALL_CLICK, INPUT_PULLUP);
  attachInterrupt(TDECK_TRACKBALL_CLICK, ISR_click, FALLING);

  pinMode(BOARD_TBOX_G02, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G01, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G04, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G03, INPUT_PULLUP);

  // Attach trackball direction interrupts
  attachInterrupt(TDECK_TRACKBALL_UP,    ISR_trackball_up,    FALLING);
  attachInterrupt(TDECK_TRACKBALL_DOWN,  ISR_trackball_down,  FALLING);
  attachInterrupt(TDECK_TRACKBALL_LEFT,  ISR_trackball_left,  FALLING);
  attachInterrupt(TDECK_TRACKBALL_RIGHT, ISR_trackball_right, FALLING);
}

void input_dev_wake_pin_release(void) {
  detachInterrupt(TDECK_TRACKBALL_CLICK);
}

void input_dev_wake_pin_restore(void) {
  pinMode(TDECK_TRACKBALL_CLICK, INPUT_PULLUP);
  attachInterrupt(TDECK_TRACKBALL_CLICK, ISR_click, FALLING);
}

void input_dev_shutdown_prepare(void) {
  input_dev_kbd_backlight(0);
  // GT911 sleep: the driver drives INT low, then command 0x05. Only a >5ms
  // INT high pulse wakes it (I2C is dead in sleep) — input_dev_init sends
  // that pulse on deep-sleep-wake boots. INT low also disarms the board's
  // TP_EN touch-power latch (see power_tdeck.cpp).
  touch.sleep();
}

void input_dev_init(uint8_t kbd_backlight_boot) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // Waking from power-off: the shutdown path put the GT911 to sleep, and
    // in that state its I2C is dead — begin() below would fail and hang the
    // probe loop. It wakes only on a >5ms HIGH pulse on INT. Cold boots must
    // skip this: an active GT911 drives INT itself (contention).
    pinMode(BOARD_TOUCH_INT, OUTPUT);
    digitalWrite(BOARD_TOUCH_INT, HIGH);
    delay(8);
    pinMode(BOARD_TOUCH_INT, INPUT);
    delay(10);
  }

  // Set touch int input
  pinMode(BOARD_TOUCH_INT, INPUT);
  delay(20);

  SLog.println("Initializing GT911 touch sensor");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

  touch.setPins(-1, BOARD_TOUCH_INT);
  if (!touch.begin(Wire, GT911_SLAVE_ADDRESS_L)) {
    while (1) {
      SLog.println("Failed to find GT911 - check your wiring!");
      delay(1000);
    }
  }

  // Set touch max xy
  touch.setMaxCoordinates(320, 240);

  // Set swap xy
  touch.setSwapXY(true);

  // Set mirror xy
  touch.setMirrorXY(false, true);
  touch.setInterruptMode(LOW_LEVEL_QUERY);

  // Initialize keyboard
  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  if (Wire.endTransmission() == 0) {
    s_keyboard_available = true;
    SLog.println("T-Deck keyboard found!");

    // Set initial keyboard brightness
    input_dev_kbd_backlight_default(127);
    input_dev_kbd_backlight(kbd_backlight_boot);

    // Switch keyboard to raw matrix mode for hold detection — unless legacy
    // ASCII mode is persisted (old keyboard-MCU firmware without raw-mode
    // support; the KEY command is a no-op there and forces the single-byte
    // protocol on newer firmware so both behave identically).
    Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
    Wire.write(s_kb_legacy_mode ? LILYGO_KB_MODE_KEY_CMD : LILYGO_KB_MODE_RAW_CMD);
    Wire.endTransmission();
    SLog.printf("Keyboard switched to %s mode\n",
                s_kb_legacy_mode ? "legacy ASCII" : "raw matrix");
  } else {
    SLog.println("T-Deck keyboard not found!");
  }
}

// ── Keyboard sampling ──────────────────────────────────────────────────────

void input_dev_kbd_poll(bool detect_legacy_fw) {
  memcpy(s_prev_matrix, s_cur_matrix, KB_COLS);
  memset(s_cur_matrix, 0, KB_COLS);
  s_legacy_byte = 0;

  if (s_kb_legacy_mode) {
    Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
    if (Wire.available()) s_legacy_byte = Wire.read();
    // cur_matrix stays zero: every matrix-derived query (modifiers, mic
    // edge, decode) reads "nothing pressed" and is inert.
  } else {
    Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, KB_COLS);
    for (int c = 0; c < KB_COLS && Wire.available(); c++) {
      s_cur_matrix[c] = Wire.read();
    }
    // Old-keyboard-firmware detection (see s_kb_* above): lowercase ASCII in
    // col 0 before any col 1-4 byte = the MCU ignored the raw-mode command
    // and is sending single chars. Either verdict latches s_kb_autoswitch_done
    // and detection never runs again this session. loop() performs the switch.
    if (detect_legacy_fw && !s_kb_autoswitch_done) {
      if (s_cur_matrix[1] | s_cur_matrix[2] | s_cur_matrix[3] | s_cur_matrix[4]) {
        s_kb_autoswitch_done = true;   // raw firmware proven — detection closed
        SLog.println("[KB] raw matrix confirmed");
      } else if (s_prev_matrix[0] == 0 &&
                 s_cur_matrix[0] >= 0x61 && s_cur_matrix[0] <= 0x7A) {
        s_kb_suspect_events++;
        SLog.printf("[KB] ASCII-mode signature 0x%02X (%d/3)\n",
                    s_cur_matrix[0], s_kb_suspect_events);
        if (s_kb_suspect_events >= 3) {
          s_kb_legacy_autoswitch_pending = true;
          s_kb_autoswitch_done = true;
        }
      }
    }
  }
}

uint8_t input_dev_kbd_legacy_byte(void) {
  return s_legacy_byte;
}

void input_dev_kbd_mods(bool* lshift, bool* rshift, bool* sym, bool* alt) {
  if (lshift) *lshift = s_cur_matrix[KB_MOD_LSHIFT_COL] & (1 << KB_MOD_LSHIFT_ROW);
  if (rshift) *rshift = s_cur_matrix[KB_MOD_RSHIFT_COL] & (1 << KB_MOD_RSHIFT_ROW);
  if (sym)    *sym    = s_cur_matrix[KB_MOD_SYM_COL]    & (1 << KB_MOD_SYM_ROW);
  if (alt)    *alt    = s_cur_matrix[KB_MOD_ALT_COL]    & (1 << KB_MOD_ALT_ROW);
}

int input_dev_kbd_decode(InputKeyEv* out, int max) {
  int n = 0;
  for (int c = 0; c < KB_COLS; c++) {
    if (s_cur_matrix[c] == 0) continue;
    for (int r = 0; r < KB_ROWS; r++) {
      if (!(s_cur_matrix[c] & (1 << r))) continue;
      if (c == KB_MOD_SYM_COL && r == KB_MOD_SYM_ROW) continue;
      if (c == KB_MOD_ALT_COL && r == KB_MOD_ALT_ROW) continue;
      if (c == KB_MOD_LSHIFT_COL && r == KB_MOD_LSHIFT_ROW) continue;
      if (c == KB_MOD_RSHIFT_COL && r == KB_MOD_RSHIFT_ROW) continue;
      if (n >= max) return n;

      if (c == KB_KEY_ENTER_COL && r == KB_KEY_ENTER_ROW) {
        out[n].base = 0x0D; out[n].sym = 0x0D;
      } else if (c == KB_KEY_BS_COL && r == KB_KEY_BS_ROW) {
        out[n].base = 0x08; out[n].sym = 0x08;
      } else {
        // (0,6) is the mic key: no normal-layer character, but its symbol
        // layer is '0' — it flows through as {0,'0'} so sym+mic can type
        // the digit zero while the bare press stays dead in consumers.
        out[n].base = (uint8_t)kb_matrix[c][r];
        out[n].sym  = (uint8_t)kb_matrix_symbol[c][r];
        if (out[n].base == 0 && out[n].sym == 0) continue;
      }
      n++;
    }
  }
  return n;
}

bool input_dev_kbd_mic_edge(void) {
  bool mic_now  = s_cur_matrix[KB_KEY_MIC_COL]  & (1 << KB_KEY_MIC_ROW);
  bool mic_prev = s_prev_matrix[KB_KEY_MIC_COL] & (1 << KB_KEY_MIC_ROW);
  return mic_now && !mic_prev;
}

// ── Legacy ASCII mode facet ────────────────────────────────────────────────

bool input_dev_kbd_legacy_get(void) { return s_kb_legacy_mode; }

void input_dev_kbd_legacy_load(bool on) { s_kb_legacy_mode = on; }

// Switch keyboard input mode at runtime. Sends the matching mode command to
// the keyboard MCU (a no-op on firmware without command support) and drops
// the backend's sampling state derived under the previous mode. The policy
// layer (input_ui_set_legacy) drops ITS derived state around this call.
void input_dev_kbd_legacy_set(bool on) {
  s_kb_legacy_mode = on;
  if (s_keyboard_available) {
    Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
    Wire.write(on ? LILYGO_KB_MODE_KEY_CMD : LILYGO_KB_MODE_RAW_CMD);
    Wire.endTransmission();
  }
  memset(s_cur_matrix, 0, KB_COLS);
  memset(s_prev_matrix, 0, KB_COLS);
  s_legacy_byte = 0;
  s_kb_suspect_events = 0;
  SLog.printf("[KB] %s mode\n", on ? "legacy ASCII" : "raw matrix");
}

bool input_dev_kbd_legacy_autoswitch_pending(void) {
  if (!s_kb_legacy_autoswitch_pending) return false;
  s_kb_legacy_autoswitch_pending = false;
  return true;
}

void input_dev_kbd_autoswitch_close(void) { s_kb_autoswitch_done = true; }

// ── Keyboard backlight ─────────────────────────────────────────────────────

void input_dev_kbd_backlight(uint8_t value) {
  if (!s_keyboard_available)
    return;

  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
}

void input_dev_kbd_backlight_default(uint8_t value) {
  if (!s_keyboard_available)
    return;

  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_ALT_B_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
}

// ── Trackball click level ──────────────────────────────────────────────────

bool input_dev_nav_click_held(void) {
  return digitalRead(TDECK_TRACKBALL_CLICK) == LOW;
}

// No second/mode button on the T-Deck.
bool input_dev_aux_btn_take(void) { return false; }

// ── Touch ──────────────────────────────────────────────────────────────────

bool input_dev_touch_read(int16_t* tx, int16_t* ty) {
  int16_t x[INPUT_DEV_TOUCH_MAX], y[INPUT_DEV_TOUCH_MAX];
  if (input_dev_touch_read_multi(x, y, INPUT_DEV_TOUCH_MAX) <= 0) return false;
  if (tx) *tx = x[0];
  if (ty) *ty = y[0];
  return true;
}

// The GT911 tracks 5 points and the driver already returns them all; the
// controller is queried for its own supported count so a differently
// configured panel can't overrun the caller's arrays.
// The GT911 driver is told the panel geometry (setMaxCoordinates below) and
// returns screen coordinates directly, so "raw" and mapped are the same
// here; reported anyway so the measurement app runs on both boards.
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

int input_dev_touch_read_multi(int16_t* xs, int16_t* ys, int max) {
  if (!xs || !ys || max <= 0) return 0;
  int16_t x[INPUT_DEV_TOUCH_MAX], y[INPUT_DEV_TOUCH_MAX];
  int want = touch.getSupportTouchPoint();
  if (want > INPUT_DEV_TOUCH_MAX) want = INPUT_DEV_TOUCH_MAX;
  if (want < 1) want = 1;
  int n = (int)touch.getPoint(x, y, (uint8_t)want);
  if (n > max) n = max;
  for (int i = 0; i < n; i++) { xs[i] = x[i]; ys[i] = y[i]; }
  if (n > 0) {
    s_raw_x  = x[0];
    s_raw_y  = y[0];
    s_raw_x1 = (n > 1) ? x[1] : -1;
    s_raw_y1 = (n > 1) ? y[1] : -1;
    s_raw_n  = (uint8_t)n;
  }
  return n;
}

#endif // BOARD_TDECK
