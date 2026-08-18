// audio_tdeck.cpp — T-Deck audio-output backend (contract: audio_dev.h).
//
// I2S DAC/amp on the T-Deck speaker pins. The mixer, volume/mute prefs and
// the notification melody all live above this in sound.cpp / notify.cpp and
// are device-neutral; this file exists only to create the player and wire
// its pinout.

#if defined(BOARD_TDECK)

#include "audio_dev.h"
#include "Audio.h"
#include "../tdeck-pins.h"   // TDECK_I2S_BCK / WS / DOUT

Audio* audio_dev_init(void) {
  Audio* audio = new Audio();
  audio->setPinout(TDECK_I2S_BCK, TDECK_I2S_WS, TDECK_I2S_DOUT);
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

#endif // BOARD_TDECK
