// host_osk.h — host-rendered on-screen keyboard for ELF module runs.
//
// LVGL is dead while a module owns the device, so this keyboard is raw
// pixels: a full-width panel over the bottom of the 320x240 module screen,
// rendered once into a cached PSRAM frame and pushed via the same
// display-push signature the blit paths use. Its keys are input_zones
// entries whose OUT codes are plain ASCII (plus Enter/Backspace/Esc), so
// key edges ride the exact zone->kq_push path controller zones use.
//
// The game keeps rendering while the keyboard is open: the ELF host stamps
// the cached keyboard rows INTO every module buffer that overlaps the
// region before it is pushed (host_osk_stamp — one push carries game +
// keyboard, so nothing alternates on the panel; a post-push re-push
// flickered at the module's frame rate). Parts outside the module's frame
// live in the letterbox borders, painted once at open and repainted via
// host_osk_maintain when the module clears the screen. Closing black-fills
// the region; the module's own drawing recovers it (a full-frame renderer
// on its next frame, DOS when its text scrolls).
//
// Single consumer (elf_host), but NOT single-threaded: open/close/feedback
// run on the Core-1 input task (priority 5) while stamp/maintain run on the
// module thread (Core 0) or the Core-1 blit task (priority 3, which the
// input task preempts). The cached frame is therefore never freed by close —
// only by host_osk_free() at module teardown, which runs after blit_drain()
// with nothing else in flight. Closing merely marks the keyboard inactive,
// so a stamp already in progress finishes against live memory instead of a
// freed block.

#pragma once

#include <stdint.h>
#include "input_zones.h"

// The keyboard's zone table (OUT = ASCII / 0x0D / 0x08 / 0x1B).
void host_osk_zones(const InputZone** zones, int* count);

// Render + push the keyboard. False if the frame allocation failed (the
// caller must then not arm the OSK zones). push = display_dev_blit.
bool host_osk_open(void (*push)(int x, int y, int w, int h, const uint16_t* px));

// Mark the keyboard inactive and black-fill its region (push may be NULL to
// skip the panel work). Keeps the buffer — see the threading note above.
void host_osk_close(void (*push)(int x, int y, int w, int h, const uint16_t* px));

// Release the cached frame. ONLY at module teardown, after blit_drain().
void host_osk_free(void);

bool host_osk_active(void);

// Stamp the cached keyboard pixels (opaque) into a module buffer positioned
// at screen (fx,fy) sized fw x fh, clipped — called BEFORE that buffer is
// pushed. Both sides are in wire byte order, so it's a straight row copy.
void host_osk_stamp(uint16_t* frame, int fx, int fy, int fw, int fh);

// Directly re-push the cached rows inside screen rect (bx,by,bw,bh) — for
// repaints with no module buffer in hand (host_clear_screen).
void host_osk_maintain(int bx, int by, int bw, int bh,
                       void (*push)(int x, int y, int w, int h, const uint16_t* px));

// Pressed-key highlight: redraw one key tile (by its OUT code) inverted or
// normal, in the cached frame and on the panel.
void host_osk_key_feedback(uint8_t out, bool pressed,
                           void (*push)(int x, int y, int w, int h, const uint16_t* px));
