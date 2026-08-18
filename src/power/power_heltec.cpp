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
// (LCD, touch, SD, GPS) was dark/silent until these drives landed. Rail
// power management (sleep) is a later effort; bring-up just turns them on.

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>

#include "power_dev.h"
#include "../boards/punk_heltec_board.h"

extern PunkHeltecBoard board;   // main.cpp

#define HELTEC_VEXT_CTRL   40   // P-FET gate: LOW = Vext rail ON
#define HELTEC_VGNSS_CTRL  42   // P-FET gate: LOW = GNSS rail ON

void power_dev_init(void) {
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

#endif // BOARD_HELTEC_V4
