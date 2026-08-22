// gps_tdeck.cpp — T-Deck GPS transport backend (contract: gps_dev.h).
//
// T-Deck Plus onboard receiver on UART1: a u-blox MIA-M10Q (the daughter
// board's U1 designator; L1 GPS+GAL+BDS+QZSS, NMEA 4.1, 38400). Its command
// language is UBX — text dialects (PMTK/PCAS/CASIC) are foreign framing
// that a u-blox discards without a NAK, which is why earlier probing looked
// like a chip that rejects everything.

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
  // Nothing sent yet. The module speaks UBX (see the file header); u-blox
  // configuration (rate/constellations via VALSET) could live here now.
}

// UBX-RXM-PMREQ (u-blox): backup sleep until a wake source. The module is a
// u-blox MIA-M10Q — read directly off the T-Deck-GPS daughter schematic
// (U1) after every non-UBX dialect met total silence; a u-blox discards
// foreign framing without a NAK, which is why the earlier "rejects
// everything" probing looked the way it did. v1 16-byte payload: version 0,
// duration 0 (= sleep until woken), flags = backup, wakeupSources = uartrx
// (gps_dev_wake's byte). PMREQ is never ACKed; the NMEA stream going silent
// is the confirmation. A receiver that ignores it just keeps running.
void gps_dev_power_down(void) {
  uint8_t f[24];
  const uint8_t pay[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                            0x02, 0, 0, 0, 0xE8, 0, 0, 0 };
  f[0] = 0xB5; f[1] = 0x62; f[2] = 0x02; f[3] = 0x41; f[4] = 16; f[5] = 0;
  memcpy(&f[6], pay, sizeof(pay));
  uint8_t a = 0, b = 0;
  for (int i = 2; i < 22; i++) { a += f[i]; b += a; }
  f[22] = a;
  f[23] = b;
  GPSSerial.write(f, sizeof(f));
  GPSSerial.flush();   // drain before any power/deep-sleep transition
}

void gps_dev_wake(void) {
  // Hw-proven wake from PMREQ backup: a small CONTIGUOUS UBX frame (the
  // MON-VER poll). Single bytes and spaced edge bursts do NOT wake this
  // module; a dense frame does — and the M10Q's wake is fast enough that
  // it even answers the frame that woke it. Sent twice with a gap purely
  // as margin; both are harmless no-ops on an already-awake receiver.
  static const uint8_t ver[] = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };
  GPSSerial.write(ver, sizeof(ver));
  delay(100);
  GPSSerial.write(ver, sizeof(ver));
}

int gps_dev_rx_pin(void) { return TDECK_GPS_RX; }
int gps_dev_tx_pin(void) { return TDECK_GPS_TX; }

#endif // BOARD_TDECK
