// audio_dev.h — per-device audio-output backend contract.
//
// Same selection model as input_dev.h / display_dev.h: one backend compiles
// per build via the board define. The backend owns the output hardware
// (codec pins, amp enable, whatever the board needs); the mixer above it
// (sound.cpp) and the notification melody (notify.cpp) are device-neutral.

#pragma once

#include <stdint.h>

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

// ── Decode-only player (boards with no I2S DAC) ────────────────────────────
// The ESP32-audioI2S Audio object IS the MP3/AAC/FLAC decoder, so a board
// that returned nullptr from audio_dev_init() cannot decode a file at all —
// even when a USB dongle is attached and could play the result. These open
// an instance used ONLY as a decoder: sound.cpp's audio_process_extern hook
// takes its PCM and routes it to the USB sink, and nothing is ever written
// to I2S. Created on demand and destroyed when playback ends, because it
// costs ~300KB PSRAM (decode ring) while it exists.
//
// open() returns nullptr on boards that already have a real player (they
// never need a second one) and on any board where the instance failed to
// come up. close() is safe with nullptr.
Audio* audio_dev_decoder_open(void);
void   audio_dev_decoder_close(Audio* a);

// Buzzer drive for the sound.cpp tone-translation path (tone-only boards
// render app tones AND melodies as square-wave notes, so they need no
// separate alert path — notify.cpp just plays its melody through the mixer
// like every other board): set the current frequency, or silence it. Called
// from the Core-1 sound task at the mixer cadence, only on frequency
// changes. No-ops on boards with a full PCM path.
void audio_dev_buzzer_tone(uint32_t freq_hz);
void audio_dev_buzzer_off(void);
