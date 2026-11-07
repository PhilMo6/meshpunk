// Sound compilation: classic sounds, MML and PCM into command lists. Port of
// pyxel-core sound.rs (emit_commands), mml_parser.rs, old_mml_parser.rs and
// pcm_decoder.rs (MIT, Takashi Kitao). PCM decoding handles WAV only.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

#define TICKS_PER_QUARTER_NOTE 48
#define SOUND_TICKS_PER_SECOND 120
#define VIBRATO_PERIOD_TICKS   (SOUND_TICKS_PER_SECOND / 6)
#define VIBRATO_DEPTH_CENTS    25
#define MAX_VOLUME             7

enum { FX_NONE, FX_SLIDE, FX_VIBRATO, FX_FADEOUT, FX_HALF_FADEOUT, FX_QUARTER_FADEOUT };

// ---------------------------------------------------------------------------
// Command lists
// ---------------------------------------------------------------------------
void px_snd_init(px_snd* s) {
    memset(s, 0, sizeof(*s));
    s->pcm = -1;
    s->empty = true;
}

void px_snd_free(px_snd* s) {
    free(s->cmds);
    free(s->segs);
    if (s->pcm >= 0) px_pcm_unref(s->pcm);
    px_snd_init(s);
}

static px_cmd* push(px_snd* s, int type) {
    if (s->ncmd == s->capcmd) {
        int cap = s->capcmd ? s->capcmd * 2 : 32;
        px_cmd* g = (px_cmd*)realloc(s->cmds, sizeof(px_cmd) * (size_t)cap);
        if (!g) return NULL;
        s->cmds = g;
        s->capcmd = cap;
    }
    px_cmd* c = &s->cmds[s->ncmd++];
    memset(c, 0, sizeof(*c));
    c->type = (uint8_t)type;
    return c;
}

static bool push_seg(px_snd* s, uint32_t ticks, float level) {
    if (s->nseg == s->capseg) {
        int cap = s->capseg ? s->capseg * 2 : 8;
        px_env_seg* g = (px_env_seg*)realloc(s->segs, sizeof(px_env_seg) * (size_t)cap);
        if (!g) return false;
        s->segs = g;
        s->capseg = cap;
    }
    s->segs[s->nseg].ticks = ticks;
    s->segs[s->nseg].level = level;
    s->nseg++;
    return true;
}

#define PUSH(s, t, c) do { if (!((c) = push((s), (t)))) return false; } while (0)

uint32_t px_bpm_to_clocks_per_tick(uint32_t bpm) {
    uint64_t num = (uint64_t)PX_AUDIO_CLOCK_RATE * 60;
    uint64_t den = (uint64_t)bpm * TICKS_PER_QUARTER_NOTE;
    uint64_t v = (num + den / 2) / den;
    return v ? (uint32_t)v : 1;
}

// ---------------------------------------------------------------------------
// Classic sounds (sound.rs emit_commands)
// ---------------------------------------------------------------------------
static int cycled(const int* v, int n, int i, int def) {
    return n ? v[i % n] : def;
}

static bool contains(const int* v, int n, int x) {
    for (int i = 0; i < n; i++)
        if (v[i] == x) return true;
    return false;
}

static bool env_set(px_snd* s, uint32_t slot, float init, const px_env_seg* segs, int n) {
    px_cmd* c;
    PUSH(s, PX_CMD_ENVELOPE_SET, c);
    c->u = slot;
    c->f = init;
    c->seg = (uint32_t)s->nseg;
    c->nseg = (uint32_t)n;
    for (int i = 0; i < n; i++)
        if (!push_seg(s, segs[i].ticks, segs[i].level)) return false;
    return true;
}

bool px_snd_from_legacy(px_snd* s, const int* notes, int nn, const int* tones, int nt,
                        const int* vols, int nv, const int* fxs, int nf, int speed,
                        const int* tone_modes, int n_tones) {
    px_snd_init(s);
    s->empty = nn == 0;
    px_cmd* c;
    PUSH(s, PX_CMD_TEMPO, c);
    c->u = PX_AUDIO_CLOCK_RATE / SOUND_TICKS_PER_SECOND;
    PUSH(s, PX_CMD_QUANTIZE, c);
    c->f = 1.0f;
    PUSH(s, PX_CMD_TRANSPOSE, c);
    PUSH(s, PX_CMD_DETUNE, c);

    if (contains(fxs, nf, FX_FADEOUT)) {
        px_env_seg seg = { (uint32_t)speed, 0.0f };
        if (!env_set(s, 1, 1.0f, &seg, 1)) return false;
    }
    if (contains(fxs, nf, FX_HALF_FADEOUT)) {
        uint32_t fade = (uint32_t)roundf((float)speed / 2.0f);
        px_env_seg seg[2] = { { (uint32_t)speed - fade, 1.0f }, { fade, 0.0f } };
        if (!env_set(s, 2, 1.0f, seg, 2)) return false;
    }
    if (contains(fxs, nf, FX_QUARTER_FADEOUT)) {
        uint32_t fade = (uint32_t)roundf((float)speed / 4.0f);
        px_env_seg seg[2] = { { (uint32_t)speed - fade, 1.0f }, { fade, 0.0f } };
        if (!env_set(s, 3, 1.0f, seg, 2)) return false;
    }
    if (contains(fxs, nf, FX_VIBRATO)) {
        PUSH(s, PX_CMD_VIBRATO_SET, c);
        c->u = 1;
        c->ticks = 0;
        c->period = VIBRATO_PERIOD_TICKS;
        c->f = VIBRATO_DEPTH_CENTS / 100.0f;
    } else {
        PUSH(s, PX_CMD_VIBRATO, c);
        c->u = 0;
    }
    if (contains(fxs, nf, FX_SLIDE)) {
        PUSH(s, PX_CMD_GLIDE_SET, c);
        c->u = 1;
        c->f = NAN;
        c->ticks = PX_AUTO;
    } else {
        PUSH(s, PX_CMD_GLIDE, c);
        c->u = 0;
    }

    int last_tone = -1, last_vol = -1, last_fade = -1, last_vib = -1, last_slide = -1;
    for (int i = 0; i < nn; i++) {
        if (notes[i] < 0) {
            PUSH(s, PX_CMD_REST, c);
            c->ticks = (uint32_t)speed;
            continue;
        }
        int tone = cycled(tones, nt, i, 0);
        int vol = cycled(vols, nv, i, MAX_VOLUME);
        int fx = cycled(fxs, nf, i, FX_NONE);
        if (last_tone != tone) {
            last_tone = tone;
            PUSH(s, PX_CMD_TONE, c);
            c->u = (uint32_t)tone;
        }
        if (last_vol != vol) {
            last_vol = vol;
            PUSH(s, PX_CMD_VOLUME, c);
            c->f = (float)vol / MAX_VOLUME;
        }
        if (last_fade != fx) {
            last_fade = fx;
            PUSH(s, PX_CMD_ENVELOPE, c);
            c->u = fx == FX_FADEOUT ? 1 : fx == FX_HALF_FADEOUT ? 2 : fx == FX_QUARTER_FADEOUT ? 3 : 0;
        }
        if (last_vib != fx) {
            last_vib = fx;
            PUSH(s, PX_CMD_VIBRATO, c);
            c->u = fx == FX_VIBRATO;
        }
        if (last_slide != fx) {
            last_slide = fx;
            PUSH(s, PX_CMD_GLIDE, c);
            c->u = fx == FX_SLIDE;
        }
        int t = (tone >= 0 && tone < n_tones) ? tone : 0;
        int base = (n_tones == 0 || tone_modes[t] == 0) ? 36 : 60;
        PUSH(s, PX_CMD_NOTE, c);
        c->u = (uint32_t)(notes[i] + base);
        c->ticks = (uint32_t)speed;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MML (mml_parser.rs)
// ---------------------------------------------------------------------------
typedef struct {
    const char* p;
    size_t pos, len;
    char* err;
    size_t errlen;
} mml_t;

#define RANGE_ALL_MIN (-2147483647 - 1)
#define RANGE_ALL_MAX 2147483647

static bool mml_fail(mml_t* m, const char* fmt, const char* arg, long v) {
    char msg[96];
    if (arg) snprintf(msg, sizeof(msg), fmt, arg, v);
    else snprintf(msg, sizeof(msg), fmt, v);
    snprintf(m->err, m->errlen, "MML:%u: %s", (unsigned)m->pos, msg);
    return false;
}

static int peek(mml_t* m) {
    return m->pos < m->len ? (unsigned char)m->p[m->pos] : -1;
}

static void skip_ws(mml_t* m) {
    while (m->pos < m->len) {
        char c = m->p[m->pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') break;
        m->pos++;
    }
}

static int upper(int c) {
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

// Case-insensitive literal after whitespace; the position is kept on failure.
static bool parse_lit(mml_t* m, const char* lit) {
    skip_ws(m);
    size_t pos = m->pos;
    for (const char* q = lit; *q; q++) {
        int c = peek(m);
        if (c < 0 || upper(c) != upper(*q)) {
            m->pos = pos;
            return false;
        }
        m->pos++;
    }
    return true;
}

// 0 = parsed, 1 = missing, 2 = error (message set)
static int parse_num(mml_t* m, const char* name, long lo, long hi, long* out) {
    skip_ws(m);
    size_t pos = m->pos;
    bool neg = peek(m) == '-';
    if (neg) m->pos++;
    long long v = 0;
    bool digit = false;
    while (peek(m) >= '0' && peek(m) <= '9') {
        v = v * 10 + (peek(m) - '0');
        if (v > 2147483647LL) v = 2147483647LL;
        digit = true;
        m->pos++;
    }
    if (!digit) {
        m->pos = pos;
        return 1;
    }
    if (neg) v = -v;
    if (v < lo) { mml_fail(m, "'%s' is below minimum %ld", name, lo); return 2; }
    if (v > hi) { mml_fail(m, "'%s' exceeds maximum %ld", name, hi); return 2; }
    *out = (long)v;
    return 0;
}

static bool expect_num(mml_t* m, const char* name, long lo, long hi, long* out) {
    int r = parse_num(m, name, lo, hi, out);
    if (r == 1) return mml_fail(m, "Expected value for '%s'", name, 0);
    return r == 0;
}

static bool expect_lit(mml_t* m, const char* lit) {
    if (!parse_lit(m, lit)) return mml_fail(m, "Expected '%s'", lit, 0);
    return true;
}

// Returns -1 when the command is absent, 0 on error, 1 when parsed.
static int parse_cmd(mml_t* m, const char* name, long lo, long hi, long* out) {
    if (!parse_lit(m, name)) return -1;
    return expect_num(m, name, lo, hi, out) ? 1 : 0;
}

static bool parse_length(mml_t* m, uint32_t note_ticks, uint32_t* out) {
    const uint32_t whole = TICKS_PER_QUARTER_NOTE * 4;
    skip_ws(m);
    if (peek(m) >= '0' && peek(m) <= '9') {
        long len;
        if (!expect_num(m, "Note length", 1, 192, &len)) return false;
        if (whole % (uint32_t)len != 0) return mml_fail(m, "Invalid note length '%ld'", NULL, len);
        note_ticks = whole / (uint32_t)len;
    }
    uint32_t dot = note_ticks;
    while (parse_lit(m, ".")) {
        if (dot % 2 != 0) return mml_fail(m, "Cannot apply dot to odd note length", NULL, 0);
        dot /= 2;
        note_ticks += dot;
    }
    *out = note_ticks;
    return true;
}

static float vol_level(long v) { return (float)v / 127.0f; }

static bool mml_parse(px_snd* s, mml_t* m) {
    int octave = 4;
    uint32_t note_ticks = TICKS_PER_QUARTER_NOTE * 4 / 4;
    uint32_t quantize = 80;
    bool tempo_set = false, quant_set = false, tone_set = false, vol_set = false;
    bool transpose_set = false, detune_set = false, env_set_ = false, vib_set = false, gli_set = false;
    bool connected = false;
    uint32_t connected_note = 0;
    int last_note = -1;
    int depth = 0;
    long v;
    int r;
    px_cmd* c;

    for (;;) {
        skip_ws(m);
        if (peek(m) < 0) break;
        if ((r = parse_cmd(m, "T", 1, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            tempo_set = true;
            PUSH(s, PX_CMD_TEMPO, c);
            c->u = px_bpm_to_clocks_per_tick((uint32_t)v);
        } else if ((r = parse_cmd(m, "Q", 0, 100, &v)) >= 0) {
            if (!r) return false;
            quant_set = true;
            quantize = (uint32_t)v;
            PUSH(s, PX_CMD_QUANTIZE, c);
            c->f = (float)v / 100.0f;
        } else if ((r = parse_cmd(m, "V", 0, 127, &v)) >= 0) {
            if (!r) return false;
            vol_set = true;
            PUSH(s, PX_CMD_VOLUME, c);
            c->f = vol_level(v);
        } else if ((r = parse_cmd(m, "K", RANGE_ALL_MIN, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            transpose_set = true;
            PUSH(s, PX_CMD_TRANSPOSE, c);
            c->f = (float)v;
        } else if ((r = parse_cmd(m, "Y", RANGE_ALL_MIN, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            detune_set = true;
            PUSH(s, PX_CMD_DETUNE, c);
            c->f = (float)v / 100.0f;
        } else if ((r = parse_cmd(m, "@ENV", 0, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            env_set_ = true;
            uint32_t slot = (uint32_t)v;
            if (!parse_lit(m, "{")) {
                PUSH(s, PX_CMD_ENVELOPE, c);
                c->u = slot;
                continue;
            }
            if (slot == 0) return mml_fail(m, "Envelope slot 0 is reserved for disable", NULL, 0);
            long init;
            if (!expect_num(m, "init_vol", 0, 127, &init)) return false;
            uint32_t first = (uint32_t)s->nseg, n = 0;
            while (!parse_lit(m, "}")) {
                long dur, vol;
                if (!expect_lit(m, ",") || !expect_num(m, "dur_ticks", 0, RANGE_ALL_MAX, &dur) ||
                    !expect_lit(m, ",") || !expect_num(m, "vol", 0, 127, &vol))
                    return false;
                if (!push_seg(s, (uint32_t)dur, vol_level(vol))) return false;
                n++;
            }
            PUSH(s, PX_CMD_ENVELOPE_SET, c);
            c->u = slot;
            c->f = vol_level(init);
            c->seg = first;
            c->nseg = n;
        } else if ((r = parse_cmd(m, "@VIB", 0, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            vib_set = true;
            uint32_t slot = (uint32_t)v;
            if (!parse_lit(m, "{")) {
                PUSH(s, PX_CMD_VIBRATO, c);
                c->u = slot;
                continue;
            }
            if (slot == 0) return mml_fail(m, "Vibrato slot 0 is reserved for disable", NULL, 0);
            long delay, period, depth_c;
            if (!expect_num(m, "delay_ticks", 0, RANGE_ALL_MAX, &delay) || !expect_lit(m, ",") ||
                !expect_num(m, "period_ticks", 0, RANGE_ALL_MAX, &period) || !expect_lit(m, ",") ||
                !expect_num(m, "depth_cents", RANGE_ALL_MIN, RANGE_ALL_MAX, &depth_c) ||
                !expect_lit(m, "}"))
                return false;
            PUSH(s, PX_CMD_VIBRATO_SET, c);
            c->u = slot;
            c->ticks = (uint32_t)delay;
            c->period = (uint32_t)period;
            c->f = (float)depth_c / 100.0f;
        } else if ((r = parse_cmd(m, "@GLI", 0, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            gli_set = true;
            uint32_t slot = (uint32_t)v;
            if (!parse_lit(m, "{")) {
                PUSH(s, PX_CMD_GLIDE, c);
                c->u = slot;
                continue;
            }
            if (slot == 0) return mml_fail(m, "Glide slot 0 is reserved for disable", NULL, 0);
            float off = NAN;
            uint32_t dur = PX_AUTO;
            long x;
            if (!parse_lit(m, "*")) {
                if (!expect_num(m, "offset_cents", RANGE_ALL_MIN, RANGE_ALL_MAX, &x)) return false;
                off = (float)x / 100.0f;
            }
            if (!expect_lit(m, ",")) return false;
            if (!parse_lit(m, "*")) {
                if (!expect_num(m, "dur_ticks", 0, RANGE_ALL_MAX, &x)) return false;
                dur = (uint32_t)x;
            }
            if (!expect_lit(m, "}")) return false;
            PUSH(s, PX_CMD_GLIDE_SET, c);
            c->u = slot;
            c->f = off;
            c->ticks = dur;
        } else if ((r = parse_cmd(m, "@", 0, RANGE_ALL_MAX, &v)) >= 0) {
            if (!r) return false;
            tone_set = true;
            PUSH(s, PX_CMD_TONE, c);
            c->u = (uint32_t)v;
        } else if ((r = parse_cmd(m, "O", -1, 9, &v)) >= 0) {
            if (!r) return false;
            octave = (int)v;
        } else if (parse_lit(m, ">")) {
            if (octave >= 9) return mml_fail(m, "Octave exceeds maximum %ld", NULL, octave);
            octave++;
        } else if (parse_lit(m, "<")) {
            if (octave <= -1) return mml_fail(m, "Octave is below minimum %ld", NULL, octave);
            octave--;
        } else if (parse_lit(m, "L")) {
            if (!parse_length(m, note_ticks, &note_ticks)) return false;
        } else if (strchr("CDEFGABcdefgab", peek(m)) && peek(m) > 0) {
            // C/D/E/F/G/A/B[#+-][<len>][.][&]
            static const int SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
            int semitone = SEMI[upper(peek(m)) - 'A'];
            m->pos++;
            int midi = (octave + 1) * 12 + semitone;
            if (parse_lit(m, "#") || parse_lit(m, "+")) midi += 1;
            else if (parse_lit(m, "-")) midi = midi > 0 ? midi - 1 : 0;
            uint32_t dur;
            if (!parse_length(m, note_ticks, &dur)) return false;
            bool is_connected = false;
            while (parse_lit(m, "&")) {
                skip_ws(m);
                if (peek(m) >= '0' && peek(m) <= '9') {
                    uint32_t extra;
                    if (!parse_length(m, note_ticks, &extra)) return false;
                    dur += extra;
                } else {
                    is_connected = true;
                    break;
                }
            }
            // A tie to the same pitch lengthens the previous note.
            if (connected) {
                connected = false;
                if ((uint32_t)midi == connected_note) {
                    if (last_note >= 0) s->cmds[last_note].ticks += dur;
                    if (is_connected) connected = true;
                    continue;
                }
            }
            if (!tempo_set) { tempo_set = true; PUSH(s, PX_CMD_TEMPO, c); c->u = px_bpm_to_clocks_per_tick(120); }
            if (!quant_set) { quant_set = true; PUSH(s, PX_CMD_QUANTIZE, c); c->f = 0.8f; }
            if (!tone_set) { tone_set = true; PUSH(s, PX_CMD_TONE, c); c->u = 0; }
            if (!vol_set) { vol_set = true; PUSH(s, PX_CMD_VOLUME, c); c->f = vol_level(100); }
            if (!transpose_set) { transpose_set = true; PUSH(s, PX_CMD_TRANSPOSE, c); c->f = 0.0f; }
            if (!detune_set) { detune_set = true; PUSH(s, PX_CMD_DETUNE, c); c->f = 0.0f; }
            if (!env_set_) { env_set_ = true; PUSH(s, PX_CMD_ENVELOPE, c); c->u = 0; }
            if (!vib_set) { vib_set = true; PUSH(s, PX_CMD_VIBRATO, c); c->u = 0; }
            if (!gli_set) { gli_set = true; PUSH(s, PX_CMD_GLIDE, c); c->u = 0; }
            if (quantize != 100) {
                PUSH(s, PX_CMD_QUANTIZE, c);
                c->f = is_connected ? 1.0f : (float)quantize / 100.0f;
            }
            if (is_connected) {
                connected = true;
                connected_note = (uint32_t)midi;
            }
            last_note = s->ncmd;
            PUSH(s, PX_CMD_NOTE, c);
            c->u = (uint32_t)midi;
            c->ticks = dur;
        } else if (parse_lit(m, "R")) {
            uint32_t dur;
            if (!parse_length(m, note_ticks, &dur)) return false;
            while (parse_lit(m, "&")) {
                skip_ws(m);
                if (!(peek(m) >= '0' && peek(m) <= '9'))
                    return mml_fail(m, "Tie '&' after a rest requires a length", NULL, 0);
                uint32_t extra;
                if (!parse_length(m, note_ticks, &extra)) return false;
                dur += extra;
            }
            if (connected) return mml_fail(m, "Tie '&' is not followed by a note", NULL, 0);
            if (!tempo_set) { tempo_set = true; PUSH(s, PX_CMD_TEMPO, c); c->u = px_bpm_to_clocks_per_tick(120); }
            PUSH(s, PX_CMD_REST, c);
            c->ticks = dur;
            last_note = -1;
        } else if (parse_lit(m, "[")) {
            depth++;
            PUSH(s, PX_CMD_REPEAT_START, c);
        } else if (parse_lit(m, "]")) {
            if (depth == 0) return mml_fail(m, "Repeat end ']' has no matching '['", NULL, 0);
            depth--;
            long count = 0;
            r = parse_num(m, "count", 0, RANGE_ALL_MAX, &count);
            if (r == 2) return false;
            if (r == 1) count = 0;
            PUSH(s, PX_CMD_REPEAT_END, c);
            c->u = (uint32_t)count;
        } else {
            char ch[2] = { (char)peek(m), 0 };
            return mml_fail(m, "Unexpected character '%s'", ch, 0);
        }
    }
    if (connected) return mml_fail(m, "Tie '&' is not followed by a note", NULL, 0);
    if (depth > 0) return mml_fail(m, "Repeat start '[' has no matching ']'", NULL, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Old MML (old_mml_parser.rs): builds a classic sound, then emits commands.
// ---------------------------------------------------------------------------
typedef struct {
    int* v;
    int n, cap;
} ivec;

static bool ivec_push(ivec* a, int x) {
    if (a->n == a->cap) {
        int cap = a->cap ? a->cap * 2 : 64;
        int* g = (int*)realloc(a->v, sizeof(int) * (size_t)cap);
        if (!g) return false;
        a->v = g;
        a->cap = cap;
    }
    a->v[a->n++] = x;
    return true;
}

static bool ivec_repeat(ivec* a, int x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!ivec_push(a, x)) return false;
    return true;
}

typedef struct {
    uint32_t length, quantize;
    int tone;
    uint32_t env_start;
    int env[64];
    int nenv;
    bool vibrato;
    int note;
    bool tied;
} old_note;

typedef struct {
    ivec notes, tones, vols, fxs;
} old_sound;

static bool old_add_note(old_sound* o, const old_note* ni) {
    if (ni->length == 0) return true;
    if (!ivec_repeat(&o->tones, ni->tone, ni->length)) return false;
    for (uint32_t i = 0; i < ni->length; i++) {
        uint32_t k = ni->env_start + i;
        if (k > (uint32_t)(ni->nenv - 1)) k = (uint32_t)(ni->nenv - 1);
        if (!ivec_push(&o->vols, ni->env[k])) return false;
    }
    if (ni->note == -1) {
        return ivec_repeat(&o->notes, -1, ni->length) && ivec_repeat(&o->fxs, FX_NONE, ni->length);
    }
    uint32_t duration = ni->length * ni->quantize;
    uint32_t num_notes = duration / 8;
    int note_fx = ni->vibrato ? FX_VIBRATO : FX_NONE;
    if (!ivec_repeat(&o->notes, ni->note, num_notes) || !ivec_repeat(&o->fxs, note_fx, num_notes))
        return false;
    if (num_notes == ni->length) return true;
    if (!ivec_push(&o->notes, ni->note)) return false;
    int fx = num_notes > 0 ? FX_FADEOUT : duration >= 6 ? FX_QUARTER_FADEOUT
           : duration >= 4 ? FX_HALF_FADEOUT : FX_FADEOUT;
    if (!ivec_push(&o->fxs, fx)) return false;
    uint32_t num_rests = ni->length - num_notes - 1;
    return ivec_repeat(&o->notes, -1, num_rests) && ivec_repeat(&o->fxs, FX_NONE, num_rests);
}

static bool old_char(mml_t* m, char c) {
    skip_ws(m);
    if (peek(m) >= 0 && upper(peek(m)) == upper(c)) {
        m->pos++;
        return true;
    }
    return false;
}

// -1 absent, 0 error, 1 parsed
static int old_num(mml_t* m, uint32_t* out) {
    skip_ws(m);
    if (!(peek(m) >= '0' && peek(m) <= '9')) return -1;
    uint64_t v = 0;
    while (peek(m) >= '0' && peek(m) <= '9') {
        v = v * 10 + (uint64_t)(peek(m) - '0');
        if (v > 0xFFFFFFFFu) {
            snprintf(m->err, m->errlen, "Invalid number in MML");
            return 0;
        }
        m->pos++;
    }
    *out = (uint32_t)v;
    return 1;
}

static int old_cmd(mml_t* m, char c, uint32_t* out) {
    if (!old_char(m, c)) return -1;
    int r = old_num(m, out);
    if (r != 1) {
        if (r == -1) snprintf(m->err, m->errlen, "Missing value after '%c' in MML", c);
        return 0;
    }
    return 1;
}

static bool old_length(mml_t* m, uint32_t cur, uint32_t* out) {
    uint32_t len = cur, t;
    int r = old_num(m, &t);
    if (r == 0) return false;
    if (r == 1) {
        if (t >= 1 && t <= 32 && 32 % t == 0) {
            len = 32 / t;
        } else {
            snprintf(m->err, m->errlen, "Invalid note length '%u' in MML", (unsigned)t);
            return false;
        }
    }
    uint32_t target = len;
    while (old_char(m, '.')) {
        if (target < 2) {
            snprintf(m->err, m->errlen, "Length added by dot is too short in MML");
            return false;
        }
        target /= 2;
        len += target;
    }
    *out = len;
    return true;
}

static bool old_mml_parse(px_snd* s, mml_t* m, const int* tone_modes, int n_tones) {
    old_sound o;
    memset(&o, 0, sizeof(o));
    uint32_t length = 4, quantize = 7;
    int octave = 2, tone = 0;
    bool vol_is_env = false;
    int vol_const = 7, vol_env = 0;
    int envs[8][64];
    int nenvs[8];
    for (int i = 0; i < 8; i++) { envs[i][0] = 7; nenvs[i] = 1; }
    old_note ni;
    memset(&ni, 0, sizeof(ni));
    int speed = 9;
    bool ok = false;
    uint32_t v;
    int r;

    for (;;) {
        skip_ws(m);
        if (peek(m) < 0) break;
        if ((r = old_cmd(m, 't', &v)) >= 0) {
            if (!r) goto fail;
            if (v == 0) { snprintf(m->err, m->errlen, "Invalid tempo value '0' in MML"); goto fail; }
            speed = (int)(900 / v);
            if (speed < 1) speed = 1;
        } else if (old_char(m, 'l')) {
            if (!old_length(m, length, &length)) goto fail;
        } else if ((r = old_cmd(m, '@', &v)) >= 0) {
            if (!r) goto fail;
            if (v > 3) { snprintf(m->err, m->errlen, "Invalid tone value '%u' in MML", (unsigned)v); goto fail; }
            tone = (int)v;
        } else if ((r = old_cmd(m, 'o', &v)) >= 0) {
            if (!r) goto fail;
            if (v > 4) { snprintf(m->err, m->errlen, "Invalid octave value '%u' in MML", (unsigned)v); goto fail; }
            octave = (int)v;
        } else if (old_char(m, '>')) {
            if (octave >= 4) { snprintf(m->err, m->errlen, "Octave exceeded maximum in MML"); goto fail; }
            octave++;
        } else if (old_char(m, '<')) {
            if (octave <= 0) { snprintf(m->err, m->errlen, "Octave exceeded minimum in MML"); goto fail; }
            octave--;
        } else if ((r = old_cmd(m, 'q', &v)) >= 0) {
            if (!r) goto fail;
            if (v < 1 || v > 8) { snprintf(m->err, m->errlen, "Invalid quantize value '%u' in MML", (unsigned)v); goto fail; }
            quantize = v;
        } else if ((r = old_cmd(m, 'v', &v)) >= 0) {
            if (!r) goto fail;
            if (v > 7) { snprintf(m->err, m->errlen, "Invalid volume value '%u' in MML", (unsigned)v); goto fail; }
            vol_is_env = false;
            vol_const = (int)v;
        } else if ((r = old_cmd(m, 'x', &v)) >= 0) {
            if (!r) goto fail;
            if (v > 7) { snprintf(m->err, m->errlen, "Invalid envelope value '%u' in MML", (unsigned)v); goto fail; }
            vol_is_env = true;
            vol_env = (int)v;
            if (old_char(m, ':')) {
                int n = 0;
                skip_ws(m);
                while (peek(m) >= '0' && peek(m) <= '9') {
                    int vol = peek(m) - '0';
                    m->pos++;
                    if (vol > 7) { snprintf(m->err, m->errlen, "Invalid envelope volume '%d' in MML", vol); goto fail; }
                    if (n == 64) { snprintf(m->err, m->errlen, "Envelope longer than 64 steps in MML"); goto fail; }
                    envs[vol_env][n++] = vol;
                    skip_ws(m);
                }
                if (n == 0) { snprintf(m->err, m->errlen, "Missing envelope volumes in MML"); goto fail; }
                nenvs[vol_env] = n;
            }
        } else if (strchr("CDEFGABcdefgab", peek(m)) && peek(m) > 0) {
            static const int SEMI[7] = { 9, 11, 0, 2, 4, 5, 7 };
            int note = SEMI[upper(peek(m)) - 'A'];
            m->pos++;
            if (old_char(m, '#') || old_char(m, '+')) note++;
            else if (old_char(m, '-')) note--;
            uint32_t len;
            if (!old_length(m, length, &len)) goto fail;
            if (!old_add_note(&o, &ni)) goto oom;
            note += octave * 12;
            old_note next;
            memset(&next, 0, sizeof(next));
            if (vol_is_env) {
                memcpy(next.env, envs[vol_env], sizeof(int) * (size_t)nenvs[vol_env]);
                next.nenv = nenvs[vol_env];
            } else {
                next.env[0] = vol_const;
                next.nenv = 1;
            }
            next.env_start = (ni.tied && ni.note == note) ? ni.length + ni.env_start : 0;
            next.length = len;
            next.quantize = quantize;
            next.tone = tone;
            next.note = note;
            ni = next;
        } else if (old_char(m, 'r')) {
            uint32_t len;
            if (!old_length(m, length, &len)) goto fail;
            if (!old_add_note(&o, &ni)) goto oom;
            memset(&ni, 0, sizeof(ni));
            ni.length = len;
            ni.quantize = quantize;
            ni.tone = tone;
            ni.env[0] = 0;
            ni.nenv = 1;
            ni.note = -1;
        } else if (old_char(m, '~')) {
            ni.vibrato = true;
        } else if (old_char(m, '&')) {
            ni.quantize = 8;
            ni.tied = true;
        } else {
            snprintf(m->err, m->errlen, "Invalid command '%c' in MML", (char)peek(m));
            goto fail;
        }
    }
    if (!old_add_note(&o, &ni)) goto oom;
    ok = px_snd_from_legacy(s, o.notes.v, o.notes.n, o.tones.v, o.tones.n, o.vols.v, o.vols.n,
                            o.fxs.v, o.fxs.n, speed, tone_modes, n_tones);
    if (!ok) snprintf(m->err, m->errlen, "out of memory");
    goto done;
oom:
    snprintf(m->err, m->errlen, "out of memory");
fail:
    ok = false;
done:
    free(o.notes.v);
    free(o.tones.v);
    free(o.vols.v);
    free(o.fxs.v);
    return ok;
}

bool px_snd_from_mml(px_snd* s, const char* code, bool force_old, const int* tone_modes,
                     int n_tones, char* err, size_t errlen) {
    px_snd_init(s);
    mml_t m = { code, 0, strlen(code), err, errlen };
    bool old = force_old || strchr(code, 'x') || strchr(code, 'X') || strchr(code, '~');
    bool ok = old ? old_mml_parse(s, &m, tone_modes, n_tones) : mml_parse(s, &m);
    if (!ok) {
        if (!err[0]) snprintf(err, errlen, "out of memory");
        px_snd_free(s);
        return false;
    }
    // Upstream plays an MML sound unless it produced no commands at all.
    s->empty = !old && s->ncmd == 0;
    return true;
}

void px_snd_from_pcm(px_snd* s, int handle) {
    px_snd_init(s);
    s->pcm = handle;
    px_pcm_ref(handle);
    s->empty = false;
}

// ---------------------------------------------------------------------------
// Duration (mml_parser.rs DurationTransform): clocks = ticks before any tempo
// scaled at the default tempo + clocks accumulated after tempo changes.
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t ticks, fixed;
    uint32_t tempo;     // 0 = none
} dur_t;

static bool add_ov(uint64_t a, uint64_t b, uint64_t* out) {
    *out = a + b;
    return *out >= a;
}

static bool mul_ov(uint64_t a, uint64_t b, uint64_t* out) {
    if (a && b > UINT64_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool dur_then(dur_t* self, const dur_t* next) {
    uint64_t ticks, next_fixed = 0;
    if (self->tempo) {
        ticks = self->ticks;
        if (!mul_ov(next->ticks, self->tempo, &next_fixed)) return false;
    } else if (!add_ov(self->ticks, next->ticks, &ticks)) {
        return false;
    }
    uint64_t fixed;
    if (!add_ov(self->fixed, next_fixed, &fixed) || !add_ov(fixed, next->fixed, &fixed)) return false;
    self->ticks = ticks;
    self->fixed = fixed;
    if (next->tempo) self->tempo = next->tempo;
    return true;
}

static bool dur_repeated(dur_t* d, uint32_t count) {
    if (count == 0) return false;
    if (d->tempo) {
        uint64_t per, rep;
        if (!mul_ov(d->ticks, d->tempo, &per) || !add_ov(per, d->fixed, &per) ||
            !mul_ov(per, count - 1, &rep) || !add_ov(d->fixed, rep, &d->fixed))
            return false;
    } else {
        if (!mul_ov(d->ticks, count, &d->ticks) || !mul_ov(d->fixed, count, &d->fixed)) return false;
    }
    return true;
}

bool px_snd_total_clocks(const px_snd* s, uint64_t* clocks) {
    if (s->pcm >= 0) {
        uint32_t n = 0;
        px_pcm_samples(s->pcm, &n);
        *clocks = (uint64_t)n * PX_AUDIO_CLOCK_RATE / PX_AUDIO_RATE;
        return true;
    }
    int depth = 0, cap = 8;
    dur_t* stack = (dur_t*)calloc((size_t)cap, sizeof(dur_t));
    if (!stack) return false;
    bool ok = true;
    for (int i = 0; ok && i < s->ncmd; i++) {
        const px_cmd* c = &s->cmds[i];
        dur_t next = { 0, 0, 0 };
        switch (c->type) {
        case PX_CMD_TEMPO:
            next.tempo = c->u;
            break;
        case PX_CMD_NOTE:
        case PX_CMD_REST:
            next.ticks = c->ticks;
            break;
        case PX_CMD_REPEAT_START:
            if (depth + 1 == cap) {
                cap *= 2;
                dur_t* g = (dur_t*)realloc(stack, sizeof(dur_t) * (size_t)cap);
                if (!g) { ok = false; continue; }
                stack = g;
            }
            depth++;
            memset(&stack[depth], 0, sizeof(dur_t));
            continue;
        case PX_CMD_REPEAT_END:
            if (depth == 0) { ok = false; continue; }
            next = stack[depth--];
            if (!dur_repeated(&next, c->u)) { ok = false; continue; }
            break;
        default:
            break;
        }
        ok = dur_then(&stack[depth], &next);
    }
    if (ok && depth == 0) {
        uint64_t t;
        ok = mul_ov(stack[0].ticks, px_bpm_to_clocks_per_tick(120), &t) && add_ov(t, stack[0].fixed, clocks);
    } else {
        ok = false;
    }
    free(stack);
    return ok;
}

// ---------------------------------------------------------------------------
// PCM (pcm_decoder.rs): WAV -> mono f32 -> linear resample -> i16.
// ---------------------------------------------------------------------------
typedef struct {
    int16_t* data;
    uint32_t n;
    int refs;
} pcm_t;

static pcm_t* s_pcm;
static int s_npcm;

static uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t f32_to_i16(float x) {
    if (x >= 1.0f) return 32767;
    if (x <= -1.0f) return -32768;
    return (int16_t)(x * 32767.0f);
}

int px_pcm_load(const char* path, char* err, size_t errlen) {
    size_t n = 0;
    unsigned char* f = (unsigned char*)px_vfs_read(path, &n);
    if (!f) { snprintf(err, errlen, "Failed to open file '%s'", path); return -1; }
    int handle = -1;
    float* mono = NULL;
    int16_t* out = NULL;
    if (n < 12 || memcmp(f, "RIFF", 4) != 0 || memcmp(f + 8, "WAVE", 4) != 0) {
        snprintf(err, errlen, n >= 4 && memcmp(f, "OggS", 4) == 0
                 ? "OGG audio is not supported yet ('%s')" : "Failed to probe file '%s'", path);
        goto done;
    }
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* data = NULL;
    uint32_t data_len = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = le32(f + p + 4);
        const unsigned char* body = f + p + 8;
        size_t avail = n - (p + 8);
        if (memcmp(f + p, "fmt ", 4) == 0 && len >= 16 && avail >= 16) {
            fmt = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (fmt == 0xFFFE && len >= 26 && avail >= 26) fmt = le16(body + 24);
        } else if (memcmp(f + p, "data", 4) == 0) {
            data = body;
            data_len = (uint32_t)(len < avail ? len : avail);
        }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!data || !channels || !rate) {
        snprintf(err, errlen, "No audio track found in file '%s'", path);
        goto done;
    }
    bool is_float = fmt == 3;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (is_float && bits == 32))) {
        snprintf(err, errlen, "Unsupported WAV format in '%s'", path);
        goto done;
    }
    uint32_t frame = (uint32_t)channels * (bits / 8);
    uint32_t frames = data_len / frame;
    if (frames == 0) {
        snprintf(err, errlen, "No audio data found in file '%s'", path);
        goto done;
    }
    mono = (float*)malloc(sizeof(float) * frames);
    if (!mono) { snprintf(err, errlen, "out of memory"); goto done; }
    for (uint32_t i = 0; i < frames; i++) {
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; c++) {
            const unsigned char* q = data + (size_t)i * frame + (size_t)c * (bits / 8);
            float v;
            if (is_float) {
                uint32_t u = le32(q);
                memcpy(&v, &u, 4);
            } else if (bits == 8) {
                v = ((float)q[0] - 128.0f) / 128.0f;
            } else if (bits == 16) {
                v = (float)(int16_t)le16(q) / 32768.0f;
            } else if (bits == 24) {
                int32_t x = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8;
                v = (float)x / 8388608.0f;
            } else {
                v = (float)(int32_t)le32(q) / 2147483648.0f;
            }
            sum += v;
        }
        mono[i] = channels == 1 ? sum : sum / (float)channels;
    }
    uint32_t out_n = frames;
    if (rate != PX_AUDIO_RATE) {
        double ratio = (double)rate / (double)PX_AUDIO_RATE;
        out_n = (uint32_t)ceil((double)frames / ratio);
    }
    out = (int16_t*)malloc(sizeof(int16_t) * (out_n ? out_n : 1));
    if (!out) { snprintf(err, errlen, "out of memory"); goto done; }
    if (rate == PX_AUDIO_RATE) {
        for (uint32_t i = 0; i < out_n; i++) out[i] = f32_to_i16(mono[i]);
    } else {
        double ratio = (double)rate / (double)PX_AUDIO_RATE;
        for (uint32_t i = 0; i < out_n; i++) {
            double pos = (double)i * ratio;
            uint32_t idx = (uint32_t)pos;
            float frac = (float)(pos - idx);
            float s0 = idx < frames ? mono[idx] : 0.0f;
            float s1 = idx + 1 < frames ? mono[idx + 1] : s0;
            out[i] = f32_to_i16(s0 + (s1 - s0) * frac);
        }
    }
    for (int i = 0; i < s_npcm; i++) {
        if (!s_pcm[i].data) { handle = i; break; }
    }
    if (handle < 0) {
        pcm_t* g = (pcm_t*)realloc(s_pcm, sizeof(pcm_t) * (size_t)(s_npcm + 1));
        if (!g) { snprintf(err, errlen, "out of memory"); goto done; }
        s_pcm = g;
        handle = s_npcm++;
    }
    s_pcm[handle].data = out;
    s_pcm[handle].n = out_n;
    s_pcm[handle].refs = 1;
    out = NULL;
done:
    free(f);
    free(mono);
    free(out);
    return handle;
}

void px_pcm_ref(int h) {
    if (h >= 0 && h < s_npcm && s_pcm[h].data) s_pcm[h].refs++;
}

void px_pcm_unref(int h) {
    if (h < 0 || h >= s_npcm || !s_pcm[h].data) return;
    if (--s_pcm[h].refs <= 0) {
        free(s_pcm[h].data);
        s_pcm[h].data = NULL;
        s_pcm[h].n = 0;
    }
}

const int16_t* px_pcm_samples(int h, uint32_t* n) {
    if (h < 0 || h >= s_npcm || !s_pcm[h].data) {
        *n = 0;
        return NULL;
    }
    *n = s_pcm[h].n;
    return s_pcm[h].data;
}

void px_pcm_shutdown(void) {
    for (int i = 0; i < s_npcm; i++) free(s_pcm[i].data);
    free(s_pcm);
    s_pcm = NULL;
    s_npcm = 0;
}
