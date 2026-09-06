#pragma once

// Minimal CayenneLPP for the companion's self-telemetry frame — the real lib
// drags ArduinoJson and std::map into a -nostdlib build for what is, here,
// ONE encode call. Wire format matched to the lib exactly: voltage = type
// 116, 2 bytes big-endian, 0.01V units (value*100, truncating cast like the
// lib's). Extend per-type if the companion ever encodes more.

#include <stdint.h>

#define TELEM_LPP_VOLTAGE 116

class CayenneLPP {
    uint8_t _buf[64];
    uint8_t _len;
    uint8_t _cap;

public:
    explicit CayenneLPP(uint8_t cap)
        : _len(0), _cap(cap > sizeof(_buf) ? (uint8_t)sizeof(_buf) : cap) {}

    void reset() { _len = 0; }

    uint8_t addVoltage(uint8_t channel, float volts) {
        if ((uint8_t)(_len + 4) > _cap) return 0;
        uint16_t v = (uint16_t)(volts * 100.0f);
        _buf[_len++] = channel;
        _buf[_len++] = TELEM_LPP_VOLTAGE;
        _buf[_len++] = (uint8_t)(v >> 8);
        _buf[_len++] = (uint8_t)(v & 0xFF);
        return _len;
    }

    uint8_t  getSize()   { return _len; }
    uint8_t* getBuffer() { return _buf; }
};
