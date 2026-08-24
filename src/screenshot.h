// screenshot.h — screen capture to PNG, shared by the LVGL and ELF worlds.
//
// One RGB565 staging buffer (320x240, PSRAM, allocated per capture) is filled
// by whichever feeder is armed, then written out as a PNG row by row. The two
// feeders are:
//
//   LVGL world   disp_flush_cb (main.cpp) hands over each flushed rect while a
//                capture is armed, and skips the panel write for that refresh.
//   ELF world    host_blit_frame / host_blit_rect (elf_host.cpp) hand over the
//                module's source pixels BEFORE the zone/OSK overlays are
//                stamped into them, so a captured game frame carries neither.
//
// PIXEL FORMAT: both feeders deliver BIG-ENDIAN RGB565 — LVGL because
// LV_COLOR_16_SWAP is 1 (it byte-swaps the whole framebuffer at flush), the
// modules because they build their palettes pre-swapped for the SPI panel.
// The staging buffer holds that same wire order; the swap happens once, at
// write time.
//
// The writer streams: it holds ~10KB of PSRAM scratch and never materialises
// the filtered image or the compressed output. See screenshot.cpp.
//
// THREADING: the staging buffer is a single global, so one capture at a time.
// screenshot_begin() fails while another is in flight. Callers are the LVGL
// thread (loop's dispatcher) and, during a module run, the ELF capture task —
// never both, because loop() is blocked inside elf_host_run_pending() for the
// whole run.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Claim the staging buffer and fill it black. False when a capture is already
// in flight or PSRAM is short (153,600 bytes).
bool screenshot_begin(void);

// True while a capture owns the staging buffer — still collecting, or being
// written out. Tells the two failure causes of screenshot_begin apart, so a
// refusal is never reported as "low memory" when it was really "still busy".
bool screenshot_busy(void);

// Copy a rectangle of big-endian RGB565 into the staging buffer at (x, y).
// Clipped to the panel; a rect wholly outside is dropped. No-op when no
// capture is in flight.
void screenshot_feed_be565(const uint16_t* px, int x, int y, int w, int h);

// Encode the staging buffer to `vfs_path` (a POSIX path: "/sd/..." or
// "/littlefs/..."). Logs dimensions, file size and elapsed ms.
bool screenshot_write_png(const char* vfs_path);

// Release the staging buffer. Safe to call when no capture is in flight.
void screenshot_end(void);

// Build the next screenshot path into `out` and create its directory:
//   /sd/screenshots/shot_YYYYMMDD_HHMMSS.png   (SD mounted, not shared to a PC)
//   /littlefs/screenshots/shot_...             (fallback)
// A _NN suffix resolves same-second collisions. When the RTC has no time the
// name falls back to a boot-relative counter (shot_0001.png). Returns false
// when no drive can take the file — including while USB drive mode owns the
// card — with the reason in `out`.
bool screenshot_make_path(char* out, size_t n);

// Whole capture in one call: begin + the feeder's work is the caller's, this
// is only the tail (write + end). Kept separate so the ELF path can run the
// write on its own task after the module's blit filled the buffer.
bool screenshot_finish_to_disk(char* path_out, size_t path_n);
