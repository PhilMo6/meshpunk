// Screen-space 2D overlay for the jet3d module: text and filled rectangles.
//
// Band rendering never materialises a whole frame, so anything screen-space has
// to be drawn per band. Calling into Lua once per band to re-issue draw calls
// would multiply the script cost by the band count, so instead the game's
// jet.draw() runs ONCE per frame and records a display list here; the band loop
// then replays that list, clipping each command to the rows the current band
// covers. Lua stays at one crossing per frame and the output is identical to
// drawing against a full framebuffer.
//
// Storage is a fixed pool — nothing here allocates during a frame.
//
// Jet's own Sprite2D covers textured/solid overlay quads and is now band-aware
// too (Scene::drawSprites clips to yBandMin/yBandMax). This layer exists for
// text, which Jet has no facility for at all, and for cheap solid rectangles
// that would otherwise need a Material and a registered sprite each.

#ifndef JET_OVERLAY_H
#define JET_OVERLAY_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// Drop every recorded command. Called once per frame before jet.draw().
void jet_ovl_begin(void);

// Record a string at (x, y) in screen pixels, y being the top of the cell.
// `scale` >= 1 multiplies the glyph size. Text is copied into the pool, so the
// caller's buffer need not outlive the call. Silently ignored when either pool
// is full.
void jet_ovl_text(int x, int y, uint16_t color, int scale, const char* s);

// Record a filled rectangle in screen pixels.
void jet_ovl_rect(int x, int y, int w, int h, uint16_t color);

// Replay the list into one band. `band` points at the first pixel of row `y0`;
// rows [y0, y1) are writable. Commands are clipped to that range and to the
// screen width.
void jet_ovl_draw_band(uint16_t* band, int screenW, int y0, int y1);

// Rendered width in pixels of `s` at `scale`, for right-aligning or centring.
int jet_ovl_text_width(int scale, const char* s);

// Cell metrics of the built-in font, in pixels at scale 1.
#define JET_OVL_GLYPH_W 6
#define JET_OVL_GLYPH_H 8

#if defined(__cplusplus)
}
#endif

#endif // JET_OVERLAY_H
