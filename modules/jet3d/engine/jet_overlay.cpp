#include "jet_overlay.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 5x7 font, drawn in a 6x8 cell (one column and one row of spacing).
// ---------------------------------------------------------------------------
// Covers printable ASCII 32..126. Five bytes per glyph, one byte per COLUMN,
// bit 0 = top row through bit 6 = bottom row. Column-major suits the draw loop:
// one byte is fetched per column and shifted down through the rows.
static const uint8_t s_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x14,0x08,0x3E,0x08,0x14}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x01,0x01}, // 70 F
    {0x3E,0x41,0x41,0x51,0x32}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x03,0x04,0x78,0x04,0x03}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x00,0x7F,0x41,0x41}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x41,0x41,0x7F,0x00,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x08,0x14,0x54,0x54,0x3C}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x00,0x7F,0x10,0x28,0x44}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x08,0x04,0x08,0x10,0x08}, // 126 ~
};

// ---------------------------------------------------------------------------
// Display list
// ---------------------------------------------------------------------------

#define OVL_MAX_CMDS   96
#define OVL_TEXT_POOL  1536

enum { OVL_TEXT = 0, OVL_RECT = 1 };

typedef struct {
    uint8_t  type;
    uint8_t  scale;
    int16_t  x, y, w, h;
    uint16_t color;
    uint16_t textOff;   // offset into s_textPool for OVL_TEXT
} OvlCmd;

static OvlCmd s_cmds[OVL_MAX_CMDS];
static int    s_cmdCount = 0;
static char   s_textPool[OVL_TEXT_POOL];
static int    s_textUsed = 0;

void jet_ovl_begin(void)
{
    s_cmdCount = 0;
    s_textUsed = 0;
}

void jet_ovl_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_cmdCount >= OVL_MAX_CMDS || w <= 0 || h <= 0) return;
    OvlCmd* c = &s_cmds[s_cmdCount++];
    c->type = OVL_RECT;
    c->scale = 1;
    c->x = (int16_t)x; c->y = (int16_t)y;
    c->w = (int16_t)w; c->h = (int16_t)h;
    c->color = color;
    c->textOff = 0;
}

void jet_ovl_text(int x, int y, uint16_t color, int scale, const char* s)
{
    if (!s || s_cmdCount >= OVL_MAX_CMDS) return;
    if (scale < 1) scale = 1;

    const int len = (int)strlen(s);
    if (len == 0) return;
    if (s_textUsed + len + 1 > OVL_TEXT_POOL) return;   // pool full: drop

    OvlCmd* c = &s_cmds[s_cmdCount++];
    c->type = OVL_TEXT;
    c->scale = (uint8_t)scale;
    c->x = (int16_t)x; c->y = (int16_t)y;
    c->w = (int16_t)(len * JET_OVL_GLYPH_W * scale);
    c->h = (int16_t)(JET_OVL_GLYPH_H * scale);
    c->color = color;
    c->textOff = (uint16_t)s_textUsed;

    memcpy(&s_textPool[s_textUsed], s, (size_t)len);
    s_textUsed += len;
    s_textPool[s_textUsed++] = '\0';
}

int jet_ovl_text_width(int scale, const char* s)
{
    if (!s) return 0;
    if (scale < 1) scale = 1;
    return (int)strlen(s) * JET_OVL_GLYPH_W * scale;
}

// ---------------------------------------------------------------------------
// Band replay
// ---------------------------------------------------------------------------

static void fill_rect_band(uint16_t* band, int screenW, int y0, int y1,
                           int x, int y, int w, int h, uint16_t color)
{
    int rx0 = x, rx1 = x + w;
    if (rx0 < 0) rx0 = 0;
    if (rx1 > screenW) rx1 = screenW;
    if (rx0 >= rx1) return;

    int ry0 = y, ry1 = y + h;
    if (ry0 < y0) ry0 = y0;
    if (ry1 > y1) ry1 = y1;
    if (ry0 >= ry1) return;

    for (int row = ry0; row < ry1; ++row) {
        uint16_t* dst = band + (size_t)(row - y0) * screenW + rx0;
        for (int i = rx1 - rx0; i > 0; --i) *dst++ = color;
    }
}

// One glyph. Columns come from the font byte; rows are the bits within it.
// Only the rows intersecting the band are touched, so a string straddling a
// band boundary is drawn correctly in two pieces.
static void draw_glyph_band(uint16_t* band, int screenW, int y0, int y1,
                            int gx, int gy, int scale, unsigned char ch,
                            uint16_t color)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* col = s_font5x7[ch - 32];

    for (int cx = 0; cx < 5; ++cx) {
        const uint8_t bits = col[cx];
        if (!bits) continue;
        const int px = gx + cx * scale;
        if (px + scale <= 0 || px >= screenW) continue;

        for (int cy = 0; cy < 7; ++cy) {
            if (!(bits & (1u << cy))) continue;
            fill_rect_band(band, screenW, y0, y1,
                           px, gy + cy * scale, scale, scale, color);
        }
    }
}

void jet_ovl_draw_band(uint16_t* band, int screenW, int y0, int y1)
{
    if (!band || s_cmdCount == 0) return;

    for (int i = 0; i < s_cmdCount; ++i) {
        const OvlCmd* c = &s_cmds[i];

        // Whole command above or below this band: nothing to do.
        if (c->y >= y1 || c->y + c->h <= y0) continue;

        if (c->type == OVL_RECT) {
            fill_rect_band(band, screenW, y0, y1, c->x, c->y, c->w, c->h, c->color);
        } else {
            const char* s = &s_textPool[c->textOff];
            int gx = c->x;
            const int adv = JET_OVL_GLYPH_W * c->scale;
            for (; *s; ++s, gx += adv) {
                if (gx >= screenW) break;          // ran off the right edge
                if (gx + adv <= 0) continue;       // still off the left edge
                // MESHPUNK: 1px black outline (8 directions) under every
                // glyph. White HUD/menu text was dissolving into bright
                // track surfaces (start line, glow rims) scrolling behind
                // it — the overlay is opaque and drawn last, so this was
                // contrast, not occlusion. The outline separates text from
                // ANY background without per-screen backing boxes.
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox)
                        if (ox || oy)
                            draw_glyph_band(band, screenW, y0, y1,
                                            gx + ox, c->y + oy, c->scale,
                                            (unsigned char)*s, 0x0000);
            }
            // Second pass so a glyph's outline never overwrites the face of
            // its already-drawn neighbour (advance is one glyph pixel).
            gx = c->x; s = &s_textPool[c->textOff];
            for (; *s; ++s, gx += adv) {
                if (gx >= screenW) break;
                if (gx + adv <= 0) continue;
                draw_glyph_band(band, screenW, y0, y1,
                                gx, c->y, c->scale, (unsigned char)*s, c->color);
            }
        }
    }
}
