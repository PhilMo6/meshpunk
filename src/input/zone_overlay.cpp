// zone_overlay.cpp — indicator-chip renderer (contract: zone_overlay.h).

#include <Arduino.h>
#include <string.h>

#include "zone_overlay.h"

// ── 5x7 bitmap font ─────────────────────────────────────────────────────────
// Column-major, 5 bytes per glyph, bit0 = top row. Uppercase letters, digits
// and the few symbols zone labels use; anything else renders as a blank
// column gap. Lowercase input is uppercased before lookup.

static const uint8_t FONT_DIGITS[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
};

static const uint8_t FONT_UPPER[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
};

static const uint8_t GLYPH_CARET[5] = {0x04,0x02,0x01,0x02,0x04};  // ^
static const uint8_t GLYPH_LT[5]    = {0x08,0x14,0x22,0x41,0x00};  // <
static const uint8_t GLYPH_GT[5]    = {0x00,0x41,0x22,0x14,0x08};  // >
static const uint8_t GLYPH_PLUS[5]  = {0x08,0x08,0x3E,0x08,0x08};  // +
static const uint8_t GLYPH_MINUS[5] = {0x08,0x08,0x08,0x08,0x08};  // -
static const uint8_t GLYPH_BANG[5]  = {0x00,0x00,0x5F,0x00,0x00};  // !
static const uint8_t GLYPH_DOT[5]   = {0x00,0x60,0x60,0x00,0x00};  // .
static const uint8_t GLYPH_COLON[5] = {0x00,0x36,0x36,0x00,0x00};  // :
static const uint8_t GLYPH_SLASH[5] = {0x20,0x10,0x08,0x04,0x02};  // /
static const uint8_t GLYPH_BSLSH[5] = {0x02,0x04,0x08,0x10,0x20};  // backslash

static const uint8_t* glyph_for(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return FONT_UPPER[c - 'A'];
    if (c >= '0' && c <= '9') return FONT_DIGITS[c - '0'];
    switch (c) {
        case '^': return GLYPH_CARET;
        case '<': return GLYPH_LT;
        case '>': return GLYPH_GT;
        case '+': return GLYPH_PLUS;
        case '-': return GLYPH_MINUS;
        case '!': return GLYPH_BANG;
        case '.': return GLYPH_DOT;
        case ':': return GLYPH_COLON;
        case '/': return GLYPH_SLASH;
        case '\\': return GLYPH_BSLSH;
        default:  return nullptr;   // blank cell (space and unknowns)
    }
}

// ── Chip building ───────────────────────────────────────────────────────────
// Text at 2x scale: 10x14 px per char, 12 px advance, 4 px padding, 1 px
// white border, transparent interior. Every white pixel gets a 1px shadow
// at (+1,+1) so the label stays readable over bright game pixels.

#define CHIP_SCALE   2
#define CHIP_ADV     (6 * CHIP_SCALE)
#define CHIP_TEXT_H  (7 * CHIP_SCALE)
#define CHIP_PAD     4

// Foreground colors must survive both byte orders unchanged (see header):
// only equal-byte RGB565 values qualify. 0x0000 is the transparent sentinel.
#define ZO_WHITE  0xFFFF
#define ZO_SHADOW 0x2121
#define ZO_CLEAR  0x0000

static ZoneChip  s_chips[INPUT_ZONES_MAX];
static int       s_chip_count = 0;
static uint16_t* s_scratch = nullptr;      // repack buffer, sized to largest chip
static int       s_scratch_px = 0;

void zone_overlay_clear(void) {
    for (int i = 0; i < s_chip_count; i++) {
        free(s_chips[i].buf);
        s_chips[i].buf = nullptr;
    }
    s_chip_count = 0;
    free(s_scratch);
    s_scratch = nullptr;
    s_scratch_px = 0;
}

static void draw_glyph_scaled(uint16_t* buf, int bw, int px, int py,
                              const uint8_t* g, int scale, uint16_t color) {
    if (!g) return;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            int bx = px + col * scale;
            int by = py + row * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    buf[(by + dy) * bw + (bx + dx)] = color;
        }
    }
}

void zone_overlay_draw_text(uint16_t* buf, int bw, int x, int y, int scale,
                            const char* s, uint16_t color) {
    if (!buf || !s || scale < 1) return;
    for (int i = 0; s[i]; i++)
        draw_glyph_scaled(buf, bw, x + i * 6 * scale, y, glyph_for(s[i]),
                          scale, color);
}

static void draw_glyph(uint16_t* buf, int bw, int px, int py, const uint8_t* g) {
    draw_glyph_scaled(buf, bw, px, py, g, CHIP_SCALE, ZO_WHITE);
}

void zone_overlay_build(const InputZone* zones, int count) {
    zone_overlay_clear();
    if (!zones) return;
    for (int i = 0; i < count && s_chip_count < INPUT_ZONES_MAX; i++) {
        const InputZone* z = &zones[i];
        if (z->out == INPUT_ZONE_MODE) continue;
        int len = (int)strnlen(z->label, sizeof(z->label));
        if (len == 0) continue;

        int w = len * CHIP_ADV - CHIP_SCALE + 2 * CHIP_PAD;
        int h = CHIP_TEXT_H + 2 * CHIP_PAD;
        uint16_t* buf = (uint16_t*)ps_malloc((size_t)w * h * sizeof(uint16_t));
        if (!buf) continue;

        for (int p = 0; p < w * h; p++) buf[p] = ZO_CLEAR;
        for (int x = 0; x < w; x++) { buf[x] = ZO_WHITE; buf[(h - 1) * w + x] = ZO_WHITE; }
        for (int y = 0; y < h; y++) { buf[y * w] = ZO_WHITE; buf[y * w + w - 1] = ZO_WHITE; }
        for (int c = 0; c < len; c++)
            draw_glyph(buf, w, CHIP_PAD + c * CHIP_ADV, CHIP_PAD, glyph_for(z->label[c]));

        // Drop shadow: any transparent pixel diagonally below-right of a
        // white one. Single forward pass is safe — shadows are never white,
        // so they can't seed further shadows.
        for (int y = 0; y < h - 1; y++)
            for (int x = 0; x < w - 1; x++)
                if (buf[y * w + x] == ZO_WHITE &&
                    buf[(y + 1) * w + x + 1] == ZO_CLEAR)
                    buf[(y + 1) * w + x + 1] = ZO_SHADOW;

        ZoneChip* chip = &s_chips[s_chip_count];
        chip->w = (int16_t)w;
        chip->h = (int16_t)h;
        chip->x = (int16_t)(z->x + (z->w - w) / 2);
        chip->y = (int16_t)(z->y + (z->h - h) / 2);
        if (chip->x < 0) chip->x = 0;
        if (chip->y < 0) chip->y = 0;
        chip->buf = buf;
        s_chip_count++;
        if (w * h > s_scratch_px) s_scratch_px = w * h;
    }
    if (s_chip_count > 0)
        s_scratch = (uint16_t*)ps_malloc((size_t)s_scratch_px * sizeof(uint16_t));
}

int zone_overlay_count(void) { return s_chip_count; }

const ZoneChip* zone_overlay_chip(int i) {
    if (i < 0 || i >= s_chip_count) return nullptr;
    return &s_chips[i];
}

void zone_overlay_stamp(uint16_t* frame, int fx, int fy, int fw, int fh) {
    if (!frame) return;
    for (int i = 0; i < s_chip_count; i++) {
        const ZoneChip* c = &s_chips[i];
        // Chip rectangle in frame coordinates, clipped to the frame.
        int x0 = c->x - fx, y0 = c->y - fy;
        int sx = 0, sy = 0;
        int w = c->w, h = c->h;
        if (x0 < 0) { sx = -x0; w += x0; x0 = 0; }
        if (y0 < 0) { sy = -y0; h += y0; y0 = 0; }
        if (x0 + w > fw) w = fw - x0;
        if (y0 + h > fh) h = fh - y0;
        if (w <= 0 || h <= 0) continue;
        for (int row = 0; row < h; row++) {
            uint16_t*       dst = &frame[(y0 + row) * fw + x0];
            const uint16_t* src = &c->buf[(sy + row) * c->w + sx];
            for (int col = 0; col < w; col++)
                if (src[col] != ZO_CLEAR) dst[col] = src[col];
        }
    }
}

// Repack one chip sub-rect into the scratch buffer (transparent pixels
// become black — these pushes land in the letterbox borders) and push it.
static void push_part(const ZoneChip* c, int rx, int ry, int rw, int rh,
                      bool erase,
                      void (*push)(int, int, int, int, const uint16_t*)) {
    if (rw <= 0 || rh <= 0 || !s_scratch) return;
    if (rw * rh > s_scratch_px) return;
    if (erase) {
        memset(s_scratch, 0, (size_t)rw * rh * sizeof(uint16_t));
    } else {
        for (int row = 0; row < rh; row++)
            memcpy(&s_scratch[row * rw], &c->buf[(ry + row) * c->w + rx],
                   (size_t)rw * sizeof(uint16_t));
    }
    push(c->x + rx, c->y + ry, rw, rh, s_scratch);
}

void zone_overlay_push_outside(int fx, int fy, int fw, int fh, bool erase,
                               void (*push)(int, int, int, int,
                                            const uint16_t*)) {
    if (!push) return;
    for (int i = 0; i < s_chip_count; i++) {
        const ZoneChip* c = &s_chips[i];
        // Frame rect in chip-local coordinates, clamped to the chip.
        int ix0 = fx - c->x, iy0 = fy - c->y;
        int ix1 = ix0 + fw,  iy1 = iy0 + fh;
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 > c->w) ix1 = c->w;
        if (iy1 > c->h) iy1 = c->h;
        if (ix0 >= ix1 || iy0 >= iy1) {
            // No overlap: the whole chip lies outside the frame.
            push_part(c, 0, 0, c->w, c->h, erase, push);
            continue;
        }
        // Up to four bands around the intersection.
        push_part(c, 0,   0,   c->w,        iy0,        erase, push); // top
        push_part(c, 0,   iy1, c->w,        c->h - iy1, erase, push); // bottom
        push_part(c, 0,   iy0, ix0,         iy1 - iy0,  erase, push); // left
        push_part(c, ix1, iy0, c->w - ix1,  iy1 - iy0,  erase, push); // right
    }
}
