// power_wio_l2.cpp — Seeed Wio Tracker L2 power/battery backend
// (contract: power_dev.h).
//
// Every switched rail on this board is a PCA9555 expander output (LCD,
// GNSS, SD, Grove, audio PA, battery divider enable — see
// src/boards/wio_l2_board.h). power_dev_init() runs the board bring-up
// (wio_l2_board_init) that powers and resets them in order.
//
// Battery: ADS1115 (I2C 0x48) AIN0 behind a 1:2 divider — the Meshtastic
// ADS1115BatteryLevel reading: GAIN_ONE (+/-4.096 V), 860 SPS, single-ended
// AIN0, volts x 2. The divider enable (expander P17) is left on by the
// bring-up.
//
// Wake from power-off: the USER button (GPIO0, active LOW) via ext0. The
// WAKE button cannot do it — it sits on the expander, whose INT (GPIO45) is
// not an RTC pad.
//
// USB host port power: the AW35615 USB-C port controller (I2C 0x22) and
// OTG_EN (expander P13) — see its section below.

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>
#include <stdarg.h>
#include <stdio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>      // esp_reset_reason

#include "power_dev.h"
#include "aw35615.h"
#include "../boards/board_pins.h"     // PIN_BOOT_BTN
#include "../boards/wio_l2_board.h"
#include "../meshpunk_sync.h"         // SLog
#include "../usb_manager.h"           // usb_ulog

static void usb_port_boot(void);

void power_dev_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // Waking from power-off: release the RTC routing the shutdown path armed
    // on the wake button, or input_dev_preinit's pull-up does nothing
    // against the RTC-routed pad.
    rtc_gpio_deinit((gpio_num_t)PIN_BOOT_BTN);
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  }

  // I2C bus, expander probe, then the rail/reset sequence. Logs its own
  // FAIL line; everything downstream that needs the expander logs too.
  wio_l2_board_init();

  usb_port_boot();
}

// ── Battery (ADS1115) ──────────────────────────────────────────────────────
#define ADS_REG_CONV    0x00
#define ADS_REG_CONFIG  0x01
// OS=1 (start a conversion), MUX=100 (AIN0 vs GND), PGA=001 (+/-4.096 V),
// MODE=1 (single shot), DR=111 (860 SPS), comparator off (COMP_QUE=11).
#define ADS_CFG_AIN0_SINGLE  0xC3E3
#define ADS_SAMPLES          4

static bool ads_write_config(uint16_t cfg) {
  Wire.beginTransmission(WIO_L2_BATT_ADC_ADDR);
  Wire.write(ADS_REG_CONFIG);
  Wire.write((uint8_t)(cfg >> 8));
  Wire.write((uint8_t)(cfg & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool ads_read_reg(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(WIO_L2_BATT_ADC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WIO_L2_BATT_ADC_ADDR, 2) != 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  *out = (uint16_t)((hi << 8) | lo);
  return true;
}

// One single-shot AIN0 conversion. A conversion takes 1.2 ms at 860 SPS;
// the config register's OS bit reads 1 once it is done.
static bool ads_read_ain0(int16_t* raw) {
  if (!ads_write_config(ADS_CFG_AIN0_SINGLE)) return false;
  for (int i = 0; i < 10; i++) {
    delay(1);
    uint16_t cfg = 0;
    if (!ads_read_reg(ADS_REG_CONFIG, &cfg)) return false;
    if (cfg & 0x8000) {
      uint16_t v = 0;
      if (!ads_read_reg(ADS_REG_CONV, &v)) return false;
      *raw = (int16_t)v;
      return true;
    }
  }
  return false;
}

static bool s_batt_fail_logged = false;

uint16_t power_dev_battery_mv(void) {
  int32_t sum = 0;
  for (int i = 0; i < ADS_SAMPLES; i++) {
    int16_t raw = 0;
    if (!ads_read_ain0(&raw)) {
      if (!s_batt_fail_logged) {
        s_batt_fail_logged = true;
        SLog.printf("[BATT] FAIL: ADS1115 at 0x%02X not answering\n",
                    WIO_L2_BATT_ADC_ADDR);
      }
      return 0;
    }
    sum += raw;
  }
  // 4.096 V over 32768 counts = 0.125 mV per count at the pin; x2 for the
  // divider = 0.25 mV per count.
  int32_t mv = (sum / ADS_SAMPLES) / 4;
  if (mv < 0) mv = 0;
  return (uint16_t)mv;
}

// ── USB host port power (AW35615 + OTG_EN) ─────────────────────────────────
// Host mode makes the USB-C port a Type-C source. The AW35615 toggles in SRC
// mode (Rp at default USB current) and stops on a sink's Rd; only then, and
// only while VBUS is below 4.0 V (nothing else is powering it), OTG_EN
// (expander P13) switches 5 V onto VBUS. The measured CC pin rising above
// 1.6 V means the sink's Rd is gone: OTG_EN goes low and VBUS is discharged.
// Outside host mode the chip is at its reset defaults, Rd on both CC pins: a
// sink, which is what a charger needs before it supplies the board. The chip
// keeps its registers across an ESP32 reset, so boot and power-off reset it.
//
// State changes print to serial (SLog) and to the Tools/USB Host log
// (usb_ulog): in host mode the port no longer carries USB serial.

enum UsbSrcState : uint8_t {
  USB_SRC_OFF,        // host mode not running; the port is a sink
  USB_SRC_ARMED,      // toggling as a source, waiting for a sink
  USB_SRC_EXTERNAL,   // a sink attached while VBUS was already up from elsewhere
  USB_SRC_SOURCING,   // OTG_EN high: 5 V on VBUS
  USB_SRC_FAILED,     // stopped after a logged failure until host mode restarts
};

static SemaphoreHandle_t s_usb_mux     = nullptr;
static UsbSrcState       s_usb_state   = USB_SRC_OFF;
static bool              s_usbc_ok     = false;   // the AW35615 answered at boot
static uint8_t           s_usb_cc      = 0;       // the sink's CC pin while sourcing
static uint32_t          s_usb_tick_ms = 0;

static void usbc_log(const char* fmt, ...) {
  char line[96];   // the usb_ulog line length
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  SLog.printf("[USBC] %s\n", line);
  usb_ulog("%s", line);
}

static void usb_port_boot(void) {
  s_usb_mux = xSemaphoreCreateMutex();
  uint16_t vendor = 0;
  uint8_t  id     = 0;
  s_usbc_ok = aw35615_reset(Wire, WIO_L2_TYPEC_ADDR, &vendor, &id);
  if (s_usbc_ok)
    SLog.printf("[USBC] AW35615 vendor 0x%04X id 0x%02X: reset to sink\n", vendor, id);
  else
    SLog.printf("[USBC] FAIL: AW35615 at 0x%02X %s (vendor 0x%04X)\n", WIO_L2_TYPEC_ADDR,
                vendor ? "reports another vendor ID" : "not answering", vendor);
}

// OTG_EN low, then the 660 ohm discharge until VBUSOK clears (500 ms cap).
// Returns the discharge time in ms; -1 when VBUS was still at or above
// 4.0 V at the cap; -2 when the chip did not answer.
static int usb_vbus_off(void) {
  if (!wio_l2_exp_set(WIO_L2_EXP_USB_OTG, false))
    usbc_log("FAIL: expander write OTG_EN low");
  int ms = -2;
  if (aw35615_vbus_discharge(true)) {
    uint32_t t0 = millis();
    for (;;) {
      bool vbus = true;
      if (!aw35615_status(&vbus, nullptr)) break;
      if (!vbus) {
        ms = (int)(millis() - t0);
        break;
      }
      if (millis() - t0 >= 500) {
        ms = -1;
        break;
      }
      delay(5);
    }
  }
  aw35615_vbus_discharge(false);
  return ms;
}

// Loud stop: OTG_EN low, then nothing until host mode restarts.
static void usb_src_fail(const char* what) {
  if (!wio_l2_exp_set(WIO_L2_EXP_USB_OTG, false))
    usbc_log("FAIL: expander write OTG_EN low");
  s_usb_state = USB_SRC_FAILED;
  s_usb_cc    = 0;
  usbc_log("FAIL: %s; port power off until host restart", what);
}

static void usb_src_rearm(void) {
  s_usb_cc = 0;
  if (!aw35615_source_detect()) {
    usb_src_fail("AW35615 write (source detect)");
    return;
  }
  s_usb_state = USB_SRC_ARMED;
}

static void usb_src_armed(void) {
  uint8_t cc = 0;
  if (!aw35615_detected_sink(&cc)) {
    usb_src_fail("AW35615 read (STATUS1A)");
    return;
  }
  if (cc == 0) return;

  bool vbus = false;
  if (!aw35615_status(&vbus, nullptr)) {
    usb_src_fail("AW35615 read (STATUS0)");
    return;
  }
  if (vbus) {
    s_usb_state = USB_SRC_EXTERNAL;
    usbc_log("device on CC%u, VBUS already up from elsewhere: 5 V stays off", (unsigned)cc);
    return;
  }

  if (!aw35615_source_hold(cc)) {
    usb_src_fail("AW35615 write (source hold)");
    return;
  }
  delay(1);   // before the comparator read that follows the measure switch change
  bool open = true;
  if (!aw35615_status(nullptr, &open)) {
    usb_src_fail("AW35615 read (STATUS0)");
    return;
  }
  if (open) {   // the Rd was gone by the time the pin was measured
    usb_src_rearm();
    return;
  }

  if (!wio_l2_exp_set(WIO_L2_EXP_USB_OTG, true)) {
    usb_src_fail("expander write OTG_EN high");
    return;
  }
  // VBUS trace: VBUSOK and the CC comparator sampled every 1 ms for 500 ms
  // after OTG_EN high, then printed as ok/LOW runs (first 8) and a result.
  struct VbusRun { bool ok; uint16_t from, to; };
  VbusRun  runs[8];
  int      nruns      = 0;
  bool     more       = false;
  int      up_ms      = -1;   // first VBUSOK sample
  int      cc_lost_ms = -1;   // first COMP sample (the sink's Rd gone)
  uint32_t dip_ms     = 0;    // time below 4.0 V after up_ms
  uint32_t prev_t     = 0;
  bool     prev_ok    = false;
  uint32_t t0 = millis();
  for (;;) {
    uint32_t t = millis() - t0;
    if (t >= 500) break;
    bool open = false;
    if (!aw35615_status(&vbus, &open)) {
      usb_src_fail("AW35615 read (STATUS0)");
      return;
    }
    if (up_ms >= 0 && !prev_ok) dip_ms += t - prev_t;
    if (vbus && up_ms < 0) up_ms = (int)t;
    if (open && cc_lost_ms < 0) cc_lost_ms = (int)t;
    if (nruns > 0 && runs[nruns - 1].ok == vbus) runs[nruns - 1].to = (uint16_t)t;
    else if (nruns < 8) runs[nruns++] = { vbus, (uint16_t)t, (uint16_t)t };
    else more = true;
    prev_t  = t;
    prev_ok = vbus;
    delay(1);
  }

  usbc_log("VBUS trace CC%u, 500 ms:", (unsigned)cc);
  for (int i = 0; i < nruns; i++)
    usbc_log(" %-4s %u-%u", runs[i].ok ? "ok" : "LOW", (unsigned)runs[i].from,
             (unsigned)runs[i].to);
  if (more) usbc_log(" (more)");
  if (cc_lost_ms < 0) usbc_log("CC held whole time");
  else                usbc_log("CC lost at %d ms", cc_lost_ms);
  if (up_ms < 0)      usbc_log("RESULT: VBUS never came up");
  else if (!prev_ok)  usbc_log("RESULT: VBUS stayed low");
  else if (dip_ms)    usbc_log("RESULT: dip %lu ms, recovered", (unsigned long)dip_ms);
  else                usbc_log("RESULT: never dipped");

  if (up_ms < 0) {
    usb_src_fail("VBUS below 4.0 V 500 ms after OTG_EN high");
    return;
  }
  s_usb_cc    = cc;
  s_usb_state = USB_SRC_SOURCING;
  usbc_log("device attached (CC%u): 5 V on, VBUS up in %d ms", (unsigned)cc, up_ms);
}

static void usb_src_sourcing(void) {
  bool vbus = true;
  bool open = false;
  if (!aw35615_status(&vbus, &open)) {
    usb_src_fail("AW35615 read (STATUS0)");
    return;
  }
  if (open) {
    unsigned cc = s_usb_cc;
    int ms = usb_vbus_off();
    if (ms >= 0)
      usbc_log("device removed (CC%u): 5 V off, VBUS below 4.0 V in %d ms", cc, ms);
    else if (ms == -1)
      usbc_log("device removed (CC%u): 5 V off, VBUS still 4.0 V+ after 500 ms", cc);
    else
      usbc_log("device removed (CC%u): 5 V off; AW35615 read failed in discharge", cc);
    usb_src_rearm();
    return;
  }
  if (!vbus) usb_src_fail("VBUS fell below 4.0 V with the device attached");
}

static void usb_src_external(void) {
  bool vbus = true;
  if (!aw35615_status(&vbus, nullptr)) {
    usb_src_fail("AW35615 read (STATUS0)");
    return;
  }
  if (vbus) return;
  usbc_log("outside VBUS gone: waiting for a device");
  usb_src_rearm();
}

bool power_dev_usb_host_begin(void) {
  if (!s_usbc_ok) {
    usbc_log("FAIL: AW35615 did not answer at boot: no port power");
    return false;
  }
  xSemaphoreTake(s_usb_mux, portMAX_DELAY);
  s_usb_tick_ms = 0;
  usb_src_rearm();
  bool armed = s_usb_state == USB_SRC_ARMED;
  if (armed) usbc_log("port power armed: 5 V turns on when a device is attached");
  xSemaphoreGive(s_usb_mux);
  return armed;
}

bool power_dev_usb_port_power(void) { return s_usbc_ok; }

void power_dev_usb_host_tick(void) {
  if (!s_usbc_ok) return;
  uint32_t now = millis();
  if (now - s_usb_tick_ms < 100) return;
  s_usb_tick_ms = now;
  xSemaphoreTake(s_usb_mux, portMAX_DELAY);
  switch (s_usb_state) {
    case USB_SRC_ARMED:    usb_src_armed();    break;
    case USB_SRC_EXTERNAL: usb_src_external(); break;
    case USB_SRC_SOURCING: usb_src_sourcing(); break;
    default:               break;
  }
  xSemaphoreGive(s_usb_mux);
}

void power_dev_usb_host_end(void) {
  if (!s_usbc_ok) return;
  xSemaphoreTake(s_usb_mux, portMAX_DELAY);
  if (s_usb_state == USB_SRC_SOURCING) {
    usb_vbus_off();
  } else if (!wio_l2_exp_set(WIO_L2_EXP_USB_OTG, false)) {
    usbc_log("FAIL: expander write OTG_EN low");
  }
  s_usb_state = USB_SRC_OFF;
  s_usb_cc    = 0;
  uint16_t vendor = 0;
  uint8_t  id     = 0;
  if (aw35615_reset(Wire, WIO_L2_TYPEC_ADDR, &vendor, &id))
    usbc_log("port power off: USB-C port back to a sink (charging)");
  else
    usbc_log("FAIL: AW35615 reset to sink after host mode");
  xSemaphoreGive(s_usb_mux);
}

// Light sleep stops the usb task that polls the port, so standby switches
// 5 V off and re-arms detection; the first tick after wake finds a device
// that is still plugged in and powers it again.
static void usb_src_standby(void) {
  if (!s_usbc_ok) return;
  xSemaphoreTake(s_usb_mux, portMAX_DELAY);
  if (s_usb_state == USB_SRC_SOURCING) {
    usb_vbus_off();
    usbc_log("standby: 5 V off");
    usb_src_rearm();
  }
  xSemaphoreGive(s_usb_mux);
}

// ── Power off ──────────────────────────────────────────────────────────────

void power_dev_shutdown(void) {
  // Switched rails OFF, and the panel/touch control lines LOW so nothing
  // back-feeds the unpowered parts. The expander holds its output register
  // while powered, so these levels persist through deep sleep.
  static const uint8_t kOff[] = {
    WIO_L2_EXP_LCD_PWR,  WIO_L2_EXP_LCD_RST,   WIO_L2_EXP_LCD_CTRL,
    WIO_L2_EXP_TP_RST,   WIO_L2_EXP_GNSS_PWR,  WIO_L2_EXP_SD_PWR,
    WIO_L2_EXP_GROVE_PWR, WIO_L2_EXP_AUDIO_PA, WIO_L2_EXP_USER_LED,
    WIO_L2_EXP_USB_OTG,  WIO_L2_EXP_BAT_SENSE,
  };
  for (uint8_t bit : kOff) wio_l2_exp_set(bit, false);

  // Port controller back to a sink, so a charger supplies the board while it
  // is off, whatever host mode left it as.
  if (s_usbc_ok) {
    xSemaphoreTake(s_usb_mux, portMAX_DELAY);
    s_usb_state = USB_SRC_OFF;
    uint16_t vendor = 0;
    uint8_t  id     = 0;
    if (!aw35615_reset(Wire, WIO_L2_TYPEC_ADDR, &vendor, &id))
      SLog.println("[USBC] FAIL: AW35615 reset to sink at power off");
    xSemaphoreGive(s_usb_mux);
  }

  // Wake: USER button (GPIO0, active LOW) — the T-Deck/Heltec recipe; the
  // RTC-domain pull-up must persist or the pad floats LOW and instantly
  // re-wakes.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_init((gpio_num_t)PIN_BOOT_BTN);
  rtc_gpio_set_direction((gpio_num_t)PIN_BOOT_BTN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BOOT_BTN);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BOOT_BTN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT_BTN, 0);

  esp_deep_sleep_start();   // never returns; a press reboots
}

// Standby keeps the LCD and SD rails (panel sleeping, card mounted) and cuts
// the GNSS rail, as on the Heltec, plus the speaker amp (the mixer is
// suspended for standby). The L76K cold-boots back to its factory 9600 NMEA
// on exit, the rate the firmware's baud probe locked at boot. Expander
// outputs are external state: nothing to hold across light sleep. USB port
// power goes off for the standby (usb_src_standby).
void power_dev_standby_enter(void) {
  wio_l2_exp_set(WIO_L2_EXP_GNSS_PWR, false);
  wio_l2_exp_set(WIO_L2_EXP_AUDIO_PA, false);
  usb_src_standby();
}

void power_dev_standby_exit(void) {
  wio_l2_exp_set(WIO_L2_EXP_GNSS_PWR, true);
  wio_l2_exp_set(WIO_L2_EXP_AUDIO_PA, true);
}

#endif // BOARD_WIO_L2
