// Audio mixer for the jet3d module.
//
// PUSH model, not pull. host_audio_set_pull() makes the firmware sound task
// call into module code roughly 172 times a second; that cost is per-wake, not
// per-sample, and it measured at ~15fps on the SNES module. Pushing instead lets
// the sound task drain a firmware-side ring without ever entering this module,
// and lets the mixer synthesise a whole frame's worth in one batch on Core 0.
//
// Format: signed 16-bit mono at 22050 Hz. The firmware mixer upsamples by an
// integer factor (44100 / rate), so the rate must divide 44100 exactly; 22050
// halves the mixing work versus 44100 and the ×2 upsample costs us nothing.
// The firmware ring holds 4096 samples = ~186ms, comfortably more than one
// frame of headroom at any frame rate this engine reaches.
//
// Clips are raw headerless PCM — same convention as the raw RGB565 textures,
// so no decoder is linked in.

#ifndef JET_AUDIO_H
#define JET_AUDIO_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define JET_AUDIO_RATE   22050
#define JET_AUDIO_VOICES 8

enum { JET_WAVE_SQUARE = 0, JET_WAVE_NOISE = 1, JET_WAVE_TRIANGLE = 2 };

void jet_audio_init(void);

// Frees every loaded clip and silences all voices.
void jet_audio_shutdown(void);

// Load raw signed-16-bit mono PCM at JET_AUDIO_RATE. Returns a clip id, or -1.
int  jet_audio_load(const char* path);

// Start a clip. pitch 1.0 = native rate. Returns a voice id, or -1 if the clip
// is invalid. Voice ids carry a generation counter so stopping a finished voice
// cannot silence whatever reused its slot.
int  jet_audio_play(int clip, float volume, int loop, float pitch);

// Procedural tone, so a game gets sound without shipping assets.
int  jet_audio_tone(float freq, int ms, float volume, int wave);

void jet_audio_stop(int voice);
void jet_audio_stop_all(void);
void jet_audio_master(float volume);

// Synthesise and push whatever real time has elapsed since the last call.
// Called once per frame from the main loop. Does nothing when idle, so a game
// with no sound pays only the elapsed-time check.
void jet_audio_service(uint32_t nowUs);

// Voices currently producing samples.
int  jet_audio_active(void);

#if defined(__cplusplus)
}
#endif

#endif // JET_AUDIO_H
