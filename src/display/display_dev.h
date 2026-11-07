// display_dev.h — per-device display backend contract.
//
// Same selection model as input_dev.h: exactly ONE backend implementation is
// compiled per build via the board define (-DBOARD_TDECK ->
// display_tdeck.cpp). The backend owns the panel driver object, its init and
// flush path (including any bus locking and tear-sync the board topology
// needs), the backlight hardware, and the panel dimensions. Callers never
// see the driver library or the pins.
//
// MODULE VIDEO CONTRACT: 320x240 RGB565 is the standard blit target every
// ELF module is built against (host_blit_frame/host_blit_rect in
// elf_host.cpp). On boards whose panel differs, the backend — not the
// module, not elf_host — is where scaling/letterboxing happens, so the
// module ABI never changes per device.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Panel init: driver begin + rotation + clear to black. Called right after
// the shared SPI bus is begun in setup(), before the SD card and the radio.
void display_dev_init(void);

// Boot splash: the MeshPunk wordmark centered on the cleared panel, drawn
// with the panel driver's own font before LVGL exists. LVGL's first frame
// replaces it.
void display_dev_splash(void);

// Active (post-rotation) panel dimensions in pixels. These follow the
// EFFECTIVE orientation below, so LVGL sizing, elf_host bounds checks and
// _device_caps all read the same geometry.
int display_dev_width(void);
int display_dev_height(void);

// User display orientation: quarter turns from the board's native landscape
// (0 = landscape as shipped, 1/3 = the two portraits, 2 = landscape flipped).
// set applies the panel rotation and clears to black — callers apply it once
// after the prefs load and BEFORE LVGL is created; a later change persists
// the pref and takes effect on restart. The getter returns the EFFECTIVE
// orientation: 0 while a module video session is active, the user setting
// otherwise.
void    display_dev_set_orientation(uint8_t o);
uint8_t display_dev_orientation(void);

// Module video session: ELF modules are built against the landscape 320x240
// contract (see above), so entering a session forces the panel to native
// landscape and makes width/height/orientation report it; leaving restores
// the user setting. Both edges clear the panel to black.
void display_dev_module_video(bool active);

// Map a point from native-landscape screen coordinates into the effective
// orientation's coordinates. The input backends run every touch point
// through this after their board transform (which produces landscape
// coords); identity at effective orientation 0, so module sessions and
// their touch overlays are untouched.
void display_dev_orient_point(int16_t* x, int16_t* y);

// LVGL flush path: write a rectangle of RGB565 pixels at (x, y). The backend
// owns whatever bus locking / tear-sync the board needs; the caller only
// signals LVGL when this returns.
void display_dev_flush_rect(int x, int y, int w, int h, const uint16_t* px);

// Raw blit primitive for the ELF module video path: push pixels, nothing
// else. NO bounds policy here — callers (elf_host) keep their own contract
// checks, exactly as before the split. Takes the bus lock internally.
void display_dev_blit(int x, int y, int w, int h, const uint16_t* px);

// Clear the whole panel to black (module exit).
void display_dev_fill_black(void);

// Backlight: pin/controller setup, then brightness 0-16 (0 = off; the
// T-Deck's pulse-counted chip gives 16 levels — other backends map their
// PWM range onto the same 0-16 scale so the persisted pref stays portable).
void display_dev_backlight_init(void);
void display_dev_brightness(uint8_t value);

// Panel sleep-in/sleep-out for standby. Frame memory survives it on the
// ST7789 boards (SLPIN 0x10 / SLPOUT 0x11) but not on every panel — the
// Wio L2's NV3031B wakes through a software reset — so callers repaint the
// UI after waking. Callers pair this with display_dev_brightness().
void display_dev_sleep(bool sleep);

// Full known-state backlight re-init to `value`. The T-Deck's pulse-counted
// chip can end up dark while the driver's level tracking says lit (any
// glitch that trips its shutdown threshold does it, and the incremental
// path then pulses zero times forever) — this forces a guaranteed shutdown
// and a fresh turn-on, resynchronizing chip and driver unconditionally.
// Use when resuming from a long off period (standby exit); costs ~5ms.
void display_dev_backlight_reset(uint8_t value);
