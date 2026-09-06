// punk_heltec_board.h — MeshCore board object for the Heltec V4-R8.
//
// Subclasses MeshCore's ESP32Board in OUR tree (same convention as
// PunkSX1262Wrapper — lib/MeshCore stays pristine). Responsibilities:
//   - drive the RF front-end TX/RX switching from the transmit hooks the
//     RadioLib wrapper already calls on every packet
//   - battery measurement via the Heltec divider/ADC-gate circuit
//   - radiated-dBm <-> chip-dBm mapping for the FEM's PA gain, so the
//     Settings TX power number means dBm AT THE ANTENNA on every board
//
// Battery formula and FEM gain curve come from MeshCore's
// variants/heltec_v4 (HeltecV4Board.cpp + the GC1109 measurements in the
// Meshtastic heltec_v4 variant.h): net TX gain +11dB at 0-15dBm chip
// output, +10 at 16-17, +9 at 18-19, +7 at 21 (PA compression).

#pragma once

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include "heltec_fem.h"

class PunkHeltecBoard : public ESP32Board {
public:
  LoRaFEMControl fem;

  void begin() {
    ESP32Board::begin();
    fem.init();
    fem.setRxModeEnable();             // idle = receive path
  }

  void onBeforeTransmit(void) override { fem.setTxModeEnable(); }
  void onAfterTransmit(void) override  { fem.setRxModeEnable(); }

  // R8 battery path: permanent 390K/100K divider into GPIO1 (no ADC gate on
  // this board revision — the base V4's GPIO37 gate is a PSRAM pin here).
  // Divider ratio (390+100)/100 = 4.9; analogReadMilliVolts is
  // factory-calibrated so no manual attenuation math is needed.
  uint16_t getBattMilliVolts() override {
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) {
      mv += analogReadMilliVolts(PIN_VBAT_READ);
    }
    mv = mv / 8;
    return (uint16_t)((mv * 49UL) / 10UL);   // x4.9
  }

  const char* getManufacturerName() const override {
    return fem.getFEMType() == KCT8103L_PA ? "Heltec V4.3 TFT" : "Heltec V4 TFT";
  }
};

// Radiated dBm -> SX1262 chip dBm, inverting the FEM gain curve. The UI and
// prefs store RADIATED power (cap MAX_LORA_TX_POWER=22); this is applied at
// the single choke point in radio_hal.cpp.
static inline int8_t heltec_radiated_to_chip_dbm(int8_t radiated) {
  if (radiated > 28) radiated = 28;
  // Walk chip power up until chip + gain(chip) reaches the request.
  for (int8_t chip = -9; chip <= 22; chip++) {
    int gain = (chip <= 15) ? 11 : (chip <= 17) ? 10 : (chip <= 19) ? 9 : 7;
    if (chip + gain >= radiated) return chip;
  }
  return 22;
}

#endif // BOARD_HELTEC_V4
