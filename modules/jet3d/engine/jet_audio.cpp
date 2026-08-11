#include "jet_audio.h"
#include "music/jet_music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

// A clip is either plain 16-bit PCM or IMA ADPCM at 4 bits a sample.
// ADPCM exists because flash, not RAM, is the binding constraint: at 22050 it
// is HALF the size of the 11025 PCM it replaced while keeping the full
// bandwidth. It decodes with about ten integer operations a sample, against
// the twenty-odd million a second the FM synthesis cost.
//
// Container (tools/adpcm.py writes it): "JADP", u16 version, u16 block
// samples, u32 total samples, u32 rate, then fixed-size blocks. Each block
// restates the predictor in 4 header bytes, which is what lets a loop restart
// cleanly and keeps the format resynchronisable.
#define CLIP_PCM16  0
#define CLIP_ADPCM  1
#define ADPCM_HEADER 16

typedef struct {
    int16_t* data;          // PCM16: samples. ADPCM: the raw block stream.
    uint32_t frames;
    uint8_t  format;
    uint16_t blockSamples;  // ADPCM only
    uint32_t blockBytes;    // ADPCM only
} Clip;

// IMA ADPCM tables. Standard, and they must match tools/adpcm.py exactly or
// the predictor drifts apart from what was encoded.
static const int16_t s_imaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t s_imaIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    uint8_t  active;
    uint8_t  wave;       // procedural voices only
    uint8_t  loop;
    uint8_t  gen;        // bumped per reuse; encoded in the voice id
    uint8_t  hold;       // sustained tone: ignores `remaining`, runs till stop
    uint8_t  buzz;       // harsher variant of the waveform
    int      clip;       // -1 for a procedural tone
    // CLIP POSITION, split deliberately. This used to be one 16.16 uint32,
    // which can only address 65535 samples -- under three seconds at 22050 --
    // and `frames << 16` overflowed as well, so any longer clip decoded
    // correctly for 2.97 s and then wrapped into garbage. A music loop is
    // twenty seconds, so it has to be an integer sample index with the
    // fraction kept beside it.
    uint32_t pos;        // whole samples into the clip
    uint32_t posFrac;    // 16.16 remainder; only the low 16 bits are used
    // ADPCM decoder. No decode buffer: an ADPCM clip plays at exactly its
    // native rate, so one output sample consumes exactly one input nibble and
    // the whole decoder is four values.
    uint32_t adpByte;    // read cursor into the block stream
    int32_t  adpPred;
    uint8_t  adpIndex;
    uint8_t  adpHigh;    // next nibble is the high half of the current byte
    uint32_t step;       // 16.16 advance per output sample
    int32_t  vol;        // 0..256 (the PEAK; the envelope scales below it)
    uint32_t remaining;  // samples left for a tone; 0 = until stopped
    uint32_t phase;      // 16.16 oscillator phase
    uint32_t phaseInc;
    uint32_t phaseIncEnd; // sweep target; == phaseInc when not sweeping
    uint32_t total;      // envelope length in samples (0 = unshaped/legacy)
    uint32_t attack;     // samples of attack ramp
    uint32_t decay;      // samples of decay ramp at the tail
    uint32_t rng;
    float    nHist[4];   // cascaded one-pole states for the noise filter
    uint8_t  nPoles;     // 1..4, see jet_audio.h
    uint32_t lfoPhase;   // 16.16 vibrato phase
    uint32_t lfoInc;     // 0 = no vibrato
    float    vibDepth;
    float    tremDepth;
    uint8_t  vibSteps;
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
    const uint8_t* raw = (const uint8_t*)buf;

    if (got > ADPCM_HEADER && raw[0] == 'J' && raw[1] == 'A'
        && raw[2] == 'D' && raw[3] == 'P') {
        uint16_t blockSamples;
        uint32_t total;
        memcpy(&blockSamples, raw + 6, 2);
        memcpy(&total, raw + 8, 4);
        s_clips[id].format = CLIP_ADPCM;
        s_clips[id].blockSamples = blockSamples;
        // 4 header bytes plus one nibble per remaining sample.
        s_clips[id].blockBytes = 4u + (uint32_t)((blockSamples - 1 + 1) / 2);
        s_clips[id].frames = total;
        s_clips[id].data = buf;
        return id;
    }

    s_clips[id].format = CLIP_PCM16;
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
        // A held tone has remaining == 0, which would otherwise rank it as
        // the LEAST work and make it the first thing any short effect steals
        // — the engine note would die on the next pickup blip.
        if (s_voices[i].hold) left = 0xFFFFFFFEu;
        else if (s_voices[i].loop) left = 0xFFFFFFFEu;
        else if (s_voices[i].clip >= 0)
            left = s_clips[s_voices[i].clip].frames - s_voices[i].pos;
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

static void adpcm_rewind(Voice* v, const Clip* c);

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
    v->hold = 0;
    v->pos = 0;
    v->posFrac = 0;
    if (s_clips[clip].format == CLIP_ADPCM) {
        // One nibble per output sample is the whole reason the decoder needs
        // no buffer, so an ADPCM clip is not resampled -- it plays at its own
        // rate whatever was asked for.
        pitch = 1.0f;
        adpcm_rewind(v, &s_clips[clip]);
    }
    v->step = (uint32_t)(pitch * 65536.0f);
    v->vol = clamp_vol(volume);
    v->remaining = 0;
    return voice_id(i);
}

// ms <= 0 means HOLD: the tone sustains until jet_audio_stop, and its pitch
// and level can be steered live with jet_audio_voice_set. That is what a
// continuous engine note needs — retriggering a fixed-length tone every frame
// resets the oscillator phase, which clicks at the frame rate.
static inline uint32_t inc_for(float freq)
{
    if (freq < 1.0f) freq = 1.0f;
    return (uint32_t)((freq * 65536.0f) / (float)JET_AUDIO_RATE);
}

static int tone_start(float freq, int ms, float volume, int wave, int hold)
{
    if (freq < 1.0f) freq = 1.0f;
    if (!hold && ms <= 0) ms = 1;

    const int i = alloc_voice();
    Voice* v = &s_voices[i];
    v->gen++;
    v->active = 1;
    v->clip = -1;
    v->loop = 0;
    v->hold = hold ? 1 : 0;
    v->buzz = 0;
    v->wave = (uint8_t)wave;
    v->vol = clamp_vol(volume);
    v->remaining = hold ? 0u
                        : (uint32_t)((int64_t)ms * JET_AUDIO_RATE / 1000);
    v->phase = 0;
    v->phaseInc = v->phaseIncEnd = inc_for(freq);
    // Unshaped: total 0 keeps the envelope flat, so the plain tone() call
    // sounds exactly as it always did.
    v->total = v->attack = v->decay = 0;
    v->rng = 0x1234567u ^ (uint32_t)(freq * 7.0f);
    v->nHist[0] = v->nHist[1] = v->nHist[2] = v->nHist[3] = 0.0f;
    v->nPoles = 1;
    v->lfoPhase = v->lfoInc = 0; v->vibDepth = 0.0f; v->vibSteps = 0;
    v->tremDepth = 0.0f;
    return voice_id(i);
}

// The shaped one-shot. Everything the game actually uses goes through here.
int jet_audio_fx(const jet_fx_t* fx)
{
    if (!fx) return -1;
    int ms = fx->ms;
    if (!fx->hold && ms <= 0) ms = 1;

    const int i = alloc_voice();
    Voice* v = &s_voices[i];
    v->gen++;
    v->active = 1;
    v->clip = -1;
    v->loop = 0;
    v->hold = fx->hold ? 1 : 0;
    v->buzz = fx->buzz ? 1 : 0;
    v->wave = (uint8_t)fx->wave;
    v->vol = clamp_vol(fx->volume);
    v->phase = 0;
    v->phaseInc = inc_for(fx->freq);
    // A hold has no progress to sweep along (remaining stays 0), so a sweep
    // target would simply pin it at the target. Sweeps are for one-shots.
    v->phaseIncEnd = (fx->freq_to > 0.0f && !v->hold)
        ? inc_for(fx->freq_to) : v->phaseInc;

    const uint32_t dur = v->hold
        ? (uint32_t)JET_AUDIO_RATE          // sweep/attack reference for a hold
        : (uint32_t)((int64_t)ms * JET_AUDIO_RATE / 1000);
    v->remaining = v->hold ? 0u : dur;
    v->total   = dur ? dur : 1u;
    v->attack  = (uint32_t)(((fx->attack_ms < 0 ? 3 : fx->attack_ms)
                             * (int64_t)JET_AUDIO_RATE) / 1000);
    // Default decay is the whole tail after the attack, which is what makes a
    // sound fade instead of stopping dead.
    v->decay   = (fx->decay_ms < 0)
        ? ((v->total > v->attack) ? (v->total - v->attack) : 0u)
        : (uint32_t)(((int64_t)fx->decay_ms * JET_AUDIO_RATE) / 1000);
    if (v->attack > v->total) v->attack = v->total;
    if (v->decay  > v->total) v->decay  = v->total;
    v->rng = 0x2545F491u ^ (uint32_t)(fx->freq * 13.0f) ^ (uint32_t)i;
    v->nHist[0] = v->nHist[1] = v->nHist[2] = v->nHist[3] = 0.0f;
    v->nPoles = (fx->poles < 1) ? 1 : ((fx->poles > 4) ? 4 : (uint8_t)fx->poles);
    v->lfoPhase = 0;
    v->lfoInc   = (fx->vib_hz > 0.0f
                   && (fx->vib_depth > 0.0f || fx->trem_depth > 0.0f))
        ? (uint32_t)((fx->vib_hz * 65536.0f) / (float)JET_AUDIO_RATE) : 0u;
    v->vibDepth = fx->vib_depth;
    v->tremDepth = (fx->trem_depth > 1.0f) ? 1.0f
                 : ((fx->trem_depth > 0.0f) ? fx->trem_depth : 0.0f);
    v->vibSteps = (uint8_t)((fx->vib_steps > 0 && fx->vib_steps < 64)
                            ? fx->vib_steps : 0);
    return voice_id(i);
}

int jet_audio_tone(float freq, int ms, float volume, int wave)
{
    return tone_start(freq, ms, volume, wave, 0);
}

int jet_audio_tone_hold(float freq, float volume, int wave)
{
    return tone_start(freq, 0, volume, wave, 1);
}

// Retune / re-level a LIVE voice without restarting it: the phase is left
// alone, so a sweep is continuous. Silently ignores a stale or finished id,
// like jet_audio_stop. freq <= 0 leaves the pitch as it is (level-only
// change); on a clip voice, freq is applied as a playback-rate multiplier.
void jet_audio_voice_set(int voice, float freq, float volume)
{
    if (voice < 0) return;
    const int slot = voice & 0xFF;
    if (slot >= JET_AUDIO_VOICES) return;
    Voice* v = &s_voices[slot];
    if (v->gen != ((voice >> 8) & 0xFF) || !v->active) return;
    if (volume >= 0.0f) v->vol = clamp_vol(volume);
    if (freq > 0.0f) {
        if (v->clip >= 0) {
            v->step = (uint32_t)(freq * 65536.0f);
        } else {
            // BOTH, always. Setting only phaseInc leaves phaseIncEnd at the
            // creation frequency, which makes the sweep path go live and
            // lerp the pitch straight back to where it started -- the voice
            // then ignores every retune for the rest of its life.
            v->phaseInc = v->phaseIncEnd = inc_for(freq);
        }
    }
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

// ---------------------------------------------------------------------------
// Waveforms
// ---------------------------------------------------------------------------
// The shapes past square/triangle/noise are adapted from zepto-8's synth
// (Copyright (c) 2016-2020 Sam Hocevar, WTFPL — see PICO8-SYNTH-LICENSE.txt),
// whose formulas were reverse-engineered from PICO-8's own output. They are
// here because flat square/triangle tones sound like a calculator, while
// ORGAN and PULSE carry enough harmonic content to read as instruments.
//
// `t` is the phase within one cycle, 0..1. Output is roughly -1..1.
static inline float fract1(float x) { return x - (float)(int)x; }

static inline float wave_sample(Voice* v, float t, float nScale, float nCoef)
{
    const bool buzz = v->buzz != 0;
    switch (v->wave) {
        case JET_WAVE_TRIANGLE: {
            float ret = 1.0f - fabsf(4.0f * t - 2.0f);
            if (buzz) {
                const float a = 0.875f;
                const float b = (t < a) ? (2.0f * t / a - 1.0f)
                                        : (2.0f * (1.0f - t) / (1.0f - a) - 1.0f);
                ret = ret * 0.75f + b * 0.25f;
            }
            return ret;                 // normalised to +-1
        }
        case JET_WAVE_TILTSAW: {
            const float a = buzz ? 0.975f : 0.875f;
            const float ret = (t < a) ? (2.0f * t / a - 1.0f)
                                      : (2.0f * (1.0f - t) / (1.0f - a) - 1.0f);
            return ret;                 // normalised to +-1
        }
        case JET_WAVE_SAW:
            return 1.96f * ((t < 0.5f) ? t : (t - 1.0f));   // +-1
        case JET_WAVE_PULSE:
            return (t < (buzz ? 0.255f : 0.316f)) ? 1.0f : -1.0f;
        case JET_WAVE_ORGAN: {
            float ret = (t < 0.5f) ? (3.0f - fabsf(24.0f * t - 6.0f))
                                   : (1.0f - fabsf(16.0f * t - 12.0f));
            return ret / 3.0f;   // +-1
        }
        case JET_WAVE_NOISE: {
            // A one-pole lowpass on white noise whose cutoff follows the
            // pitch. This is what makes noise read as a drum or a whoosh
            // instead of undifferentiated hiss.
            uint32_t x = v->rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            v->rng = x;
            const float white = (float)(x >> 8) * (1.0f / 8388608.0f) - 1.0f;
            // nScale/nCoef are hoisted by the caller: no divide per sample.
            // Cascaded identical one-poles: 6dB/octave each. The make-up
            // gains keep loudness constant across pole counts (measured at
            // 500Hz and 710Hz), so a layer can be made steeper without
            // having to retune the game's levels.
            static const float MAKEUP[4] = { 1.0f, 1.50f, 1.65f, 1.83f };
            float filt = white;
            const int np = v->nPoles;
            for (int k = 0; k < np; ++k) {
                v->nHist[k] = (v->nHist[k] + nScale * filt) * nCoef;
                filt = v->nHist[k];
            }
            filt *= MAKEUP[np - 1];
            // Pitch-compensating gain, as in the original: the lower the
            // cutoff the harder this one-pole attenuates, so a low noise
            // voice is nearly SILENT without it. Dropping this term is
            // what made a low rumble layer inaudible.
            float lo = 1.0f - nScale * 5.0f;          // ~1 low, 0 from ~500Hz up
            if (lo < 0.0f) lo = 0.0f;
            const float nv = filt * 2.0f * (1.0f + lo * lo * 3.0f);
            return (nv > 1.0f) ? 1.0f : (nv < -1.0f ? -1.0f : nv);
        }
        case JET_WAVE_SINE: {
            // No table and no sinf(): a parabola plus one refinement step
            // lands within ~0.2% of a sine, which is inaudible here, and it
            // costs a few multiplies in a loop that runs 22050 times a
            // second per voice. sin(2*pi*t) == -sin(pi*u) for u = 2t-1.
            const float u  = 2.0f * t - 1.0f;
            const float au = (u < 0.0f) ? -u : u;
            float y = 4.0f * u * (1.0f - au);
            const float ay = (y < 0.0f) ? -y : y;
            y = y * (0.775f + 0.225f * ay);
            return -y;
        }
        default:  // square
            return (t < (buzz ? 0.4f : 0.5f)) ? 1.0f : -1.0f;
    }
}

// Mix one voice into the accumulator. Split from the caller so each voice's
// inner loop is tight and its state stays in registers.
// Re-seed from the first block header. Also what a loop restart does: the
// per-block predictor is exactly what makes that free.
static void adpcm_rewind(Voice* v, const Clip* c)
{
    const uint8_t* p = (const uint8_t*)c->data + ADPCM_HEADER;
    int16_t pred;
    memcpy(&pred, p, 2);
    v->adpPred = pred;
    v->adpIndex = p[2];
    v->adpByte = 0;      // samples consumed within the stream
    v->adpHigh = 0;
}

// One sample. `index` is the sample number, used only to spot a block
// boundary, where the predictor is restated rather than carried across.
static inline int32_t adpcm_next(Voice* v, const Clip* c, uint32_t index)
{
    const uint8_t* base = (const uint8_t*)c->data + ADPCM_HEADER;
    const uint32_t within = index % c->blockSamples;
    const uint32_t block = index / c->blockSamples;
    const uint8_t* blk = base + (size_t)block * c->blockBytes;
    int code, step, delta;

    if (within == 0) {
        int16_t pred;
        memcpy(&pred, blk, 2);
        v->adpPred = pred;
        v->adpIndex = blk[2];
        return pred;                 // the header sample itself
    }

    {
        const uint32_t nib = within - 1;
        const uint8_t byte = blk[4 + (nib >> 1)];
        code = (nib & 1) ? (byte >> 4) : (byte & 0x0F);
    }

    step = s_imaStep[v->adpIndex];
    delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    v->adpPred = (code & 8) ? (v->adpPred - delta) : (v->adpPred + delta);
    if (v->adpPred > 32767) v->adpPred = 32767;
    if (v->adpPred < -32768) v->adpPred = -32768;
    {
        int idx = v->adpIndex + s_imaIndex[code & 7];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        v->adpIndex = (uint8_t)idx;
    }
    return v->adpPred;
}

static void mix_voice(Voice* v, int32_t* acc, int n)
{
    if (v->clip >= 0 && s_clips[v->clip].format == CLIP_ADPCM) {
        const Clip* c = &s_clips[v->clip];
        const int32_t vol = v->vol;
        uint32_t idx = v->pos;           // whole samples: the rate is 1.0

        for (int i = 0; i < n; ++i) {
            if (idx >= c->frames) {
                if (!v->loop) { v->active = 0; break; }
                idx = 0;                 // the block header re-seeds the
                                         // predictor, so a loop needs nothing
                                         // else to restart cleanly
            }
            acc[i] += (adpcm_next(v, c, idx) * vol) >> 8;
            ++idx;
        }
        v->pos = idx;
        return;
    }

    if (v->clip >= 0) {
        const Clip* c = &s_clips[v->clip];
        const int16_t* src = c->data;
        uint32_t idx = v->pos;
        uint32_t frac = v->posFrac;
        const uint32_t step = v->step;      // 16.16 samples per output sample
        const int32_t vol = v->vol;

        for (int i = 0; i < n; ++i) {
            if (idx >= c->frames) {
                if (!v->loop) { v->active = 0; break; }
                idx = 0;                    // keep the fraction: a wrap
                                            // mid-sample stays smooth
            }
            acc[i] += ((int32_t)src[idx] * vol) >> 8;
            frac += step;
            idx += frac >> 16;
            frac &= 0xFFFF;
        }
        v->pos = idx;
        v->posFrac = frac;
        return;
    }

    // Procedural voice.
    //
    // EFFICIENCY: the inner loop must contain no divides. This runs
    // rate * active_voices times a second (22050 * up to 12), so a 64-bit
    // divide here is a libgcc __divdi3 CALL per sample on a 32-bit core, and
    // an integer divide is dozens of cycles. Everything divisible is hoisted
    // into per-call reciprocals and the sweep advances by a constant step.
    uint32_t left = v->remaining;

    // A silent voice still costs full synthesis if we let it through, and the
    // engine deliberately parks layers at zero (intake off-throttle, deck in
    // the air). Advance its state and leave.
    if (v->vol == 0) {
        if (!v->hold) {
            if (left <= (uint32_t)n) { v->remaining = 0; v->active = 0; }
            else v->remaining = left - (uint32_t)n;
        }
        v->phase += v->phaseInc * (uint32_t)n;
        return;
    }

    // Sweep: position derived from progress once, then stepped. phaseInc and
    // phaseIncEnd are equal unless an fx() asked for a sweep, and a retune
    // (jet_audio_voice_set) clears the sweep by setting both.
    float incF = (float)v->phaseInc;
    float incStep = 0.0f;
    if (v->phaseIncEnd != v->phaseInc && v->total) {
        const float span = (float)v->total;
        const uint32_t doneNow = (v->total > left) ? (v->total - left) : v->total;
        incStep = ((float)v->phaseIncEnd - (float)v->phaseInc) / span;
        incF    = (float)v->phaseInc + incStep * (float)doneNow;
    }

    // Envelope as reciprocals, so the per-sample form is a multiply.
    const float atkR = v->attack ? (1.0f / (float)v->attack) : 0.0f;
    const float decR = v->decay  ? (1.0f / (float)v->decay)  : 0.0f;
    uint32_t done = (v->total > left) ? (v->total - left) : 0u;

    // Noise filter coefficient, hoisted. Under vibrato the cutoff wobbles by
    // a couple of percent; reusing the call's coefficient is inaudible on a
    // noise layer and saves a float divide per sample.
    const float nScale = incF * (1.0f / 65536.0f) * 8.858923f;
    const float nCoef  = 1.0f / (1.0f + nScale);

    // Fold volume and full-scale into one factor (volume 1.0 == full scale).
    const float volF = (float)v->vol * (32000.0f / 256.0f);

    uint32_t ph = v->phase;

    for (int i = 0; i < n; ++i) {
        if (!v->hold && left == 0) { v->active = 0; break; }

        uint32_t inc = (uint32_t)incF;
        float trem = 1.0f;
        if (v->lfoInc) {
            // Triangle LFO in -1..1; indistinguishable from a sine at these
            // depths and needs no table. Stepped when vibSteps is set, which
            // reads as hardware-era rather than as a vibrato pedal.
            v->lfoPhase += v->lfoInc;
            const float lp = (float)(v->lfoPhase & 0xFFFF) * (1.0f / 65536.0f);
            float lfo = (lp < 0.5f) ? (4.0f * lp - 1.0f) : (3.0f - 4.0f * lp);
            if (v->vibSteps) {
                const float q = (float)v->vibSteps;
                lfo = (float)(int)(lfo * q + (lfo >= 0 ? 0.5f : -0.5f)) / q;
            }
            if (v->vibDepth != 0.0f) {
                inc = (uint32_t)(incF * (1.0f + v->vibDepth * lfo));
            }
            // Ducks below the peak only, never above it.
            if (v->tremDepth != 0.0f) {
                trem = 1.0f - v->tremDepth * (0.5f - 0.5f * lfo);
            }
        }
        if (!inc) inc = 1;

        float env = 1.0f;
        if (v->total) {
            if (v->attack && done < v->attack)            env = done * atkR;
            else if (!v->hold && v->decay && left < v->decay) env = left * decR;
        }
        if (trem != 1.0f) env *= trem;

        const float t = (float)(ph & 0xFFFF) * (1.0f / 65536.0f);
        const float s = wave_sample(v, t, nScale, nCoef);
        acc[i] += (int32_t)(s * env * volF);

        ph += inc;
        incF += incStep;
        if (left) left--;
        done++;
    }
    v->phase = ph;
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
    // Music counts as playing: it has its own voices inside the OPL chip, so
    // jet_audio_active() knows nothing about it and would silence a song
    // whenever no sound effect happened to be running.
    const int music = jet_music_playing();
    if (jet_audio_active() == 0 && !music) { s_credit = 0; return; }

    int32_t acc[CHUNK];
    int16_t out[CHUNK];

    while (s_credit > 0) {
        const int n = (s_credit > CHUNK) ? CHUNK : s_credit;
        memset(acc, 0, sizeof(int32_t) * (size_t)n);

        for (int i = 0; i < JET_AUDIO_VOICES; ++i)
            if (s_voices[i].active) mix_voice(&s_voices[i], acc, n);

        // The OPL synth ADDS into the same accumulator at the same rate, so
        // music needs no resampling and no second output path.
        if (music) jet_music_mix(acc, n);

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
