// audio_wio_l2.cpp — Seeed Wio Tracker L2 audio backend
// (contract: audio_dev.h).
//
// ES8311 codec (I2C 0x18 on the shared bus, src/audio/es8311.cpp) fed by
// I2S MCLK 10 / BCK 11 / WS 12 / DOUT 16 (Seeed wiki; Meshtastic variant
// DAC_I2S_*), driving the 6 ohm speaker through an amplifier whose enable is
// expander P14 (AUDIO_PA). The mixer, volume/mute prefs and the notification
// melody live above this in sound.cpp / notify.cpp, exactly as on the
// T-Deck's I2S path: the codec runs at 0 dB and the player's software volume
// sets loudness.
//
// The amplifier is switched on here and follows the board's power states in
// power_wio_l2.cpp (off for standby and power-off).

#if defined(BOARD_WIO_L2)

#include <Arduino.h>
#include <Wire.h>

#include "audio_dev.h"
#include "Audio.h"
#include "es8311.h"
#include "../boards/wio_l2_board.h"
#include "../meshpunk_sync.h"   // SLog

#define WIO_I2S_MCLK      10
#define WIO_I2S_BCK       11
#define WIO_I2S_WS        12
#define WIO_I2S_DOUT      16
#define WIO_ES8311_ADDR   0x18   // Espressif ES8311_ADDRESS_0 (CE low)

Audio* audio_dev_init(void) {
  Audio* audio = new Audio();
  audio->setPinout(WIO_I2S_BCK, WIO_I2S_WS, WIO_I2S_DOUT, I2S_PIN_NO_CHANGE,
                   WIO_I2S_MCLK);
  es8311_init(Wire, WIO_ES8311_ADDR);   // logs its own FAIL line
  if (!wio_l2_exp_set(WIO_L2_EXP_AUDIO_PA, true))
    SLog.println("[AUDIO] FAIL: expander write for the speaker amp (P14)");
  return audio;
}

AudioDevKind audio_dev_kind(void) { return AUDIO_DEV_I2S; }

// Tone-translation drive: unused on the I2S path (sound.cpp mixes real PCM).
void audio_dev_buzzer_tone(uint32_t freq_hz) { (void)freq_hz; }
void audio_dev_buzzer_off(void) { }

// This board's player from audio_dev_init() already decodes; a second
// instance would fight it for the I2S port.
Audio* audio_dev_decoder_open(void) { return nullptr; }
void   audio_dev_decoder_close(Audio* a) { (void)a; }

#endif // BOARD_WIO_L2
