#include "jet_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
    void host_audio_push(const int16_t* samples, int count, int sample_rate);
    void host_log(const char* msg);
}

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// Samples synthesised per inner pass. 256 keeps the int32 accumulator at 1KB so
// it stays cache-resident; a single frame-sized buffer would be ~8KB of PSRAM
// traffic per frame for no benefit. host_audio_push is a cheap ring copy, so
// several calls per frame cost nothing measurable.
#define CHUNK 256

// Ceiling on how much audio one service call may generate. Without it a long
// stall (SD read, radio burst) would ask for a huge catch-up batch, overflow the
// firmware ring and drop most of it anyway. Two frames at 20fps is ample.
#define MAX_CATCHUP (JET_AUDIO_RATE / 8)

#define MAX_CLIPS 32

typedef struct {
    int16_t* data;
    uint32_t frames;
} Clip;

typedef struct {
    uint8_t  active;
    uint8_t  wave;       // procedural voices only
    uint8_t  loop;
    uint8_t  gen;        // bumped per reuse; encoded in the voice id
    int      clip;       // -1 for a procedural tone
    uint32_t pos;        // 16.16 fixed-point index into the clip
    uint32_t step;       // 16.16 advance per output sample
    int32_t  vol;        // 0..256
    uint32_t remaining;  // samples left for a tone; 0 = until stopped
    uint32_t phase;      // 16.16 oscillator phase
    uint32_t phaseInc;
    uint32_t rng;
} Voice;

static Clip  s_clips[MAX_CLIPS];
static int   s_clipCount = 0;
static Voice s_voices[JET_AUDIO_VOICES];
static int32_t s_master = 256;          // 0..256
static uint32_t s_lastUs = 0;
static int32_t  s_credit = 0;           // whole samples owed
static uint32_t s_creditFrac = 0;       // 16.16 remainder, so the rate never drifts
static int   s_started = 0;

void jet_audio_init(void)
{
    memset(s_clips, 0, sizeof(s_clips));
    memset(s_voices, 0, sizeof(s_voices));
    s_clipCount = 0;
    s_master = 256;
    s_lastUs = 0;
    s_credit = 0;
    s_creditFrac = 0;
    s_started = 0;
}

void jet_audio_shutdown(void)
{
    for (int i = 0; i < s_clipCount; ++i) {
        if (s_clips[i].data) free(s_clips[i].data);
        s_clips[i].data = NULL;
    }
    s_clipCount = 0;
    memset(s_voices, 0, sizeof(s_voices));
}

int jet_audio_load(const char* path)
{
    if (!path || s_clipCount >= MAX_CLIPS) return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes < 2) { fclose(f); return -1; }

    int16_t* buf = (int16_t*)malloc((size_t)bytes);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)bytes, f);
    fclose(f);
    if (got < 2) { free(buf); return -1; }

    const int id = s_clipCount++;
    s_clips[id].data = buf;
    s_clips[id].frames = (uint32_t)(got / 2);
    return id;
}

// Slot choice: a free slot first, else the voice with the least remaining work,
// so a long looping bed is not stolen by a short effect.
static int alloc_voice(void)
{
    for (int i = 0; i < JET_AUDIO_VOICES; ++i)
        if (!s_voices[i].active) return i;

    int best = 0;
    uint32_t bestLeft = 0xFFFFFFFFu;
    for (int i = 0; i < JET_AUDIO_VOICES; ++i) {
        uint32_t left;
        if (s_voices[i].loop) left = 0xFFFFFFFEu;
        else if (s_voices[i].clip >= 0)
            left = s_clips[s_voices[i].clip].frames - (s_voices[i].pos >> 16);
        else left = s_voices[i].remaining;
        if (left < bestLeft) { bestLeft = left; best = i; }
    }
    return best;
}

static inline int32_t clamp_vol(float v)
{
    int32_t x = (int32_t)(v * 256.0f + 0.5f);
    if (x < 0) x = 0;
    if (x > 1024) x = 1024;      // allow deliberate overdrive up to 4x
    return x;
}

static int voice_id(int slot) { return (slot & 0xFF) | (s_voices[slot].gen << 8); }

int jet_audio_play(int clip, float volume, int loop, float pitch)
{
    if (clip < 0 || clip >= s_clipCount || !s_clips[clip].data) return -1;
    if (pitch <= 0.01f) pitch = 1.0f;

    const int i = alloc_voice();
    Voice* v = &s_voices[i];
    v->gen++;
    v->active = 1;
    v->clip = clip;
    v->loop = loop ? 1 : 0;
    v->pos = 0;
    v->step = (uint32_t)(pitch * 65536.0f);
    v->vol = clamp_vol(volume);
    v->remaining = 0;
    return voice_id(i);
}

int jet_audio_tone(float freq, int ms, float volume, int wave)
{
    if (freq < 1.0f) freq = 1.0f;
    if (ms <= 0) ms = 1;

    const int i = alloc_voice();
    Voice* v = &s_voices[i];
    v->gen++;
    v->active = 1;
    v->clip = -1;
    v->loop = 0;
    v->wave = (uint8_t)wave;
    v->vol = clamp_vol(volume);
    v->remaining = (uint32_t)((int64_t)ms * JET_AUDIO_RATE / 1000);
    v->phase = 0;
    // 16.16 phase increment: one full turn per period.
    v->phaseInc = (uint32_t)((freq * 65536.0f) / (float)JET_AUDIO_RATE);
    v->rng = 0x1234567u ^ (uint32_t)(freq * 7.0f);
    return voice_id(i);
}

void jet_audio_stop(int voice)
{
    if (voice < 0) return;
    const int slot = voice & 0xFF;
    if (slot >= JET_AUDIO_VOICES) return;
    // Generation check: a stale id must not silence a slot that was reused.
    if (s_voices[slot].gen != ((voice >> 8) & 0xFF)) return;
    s_voices[slot].active = 0;
}

void jet_audio_stop_all(void)
{
    for (int i = 0; i < JET_AUDIO_VOICES; ++i) s_voices[i].active = 0;
}

void jet_audio_master(float volume) { s_master = clamp_vol(volume); }

int jet_audio_active(void)
{
    int n = 0;
    for (int i = 0; i < JET_AUDIO_VOICES; ++i) if (s_voices[i].active) n++;
    return n;
}

// Mix one voice into the accumulator. Split from the caller so each voice's
// inner loop is tight and its state stays in registers.
static void mix_voice(Voice* v, int32_t* acc, int n)
{
    if (v->clip >= 0) {
        const Clip* c = &s_clips[v->clip];
        const int16_t* src = c->data;
        const uint32_t end = c->frames << 16;
        uint32_t pos = v->pos;
        const uint32_t step = v->step;
        const int32_t vol = v->vol;

        for (int i = 0; i < n; ++i) {
            if (pos >= end) {
                if (!v->loop) { v->active = 0; break; }
                pos -= end;                       // wrap, keeping the fraction
                if (pos >= end) pos = 0;          // guard against absurd pitch
            }
            acc[i] += ((int32_t)src[pos >> 16] * vol) >> 8;
            pos += step;
        }
        v->pos = pos;
        return;
    }

    // Procedural. Square and triangle come off the same phase accumulator;
    // noise uses an LCG, which is cheaper than any table and sounds right for
    // percussion at this sample rate.
    uint32_t ph = v->phase;
    const uint32_t inc = v->phaseInc;
    const int32_t vol = v->vol;
    uint32_t rng = v->rng;
    uint32_t left = v->remaining;

    for (int i = 0; i < n; ++i) {
        if (left == 0) { v->active = 0; break; }
        int32_t s;
        switch (v->wave) {
            case JET_WAVE_NOISE:
                rng = rng * 1664525u + 1013904223u;
                s = (int32_t)((int16_t)(rng >> 16)) >> 1;
                break;
            case JET_WAVE_TRIANGLE: {
                // Phase 0..65535 folded into a -1..1 ramp.
                const uint32_t p = ph & 0xFFFF;
                const int32_t tri = (p < 32768) ? (int32_t)p - 16384
                                                : 49152 - (int32_t)p;
                s = tri * 2;
                break;
            }
            default:  // square
                s = (ph & 0x8000) ? 12000 : -12000;
                break;
        }
        acc[i] += (s * vol) >> 8;
        ph += inc;
        left--;
    }
    v->phase = ph;
    v->rng = rng;
    v->remaining = left;
}

void jet_audio_service(uint32_t nowUs)
{
    if (!s_started) { s_started = 1; s_lastUs = nowUs; return; }

    const uint32_t dtUs = nowUs - s_lastUs;
    s_lastUs = nowUs;

    // Credit accumulation in 16.16 so rounding never drifts the output rate.
    const uint64_t exact = ((uint64_t)dtUs * JET_AUDIO_RATE << 16) / 1000000u;
    const uint64_t total = exact + s_creditFrac;
    s_credit += (int32_t)(total >> 16);
    s_creditFrac = (uint32_t)(total & 0xFFFF);
    if (s_credit > MAX_CATCHUP) s_credit = MAX_CATCHUP;
    if (s_credit <= 0) return;

    // Nothing playing: drop the debt rather than pushing silence. The firmware
    // mixer already outputs silence when the ring is empty, so pushing zeroes
    // would just burn a ring copy every frame for the same result.
    if (jet_audio_active() == 0) { s_credit = 0; return; }

    int32_t acc[CHUNK];
    int16_t out[CHUNK];

    while (s_credit > 0) {
        const int n = (s_credit > CHUNK) ? CHUNK : s_credit;
        memset(acc, 0, sizeof(int32_t) * (size_t)n);

        for (int i = 0; i < JET_AUDIO_VOICES; ++i)
            if (s_voices[i].active) mix_voice(&s_voices[i], acc, n);

        for (int i = 0; i < n; ++i) {
            int32_t s = (acc[i] * s_master) >> 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            out[i] = (int16_t)s;
        }

        host_audio_push(out, n, JET_AUDIO_RATE);
        s_credit -= n;
    }
}
