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
// 12, not 8. Music is a pre-rendered CLIP now (see music/jet_music.h), and a
// clip voice costs an index and an add rather than synthesis -- so the pool
// grew to keep the engine's four held layers, the music, and a busy moment's
// worth of item cues from stealing each other. Silent voices cost nothing:
// the mixer has a fast path for them.
#define JET_AUDIO_VOICES 12

// Waveforms. 0-2 keep their original ids; 3-6 are the richer shapes lifted
// from zepto-8's synth (see the attribution in jet_audio.cpp). ORGAN and
// PULSE are the useful ones for anything that should not sound like a beep.
enum { JET_WAVE_SQUARE = 0, JET_WAVE_NOISE = 1, JET_WAVE_TRIANGLE = 2,
       JET_WAVE_SAW = 3, JET_WAVE_TILTSAW = 4, JET_WAVE_PULSE = 5,
       JET_WAVE_ORGAN = 6, JET_WAVE_SINE = 7 };

// A shaped one-shot. The reason this exists: a flat tone that starts and
// stops at full amplitude IS the beep sound. An attack ramp, a decay to
// silence and an optional pitch sweep are what turn the same oscillator into
// a blip, a thud, a whoosh or a laser.
typedef struct {
    float freq;        // starting frequency
    float freq_to;     // ending frequency; <= 0 means "no sweep"
    int   ms;          // duration (ignored when hold is set)
    float volume;      // peak level
    int   wave;        // JET_WAVE_*
    int   attack_ms;   // ramp up; < 0 uses a 3ms default
    int   decay_ms;    // ramp down at the END; < 0 = the whole tail
    int   buzz;        // zepto's harsher variant of the waveform
    int   hold;        // sustain until jet_audio_stop (attack still applies)
    // Vibrato: a per-SAMPLE pitch wobble. This cannot be done from game
    // code -- a game updating pitch once per frame can only modulate at a
    // few Hz before it aliases, and mechanical roughness lives at 15-40Hz.
    // It is the difference between a tone and something running.
    float vib_hz;      // LFO RATE, shared by vib_depth and trem_depth.
                       // 0 = no modulation of either kind.
    float vib_depth;   // fraction of pitch, e.g. 0.03 = +-3%
    int   vib_steps;   // quantise the LFO to N steps; 0 = smooth. Stepped
                       // modulation is what reads as hardware-era rather
                       // than as a vibrato pedal.
    // Tremolo, off the SAME LFO: level instead of pitch. It only ever ducks
    // BELOW the voice's peak (1.0 = pulses to silence, 0.5 = pulses down to
    // half), so adding it can never cost headroom. A pitch wobble and a
    // level throb are different things -- a throb is what "pulsating" means
    // -- and running both off one oscillator keeps them locked together.
    float trem_depth;  // 0 = off
    // NOISE only: how steep the lowpass is, 1 to 4, at 6dB/octave each.
    // One pole barely filters -- a "500Hz" noise voice still carries plenty
    // of 5kHz, and stacked layers of that are television static no matter
    // how low the cutoff goes. Steepness is the variable that matters, not
    // cutoff. Pair a steep setting with a HIGHER cutoff: 4 poles at 1kHz is
    // a defined band of rumble, where 1 pole at 400Hz is still a hiss.
    // Levels are matched internally, so changing this does not change how
    // loud the voice is. 0 or 1 = the original single pole.
    int   poles;
} jet_fx_t;

int  jet_audio_fx(const jet_fx_t* fx);

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

// Sustained procedural tone: runs until jet_audio_stop. Steer it live with
// jet_audio_voice_set — that is what a continuous engine note needs, since
// retriggering a fixed-length tone per frame resets the oscillator phase and
// clicks at the frame rate. Held voices are also the LAST to be stolen when
// every voice is busy (see alloc_voice).
int  jet_audio_tone_hold(float freq, float volume, int wave);

// Retune / re-level a live voice without restarting it (phase is preserved,
// so a sweep is continuous). Stale or finished ids are ignored. freq <= 0
// changes only the level; volume < 0 changes only the pitch. On a clip voice
// freq acts as the playback-rate multiplier.
void jet_audio_voice_set(int voice, float freq, float volume);

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
