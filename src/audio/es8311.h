// es8311.h — Everest ES8311 audio codec: I2C control for DAC playback.
//
// Configures the codec as an I2S slave taking MCLK on its MCLK pin at
// 256 x fs (the ESP32 I2S master default, mclk_multiple 0), 16-bit I2S
// format in and out. At 256 x fs every clock-divider coefficient is the
// same for all sample rates 8-64 kHz, so one init covers every rate the
// player switches to. Register sequence = Espressif's esp_codec_dev ES8311
// driver (esp-adf components/esp_codec_dev/device/es8311, Apache-2.0).
//
// Board-neutral: the board backend owns the I2C bus, the I2S pins and the
// speaker amplifier.

#pragma once

#include <stdint.h>

class TwoWire;

// Full codec setup (esp_codec_dev open, sample config, format, start), DAC
// digital volume 0 dB (the player's software volume sets loudness),
// unmuted. False (logged) on any I2C failure.
bool es8311_init(TwoWire& wire, uint8_t addr);
