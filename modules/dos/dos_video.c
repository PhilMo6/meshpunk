// Video path for the tiny386 DOS module on T-Deck.
//
// tiny386 renders into a fixed-size RGB565 canvas (BPP=16) and centres the
// guest's active video mode inside it, leaving the rest blank. The canvas is
// sized for the largest mode we support (640x480); the panel is 320x240. So
// each push stretches ONLY the active-mode rect -- published by the MESHPUNK
// patch in vga.c as meshpunk_vga_* -- onto the full panel. Scaling the padded
// canvas instead would shrink a 320x200 game into a quarter of the screen.
//
// The scaler (nearest-neighbour maps + row-repeat memcpy), the byte-swap and
// the frame-rate cap are carried over from the Faux86 PC-XT module, where they
// were tuned on hardware; see modules/pcxt/tdeck_host.cpp.

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "tiny386-src/vga.h"

extern void     host_blit_frame_async(const uint16_t *rgb565, int w, int h);
extern uint32_t host_get_ticks_us(void);

#define PANEL_W 320
#define PANEL_H 240

// The host's wire format is byte-swapped RGB565. tiny386's 16bpp writer packs
// native-endian, so every pixel is swapped on the way out. If colours come out
// wrong on hardware, flip this to 0.
#define BLIT_BYTESWAP 1

static uint16_t s_blit[2][PANEL_W * PANEL_H];
static int      s_blit_idx = 0;
static uint16_t s_sx_map[PANEL_W];
static uint16_t s_sy_map[PANEL_H];

static int s_src_w = 0, s_src_h = 0;   // source rect the maps were built for
static int s_xtaps = 1, s_ytaps = 1;   // box-filter taps per axis (1 = point sample)
static int s_smoothing = 1;            // launcher's "Text smoothing" setting

// Direct-to-panel rendering.
//
// The canvas path costs three PSRAM crossings per frame: the VGA writes the
// guest's mode into the 640x480 canvas, then flush() reads it back out and
// scales into the panel buffer. Rendering straight into the panel buffer
// deletes the middle crossing -- about half the video memory traffic.
//
// Inverse of s_sy_map: for each SOURCE row, which output rows it feeds. The map
// is monotonic non-decreasing, so that is always a contiguous range. A source
// row with count 0 is never sampled and the renderer can skip decoding it
// entirely.
#define MAX_SRC_H 480
static uint16_t s_row_first[MAX_SRC_H];
static uint8_t  s_row_count[MAX_SRC_H];
static int s_direct = 0;        // renderer is emitting rows this frame
static int s_panel_ready = 0;   // panel buffer holds a complete frame

// Display pacing. The panel shares its SPI bus with the LoRa radio and the SD
// card, so a push can take 75ms+ under RX load; ~11fps is what the Faux86
// module settled on. render_due() hands the same clock to the caller so a full
// vga_refresh() is only computed for frames we are actually going to show --
// vga_step() signals "refresh due" at 60Hz, so ungated we build (and discard)
// about five frames for every one displayed.
#define BLIT_INTERVAL_US 90000u

static uint32_t s_last_blit_us = 0;
static int s_dirty = 0;                // set by redraw, consumed by dos_video_flush

// Canvas geometry, set once at startup.
static const uint16_t *s_fb = 0;
static int s_fb_w = 0, s_fb_h = 0;

void dos_video_init(const uint16_t *fb, int fb_w, int fb_h)
{
    s_fb = fb;
    s_fb_w = fb_w;
    s_fb_h = fb_h;
}

// Launcher setting. Takes effect on the next mode change; call before the
// emulator starts so the first mode already honours it.
void dos_video_set_smoothing(int on)
{
    s_smoothing = on ? 1 : 0;
    s_src_w = 0;   // force the maps (and tap counts) to rebuild
    s_src_h = 0;
}

// Uncapped (render_gate = 0) reproduces the original behaviour: every 60Hz
// retrace triggers a full re-render whether or not it will be displayed.
static int s_render_gate = 1;

void dos_video_set_render_gate(int on)
{
    s_render_gate = on ? 1 : 0;
}

// True when the next displayed frame is due. The emulation loop uses this to
// decide whether a full vga_refresh() is worth computing.
int dos_video_render_due(void)
{
    if (!s_render_gate) return 1;
    uint32_t now = host_get_ticks_us();
    if (!s_last_blit_us) return 1;
    return (uint32_t)(now - s_last_blit_us) >= BLIT_INTERVAL_US;
}

// SimpleFBDrawFunc. Called from inside vga_refresh() with the dirty rect; the
// actual SPI push happens in dos_video_flush() from the emulation loop, so a
// slow panel write can never re-enter the VGA code.
void dos_video_redraw(void *opaque, int x, int y, int w, int h)
{
    (void)opaque; (void)x; (void)y; (void)w; (void)h;
    s_dirty = 1;
}

static void rebuild_scale_maps(int src_w, int src_h)
{
    // Stretch to fill the whole panel. Aspect-fit letterboxing wastes the
    // scarce horizontal pixels -- the same call the PC-XT module made, and it
    // matters most for 40-column DOS text.
    for (int x = 0; x < PANEL_W; x++)
        s_sx_map[x] = (uint16_t)((uint32_t)x * src_w / PANEL_W);
    for (int y = 0; y < PANEL_H; y++)
        s_sy_map[y] = (uint16_t)((uint32_t)y * src_h / PANEL_H);
    s_src_w = src_w;
    s_src_h = src_h;

    // Box-filter any axis that SHRINKS. Point sampling drops whole source
    // lines: 80-column VGA text is 640x400, so 400->240 discards 2 scanlines
    // in every 5 -- about 40% of a 16-pixel glyph, enough to erase the bottom
    // of a 'b' and leave an 'h'. Averaging the dropped line into its neighbour
    // keeps the stroke as a grey row instead of deleting it.
    //
    // Only on shrinking axes: a 320x200 game is 1:1 across and an UPSCALE
    // down the screen, and blending there would just blur it.
    // "Text smoothing = OFF" in the launcher reverts to point sampling: sharper
    // edges, at the cost of losing whole scanlines out of shrinking modes.
    s_xtaps = (s_smoothing && src_w > PANEL_W) ? 2 : 1;
    s_ytaps = (s_smoothing && src_h > PANEL_H) ? 2 : 1;

    // Invert s_sy_map for the direct path.
    memset(s_row_count, 0, sizeof(s_row_count));
    if (src_h <= MAX_SRC_H) {
        for (int oy = 0; oy < PANEL_H; oy++) {
            int r = s_sy_map[oy];
            if (r < MAX_SRC_H) {
                if (!s_row_count[r]) s_row_first[r] = (uint16_t)oy;
                if (s_row_count[r] < 255) s_row_count[r]++;
            }
        }
    }

    printf("[dos] video mode %dx%d -> %dx%d (taps %dx%d)\n",
           src_w, src_h, PANEL_W, PANEL_H, s_xtaps, s_ytaps);
}

// Called by vga_graphic_refresh() once the mode rect is known. Returns 1 if it
// should emit rows through dos_video_emit_row() instead of writing the canvas.
//
// Deliberately narrow: only the point-sampled case. Box filtering needs two
// source rows, which a source-driven walk does not have, and text mode renders
// through a different path that still uses the canvas.
int dos_video_direct_begin(int src_w, int src_h)
{
    s_direct = 0;
    if (!s_fb || src_w <= 0 || src_h <= 0 || src_h > MAX_SRC_H) return 0;
    if (src_w != s_src_w || src_h != s_src_h)
        rebuild_scale_maps(src_w, src_h);
    if (s_xtaps != 1 || s_ytaps != 1) return 0;
    s_direct = 1;
    return 1;
}

// One decoded source row. `idx` is the guest's 8-bit chunky scanline, `pal` the
// 256-entry RGB565 palette; xdiv is the horizontal pixel doubling the mode is
// programmed with. Writes every output row this source row feeds.
void dos_video_emit_row(int src_y, const uint8_t *idx, const uint32_t *pal,
                        int xdiv)
{
    if (!s_direct || src_y < 0 || src_y >= MAX_SRC_H) return;
    int n = s_row_count[src_y];
    if (!n) return;                       // this source row is never sampled

    uint16_t *dst = &s_blit[s_blit_idx][(uint32_t)s_row_first[src_y] * PANEL_W];

    if (xdiv == 1 && s_src_w == PANEL_W) {
        // The overwhelmingly common case: 320-wide source, 1:1 across.
        for (int ox = 0; ox < PANEL_W; ox++) {
            uint16_t c = (uint16_t)pal[idx[ox]];
#if BLIT_BYTESWAP
            c = (uint16_t)((c >> 8) | (c << 8));
#endif
            dst[ox] = c;
        }
    } else if (xdiv == 1) {
        for (int ox = 0; ox < PANEL_W; ox++) {
            uint16_t c = (uint16_t)pal[idx[s_sx_map[ox]]];
#if BLIT_BYTESWAP
            c = (uint16_t)((c >> 8) | (c << 8));
#endif
            dst[ox] = c;
        }
    } else {
        for (int ox = 0; ox < PANEL_W; ox++) {
            uint16_t c = (uint16_t)pal[idx[s_sx_map[ox] / xdiv]];
#if BLIT_BYTESWAP
            c = (uint16_t)((c >> 8) | (c << 8));
#endif
            dst[ox] = c;
        }
    }

    // Upscaling: this source row also fills the rows below it.
    for (int k = 1; k < n; k++)
        memcpy(dst + (uint32_t)k * PANEL_W, dst, PANEL_W * sizeof(uint16_t));
}

// Only a frame that completed is publishable -- a mode change mid-render would
// otherwise push a half-filled panel.
void dos_video_direct_end(int complete)
{
    if (s_direct && complete) s_panel_ready = 1;
    s_direct = 0;
}

// Push one frame if the VGA marked the canvas dirty. Rate-capped: the panel
// shares its SPI bus with the LoRa radio and the SD card, so a push can take
// 75ms+ under RX load. Without the cap, pushes pile up inside a single
// emulation step and stall the CPU (measured on the Faux86 module as
// sim=2800ms/loop). ~11fps of real pushes; excess dirty frames are dropped.
void dos_video_flush(void)
{
    if (!s_dirty || !s_fb) return;

    uint32_t now_us = host_get_ticks_us();
    if (s_last_blit_us && (uint32_t)(now_us - s_last_blit_us) < BLIT_INTERVAL_US)
        return;
    s_last_blit_us = now_us;
    s_dirty = 0;

    // Direct path: the renderer already wrote the panel buffer, so the whole
    // sample-and-scale pass below is skipped.
    if (s_panel_ready) {
        s_panel_ready = 0;
        host_blit_frame_async(s_blit[s_blit_idx], PANEL_W, PANEL_H);
        s_blit_idx ^= 1;
        return;
    }

    // Active-mode rect inside the canvas. Before the first mode is programmed
    // (w/h still zero) fall back to the whole canvas so a blank boot screen
    // still blits rather than dividing by zero.
    int sx = meshpunk_vga_x, sy = meshpunk_vga_y;
    int sw = meshpunk_vga_w, sh = meshpunk_vga_h;
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = s_fb_w; sh = s_fb_h; }
    if (sx + sw > s_fb_w) sw = s_fb_w - sx;
    if (sy + sh > s_fb_h) sh = s_fb_h - sy;
    if (sw <= 0 || sh <= 0) return;

    if (sw != s_src_w || sh != s_src_h)
        rebuild_scale_maps(sw, sh);

    uint16_t *dst_fb = s_blit[s_blit_idx];
    const uint16_t *src_base = s_fb + (uint32_t)sy * s_fb_w + sx;

    // Second tap stays inside the source rect; on the last row/column it
    // collapses back onto the first, which averages a pixel with itself.
    const int xt = s_xtaps, yt = s_ytaps;
    const int max_row = sh - 1, max_col = sw - 1;

    int prev_sy = -1;
    const uint16_t *prev_row = 0;
    for (int oy = 0; oy < PANEL_H; oy++) {
        uint16_t *dst = &dst_fb[oy * PANEL_W];
        int row = s_sy_map[oy];
        if (row == prev_sy) {
            // Same source row(s) as the line above: copy the finished output
            // row instead of re-sampling it. Still valid with filtering on,
            // because the tap count is constant for the whole frame.
            memcpy(dst, prev_row, PANEL_W * sizeof(uint16_t));
        } else {
            int row2 = (yt > 1 && row < max_row) ? row + 1 : row;
            const uint16_t *src  = src_base + (uint32_t)row  * s_fb_w;
            const uint16_t *src2 = src_base + (uint32_t)row2 * s_fb_w;

            if (xt == 1 && yt == 1 && sw == PANEL_W) {
                // Identity map: a 320-wide source samples 1:1, so s_sx_map[x]
                // is exactly x. Reading it anyway costs a load per pixel and
                // hides from the compiler that the source walk is sequential.
                // This is the case every 320x200 game hits.
                for (int ox = 0; ox < PANEL_W; ox++) {
                    uint16_t c = src[ox];
#if BLIT_BYTESWAP
                    c = (uint16_t)((c >> 8) | (c << 8));
#endif
                    dst[ox] = c;
                }
            } else if (xt == 1 && yt == 1) {
                // Point sample, non-identity horizontal map.
                for (int ox = 0; ox < PANEL_W; ox++) {
                    uint16_t c = src[s_sx_map[ox]];
#if BLIT_BYTESWAP
                    c = (uint16_t)((c >> 8) | (c << 8));
#endif
                    dst[ox] = c;
                }
            } else {
                for (int ox = 0; ox < PANEL_W; ox++) {
                    int col = s_sx_map[ox];
                    int col2 = (xt > 1 && col < max_col) ? col + 1 : col;
                    // Average 2 or 4 RGB565 samples per output pixel. The tap
                    // count is a power of two, so the divide is a shift.
                    uint32_t r = 0, g = 0, b = 0;
                    uint16_t s0 = src[col],  s1 = src[col2];
                    uint16_t s2 = src2[col], s3 = src2[col2];
                    r  = ((s0 >> 11) & 0x1F) + ((s1 >> 11) & 0x1F)
                       + ((s2 >> 11) & 0x1F) + ((s3 >> 11) & 0x1F);
                    g  = ((s0 >> 5) & 0x3F) + ((s1 >> 5) & 0x3F)
                       + ((s2 >> 5) & 0x3F) + ((s3 >> 5) & 0x3F);
                    b  = (s0 & 0x1F) + (s1 & 0x1F)
                       + (s2 & 0x1F) + (s3 & 0x1F);
                    // s1/s3 duplicate s0/s2 when xt==1, and s2/s3 duplicate
                    // s0/s1 when yt==1, so the sum is always over 4 terms.
                    uint16_t c = (uint16_t)(((r >> 2) << 11)
                                          | ((g >> 2) << 5)
                                          |  (b >> 2));
#if BLIT_BYTESWAP
                    c = (uint16_t)((c >> 8) | (c << 8));
#endif
                    dst[ox] = c;
                }
            }
            prev_sy = row;
        }
        prev_row = dst;
    }

    host_blit_frame_async(dst_fb, PANEL_W, PANEL_H);
    s_blit_idx ^= 1;
}
