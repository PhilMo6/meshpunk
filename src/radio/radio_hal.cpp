#include "radio_hal.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <ctype.h>
#include <string.h>

#include "../meshpunk_sync.h"   // SPI_LOCK/UNLOCK, SLog

#if defined(BOARD_HELTEC_V4)
#include "../boards/punk_heltec_board.h"   // heltec_radiated_to_chip_dbm()
#endif

static SX1262* s_radio = nullptr;

// Last-programmed state the recovery/score paths need back: operating
// frequency (image recalibration after an AGC reset), spreading factor
// (per-SF packet scoring), rx boost (re-applied after an AGC reset).
static float   s_last_freq_mhz = 0.0f;
static uint8_t s_last_sf = 10;
static bool    s_rx_boost = false;

// Radiated-dBm (UI/prefs unit) -> chip output dBm. Same mapping as
// board_tx_dbm_to_chip in main.cpp: identity except on boards with a PA
// front-end.
static inline int8_t hal_tx_dbm_to_chip(int8_t radiated) {
#if defined(BOARD_HELTEC_V4)
  return heltec_radiated_to_chip_dbm(radiated);
#else
  return radiated;
#endif
}

void radio_hal_init(SX1262* radio) {
  s_radio = radio;
}

bool radio_hal_begin() {
  if (!s_radio) return false;
  SPI_LOCK();
  int16_t state = s_radio->begin();
  SLog.printf("[RADIO] begin() = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
#if defined(BOARD_HELTEC_V4)
  // SX1262 module wiring on this board: 1.8V TCXO on DIO3, the FEM's TX/RX
  // switch driven from DIO2, 140mA chip limit (the PA has its own LDO).
  int16_t s = s_radio->setTCXO(1.8);
  SLog.printf("[RADIO] setTCXO(1.8) = %d %s\n", s,
              s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  s = s_radio->setDio2AsRfSwitch(true);
  SLog.printf("[RADIO] setDio2AsRfSwitch = %d %s\n", s,
              s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  s_radio->setCurrentLimit(140);
#endif
  SPI_UNLOCK();
  return state == RADIOLIB_ERR_NONE;
}

bool radio_hal_config(const RadioParams& p) {
  if (!s_radio) return false;
  bool ok = true;
  SLog.printf("[RADIO] Setting freq=%.3f MHz, BW=%.0f kHz, SF=%d, CR=%d, TX=%d dBm\n",
              p.freq_mhz, p.bw_khz, p.sf, p.cr, (int)p.tx_dbm_radiated);
  SPI_LOCK();
  int16_t state = s_radio->setFrequency(p.freq_mhz);
  SLog.printf("[RADIO] setFrequency = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_radio->setBandwidth(p.bw_khz);
  SLog.printf("[RADIO] setBandwidth = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_radio->setSpreadingFactor(p.sf);
  SLog.printf("[RADIO] setSpreadingFactor = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_radio->setCodingRate(p.cr);
  SLog.printf("[RADIO] setCodingRate = %d %s\n", state,
              state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);

  state = s_radio->setSyncWord(p.sync_word);
  ok &= (state == RADIOLIB_ERR_NONE);
  int16_t state2 = s_radio->setPreambleLength(p.preamble_len);
  ok &= (state2 == RADIOLIB_ERR_NONE);
  SLog.printf("[RADIO] setSyncWord(0x%02X) = %d, setPreambleLength(%u) = %d\n",
              p.sync_word, state, (unsigned)p.preamble_len, state2);

  // LoRa CRC is on/off; pass 1/0 exactly as the pre-HAL code's setCRC(true).
  s_radio->setCRC(p.crc ? 1 : 0);

  int8_t chip = hal_tx_dbm_to_chip(p.tx_dbm_radiated);
  state = s_radio->setOutputPower(chip);
  SLog.printf("[RADIO] setOutputPower(chip %d for %d radiated) = %d %s\n",
              (int)chip, (int)p.tx_dbm_radiated,
              state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  ok &= (state == RADIOLIB_ERR_NONE);
  SPI_UNLOCK();
  s_last_freq_mhz = p.freq_mhz;
  s_last_sf = p.sf;
  return ok;
}

bool radio_hal_set_tx_power(int8_t radiated_dbm) {
  if (!s_radio) return false;
  int8_t chip = hal_tx_dbm_to_chip(radiated_dbm);
  SPI_LOCK();
  int16_t s = s_radio->setOutputPower(chip);
  SPI_UNLOCK();
  SLog.printf("[RADIO] setOutputPower(chip %d for %d radiated) = %d %s\n",
              (int)chip, (int)radiated_dbm,
              s, s == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  return s == RADIOLIB_ERR_NONE;
}

uint8_t radio_hal_last_sf() { return s_last_sf; }

// ── Raw ops (protocol modules) ───────────────────────────────────────────────

// Armed-in-RX tracking (every transition below updates it; every protocol
// module drives the chip through these raw ops via the MeshHostApi table).
static volatile bool s_in_recv = false;

bool radio_hal_start_receive() {
  if (!s_radio) return false;
  SPI_LOCK();
  int16_t s = s_radio->startReceive();
  SPI_UNLOCK();
  s_in_recv = (s == RADIOLIB_ERR_NONE);
  return s == RADIOLIB_ERR_NONE;
}

bool radio_hal_in_recv() { return s_in_recv; }

uint32_t radio_hal_poll_irq() {
  if (!s_radio) return 0;
  SPI_LOCK();
  uint32_t flags = s_radio->getIrqFlags();
  SPI_UNLOCK();
  return flags;
}

int radio_hal_read_packet(uint8_t* buf, int max_len) {
  if (!s_radio || !buf || max_len <= 0) return -1;
  SPI_LOCK();
  int len = (int)s_radio->getPacketLength();
  if (len > max_len) len = max_len;
  int r = -1;
  if (len > 0 && s_radio->readData(buf, (size_t)len) == RADIOLIB_ERR_NONE) r = len;
  // readData leaves the chip in standby; re-enter RX so the poll loop never
  // strands the receiver.
  s_radio->startReceive();
  s_in_recv = true;
  SPI_UNLOCK();
  return r;
}

bool radio_hal_start_send(const uint8_t* buf, int len) {
  if (!s_radio || !buf || len <= 0) return false;
  SPI_LOCK();
  int16_t s = s_radio->startTransmit(buf, (size_t)len);
  SPI_UNLOCK();
  if (s == RADIOLIB_ERR_NONE) s_in_recv = false;   // in TX until send_finished
  return s == RADIOLIB_ERR_NONE;
}

void radio_hal_send_finished() {
  if (!s_radio) return;
  SPI_LOCK();
  s_radio->finishTransmit();
  s_radio->startReceive();
  SPI_UNLOCK();
  s_in_recv = true;
}

float radio_hal_last_rssi() {
  if (!s_radio) return 0.0f;
  SPI_LOCK();
  float r = s_radio->getRSSI();
  SPI_UNLOCK();
  return r;
}

float radio_hal_last_snr() {
  if (!s_radio) return 0.0f;
  SPI_LOCK();
  float r = s_radio->getSNR();
  SPI_UNLOCK();
  return r;
}

bool radio_hal_standby() {
  if (!s_radio) return false;
  SPI_LOCK();
  int16_t s = s_radio->standby();
  SPI_UNLOCK();
  s_in_recv = false;
  return s == RADIOLIB_ERR_NONE;
}

bool radio_hal_sleep() {
  if (!s_radio) return false;
  SPI_LOCK();
  int16_t s = s_radio->sleep();
  SPI_UNLOCK();
  s_in_recv = false;
  return s == RADIOLIB_ERR_NONE;
}

void radio_hal_set_rx_boost(bool en) {
  if (!s_radio) return;
  SPI_LOCK();
  s_radio->setRxBoostedGainMode(en, true);
  SPI_UNLOCK();
  s_rx_boost = en;
}

bool radio_hal_rx_boost() { return s_rx_boost; }

float radio_hal_current_rssi() {
  if (!s_radio) return 0.0f;
  SPI_LOCK();
  float r = s_radio->getRSSI(false);   // instantaneous, not the last packet's
  SPI_UNLOCK();
  return r;
}

uint32_t radio_hal_time_on_air_ms(int len_bytes) {
  if (!s_radio || len_bytes <= 0) return 0;
  SPI_LOCK();
  uint32_t us = (uint32_t)s_radio->getTimeOnAir((size_t)len_bytes);
  SPI_UNLOCK();
  return us / 1000;
}

// Full receiver recovery (MeshCore's sx126xResetAGC, HAL-owned): warm sleep
// powers down the analog front end, Calibrate(ALL) refreshes ADC/PLL/image
// calibration, then image calibration is redone for the operating band
// (Calibrate defaults it to 902-928 MHz) and the board wiring + rx boost the
// calibration cleared are re-applied. Leaves the chip in standby — the
// caller re-enters RX.
void radio_hal_reset_agc() {
  if (!s_radio) return;
  SPI_LOCK();
  s_radio->sleep(true);
  s_radio->standby(RADIOLIB_SX126X_STANDBY_RC, true);
  uint8_t calData = RADIOLIB_SX126X_CALIBRATE_ALL;
  s_radio->mod->SPIwriteStream(RADIOLIB_SX126X_CMD_CALIBRATE, &calData, 1, true, false);
  s_radio->mod->hal->delay(5);
  uint32_t start = millis();
  while (s_radio->mod->hal->digitalRead(s_radio->mod->getGpio())) {
    if (millis() - start > 50) break;
    s_radio->mod->hal->yield();
  }
  if (s_last_freq_mhz > 0.0f) s_radio->calibrateImage(s_last_freq_mhz);
#if defined(BOARD_HELTEC_V4)
  s_radio->setDio2AsRfSwitch(true);
#endif
  if (s_rx_boost) s_radio->setRxBoostedGainMode(true, true);
  SPI_UNLOCK();
  s_in_recv = false;
}

// ── Boot protocol selector ───────────────────────────────────────────────────

static char s_proto_requested[16] = "meshcore";
static char s_proto_active[16]    = "meshcore";

// Lowercase [a-z0-9_], 1..15 chars; anything else -> "meshcore".
static void sanitize_proto_id(char* out, size_t out_sz, const char* id) {
  size_t n = 0;
  if (id) {
    for (const char* c = id; *c && n < out_sz - 1; c++) {
      char ch = (char)tolower((unsigned char)*c);
      if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
        out[n++] = ch;
      } else {
        n = 0;
        break;
      }
    }
  }
  out[n] = '\0';
  if (n == 0) strncpy(out, "meshcore", out_sz - 1);
}

void lora_proto_set_requested(const char* id) {
  sanitize_proto_id(s_proto_requested, sizeof(s_proto_requested), id);
}

const char* lora_proto_requested() { return s_proto_requested; }
const char* lora_proto_active()    { return s_proto_active; }

void lora_proto_mark_active(const char* id) {
  sanitize_proto_id(s_proto_active, sizeof(s_proto_active), id);
}
