// Four-channel synthesis: a port of pyxel-core channel.rs and voice.rs (MIT,
// Takashi Kitao). Channels run the command lists sound.c compiles; time is
// counted in NES APU clocks (PX_AUDIO_CLOCK_RATE) as upstream does, and each
// output sample averages the oscillator over the clocks it covers.
//
// Samples are synthesized on the module's own core and pushed to the
// firmware's audio ring, kept LEAD samples ahead of real time. px_audio_poll()
// runs from the MicroPython VM hook, so the ring stays fed while Python code
// runs.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

extern void host_audio_push(const int16_t* samples, int count, int sample_rate);

#define CLOCK      PX_AUDIO_CLOCK_RATE
#define RATE       PX_AUDIO_RATE
#define INTERP     (CLOCK / 1250)          // 0.8 ms gain crossfade (NOTE_INTERP_CLOCKS)
#define LEAD       1654                    // 75 ms ahead of real time
#define MIN_BATCH  256
#define CHUNK      512
#define POLL_US    4000

enum { MODE_WAVETABLE, MODE_SHORT_NOISE, MODE_LONG_NOISE };

typedef struct {
    int mode;
    int len;
    float wave[PX_TONE_MAX_SAMPLES];
    float gain;
} tone_t;

typedef struct {
    float start_tick, start_level, slope;
} env_pt;

// voice.rs Voice
typedef struct {
    int tone;
    int wave_idx;
    uint16_t lfsr;
    int tap;                    // 0 = wavetable, 6 short noise, 1 long noise
    float osc;
    uint32_t sample_clocks, sample_remaining;

    float base_freq, velocity;
    uint64_t remaining, elapsed;
    // Ticks since playback started = pb_base + pb_clocks / cpt; pb_base
    // absorbs pb_clocks whenever the tempo changes.
    double pb_base;
    uint64_t pb_clocks;
    uint64_t next_mod;
    uint32_t cpt;
    float inv_cpt;

    bool env_on;
    env_pt* env;
    int env_n, env_cap, env_idx;

    bool vib_on;
    uint32_t vib_delay, vib_period;
    double vib_inv;
    float vib_depth, vib_mult;

    bool gli_on;
    float gli_off, gli_slope, gli_mult;
    uint32_t gli_dur;

    float last_gain, start_gain, end_gain;
    bool has_start, has_end;
} voice_t;

typedef struct { uint32_t slot; float init; px_env_seg* segs; int n; } env_slot;
typedef struct { uint32_t slot, delay, period; float depth; } vib_slot;
typedef struct { uint32_t slot; float off; uint32_t dur; } gli_slot;
typedef struct { int index; uint32_t count; uint64_t start; } rep_t;

// channel.rs Channel
typedef struct {
    px_snd* snds;
    int nsnd;
    bool loop;
    px_snd* rsnds;
    int nrsnd;
    bool rloop;
    bool resume, playing, playing_pcm;
    uint32_t pcm_pos;
    float gain;
    int detune;                 // cents
    int si, ci;
    uint64_t note_dur, sound_elapsed, total_elapsed;
    rep_t* reps;
    int nrep, caprep;
    uint32_t cpt;
    float gate, vol, transpose, detune_semis;
    bool gli_pending;
    float gli_pend_off;
    uint32_t gli_pend_ticks;
    bool has_last;
    float last_midi;
    env_slot* envs;
    int nenv;
    vib_slot* vibs;
    int nvib;
    gli_slot* glis;
    int ngli;
    voice_t v;
} chan_t;

static tone_t s_tones[PX_NUM_TONES];
static chan_t s_ch[PX_NUM_CHANNELS];
static bool s_enabled;
static bool s_started;
static int64_t s_pushed;          // samples handed to the ring
static int64_t s_elapsed_us;      // real time since the first push
static uint32_t s_last_us;
static uint32_t s_last_poll_us;
static uint32_t s_clk_rem;
static int16_t s_buf[CHUNK];
static float s_dc_x, s_dc_y;

uint32_t px_audio_samples, px_audio_underruns, px_audio_us;

// ---------------------------------------------------------------------------
// Tones (tone.rs)
// ---------------------------------------------------------------------------
void px_audio_set_tone(int i, int mode, const uint32_t* table, int len, int sample_bits, float gain) {
    if (i < 0 || i >= PX_NUM_TONES) return;
    tone_t* t = &s_tones[i];
    t->mode = mode;
    t->gain = gain;
    if (len > PX_TONE_MAX_SAMPLES) len = PX_TONE_MAX_SAMPLES;
    t->len = 0;
    if (sample_bits < 1 || sample_bits > 16) return;
    float max = (float)((1u << sample_bits) - 1);
    for (int k = 0; k < len; k++) {
        uint32_t v = table[k] > (uint32_t)max ? (uint32_t)max : table[k];
        t->wave[k] = (float)v / max * 2.0f - 1.0f;
    }
    t->len = len;
}

int px_audio_tone_mode(int i) {
    return (i >= 0 && i < PX_NUM_TONES) ? s_tones[i].mode : 0;
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------
static void osc_update(voice_t* v) {
    const tone_t* t = &s_tones[v->tone];
    if (v->tap == 0) v->osc = v->wave_idx < t->len ? t->wave[v->wave_idx] : 0.0f;
    else v->osc = (v->lfsr & 1) ? -1.0f : 1.0f;
}

static void osc_step(voice_t* v) {
    if (v->tap == 0) {
        int len = s_tones[v->tone].len;
        if (len > 0) v->wave_idx = (v->wave_idx + 1) % len;
    } else {
        uint16_t fb = (uint16_t)((v->lfsr ^ (v->lfsr >> v->tap)) & 1);
        v->lfsr = (uint16_t)(((v->lfsr >> 1) | (fb << 14)) & 0x7FFF);
    }
    osc_update(v);
}

static void voice_set_tone(voice_t* v, int tone) {
    v->tone = tone;
    const tone_t* t = &s_tones[tone];
    if (t->mode == MODE_WAVETABLE) {
        if (v->wave_idx >= t->len) v->wave_idx = 0;
        v->tap = 0;
    } else {
        int tap = t->mode == MODE_SHORT_NOISE ? 6 : 1;
        if (tap != v->tap) {
            v->lfsr = tap == 6 ? 0x0201 : 0x7001;
            v->tap = tap;
        }
    }
    osc_update(v);
}

static float pitch_mult(float semitones) {
    return exp2f(semitones / 12.0f);
}

static void voice_set_cpt(voice_t* v, uint32_t cpt) {
    if (cpt == 0) cpt = 1;
    v->pb_base += (double)v->pb_clocks / (double)v->cpt;
    v->pb_clocks = 0;
    v->cpt = cpt;
    v->inv_cpt = 1.0f / (float)cpt;
}

static void update_modulators(voice_t* v) {
    double note_ticks = (double)(v->elapsed / v->cpt);
    if (!v->vib_on) {
        v->vib_mult = 1.0f;
    } else {
        double el = v->vib_delay > 0 ? note_ticks - (double)v->vib_delay
                                     : v->pb_base + (double)v->pb_clocks / (double)v->cpt;
        if (el < 0.0) {
            v->vib_mult = 1.0f;
        } else {
            double ph = el * v->vib_inv + 0.25;
            ph -= floor(ph);
            float mod = 1.0f - 4.0f * (float)fabs(ph - 0.5);
            v->vib_mult = pitch_mult(mod * v->vib_depth);
        }
    }
    if (!v->gli_on || note_ticks >= (double)v->gli_dur)
        v->gli_mult = 1.0f;
    else
        v->gli_mult = pitch_mult(v->gli_off + v->gli_slope * (float)note_ticks);
}

static void update_sample_clocks(voice_t* v) {
    float f = v->base_freq * v->vib_mult * v->gli_mult;
    const tone_t* t = &s_tones[v->tone];
    int spc = (t->mode == MODE_WAVETABLE && t->len > 0) ? t->len : 1;
    float c = f > 0.0f ? roundf((float)CLOCK / f / (float)spc) : 1.0e9f;
    if (c < 1.0f) c = 1.0f;
    if (c > 1.0e9f) c = 1.0e9f;
    v->sample_clocks = (uint32_t)c;
}

static uint32_t rescale(uint32_t rem, uint32_t prev, uint32_t cur) {
    if (rem == 0 || prev == 0) return cur;
    uint64_t s = ((uint64_t)rem * cur + prev - 1) / prev;
    if (s < 1) s = 1;
    if (s > 0xFFFFFFFFu) s = 0xFFFFFFFFu;
    return (uint32_t)s;
}

static void env_set(voice_t* v, float init, const px_env_seg* segs, int n) {
    if (n + 1 > v->env_cap) {
        env_pt* g = (env_pt*)realloc(v->env, sizeof(env_pt) * (size_t)(n + 1));
        if (!g) { v->env_n = 0; return; }
        v->env = g;
        v->env_cap = n + 1;
    }
    uint64_t start = 0;
    float level = init;
    for (int i = 0; i < n; i++) {
        float slope = segs[i].ticks > 0 ? (segs[i].level - level) / (float)segs[i].ticks : 0.0f;
        v->env[i].start_tick = (float)start;
        v->env[i].start_level = level;
        v->env[i].slope = slope;
        start += segs[i].ticks;
        level = segs[i].level;
    }
    v->env[n].start_tick = (float)start;
    v->env[n].start_level = level;
    v->env[n].slope = 0.0f;
    v->env_n = n + 1;
    v->env_idx = 0;
}

static float env_level(voice_t* v) {
    if (!v->env_on || v->env_n == 0) return 1.0f;
    uint64_t el = v->elapsed;
    float t = (float)(uint32_t)(el > 0xFFFFFFFFu ? 0xFFFFFFFFu : el) * v->inv_cpt;
    while (v->env_idx + 1 < v->env_n && t >= v->env[v->env_idx + 1].start_tick) v->env_idx++;
    const env_pt* e = &v->env[v->env_idx];
    return e->start_level + e->slope * (t - e->start_tick);
}

static void vib_set(voice_t* v, uint32_t delay, uint32_t period, float depth) {
    v->vib_delay = delay;
    v->vib_depth = depth;
    if (period != v->vib_period) {
        v->vib_period = period;
        v->vib_inv = period > 0 ? 1.0 / (double)period : 0.0;
    }
}

static void gli_set(voice_t* v, float off, uint32_t dur) {
    if (off != v->gli_off || dur != v->gli_dur) {
        v->gli_off = off;
        v->gli_dur = dur;
        v->gli_slope = dur > 0 ? -off / (float)dur : 0.0f;
    }
}

static void voice_play_note(voice_t* v, float midi, float velocity, uint64_t playback) {
    uint32_t prev_clocks = v->sample_clocks, prev_rem = v->sample_remaining;
    v->base_freq = 440.0f * exp2f((midi - 69.0f) / 12.0f);
    v->velocity = velocity;
    v->remaining = playback + INTERP;
    v->elapsed = 0;
    v->next_mod = v->cpt;
    v->has_start = v->has_end = false;
    v->env_idx = 0;
    update_modulators(v);
    update_sample_clocks(v);
    v->sample_remaining = rescale(prev_rem, prev_clocks, v->sample_clocks);
}

static void voice_cancel(voice_t* v) {
    if (v->remaining > INTERP) v->remaining = INTERP;
    v->sample_remaining = v->sample_clocks;
}

static bool voice_needs(const voice_t* v) {
    return v->remaining > 0 || v->last_gain != 0.0f;
}

// Advance n clocks; with synth, accumulate oscillator * gain * clocks into acc.
static void voice_process(voice_t* v, uint32_t n, bool synth, float* acc) {
    while (n > 0) {
        if (v->remaining == 0) {
            v->last_gain = 0.0f;
            return;
        }
        if (synth && v->sample_remaining == 0) v->sample_remaining = v->sample_clocks;
        float g;
        uint64_t limit;
        if (v->elapsed < INTERP && v->remaining >= INTERP) {
            // head: crossfade from the previous gain
            if (!v->has_start) {
                v->has_start = true;
                v->start_gain = v->last_gain;
            }
            float target = env_level(v) * v->velocity * s_tones[v->tone].gain;
            uint32_t el = (uint32_t)v->elapsed;   // < INTERP here
            g = (v->start_gain * (float)(INTERP - el) + target * (float)el) * (1.0f / (float)INTERP);
            limit = INTERP - v->elapsed;
            uint64_t to_tail = v->remaining > INTERP ? v->remaining - INTERP : 1;
            if (to_tail < limit) limit = to_tail;
        } else if (v->remaining > INTERP) {
            g = env_level(v) * v->velocity * s_tones[v->tone].gain;
            limit = v->remaining - INTERP;
        } else {
            // tail: fade out over the last INTERP clocks
            if (!v->has_end) {
                v->has_end = true;
                v->end_gain = v->last_gain;
            }
            g = v->end_gain * (float)(uint32_t)v->remaining * (1.0f / (float)INTERP);   // remaining <= INTERP
            limit = v->remaining;
        }
        v->last_gain = g;

        uint32_t seg = n;
        if (limit < seg) seg = (uint32_t)limit;
        if (synth && v->sample_remaining < seg) seg = v->sample_remaining;
        if (v->next_mod > v->elapsed && v->next_mod - v->elapsed < seg) seg = (uint32_t)(v->next_mod - v->elapsed);
        if (seg == 0) seg = 1;

        if (synth) *acc += v->osc * g * (float)seg;
        v->remaining -= seg;
        v->elapsed += seg;
        v->pb_clocks += seg;
        n -= seg;

        uint32_t prev_clocks = v->sample_clocks;
        bool stepped = false;
        if (synth) {
            v->sample_remaining -= seg;
            if (v->sample_remaining == 0 && v->remaining > 0) {
                osc_step(v);
                stepped = true;
            }
        }
        if (v->elapsed >= v->next_mod) {
            v->next_mod = (v->elapsed / v->cpt + 1) * (uint64_t)v->cpt;
            update_modulators(v);
            update_sample_clocks(v);
        }
        if (v->remaining == 0) v->sample_remaining = 0;
        else if (!synth || stepped) v->sample_remaining = v->sample_clocks;
        else if (v->sample_clocks != prev_clocks)
            v->sample_remaining = rescale(v->sample_remaining, prev_clocks, v->sample_clocks);
    }
}

// ---------------------------------------------------------------------------
// Channel: effect slots
// ---------------------------------------------------------------------------
static env_slot* find_env(chan_t* c, uint32_t slot) {
    for (int i = 0; i < c->nenv; i++)
        if (c->envs[i].slot == slot) return &c->envs[i];
    return NULL;
}

static vib_slot* find_vib(chan_t* c, uint32_t slot) {
    for (int i = 0; i < c->nvib; i++)
        if (c->vibs[i].slot == slot) return &c->vibs[i];
    return NULL;
}

static gli_slot* find_gli(chan_t* c, uint32_t slot) {
    for (int i = 0; i < c->ngli; i++)
        if (c->glis[i].slot == slot) return &c->glis[i];
    return NULL;
}

static void store_env(chan_t* c, uint32_t slot, float init, const px_env_seg* segs, int n) {
    env_slot* e = find_env(c, slot);
    if (!e) {
        env_slot* g = (env_slot*)realloc(c->envs, sizeof(env_slot) * (size_t)(c->nenv + 1));
        if (!g) return;
        c->envs = g;
        e = &c->envs[c->nenv++];
        memset(e, 0, sizeof(*e));
        e->slot = slot;
    }
    px_env_seg* copy = (px_env_seg*)malloc(sizeof(px_env_seg) * (size_t)(n ? n : 1));
    if (!copy) return;
    memcpy(copy, segs, sizeof(px_env_seg) * (size_t)n);
    free(e->segs);
    e->segs = copy;
    e->n = n;
    e->init = init;
}

static void store_vib(chan_t* c, uint32_t slot, uint32_t delay, uint32_t period, float depth) {
    vib_slot* s = find_vib(c, slot);
    if (!s) {
        vib_slot* g = (vib_slot*)realloc(c->vibs, sizeof(vib_slot) * (size_t)(c->nvib + 1));
        if (!g) return;
        c->vibs = g;
        s = &c->vibs[c->nvib++];
        s->slot = slot;
    }
    s->delay = delay;
    s->period = period;
    s->depth = depth;
}

static void store_gli(chan_t* c, uint32_t slot, float off, uint32_t dur) {
    gli_slot* s = find_gli(c, slot);
    if (!s) {
        gli_slot* g = (gli_slot*)realloc(c->glis, sizeof(gli_slot) * (size_t)(c->ngli + 1));
        if (!g) return;
        c->glis = g;
        s = &c->glis[c->ngli++];
        s->slot = slot;
    }
    s->off = off;
    s->dur = dur;
}

// Glide with both parameters given applies now; '*' parameters resolve per note.
static void apply_glide(chan_t* c, float off, uint32_t dur) {
    if (!isnan(off) && dur != PX_AUTO) {
        c->gli_pending = false;
        gli_set(&c->v, off, dur);
    } else {
        c->gli_pending = true;
        c->gli_pend_off = off;
        c->gli_pend_ticks = dur;
    }
    c->v.gli_on = true;
}

// ---------------------------------------------------------------------------
// Channel: sequencing
// ---------------------------------------------------------------------------
static void free_snds(px_snd* s, int n) {
    if (!s) return;
    for (int i = 0; i < n; i++) px_snd_free(&s[i]);
    free(s);
}

static void update_playing_pcm(chan_t* c) {
    c->playing_pcm = c->si < c->nsnd && c->snds[c->si].pcm >= 0;
}

static void play_from(chan_t* c, px_snd* snds, int n, uint64_t start, bool loop, bool resume);

static void advance_command(chan_t* c) {
    const px_snd* s = &c->snds[c->si];
    while (c->ci < s->ncmd) {
        const px_cmd* m = &s->cmds[c->ci++];
        switch (m->type) {
        case PX_CMD_TEMPO:
            c->cpt = m->u;
            voice_set_cpt(&c->v, m->u);
            break;
        case PX_CMD_QUANTIZE:
            c->gate = m->f;
            break;
        case PX_CMD_TONE:
            voice_set_tone(&c->v, m->u < PX_NUM_TONES ? (int)m->u : 0);
            break;
        case PX_CMD_VOLUME:
            c->vol = m->f;
            break;
        case PX_CMD_TRANSPOSE:
            c->transpose = m->f;
            break;
        case PX_CMD_DETUNE:
            c->detune_semis = m->f;
            break;
        case PX_CMD_ENVELOPE: {
            env_slot* e = find_env(c, m->u);
            if (e) {
                env_set(&c->v, e->init, e->segs, e->n);
                c->v.env_on = true;
            } else {
                c->v.env_on = false;
            }
            break;
        }
        case PX_CMD_ENVELOPE_SET:
            store_env(c, m->u, m->f, &s->segs[m->seg], (int)m->nseg);
            env_set(&c->v, m->f, &s->segs[m->seg], (int)m->nseg);
            c->v.env_on = true;
            break;
        case PX_CMD_VIBRATO: {
            vib_slot* vs = find_vib(c, m->u);
            if (vs) {
                vib_set(&c->v, vs->delay, vs->period, vs->depth);
                c->v.vib_on = true;
            } else {
                c->v.vib_on = false;
            }
            break;
        }
        case PX_CMD_VIBRATO_SET:
            store_vib(c, m->u, m->ticks, m->period, m->f);
            vib_set(&c->v, m->ticks, m->period, m->f);
            c->v.vib_on = true;
            break;
        case PX_CMD_GLIDE: {
            gli_slot* gs = find_gli(c, m->u);
            if (gs) {
                apply_glide(c, gs->off, gs->dur);
            } else {
                c->gli_pending = false;
                c->v.gli_on = false;
            }
            break;
        }
        case PX_CMD_GLIDE_SET:
            store_gli(c, m->u, m->f, m->ticks);
            apply_glide(c, m->f, m->ticks);
            break;
        case PX_CMD_NOTE: {
            float midi = (float)m->u + c->transpose + c->detune_semis + (float)c->detune / 100.0f;
            c->note_dur = (uint64_t)c->cpt * m->ticks;
            uint64_t playback = (uint64_t)((double)c->note_dur * (double)c->gate + 0.5);
            if (c->gli_pending) {
                float off = isnan(c->gli_pend_off) ? (c->has_last ? c->last_midi - midi : 0.0f)
                                                   : c->gli_pend_off;
                uint32_t t = c->gli_pend_ticks == PX_AUTO ? m->ticks : c->gli_pend_ticks;
                gli_set(&c->v, off, t);
            }
            voice_play_note(&c->v, midi, c->gain * c->vol, playback);
            c->last_midi = midi;
            c->has_last = true;
            return;
        }
        case PX_CMD_REST:
            c->note_dur = (uint64_t)c->cpt * m->ticks;
            return;
        case PX_CMD_REPEAT_START:
            if (c->nrep == c->caprep) {
                int cap = c->caprep ? c->caprep * 2 : 4;
                rep_t* g = (rep_t*)realloc(c->reps, sizeof(rep_t) * (size_t)cap);
                if (!g) break;
                c->reps = g;
                c->caprep = cap;
            }
            c->reps[c->nrep].index = c->ci;
            c->reps[c->nrep].count = 0;
            c->reps[c->nrep].start = c->sound_elapsed;
            c->nrep++;
            break;
        case PX_CMD_REPEAT_END:
            if (c->nrep > 0) {
                rep_t r = c->reps[--c->nrep];
                if (c->sound_elapsed == r.start) {
                    // an empty repeat body: a forever repeat ends the sound
                    if (m->u == 0) {
                        c->ci = s->ncmd;
                        c->nrep = 0;
                    }
                    continue;
                }
                uint32_t next = r.count + 1;
                if (m->u == 0 || next < m->u) {
                    c->reps[c->nrep].index = r.index;
                    c->reps[c->nrep].count = next;
                    c->reps[c->nrep].start = c->sound_elapsed;
                    c->nrep++;
                    c->ci = r.index;
                }
            }
            break;
        default:
            break;
        }
    }
}

static void resume_background(chan_t* c) {
    px_snd* rs = c->rsnds;
    int rn = c->nrsnd;
    bool rl = c->rloop;
    c->rsnds = NULL;
    c->nrsnd = 0;
    play_from(c, rs, rn, c->total_elapsed, rl, false);
}

// channel.rs process: run clk clocks of the command stream.
static void chan_process(chan_t* c, uint32_t clk, bool synth, float* acc) {
    uint32_t start = clk;
    while (clk > 0) {
        if (!c->playing) {
            voice_process(&c->v, clk, synth, acc);
            return;
        }
        if (c->playing_pcm) {
            if (voice_needs(&c->v)) voice_process(&c->v, clk, synth, acc);
            return;
        }
        if (c->sound_elapsed == 0) {
            c->note_dur = 0;
            c->ci = 0;
            c->nrep = 0;
            advance_command(c);
            if (!c->playing) continue;
        }
        uint32_t pc = c->note_dur < clk ? (uint32_t)c->note_dur : clk;
        voice_process(&c->v, pc, synth, acc);
        clk -= pc;
        c->note_dur -= pc;
        c->sound_elapsed += pc;
        c->total_elapsed += pc;
        if (c->note_dur == 0) {
            advance_command(c);
            if (!c->playing) continue;
        }
        if (c->note_dur == 0) {
            c->si++;
            c->sound_elapsed = 0;
            if (c->si < c->nsnd) {
                update_playing_pcm(c);
            } else if (c->loop && clk < start) {
                c->si = 0;
                update_playing_pcm(c);
            } else if (c->resume) {
                resume_background(c);
            } else {
                c->playing = false;
                c->playing_pcm = false;
            }
        }
    }
}

static bool advance_pcm_sound(chan_t* c) {
    c->si++;
    c->sound_elapsed = 0;
    c->pcm_pos = 0;
    if (c->si >= c->nsnd) {
        if (c->loop) {
            c->si = 0;
        } else if (c->resume) {
            resume_background(c);
            return c->playing;
        } else {
            c->playing = false;
            c->playing_pcm = false;
            return false;
        }
    }
    update_playing_pcm(c);
    return c->playing;
}

// One PCM output sample (channel.rs mix_pcm), gain applied.
static float pcm_sample(chan_t* c) {
    int skipped = 0;
    while (c->playing && c->playing_pcm) {
        uint32_t len;
        const int16_t* d = px_pcm_samples(c->snds[c->si].pcm, &len);
        if (c->pcm_pos < len) {
            float v = (float)d[c->pcm_pos] * c->gain / 32767.0f;
            c->pcm_pos++;
            uint64_t el = (uint64_t)c->pcm_pos * CLOCK / RATE;
            c->total_elapsed += el - c->sound_elapsed;
            c->sound_elapsed = el;
            if (c->pcm_pos >= len) advance_pcm_sound(c);
            return v;
        }
        if (skipped >= c->nsnd || !advance_pcm_sound(c)) {
            c->playing = false;
            c->playing_pcm = false;
            return 0.0f;
        }
        skipped++;
    }
    return 0.0f;
}

// channel.rs seek_pcm: returns the clocks left when a command sound or the
// playlist end is reached.
static uint64_t seek_pcm(chan_t* c, uint64_t start_clock) {
    uint64_t offset = start_clock * RATE / CLOCK;
    uint64_t loop_samples = 0;
    if (c->loop && c->si == 0 && c->pcm_pos == 0) {
        bool all_pcm = true;
        for (int i = 0; i < c->nsnd && all_pcm; i++) {
            uint32_t len;
            if (c->snds[i].pcm < 0) all_pcm = false;
            else if (px_pcm_samples(c->snds[i].pcm, &len)) loop_samples += len;
        }
        if (!all_pcm) loop_samples = 0;
    }
    uint64_t remaining = loop_samples ? offset % loop_samples : offset;
    bool progress = false;
    while (remaining > 0) {
        if (c->si >= c->nsnd) {
            if (c->loop && progress) {
                c->si = 0;
                progress = false;
                continue;
            }
            break;
        }
        if (c->snds[c->si].pcm < 0) break;
        uint32_t len = 0;
        px_pcm_samples(c->snds[c->si].pcm, &len);
        if (remaining >= len) {
            remaining -= len;
            progress |= len > 0;
            c->si++;
            c->sound_elapsed = 0;
            c->pcm_pos = 0;
        } else {
            c->pcm_pos = (uint32_t)remaining;
            remaining = 0;
        }
    }
    c->sound_elapsed = (uint64_t)c->pcm_pos * CLOCK / RATE;
    uint64_t rem_clocks = remaining * CLOCK / RATE;
    if (rem_clocks > start_clock) rem_clocks = start_clock;
    c->total_elapsed += start_clock - rem_clocks;
    if (c->si >= c->nsnd) {
        if (c->loop && progress) {
            c->si = 0;
            update_playing_pcm(c);
        } else {
            c->playing = false;
            c->playing_pcm = false;
        }
    } else {
        update_playing_pcm(c);
    }
    return rem_clocks;
}

static void chan_seek(chan_t* c, uint64_t clocks) {
    while (clocks > 0 && c->playing) {
        if (c->playing_pcm) {
            clocks = seek_pcm(c, clocks);
        } else {
            uint32_t chunk = clocks < (1u << 30) ? (uint32_t)clocks : (1u << 30);
            uint64_t before = c->total_elapsed;
            chan_process(c, chunk, false, NULL);
            uint64_t consumed = c->total_elapsed - before;
            if (consumed == 0) return;
            clocks -= consumed < chunk ? consumed : chunk;
        }
    }
}

static uint64_t wrap_loop(chan_t* c, uint64_t start) {
    if (!c->loop) return start;
    uint64_t total = 0;
    for (int i = 0; i < c->nsnd; i++) {
        uint64_t t;
        if (c->snds[i].pcm >= 0 || !px_snd_total_clocks(&c->snds[i], &t)) return start;
        total += t;
    }
    return total ? start % total : start;
}

// channel.rs play_from_clock; takes ownership of snds.
static void play_from(chan_t* c, px_snd* snds, int n, uint64_t start, bool loop, bool resume) {
    bool empty = true;
    for (int i = 0; i < n; i++)
        if (!snds[i].empty) empty = false;
    if (empty) {
        free_snds(snds, n);
        c->playing = false;
        c->playing_pcm = false;
        return;
    }
    if (!resume) {
        c->total_elapsed = 0;
    } else if (!c->resume) {
        free_snds(c->rsnds, c->nrsnd);
        c->rsnds = c->snds;
        c->nrsnd = c->nsnd;
        c->rloop = c->loop;
        c->snds = NULL;
        c->nsnd = 0;
    }
    free_snds(c->snds, c->nsnd);
    c->snds = snds;
    c->nsnd = n;
    c->playing = true;
    c->loop = loop;
    c->resume = resume;
    c->si = 0;
    c->note_dur = 0;
    c->sound_elapsed = 0;
    c->ci = 0;
    c->nrep = 0;
    c->has_last = false;
    c->pcm_pos = 0;
    c->v.pb_base = 0.0;
    c->v.pb_clocks = 0;
    update_playing_pcm(c);
    if (c->playing_pcm) voice_cancel(&c->v);
    if (start > 0) {
        uint64_t seek = wrap_loop(c, start);
        chan_seek(c, seek);
        if (seek != start) c->total_elapsed = start;
    }
}

// ---------------------------------------------------------------------------
// Public control
// ---------------------------------------------------------------------------
void px_audio_set_channel(int ch, float gain, int detune_cents) {
    if (ch < 0 || ch >= PX_NUM_CHANNELS) return;
    s_ch[ch].gain = gain;
    s_ch[ch].detune = detune_cents;
}

void px_audio_play(int ch, px_snd* snds, int n, float sec, bool loop, bool resume) {
    if (ch < 0 || ch >= PX_NUM_CHANNELS) {
        free_snds(snds, n);
        return;
    }
    play_from(&s_ch[ch], snds, n, (uint64_t)((double)sec * CLOCK + 0.5), loop, resume);
}

void px_audio_stop(int ch) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++) {
        if (ch >= 0 && i != ch) continue;
        s_ch[i].playing = false;
        s_ch[i].playing_pcm = false;
        voice_cancel(&s_ch[i].v);
    }
}

bool px_audio_pos(int ch, int* sound_index, float* sec) {
    if (ch < 0 || ch >= PX_NUM_CHANNELS || !s_ch[ch].playing) return false;
    *sound_index = s_ch[ch].si;
    *sec = (float)((double)s_ch[ch].sound_elapsed / CLOCK);
    return true;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
static bool chan_active(const chan_t* c) {
    return c->playing || voice_needs(&c->v);
}

static bool any_active(void) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++)
        if (chan_active(&s_ch[i])) return true;
    return false;
}

static void synth(int16_t* out, int n) {
    for (int k = 0; k < n; k++) {
        s_clk_rem += CLOCK;
        uint32_t clk = s_clk_rem / RATE;
        s_clk_rem %= RATE;
        float voices = 0.0f, pcm = 0.0f;
        for (int i = 0; i < PX_NUM_CHANNELS; i++) {
            chan_t* c = &s_ch[i];
            if (!chan_active(c)) continue;
            float acc = 0.0f;
            if (c->playing && c->playing_pcm) {
                pcm += pcm_sample(c);
                if (voice_needs(&c->v)) voice_process(&c->v, clk, true, &acc);
            } else {
                chan_process(c, clk, true, &acc);
            }
            voices += acc / (float)clk;
        }
        // One-pole DC blocker on the synthesized voices, pole 1 - 1/512: the
        // high-pass blip_buf applies upstream (bass_shift 9, about 7 Hz at
        // 22050 Hz). PCM is mixed after it, unfiltered, as upstream.
        s_dc_y = voices - s_dc_x + s_dc_y * (1.0f - 1.0f / 512.0f);
        s_dc_x = voices;
        float f = (s_dc_y + pcm) * 32767.0f;
        int v = (int)(f + (f >= 0.0f ? 0.5f : -0.5f));
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

void px_audio_service(void) {
    uint32_t now = host_get_ticks_us();
    s_elapsed_us += (uint32_t)(now - s_last_us);
    s_last_us = now;
    s_last_poll_us = now;
    int64_t consumed = s_elapsed_us * RATE / 1000000;
    int64_t fill = s_pushed - consumed;

    if (!s_enabled) {
        // Keep the sequencer on time without producing sound.
        if (fill < 0) {
            uint64_t clocks = (uint64_t)(-fill) * CLOCK / RATE;
            for (int i = 0; i < PX_NUM_CHANNELS; i++) {
                chan_t* c = &s_ch[i];
                if (c->playing) chan_seek(c, clocks);
                else if (voice_needs(&c->v)) voice_process(&c->v, (uint32_t)(clocks < (1u << 30) ? clocks : (1u << 30)), false, NULL);
            }
            s_pushed = consumed;
        }
        return;
    }
    bool active = any_active();
    if (fill < 0) {
        if (active && s_started) px_audio_underruns++;
        s_pushed = consumed;
        fill = 0;
    }
    if (!active) {
        s_started = false;
        return;
    }
    int64_t want = LEAD - fill;
    if (want < MIN_BATCH && s_started) return;
    s_started = true;
    uint32_t t0 = host_get_ticks_us();
    while (want > 0) {
        int n = want < CHUNK ? (int)want : CHUNK;
        synth(s_buf, n);
        host_audio_push(s_buf, n, RATE);
        s_pushed += n;
        px_audio_samples += (uint32_t)n;
        want -= n;
    }
    px_audio_us += host_get_ticks_us() - t0;
}

void px_audio_poll(void) {
    uint32_t now = host_get_ticks_us();
    if ((uint32_t)(now - s_last_poll_us) < POLL_US) return;
    px_audio_service();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
static const uint32_t TRIANGLE[32] = {
    8, 9, 10, 11, 12, 13, 14, 15, 15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7,
};
static const uint32_t SQUARE[2] = { 1, 0 };
static const uint32_t PULSE[4] = { 1, 0, 0, 0 };

void px_audio_init(bool enabled) {
    px_audio_shutdown();
    s_enabled = enabled;
    px_audio_set_tone(0, MODE_WAVETABLE, TRIANGLE, 32, 4, 1.0f);
    px_audio_set_tone(1, MODE_WAVETABLE, SQUARE, 2, 1, 0.3f);
    px_audio_set_tone(2, MODE_WAVETABLE, PULSE, 4, 1, 0.3f);
    px_audio_set_tone(3, MODE_LONG_NOISE, NULL, 0, 4, 0.6f);
    for (int i = 0; i < PX_NUM_CHANNELS; i++) {
        chan_t* c = &s_ch[i];
        c->gain = 0.125f;
        c->gate = 1.0f;
        c->vol = 1.0f;
        c->v.cpt = 1;
        c->v.inv_cpt = 1.0f;
        c->v.vib_period = 1;
        c->v.vib_inv = 1.0;
        c->v.vib_mult = 1.0f;
        c->v.gli_mult = 1.0f;
    }
    s_last_us = host_get_ticks_us();
    s_last_poll_us = s_last_us;
}

void px_audio_shutdown(void) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++) {
        chan_t* c = &s_ch[i];
        free_snds(c->snds, c->nsnd);
        free_snds(c->rsnds, c->nrsnd);
        free(c->reps);
        for (int k = 0; k < c->nenv; k++) free(c->envs[k].segs);
        free(c->envs);
        free(c->vibs);
        free(c->glis);
        free(c->v.env);
    }
    memset(s_ch, 0, sizeof(s_ch));
    px_pcm_shutdown();
    s_dc_x = s_dc_y = 0.0f;
    s_clk_rem = 0;
    s_pushed = 0;
    s_elapsed_us = 0;
    s_started = false;
}
