#ifndef PUNK_RADIO_WRAPPER_H
#define PUNK_RADIO_WRAPPER_H

#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include "meshpunk_sync.h"

// SPI-safe wrapper. The T-Deck shares one SPI bus between the SX1262 radio,
// ILI9341 display, and SD card. Every radio SPI access must be serialized
// against display/SD. By overriding here instead of patching MeshCore, we
// keep the submodule clean for painless updates.
class PunkSX1262Wrapper : public CustomSX1262Wrapper {
  CustomSX1262* _sx;                      // concrete radio, for raw IRQ reads
  volatile bool _ls_wake_suspect = false; // set after a light-sleep DIO1 wake
public:
  PunkSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board)
    : CustomSX1262Wrapper(radio, board), _sx(&radio) {}

  void begin() override {
    SPI_LOCK(); CustomSX1262Wrapper::begin(); SPI_UNLOCK();
  }
  void powerOff() override {
    SPI_LOCK(); CustomSX1262Wrapper::powerOff(); SPI_UNLOCK();
  }

  // Standby: a DIO1 rising edge during light sleep wakes the chip, but the
  // edge itself is lost (the GPIO interrupt logic is clock-gated during the
  // sleep), so the base wrapper's ISR flag never gets set and the received
  // packet would sit unread with DIO1 latched high forever. The standby
  // loop calls this after every DIO1 wake; the dispatcher's next recvRaw()
  // then falls back to the SX1262's own IRQ register.
  void noteLightSleepWake() { _ls_wake_suspect = true; }

  int recvRaw(uint8_t* bytes, int sz) override {
    SPI_LOCK();
    int r = CustomSX1262Wrapper::recvRaw(bytes, sz);
    if (r <= 0 && _ls_wake_suspect) {
      _ls_wake_suspect = false;
      if (_sx->getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) {
        int len = _sx->getPacketLength();
        if (len > 0) {
          if (len > sz) len = sz;
          if (_sx->readData(bytes, (size_t)len) == RADIOLIB_ERR_NONE) {
            n_recv++;
            r = len;
          } else {
            n_recv_errors++;
          }
        }
        // readData left the chip in standby, and the base call's re-arm
        // branch didn't run (no ISR flag) — re-enter RX here. The base's
        // state variable still says RX, which is true again after this.
        _sx->startReceive();
      }
    }
    SPI_UNLOCK(); return r;
  }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    SPI_LOCK(); bool r = CustomSX1262Wrapper::startSendRaw(bytes, len); SPI_UNLOCK(); return r;
  }
  void onSendFinished() override {
    SPI_LOCK(); CustomSX1262Wrapper::onSendFinished(); SPI_UNLOCK();
  }
  void loop() override {
    SPI_LOCK(); CustomSX1262Wrapper::loop(); SPI_UNLOCK();
  }
  bool isReceivingPacket() override {
    SPI_LOCK(); bool r = CustomSX1262Wrapper::isReceivingPacket(); SPI_UNLOCK(); return r;
  }
  float getCurrentRSSI() override {
    SPI_LOCK(); float r = CustomSX1262Wrapper::getCurrentRSSI(); SPI_UNLOCK(); return r;
  }
  float getLastRSSI() const override {
    SPI_LOCK(); float r = CustomSX1262Wrapper::getLastRSSI(); SPI_UNLOCK(); return r;
  }
  float getLastSNR() const override {
    SPI_LOCK(); float r = CustomSX1262Wrapper::getLastSNR(); SPI_UNLOCK(); return r;
  }
  void setRxBoostedGainMode(bool en) override {
    SPI_LOCK(); CustomSX1262Wrapper::setRxBoostedGainMode(en); SPI_UNLOCK();
  }

  // Live param changes: RadioLib set* calls leave the chip in standby while
  // the wrapper still thinks STATE_RX, so nothing would re-arm the receiver.
  // This forces the wrapper to STATE_IDLE (protected idle()); the dispatcher's
  // next recvRaw() then re-arms RX with whatever params were set in between —
  // the same recovery path used after every TX.
  void standbyForConfig() {
    SPI_LOCK(); idle(); SPI_UNLOCK();
  }
};

#endif
