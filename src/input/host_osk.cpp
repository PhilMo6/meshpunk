// host_osk.cpp — host-rendered ELF on-screen keyboard (contract: host_osk.h).

#include <Arduino.h>
#include <string.h>

#include "host_osk.h"
#include "zone_overlay.h"   // shared 5x7 text renderer

// Module video contract is 320x240 (display_dev.h); the keyboard takes the
// bottom 5 rows of 36px. Everything above stays the module's screen — with
// DOS the prompt echo is visible there while typing.
#define OSK_W       320
#define OSK_Y       60
#define OSK_H       180
#define OSK_ROW_H   36

// Colors in WIRE byte order: this frame only ever goes through direct
// display pushes, which forward raw bytes (modules pre-swap their frames
// the same way). Wire value = bswap16(logical RGB565).
#define OSK_BG      0x0000   // black
#define OSK_TILE    0x0842   // logical 0x4208 — dark gray tile
#define OSK_TILE_HI 0xFFFF   // pressed tile: white
#define OSK_TEXT    0xFFFF
#define OSK_TEXT_HI 0x0000

typedef struct {
    int16_t x, w;
    int8_t  row;
    uint8_t out;
    const char* label;
} OskKey;

static const OskKey KEYS[] = {
    { 0,   32, 0, '1', "1" }, { 32,  32, 0, '2', "2" }, { 64,  32, 0, '3', "3" },
    { 96,  32, 0, '4', "4" }, { 128, 32, 0, '5', "5" }, { 160, 32, 0, '6', "6" },
    { 192, 32, 0, '7', "7" }, { 224, 32, 0, '8', "8" }, { 256, 32, 0, '9', "9" },
    { 288, 32, 0, '0', "0" },

    { 0,   32, 1, 'q', "Q" }, { 32,  32, 1, 'w', "W" }, { 64,  32, 1, 'e', "E" },
    { 96,  32, 1, 'r', "R" }, { 128, 32, 1, 't', "T" }, { 160, 32, 1, 'y', "Y" },
    { 192, 32, 1, 'u', "U" }, { 224, 32, 1, 'i', "I" }, { 256, 32, 1, 'o', "O" },
    { 288, 32, 1, 'p', "P" },

    { 0,   32, 2, 'a', "A" }, { 32,  32, 2, 's', "S" }, { 64,  32, 2, 'd', "D" },
    { 96,  32, 2, 'f', "F" }, { 128, 32, 2, 'g', "G" }, { 160, 32, 2, 'h', "H" },
    { 192, 32, 2, 'j', "J" }, { 224, 32, 2, 'k', "K" }, { 256, 32, 2, 'l', "L" },
    { 288, 32, 2, ':', ":" },

    { 0,   32, 3, 'z', "Z" }, { 32,  32, 3, 'x', "X" }, { 64,  32, 3, 'c', "C" },
    { 96,  32, 3, 'v', "V" }, { 128, 32, 3, 'b', "B" }, { 160, 32, 3, 'n', "N" },
    { 192, 32, 3, 'm', "M" }, { 224, 32, 3, '-', "-" }, { 256, 32, 3, '.', "." },
    { 288, 32, 3, '/', "/" },

    { 0,   44, 4, 0x1B, "ESC"   }, { 44,  32,  4, '\\', "\\"   },
    { 76,  112, 4, ' ',  "SPACE" }, { 188, 60,  4, 0x08, "BKSP" },
    { 248, 72, 4, 0x0D, "ENT"   },
};
#define OSK_NKEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

static InputZone s_osk_zones[OSK_NKEYS];
static bool      s_zones_built = false;
static uint16_t* s_frame = nullptr;   // OSK_W x OSK_H, PSRAM, wire byte order
// Kept separate from s_frame so closing never frees a buffer another core
// may be mid-copy from: close clears this, teardown frees the buffer.
static volatile bool s_active = false;

static int key_top(const OskKey* k) { return OSK_Y + k->row * OSK_ROW_H; }

void host_osk_zones(const InputZone** zones, int* count) {
    if (!s_zones_built) {
        for (int i = 0; i < OSK_NKEYS; i++) {
            InputZone* z = &s_osk_zones[i];
            memset(z, 0, sizeof(*z));
            z->x   = KEYS[i].x;
            z->y   = (int16_t)key_top(&KEYS[i]);
            z->w   = KEYS[i].w;
            z->h   = OSK_ROW_H;
            z->out = KEYS[i].out;
            strncpy(z->label, KEYS[i].label, sizeof(z->label) - 1);
        }
        s_zones_built = true;
    }
    *zones = s_osk_zones;
    *count = OSK_NKEYS;
}

// Render one key tile (2px cell inset) into the cached frame.
static void draw_key(const OskKey* k, bool pressed) {
    uint16_t fill = pressed ? OSK_TILE_HI : OSK_TILE;
    uint16_t text = pressed ? OSK_TEXT_HI : OSK_TEXT;
    int fy = k->row * OSK_ROW_H;    // frame-local top of the cell
    for (int y = fy + 2; y < fy + OSK_ROW_H - 2; y++)
        for (int x = k->x + 2; x < k->x + k->w - 2; x++)
            s_frame[y * OSK_W + x] = fill;
    int len = (int)strlen(k->label);
    int tw = len * 12 - 2;          // scale-2 text: 12px advance, 10px glyph
    int tx = k->x + (k->w - tw) / 2;
    int ty = fy + (OSK_ROW_H - 14) / 2;
    zone_overlay_draw_text(s_frame, OSK_W, tx, ty, 2, k->label, text);
}

bool host_osk_open(void (*push)(int, int, int, int, const uint16_t*)) {
    if (!s_frame) {
        s_frame = (uint16_t*)ps_malloc((size_t)OSK_W * OSK_H * sizeof(uint16_t));
        if (!s_frame) return false;
    }
    // Always re-render: close blanks the buffer to paint the region black,
    // and the buffer now survives a close/reopen cycle. 45 tiles of plain
    // memory writes, far cheaper than reallocating.
    for (int p = 0; p < OSK_W * OSK_H; p++) s_frame[p] = OSK_BG;
    for (int i = 0; i < OSK_NKEYS; i++) draw_key(&KEYS[i], false);
    s_active = true;
    if (push) push(0, OSK_Y, OSK_W, OSK_H, s_frame);
    return true;
}

void host_osk_close(void (*push)(int, int, int, int, const uint16_t*)) {
    if (!s_frame) return;
    s_active = false;          // stampers stop on their next frame
    if (push) {
        // Blanking the cached frame doubles as the black fill. A stamp
        // already running on another core then copies black into that one
        // in-flight module frame — visually identical to the fill itself,
        // and the module repaints the region on its next frame.
        memset(s_frame, 0, (size_t)OSK_W * OSK_H * sizeof(uint16_t));
        push(0, OSK_Y, OSK_W, OSK_H, s_frame);
    }
}

void host_osk_free(void) {
    s_active = false;
    free(s_frame);
    s_frame = nullptr;
}

bool host_osk_active(void) { return s_active; }

void host_osk_stamp(uint16_t* frame, int fx, int fy, int fw, int fh) {
    if (!s_active || !s_frame || !frame) return;
    // Keyboard rect in the target buffer's coordinates, clipped both ways.
    int x0 = -fx,         y0 = OSK_Y - fy;
    int sx = 0, sy = 0;
    int w = OSK_W, h = OSK_H;
    if (x0 < 0) { sx = -x0; w += x0; x0 = 0; }
    if (y0 < 0) { sy = -y0; h += y0; y0 = 0; }
    if (x0 + w > fw) w = fw - x0;
    if (y0 + h > fh) h = fh - y0;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++)
        memcpy(&frame[(y0 + row) * fw + x0],
               &s_frame[(sy + row) * OSK_W + sx],
               (size_t)w * sizeof(uint16_t));
}

void host_osk_maintain(int bx, int by, int bw, int bh,
                       void (*push)(int, int, int, int, const uint16_t*)) {
    (void)bx; (void)bw;
    if (!s_active || !s_frame || !push) return;
    int y0 = by, y1 = by + bh;
    if (y0 < OSK_Y) y0 = OSK_Y;
    if (y1 > OSK_Y + OSK_H) y1 = OSK_Y + OSK_H;
    if (y0 >= y1) return;
    // Full-width row band: packed in the cached frame, so no repacking. The
    // extra columns beyond a narrow module blit cost microseconds of SPI.
    push(0, y0, OSK_W, y1 - y0, &s_frame[(y0 - OSK_Y) * OSK_W]);
}

void host_osk_key_feedback(uint8_t out, bool pressed,
                           void (*push)(int, int, int, int, const uint16_t*)) {
    if (!s_active || !s_frame) return;
    for (int i = 0; i < OSK_NKEYS; i++) {
        if (KEYS[i].out != out) continue;
        draw_key(&KEYS[i], pressed);
        if (push) {
            int fy = KEYS[i].row * OSK_ROW_H;
            push(0, key_top(&KEYS[i]), OSK_W, OSK_ROW_H, &s_frame[fy * OSK_W]);
        }
        return;
    }
}
