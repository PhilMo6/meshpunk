// BDF bitmap fonts: a port of the BDF half of pyxel-core font.rs (MIT,
// Takashi Kitao). OTF/TTF fonts are not supported.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

typedef struct {
    int32_t code;
    int32_t dwidth;
    int32_t w, h, x, y;       // BBX
    uint32_t row;             // first bitmap row in the font's row pool
    uint32_t nrows;
} glyph_t;

typedef struct {
    bool used;
    int32_t bw, bh, bx, by;   // FONTBOUNDINGBOX
    glyph_t* glyphs;
    int n;
    uint32_t* rows;           // bit j = pixel j of the row (leftmost = bit 0)
} font_t;

static font_t* s_fonts;
static int s_nfonts;

static int cmp_glyph(const void* a, const void* b) {
    int32_t x = ((const glyph_t*)a)->code, y = ((const glyph_t*)b)->code;
    return x < y ? -1 : x > y;
}

static bool starts(const char* line, const char* kw) {
    return strncmp(line, kw, strlen(kw)) == 0;
}

// "KEYWORD v1 v2 ..." -> up to n integers; returns how many parsed.
static int ints_after(const char* line, int32_t* out, int n) {
    const char* p = line;
    while (*p && *p != ' ' && *p != '\t') p++;
    int k = 0;
    while (k < n) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\r' || *p == '\n') break;
        char* e;
        long v = strtol(p, &e, 10);
        if (e == p) break;
        out[k++] = (int32_t)v;
        p = e;
    }
    return k;
}

static uint32_t reverse_bits(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

int px_font_load(const char* path, char* err, size_t errlen) {
    size_t n = 0;
    char* text = px_vfs_read(path, &n);
    if (!text) { snprintf(err, errlen, "Failed to open file '%s'", path); return -1; }
    font_t f;
    memset(&f, 0, sizeof(f));
    int cap = 0;
    uint32_t nrows = 0, caprows = 0;
    bool have_code = false, in_bitmap = false;
    int32_t code = 0, dwidth = 0;
    int32_t bbx[4] = { 0, 0, 0, 0 };
    uint32_t glyph_row = 0;
    bool ok = true;

    char* p = text;
    char* end = text + n;
    while (ok && p < end) {
        char* line = p;
        while (p < end && *p != '\n') p++;
        if (p < end) *p++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        int32_t v[4];
        if (starts(line, "FONTBOUNDINGBOX")) {
            if (ints_after(line, v, 4) < 4) { ok = false; break; }
            if (v[0] > 32) {
                snprintf(err, errlen, "BDF glyph width %d exceeds 32 pixel limit", (int)v[0]);
                goto fail;
            }
            f.bw = v[0]; f.bh = v[1]; f.bx = v[2]; f.by = v[3];
        } else if (starts(line, "ENCODING")) {
            if (ints_after(line, v, 1) < 1) { ok = false; break; }
            code = v[0];
            have_code = true;
        } else if (starts(line, "DWIDTH")) {
            if (ints_after(line, v, 1) < 1) { ok = false; break; }
            dwidth = v[0];
        } else if (starts(line, "BBX")) {
            if (ints_after(line, v, 4) < 4) { ok = false; break; }
            if (v[0] > 32) {
                snprintf(err, errlen, "BDF glyph width %d exceeds 32 pixel limit", (int)v[0]);
                goto fail;
            }
            memcpy(bbx, v, sizeof(bbx));
        } else if (starts(line, "BITMAP")) {
            in_bitmap = true;
            glyph_row = nrows;
        } else if (starts(line, "ENDCHAR")) {
            if (have_code && in_bitmap) {
                if (f.n == cap) {
                    cap = cap ? cap * 2 : 128;
                    glyph_t* g = (glyph_t*)realloc(f.glyphs, sizeof(glyph_t) * (size_t)cap);
                    if (!g) { snprintf(err, errlen, "out of memory"); goto fail; }
                    f.glyphs = g;
                }
                glyph_t* g = &f.glyphs[f.n++];
                g->code = code;
                g->dwidth = dwidth;
                g->w = bbx[0]; g->h = bbx[1]; g->x = bbx[2]; g->y = bbx[3];
                g->row = glyph_row;
                g->nrows = nrows - glyph_row;
            }
            in_bitmap = false;
        } else if (in_bitmap) {
            const char* h = line;
            while (*h == ' ' || *h == '\t') h++;
            size_t hl = strlen(h);
            while (hl && (h[hl - 1] == ' ' || h[hl - 1] == '\t')) hl--;
            if (hl == 0 || hl > 8) { ok = false; break; }
            uint32_t bits = 0;
            for (size_t i = 0; i < hl; i++) {
                char c = h[i];
                int d = (c >= '0' && c <= '9') ? c - '0' : ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') ? (c | 0x20) - 'a' + 10 : -1;
                if (d < 0) { ok = false; break; }
                bits = bits << 4 | (uint32_t)d;
            }
            if (!ok) break;
            if (nrows == caprows) {
                caprows = caprows ? caprows * 2 : 1024;
                uint32_t* g = (uint32_t*)realloc(f.rows, sizeof(uint32_t) * caprows);
                if (!g) { snprintf(err, errlen, "out of memory"); goto fail; }
                f.rows = g;
            }
            f.rows[nrows++] = reverse_bits(bits) >> (32 - hl * 4);
        }
    }
    if (!ok) {
        snprintf(err, errlen, "Failed to parse file '%s'", path);
        goto fail;
    }
    free(text);
    qsort(f.glyphs, (size_t)f.n, sizeof(glyph_t), cmp_glyph);
    f.used = true;
    for (int i = 0; i < s_nfonts; i++) {
        if (!s_fonts[i].used) {
            s_fonts[i] = f;
            return i;
        }
    }
    font_t* grown = (font_t*)realloc(s_fonts, sizeof(font_t) * (size_t)(s_nfonts + 1));
    if (!grown) {
        snprintf(err, errlen, "out of memory");
        free(f.glyphs);
        free(f.rows);
        return -1;
    }
    s_fonts = grown;
    s_fonts[s_nfonts] = f;
    return s_nfonts++;
fail:
    free(text);
    free(f.glyphs);
    free(f.rows);
    return -1;
}

static font_t* font_get(int h) {
    return (h >= 0 && h < s_nfonts && s_fonts[h].used) ? &s_fonts[h] : NULL;
}

static const glyph_t* glyph_find(const font_t* f, int32_t code) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int32_t c = f->glyphs[mid].code;
        if (c == code) return &f->glyphs[mid];
        if (c < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// Next UTF-8 code point from s; malformed bytes decode as U+FFFD.
static uint32_t utf8_next(const char** s, const char* end) {
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t c = *p++;
    int more = 0;
    if (c >= 0xF0) { c &= 0x07; more = 3; }
    else if (c >= 0xE0) { c &= 0x0F; more = 2; }
    else if (c >= 0xC0) { c &= 0x1F; more = 1; }
    else if (c >= 0x80) { *s = (const char*)p; return 0xFFFD; }
    while (more-- > 0) {
        if ((const char*)p >= end || (*p & 0xC0) != 0x80) { *s = (const char*)p; return 0xFFFD; }
        c = c << 6 | (*p++ & 0x3F);
    }
    *s = (const char*)p;
    return c;
}

// font.rs is_invisible: control characters, variation selectors, bidi
// controls and zero-width specials take no space.
static bool invisible(uint32_t c) {
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return true;
    if ((c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF)) return true;
    if ((c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069))
        return true;
    return c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060;
}

bool px_font_valid(int h) {
    return font_get(h) != NULL;
}

void px_font_draw(int h, px_image* dst, float fx, float fy, const char* s, size_t len, int col) {
    const font_t* f = font_get(h);
    if (!f) return;
    px_canvas* cv = &dst->cv;
    int x = px_f2i(fx) - cv->cam_x;
    int y = px_f2i(fy) - cv->cam_y;
    uint8_t color = dst->pal[col & 0xFF];
    int start_x = x;
    const char* end = s + len;
    while (s < end) {
        uint32_t c = utf8_next(&s, end);
        if (c == '\n') {
            x = start_x;
            y += f->bh;
            continue;
        }
        if (invisible(c)) continue;
        const glyph_t* g = glyph_find(f, (int32_t)c);
        if (!g) continue;
        int gx = x + f->bx + g->x;
        int gy = y + f->by + f->bh - g->y - g->h;
        for (uint32_t i = 0; i < g->nrows; i++) {
            uint32_t row = f->rows[g->row + i];
            for (int j = 0; j < g->w && row; j++) {
                if ((row >> j) & 1) px_write_clipped_u8(cv, gx + j, gy + (int)i, color);
            }
        }
        x += g->dwidth;
    }
}

int px_font_text_width(int h, const char* s, size_t len) {
    const font_t* f = font_get(h);
    if (!f) return 0;
    int max_w = 0, w = 0;
    const char* end = s + len;
    while (s < end) {
        uint32_t c = utf8_next(&s, end);
        if (c == '\n') {
            if (w > max_w) max_w = w;
            w = 0;
            continue;
        }
        if (invisible(c)) continue;
        const glyph_t* g = glyph_find(f, (int32_t)c);
        if (g) w += g->dwidth;
    }
    return w > max_w ? w : max_w;
}

void px_font_shutdown(void) {
    for (int i = 0; i < s_nfonts; i++) {
        free(s_fonts[i].glyphs);
        free(s_fonts[i].rows);
    }
    free(s_fonts);
    s_fonts = NULL;
    s_nfonts = 0;
}
