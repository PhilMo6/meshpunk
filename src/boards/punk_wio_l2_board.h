// punk_wio_l2_board.h — MeshCore board object for the Seeed Wio Tracker L2.
//
// Subclasses MeshCore's ESP32Board in OUR tree (lib/MeshCore stays
// pristine), like PunkHeltecBoard. The battery is not on an ESP32 ADC pin
// on this board (ADS1115 on I2C), so ESP32Board's analog read would report
// 0: the MeshCore-facing read returns the power backend's instead, keeping
// one battery number for the UI, Lua, the BLE companion and the protocol
// modules (firmware_batt_mv in main.cpp).

#pragma once

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include "../power/power_dev.h"

class PunkWioL2Board : public ESP32Board {
public:
  uint16_t getBattMilliVolts() override { return power_dev_battery_mv(); }

  const char* getManufacturerName() const override { return "Seeed Wio Tracker L2"; }
};

#endif // BOARD_WIO_L2
