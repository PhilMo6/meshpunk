// FM music for jet3d.
//
// This is an OPL3 synthesiser -- the Yamaha chip an AdLib/Sound Blaster had
// -- emulated in software, driven by a standard MIDI file. It is a wholly
// separate voice pool from jet_audio's tone synth: music costs zero SFX
// voices, which is why the engine's four held layers are unaffected by it.
//
// The stack came from the doom module (see LICENSE-MUSIC.txt): DOSBox's
// dbopl, Chocolate Doom's OPL player and MIDI parser, and our own T-Deck
// backend. Everything under music/ except the compat/ headers and this file
// is carried upstream code.
//
// Output is 22050Hz mono, the same rate jet_audio mixes at, and
// jet_music_mix() ADDS into the same int32 accumulator -- no resampling and
// no second output path.

#ifndef JET_MUSIC_H
#define JET_MUSIC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the OPL chip and loads the instrument bank. Safe to call twice.
// Returns 0 if music is unavailable, in which case every other call here is
// a harmless no-op and the game runs silent rather than failing.
int  jet_music_init(void);

void jet_music_shutdown(void);

// Starts a standard MIDI file held in memory, SYNTHESISED LIVE. Used by the
// offline renderer; the game does not take this path, because FM synthesis
// costs far too much per frame on the device (see jet_music_play_file).
int  jet_music_play(const void *midi, size_t len, int looping);

// Starts a track by path. Prefers a pre-rendered PCM loop sitting beside the
// MIDI (same name, ".raw"), which costs an index and an add per sample
// instead of emulating a chip; falls back to the live synth if it is absent.
// Clips are loaded once and kept, so switching tracks does not reload.
int  jet_music_play_file(const char *midi_path, int looping);

void jet_music_stop(void);
void jet_music_pause(int paused);

// 0.0 .. 1.0. Applies immediately, and persists across songs.
void jet_music_volume(float vol);

int  jet_music_playing(void);

// Renders `n` samples and ADDS them into `mix`, dispatching the sequencer's
// timer callbacks at their exact sample positions on the way through. Called
// from jet_audio's render loop; does nothing when no song is playing.
void jet_music_mix(int32_t *mix, int n);

#ifdef __cplusplus
}
#endif

#endif
