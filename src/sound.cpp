#include "sound.h"
#include "Audio.h"
#include <driver/i2s.h>
#include <esp_random.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "meshpunk_sync.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

struct LuaFileHandle {
    fs::File* file;
    bool is_sd;
};

// ── State ─────────────────────────────────────────────────────────────────────

static Audio*    s_audio          = nullptr;
static void    (*s_prefs_save)()  = nullptr;

static SoundObject** sound_objects      = nullptr;
static int           sound_obj_count    = 0;
static int           sound_obj_capacity = 0;
static int           next_sound_id      = 1;

static uint8_t sound_volume    = 10;
static bool    sound_muted     = false;
static bool    active_file_is_sd = false;

static const uint32_t TONE_SR = 44100;
static bool tone_sr_set = false;
// NOTE (2026-06-12): a "pull-native I2S" experiment lived here — it
// uninstalled/reinstalled the I2S driver at the module's rate with a
// shallow DMA queue (lower latency, no resampling). REMOVED at the base-
// architecture level: the boot-installed driver is shared with the
// ESP32-audioI2S lib (notifications/MP3) and the push path (Doom), and
// driver juggling left the whole boot's audio broken when the restore
// raced playback or its 32KB DMA realloc failed. Do not reintroduce
// driver reinstalls here; if pull latency matters again, solve it at the
// lib-config level and test notifications + MP3 + Doom + PICO-8 together.

static TaskHandle_t      s_sound_task  = nullptr;
static SemaphoreHandle_t s_sound_mutex = nullptr;
static volatile bool     s_sound_suspended = false;  // I2S halted for native module

// ── External audio ring buffer (mono → upsampled to 44100 Hz stereo) ──────────
#define EXTERN_RING_SIZE 4096
static int16_t s_extern_ring[EXTERN_RING_SIZE];
static volatile int s_extern_head = 0;
static volatile int s_extern_tail = 0;
static volatile int s_extern_upsample = 4;  // 44100 / input_rate (default 11025 Hz)
// Pull-model source — when set, the mixer fetches samples from the module
// instead of the ring. Written only under s_sound_mutex.
static void (*s_extern_pull)(int16_t* out, int count) = nullptr;

static void sound_task_body(void* param);

// ── Init ──────────────────────────────────────────────────────────────────────

void sound_init(Audio* audio_ptr, void (*prefs_save_fn)()) {
    s_audio      = audio_ptr;
    s_prefs_save = prefs_save_fn;
    s_sound_mutex = xSemaphoreCreateMutex();
    // 12KB stack: a pull-model ELF module's synth (sound_extern_set_pull)
    // runs its code on this task, on top of the mixer's ~4KB of locals.
    xTaskCreatePinnedToCore(
        sound_task_body, "sound_task",
        12 * 1024, nullptr, 3, &s_sound_task, 1
    );
}

// ── Suspend / resume (for native-module takeover) ───────────────────────────────

void sound_suspend() {
    if (s_sound_suspended) return;
    s_sound_suspended = true;
    // Stop any file playback so audio->loop() (Core 0) won't touch I2S either.
    if (s_audio && s_audio->isRunning()) s_audio->stopSong();
    // Let the sound task observe the flag and leave any in-flight i2s_write.
    vTaskDelay(pdMS_TO_TICKS(30));
    // Halt the I2S peripheral + DMA so its TX-EOF ISR stops firing while the
    // CPU that services audio is handed to the module.
    i2s_stop(I2S_NUM_0);
}

void sound_resume() {
    if (!s_sound_suspended) return;
    i2s_start(I2S_NUM_0);
    tone_sr_set = false;          // force sample-rate reprogram on next tone
    s_sound_suspended = false;
}

// ── External audio ring buffer ────────────────────────────────────────────────

void sound_extern_set_rate(int sample_rate) {
    if (sample_rate <= 0) sample_rate = 11025;
    int factor = 44100 / sample_rate;
    if (factor < 1) factor = 1;
    if (factor > 4) factor = 4;
    s_extern_upsample = factor;
}

void sound_extern_push(const int16_t* samples, int count) {
    bool was_empty = (s_extern_head == s_extern_tail);
    for (int i = 0; i < count; i++) {
        int next = (s_extern_head + 1) % EXTERN_RING_SIZE;
        if (next == s_extern_tail) break; // full — drop newest
        s_extern_ring[s_extern_head] = samples[i];
        s_extern_head = next;
    }
    // Wake the sound task immediately when new data arrives after the ring
    // was empty — otherwise it sleeps for up to 50ms, causing choppy audio.
    if (was_empty && s_sound_task)
        xTaskNotifyGive(s_sound_task);
}

bool sound_extern_active(void) {
    return s_extern_head != s_extern_tail;
}

void sound_extern_flush(void) {
    s_extern_tail = s_extern_head;
    s_extern_upsample = 4;  // reset to default (11025 Hz)
}

void sound_extern_set_pull(void (*cb)(int16_t* out, int count), int sample_rate) {
    // Taking the mutex guarantees the mixer is not inside the old callback
    // when we return — required before the module's code is unloaded.
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    s_extern_pull = cb;
    if (cb) {
        sound_extern_set_rate(sample_rate);
        s_extern_tail = s_extern_head;  // drop any queued push-model audio
    } else {
        s_extern_upsample = 4;
    }
    xSemaphoreGive(s_sound_mutex);
    if (cb && s_sound_task) xTaskNotifyGive(s_sound_task);
}

// ── Accessors ─────────────────────────────────────────────────────────────────

uint8_t sound_get_volume()          { return sound_volume; }
void    sound_set_volume(uint8_t v) { sound_volume = v; }
bool    sound_get_muted()           { return sound_muted; }
void    sound_set_muted(bool m)     { sound_muted = m; }

bool sound_file_is_sd()                 { return active_file_is_sd; }

bool sound_is_playing() {
    if (s_audio->isRunning()) return true;
    for (int i = 0; i < sound_obj_count; i++)
        if (sound_objects[i]->type == SoundObject::TONE && sound_objects[i]->tone_playing)
            return true;
    return false;
}

// ── Object registry ───────────────────────────────────────────────────────────

static void sound_obj_add(SoundObject* obj) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    if (sound_obj_count >= sound_obj_capacity) {
        int new_cap = sound_obj_capacity == 0 ? 8 : sound_obj_capacity * 2;
        sound_objects = (SoundObject**)realloc(sound_objects, new_cap * sizeof(SoundObject*));
        sound_obj_capacity = new_cap;
    }
    sound_objects[sound_obj_count++] = obj;
    xSemaphoreGive(s_sound_mutex);
}

static SoundObject* sound_obj_find(int id) {
    for (int i = 0; i < sound_obj_count; i++)
        if (sound_objects[i]->id == id) return sound_objects[i];
    return nullptr;
}

static void sound_obj_remove(int id) {
    SoundObject* obj = nullptr;
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    for (int i = 0; i < sound_obj_count; i++) {
        if (sound_objects[i]->id != id) continue;
        obj = sound_objects[i];
        obj->tone_playing = false;
        sound_objects[i] = sound_objects[--sound_obj_count];
        break;
    }
    xSemaphoreGive(s_sound_mutex);
    if (!obj) return;
    if (obj->type == SoundObject::TONE && obj->pcm_buffer)
        free(obj->pcm_buffer);
    if (obj->type == SoundObject::AUDIO_FILE && obj->file) {
        obj->file->close();
        delete obj->file;
    }
    delete obj;
}

// ── Tone generation ───────────────────────────────────────────────────────────

int sound_create_tone(const ToneParams& p) {
    const uint32_t SR = TONE_SR;
    uint32_t frames = (SR * p.duration_ms) / 1000;
    if (frames == 0) return -1;
    int16_t* buf = (int16_t*)ps_malloc(frames * 2 * sizeof(int16_t));
    if (!buf) return -1;

    // ADSR sample boundaries
    uint32_t a_samples = (SR * p.attack_ms)  / 1000;
    uint32_t d_samples = (SR * p.decay_ms)   / 1000;
    uint32_t r_samples = (SR * p.release_ms) / 1000;
    if (a_samples + d_samples + r_samples > frames) {
        float scale = (float)frames / (a_samples + d_samples + r_samples);
        a_samples = (uint32_t)(a_samples * scale);
        d_samples = (uint32_t)(d_samples * scale);
        r_samples = frames - a_samples - d_samples;
    }
    uint32_t s_samples = frames - a_samples - d_samples - r_samples;

    // Frequency sweep
    bool do_sweep = (p.end_freq_hz > 0 && p.end_freq_hz != p.freq_hz);
    float freq_start = (float)p.freq_hz;
    float freq_end   = do_sweep ? (float)p.end_freq_hz : freq_start;
    float log_ratio  = 0.0f;
    if (do_sweep && p.sweep_exp && freq_start > 0.0f && freq_end > 0.0f)
        log_ratio = logf(freq_end / freq_start);

    float phase = 0.0f;
    float mod_phase = 0.0f;
    const float two_pi = 2.0f * (float)M_PI;
    bool do_fm = (p.fm_ratio > 0.0f && p.fm_index > 0.0f);

    for (uint32_t i = 0; i < frames; i++) {
        // Instantaneous frequency
        float freq;
        if (!do_sweep) {
            freq = freq_start;
        } else {
            float t = (float)i / (float)frames;
            freq = p.sweep_exp ? freq_start * expf(log_ratio * t)
                               : freq_start + (freq_end - freq_start) * t;
        }

        phase += two_pi * freq / SR;
        if (phase >= two_pi) phase -= two_pi;

        // FM modulator
        float fm_offset = 0.0f;
        if (do_fm) {
            mod_phase += two_pi * (freq * p.fm_ratio) / SR;
            if (mod_phase >= two_pi) mod_phase -= two_pi;
            fm_offset = p.fm_index * sinf(mod_phase);
        }

        // ADSR envelope
        float env;
        if (i < a_samples) {
            env = (float)i / (float)a_samples;
        } else if (i < a_samples + d_samples) {
            float pos = (float)(i - a_samples) / (float)d_samples;
            env = 1.0f - (1.0f - p.sustain_level) * pos;
        } else if (i < a_samples + d_samples + s_samples) {
            env = p.sustain_level;
        } else {
            float pos = (float)(i - a_samples - d_samples - s_samples) / (float)r_samples;
            env = p.sustain_level * (1.0f - pos);
        }

        // Waveform (FM offsets the phase used for evaluation)
        float sample;
        float eval_phase = fmodf(phase + fm_offset, two_pi);
        if (eval_phase < 0.0f) eval_phase += two_pi;
        switch (p.waveform) {
            case WAVE_SQUARE:
                sample = (eval_phase < (float)M_PI) ? 1.0f : -1.0f;
                break;
            case WAVE_SAW:
                sample = 1.0f - 2.0f * (eval_phase / two_pi);
                break;
            case WAVE_TRIANGLE:
                sample = (eval_phase < (float)M_PI)
                    ? (-1.0f + 2.0f * eval_phase / (float)M_PI)
                    : (3.0f - 2.0f * eval_phase / (float)M_PI);
                break;
            case WAVE_NOISE:
                sample = ((float)(esp_random() & 0xFFFF) / 32768.0f) - 1.0f;
                break;
            default:
                sample = sinf(eval_phase);
                break;
        }

        int16_t s = (int16_t)(env * 16000.0f * sample);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }

    SoundObject* obj = new SoundObject{};
    obj->id           = next_sound_id++;
    obj->type         = SoundObject::TONE;
    obj->pcm_buffer   = buf;
    obj->sample_count = frames * 2;
    obj->play_pos     = 0;
    obj->tone_playing = false;
    obj->tone_paused  = false;
    obj->tone_loop    = false;
    sound_obj_add(obj);
    return obj->id;
}

int sound_create_tone(uint16_t freq_hz, uint16_t duration_ms) {
    ToneParams p{};
    p.freq_hz     = freq_hz;
    p.duration_ms = duration_ms;
    return sound_create_tone(p);
}

// ── Chord generation ──────────────────────────────────────────────────────────

int sound_create_chord(const uint16_t* freqs, int freq_count, const ToneParams& base) {
    if (freq_count <= 0 || freq_count > 16) return -1;
    const uint32_t SR = TONE_SR;
    uint32_t frames = (SR * base.duration_ms) / 1000;
    if (frames == 0) return -1;
    int16_t* buf = (int16_t*)ps_malloc(frames * 2 * sizeof(int16_t));
    if (!buf) return -1;

    float scale = 1.0f / sqrtf((float)freq_count);
    float* accum = (float*)ps_calloc(frames, sizeof(float));
    if (!accum) { free(buf); return -1; }

    for (int n = 0; n < freq_count; n++) {
        if (freqs[n] == 0) continue;
        ToneParams p = base;
        p.freq_hz = freqs[n];

        // ADSR boundaries
        uint32_t a_samples = (SR * p.attack_ms)  / 1000;
        uint32_t d_samples = (SR * p.decay_ms)   / 1000;
        uint32_t r_samples = (SR * p.release_ms) / 1000;
        if (a_samples + d_samples + r_samples > frames) {
            float s = (float)frames / (a_samples + d_samples + r_samples);
            a_samples = (uint32_t)(a_samples * s);
            d_samples = (uint32_t)(d_samples * s);
            r_samples = frames - a_samples - d_samples;
        }
        uint32_t s_samples = frames - a_samples - d_samples - r_samples;

        // Sweep
        bool do_sweep = (p.end_freq_hz > 0 && p.end_freq_hz != p.freq_hz);
        float freq_start = (float)p.freq_hz;
        float freq_end   = do_sweep ? (float)p.end_freq_hz : freq_start;
        float log_ratio  = 0.0f;
        if (do_sweep && p.sweep_exp && freq_start > 0.0f && freq_end > 0.0f)
            log_ratio = logf(freq_end / freq_start);

        float phase = 0.0f;
        float mod_phase = 0.0f;
        const float two_pi = 2.0f * (float)M_PI;
        bool do_fm = (p.fm_ratio > 0.0f && p.fm_index > 0.0f);

        for (uint32_t i = 0; i < frames; i++) {
            float freq;
            if (!do_sweep) {
                freq = freq_start;
            } else {
                float t = (float)i / (float)frames;
                freq = p.sweep_exp ? freq_start * expf(log_ratio * t)
                                   : freq_start + (freq_end - freq_start) * t;
            }

            phase += two_pi * freq / SR;
            if (phase >= two_pi) phase -= two_pi;

            float fm_offset = 0.0f;
            if (do_fm) {
                mod_phase += two_pi * (freq * p.fm_ratio) / SR;
                if (mod_phase >= two_pi) mod_phase -= two_pi;
                fm_offset = p.fm_index * sinf(mod_phase);
            }

            float env;
            if (i < a_samples) {
                env = (float)i / (float)a_samples;
            } else if (i < a_samples + d_samples) {
                float pos = (float)(i - a_samples) / (float)d_samples;
                env = 1.0f - (1.0f - p.sustain_level) * pos;
            } else if (i < a_samples + d_samples + s_samples) {
                env = p.sustain_level;
            } else {
                float pos = (float)(i - a_samples - d_samples - s_samples) / (float)r_samples;
                env = p.sustain_level * (1.0f - pos);
            }

            float sample;
            float eval_phase = fmodf(phase + fm_offset, two_pi);
            if (eval_phase < 0.0f) eval_phase += two_pi;
            switch (p.waveform) {
                case WAVE_SQUARE:   sample = (eval_phase < (float)M_PI) ? 1.0f : -1.0f; break;
                case WAVE_SAW:      sample = 1.0f - 2.0f * (eval_phase / two_pi); break;
                case WAVE_TRIANGLE:
                    sample = (eval_phase < (float)M_PI)
                        ? (-1.0f + 2.0f * eval_phase / (float)M_PI)
                        : (3.0f - 2.0f * eval_phase / (float)M_PI);
                    break;
                case WAVE_NOISE:    sample = ((float)(esp_random() & 0xFFFF) / 32768.0f) - 1.0f; break;
                default:            sample = sinf(eval_phase); break;
            }

            accum[i] += env * sample * scale;
        }
    }

    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = (int16_t)(accum[i] * 16000.0f);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    free(accum);

    SoundObject* obj = new SoundObject{};
    obj->id           = next_sound_id++;
    obj->type         = SoundObject::TONE;
    obj->pcm_buffer   = buf;
    obj->sample_count = frames * 2;
    obj->play_pos     = 0;
    obj->tone_playing = false;
    obj->tone_paused  = false;
    obj->tone_loop    = false;
    sound_obj_add(obj);
    return obj->id;
}

// ── Melody generation ─────────────────────────────────────────────────────────

int sound_create_melody(lua_State* L) {
    if (!lua_istable(L, 1)) return -1;
    int note_count = (int)lua_rawlen(L, 1);
    if (note_count <= 0 || note_count > 256) return -1;

    // Parse shared opts from arg 2
    ToneParams base{};
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "waveform");
        if (lua_isstring(L, -1)) {
            const char* w = lua_tostring(L, -1);
            if      (strcmp(w, "square")   == 0) base.waveform = WAVE_SQUARE;
            else if (strcmp(w, "saw")      == 0) base.waveform = WAVE_SAW;
            else if (strcmp(w, "triangle") == 0) base.waveform = WAVE_TRIANGLE;
            else if (strcmp(w, "noise")    == 0) base.waveform = WAVE_NOISE;
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "attack");
        if (lua_isnumber(L, -1)) base.attack_ms = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "decay");
        if (lua_isnumber(L, -1)) base.decay_ms = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "sustain");
        if (lua_isnumber(L, -1)) {
            base.sustain_level = (float)lua_tonumber(L, -1);
            if (base.sustain_level < 0.0f) base.sustain_level = 0.0f;
            if (base.sustain_level > 1.0f) base.sustain_level = 1.0f;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "release");
        if (lua_isnumber(L, -1)) base.release_ms = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "fm_ratio");
        if (lua_isnumber(L, -1)) {
            base.fm_ratio = (float)lua_tonumber(L, -1);
            if (base.fm_ratio < 0.0f) base.fm_ratio = 0.0f;
            if (base.fm_ratio > 32.0f) base.fm_ratio = 32.0f;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "fm_index");
        if (lua_isnumber(L, -1)) {
            base.fm_index = (float)lua_tonumber(L, -1);
            if (base.fm_index < 0.0f) base.fm_index = 0.0f;
            if (base.fm_index > 20.0f) base.fm_index = 20.0f;
        }
        lua_pop(L, 1);
    }

    // First pass: compute total frames
    const uint32_t SR = TONE_SR;
    uint32_t total_frames = 0;
    for (int n = 1; n <= note_count; n++) {
        lua_rawgeti(L, 1, n);
        lua_getfield(L, -1, "ms");
        int ms = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 200;
        lua_pop(L, 2);
        if (ms < 1) ms = 1;
        if (ms > 10000) ms = 10000;
        total_frames += (SR * ms) / 1000;
    }
    if (total_frames == 0) return -1;

    int16_t* buf = (int16_t*)ps_malloc(total_frames * 2 * sizeof(int16_t));
    if (!buf) return -1;

    // Second pass: render each note
    uint32_t write_pos = 0;
    for (int n = 1; n <= note_count; n++) {
        lua_rawgeti(L, 1, n);

        lua_getfield(L, -1, "freq");
        int freq = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "ms");
        int ms = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 200;
        lua_pop(L, 1);

        lua_pop(L, 1); // pop note table

        if (ms < 1) ms = 1;
        if (ms > 10000) ms = 10000;
        uint32_t note_frames = (SR * ms) / 1000;
        if (note_frames == 0) continue;

        if (freq <= 0) {
            // Rest: write silence
            for (uint32_t i = 0; i < note_frames && write_pos < total_frames; i++, write_pos++) {
                buf[write_pos * 2]     = 0;
                buf[write_pos * 2 + 1] = 0;
            }
            continue;
        }
        if (freq > 20000) freq = 20000;
        if (freq < 20) freq = 20;

        ToneParams p = base;
        p.freq_hz     = (uint16_t)freq;
        p.duration_ms = (uint16_t)ms;

        // ADSR
        uint32_t a_samples = (SR * p.attack_ms)  / 1000;
        uint32_t d_samples = (SR * p.decay_ms)   / 1000;
        uint32_t r_samples = (SR * p.release_ms) / 1000;
        if (a_samples + d_samples + r_samples > note_frames) {
            float sc = (float)note_frames / (a_samples + d_samples + r_samples);
            a_samples = (uint32_t)(a_samples * sc);
            d_samples = (uint32_t)(d_samples * sc);
            r_samples = note_frames - a_samples - d_samples;
        }
        uint32_t s_samples = note_frames - a_samples - d_samples - r_samples;

        // Sweep
        bool do_sweep = (p.end_freq_hz > 0 && p.end_freq_hz != p.freq_hz);
        float freq_start = (float)p.freq_hz;
        float freq_end   = do_sweep ? (float)p.end_freq_hz : freq_start;
        float log_ratio  = 0.0f;
        if (do_sweep && p.sweep_exp && freq_start > 0.0f && freq_end > 0.0f)
            log_ratio = logf(freq_end / freq_start);

        float phase = 0.0f;
        float mod_phase = 0.0f;
        const float two_pi = 2.0f * (float)M_PI;
        bool do_fm = (p.fm_ratio > 0.0f && p.fm_index > 0.0f);

        for (uint32_t i = 0; i < note_frames && write_pos < total_frames; i++, write_pos++) {
            float fr;
            if (!do_sweep) {
                fr = freq_start;
            } else {
                float t = (float)i / (float)note_frames;
                fr = p.sweep_exp ? freq_start * expf(log_ratio * t)
                                 : freq_start + (freq_end - freq_start) * t;
            }

            phase += two_pi * fr / SR;
            if (phase >= two_pi) phase -= two_pi;

            float fm_offset = 0.0f;
            if (do_fm) {
                mod_phase += two_pi * (fr * p.fm_ratio) / SR;
                if (mod_phase >= two_pi) mod_phase -= two_pi;
                fm_offset = p.fm_index * sinf(mod_phase);
            }

            float env;
            if (i < a_samples) {
                env = (float)i / (float)a_samples;
            } else if (i < a_samples + d_samples) {
                float pos = (float)(i - a_samples) / (float)d_samples;
                env = 1.0f - (1.0f - p.sustain_level) * pos;
            } else if (i < a_samples + d_samples + s_samples) {
                env = p.sustain_level;
            } else {
                float pos = (float)(i - a_samples - d_samples - s_samples) / (float)r_samples;
                env = p.sustain_level * (1.0f - pos);
            }

            float sample;
            float eval_phase = fmodf(phase + fm_offset, two_pi);
            if (eval_phase < 0.0f) eval_phase += two_pi;
            switch (p.waveform) {
                case WAVE_SQUARE:   sample = (eval_phase < (float)M_PI) ? 1.0f : -1.0f; break;
                case WAVE_SAW:      sample = 1.0f - 2.0f * (eval_phase / two_pi); break;
                case WAVE_TRIANGLE:
                    sample = (eval_phase < (float)M_PI)
                        ? (-1.0f + 2.0f * eval_phase / (float)M_PI)
                        : (3.0f - 2.0f * eval_phase / (float)M_PI);
                    break;
                case WAVE_NOISE:    sample = ((float)(esp_random() & 0xFFFF) / 32768.0f) - 1.0f; break;
                default:            sample = sinf(eval_phase); break;
            }

            int16_t s = (int16_t)(env * 16000.0f * sample);
            buf[write_pos * 2]     = s;
            buf[write_pos * 2 + 1] = s;
        }
    }

    SoundObject* obj = new SoundObject{};
    obj->id           = next_sound_id++;
    obj->type         = SoundObject::TONE;
    obj->pcm_buffer   = buf;
    obj->sample_count = total_frames * 2;
    obj->play_pos     = 0;
    obj->tone_playing = false;
    obj->tone_paused  = false;
    obj->tone_loop    = false;
    sound_obj_add(obj);
    return obj->id;
}

// ── File loading ──────────────────────────────────────────────────────────────

int sound_load_file(lua_State* L) {
    LuaFileHandle* fh = (LuaFileHandle*)luaL_checkudata(L, 1, "esp32_file");
    if (!fh || !fh->file) {
        lua_pushinteger(L, -1);
        return 1;
    }
    SoundObject* obj = new SoundObject{};
    obj->id          = next_sound_id++;
    obj->type        = SoundObject::AUDIO_FILE;
    obj->file        = fh->file;
    obj->file_is_sd  = fh->is_sd;
    obj->file_paused = false;
    fh->file = nullptr;
    sound_obj_add(obj);
    lua_pushinteger(L, obj->id);
    return 1;
}

// ── Playback control ──────────────────────────────────────────────────────────

void sound_play(int id) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    SoundObject* obj = sound_obj_find(id);
    if (!obj) { xSemaphoreGive(s_sound_mutex); return; }
    bool is_tone = (obj->type == SoundObject::TONE);
    if (is_tone) {
        obj->play_pos     = 0;
        obj->tone_playing = true;
        obj->tone_paused  = false;
    } else {
        s_audio->stopSong();
        active_file_is_sd = false;
        obj->file->seek(0);
        obj->file_paused = false;
        s_audio->connectToFile(*obj->file);
        active_file_is_sd = obj->file_is_sd;
    }
    xSemaphoreGive(s_sound_mutex);
    if (is_tone && s_sound_task) xTaskNotifyGive(s_sound_task);
}

void sound_stop(int id) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    SoundObject* obj = sound_obj_find(id);
    if (!obj) { xSemaphoreGive(s_sound_mutex); return; }
    if (obj->type == SoundObject::TONE) {
        obj->tone_playing = false;
        obj->play_pos     = 0;
    } else {
        s_audio->stopSong();
        active_file_is_sd = false;
        obj->file_paused  = false;
    }
    xSemaphoreGive(s_sound_mutex);
}

void sound_pause(int id) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    SoundObject* obj = sound_obj_find(id);
    if (!obj) { xSemaphoreGive(s_sound_mutex); return; }
    if (obj->type == SoundObject::TONE) {
        obj->tone_paused = !obj->tone_paused;
    } else {
        s_audio->pauseResume();
        obj->file_paused = !obj->file_paused;
    }
    xSemaphoreGive(s_sound_mutex);
}

void sound_delete(int id) {
    sound_obj_remove(id);
}

void sound_set_loop(int id, bool loop) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    SoundObject* obj = sound_obj_find(id);
    if (obj && obj->type == SoundObject::TONE)
        obj->tone_loop = loop;
    xSemaphoreGive(s_sound_mutex);
}

// ── Sound task (Core 1) ──────────────────────────────────────────────────────

static void sound_task_body(void* param) {
    const int CHUNK = 256;

    for (;;) {
        // Parked while a native module owns the device (I2S is stopped).
        if (s_sound_suspended) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_audio->isRunning()) {
            tone_sr_set = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        xSemaphoreTake(s_sound_mutex, portMAX_DELAY);

        bool any_tones = false;
        for (int i = 0; i < sound_obj_count; i++) {
            SoundObject* o = sound_objects[i];
            if (o->type == SoundObject::TONE && o->tone_playing && !o->tone_paused)
                any_tones = true;
        }

        bool has_extern = (s_extern_pull != nullptr) || sound_extern_active();

        if (!any_tones && !has_extern) {
            // Don't reset tone_sr_set here — the ring buffer goes briefly empty
            // between module audio pushes, and reconfiguring I2S every wake cycle
            // causes audible DMA glitches.  Only the Audio-library path resets it.
            xSemaphoreGive(s_sound_mutex);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }

        if (!tone_sr_set) {
            i2s_set_sample_rates(I2S_NUM_0, TONE_SR);
            tone_sr_set = true;
        }

        // ── Mix tones ───────────────────────────────────────────────
        int32_t mix[CHUNK * 2] = {};
        for (int i = 0; i < sound_obj_count; i++) {
            SoundObject* o = sound_objects[i];
            if (o->type != SoundObject::TONE || !o->tone_playing || o->tone_paused) continue;
            for (int s = 0; s < CHUNK * 2; s++) {
                if (o->play_pos >= o->sample_count) {
                    if (o->tone_loop) { o->play_pos = 0; }
                    else { o->tone_playing = false; o->play_pos = 0; break; }
                }
                mix[s] += o->pcm_buffer[o->play_pos++];
            }
        }

        // The in-mixer heap-hunt checkpoints (sound:tones/postpull/extern,
        // every 64th chunk) are GONE: their phase-discrimination question is
        // answered (module api_sfx OOB, fixed), and the 3x ~12ms walk burst
        // stalled this prio-3 task long enough to starve the prio-2 blit
        // task (a metronome-like frame hitch — every 743ms at 22050 native)
        // and to chew most of the shallow pull-native DMA queue's headroom
        // (audible click). The 250ms ambient walk at the top of the loop
        // and the meshloop walk remain the corruption soak detectors.

        // ── Mix external audio (ELF module) ─────────────────────────
        // Pull model: fetch exactly the samples needed straight from the
        // module's synth (running here, on Core 1). Push model: drain the
        // ring filled via host_audio_push. Either way, upsample to
        // 44100 Hz stereo with linear interpolation (1=44100, 2=22050,
        // 4=11025). Still under s_sound_mutex so sound_extern_set_pull()
        // can't unload the callback mid-call.
        {
            static int16_t s_prev_extern = 0;
            int upsample = s_extern_upsample;
            int needed = CHUNK / upsample;
            int n = 0;
            int16_t samp[256]; // CHUNK max (when upsample=1)
            // Stack canary after samp[] — a pull module writing more
            // samples than asked for corrupts this task's stack (volatile
            // so it stays placed after the buffer).
            volatile uint32_t samp_guard = 0xCAFEBABE;

            if (s_extern_pull) {
                s_extern_pull(samp, needed);
                n = needed;
                if (samp_guard != 0xCAFEBABE) {
                    static uint32_t s_guard_last_print = 0;
                    uint32_t now_g = millis();
                    if (now_g - s_guard_last_print >= 1000) {  // don't spam
                        s_guard_last_print = now_g;
                        SLog.printf("[sound] module pull OVERRAN samp[] "
                                      "(guard=%08x, needed=%d)\n",
                                      (unsigned)samp_guard, needed);
                    }
                    samp_guard = 0xCAFEBABE;
                }
            } else {
                int avail = (s_extern_head - s_extern_tail + EXTERN_RING_SIZE)
                            % EXTERN_RING_SIZE;
                n = (avail < needed) ? avail : needed;
                int tail = s_extern_tail;
                for (int i = 0; i < n; i++) {
                    samp[i] = s_extern_ring[tail];
                    tail = (tail + 1) % EXTERN_RING_SIZE;
                }
                s_extern_tail = tail;
            }

            for (int i = 0; i < n; i++) {
                int16_t prev = (i == 0) ? s_prev_extern : samp[i - 1];
                int16_t cur  = samp[i];
                for (int j = 0; j < upsample; j++) {
                    int32_t out = (int32_t)prev
                                + ((int32_t)(cur - prev) * j) / upsample;
                    int idx = (i * upsample + j) * 2;
                    if (idx + 1 < CHUNK * 2) {
                        mix[idx]     += (int16_t)out;
                        mix[idx + 1] += (int16_t)out;
                    }
                }
            }
            if (n > 0) s_prev_extern = samp[n - 1];
        }

        xSemaphoreGive(s_sound_mutex);

        float vol_scale = sound_muted ? 0.0f : (float)sound_volume / 21.0f;
        int16_t out[CHUNK * 2];
        for (int s = 0; s < CHUNK * 2; s++)
            out[s] = (int16_t)(constrain(mix[s], -32768, 32767) * vol_scale);
        size_t written = 0;
        i2s_write(I2S_NUM_0, out, sizeof(out), &written, pdMS_TO_TICKS(50));
    }
}

void sound_tone_tick() {}

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    if (xSemaphoreTake(s_sound_mutex, 0) != pdTRUE) {
        *continueI2S = true;
        return;
    }
    for (int i = 0; i < sound_obj_count; i++) {
        SoundObject* o = sound_objects[i];
        if (o->type != SoundObject::TONE || !o->tone_playing || o->tone_paused) continue;
        for (uint16_t s = 0; s < len; s++) {
            if (o->play_pos >= o->sample_count) {
                if (o->tone_loop) { o->play_pos = 0; }
                else { o->tone_playing = false; o->play_pos = 0; break; }
            }
            int32_t mixed = (int32_t)buff[s] + (int32_t)o->pcm_buffer[o->play_pos++];
            buff[s] = (int16_t)constrain(mixed, -32768, 32767);
        }
    }
    xSemaphoreGive(s_sound_mutex);
    *continueI2S = true;
}

// ── Lua bindings ──────────────────────────────────────────────────────────────

void sound_register_lua(lua_State* L) {
    // Volume & mute
    lua_register(L, "_sound_set_volume", [](lua_State* L) -> int {
        int v = luaL_checkinteger(L, 1);
        if (v < 0) v = 0; if (v > 21) v = 21;
        sound_volume = (uint8_t)v;
        if (!sound_muted) s_audio->setVolume(sound_volume);
        if (s_prefs_save) s_prefs_save();
        lua_pushinteger(L, sound_volume);
        return 1;
    });
    lua_register(L, "_sound_get_volume", [](lua_State* L) -> int {
        lua_pushinteger(L, sound_volume);
        return 1;
    });
    lua_register(L, "_sound_get_muted", [](lua_State* L) -> int {
        lua_pushboolean(L, sound_muted ? 1 : 0);
        return 1;
    });
    lua_register(L, "_sound_set_muted", [](lua_State* L) -> int {
        sound_muted = lua_toboolean(L, 1);
        s_audio->setVolume(sound_muted ? 0 : sound_volume);
        if (s_prefs_save) s_prefs_save();
        lua_pushboolean(L, sound_muted ? 1 : 0);
        return 1;
    });
    lua_register(L, "_sound_is_playing", [](lua_State* L) -> int {
        lua_pushboolean(L, sound_is_playing() ? 1 : 0);
        return 1;
    });

    // Tone generation
    lua_register(L, "_sound_generate_tone", [](lua_State* L) -> int {
        int freq = luaL_checkinteger(L, 1);
        int dur  = luaL_optinteger(L, 2, 200);
        if (freq < 20) freq = 20; if (freq > 20000) freq = 20000;
        if (dur  < 10) dur  = 10; if (dur  > 10000) dur  = 10000;

        ToneParams p{};
        p.freq_hz     = (uint16_t)freq;
        p.duration_ms = (uint16_t)dur;

        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "waveform");
            if (lua_isstring(L, -1)) {
                const char* w = lua_tostring(L, -1);
                if      (strcmp(w, "square")   == 0) p.waveform = WAVE_SQUARE;
                else if (strcmp(w, "saw")      == 0) p.waveform = WAVE_SAW;
                else if (strcmp(w, "triangle") == 0) p.waveform = WAVE_TRIANGLE;
                else if (strcmp(w, "noise")    == 0) p.waveform = WAVE_NOISE;
            }
            lua_pop(L, 1);

            lua_getfield(L, 3, "attack");
            if (lua_isnumber(L, -1)) p.attack_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 3, "decay");
            if (lua_isnumber(L, -1)) p.decay_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 3, "sustain");
            if (lua_isnumber(L, -1)) {
                p.sustain_level = (float)lua_tonumber(L, -1);
                if (p.sustain_level < 0.0f) p.sustain_level = 0.0f;
                if (p.sustain_level > 1.0f) p.sustain_level = 1.0f;
            }
            lua_pop(L, 1);

            lua_getfield(L, 3, "release");
            if (lua_isnumber(L, -1)) p.release_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 3, "end_freq");
            if (lua_isnumber(L, -1)) {
                int ef = (int)lua_tointeger(L, -1);
                if (ef < 20)    ef = 20;
                if (ef > 20000) ef = 20000;
                p.end_freq_hz = (uint16_t)ef;
            }
            lua_pop(L, 1);

            lua_getfield(L, 3, "sweep");
            if (lua_isstring(L, -1))
                p.sweep_exp = (strcmp(lua_tostring(L, -1), "exp") == 0);
            lua_pop(L, 1);

            lua_getfield(L, 3, "fm_ratio");
            if (lua_isnumber(L, -1)) {
                p.fm_ratio = (float)lua_tonumber(L, -1);
                if (p.fm_ratio < 0.0f) p.fm_ratio = 0.0f;
                if (p.fm_ratio > 32.0f) p.fm_ratio = 32.0f;
            }
            lua_pop(L, 1);

            lua_getfield(L, 3, "fm_index");
            if (lua_isnumber(L, -1)) {
                p.fm_index = (float)lua_tonumber(L, -1);
                if (p.fm_index < 0.0f) p.fm_index = 0.0f;
                if (p.fm_index > 20.0f) p.fm_index = 20.0f;
            }
            lua_pop(L, 1);
        }

        lua_pushinteger(L, sound_create_tone(p));
        return 1;
    });

    // Chord generation
    lua_register(L, "_sound_generate_chord", [](lua_State* L) -> int {
        if (!lua_istable(L, 1)) { lua_pushinteger(L, -1); return 1; }
        int count = (int)lua_rawlen(L, 1);
        if (count <= 0 || count > 16) { lua_pushinteger(L, -1); return 1; }

        int dur = luaL_optinteger(L, 2, 200);
        if (dur < 10) dur = 10; if (dur > 10000) dur = 10000;

        uint16_t freqs[16];
        for (int i = 0; i < count; i++) {
            lua_rawgeti(L, 1, i + 1);
            int f = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
            if (f < 0) f = 0;
            if (f > 20000) f = 20000;
            freqs[i] = (uint16_t)f;
            lua_pop(L, 1);
        }

        ToneParams p{};
        p.duration_ms = (uint16_t)dur;

        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "waveform");
            if (lua_isstring(L, -1)) {
                const char* w = lua_tostring(L, -1);
                if      (strcmp(w, "square")   == 0) p.waveform = WAVE_SQUARE;
                else if (strcmp(w, "saw")      == 0) p.waveform = WAVE_SAW;
                else if (strcmp(w, "triangle") == 0) p.waveform = WAVE_TRIANGLE;
                else if (strcmp(w, "noise")    == 0) p.waveform = WAVE_NOISE;
            }
            lua_pop(L, 1);

            lua_getfield(L, 3, "attack");
            if (lua_isnumber(L, -1)) p.attack_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "decay");
            if (lua_isnumber(L, -1)) p.decay_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "sustain");
            if (lua_isnumber(L, -1)) {
                p.sustain_level = (float)lua_tonumber(L, -1);
                if (p.sustain_level < 0.0f) p.sustain_level = 0.0f;
                if (p.sustain_level > 1.0f) p.sustain_level = 1.0f;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "release");
            if (lua_isnumber(L, -1)) p.release_ms = (uint16_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 3, "fm_ratio");
            if (lua_isnumber(L, -1)) {
                p.fm_ratio = (float)lua_tonumber(L, -1);
                if (p.fm_ratio < 0.0f) p.fm_ratio = 0.0f;
                if (p.fm_ratio > 32.0f) p.fm_ratio = 32.0f;
            }
            lua_pop(L, 1);
            lua_getfield(L, 3, "fm_index");
            if (lua_isnumber(L, -1)) {
                p.fm_index = (float)lua_tonumber(L, -1);
                if (p.fm_index < 0.0f) p.fm_index = 0.0f;
                if (p.fm_index > 20.0f) p.fm_index = 20.0f;
            }
            lua_pop(L, 1);
        }

        lua_pushinteger(L, sound_create_chord(freqs, count, p));
        return 1;
    });

    // Melody generation
    lua_register(L, "_sound_generate_melody", [](lua_State* L) -> int {
        int id = sound_create_melody(L);
        lua_pushinteger(L, id);
        return 1;
    });

    // File loading & playback control
    lua_register(L, "_sound_load_file", sound_load_file);
    lua_register(L, "_sound_play", [](lua_State* L) -> int {
        sound_play((int)luaL_checkinteger(L, 1));
        return 0;
    });
    lua_register(L, "_sound_stop", [](lua_State* L) -> int {
        sound_stop((int)luaL_checkinteger(L, 1));
        return 0;
    });
    lua_register(L, "_sound_pause", [](lua_State* L) -> int {
        sound_pause((int)luaL_checkinteger(L, 1));
        return 0;
    });
    lua_register(L, "_sound_delete", [](lua_State* L) -> int {
        sound_delete((int)luaL_checkinteger(L, 1));
        return 0;
    });
    lua_register(L, "_sound_set_loop", [](lua_State* L) -> int {
        sound_set_loop((int)luaL_checkinteger(L, 1), lua_toboolean(L, 2));
        return 0;
    });

    lua_register(L, "_sound_save_wav", [](lua_State* L) -> int {
        int id = (int)luaL_checkinteger(L, 1);
        LuaFileHandle* fh = (LuaFileHandle*)luaL_checkudata(L, 2, "esp32_file");
        if (!fh || !fh->file) {
            lua_pushnil(L); lua_pushstring(L, "invalid file"); return 2;
        }

        xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
        SoundObject* obj = sound_obj_find(id);
        if (!obj || obj->type != SoundObject::TONE || !obj->pcm_buffer) {
            xSemaphoreGive(s_sound_mutex);
            lua_pushnil(L); lua_pushstring(L, "invalid sound object"); return 2;
        }

        uint32_t data_size = obj->sample_count * sizeof(int16_t);

        uint8_t hdr[44] = {};
        memcpy(hdr,    "RIFF", 4);
        *(uint32_t*)(hdr+4)  = 36 + data_size;
        memcpy(hdr+8,  "WAVEfmt ", 8);
        *(uint32_t*)(hdr+16) = 16;
        *(uint16_t*)(hdr+20) = 1;
        *(uint16_t*)(hdr+22) = 2;
        *(uint32_t*)(hdr+24) = 44100;
        *(uint32_t*)(hdr+28) = 44100 * 2 * 2;
        *(uint16_t*)(hdr+32) = 4;
        *(uint16_t*)(hdr+34) = 16;
        memcpy(hdr+36, "data", 4);
        *(uint32_t*)(hdr+40) = data_size;

        if (fh->is_sd) SPI_LOCK();
        size_t w1 = fh->file->write(hdr, 44);
        size_t w2 = fh->file->write((const uint8_t*)obj->pcm_buffer, data_size);
        if (fh->is_sd) SPI_UNLOCK();

        xSemaphoreGive(s_sound_mutex);

        if (w1 != 44 || w2 != data_size) {
            lua_pushnil(L); lua_pushstring(L, "write failed"); return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    });
}
