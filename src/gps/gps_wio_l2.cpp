// gps_wio_l2.cpp — Seeed Wio Tracker L2 GPS transport backend
// (contract: gps_dev.h).
//
// Quectel L76K on UART1: ESP32 RX = GPIO18, ESP32 TX = GPIO17 — the
// Meshtastic seeed_wio_tracker_L2 variant's GPS_RX_PIN 18 / GPS_TX_PIN 17,
// which Meshtastic passes to HardwareSerial::begin as (rx, tx). The Seeed
// wiki lists GNSS_RX = GPIO18 / GNSS_TX = GPIO17 without naming the side;
// the wadamesh port swaps them.
//
// Power and reset are expander lines (GNSS_PWR P15, GNSS_RST P11 asserted
// HIGH), sequenced by wio_l2_board_init(); standby cuts the rail in
// power_wio_l2.cpp.
//
// The L76K accepts standard PCAS commands — chip_init is where
// rate/constellation tuning goes when wanted. Bring-up keeps the factory
// defaults (NMEA @ 9600, which the firmware's baud probe locks onto).

#if defined(BOARD_WIO_L2)

#include <Arduino.h>

#include "gps_dev.h"

#define WIO_GPS_RX      18   // ESP32 RX  <- L76K TXD
#define WIO_GPS_TX      17   // ESP32 TX  -> L76K RXD

static HardwareSerial GPSSerial(1);

void gps_dev_begin(uint32_t baud) {
  GPSSerial.begin(baud, SERIAL_8N1, WIO_GPS_RX, WIO_GPS_TX);
}

void gps_dev_update_baud(uint32_t baud) {
  GPSSerial.updateBaudRate(baud);
}

Stream& gps_dev_stream(void) { return GPSSerial; }

void gps_dev_chip_init(void) {
  // Factory defaults at bring-up; PCAS tuning can go here later.
}

// Power is handled at the rail (expander GNSS_PWR), so there is nothing to
// say to the chip itself.
void gps_dev_power_down(void) {}
void gps_dev_wake(void)       {}

int gps_dev_rx_pin(void) { return WIO_GPS_RX; }
int gps_dev_tx_pin(void) { return WIO_GPS_TX; }

#endif // BOARD_WIO_L2
