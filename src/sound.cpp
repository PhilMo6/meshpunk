#include "sound.h"
#include "Audio.h"
#include "usb_manager.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>
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

struct LuaFileHandle {   // must match the definition in main.cpp
    fs::File* file;
    bool is_sd;
    bool is_flash;
    bool is_write;
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
// Id of the AUDIO_FILE object currently connected to the mixer, or -1. Lets
// sound_obj_remove/sound_sweep stopSong() before closing a File the streamer
// is still reading (deleting a playing file object was a latent use-after-free).
static int     s_active_file_id  = -1;

// Set true by the ESP32-audioI2S end-of-file callback (audio_eof_mp3, defined
// below) and cleared when a new file starts or when Lua polls _sound_file_ended.
// Lets the Lua MP3 player auto-advance reliably. Safe because notifications use
// TONE melodies (notify.cpp), so this callback only fires for app file playback.
static volatile bool s_file_eof = false;

// ── Staged file-PCM playout (decode/playout split) ────────────────────────────
// audio_process_extern (fired inside audio->loop() by the decode pump below)
// copies each decoded 16-bit chunk here and returns continueI2S=false, so the
// library never blocks on I2S itself. The pump then plays the chunk AFTER
// releasing s_audio_mutex and the SPI bus lock. Holding those across the I2S
// DMA pacing wait (~26ms per MP3 frame, ~100% duty cycle with only a ~1µs
// release window) starved every other waiter — FreeRTOS mutexes don't hand
// off, so LVGL's flush (SPI, Core 0) and the pos/dur polls (s_audio_mutex)
// lost the re-acquire race indefinitely: frozen UI while the MP3 kept playing.
// With the split, the locks cover only the SD read + decode (a few ms) and the
// pacing wait runs lock-free.
//
// History: briefly reverted 2026-07-07 chasing the played-without-USB-first
// wedge, then RESTORED the same day — the fallback build reproduced that wedge
// too, exonerating the split (and it had already hw-validated as the fix for
// the play-over-USB UI freeze). The sequence wedge is a separate bug.
static int16_t* s_stage        = nullptr;   // PSRAM; sized to the lib's m_outBuff
static int      s_stage_frames = 0;         // frames staged by the hook this pass
static int      s_stage_ch     = 2;
static int      s_stage_rate   = 44100;
#define STAGE_MAX_FRAMES 2048               // == lib m_outBuff (int16_t[2048*2])

// Lock-free playback introspection. The decode pump refreshes these once per
// loop() pass while it already holds s_audio_mutex; the Lua bindings read them
// with no lock at all (aligned 32-bit reads can't tear). Cleared on play/stop/
// disconnect so a dead track never reports stale times.
static volatile uint32_t s_snap_pos = 0, s_snap_dur = 0;
static volatile uint32_t s_snap_br  = 0, s_snap_sr  = 0, s_snap_ch = 0;

// Speaker-gain replica of ESP32-audioI2S playSample(): input halved ("half
// Vin"), then scaled by volumetable[vol] >> 6. Copying the exact curve keeps
// the staged path loudness-identical to the lib's own playout. (The lib's IIR
// tone-control filters are skipped: nothing here ever sets them, so they're
// flat pass-throughs.)
static const uint8_t kVolTable[22] = { 0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 17,
                                       20, 23, 27, 30, 34, 38, 43, 48, 52, 58, 64 };

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
// Guards the ESP32-audioI2S decoder object (loop/stopSong/connectToFile/
// pauseResume) so the Core-1 decode pump (in sound_task) can't race Lua
// play/stop/pause on Core 0 or notify on Core 1. Lock order: s_sound_mutex
// OUTER, s_audio_mutex INNER. The one inversion — the pump holds s_audio_mutex
// while audio_process_extern try-takes s_sound_mutex — is deadlock-free
// because that inner take uses timeout 0 and never blocks.
static SemaphoreHandle_t s_audio_mutex = nullptr;
static volatile bool     s_sound_suspended = false;  // I2S halted for native module

extern void sd_spi_release();   // sd_spi_take() is inline in meshpunk_sync.h

// ── Playback diagnostics (permanent) ─────────────────────────────────────────
// Lock-free counters bumped on the hot paths; read by the _sound_debug Lua
// binding (the Tools/USB app shows a live line — the only window into the
// pump when USB host mode has serial disabled) and by the pump's stall
// detector, which prints the hidden decoder state to SLog when isRunning is
// true but nothing has moved for 2s. Earned their keep in the 2026-07-08
// every-3rd-track wedge hunt (see the MESHPUNK patch in vendored Audio.cpp:
// the lib's setDefaults() closed fd 0 every track via a core ssl bug, and SD
// songs inheriting fd 0 were closed out from under the decoder — the STALL
// line's fpos=-1 + libsize-intact signature identifies exactly that class).
static volatile uint32_t s_dbg_pass_decode = 0;  // decode-branch passes
static volatile uint32_t s_dbg_pass_mixer  = 0;  // mixer-branch passes
static volatile uint32_t s_dbg_staged      = 0;  // chunks staged by the hook
static volatile uint32_t s_dbg_played      = 0;  // chunks played by play_staged
static volatile uint32_t s_dbg_i2s_short   = 0;  // short/timed-out I2S writes
static volatile uint32_t s_dbg_eof         = 0;  // audio_eof_mp3 fires
static volatile uint32_t s_dbg_inbuff      = 0;  // lib InBuff fill (bytes)
static volatile uint32_t s_dbg_filepos     = 0;  // decoder file position
static volatile uint32_t s_dbg_fsize       = 0;  // lib's audiofile.size() (0 = invalid)

// ESP32-audioI2S info hook (weak in Audio.h). The library narrates its whole
// lifecycle through this — decoder allocs (with free-heap!), "stream ready",
// file close, sync/decode errors. Mirror it to serial: it is the primary
// evidence channel for the per-track wedge.
void audio_info(const char* s) { SLog.printf("[AUDIO] %s\n", s); }

// ESP32-audioI2S end-of-file hook (weak in Audio.h). Fired by the decode pump
// when the current file finishes; the Lua player polls _sound_file_ended().
void audio_eof_mp3(const char* name) {
    s_file_eof = true;
    s_dbg_eof++;
    SLog.printf("[SOUND] EOF %s\n", name ? name : "?");
}

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
    s_audio_mutex = xSemaphoreCreateMutex();
    // Staging buffer for the decode/playout split. If PSRAM ever fails here,
    // the hook leaves continueI2S=true and the lib plays I2S itself (the old
    // locks-held-while-pacing behavior) — degraded but functional.
    s_stage = (int16_t*)ps_malloc(STAGE_MAX_FRAMES * 2 * sizeof(int16_t));
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
    // Stop any file playback so the Core-1 decode pump won't touch I2S either.
    if (s_audio && s_audio->isRunning()) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio->stopSong();
        xSemaphoreGive(s_audio_mutex);
    }
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

// False when growing the registry failed — the caller still owns obj and must
// free it (the old array stays valid; nothing was registered).
static bool sound_obj_add(SoundObject* obj) {
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    if (sound_obj_count >= sound_obj_capacity) {
        int new_cap = sound_obj_capacity == 0 ? 8 : sound_obj_capacity * 2;
        SoundObject** grown = (SoundObject**)realloc(sound_objects, new_cap * sizeof(SoundObject*));
        if (!grown) {
            xSemaphoreGive(s_sound_mutex);
            return false;
        }
        sound_objects = grown;
        sound_obj_capacity = new_cap;
    }
    sound_objects[sound_obj_count++] = obj;
    xSemaphoreGive(s_sound_mutex);
    return true;
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
    // Removing the file the mixer is streaming from: disconnect BEFORE the
    // File is closed below, or the streamer keeps reading a dead handle.
    if (obj && obj->type == SoundObject::AUDIO_FILE && obj->id == s_active_file_id) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio->stopSong();
        s_snap_pos = 0; s_snap_dur = 0;
        s_snap_br  = 0; s_snap_sr  = 0; s_snap_ch = 0;
        xSemaphoreGive(s_audio_mutex);
        active_file_is_sd = false;
        s_active_file_id  = -1;
    }
    xSemaphoreGive(s_sound_mutex);
    if (!obj) return;
    if (obj->type == SoundObject::TONE && obj->pcm_buffer)
        free(obj->pcm_buffer);
    if (obj->type == SoundObject::AUDIO_FILE && obj->file) {
        // Closing an SD file is SPI traffic (directory-entry flush) — take the
        // bus lock or this Core-0 close races the radio on Core 1.
        if (obj->file_is_sd) sd_spi_take();
        obj->file->close();
        if (obj->file_is_sd) sd_spi_release();
        delete obj->file;
    }
    delete obj;
}

// ── Id-watermark sweep ────────────────────────────────────────────────────────
// Sound handles on the Lua side are plain integer wrappers with no __gc, so an
// app that exits without delete()ing leaks its PCM renders into this registry
// forever (100s of KB of PSRAM per abandoned session). Ids are monotonic:
// everything created at-or-after a mark belongs to that app session. The
// launcher marks at app launch and sweeps on app exit (lib/apps.lua);
// luaTearDown sweeps from the boot mark as the ELF-launch safety net. C-owned
// sounds (the notify melody) predate every mark and survive.

int sound_mark(void) {
    return next_sound_id;
}

// Sweep ids in [from_id, to_id); to_id == 0 means no upper bound. The bound
// matters for app→app launches: the NEW app's chunk runs before the old one is
// torn down, so its fresh sounds sit above the old app's range and must survive.
int sound_sweep(int from_id, int to_id) {
    int swept = 0;
    for (;;) {
        // Find one victim under the mutex, remove it outside the scan so the
        // swap-with-last removal can't skip entries mid-iteration.
        int victim = -1;
        xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
        for (int i = 0; i < sound_obj_count; i++) {
            int oid = sound_objects[i]->id;
            if (oid >= from_id && (to_id == 0 || oid < to_id)) { victim = oid; break; }
        }
        xSemaphoreGive(s_sound_mutex);
        if (victim < 0) break;
        sound_obj_remove(victim);   // handles mixer/file disconnect safety
        swept++;
    }
    if (swept > 0)
        SLog.printf("[SOUND] swept %d leaked object(s) (id %d..%d)\n", swept, from_id, to_id);
    return swept;
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
    if (!sound_obj_add(obj)) {   // registry realloc failed: don't leak the render
        free(obj->pcm_buffer);
        delete obj;
        return -1;
    }
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
    if (!sound_obj_add(obj)) {   // registry realloc failed: don't leak the render
        free(obj->pcm_buffer);
        delete obj;
        return -1;
    }
    return obj->id;
}

// ── Melody generation ─────────────────────────────────────────────────────────

// C-array renderer — the synth core shared by the Lua binding below and C
// callers (the boot-time notification melody in notify.cpp, which must not
// depend on a lua_State). Renders all notes into one PSRAM PCM buffer.
int sound_create_melody_notes(const MelodyNote* notes, int count, const ToneParams& base) {
    if (!notes || count <= 0 || count > 256) return -1;

    // First pass: compute total frames
    const uint32_t SR = TONE_SR;
    uint32_t total_frames = 0;
    for (int n = 0; n < count; n++) {
        int ms = notes[n].ms;
        if (ms < 1) ms = 1;
        if (ms > 10000) ms = 10000;
        total_frames += (SR * ms) / 1000;
    }
    if (total_frames == 0) return -1;

    int16_t* buf = (int16_t*)ps_malloc(total_frames * 2 * sizeof(int16_t));
    if (!buf) return -1;

    // Second pass: render each note
    uint32_t write_pos = 0;
    for (int n = 0; n < count; n++) {
        int freq = notes[n].freq_hz;
        int ms   = notes[n].ms;

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
    if (!sound_obj_add(obj)) {   // registry realloc failed: don't leak the render
        free(obj->pcm_buffer);
        delete obj;
        return -1;
    }
    return obj->id;
}

// Lua-facing wrapper: parses the note tables + shared opts into plain arrays,
// then delegates to the C renderer above.
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

    // Copy the note tables into a plain array for the renderer.
    MelodyNote notes[256];
    for (int n = 1; n <= note_count; n++) {
        lua_rawgeti(L, 1, n);

        lua_getfield(L, -1, "freq");
        int freq = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "ms");
        int ms = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 200;
        lua_pop(L, 2);   // ms value + note table

        if (freq < 0) freq = 0;
        if (freq > 20000) freq = 20000;
        if (ms < 0) ms = 0;
        if (ms > 10000) ms = 10000;
        notes[n - 1].freq_hz = (uint16_t)freq;
        notes[n - 1].ms      = (uint16_t)ms;
    }

    return sound_create_melody_notes(notes, note_count, base);
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
    if (!sound_obj_add(obj)) {   // registry realloc failed: we own the File now
        obj->file->close();
        delete obj->file;
        delete obj;
        lua_pushinteger(L, -1);
        return 1;
    }
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
        // Decoder state changes — serialize against the Core-1 decode pump.
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio->stopSong();
        active_file_is_sd = false;
        s_file_eof = false;          // fresh track: drop any stale end-of-file flag
        // The seek and connect touch the file (FAT walk; header probe/close on
        // a failed decoder init) — SD SPI traffic that must hold the bus lock
        // or it races the radio's transactions on Core 1. Lock order matches
        // the decode pump: s_audio_mutex OUTER, SPI INNER. (stopSong above
        // never touches external Files — the lib skips the close for them.)
        if (obj->file_is_sd) sd_spi_take();
        obj->file->seek(0);
        obj->file_paused = false;
        bool conn_ok = s_audio->connectToFile(*obj->file);
        if (obj->file_is_sd) sd_spi_release();
        active_file_is_sd = obj->file_is_sd;
        s_active_file_id  = obj->id;
        s_snap_pos = 0; s_snap_dur = 0;
        s_snap_br  = 0; s_snap_sr  = 0; s_snap_ch = 0;
        bool running_now = s_audio->isRunning();
        xSemaphoreGive(s_audio_mutex);
        // Permanent diagnostic: a silent connect failure looks exactly like a
        // frozen player (Lua's play() has no return path for it), so log it.
        SLog.printf("[SOUND] play id=%d objs=%d connect=%s running=%d heap=%u largest=%u\n",
                    id, sound_obj_count, conn_ok ? "OK" : "FAILED", (int)running_now,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio->stopSong();
        s_snap_pos = 0; s_snap_dur = 0;
        s_snap_br  = 0; s_snap_sr  = 0; s_snap_ch = 0;
        xSemaphoreGive(s_audio_mutex);
        active_file_is_sd = false;
        obj->file_paused  = false;
        s_active_file_id  = -1;
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
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_audio->pauseResume();
        xSemaphoreGive(s_audio_mutex);
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

// Play one staged file chunk: mix active notification tones, mirror to USB,
// apply the speaker volume, expand mono, and do the blocking DMA-paced I2S
// write. Runs on the decode pump with NO locks held except a short
// s_sound_mutex window for the tone registry — the pacing wait (the bulk of
// each cycle) leaves s_audio_mutex and the SPI bus free for the UI/radio.
static void play_staged() {
    int frames = s_stage_frames;
    int ch     = s_stage_ch;
    int rate   = s_stage_rate > 0 ? s_stage_rate : 44100;
    s_stage_frames = 0;

    // ── Mix tones (notifications during file playback) ──────────────────
    // Tone PCM is 44.1k interleaved stereo; step it by 44100/file-rate per
    // output frame so pitch survives 48k/22k files. The sub-frame remainder
    // resets each chunk (≤1 frame drift per ~26ms chunk — inaudible; exact
    // at 44.1k). Replaces the old audio_process_extern mix, which treated the
    // frame count as an int16 count and only covered half of each chunk.
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    for (int i = 0; i < sound_obj_count; i++) {
        SoundObject* o = sound_objects[i];
        if (o->type != SoundObject::TONE || !o->tone_playing || o->tone_paused) continue;
        uint32_t step = ((uint32_t)44100 << 8) / (uint32_t)rate;   // .8 fixed point
        uint32_t acc  = 0;
        for (int f = 0; f < frames; f++) {
            if (o->play_pos >= o->sample_count) {
                if (o->tone_loop) { o->play_pos = 0; }
                else { o->tone_playing = false; o->play_pos = 0; break; }
            }
            int32_t tl = o->pcm_buffer[o->play_pos];
            int32_t tr = (o->play_pos + 1 < o->sample_count)
                       ? o->pcm_buffer[o->play_pos + 1] : tl;
            if (ch == 2) {
                int32_t l = (int32_t)s_stage[f * 2]     + tl;
                int32_t r = (int32_t)s_stage[f * 2 + 1] + tr;
                s_stage[f * 2]     = (int16_t)constrain(l, -32768, 32767);
                s_stage[f * 2 + 1] = (int16_t)constrain(r, -32768, 32767);
            } else {
                int32_t m = (int32_t)s_stage[f] + ((tl + tr) >> 1);
                s_stage[f] = (int16_t)constrain(m, -32768, 32767);
            }
            acc += step;
            o->play_pos += (int)(acc >> 8) * 2;   // whole tone FRAMES consumed
            acc &= 0xFF;
        }
    }
    xSemaphoreGive(s_sound_mutex);

    // ── USB mirror ───────────────────────────────────────────────────────
    // Pre-volume, native rate/channels — same feed the in-lib tap always gave
    // it (the USB sink applies the system volume at playout). Now runs with
    // the SPI/audio locks already released.
    bool silence_spk = usb_audio_push(s_stage, frames, rate, ch);

    // ── Speaker: lib-identical gain, mono→stereo, paced write ────────────
    // Written in small slices so the stack buffer stays tiny. The blocking
    // i2s_write here is the pacing point for the entire decode loop —
    // deliberately outside every lock. When USB owns the output the slices
    // are zeroed but still written: the DMA drain is what paces decoding.
    int16_t slice[256 * 2];
    uint8_t vol = sound_volume > 21 ? 21 : sound_volume;
    int32_t g   = sound_muted ? 0 : kVolTable[vol];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 256) n = 256;
        if (silence_spk || g == 0) {
            memset(slice, 0, (size_t)n * 2 * sizeof(int16_t));
        } else {
            for (int f = 0; f < n; f++) {
                int32_t l, r;
                if (ch == 2) { l = s_stage[(done + f) * 2]; r = s_stage[(done + f) * 2 + 1]; }
                else         { l = r = s_stage[done + f]; }
                // playSample() replica: half Vin, then volumetable gain >> 6.
                slice[f * 2]     = (int16_t)(((l >> 1) * g) >> 6);
                slice[f * 2 + 1] = (int16_t)(((r >> 1) * g) >> 6);
            }
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, slice, (size_t)n * 2 * sizeof(int16_t), &written,
                  pdMS_TO_TICKS(100));
        done += n;
        // Short write = I2S halted under us (native-module suspend or stop
        // race). Drop the remainder rather than spin.
        if (written < (size_t)n * 2 * sizeof(int16_t)) { s_dbg_i2s_short++; break; }
    }
    s_dbg_played++;
}

static void sound_task_body(void* param) {
    const int CHUNK = 256;

    for (;;) {
        // Parked while a native module owns the device (I2S is stopped).
        if (s_sound_suspended) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_audio->isRunning()) {
            // File decode runs HERE on Core 1 — moved off Core 0's loop() so a
            // heavy MP3/FLAC decode can't stutter LVGL/Lua. s_audio_mutex
            // serializes the decoder against Lua play/stop/pause (Core 0) and
            // notify (Core 1). The locks cover ONLY the SD read + decode:
            // audio_process_extern stages the decoded PCM and skips the lib's
            // own I2S write, so the DMA pacing wait happens in play_staged()
            // below with both locks released. (Holding them across the pacing
            // wait starved the Core-0 flush/poll waiters — the frozen-UI bug.)
            tone_sr_set = false;
            s_stage_frames = 0;
            s_dbg_pass_decode++;
            xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
            if (s_audio->isRunning()) {          // re-check: may have been stopped
                if (active_file_is_sd) { sd_spi_take(); s_audio->loop(); sd_spi_release(); }
                else                   { s_audio->loop(); }
                // Refresh the lock-free introspection snapshot while the
                // decoder is still ours (see the Lua bindings).
                s_snap_pos = s_audio->getAudioCurrentTime();
                s_snap_dur = s_audio->getAudioFileDuration();
                s_snap_br  = s_audio->getBitRate();
                s_snap_sr  = s_audio->getSampleRate();
                s_snap_ch  = s_audio->getChannels();
                s_dbg_inbuff  = s_audio->inBufferFilled();
                s_dbg_filepos = s_audio->getFilePos();
                s_dbg_fsize   = s_audio->getFileSize();
            }
            xSemaphoreGive(s_audio_mutex);
            if (s_stage_frames > 0) {
                play_staged();       // pacing happens here, no locks held
            } else {
                // No PCM this pass: prefill/header parse, the lib's MP3/FLAC
                // decode-spreading skip (only every Nth pass decodes), or a
                // non-16-bit file on the in-lib path. Sleep one tick — bounds
                // the spin AND guarantees a real window in which Core-0/mesh
                // waiters can take the locks this loop cycles.
                vTaskDelay(1);
            }
            // Stall detector (wedge hunt): isRunning is true but neither the
            // play position nor the staged-chunk count has moved for 2s —
            // print the state the wedge hides in, at most once per 5s.
            {
                static uint32_t last_pos = 0, last_staged = 0;
                static uint32_t quiet_since = 0, last_print = 0;
                uint32_t now = millis();
                if (quiet_since == 0 ||
                    s_snap_pos != last_pos || s_dbg_staged != last_staged) {
                    // (quiet_since==0: first-ever pass — arm the baseline so
                    // track 1 doesn't trip a false STALL at boot.)
                    last_pos = s_snap_pos; last_staged = s_dbg_staged;
                    quiet_since = now;
                } else if (now - quiet_since > 2000 && now - last_print > 5000) {
                    last_print = now;
                    SLog.printf("[SOUND] STALL pos=%u dur=%u sr=%u inbuff=%u fpos=%d "
                                "libsize=%u staged=%u played=%u short=%u dec=%u mix=%u\n",
                                (unsigned)s_snap_pos, (unsigned)s_snap_dur,
                                (unsigned)s_snap_sr, (unsigned)s_dbg_inbuff,
                                (int)s_dbg_filepos, (unsigned)s_dbg_fsize,
                                (unsigned)s_dbg_staged,
                                (unsigned)s_dbg_played, (unsigned)s_dbg_i2s_short,
                                (unsigned)s_dbg_pass_decode, (unsigned)s_dbg_pass_mixer);
                }
            }
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

        if (!any_tones && !has_extern && !usb_audio_active()) {
            // Don't reset tone_sr_set here — the ring buffer goes briefly empty
            // between module audio pushes, and reconfiguring I2S every wake cycle
            // causes audible DMA glitches.  Only the Audio-library path resets it.
            xSemaphoreGive(s_sound_mutex);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }
        // While USB is routing, don't idle-sleep: fall through and produce a
        // silent chunk each cycle so the USB ring stays primed. Otherwise the
        // ring drains between sounds and each new sound starts into an empty
        // ring — the click heard on the sound-settings test buttons.

        s_dbg_pass_mixer++;   // productive mixer pass (not the idle-sleep path)
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

        int16_t out[CHUNK * 2];
        // Full-scale clipped mix. USB gets PRE-volume audio (it applies the
        // system volume at its own sink, so the slider tracks both outputs);
        // the speaker copy is volume-scaled below. i2s_write still runs (its
        // blocking DMA paces this loop); the speaker buffer is zeroed only if
        // the speaker should stay silent while routed.
        for (int s = 0; s < CHUNK * 2; s++)
            out[s] = (int16_t)constrain(mix[s], -32768, 32767);
        bool silence_spk = usb_audio_push(out, CHUNK, TONE_SR, 2);
        if (silence_spk) {
            memset(out, 0, sizeof(out));
        } else {
            float vol_scale = sound_muted ? 0.0f : (float)sound_volume / 21.0f;
            for (int s = 0; s < CHUNK * 2; s++)
                out[s] = (int16_t)(out[s] * vol_scale);
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, out, sizeof(out), &written, pdMS_TO_TICKS(50));
    }
}

void sound_tone_tick() {}

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    // Fired from sendBytes() inside audio->loop() — i.e. on the Core-1 decode
    // pump, with s_audio_mutex (and the SPI lock for SD files) held. `len` is
    // FRAMES; buff is interleaved at the file's native channel count.
    //
    // Divert 16-bit playback into the staging buffer and skip the library's
    // playChunk(): its blocking I2S write must not run under those locks (see
    // the staging notes at the top). Tone mixing, the USB mirror, volume and
    // the paced I2S write all happen in play_staged() after the locks drop.
    // Non-16-bit files (8-bit WAV) stay on the in-lib path: their m_outBuff
    // packing is byte-oriented and they're rare — the pump's no-PCM tick delay
    // still gives lock waiters a window each pass. (No tones/USB mirror on
    // that path.)
    if (s_stage && len > 0 && s_audio->getBitsPerSample() == 16) {
        int ch = s_audio->getChannels();
        if (ch < 1) ch = 1;
        if (ch > 2) ch = 2;
        int frames = len;
        if (frames > STAGE_MAX_FRAMES) frames = STAGE_MAX_FRAMES;
        memcpy(s_stage, buff, (size_t)frames * ch * sizeof(int16_t));
        s_stage_frames = frames;
        s_stage_ch     = ch;
        s_stage_rate   = (int)s_audio->getSampleRate();
        s_dbg_staged++;
        *continueI2S = false;
        return;
    }
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

    // ── File playback introspection (for the MP3 player) ──────────────────────
    // Reads come from the volatile snapshot the decode pump refreshes each
    // pass — no mutex. The pump used to hold s_audio_mutex near-continuously,
    // so a blocking read here could stall the UI task for a decode cycle (or,
    // pre-split, forever). Times are in whole seconds.
    lua_register(L, "_sound_get_pos", [](lua_State* L) -> int {
        lua_pushinteger(L, (lua_Integer)s_snap_pos);
        return 1;
    });
    lua_register(L, "_sound_get_duration", [](lua_State* L) -> int {
        lua_pushinteger(L, (lua_Integer)s_snap_dur);
        return 1;
    });
    lua_register(L, "_sound_seek", [](lua_State* L) -> int {
        int sec = luaL_checkinteger(L, 1);
        if (sec < 0) sec = 0;
        // Mutates decoder state, so it takes s_audio_mutex like play/stop. No
        // SPI lock needed: setAudioPlayPosition only records m_resumeFilePos;
        // the actual file seek happens on the pump's next pass, under its SPI
        // bracket.
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        bool ok = s_audio->setAudioPlayPosition((uint16_t)sec);
        xSemaphoreGive(s_audio_mutex);
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    });
    // Returns true exactly once per finished file (self-clearing). Clear only
    // after observing true: an unconditional clear could wipe an EOF the
    // Core-1 pump sets between our read and the store, losing an auto-advance.
    lua_register(L, "_sound_file_ended", [](lua_State* L) -> int {
        bool ended = s_file_eof;
        if (ended) s_file_eof = false;
        lua_pushboolean(L, ended ? 1 : 0);
        return 1;
    });
    lua_register(L, "_sound_get_info", [](lua_State* L) -> int {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)s_snap_br); lua_setfield(L, -2, "bitrate");
        lua_pushinteger(L, (lua_Integer)s_snap_sr); lua_setfield(L, -2, "samplerate");
        lua_pushinteger(L, (lua_Integer)s_snap_ch); lua_setfield(L, -2, "channels");
        return 1;
    });
    // Wedge-hunt instrumentation (see the s_dbg_* block). Lock-free reads; the
    // Tools/USB app shows a compact line so the pump state is visible even
    // when USB host mode has serial disabled.
    lua_register(L, "_sound_debug", [](lua_State* L) -> int {
        lua_newtable(L);
        lua_pushboolean(L, s_audio->isRunning() ? 1 : 0); lua_setfield(L, -2, "running");
        lua_pushinteger(L, (lua_Integer)s_dbg_pass_decode); lua_setfield(L, -2, "dec");
        lua_pushinteger(L, (lua_Integer)s_dbg_pass_mixer);  lua_setfield(L, -2, "mix");
        lua_pushinteger(L, (lua_Integer)s_dbg_staged);      lua_setfield(L, -2, "staged");
        lua_pushinteger(L, (lua_Integer)s_dbg_played);      lua_setfield(L, -2, "played");
        lua_pushinteger(L, (lua_Integer)s_dbg_i2s_short);   lua_setfield(L, -2, "short");
        lua_pushinteger(L, (lua_Integer)s_dbg_eof);         lua_setfield(L, -2, "eof");
        lua_pushinteger(L, (lua_Integer)s_dbg_inbuff);      lua_setfield(L, -2, "inbuff");
        lua_pushinteger(L, (lua_Integer)s_dbg_filepos);     lua_setfield(L, -2, "fpos");
        lua_pushinteger(L, (lua_Integer)s_snap_pos);        lua_setfield(L, -2, "pos");
        lua_pushinteger(L, (lua_Integer)s_snap_dur);        lua_setfield(L, -2, "dur");
        lua_pushinteger(L, (lua_Integer)sound_obj_count);   lua_setfield(L, -2, "objs");
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
    // Id-watermark ownership for the launcher (lib/apps.lua): mark before an
    // app's chunk runs, sweep everything >= mark when the app exits — apps
    // can no longer leak PCM renders by skipping delete() on an exit path.
    lua_register(L, "_sound_mark", [](lua_State* L) -> int {
        lua_pushinteger(L, sound_mark());
        return 1;
    });
    lua_register(L, "_sound_sweep", [](lua_State* L) -> int {
        lua_pushinteger(L, sound_sweep((int)luaL_checkinteger(L, 1),
                                       (int)luaL_optinteger(L, 2, 0)));
        return 1;
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
