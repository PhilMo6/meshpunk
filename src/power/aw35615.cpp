// aw35615.cpp — see aw35615.h.

#include "aw35615.h"

#include <Arduino.h>
#include <Wire.h>

// Registers (datasheet register list).
#define REG_DEVICE_ID   0x01
#define REG_SWITCHES0   0x02
#define REG_MEASURE     0x04
#define REG_CONTROL0    0x06
#define REG_CONTROL2    0x08
#define REG_POWER       0x0B
#define REG_RESET       0x0C
#define REG_CONTROL4    0x10
#define REG_CONTROL5    0x11
#define REG_VDL         0x3A
#define REG_VDH         0x3B
#define REG_STATUS1A    0x3D
#define REG_STATUS0     0x40

#define VENDOR_ID       0x344F

// SWITCHES0: PU_EN2 (7) / PU_EN1 (6) = Rp current source on CC2 / CC1,
// MEAS_CC2 (3) / MEAS_CC1 (2) = measure block on CC2 / CC1.
#define SW0_PU_EN2      0x80
#define SW0_PU_EN1      0x40
#define SW0_MEAS_CC2    0x08
#define SW0_MEAS_CC1    0x04

// CONTROL0 = its reset value: INT_MASK (5) set, HOST_CUR (3:2) 01 = 80 uA,
// default USB power.
#define CTL0_DEFAULT_CUR_MASKED  0x24

// CONTROL2: TOG_RD_ONLY (5) = only Rd stops the toggle, MODE (2:1) 11 = SRC
// polling, TOGGLE (0) = automatic detection on.
#define CTL2_SRC_RD_ONLY         0x26
#define CTL2_TOGGLE              0x01
#define CTL2_MANUAL              0x00

// POWER (3:0): bandgap + wake, receiver + measure current references,
// measure block, internal oscillator.
#define POWER_ALL       0x0F

#define RESET_SW_RES    0x01

// MEASURE: MEAS_VBUS (6) = 0 (measure CC), MDAC (5:0) = 0x26: 38 x 42 mV =
// 1.596 V, the Rd attach/detach threshold for default and 1.5 A Rp current
// (datasheet SRC detection table).
#define MEASURE_RD_DEFAULT  0x26

#define STATUS0_VBUSOK  0x80
#define STATUS0_COMP    0x20

// STATUS1A TOGSS (5:3): 001 = stopped as SRC with a sink on CC1, 010 = on CC2.
#define TOGSS_SHIFT     3
#define TOGSS_MASK      0x07
#define TOGSS_SRC_CC1   0x01
#define TOGSS_SRC_CC2   0x02

// CONTROL4 EN_PAR_CFG (1) enables writes to CONTROL5; CONTROL5 VBUS_DIS_SEL
// (4:3) 01 = 660 ohm discharge.
#define CTL4_EN_PAR_CFG     0x02
#define CTL5_VBUS_DIS_660   0x08

static TwoWire* s_wire = nullptr;
static uint8_t  s_addr = 0;

static bool wr(uint8_t reg, uint8_t val) {
  s_wire->beginTransmission(s_addr);
  s_wire->write(reg);
  s_wire->write(val);
  return s_wire->endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t* val) {
  s_wire->beginTransmission(s_addr);
  s_wire->write(reg);
  if (s_wire->endTransmission(false) != 0) return false;
  if (s_wire->requestFrom((int)s_addr, 1) != 1) return false;
  *val = s_wire->read();
  return true;
}

bool aw35615_reset(TwoWire& wire, uint8_t addr, uint16_t* vendor_id, uint8_t* device_id) {
  s_wire = &wire;
  s_addr = addr;
  uint8_t vdl = 0, vdh = 0, id = 0;
  *vendor_id = 0;
  *device_id = 0;
  if (!rd(REG_VDL, &vdl) || !rd(REG_VDH, &vdh) || !rd(REG_DEVICE_ID, &id)) return false;
  *vendor_id = (uint16_t)((vdh << 8) | vdl);
  *device_id = id;
  if (*vendor_id != VENDOR_ID) return false;
  if (!wr(REG_RESET, RESET_SW_RES)) return false;
  delay(1);   // the datasheet gives no reset time
  return true;
}

// TOGGLE is cleared before it is set (the Linux fusb302 driver's restart
// order), so the write that sets it is a 0 -> 1 change whether or not the
// chip cleared the bit when the toggle stopped.
bool aw35615_source_detect(void) {
  return wr(REG_POWER, POWER_ALL) &&
         wr(REG_CONTROL0, CTL0_DEFAULT_CUR_MASKED) &&
         wr(REG_SWITCHES0, 0x00) &&
         wr(REG_CONTROL2, CTL2_SRC_RD_ONLY) &&
         wr(REG_CONTROL2, CTL2_SRC_RD_ONLY | CTL2_TOGGLE);
}

bool aw35615_detected_sink(uint8_t* cc) {
  uint8_t s1a = 0;
  if (!rd(REG_STATUS1A, &s1a)) return false;
  uint8_t togss = (s1a >> TOGSS_SHIFT) & TOGSS_MASK;
  *cc = togss == TOGSS_SRC_CC1 ? 1 : togss == TOGSS_SRC_CC2 ? 2 : 0;
  return true;
}

bool aw35615_source_hold(uint8_t cc) {
  uint8_t sw0 = (cc == 1) ? (SW0_PU_EN1 | SW0_MEAS_CC1) : (SW0_PU_EN2 | SW0_MEAS_CC2);
  return wr(REG_SWITCHES0, sw0) &&
         wr(REG_MEASURE, MEASURE_RD_DEFAULT) &&
         wr(REG_CONTROL2, CTL2_MANUAL);
}

bool aw35615_status(bool* vbus_ok, bool* cc_above) {
  uint8_t s0 = 0;
  if (!rd(REG_STATUS0, &s0)) return false;
  if (vbus_ok)  *vbus_ok  = (s0 & STATUS0_VBUSOK) != 0;
  if (cc_above) *cc_above = (s0 & STATUS0_COMP) != 0;
  return true;
}

bool aw35615_vbus_discharge(bool on) {
  if (on) return wr(REG_CONTROL4, CTL4_EN_PAR_CFG) && wr(REG_CONTROL5, CTL5_VBUS_DIS_660);
  return wr(REG_CONTROL5, 0x00) && wr(REG_CONTROL4, 0x00);
}
