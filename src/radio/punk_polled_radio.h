#ifndef PUNK_POLLED_RADIO_H
#define PUNK_POLLED_RADIO_H

#include <Mesh.h>
#include <RadioLib.h>
#include "radio_hal.h"

// mesh::Radio implemented over the polled radio HAL — the same raw ops
// protocol modules use, so the chip has exactly one access path whichever
// protocol is active. No ISR anywhere: the dispatcher's mesh-task loop reads the chip's
// IRQ register through radio_hal_poll_irq(). (The ISR era's DIO1 handler only
// ever set a flag that loop() polled; the chip owns all tight RF timing, so
// discovery latency is the tick either way.) A packet latched during light
// sleep is found by the next poll — the missed-edge fallback the old wrapper
// needed (noteLightSleepWake) has no polled equivalent to need.
//
// State model mirrors MeshCore's RadioLibWrapper: the dispatcher never calls
// recvRaw() while a send is outstanding (Dispatcher::loop returns early until
// isSendComplete()/timeout), so exactly one method polls the IRQ register in
// any state. TX_DONE stays latched until radio_hal_send_finished() clears it;
// RX_DONE until radio_hal_read_packet() does. SPI locking lives inside every
// HAL op.
class PunkPolledRadio : public mesh::Radio {
  static const uint16_t NUM_FLOOR_SAMPLES = 64;   // RadioLibWrapper's values
  static const int      FLOOR_SAMPLING_THRESHOLD = 14;

  mesh::MainBoard* _board;
  bool     _tx_wait = false;          // startSendRaw accepted, TX_DONE not yet seen
  uint32_t n_recv = 0, n_sent = 0, n_recv_errors = 0;
  int16_t  _noise_floor = 0;
  int16_t  _threshold = 0;            // 0 = interference check disabled
  uint16_t _num_floor_samples = 0;
  int32_t  _floor_sample_sum = 0;

  // Chip-level "mid-packet" state: preamble or header currently detected.
  bool isReceivingPacket() {
    uint32_t irq = radio_hal_poll_irq();
    return (irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) ||
           (irq & RADIOLIB_SX126X_IRQ_HEADER_VALID);
  }

public:
  explicit PunkPolledRadio(mesh::MainBoard& board) : _board(&board) {}

  void begin() override {
    _noise_floor = 0;
    _threshold = 0;
    _num_floor_samples = 0;
    _floor_sample_sum = 0;
    // RX is armed by the first recvRaw() (or main.cpp's explicit start).
  }

  int recvRaw(uint8_t* bytes, int sz) override {
    if (_tx_wait) return 0;
    int len = 0;
    if (radio_hal_poll_irq() & RADIOLIB_SX126X_IRQ_RX_DONE) {
      int r = radio_hal_read_packet(bytes, sz);   // re-arms RX either way
      if (r > 0) { n_recv++; len = r; }
      else       { n_recv_errors++; }
    } else if (!radio_hal_in_recv()) {
      radio_hal_start_receive();
    }
    return len;
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    _board->onBeforeTransmit();
    if (radio_hal_start_send(bytes, len)) {
      _tx_wait = true;
      return true;
    }
    _board->onAfterTransmit();
    radio_hal_start_receive();   // failed start: back to RX now
    return false;
  }

  bool isSendComplete() override {
    if (!_tx_wait) return false;
    if (radio_hal_poll_irq() & RADIOLIB_SX126X_IRQ_TX_DONE) {
      n_sent++;
      return true;
    }
    return false;
  }

  void onSendFinished() override {
    _tx_wait = false;
    radio_hal_send_finished();   // finishTransmit (clears IRQ) + re-enter RX
    _board->onAfterTransmit();
  }

  bool isInRecvMode() const override { return radio_hal_in_recv(); }

  bool isReceiving() override {
    if (isReceivingPacket()) return true;
    // Channel-activity check (CSMA): RSSI over the measured floor.
    return _threshold != 0 &&
           radio_hal_current_rssi() > _noise_floor + _threshold;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    return radio_hal_time_on_air_ms(len_bytes);
  }

  float packetScore(float snr, int packet_len) override {
    // Per-SF minimum SNR for successful reception (Semtech datasheets) —
    // RadioLibWrapper::packetScoreInt, fed from the HAL's programmed SF.
    static const float snr_threshold[] = { -7.5f, -10.0f, -12.5f,
                                           -15.0f, -17.5f, -20.0f };
    int sf = radio_hal_last_sf();
    if (sf < 7 || sf > 12) return 0.0f;
    if (snr < snr_threshold[sf - 7]) return 0.0f;
    float success = (snr - snr_threshold[sf - 7]) / 10.0f;
    float collision_penalty = 1.0f - (packet_len / 256.0f);
    float score = success * collision_penalty;
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    return score;
  }

  // Noise-floor sampling, RadioLibWrapper's algorithm: while armed in RX and
  // not mid-packet, average 64 RSSI samples taken below floor+14dB; clamp at
  // -120. Runs on the dispatcher's loop cadence.
  void loop() override {
    if (radio_hal_in_recv() && !_tx_wait &&
        _num_floor_samples < NUM_FLOOR_SAMPLES) {
      if (!isReceivingPacket()) {
        int rssi = (int)radio_hal_current_rssi();
        if (rssi < _noise_floor + FLOOR_SAMPLING_THRESHOLD) {
          _num_floor_samples++;
          _floor_sample_sum += rssi;
        }
      }
    } else if (_num_floor_samples >= NUM_FLOOR_SAMPLES &&
               _floor_sample_sum != 0) {
      _noise_floor = (int16_t)(_floor_sample_sum / NUM_FLOOR_SAMPLES);
      if (_noise_floor < -120) _noise_floor = -120;
      _floor_sample_sum = 0;
    }
  }

  int getNoiseFloor() const override { return _noise_floor; }

  void triggerNoiseFloorCalibrate(int threshold) override {
    _threshold = (int16_t)threshold;
    if (_num_floor_samples >= NUM_FLOOR_SAMPLES) {  // ignore while sampling
      _num_floor_samples = 0;
      _floor_sample_sum = 0;
    }
  }

  void resetAGC() override {
    // Never mid-packet or with an unserviced completion latched.
    uint32_t irq = radio_hal_poll_irq();
    if (irq & (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TX_DONE |
               RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED |
               RADIOLIB_SX126X_IRQ_HEADER_VALID)) return;
    radio_hal_reset_agc();   // leaves standby; next recvRaw() re-arms RX
    // Floor reconverges from scratch (a stuck low floor self-reinforces:
    // the sampling threshold rejects every normal sample).
    _noise_floor = 0;
    _num_floor_samples = 0;
    _floor_sample_sum = 0;
  }

  float getLastRSSI() const override { return radio_hal_last_rssi(); }
  float getLastSNR() const override { return radio_hal_last_snr(); }

  // ── Punk extras (main.cpp call sites, PunkSX1262Wrapper names kept) ────────
  void powerOff() { radio_hal_sleep(); }
  // Live param changes: force standby; the dispatcher's next recvRaw()
  // re-arms RX with whatever params were programmed in between.
  void standbyForConfig() { radio_hal_standby(); }
  void setRxBoostedGainMode(bool en) { radio_hal_set_rx_boost(en); }
  bool getRxBoostedGainMode() const { return radio_hal_rx_boost(); }
};

#endif
