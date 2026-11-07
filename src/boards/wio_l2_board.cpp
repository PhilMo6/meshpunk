// wio_l2_board.cpp — Seeed Wio Tracker L2 board layer (see wio_l2_board.h).

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>

#include "wio_l2_board.h"
#include "../meshpunk_sync.h"   // SLog

// PCA9555 registers: each is a pair, port 0 then port 1.
#define PCA_REG_INPUT   0x00
#define PCA_REG_OUTPUT  0x02
#define PCA_REG_CONFIG  0x06   // 1 = input, 0 = output

// Shadows of the expander's output and direction registers; every write
// goes out as the full 16-bit pair so both ports stay coherent.
static uint8_t s_out[2];
static uint8_t s_cfg[2];
static bool    s_ready = false;

static bool exp_write_pair(uint8_t reg, const uint8_t v[2]) {
  Wire.beginTransmission(WIO_L2_EXP_ADDR);
  Wire.write(reg);
  Wire.write(v[0]);
  Wire.write(v[1]);
  return Wire.endTransmission() == 0;
}

static bool exp_read_pair(uint8_t reg, uint8_t v[2]) {
  Wire.beginTransmission(WIO_L2_EXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WIO_L2_EXP_ADDR, 2) != 2) return false;
  v[0] = Wire.read();
  v[1] = Wire.read();
  return true;
}

static void stage_output(uint8_t bit, bool high) {
  uint8_t mask = (uint8_t)(1u << (bit & 7));
  if (high) s_out[bit >> 3] |= mask;
  else      s_out[bit >> 3] &= (uint8_t)~mask;
  s_cfg[bit >> 3] &= (uint8_t)~mask;
}

static void stage_input(uint8_t bit) {
  s_cfg[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static bool write_state(void) {
  return exp_write_pair(PCA_REG_OUTPUT, s_out) &&
         exp_write_pair(PCA_REG_CONFIG, s_cfg);
}

bool wio_l2_exp_set(uint8_t bit, bool level) {
  stage_output(bit, level);
  return write_state();
}

bool wio_l2_exp_get(uint8_t bit, bool* level) {
  uint8_t in[2];
  if (!exp_read_pair(PCA_REG_INPUT, in)) return false;
  if (level) *level = (in[bit >> 3] >> (bit & 7)) & 1u;
  return true;
}

bool wio_l2_board_ready(void) { return s_ready; }

// Power-up sequence = the wadamesh WioTrackerL2Io::begin() order and delays,
// hardware-tested on this board; Meshtastic's earlyInitVariant() runs the
// same LCD, GNSS and touch timing. The GT911 INT line stays driven LOW, as
// in both: LOW through the touch reset's rising edge selects I2C address
// 0x5D, and a held line raises no expander interrupts (the WAKE button's
// edge detection reads that interrupt). The battery divider enable (P17,
// Meshtastic EXPANDS_BAT_ADC_EN) is driven HIGH and left on, as Meshtastic
// does; its ADS1115 battery read never toggles it.
bool wio_l2_board_init(void) {
  if (s_ready) return true;

  Wire.begin(WIO_L2_I2C_SDA, WIO_L2_I2C_SCL, WIO_L2_I2C_FREQ);
  Wire.setTimeOut(30);
  pinMode(WIO_L2_EXP_INT_PIN, INPUT_PULLUP);

  if (!exp_read_pair(PCA_REG_OUTPUT, s_out) ||
      !exp_read_pair(PCA_REG_CONFIG, s_cfg)) {
    SLog.printf("[WIO] FAIL: PCA9555 at 0x%02X not answering on SDA %d / SCL %d\n",
                WIO_L2_EXP_ADDR, WIO_L2_I2C_SDA, WIO_L2_I2C_SCL);
    return false;
  }

  stage_input(WIO_L2_EXP_WAKE_BTN);
  stage_input(WIO_L2_EXP_I2C_INT);
  stage_input(WIO_L2_EXP_SD_DETECT);
  stage_input(WIO_L2_EXP_LCD_CTRL);
  stage_output(WIO_L2_EXP_TP_INT,    false);
  stage_output(WIO_L2_EXP_LCD_PWR,   true);
  stage_output(WIO_L2_EXP_LCD_RST,   true);
  stage_output(WIO_L2_EXP_GROVE_PWR, false);
  stage_output(WIO_L2_EXP_TP_RST,    false);
  stage_output(WIO_L2_EXP_GNSS_RST,  true);
  stage_output(WIO_L2_EXP_USER_LED,  false);
  stage_output(WIO_L2_EXP_USB_OTG,   false);
  stage_output(WIO_L2_EXP_AUDIO_PA,  false);
  stage_output(WIO_L2_EXP_GNSS_PWR,  true);
  stage_output(WIO_L2_EXP_SD_PWR,    false);
  stage_output(WIO_L2_EXP_BAT_SENSE, true);

  bool ok = write_state();
  if (ok) { delay(10);  ok = wio_l2_exp_set(WIO_L2_EXP_GNSS_RST, false); }
  if (ok) { delay(40);  ok = wio_l2_exp_set(WIO_L2_EXP_LCD_RST, false); }
  if (ok) { delay(10);  ok = wio_l2_exp_set(WIO_L2_EXP_LCD_RST, true); }
  if (ok) { delay(500); ok = wio_l2_exp_set(WIO_L2_EXP_LCD_CTRL, true); }
  if (ok) { delay(10);  ok = wio_l2_exp_set(WIO_L2_EXP_TP_RST, true); }
  if (ok) { delay(60); }   // GT911 boot after reset release (Meshtastic: 60 ms)
  if (!ok) {
    SLog.println("[WIO] FAIL: expander write during the power-up sequence");
    return false;
  }

  s_ready = true;
  SLog.printf("[WIO] expander ready: out=%02X/%02X cfg=%02X/%02X\n",
              s_out[0], s_out[1], s_cfg[0], s_cfg[1]);
  return true;
}

// ── LP5814 backlight ────────────────────────────────────────────────────────
// Register values = the wadamesh WioTrackerL2Light driver (hardware-tested):
// setup writes, 200 on the four per-channel registers 0x14-0x17, then PWM
// duty on 0x18-0x1B latched by writing 0x55 to the update register 0x0F.

#define LIGHT_REG_UPDATE  0x0F
#define LIGHT_REG_PWM0    0x18

static bool light_write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(WIO_L2_LIGHT_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void wio_l2_backlight_set(uint8_t pwm) {
  for (uint8_t ch = 0; ch < 4; ch++) (void)light_write(LIGHT_REG_PWM0 + ch, pwm);
  (void)light_write(LIGHT_REG_UPDATE, 0x55);
}

// ── Core-1 driver init ──────────────────────────────────────────────────────

struct Core1Job {
  void (*fn)(void*);
  void* arg;
  SemaphoreHandle_t done;
};

static void core1_job_task(void* p) {
  Core1Job* job = (Core1Job*)p;
  job->fn(job->arg);
  xSemaphoreGive(job->done);
  vTaskDelete(NULL);
}

bool wio_l2_run_on_core1(void (*fn)(void*), void* arg) {
  Core1Job job = { fn, arg, xSemaphoreCreateBinary() };
  if (!job.done) {
    SLog.println("[WIO] FAIL: core-1 init: no semaphore");
    return false;
  }
  // 8 KB covers LovyanGFX's panel init and esp_vfs_fat_sdmmc_mount (card
  // init + f_mount) plus SLog's 224-byte format buffer.
  if (xTaskCreatePinnedToCore(core1_job_task, "wio_core1", 8192, &job,
                              uxTaskPriorityGet(NULL), NULL, 1) != pdPASS) {
    vSemaphoreDelete(job.done);
    SLog.println("[WIO] FAIL: core-1 init: task create");
    return false;
  }
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
  return true;
}

bool wio_l2_backlight_init(uint8_t pwm) {
  bool ok = true;
  ok = light_write(0x00, 0x01) && ok;
  ok = light_write(0x01, 0x01) && ok;
  ok = light_write(0x02, 0x00) && ok;
  ok = light_write(0x04, 0x4E) && ok;
  ok = light_write(0x05, 0xF0) && ok;
  for (uint8_t ch = 0; ch < 4; ch++) ok = light_write(0x14 + ch, 200) && ok;
  ok = light_write(0x02, 0x0F) && ok;
  wio_l2_backlight_set(pwm);
  if (!ok) {
    SLog.printf("[WIO] FAIL: LP5814 backlight at 0x%02X not answering\n",
                WIO_L2_LIGHT_ADDR);
  }
  return ok;
}

#endif // BOARD_WIO_L2
