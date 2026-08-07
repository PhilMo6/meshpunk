// audio_dev.h — per-device audio-output backend contract.
//
// Same selection model as input_dev.h / display_dev.h: one backend compiles
// per build via the board define. The backend owns the output hardware
// (codec pins, amp enable, whatever the board needs); the mixer above it
// (sound.cpp) and the notification melody (notify.cpp) are device-neutral.

#pragma once

// What kind of output the board has. Callers use this to gate features that
// only make sense on a full audio path (music playback, module audio) versus
// a tone-only buzzer or a silent board.
enum AudioDevKind {
    AUDIO_DEV_NONE = 0,   // no output hardware
    AUDIO_DEV_I2S,        // I2S DAC/amp — full PCM mixing (T-Deck)
    AUDIO_DEV_BUZZER,     // single-tone piezo — melodies only, no PCM
};

class Audio;

// Create and configure the output device. Returns the ESP32-audioI2S player
// instance for sound_init(), or nullptr on boards with no I2S path (a
// buzzer/none backend returns nullptr and drives its own hardware).
Audio* audio_dev_init(void);

AudioDevKind audio_dev_kind(void);
