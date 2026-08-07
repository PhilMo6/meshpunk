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

#endif // BOARD_TDECK
