// es8311.cpp — see es8311.h.
//
// The codec keeps its registers through an ESP32 reset, so the init first
// clears what earlier firmware can leave behind: REG44 back to its reset
// value 0x00 if it holds anything else, then a digital reset (REG00 0x1F,
// then 0x00 — the ESPHome driver's order; the 5 ms hold between them is the
// Linux driver's, whose comment notes the datasheet gives none).
//
// Then the register sequence of Espressif's esp_codec_dev ES8311 driver
// (esp-adf components/esp_codec_dev/device/es8311/es8311.c, Apache-2.0),
// without its two REG44 = 0x08 writes ("I2C noise immunity"): the Linux,
// ESPHome and arduino-audio-driver ES8311 drivers never write REG44, and on
// the Wio L2 the codec NACKs the second of them. In Espressif's call order:
//   es8311_open         slave, MCLK from the MCLK pin, no MCLK/SCLK
//                       inversion
//   es8311_config_sample  the 256 x fs coefficient row: pre_div 1,
//                       pre_multi x1, adc/dac_div 1, single speed,
//                       lrck 0x00FF, bclk_div 4, adc/dac osr 0x10
//   bits + format       16-bit, standard I2S
//   es8311_start        DAC mode
// then DAC volume 0 dB (0xBF) and unmute.

#include "es8311.h"

#include <Arduino.h>
#include <Wire.h>

#include "../meshpunk_sync.h"   // SLog

static TwoWire* s_wire = nullptr;
static uint8_t  s_addr = 0;

// First failed register access of the last es8311_init(): its step (1-based
// count of rd/wr calls), register and code. For a write the code is
// TwoWire::endTransmission()'s: 2 = ESP_FAIL from the I2C driver (any NACK,
// address or data), 4 = other error, 5 = timeout. A read's
// endTransmission(false) only queues the register address and the transfer
// runs in requestFrom(), so a failed read is code 0xFF (no byte returned).
static unsigned    s_step      = 0;
static unsigned    s_fail_step = 0;
static const char* s_fail_op   = "";
static uint8_t     s_fail_reg  = 0;
static uint8_t     s_fail_code = 0;

static void note_fail(const char* op, uint8_t reg, uint8_t code) {
  if (s_fail_step) return;
  s_fail_step = s_step;
  s_fail_op   = op;
  s_fail_reg  = reg;
  s_fail_code = code;
}

static bool wr(uint8_t reg, uint8_t val) {
  s_step++;
  s_wire->beginTransmission(s_addr);
  s_wire->write(reg);
  s_wire->write(val);
  uint8_t code = s_wire->endTransmission();
  if (code != 0) note_fail("write", reg, code);
  return code == 0;
}

static bool rd(uint8_t reg, uint8_t* val) {
  s_step++;
  s_wire->beginTransmission(s_addr);
  s_wire->write(reg);
  uint8_t code = s_wire->endTransmission(false);
  if (code != 0) {
    note_fail("read", reg, code);
    return false;
  }
  if (s_wire->requestFrom((int)s_addr, 1) != 1) {
    note_fail("read", reg, 0xFF);
    return false;
  }
  *val = s_wire->read();
  return true;
}

// Read-modify-write: keep the bits in `keep`, OR in `set`.
static bool rmw(uint8_t reg, uint8_t keep, uint8_t set) {
  uint8_t v;
  if (!rd(reg, &v)) return false;
  return wr(reg, (uint8_t)((v & keep) | set));
}

// Address-only probes, 100 us apart, until the codec ACKs, for up to
// timeout_us. Returns the number of NACKed probes before the ACK, or -1 at
// the timeout (recorded as this step's failure, code 5). One step.
static int wait_ack(uint32_t timeout_us, uint32_t* waited_us) {
  s_step++;
  uint32_t t0 = micros();
  int nacks = 0;
  for (;;) {
    s_wire->beginTransmission(s_addr);
    if (s_wire->endTransmission() == 0) {
      *waited_us = micros() - t0;
      return nacks;
    }
    nacks++;
    if (micros() - t0 >= timeout_us) {
      *waited_us = micros() - t0;
      note_fail("ack-wait", 0x44, 5);
      return -1;
    }
    delayMicroseconds(100);
  }
}

bool es8311_init(TwoWire& wire, uint8_t addr) {
  s_wire = &wire;
  s_addr = addr;
  s_step = 0;
  s_fail_step = 0;

  // Presence: the chip ID registers FD/FE/FF answer.
  uint8_t id1 = 0, id2 = 0, ver = 0;
  if (!rd(0xFD, &id1) || !rd(0xFE, &id2) || !rd(0xFF, &ver)) {
    SLog.printf("[AUDIO] FAIL: ES8311 not answering at 0x%02X\n", addr);
    return false;
  }

  bool ok = true;
  uint8_t v = 0;

  // REG44 back to its reset value; the next write waits for the codec's ACK.
  uint8_t r44 = 0;
  ok = rd(0x44, &r44);
  if (ok && r44 != 0x00) {
    ok = wr(0x44, 0x00);
    if (ok) {
      uint32_t waited_us = 0;
      int nacks = wait_ack(20000, &waited_us);
      if (nacks >= 0) {
        SLog.printf("[AUDIO] ES8311 REG44 was 0x%02X, now 0x00; codec ACKed after %d NACKed probe(s), %u us\n",
                    r44, nacks, (unsigned)waited_us);
      } else {
        SLog.printf("[AUDIO] ES8311 REG44 was 0x%02X, now 0x00; no ACK within %u us\n",
                    r44, (unsigned)waited_us);
        ok = false;
      }
    }
  }

  // Digital reset, then out of reset with the state machine off.
  ok = ok && wr(0x00, 0x1F);
  if (ok) delay(5);
  ok = ok && wr(0x00, 0x00);

  // es8311_open
  ok = ok && rd(0x0D, &v);
  if (ok && v != 0xFA) ok = wr(0x0D, 0xFA);
  ok = ok && wr(0x01, 0x30);
  ok = ok && wr(0x02, 0x00);
  ok = ok && wr(0x03, 0x10);
  ok = ok && wr(0x16, 0x24);
  ok = ok && wr(0x04, 0x10);
  ok = ok && wr(0x05, 0x00);
  ok = ok && wr(0x0B, 0x00);
  ok = ok && wr(0x0C, 0x00);
  ok = ok && wr(0x10, 0x1F);
  ok = ok && wr(0x11, 0x7F);
  ok = ok && wr(0x00, 0x80);
  ok = ok && rmw(0x00, 0xBF, 0x00);            // slave
  ok = ok && wr(0x01, 0x3F);                   // clocks on, MCLK pin, not inverted
  ok = ok && rmw(0x06, (uint8_t)~0x20, 0x00);  // SCLK not inverted
  ok = ok && wr(0x13, 0x10);
  ok = ok && wr(0x1B, 0x0A);
  ok = ok && wr(0x1C, 0x6A);

  // es8311_config_sample, 256 x fs
  ok = ok && rmw(0x02, 0x07, 0x00);
  ok = ok && wr(0x05, 0x00);
  ok = ok && rmw(0x03, 0x80, 0x10);
  ok = ok && rmw(0x04, 0x80, 0x10);
  ok = ok && rmw(0x07, 0xC0, 0x00);
  ok = ok && wr(0x08, 0xFF);
  ok = ok && rmw(0x06, 0xE0, 0x03);

  // 16-bit, then standard I2S format
  ok = ok && rmw(0x09, 0xFF, 0x0C);
  ok = ok && rmw(0x0A, 0xFF, 0x0C);
  ok = ok && rmw(0x09, 0xFC, 0x00);
  ok = ok && rmw(0x0A, 0xFC, 0x00);

  // es8311_start, DAC mode (it clears REG09/0A bit 6 for DAC mode)
  ok = ok && wr(0x00, 0x80);                   // slave
  ok = ok && wr(0x01, 0x3F);
  ok = ok && rmw(0x09, 0xBF, 0x00);
  ok = ok && rmw(0x0A, 0xBF, 0x00);
  ok = ok && wr(0x17, 0xBF);
  ok = ok && wr(0x0E, 0x02);
  ok = ok && wr(0x12, 0x00);
  ok = ok && wr(0x14, 0x1A);
  ok = ok && rmw(0x14, (uint8_t)~0x40, 0x00);  // analog mic, not PDM
  ok = ok && wr(0x0D, 0x01);
  ok = ok && wr(0x15, 0x40);
  ok = ok && wr(0x37, 0x08);
  ok = ok && wr(0x45, 0x00);

  // DAC volume 0 dB, unmuted
  ok = ok && wr(0x32, 0xBF);
  ok = ok && rmw(0x31, 0x9F, 0x00);

  if (!ok) {
    SLog.printf("[AUDIO] FAIL: ES8311 register access during init: step %u, %s REG%02X, code %u\n",
                s_fail_step, s_fail_op, s_fail_reg, (unsigned)s_fail_code);
    return false;
  }
  SLog.println("[AUDIO] ES8311 up: I2S slave, 16-bit, MCLK 256 x fs, DAC 0 dB");
  return true;
}
