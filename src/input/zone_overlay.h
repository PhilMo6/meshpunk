// zone_overlay.h — pre-rendered indicator chips for touch-controller zones.
//
// Board-agnostic pixel work only: builds one small label box per zone
// (centered in the zone) as outline + label + a 1px drop shadow on a
// TRANSPARENT background. Buffer encoding: 0x0000 = transparent, everything
// else is foreground. All foreground colors are byte-swap-invariant RGB565
// values (0xFFFF white, 0x2121 shadow), so the same buffer is correct inside
// a module's byte-swapped frame and on a direct display_dev_blit — where
// transparent pixels render as plain black (direct pushes land in the black
// letterbox borders, so nothing is visibly filled).
//
// Single consumer (the ELF host's controller mode); not thread-safe beyond
// build/clear happening while no stamper is running.

#pragma once

#include <stdint.h>
#include "input_zones.h"

typedef struct {
    int16_t   x, y;     // screen position (top-left)
    int16_t   w, h;     // chip size
    uint16_t* buf;      // RGB565 chip pixels (PSRAM); 0x0000 = transparent
} ZoneChip;

// Build chips for every zone with a non-empty label (MODE pseudo-zones are
// skipped). Frees any previous set. Allocation failures skip that chip.
void zone_overlay_build(const InputZone* zones, int count);
void zone_overlay_clear(void);

int             zone_overlay_count(void);
const ZoneChip* zone_overlay_chip(int i);

// Stamp every chip that intersects a frame positioned at screen (fx,fy) with
// size fw x fh into that RGB565 frame buffer (clipped; transparent pixels
// leave the frame untouched).
void zone_overlay_stamp(uint16_t* frame, int fx, int fy, int fw, int fh);

// Push (or erase, with black) every chip PART lying outside that frame rect,
// via push(x, y, w, h, pixels) — display_dev_blit's exact signature. Covers
// the letterbox borders a module never redraws; chips fully inside the frame
// are untouched (the stamp path owns them).
void zone_overlay_push_outside(int fx, int fy, int fw, int fh, bool erase,
                               void (*push)(int x, int y, int w, int h,
                                            const uint16_t* px));

// Shared 5x7 text renderer (also used by the host OSK): draw `s` into an
// RGB565 buffer of row-stride `bw` at (x,y), scaled, in `color` — the caller
// picks wire-order colors to match the buffer's byte order. Unknown glyphs
// render blank. Advance per char is 6*scale px; glyph height 7*scale.
void zone_overlay_draw_text(uint16_t* buf, int bw, int x, int y, int scale,
                            const char* s, uint16_t color);
