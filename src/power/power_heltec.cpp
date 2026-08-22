// power_heltec.cpp — Heltec V4-R8 power/battery backend (contract:
// power_dev.h).
//
// R8-SPECIFIC PIN MAP (WiFi_LoRa_32_V4R8.pdf pinout): the base-V4 sources
// put Vext on GPIO36 and an ADC gate on GPIO37 — both are octal-PSRAM pins
// on the S3R8 and touching them corrupts the PSRAM bus (hw-confirmed hard
// hang). On the R8: Vext_Ctrl = GPIO40 (shared with GNSS wake), VGNSS_Ctrl
// = GPIO42, and the battery divider (390K/100K -> GPIO1) has NO gate.
//
// Both rails switch through AO3401 P-channel FETs: gate LOW = conducting =
// rail ON. The hardware pull-ups on their control lines therefore park the
// rails OFF at reset — hw-confirmed by a boot where everything on Vext
// (LCD, touch, SD, GPS) was dark/silent until these drives landed.
//
// Radio + FEM are on the always-on rail (NOT Vext) — power-off must park
// them explicitly (FEM LDO enable on P_LORA_PA_POWER; the SX1262 itself is
// slept by the shared shutdown path before this backend runs).

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <driver/gpio.h>     // gpio_hold_en (standby pad holds)
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>      // esp_reset_reason

#include "power_dev.h"
#include "../boards/punk_heltec_board.h"
#include "../boards/board_pins.h"   // PIN_BOOT_BTN

extern PunkHeltecBoard board;   // main.cpp

#define HELTEC_VEXT_CTRL   40   // P-FET gate: LOW = Vext rail ON
#define HELTEC_VGNSS_CTRL  42   // P-FET gate: LOW = GNSS rail ON

void power_dev_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // Waking from power-off: release the RTC holds the shutdown path armed
    // (rails + FEM LDO + wake button) before re-driving anything. The FEM
    // pins get their own hold_dis in LoRaFEMControl::init().
    rtc_gpio_hold_dis((gpio_num_t)HELTEC_VEXT_CTRL);
    rtc_gpio_hold_dis((gpio_num_t)HELTEC_VGNSS_CTRL);
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_PA_POWER);
    rtc_gpio_deinit((gpio_num_t)PIN_BOOT_BTN);
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  }

  // Rails ON (active LOW). Runs before the SD mount, display init and the
  // GPS baud probe — everything downstream needs these live. NEVER touch
  // GPIO26/33/35/36/37 on this board (in-package octal PSRAM lines).
  pinMode(HELTEC_VEXT_CTRL, OUTPUT);
  digitalWrite(HELTEC_VEXT_CTRL, LOW);
  pinMode(HELTEC_VGNSS_CTRL, OUTPUT);
  digitalWrite(HELTEC_VGNSS_CTRL, LOW);
  delay(50);   // rail settle before the first peripheral access
}

uint16_t power_dev_battery_mv(void) {
  return board.getBattMilliVolts();
}

void power_dev_shutdown(void) {
  // FEM off: PA enables low, then the FEM LDO itself, held through sleep.
  // (wadamesh leaves the LDO enable floating here — indeterminate; we don't.)
  board.fem.setSleepModeEnable();
  digitalWrite(P_LORA_PA_POWER, LOW);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_PA_POWER);

  // Rails OFF (drive the P-FET gates HIGH and hold; the pull-ups would park
  // them anyway, but a held level doesn't depend on it).
  digitalWrite(HELTEC_VEXT_CTRL, HIGH);
  rtc_gpio_hold_en((gpio_num_t)HELTEC_VEXT_CTRL);
  digitalWrite(HELTEC_VGNSS_CTRL, HIGH);
  rtc_gpio_hold_en((gpio_num_t)HELTEC_VGNSS_CTRL);

  // Wake: USER button (GPIO0, active LOW) — same recipe as the T-Deck; the
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

// Standby keeps Vext (LCD sleeping, SD idle — no remount dance on exit) and
// cuts only the GNSS rail. The L76K cold-boots back to its factory 9600
// NMEA on exit, which is the rate the firmware's baud probe locked at boot;
// gps_notify_wake() then starts a fresh sync cycle.
//
// Only the FEM LDO pin (GPIO7, an RTC pad) is held across light sleep —
// RTC-domain holds are the trustworthy mechanism (the deep-sleep shutdown
// uses them). Vext (40) and VGNSS (42) are DIGITAL-only pads and are
// deliberately NOT held: the T-Deck's backlight pin proved that digital pad
// holds release unreliably after light-sleep cycles, leaving the pad
// latched through the exit's writes (here that would strand the GPS rail
// off until reboot). Driven output levels persist through light sleep on
// their own — T-Deck-proven on its V3V rail-enable — and the exit re-latches
// the pad config before re-driving as the belt. The FEM's CSD/CTX switching
// pins are not held either — the mesh task drives them on every TX in the
// awake windows.
void power_dev_standby_enter(void) {
  digitalWrite(HELTEC_VGNSS_CTRL, HIGH);         // GPS rail off for the standby
  gpio_hold_en((gpio_num_t)P_LORA_PA_POWER);     // HIGH: FEM LDO stays up (RX path)
}

void power_dev_standby_exit(void) {
  gpio_hold_dis((gpio_num_t)P_LORA_PA_POWER);
  pinMode(HELTEC_VEXT_CTRL, OUTPUT);             // re-latch pad config after sleep
  pinMode(HELTEC_VGNSS_CTRL, OUTPUT);
  digitalWrite(HELTEC_VEXT_CTRL, LOW);
  digitalWrite(HELTEC_VGNSS_CTRL, LOW);
  digitalWrite(P_LORA_PA_POWER, HIGH);
}

#endif // BOARD_HELTEC_V4
