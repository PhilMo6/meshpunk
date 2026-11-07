// aw35615.h — Awinic AW35615 USB Type-C port controller (I2C), register-
// compatible with the onsemi FUSB302: CC terminations (Rp current source,
// Rd pull-down), autonomous SRC/SNK/DRP detection, a VBUS comparator
// (4.0 V threshold) and a VBUS discharge resistor. It senses VBUS; it does
// not switch VBUS power — the board does.
//
// Register and bit values: Awinic AW35615 datasheet V1.7 (register list and
// detailed descriptions, detection chapter).
//
// Board-neutral: the board brings up the I2C bus. Interrupts stay masked
// (CONTROL0 INT_MASK, its reset value); callers poll the status registers.

#pragma once

#include <stdint.h>

class TwoWire;

// Probe, then software reset (RESET SW_RES): every register back to its
// power-on default, which is Rd on CC1 and CC2 (a sink: what a charger
// needs before it supplies VBUS) with toggling off. *vendor_id = VDH:VDL
// (0x344F on this chip), *device_id = DEVICE_ID. False when the chip does not
// answer at `addr` or the vendor ID differs; no reset is written then.
bool aw35615_reset(TwoWire& wire, uint8_t addr, uint16_t* vendor_id, uint8_t* device_id);

// Source-role detection: every analog block powered (POWER 0x0F), Rp at
// default USB current (HOST_CUR 01), CC switches cleared, toggling
// restarted in SRC mode; the toggle stops only on an Rd termination
// (TOG_RD_ONLY). Requires a successful aw35615_reset().
bool aw35615_source_detect(void);

// After aw35615_source_detect(): *cc = 1 or 2 when the toggle has stopped as
// a source with a sink's Rd on CC1 / CC2 (STATUS1A TOGSS 001 / 010), else 0.
bool aw35615_detected_sink(uint8_t* cc);

// Manual source role on one CC pin: Rp and the measure block on `cc`, the
// DAC at the default-current Rd threshold (MDAC 0x26 = 1.596 V), toggling
// off. SWITCHES0 is written before TOGGLE clears, so Rp is already set in the
// register when manual mode takes over from the toggle logic.
bool aw35615_source_hold(uint8_t cc);

// STATUS0: *vbus_ok = VBUSOK (VBUS at or above 4.0 V); *cc_above = COMP (the
// measured CC pin above the MDAC threshold — while holding, the sink's Rd is
// gone). Either pointer may be null.
bool aw35615_status(bool* vbus_ok, bool* cc_above);

// VBUS discharge through the 660 ohm resistor (CONTROL5 VBUS_DIS_SEL 01,
// written with CONTROL4 EN_PAR_CFG set); off clears both registers.
bool aw35615_vbus_discharge(bool on);
