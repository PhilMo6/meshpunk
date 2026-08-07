// power_tdeck.cpp — T-Deck power/battery backend (contract: power_dev.h).
//
// Peripheral power rail on BOARD_POWERON, battery on a 1:2 divider read by
// the ESP32-S3 ADC (PIN_VBAT_READ, set in platformio.ini). The averaging and
// scaling match MeshCore's ESP32Board::getBattMilliVolts() exactly — that
// object stays in main.cpp for MeshCore's own use, and both paths must
// report the same number.

#if defined(BOARD_TDECK)

#include <Arduino.h>

#include "power_dev.h"
#include "../utilities.h"   // BOARD_POWERON

void power_dev_init(void) {
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

#endif // BOARD_TDECK
