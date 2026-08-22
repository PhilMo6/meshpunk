// gps_heltec.cpp — Heltec V4-R8 GPS transport backend (contract: gps_dev.h).
//
// Quectel L76K on UART1: ESP32 RX = GPIO39 (L76K TXD), ESP32 TX = GPIO38 —
// per the R8's own pinmap (GNSS_TX on GPIO39 means the receiver's output).
//
// Power: VGNSS_Ctrl = GPIO42, an AO3401 P-FET gate (LOW = on) driven by
// power_dev_init() in power_heltec.cpp — the pull-up on it parks the
// receiver OFF at reset, so nothing streams until that drive lands.
// PPS on GPIO41 is not used yet.
//
// The L76K accepts standard PCAS/PMTK commands — chip_init is where
// rate/constellation tuning goes when wanted. Bring-up keeps the factory
// defaults (NMEA @ 9600, which the firmware's baud probe locks onto).

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>

#include "gps_dev.h"

#define HELTEC_GPS_RX      39   // ESP32 RX  <- L76K TXD
#define HELTEC_GPS_TX      38   // ESP32 TX  -> L76K RXD

static HardwareSerial GPSSerial(1);

void gps_dev_begin(uint32_t baud) {
  GPSSerial.begin(baud, SERIAL_8N1, HELTEC_GPS_RX, HELTEC_GPS_TX);
}

void gps_dev_update_baud(uint32_t baud) {
  GPSSerial.updateBaudRate(baud);
}

Stream& gps_dev_stream(void) { return GPSSerial; }

void gps_dev_chip_init(void) {
  // Nothing to drive at bring-up: power/wake rails default ON via hardware
  // pull-ups (GPIO40/42 — never drive 40 low here, it would also cut the
  // display's Vext). PCAS/PMTK tuning can go here later.
}

// Power is handled at the rail: power_heltec.cpp cuts VGNSS for standby and
// shutdown, so there is nothing to say to the chip itself.
void gps_dev_power_down(void) {}
void gps_dev_wake(void)       {}

int gps_dev_rx_pin(void) { return HELTEC_GPS_RX; }
int gps_dev_tx_pin(void) { return HELTEC_GPS_TX; }

#endif // BOARD_HELTEC_V4
