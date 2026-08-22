// gps_dev.h — per-device GPS transport backend contract.
//
// TRANSPORT ONLY. The clock-authority tiers, sky detector, HDOP gate and
// adaptive cadence are device-neutral and stay in main.cpp; this seam covers
// the UART (pins, baud, byte access) and the per-chip init commands.

#pragma once

#include <stdint.h>

class Stream;

// Open the GPS UART at `baud` (pins are the backend's business).
void gps_dev_begin(uint32_t baud);

// Change baud on the open port without a full re-begin.
void gps_dev_update_baud(uint32_t baud);

// The byte stream: available()/read() for the NMEA parser, write() for
// wake-up bytes.
Stream& gps_dev_stream(void);

// Send whatever configuration commands this chip needs after begin().
// T-Deck: currently empty — the receiver is a u-blox MIA-M10Q whose command
// language is UBX (text dialects are silently discarded foreign framing);
// u-blox configuration could live here now. This hook is where a chip that
// accepts configuration (e.g. the L76K on other boards) sets its
// rate/constellations.
void gps_dev_chip_init(void);

// Chip-level power down / wake, used by shutdown and standby on boards whose
// receiver has no controllable power rail (T-Deck: always-on rail, the UART
// is the only lever — Allystar binary CFG-SLEEP). Heltec: no-ops, the VGNSS
// rail cut in power_heltec.cpp is the lever there.
void gps_dev_power_down(void);
void gps_dev_wake(void);

// RX pin, for the existing boot/diagnostic log lines only.
int gps_dev_rx_pin(void);
int gps_dev_tx_pin(void);
