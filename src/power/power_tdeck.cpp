// power_tdeck.cpp — T-Deck power/battery backend (contract: power_dev.h).
//
// Peripheral power rail on BOARD_POWERON, battery on a 1:2 divider read by
// the ESP32-S3 ADC (PIN_VBAT_READ, set in platformio.ini). The averaging and
// scaling match MeshCore's ESP32Board::getBattMilliVolts() exactly — that
// object stays in main.cpp for MeshCore's own use, and both paths must
// report the same number.
//
// Power tree (LilyGo schematic, decoded 2026-08-18): BOARD_POWERON (GPIO10)
// drives a diode-OR into an NPN+PMOS latch that switches the V3V rail
// (radio, keyboard MCU, LCD, SD, codec, trackball). A 100K pulldown on the
// latch parks V3V OFF when GPIO10 stops driving. The other OR input is
// TP_EN, a touch-pulse charge-hold from the GT911's INT line — putting the
// GT911 to sleep (input_dev_shutdown_prepare) disarms it. VDD3V3 (ESP, GPS
// port, GT911, charger) is resistor-strapped on and cannot be software-cut.

#if defined(BOARD_TDECK)

#include <Arduino.h>
#include <driver/gpio.h>     // gpio_hold_en (standby pad holds)
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>      // esp_reset_reason

#include "power_dev.h"
#include "../utilities.h"   // BOARD_POWERON, BOARD_BOOT_PIN

void power_dev_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // Waking from power-off: release the RTC holds/routing the shutdown path
    // armed, or the pinMode calls below (and input_dev_preinit's GPIO0 pull)
    // silently do nothing against a frozen pad.
    rtc_gpio_hold_dis((gpio_num_t)BOARD_POWERON);
    rtc_gpio_deinit((gpio_num_t)BOARD_POWERON);
    rtc_gpio_deinit((gpio_num_t)BOARD_BOOT_PIN);
    // input_dev_preinit() already ran with GPIO0 possibly still RTC-routed;
    // re-assert its digital config now that the pad is released.
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  }

  // The board peripheral power control pin needs to be set to HIGH when using
  // the peripheral
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
}

uint16_t power_dev_battery_mv(void) {
#ifdef PIN_VBAT_READ
  analogReadResolution(12);

  uint32_t raw = 0;
  for (int i = 0; i < 4; i++) {
    raw += analogReadMilliVolts(PIN_VBAT_READ);
  }
  raw = raw / 4;

  return (uint16_t)(2 * raw);   // 1:2 divider
#else
  return 0;   // not supported
#endif
}

void power_dev_shutdown(void) {
  // Kill the V3V rail deterministically: drive the latch input LOW and hold
  // it through deep sleep (an undriven pad would also park the rail off via
  // the latch's pulldown, but a held LOW doesn't depend on it).
  digitalWrite(BOARD_POWERON, LOW);
  rtc_gpio_hold_en((gpio_num_t)BOARD_POWERON);

  // Wake: trackball click (GPIO0, active LOW). The pad's normal pull-up dies
  // in deep sleep; the RTC-domain pull-up persists. Without it the pad
  // floats LOW and the ext0 wake fires the instant we sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_init((gpio_num_t)BOARD_BOOT_PIN);
  rtc_gpio_set_direction((gpio_num_t)BOARD_BOOT_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_PIN, 0);

  esp_deep_sleep_start();   // never returns; a click reboots
}

// The radio lives on V3V, so standby's light sleep must keep GPIO10 driving
// HIGH. Output levels normally persist through light sleep; the RTC-domain
// pad hold makes that independent of any sleep-mode pad switching (GPIO10
// is an RTC pad — the same hold mechanism the deep-sleep shutdown uses).
// The backlight pulse line (GPIO42) is deliberately NOT held: it is a
// digital-only pad whose separate hold hardware released unreliably after
// light-sleep cycles, leaving the pad latched LOW through the exit's
// backlight re-init (intermittent dark screen on an otherwise awake
// device). Float noise on it during sleep is harmless — the exit path runs
// display_dev_backlight_reset(), which forces the chip through a full
// shutdown regardless. The Plus GPS sits on the always-on rail with no
// power control — nothing to park for it.
void power_dev_standby_enter(void) {
  gpio_hold_en((gpio_num_t)BOARD_POWERON);   // V3V stays up (radio RX)
}

void power_dev_standby_exit(void) {
  gpio_hold_dis((gpio_num_t)BOARD_POWERON);
  digitalWrite(BOARD_POWERON, HIGH);
}

#endif // BOARD_TDECK
