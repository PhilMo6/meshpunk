// Resource files: .pyxres (a ZIP holding pyxel_resource.toml), .pyxpal and
// PNG images. Semantics follow pyxel-core resource_data.rs / image.rs (MIT,
// Takashi Kitao) and docs/pyxres-format.md.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"
#include "lodepng.h"

// ---------------------------------------------------------------------------
// Growable int arrays (1 or 2 levels)
// ---------------------------------------------------------------------------
typedef struct {
    px_intarr a;
    int cap, row_cap;
    bool present;
} arrbuf;

static void arr_free(arrbuf* b) {
    free(b->a.v);
    free(b->a.row_start);
    free(b->a.row_len);
    memset(b, 0, sizeof(*b));
}

static bool arr_push(arrbuf* b, int32_t v) {
    if (b->a.n == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 64;
        int32_t* g = (int32_t*)realloc(b->a.v, sizeof(int32_t) * (size_t)ncap);
        if (!g) return false;
        b->a.v = g;
        b->cap = ncap;
    }
    b->a.v[b->a.n++] = v;
    return true;
}

static bool arr_row_begin(arrbuf* b) {
    if (b->a.rows == b->row_cap) {
        int ncap = b->row_cap ? b->row_cap * 2 : 64;
        int* s = (int*)realloc(b->a.row_start, sizeof(int) * (size_t)ncap);
        if (!s) return false;
        b->a.row_start = s;
        int* l = (int*)realloc(b->a.row_len, sizeof(int) * (size_t)ncap);
        if (!l) return false;
        b->a.row_len = l;
        b->row_cap = ncap;
    }
    b->a.row_start[b->a.rows] = b->a.n;
    b->a.row_len[b->a.rows] = 0;
    b->a.rows++;
    return true;
}

// ---------------------------------------------------------------------------
// TOML subset: [[array-of-tables]] headers, bare keys, integers and nested
// integer arrays. Anything else in a value position is skipped.
// ---------------------------------------------------------------------------
typedef struct {
    const char* p;
    const char* end;
    char* err;
    size_t errlen;
} tparser;

static void skip_ws(tparser* t, bool newlines) {
    while (t->p < t->end) {
        char c = *t->p;
        if (c == ' ' || c == '\t' || c == '\r' || (newlines && c == '\n')) {
            t->p++;
        } else if (c == '#') {
            while (t->p < t->end && *t->p != '\n') t->p++;
        } else {
            break;
        }
    }
}

static bool parse_int(tparser* t, int32_t* out) {
    const char* s = t->p;
    bool neg = false;
    if (s < t->end && (*s == '-' || *s == '+')) { neg = *s == '-'; s++; }
    if (s >= t->end || *s < '0' || *s > '9') return false;
    int32_t v = 0;
    while (s < t->end && ((*s >= '0' && *s <= '9') || *s == '_')) {
        if (*s != '_') v = v * 10 + (*s - '0');
        s++;
    }
    t->p = s;
    *out = neg ? -v : v;
    return true;
}

// Skip one value of any kind (strings, floats, bools, tables...).
static void skip_value(tparser* t) {
    int depth = 0;
    while (t->p < t->end) {
        char c = *t->p;
        if (c == '"') {
            t->p++;
            while (t->p < t->end && *t->p != '"') { if (*t->p == '\\') t->p++; t->p++; }
            t->p++;
            continue;
        }
        if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') { depth--; if (depth <= 0) { t->p++; return; } }
        else if (c == '\n' && depth == 0) return;
        t->p++;
    }
}

// value := int | '[' ints ']' | '[' '[' ints ']' , ... ']'
static bool parse_array(tparser* t, arrbuf* b) {
    t->p++;                                   // '['
    b->present = true;
    for (;;) {
        skip_ws(t, true);
        if (t->p >= t->end) { snprintf(t->err, t->errlen, "unterminated array"); return false; }
        if (*t->p == ']') { t->p++; return true; }
        if (*t->p == '[') {
            t->p++;
            if (!arr_row_begin(b)) { snprintf(t->err, t->errlen, "out of memory"); return false; }
            for (;;) {
                skip_ws(t, true);
                if (t->p >= t->end) { snprintf(t->err, t->errlen, "unterminated array"); return false; }
                if (*t->p == ']') { t->p++; break; }
                int32_t v;
                if (!parse_int(t, &v)) { snprintf(t->err, t->errlen, "bad integer in array"); return false; }
                if (!arr_push(b, v)) { snprintf(t->err, t->errlen, "out of memory"); return false; }
                b->a.row_len[b->a.rows - 1]++;
                skip_ws(t, true);
                if (t->p < t->end && *t->p == ',') t->p++;
            }
        } else {
            int32_t v;
            if (!parse_int(t, &v)) { snprintf(t->err, t->errlen, "bad integer in array"); return false; }
            if (!arr_push(b, v)) { snprintf(t->err, t->errlen, "out of memory"); return false; }
        }
        skip_ws(t, true);
        if (t->p < t->end && *t->p == ',') t->p++;
    }
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------
enum { SEC_NONE, SEC_IMAGES, SEC_TILEMAPS, SEC_SOUNDS, SEC_MUSICS, SEC_OTHER };

typedef struct {
    int kind;
    int index;
    int32_t width, height, imgsrc, speed;
    bool has_width, has_height;
    arrbuf data, notes, tones, volumes, effects, seqs;
} section;

typedef struct {
    bool ex_img, ex_tm, ex_snd, ex_mus;
    const px_res_sink* sink;
    int counts[5];
    char* err;
    size_t errlen;
} loadctx;

static void section_reset(section* s) {
    arr_free(&s->data);
    arr_free(&s->notes);
    arr_free(&s->tones);
    arr_free(&s->volumes);
    arr_free(&s->effects);
    arr_free(&s->seqs);
    memset(s, 0, sizeof(*s));
    s->speed = 30;
}

// Value at (row, col) with upstream's trailing-repeat expansion (expand_vec2).
static int32_t expand_at(const px_intarr* a, int row, int col) {
    int r = row < a->rows ? row : a->rows - 1;
    int len = a->row_len[r];
    int c = col < len ? col : len - 1;
    return a->v[a->row_start[r] + c];
}

static bool validate_grid(const char* bank, int index, int w, int h, const px_intarr* a,
                          int per_cell, char* err, size_t errlen) {
    if (w <= 0 || h <= 0) {
        snprintf(err, errlen, "%s[%d] dimensions must be greater than 0", bank, index);
        return false;
    }
    if (a->rows == 0) {
        snprintf(err, errlen, "%s[%d].data must not be empty", bank, index);
        return false;
    }
    if (a->rows > h) {
        snprintf(err, errlen, "%s[%d].data has %d rows, maximum is %d", bank, index, a->rows, h);
        return false;
    }
    for (int r = 0; r < a->rows; r++) {
        if (a->row_len[r] == 0) {
            snprintf(err, errlen, "%s[%d].data[%d] must not be empty", bank, index, r);
            return false;
        }
        if (a->row_len[r] > w * per_cell) {
            snprintf(err, errlen, "%s[%d].data[%d] has %d values, maximum is %d",
                     bank, index, r, a->row_len[r], w * per_cell);
            return false;
        }
    }
    return true;
}

static bool apply_image(loadctx* c, section* s) {
    if (c->ex_img) return true;
    if (!s->has_width || !s->has_height) {
        snprintf(c->err, c->errlen, "images[%d] needs width and height", s->index);
        return false;
    }
    if (!validate_grid("images", s->index, s->width, s->height, &s->data.a, 1, c->err, c->errlen))
        return false;
    px_image* img = px_bank_image(s->index);
    if (!img) return true;                   // more banks in the file than this runtime has
    if (img->cv.w != s->width || img->cv.h != s->height) {
        uint8_t* data = (uint8_t*)calloc((size_t)s->width * (size_t)s->height, 1);
        if (!data) { snprintf(c->err, c->errlen, "out of memory"); return false; }
        free(img->cv.data);
        px_image_init(img, s->width, s->height, data);
    }
    uint8_t* d = (uint8_t*)img->cv.data;
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++)
            d[s->width * y + x] = (uint8_t)expand_at(&s->data.a, y, x);
    return true;
}

static bool apply_tilemap(loadctx* c, section* s) {
    if (c->ex_tm) return true;
    if (!s->has_width || !s->has_height) {
        snprintf(c->err, c->errlen, "tilemaps[%d] needs width and height", s->index);
        return false;
    }
    if (!validate_grid("tilemaps", s->index, s->width, s->height, &s->data.a, 2, c->err, c->errlen))
        return false;
    if (s->imgsrc < 0 || s->imgsrc >= PX_NUM_IMAGES) {
        snprintf(c->err, c->errlen, "tilemaps[%d].imgsrc %d is out of range 0..%d",
                 s->index, (int)s->imgsrc, PX_NUM_IMAGES);
        return false;
    }
    px_tilemap* tm = px_bank_tilemap(s->index);
    if (!tm) return true;
    if (tm->cv.w != s->width || tm->cv.h != s->height) {
        uint16_t* data = (uint16_t*)calloc((size_t)s->width * (size_t)s->height, sizeof(uint16_t));
        if (!data) { snprintf(c->err, c->errlen, "out of memory"); return false; }
        free(tm->cv.data);
        px_tilemap_init(tm, s->width, s->height, data, s->imgsrc);
    }
    tm->imgsrc = s->imgsrc;
    uint16_t* d = (uint16_t*)tm->cv.data;
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++)
            d[s->width * y + x] = PX_TILE(expand_at(&s->data.a, y, 2 * x),
                                          expand_at(&s->data.a, y, 2 * x + 1));
    return true;
}

static bool finish_section(loadctx* c, section* s) {
    bool ok = true;
    switch (s->kind) {
    case SEC_IMAGES: ok = apply_image(c, s); break;
    case SEC_TILEMAPS: ok = apply_tilemap(c, s); break;
    case SEC_SOUNDS:
        if (!c->ex_snd && c->sink && c->sink->sound) {
            if (s->speed <= 0) {
                snprintf(c->err, c->errlen, "sounds[%d].speed must be greater than 0", s->index);
                ok = false;
            } else {
                c->sink->sound(c->sink->ctx, s->index, &s->notes.a, &s->tones.a,
                               &s->volumes.a, &s->effects.a, s->speed);
            }
        }
        break;
    case SEC_MUSICS:
        if (!c->ex_mus && c->sink && c->sink->music)
            c->sink->music(c->sink->ctx, s->index, &s->seqs.a);
        break;
    default:
        break;
    }
    section_reset(s);
    return ok;
}

static arrbuf* section_array(section* s, const char* key, size_t klen) {
#define K(name) (klen == sizeof(name) - 1 && memcmp(key, name, klen) == 0)
    if (K("data")) return &s->data;
    if (K("notes")) return &s->notes;
    if (K("tones")) return &s->tones;
    if (K("volumes")) return &s->volumes;
    if (K("effects")) return &s->effects;
    if (K("seqs")) return &s->seqs;
    return NULL;
}

static bool parse_resource_toml(const char* text, size_t len, loadctx* c) {
    tparser t = { text, text + len, c->err, c->errlen };
    section s;
    memset(&s, 0, sizeof(s));
    section_reset(&s);
    bool ok = true;
    while (ok) {
        skip_ws(&t, true);
        if (t.p >= t.end) break;
        if (t.p + 1 < t.end && t.p[0] == '[' && t.p[1] == '[') {
            ok = finish_section(c, &s);
            if (!ok) break;
            t.p += 2;
            const char* name = t.p;
            while (t.p < t.end && *t.p != ']') t.p++;
            size_t n = (size_t)(t.p - name);
            while (t.p < t.end && *t.p == ']') t.p++;
            int kind = SEC_OTHER;
            if (n == 6 && memcmp(name, "images", 6) == 0) kind = SEC_IMAGES;
            else if (n == 8 && memcmp(name, "tilemaps", 8) == 0) kind = SEC_TILEMAPS;
            else if (n == 6 && memcmp(name, "sounds", 6) == 0) kind = SEC_SOUNDS;
            else if (n == 6 && memcmp(name, "musics", 6) == 0) kind = SEC_MUSICS;
            s.kind = kind;
            s.index = kind < SEC_OTHER ? c->counts[kind]++ : 0;
            continue;
        }
        if (*t.p == '[') {                      // plain [table]: not in the format
            while (t.p < t.end && *t.p != '\n') t.p++;
            continue;
        }
        const char* key = t.p;
        while (t.p < t.end && *t.p != '=' && *t.p != ' ' && *t.p != '\t' && *t.p != '\n') t.p++;
        size_t klen = (size_t)(t.p - key);
        skip_ws(&t, false);
        if (t.p >= t.end || *t.p != '=') {
            snprintf(c->err, c->errlen, "expected '=' after key");
            ok = false;
            break;
        }
        t.p++;
        skip_ws(&t, false);
        arrbuf* arr = section_array(&s, key, klen);
        if (arr && t.p < t.end && *t.p == '[') {
            ok = parse_array(&t, arr);
            continue;
        }
        int32_t v;
        if (t.p < t.end && parse_int(&t, &v)) {
            if (K("width")) { s.width = v; s.has_width = true; }
            else if (K("height")) { s.height = v; s.has_height = true; }
            else if (K("imgsrc")) s.imgsrc = v;
            else if (K("speed")) s.speed = v;
            else if (K("format_version") && v > 4) {
                snprintf(c->err, c->errlen, "resource format_version %d is newer than 4", (int)v);
                ok = false;
            }
        } else {
            skip_value(&t);
        }
    }
    if (ok) ok = finish_section(c, &s);
    else section_reset(&s);
    return ok;
}
#undef K

// ---------------------------------------------------------------------------
// Pre-2.0 .pyxres (old_resource_data.rs): text files under pyxel_resource/.
// Every bank is parsed twice, first only checking and then applying, so a
// file with an error changes nothing (upstream stages all banks before
// committing any).
// ---------------------------------------------------------------------------
#define OLD_DIR             "pyxel_resource/"
#define OLD_CURRENT_VERSION 20909u          // settings.rs VERSION "2.9.9"

typedef struct {
    const char* want;
    unsigned char* data;
    size_t size;
} oldfind;

static bool old_find(void* ctx, const px_zip_file* f, char* err, size_t errlen) {
    oldfind* o = (oldfind*)ctx;
    size_t n = strlen(o->want);
    if (o->data || (size_t)f->name_len != n || memcmp(f->name, o->want, n) != 0) return true;
    o->data = px_zip_file_contents(f, &o->size, err, errlen);
    return o->data != NULL;
}

// str::lines(): lines split at "\n", a "\r" before the "\n" dropped.
typedef struct {
    const char* p;
    const char* end;
} lineiter;

static bool next_line(lineiter* it, const char** s, size_t* n) {
    if (it->p >= it->end) return false;
    const char* nl = (const char*)memchr(it->p, '\n', (size_t)(it->end - it->p));
    *s = it->p;
    *n = (size_t)((nl ? nl : it->end) - it->p);
    if (nl && *n && (*s)[*n - 1] == '\r') (*n)--;
    it->p = nl ? nl + 1 : it->end;
    return true;
}

static int count_lines(const char* text, size_t size) {
    lineiter it = { text, text + size };
    const char* s;
    size_t n;
    int count = 0;
    while (next_line(&it, &s, &n)) count++;
    return count;
}

static int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// parse_hex_group: `width` hex digits as one value; col = 1-based column of the first.
static bool hex_group(const char* s, int width, const char* entry, int line, int col, uint32_t* out,
                      char* err, size_t errlen) {
    uint32_t v = 0;
    for (int i = 0; i < width; i++) {
        int d = hex_val(s[i]);
        if (d < 0) {
            snprintf(err, errlen, "invalid hexadecimal digit '%c' in '%s' at line %d, column %d",
                     s[i], entry, line, col + i);
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

// parse_hex_values: a line of `width`-digit groups appended to `out` (to its
// last row when it has rows). signed8 stores each value as an i8.
static bool hex_values(const char* s, size_t n, int width, bool signed8, const char* entry, int line,
                       arrbuf* out, char* err, size_t errlen) {
    if (n % (size_t)width) {
        snprintf(err, errlen, "invalid value width in '%s' at line %d: expected groups of %d hexadecimal digits",
                 entry, line, width);
        return false;
    }
    for (size_t g = 0; g < n / (size_t)width; g++) {
        uint32_t v;
        if (!hex_group(s + g * (size_t)width, width, entry, line, (int)(g * (size_t)width) + 1, &v, err, errlen))
            return false;
        if (!arr_push(out, signed8 ? (int32_t)(int8_t)v : (int32_t)v)) {
            snprintf(err, errlen, "out of memory");
            return false;
        }
        if (out->a.rows) out->a.row_len[out->a.rows - 1]++;
    }
    return true;
}

// str::parse::<u32>/<u16>: an optional '+', then decimal digits only.
static bool parse_dec(const char* s, size_t n, uint32_t max, uint32_t* out) {
    size_t i = (n && s[0] == '+') ? 1 : 0;
    if (i == n) return false;
    uint64_t v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > max) return false;
    }
    *out = (uint32_t)v;
    return true;
}

// parse_version_string: whitespace dropped, "a.b.c" read as a*10000 + b*100
// + c, every part after the first one or two digits long.
static bool parse_version(const char* s, size_t n, uint32_t* out) {
    char buf[64];
    size_t k = 0;
    for (size_t i = 0; i < n && k < sizeof(buf) - 1; i++) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f') continue;
        buf[k++] = s[i];
    }
    uint64_t v = 0;
    size_t start = 0;
    for (int part = 0;; part++) {
        size_t end = start;
        while (end < k && buf[end] != '.') end++;
        uint32_t num;
        if (part > 0 && end - start != 1 && end - start != 2) return false;
        if (!parse_dec(buf + start, end - start, 0xFFFFFFFFu, &num)) return false;
        v = v * 100 + num;
        if (v > 0xFFFFFFFFu) return false;
        if (end == k) break;
        start = end + 1;
    }
    *out = (uint32_t)v;
    return true;
}

static bool old_image(int index, const char* text, size_t size, const char* entry, bool apply,
                      char* err, size_t errlen) {
    px_image* img = px_bank_image(index);
    int w = img->cv.w, h = img->cv.h;
    uint8_t* d = (uint8_t*)img->cv.data;
    lineiter it = { text, text + size };
    const char* s;
    size_t n;
    for (int y = 0; next_line(&it, &s, &n); y++) {
        if (y >= h) {
            snprintf(err, errlen, "too many image rows in '%s': got %d, maximum %d",
                     entry, count_lines(text, size), h);
            return false;
        }
        if ((int)n > w) {
            snprintf(err, errlen, "too many image columns in '%s' at line %d: got %d, maximum %d",
                     entry, y + 1, (int)n, w);
            return false;
        }
        for (size_t x = 0; x < n; x++) {
            int v = hex_val(s[x]);
            if (v < 0) {
                snprintf(err, errlen, "invalid hexadecimal digit '%c' in '%s' at line %d, column %d",
                         s[x], entry, y + 1, (int)x + 1);
                return false;
            }
            if (apply) d[(size_t)y * (size_t)w + x] = (uint8_t)v;
        }
    }
    return true;
}

static bool old_tilemap(int index, uint32_t version, const char* text, size_t size, const char* entry,
                        bool apply, char* err, size_t errlen) {
    px_tilemap* tm = apply ? px_bank_tilemap(index) : px.tilemaps[index];
    int w = tm->cv.w, h = tm->cv.h;
    uint16_t* d = (uint16_t*)tm->cv.data;
    int gw = version < 10500 ? 3 : 4;
    lineiter it = { text, text + size };
    const char* s;
    size_t n;
    for (int y = 0; next_line(&it, &s, &n); y++) {
        if (y < PX_TILEMAP_SIZE) {
            if (n % (size_t)gw) {
                snprintf(err, errlen, "invalid tile width in '%s' at line %d: expected groups of %d hexadecimal digits",
                         entry, y + 1, gw);
                return false;
            }
            int tiles = (int)(n / (size_t)gw);
            if (tiles > w) {
                snprintf(err, errlen, "too many tiles in '%s' at line %d: got %d, maximum %d",
                         entry, y + 1, tiles, w);
                return false;
            }
            for (int x = 0; x < tiles; x++) {
                uint32_t t;
                if (!hex_group(s + (size_t)x * (size_t)gw, gw, entry, y + 1, x * gw + 1, &t, err, errlen))
                    return false;
                uint32_t tx = version < 10500 ? t % 32 : (t >> 8) & 0xff;
                uint32_t ty = version < 10500 ? t / 32 : t & 0xff;
                if (apply && y < h) d[(size_t)y * (size_t)w + (size_t)x] = PX_TILE(tx, ty);
            }
        } else if (y == PX_TILEMAP_SIZE) {
            uint32_t src;
            if (!parse_dec(s, n, 0xFFFFFFFFu, &src)) {
                snprintf(err, errlen, "invalid decimal value '%.*s' in '%s' at line %d", (int)n, s, entry, y + 1);
                return false;
            }
            if (src >= PX_NUM_IMAGES) {
                snprintf(err, errlen, "image index %u in '%s' at line %d is out of range 0..%d",
                         (unsigned)src, entry, y + 1, PX_NUM_IMAGES);
                return false;
            }
            if (apply) tm->imgsrc = (int)src;
        } else {
            snprintf(err, errlen, "too many tilemap lines in '%s': got %d, maximum %d",
                     entry, count_lines(text, size), PX_TILEMAP_SIZE + 1);
            return false;
        }
    }
    return true;
}

// text == NULL: the entry is missing and the sound is cleared (speed 30).
static bool old_sound(int index, const char* text, size_t size, const char* entry, bool apply,
                      const px_res_sink* sink, char* err, size_t errlen) {
    arrbuf a[4];
    memset(a, 0, sizeof(a));
    uint32_t speed = 30;
    bool ok = true;
    lineiter it = { text, text + size };
    const char* s;
    size_t n;
    for (int i = 0; ok && text && next_line(&it, &s, &n); i++) {
        if (n == 4 && memcmp(s, "none", 4) == 0) continue;
        if (i <= 3) {
            ok = hex_values(s, n, i == 0 ? 2 : 1, i == 0, entry, i + 1, &a[i], err, errlen);
        } else if (i == 4) {
            if (!parse_dec(s, n, 65535, &speed)) {
                snprintf(err, errlen, "invalid decimal value '%.*s' in '%s' at line %d", (int)n, s, entry, i + 1);
                ok = false;
            } else if (speed == 0) {
                snprintf(err, errlen, "sound speed %u in '%s' at line %d is out of range 1..65536",
                         (unsigned)speed, entry, i + 1);
                ok = false;
            }
        }
    }
    if (ok && apply && sink && sink->sound)
        sink->sound(sink->ctx, index, &a[0].a, &a[1].a, &a[2].a, &a[3].a, (int)speed);
    for (int k = 0; k < 4; k++) arr_free(&a[k]);
    return ok;
}

// text == NULL: the entry is missing and the music is cleared (4 empty channels).
static bool old_music(int index, const char* text, size_t size, const char* entry, bool apply,
                      const px_res_sink* sink, char* err, size_t errlen) {
    arrbuf seqs;
    memset(&seqs, 0, sizeof(seqs));
    bool ok = true;
    int i = 0;
    lineiter it = { text, text + size };
    const char* s;
    size_t n;
    while (ok && text && next_line(&it, &s, &n)) {
        if (i >= 4) {
            snprintf(err, errlen, "too many music channels in '%s': got %d, maximum 4",
                     entry, count_lines(text, size));
            ok = false;
            break;
        }
        if (!arr_row_begin(&seqs)) { snprintf(err, errlen, "out of memory"); ok = false; break; }
        if (!(n == 4 && memcmp(s, "none", 4) == 0))
            ok = hex_values(s, n, 2, false, entry, i + 1, &seqs, err, errlen);
        i++;
    }
    for (; ok && i < 4; i++) {
        if (!arr_row_begin(&seqs)) { snprintf(err, errlen, "out of memory"); ok = false; }
    }
    if (ok && apply && sink && sink->music) sink->music(sink->ctx, index, &seqs.a);
    arr_free(&seqs);
    return ok;
}

static bool old_bank_pass(const unsigned char* z, size_t zn, uint32_t version, bool ex_img, bool ex_tm,
                          bool ex_snd, bool ex_mus, const px_res_sink* sink, bool apply,
                          char* err, size_t errlen) {
    char entry[48];
    for (int kind = 0; kind < 4; kind++) {
        bool excluded = kind == 0 ? ex_img : kind == 1 ? ex_tm : kind == 2 ? ex_snd : ex_mus;
        int count = kind == 0 ? PX_NUM_IMAGES : kind == 1 ? PX_NUM_TILEMAPS : kind == 2 ? 64 : 8;
        if (excluded) continue;
        for (int i = 0; i < count; i++) {
            if (kind == 0) snprintf(entry, sizeof(entry), OLD_DIR "image%d", i);
            else if (kind == 1) snprintf(entry, sizeof(entry), OLD_DIR "tilemap%d", i);
            else if (kind == 2) snprintf(entry, sizeof(entry), OLD_DIR "sound%02d", i);
            else snprintf(entry, sizeof(entry), OLD_DIR "music%d", i);
            oldfind o = { entry, NULL, 0 };
            if (!px_zip_walk(z, zn, old_find, &o, err, errlen)) return false;
            const char* text = (const char*)o.data;
            bool ok = true;
            if (kind == 0) {
                if (text) ok = old_image(i, text, o.size, entry, apply, err, errlen);
                else if (apply) px_clear_u8(&px_bank_image(i)->cv, 0);
            } else if (kind == 1) {
                if (text) ok = old_tilemap(i, version, text, o.size, entry, apply, err, errlen);
                else if (apply) px_clear_u16(&px_bank_tilemap(i)->cv, PX_TILE(0, 0));
            } else if (kind == 2) {
                ok = old_sound(i, text, o.size, entry, apply, sink, err, errlen);
            } else {
                ok = old_music(i, text, o.size, entry, apply, sink, err, errlen);
            }
            free(o.data);
            if (!ok) return false;
        }
    }
    return true;
}

static bool load_old_resource(const unsigned char* z, size_t zn, bool ex_img, bool ex_tm, bool ex_snd,
                              bool ex_mus, const px_res_sink* sink, char* err, size_t errlen) {
    oldfind o = { OLD_DIR "version", NULL, 0 };
    if (!px_zip_walk(z, zn, old_find, &o, err, errlen)) return false;
    if (!o.data) { snprintf(err, errlen, "failed to open '" OLD_DIR "version'"); return false; }
    uint32_t version;
    bool ok = parse_version((const char*)o.data, o.size, &version);
    if (!ok) snprintf(err, errlen, "invalid version '%.*s'", (int)o.size, (const char*)o.data);
    else if (version > OLD_CURRENT_VERSION) {
        snprintf(err, errlen, "unsupported version '%.*s'", (int)o.size, (const char*)o.data);
        ok = false;
    }
    free(o.data);
    if (!ok) return false;
    return old_bank_pass(z, zn, version, ex_img, ex_tm, ex_snd, ex_mus, sink, false, err, errlen) &&
           old_bank_pass(z, zn, version, ex_img, ex_tm, ex_snd, ex_mus, sink, true, err, errlen);
}

// ---------------------------------------------------------------------------
// .pyxres
// ---------------------------------------------------------------------------
typedef struct {
    char* toml;
    size_t size;
    bool legacy;
} findctx;

static bool find_toml(void* ctx, const px_zip_file* f, char* err, size_t errlen) {
    findctx* fc = (findctx*)ctx;
    const char* want = "pyxel_resource.toml";
    size_t n = strlen(want);
    if ((size_t)f->name_len == n && memcmp(f->name, want, n) == 0 && !fc->toml) {
        fc->toml = (char*)px_zip_file_contents(f, &fc->size, err, errlen);
        return fc->toml != NULL;
    }
    n = strlen(OLD_DIR "version");
    if ((size_t)f->name_len == n && memcmp(f->name, OLD_DIR "version", n) == 0) fc->legacy = true;
    return true;
}

bool px_res_load(const char* path, bool ex_img, bool ex_tm, bool ex_snd, bool ex_mus,
                 const px_res_sink* sink, char* err, size_t errlen) {
    size_t zn = 0;
    unsigned char* z = (unsigned char*)px_vfs_read(path, &zn);
    if (!z) { snprintf(err, errlen, "Failed to open file '%s'", path); return false; }
    findctx fc = { NULL, 0, false };
    bool ok = px_zip_walk(z, zn, find_toml, &fc, err, errlen);
    if (ok && fc.legacy) {
        // resource.rs load_resource: pyxel_resource/version marks the old format.
        free(fc.toml);
        printf("[pyxel] An old Pyxel resource file '%s' is loaded. Please re-save it with the latest Pyxel.\n", path);
        char detail[160];
        ok = load_old_resource(z, zn, ex_img, ex_tm, ex_snd, ex_mus, sink, detail, sizeof(detail));
        if (!ok) snprintf(err, errlen, "Failed to load legacy resource file '%s': %s", path, detail);
        free(z);
        return ok;
    }
    free(z);
    if (!ok) { free(fc.toml); return false; }
    if (!fc.toml) {
        snprintf(err, errlen, "'%s' has no pyxel_resource.toml", path);
        return false;
    }
    loadctx c;
    memset(&c, 0, sizeof(c));
    c.ex_img = ex_img;
    c.ex_tm = ex_tm;
    c.ex_snd = ex_snd;
    c.ex_mus = ex_mus;
    c.sink = sink;
    c.err = err;
    c.errlen = errlen;
    ok = parse_resource_toml(fc.toml, fc.size, &c);
    free(fc.toml);
    return ok;
}

int px_res_load_pal(const char* pyxres_path, uint32_t* colors, int max) {
    char base[292];
    snprintf(base, sizeof(base), "%s", pyxres_path);
    char* dot = strrchr(base, '.');
    char* slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    char pal_path[300];
    snprintf(pal_path, sizeof(pal_path), "%s.pyxpal", base);
    size_t n = 0;
    char* text = px_vfs_read(pal_path, &n);
    if (!text) return 0;
    int count = 0;
    const char* p = text;
    while (*p && count < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        uint32_t v = 0;
        int digits = 0;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            char ch = *p++;
            v = v * 16 + (uint32_t)(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10);
            digits++;
        }
        if (digits == 0) { while (*p && *p != '\n') p++; continue; }
        colors[count++] = v & 0xFFFFFF;
    }
    free(text);
    return count;
}

// ---------------------------------------------------------------------------
// .pyxres writing: TOML laid out as the toml crate serializes ResourceData
// ---------------------------------------------------------------------------
typedef struct {
    unsigned char* p;
    size_t n, cap;
    bool oom;
} outbuf;

static void out_bytes(outbuf* o, const void* s, size_t n) {
    if (o->oom) return;
    if (o->n + n > o->cap) {
        size_t ncap = o->cap ? o->cap : 4096;
        while (ncap < o->n + n) ncap *= 2;
        unsigned char* g = (unsigned char*)realloc(o->p, ncap);
        if (!g) { o->oom = true; return; }
        o->p = g;
        o->cap = ncap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

static void out_str(outbuf* o, const char* s) {
    out_bytes(o, s, strlen(s));
}

static void out_uint(outbuf* o, uint32_t v) {
    char b[10];
    int k = (int)sizeof(b);
    do {
        b[--k] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    out_bytes(o, b + k, sizeof(b) - (size_t)k);
}

// "data = [[...], ...]" after utils.rs compress_vec2: the rows after the last
// one that differs from the final row are dropped, and each row loses the
// values after the last one that differs from its final value. Tilemap rows
// are the (tx, ty) pairs flattened. data == NULL is an all-zero grid.
static void out_grid(outbuf* o, const void* data, int w, int h, bool tiles) {
    size_t row_bytes = (size_t)w * (tiles ? sizeof(uint16_t) : 1);
    int rows = 1;
    if (data) {
        const unsigned char* d = (const unsigned char*)data;
        const unsigned char* last = d + (size_t)(h - 1) * row_bytes;
        int r = h - 2;
        while (r >= 0 && memcmp(d + (size_t)r * row_bytes, last, row_bytes) == 0) r--;
        rows = r + 2;
    }
    uint16_t* row = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)w * 2);
    if (!row) { o->oom = true; return; }
    out_str(o, "data = [");
    for (int y = 0; y < rows; y++) {
        int n = 0;
        if (!data) {
            row[n++] = 0;
        } else if (tiles) {
            const uint16_t* t = (const uint16_t*)data + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                row[n++] = PX_TILE_X(t[x]);
                row[n++] = PX_TILE_Y(t[x]);
            }
        } else {
            const uint8_t* p = (const uint8_t*)data + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) row[n++] = p[x];
        }
        int k = n - 2;
        while (k >= 0 && row[k] == row[n - 1]) k--;
        n = k + 2;
        out_str(o, y ? ", [" : "[");
        for (int i = 0; i < n; i++) {
            if (i) out_str(o, ", ");
            out_uint(o, row[i]);
        }
        out_str(o, "]");
    }
    out_str(o, "]\n");
    free(row);
}

static void out_table_head(outbuf* o, const char* table, const px_canvas* cv) {
    out_str(o, "\n[[");
    out_str(o, table);
    out_str(o, "]]\nwidth = ");
    out_uint(o, (uint32_t)cv->w);
    out_str(o, "\nheight = ");
    out_uint(o, (uint32_t)cv->h);
    out_str(o, "\n");
}

static void put16(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// ZIP headers for one stored entry: version 2.0, time 00:00, date 1980-01-01.
static void zip_entry_header(unsigned char* p, bool central, uint32_t crc, uint32_t size,
                             const char* name, size_t name_len) {
    int o = central ? 2 : 0;          // the central header adds "version made by"
    put32(p, central ? 0x02014b50u : 0x04034b50u);
    if (central) put16(p + 4, 20);
    put16(p + 4 + o, 20);             // version needed to extract
    put16(p + 6 + o, 0);              // flags
    put16(p + 8 + o, 0);              // method: stored
    put16(p + 10 + o, 0);             // time
    put16(p + 12 + o, 0x21);          // date
    put32(p + 14 + o, crc);
    put32(p + 18 + o, size);          // compressed
    put32(p + 22 + o, size);          // uncompressed
    put16(p + 26 + o, (uint32_t)name_len);
    put16(p + 28 + o, 0);             // extra field length
    if (central) memset(p + 32, 0, 14);   // comment, disk, attributes, local header offset 0
    memcpy(p + (central ? 46 : 30), name, name_len);
}

unsigned char* px_res_build(px_image* const* images, int nimages, px_tilemap* const* tilemaps,
                            int ntilemaps, const char* sounds_toml, const char* musics_toml,
                            size_t* out_size) {
    static const char NAME[] = "pyxel_resource.toml";
    const size_t name_len = sizeof(NAME) - 1;
    const size_t local_len = 30 + name_len;
    const size_t central_len = 46 + name_len;
    unsigned char head[46 + sizeof(NAME) + 22];
    memset(head, 0, sizeof(head));

    outbuf o = { NULL, 0, 0, false };
    out_bytes(&o, head, local_len);   // local header, filled in once the CRC is known
    size_t start = o.n;
    out_str(&o, "format_version = 1\n");
    if (nimages == 0) out_str(&o, "images = []\n");
    if (ntilemaps == 0) out_str(&o, "tilemaps = []\n");
    if (!sounds_toml[0]) out_str(&o, "sounds = []\n");
    if (!musics_toml[0]) out_str(&o, "musics = []\n");
    for (int i = 0; i < nimages; i++) {
        out_table_head(&o, "images", &images[i]->cv);
        out_grid(&o, images[i]->cv.data, images[i]->cv.w, images[i]->cv.h, false);
    }
    for (int i = 0; i < ntilemaps; i++) {
        const px_tilemap* tm = tilemaps[i];
        out_table_head(&o, "tilemaps", &tm->cv);
        out_str(&o, "imgsrc = ");
        out_uint(&o, tm->imgsrc < 0 ? 0 : (uint32_t)tm->imgsrc);
        out_str(&o, "\n");
        out_grid(&o, tm->cv.data, tm->cv.w, tm->cv.h, true);
    }
    out_str(&o, sounds_toml);
    out_str(&o, musics_toml);
    if (o.oom) { free(o.p); return NULL; }

    size_t toml_len = o.n - start;
    uint32_t crc = lodepng_crc32(o.p + start, toml_len);
    zip_entry_header(head, true, crc, (uint32_t)toml_len, NAME, name_len);
    unsigned char* end = head + central_len;
    put32(end, 0x06054b50u);
    put16(end + 4, 0);                // this disk
    put16(end + 6, 0);                // disk with the central directory
    put16(end + 8, 1);                // entries on this disk
    put16(end + 10, 1);               // entries
    put32(end + 12, (uint32_t)central_len);
    put32(end + 16, (uint32_t)o.n);   // central directory offset
    put16(end + 20, 0);               // comment length
    out_bytes(&o, head, central_len + 22);
    if (o.oom) { free(o.p); return NULL; }
    zip_entry_header(o.p, false, crc, (uint32_t)toml_len, NAME, name_len);
    *out_size = o.n;
    return o.p;
}

// ---------------------------------------------------------------------------
// TMX (tmx_parser.rs)
// ---------------------------------------------------------------------------

// Start of the next element named `name` at or after p ("<name" followed by
// whitespace, '>' or '/'), or NULL.
static const char* xml_find(const char* p, const char* end, const char* name) {
    size_t n = strlen(name);
    for (; p + n + 1 < end; p++) {
        if (p[0] != '<' || memcmp(p + 1, name, n) != 0) continue;
        char c = p[1 + n];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') return p;
    }
    return NULL;
}

// Value of attribute `name` in the tag starting at tag, copied into out.
static bool xml_attr(const char* tag, const char* end, const char* name, char* out, size_t cap) {
    const char* close = tag;
    while (close < end && *close != '>') close++;
    size_t n = strlen(name);
    for (const char* p = tag + 1; p + n + 2 < close; p++) {
        if ((p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\r' && p[-1] != '\n') ||
            memcmp(p, name, n) != 0 || p[n] != '=')
            continue;
        char q = p[n + 1];
        if (q != '"' && q != '\'') continue;
        const char* v = p + n + 2;
        const char* ve = v;
        while (ve < close && *ve != q) ve++;
        size_t len = (size_t)(ve - v) < cap - 1 ? (size_t)(ve - v) : cap - 1;
        memcpy(out, v, len);
        out[len] = 0;
        return true;
    }
    return false;
}

static bool xml_attr_u32(const char* tag, const char* end, const char* name, uint32_t* out) {
    char buf[24];
    if (!xml_attr(tag, end, name, buf, sizeof(buf))) return false;
    char* e;
    unsigned long v = strtoul(buf, &e, 10);
    if (e == buf || *e) return false;
    *out = (uint32_t)v;
    return true;
}

uint16_t* px_tmx_load(const char* path, int layer, int* out_w, int* out_h, char* err, size_t errlen) {
    size_t n = 0;
    char* text = px_vfs_read(path, &n);
    if (!text) { snprintf(err, errlen, "Failed to open file '%s'", path); return NULL; }
    const char* end = text + n;
    uint16_t* tiles = NULL;

    uint32_t tw, th, firstgid, columns, w, h;
    const char* map = xml_find(text, end, "map");
    if (!map || !xml_attr_u32(map, end, "tilewidth", &tw) || !xml_attr_u32(map, end, "tileheight", &th)) {
        snprintf(err, errlen, "Failed to parse file '%s'", path);
        goto done;
    }
    if (tw != PX_TILE_SIZE || th != PX_TILE_SIZE) {
        snprintf(err, errlen, "Invalid tile size in file '%s'", path);
        goto done;
    }
    const char* ts = xml_find(map, end, "tileset");
    if (!ts || !xml_attr_u32(ts, end, "firstgid", &firstgid)) {
        snprintf(err, errlen, "No tileset found in file '%s'", path);
        goto done;
    }
    if (!xml_attr_u32(ts, end, "columns", &columns) || columns == 0) {
        snprintf(err, errlen, "No embedded tileset in file '%s'", path);
        goto done;
    }
    const char* ly = map;
    for (int i = 0; i <= layer; i++) {
        ly = xml_find(ly + 1, end, "layer");
        if (!ly) break;
    }
    if (layer < 0 || !ly) {
        snprintf(err, errlen, "Layer %d not found in file '%s'", layer, path);
        goto done;
    }
    const char* data = xml_find(ly, end, "data");
    char enc[16];
    if (!xml_attr_u32(ly, end, "width", &w) || !xml_attr_u32(ly, end, "height", &h) || !data ||
        !xml_attr(data, end, "encoding", enc, sizeof(enc))) {
        snprintf(err, errlen, "Failed to parse file '%s'", path);
        goto done;
    }
    if (strcmp(enc, "csv") != 0) {
        snprintf(err, errlen, "Unsupported encoding in file '%s'", path);
        goto done;
    }
    if (w == 0 || h == 0 || (uint64_t)w * h > 16u * 1024 * 1024) {
        snprintf(err, errlen, "Layer dimensions are too large in file '%s'", path);
        goto done;
    }
    tiles = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
    if (!tiles) { snprintf(err, errlen, "out of memory"); goto done; }
    const char* p = data;
    while (p < end && *p != '>') p++;
    p++;
    size_t count = 0;
    while (p < end && *p != '<') {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (p >= end || *p == '<') break;
        char* e;
        unsigned long gid = strtoul(p, &e, 10);
        if (e == p) {
            snprintf(err, errlen, "Failed to parse file '%s'", path);
            free(tiles);
            tiles = NULL;
            goto done;
        }
        p = e;
        uint32_t id = (uint32_t)gid & 0x0FFFFFFFu;
        id = id > firstgid ? id - firstgid : 0;
        uint32_t tx = id % columns, ty = id / columns;
        if (tx > 255 || ty > 255) {
            snprintf(err, errlen, "Tile (%u, %u) in '%s' is beyond 255", (unsigned)tx, (unsigned)ty, path);
            free(tiles);
            tiles = NULL;
            goto done;
        }
        if (count < (size_t)w * h) tiles[count] = PX_TILE(tx, ty);
        count++;
    }
    *out_w = (int)w;
    *out_h = (int)h;
done:
    free(text);
    return tiles;
}

// ---------------------------------------------------------------------------
// PNG (image.rs Image::from_image; GIF/JPEG are not decoded here)
// ---------------------------------------------------------------------------
px_image* px_image_decode_png(const char* path, bool include_colors, char* err, size_t errlen) {
    size_t n = 0;
    unsigned char* file = (unsigned char*)px_vfs_read(path, &n);
    if (!file) { snprintf(err, errlen, "Failed to open file '%s'", path); return NULL; }
    unsigned char* rgba = NULL;
    unsigned w = 0, h = 0;
    unsigned e = lodepng_decode32(&rgba, &w, &h, file, n);
    free(file);
    if (e) {
        snprintf(err, errlen, "Failed to decode '%s': %s", path, lodepng_error_text(e));
        return NULL;
    }
    px_image* img = px_image_new((int)w, (int)h);
    if (!img) { free(rgba); snprintf(err, errlen, "out of memory"); return NULL; }
    uint8_t* d = (uint8_t*)img->cv.data;
    // include_colors: the file's own colors become the palette, in order of
    // first appearance. Otherwise each color maps to the nearest palette entry.
    uint32_t extracted[PX_MAX_COLORS];
    int n_extracted = 0;
    uint32_t last_rgb = 0xFFFFFFFF;
    uint8_t last_idx = 0;
    for (unsigned i = 0; i < w * h; i++) {
        const unsigned char* p = rgba + 4 * i;
        uint32_t rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        if (rgb != last_rgb) {
            if (include_colors) {
                int k = 0;
                while (k < n_extracted && extracted[k] != rgb) k++;
                if (k == n_extracted) {
                    if (n_extracted == PX_MAX_COLORS) {
                        snprintf(err, errlen, "Number of colors must be between 1 and %d", PX_MAX_COLORS);
                        free(rgba);
                        px_image_free(img);
                        return NULL;
                    }
                    extracted[n_extracted++] = rgb;
                }
                last_idx = (uint8_t)k;
            } else {
                float best = 3.4e38f;
                for (int k = 0; k < px.num_colors; k++) {
                    float dr = (float)p[0] - (float)((px.colors[k] >> 16) & 0xFF);
                    float dg = (float)p[1] - (float)((px.colors[k] >> 8) & 0xFF);
                    float db = (float)p[2] - (float)(px.colors[k] & 0xFF);
                    float dist = dr * dr + dg * dg + db * db;
                    if (dist < best) { best = dist; last_idx = (uint8_t)k; }
                }
            }
            last_rgb = rgb;
        }
        d[i] = last_idx;
    }
    free(rgba);
    if (include_colors) {
        memcpy(px.colors, extracted, sizeof(uint32_t) * (size_t)n_extracted);
        px.num_colors = n_extracted;
        px.colors_dirty = true;
    }
    return img;
}
