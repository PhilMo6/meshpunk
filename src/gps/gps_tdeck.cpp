// gps_tdeck.cpp — T-Deck GPS transport backend (contract: gps_dev.h).
//
// T-Deck Plus onboard receiver on UART1. The chip is Allystar-class L1/L5
// dual-band (GPS+GAL+BDS+QZSS, no GLONASS, NMEA 4.1, 38400) and rejects
// every known text command dialect, so gps_dev_chip_init() is deliberately
// empty — the firmware only ever listens and probes baud rates.

#if defined(BOARD_TDECK)

#include <Arduino.h>

#include "gps_dev.h"
#include "../tdeck-pins.h"   // TDECK_GPS_RX / TX

static HardwareSerial GPSSerial(1);

void gps_dev_begin(uint32_t baud) {
  GPSSerial.begin(baud, SERIAL_8N1, TDECK_GPS_RX, TDECK_GPS_TX);
}

void gps_dev_update_baud(uint32_t baud) {
  GPSSerial.updateBaudRate(baud);
}

Stream& gps_dev_stream(void) { return GPSSerial; }

void gps_dev_chip_init(void) {
  // Nothing to send: this receiver ignores PMTK/PCAS/CASIC/UBX alike.
}

int gps_dev_rx_pin(void) { return TDECK_GPS_RX; }
int gps_dev_tx_pin(void) { return TDECK_GPS_TX; }

#endif // BOARD_TDECK
