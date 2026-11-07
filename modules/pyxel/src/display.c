// Screen output: the indexed screen image is converted through the palette to
// byte-swapped RGB565 into one of two frame buffers pushed by the firmware's
// async blit. A screen that fits the 320x240 panel is drawn at the largest
// integer scale; a larger one is shrunk to fit by area averaging (each panel
// pixel is the mean of the screen pixels it covers).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

#define PX_SCREEN_LIMIT 1024           // largest screen width and height accepted
#define TAPS_MAX        6              // screen pixels one panel pixel covers per axis, at most

// Area-average weights of one panel column or row.
typedef struct {
    int first;                         // first screen pixel covered
    int n;                             // screen pixels covered
    uint32_t w[TAPS_MAX];              // their weights, 16.16, summing to 65536
} tap_t;

static uint16_t* s_out[2];
static int s_out_idx;
static int s_scale, s_out_w, s_out_h;  // s_scale 0: shrink to fit
static uint16_t s_lut[PX_MAX_COLORS];

// Shrink-to-fit state: weights per panel column/row, the palette as RGB888,
// and a ring of horizontally averaged screen rows (8.8 fixed point per
// channel), each reused by the consecutive panel rows that cover it.
static tap_t* s_tx;
static tap_t* s_ty;
static uint32_t s_rgb[PX_MAX_COLORS];
static uint16_t* s_hrows;
static int s_hrow_y[TAPS_MAX];

static tap_t* build_taps(int src, int dst) {
    tap_t* t = (tap_t*)calloc((size_t)dst, sizeof(tap_t));
    if (!t) return NULL;
    for (int o = 0; o < dst; o++) {
        // The covered interval [o * src, (o + 1) * src), in 1/dst screen pixels.
        uint32_t start = (uint32_t)o * (uint32_t)src, end = start + (uint32_t)src;
        int first = (int)(start / (uint32_t)dst), last = (int)((end - 1) / (uint32_t)dst);
        t[o].first = first;
        t[o].n = last - first + 1;
        uint32_t sum = 0;
        for (int k = 0; k < t[o].n; k++) {
            uint32_t lo = (uint32_t)(first + k) * (uint32_t)dst, hi = lo + (uint32_t)dst;
            uint32_t a = lo > start ? lo : start, b = hi < end ? hi : end;
            t[o].w[k] = (uint32_t)(((uint64_t)(b - a) << 16) / (uint32_t)src);
            sum += t[o].w[k];
        }
        t[o].w[t[o].n - 1] += 65536u - sum;
    }
    return t;
}

bool px_display_begin(int w, int h, char* err, size_t errlen) {
    px_display_end();
    if (w <= 0 || h <= 0 || w > PX_SCREEN_LIMIT || h > PX_SCREEN_LIMIT) {
        snprintf(err, errlen, "screen size %dx%d is outside 1x1..%dx%d", w, h, PX_SCREEN_LIMIT, PX_SCREEN_LIMIT);
        return false;
    }
    if (w <= PX_SCREEN_MAX_W && h <= PX_SCREEN_MAX_H) {
        int sx = PX_SCREEN_MAX_W / w, sy = PX_SCREEN_MAX_H / h;
        s_scale = sx < sy ? sx : sy;
        s_out_w = w * s_scale;
        s_out_h = h * s_scale;
    } else {
        s_scale = 0;
        if (w * PX_SCREEN_MAX_H >= h * PX_SCREEN_MAX_W) {
            s_out_w = PX_SCREEN_MAX_W;
            s_out_h = (h * PX_SCREEN_MAX_W + w / 2) / w;
        } else {
            s_out_h = PX_SCREEN_MAX_H;
            s_out_w = (w * PX_SCREEN_MAX_H + h / 2) / h;
        }
        if (s_out_w < 1) s_out_w = 1;
        if (s_out_h < 1) s_out_h = 1;
        s_tx = build_taps(w, s_out_w);
        s_ty = build_taps(h, s_out_h);
        s_hrows = (uint16_t*)malloc(sizeof(uint16_t) * 3 * (size_t)s_out_w * TAPS_MAX);
        if (!s_tx || !s_ty || !s_hrows) {
            px_display_end();
            snprintf(err, errlen, "out of memory for scaling the %dx%d screen", w, h);
            return false;
        }
    }
    size_t n = (size_t)s_out_w * (size_t)s_out_h;
    s_out[0] = (uint16_t*)malloc(n * sizeof(uint16_t));
    s_out[1] = (uint16_t*)malloc(n * sizeof(uint16_t));
    if (!s_out[0] || !s_out[1]) {
        px_display_end();
        snprintf(err, errlen, "out of memory for the %dx%d frame buffers", s_out_w, s_out_h);
        return false;
    }
    s_out_idx = 0;
    host_clear_screen();
    return true;
}

void px_display_end(void) {
    host_blit_wait();
    free(s_out[0]);
    free(s_out[1]);
    s_out[0] = s_out[1] = NULL;
    free(s_tx);
    free(s_ty);
    free(s_hrows);
    s_tx = s_ty = NULL;
    s_hrows = NULL;
}

// Screen row y averaged across each panel column's pixels, into ring slot y % TAPS_MAX.
static const uint16_t* hrow(const uint8_t* src, int w, int y) {
    int slot = y % TAPS_MAX;
    uint16_t* out = s_hrows + (size_t)slot * 3 * (size_t)s_out_w;
    if (s_hrow_y[slot] == y) return out;
    s_hrow_y[slot] = y;
    const uint8_t* sp = src + (size_t)y * (size_t)w;
    for (int ox = 0; ox < s_out_w; ox++) {
        const tap_t* t = &s_tx[ox];
        uint32_t r = 0, g = 0, b = 0;
        for (int k = 0; k < t->n; k++) {
            uint32_t c = s_rgb[sp[t->first + k]];
            r += (c >> 16) * t->w[k];
            g += ((c >> 8) & 0xFF) * t->w[k];
            b += (c & 0xFF) * t->w[k];
        }
        out[3 * ox] = (uint16_t)(r >> 8);
        out[3 * ox + 1] = (uint16_t)(g >> 8);
        out[3 * ox + 2] = (uint16_t)(b >> 8);
    }
    return out;
}

static void render_shrunk(const uint8_t* src, int w, uint16_t* dst) {
    for (int i = 0; i < PX_MAX_COLORS; i++) s_rgb[i] = i < px.num_colors ? px.colors[i] & 0xFFFFFF : 0;
    for (int i = 0; i < TAPS_MAX; i++) s_hrow_y[i] = -1;
    for (int oy = 0; oy < s_out_h; oy++) {
        const tap_t* t = &s_ty[oy];
        const uint16_t* rows[TAPS_MAX];
        for (int k = 0; k < t->n; k++) rows[k] = hrow(src, w, t->first + k);
        uint16_t* d = dst + (size_t)oy * (size_t)s_out_w;
        for (int ox = 0; ox < s_out_w; ox++) {
            uint32_t r = 1u << 23, g = 1u << 23, b = 1u << 23;
            for (int k = 0; k < t->n; k++) {
                r += rows[k][3 * ox] * t->w[k];
                g += rows[k][3 * ox + 1] * t->w[k];
                b += rows[k][3 * ox + 2] * t->w[k];
            }
            r >>= 24;
            g >>= 24;
            b >>= 24;
            uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            d[ox] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

void px_display_render(void) {
    if (!s_out[0]) return;
    const px_image* scr = px.screen;
    const uint8_t* src = (const uint8_t*)scr->cv.data;
    uint16_t* dst = s_out[s_out_idx];
    int w = scr->cv.w, h = scr->cv.h, s = s_scale;
    if (s == 0) {
        render_shrunk(src, w, dst);
    } else {
        for (int i = 0; i < PX_MAX_COLORS; i++) {
            uint32_t c = i < px.num_colors ? px.colors[i] : 0;
            uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            s_lut[i] = (uint16_t)((v >> 8) | (v << 8));
        }
        for (int y = 0; y < h; y++) {
            uint16_t* row = dst + (size_t)y * s * s_out_w;
            const uint8_t* sp = src + (size_t)y * w;
            if (s == 1) {
                for (int x = 0; x < w; x++) row[x] = s_lut[sp[x]];
            } else {
                uint16_t* d = row;
                for (int x = 0; x < w; x++) {
                    uint16_t c = s_lut[sp[x]];
                    for (int k = 0; k < s; k++) *d++ = c;
                }
                for (int k = 1; k < s; k++)
                    memcpy(row + (size_t)k * s_out_w, row, sizeof(uint16_t) * (size_t)s_out_w);
            }
        }
    }
    host_blit_frame_async(dst, s_out_w, s_out_h);
    s_out_idx ^= 1;
}

// system.rs draw_cursor: stamp the cursor image into the screen at the mouse
// position with clip and camera reset.
void px_draw_cursor(void) {
    if (!px.mouse_visible || !px.cursor) return;
    int x = px.mouse_x, y = px.mouse_y;
    int cw = px.cursor->cv.w, ch = px.cursor->cv.h;
    if (x <= -cw || x >= px.width || y <= -ch || y >= px.height) return;
    px_canvas* cv = &px.screen->cv;
    px_rect clip = cv->clip;
    int cam_x = cv->cam_x, cam_y = cv->cam_y;
    px_canvas_reset_clip(cv);
    cv->cam_x = cv->cam_y = 0;
    px_image_blt(px.screen, (float)x, (float)y, px.cursor, 0.0f, 0.0f,
                 (float)cw, (float)ch, 0, 0.0f, 1.0f);
    cv->clip = clip;
    cv->cam_x = cam_x;
    cv->cam_y = cam_y;
}
