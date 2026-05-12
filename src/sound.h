#pragma once

#include <Arduino.h>
#include <FS.h>

struct lua_State;
class Audio;

// ── Types ─────────────────────────────────────────────────────────────────────

struct SoundObject {
    int       id;
    enum Type { TONE, AUDIO_FILE } type;

    // TONE fields
    int16_t*  pcm_buffer;
    uint32_t  sample_count;
    uint32_t  play_pos;
    bool      tone_playing;
    bool      tone_paused;
    bool      tone_loop;

    // FILE fields
    fs::File* file;
    bool      file_is_sd;
    bool      file_paused;
};

enum ToneWaveform { WAVE_SINE = 0, WAVE_SQUARE, WAVE_SAW, WAVE_TRIANGLE, WAVE_NOISE };

struct ToneParams {
    uint16_t     freq_hz;
    uint16_t     duration_ms;
    ToneWaveform waveform      = WAVE_SINE;
    uint16_t     attack_ms     = 10;
    uint16_t     decay_ms      = 0;
    float        sustain_level = 1.0f;
    uint16_t     release_ms    = 10;
    uint16_t     end_freq_hz   = 0;
    bool         sweep_exp     = false;
    float        fm_ratio      = 0.0f;
    float        fm_index      = 0.0f;
};

// ── Public API ────────────────────────────────────────────────────────────────

void sound_init(Audio* audio_ptr, void (*prefs_save_fn)());

int  sound_create_tone(const ToneParams& p);
int  sound_create_tone(uint16_t freq_hz, uint16_t duration_ms);
int  sound_create_chord(const uint16_t* freqs, int freq_count, const ToneParams& base);
int  sound_create_melody(lua_State* L);
int  sound_load_file(lua_State* L);
void sound_play(int id);
void sound_stop(int id);
void sound_pause(int id);
void sound_delete(int id);
void sound_set_loop(int id, bool loop);

void sound_tone_tick();
void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S);

void sound_register_lua(lua_State* L);

// ── State accessors ───────────────────────────────────────────────────────────

uint8_t sound_get_volume();
void    sound_set_volume(uint8_t vol);
bool    sound_get_muted();
void    sound_set_muted(bool m);
bool    sound_is_playing();
bool    sound_file_is_sd();
