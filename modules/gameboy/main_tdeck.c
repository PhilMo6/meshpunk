// Entry point + platform glue for the gnuboy Game Boy / Game Boy Color
// ELF module on T-Deck. Same pattern as Doom/PICO-8: trap exit(), own the
// main loop, poll input, blit async, push audio, yield every frame.
//
// argv: <rom_vfs_path> [-pal N] [-scale 1x|fit|full] [-resume 0|1]
//   rom_vfs_path : /sd/... or /littlefs/...
//   -pal N       : DMG colorization palette (gb_palette_t index, 0..36)
//   -scale       : 1x = 160x144 centered, fit = 240x216 (default),
//                  full = 320x240 stretch
//   -resume 0|1  : load/save a full state next to the ROM on start/exit

#include "gnuboy-src/gnuboy.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// hw.h gives us the GB global for a cheap pre-reset sanity guard.
// Beware: it defines a `host` macro, so no identifier in this file may be
// named bare `host`.
#include "gnuboy-src/hw.h"

// ---------------------------------------------------------------------------
// Host function imports (resolved at ELF load time by elf_host.cpp)
// ---------------------------------------------------------------------------
extern void     host_blit_frame_async(const uint16_t* rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);
extern void     host_audio_push(const int16_t* samples, int count, int sample_rate);
extern uint32_t host_psram_largest_free(void);

// ---------------------------------------------------------------------------
// Exit/abort traps. gnuboy calls abort() if a mid-game ROM bank read fails
// (SD card failure); neither abort nor exit is host-exported, so both must
// longjmp back to main() instead of taking down the firmware.
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

void exit(int code) {
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

void abort(void) {
    host_log("gameboy: abort() called (SD read failure?)");
    s_exit_code = 1;
    longjmp(s_exit_jmp, 1);
}

// ---------------------------------------------------------------------------
// Display: gnuboy renders 160x144 RGB565 (already byte-swapped via
// GB_PIXEL_565_BE) into s_gbfb; the vblank callback upscales into one of two
// blit buffers and hands it to the Core-1 SPI task (double-buffered, same
// arrangement as PICO-8's drawFrame).
// ---------------------------------------------------------------------------
#define OUT_MAX_W 320
#define OUT_MAX_H 240

static uint16_t s_gbfb[GB_WIDTH * GB_HEIGHT];
static uint16_t s_blit[2][OUT_MAX_W * OUT_MAX_H];
static int      s_fb_idx = 0;

static int     s_out_w = 240;
static int     s_out_h = 216;
static uint8_t s_sx_map[OUT_MAX_W]; // output col -> source col (0..159)
static uint8_t s_sy_map[OUT_MAX_H]; // output row -> source row (0..143)

static void build_scale_maps(const char* mode) {
    if (mode && strcmp(mode, "1x") == 0) {
        s_out_w = GB_WIDTH;
        s_out_h = GB_HEIGHT;
    } else if (mode && strcmp(mode, "full") == 0) {
        s_out_w = 320;
        s_out_h = 240;
    } else { // "fit" — aspect-true 1.5x
        s_out_w = 240;
        s_out_h = 216;
    }
    for (int ox = 0; ox < s_out_w; ox++)
        s_sx_map[ox] = (uint8_t)(ox * GB_WIDTH / s_out_w);
    for (int oy = 0; oy < s_out_h; oy++)
        s_sy_map[oy] = (uint8_t)(oy * GB_HEIGHT / s_out_h);
}

// Called by gnuboy_run() at vblank with the finished frame.
static void video_cb(void* buffer) {
    const uint16_t* src = (const uint16_t*)buffer;
    uint16_t* fb = s_blit[s_fb_idx];

    if (s_out_w == GB_WIDTH && s_out_h == GB_HEIGHT) {
        // 1x: copy out so emulation can keep rendering while SPI pushes
        memcpy(fb, src, GB_WIDTH * GB_HEIGHT * sizeof(uint16_t));
    } else {
        // Nearest-neighbor scale; duplicate output rows are memcpy'd
        int prev_sy = -1;
        const uint16_t* prev_row = NULL;
        for (int oy = 0; oy < s_out_h; oy++) {
            uint16_t* row = &fb[oy * s_out_w];
            int sy = s_sy_map[oy];
            if (sy == prev_sy) {
                memcpy(row, prev_row, s_out_w * sizeof(uint16_t));
            } else {
                const uint16_t* srow = &src[sy * GB_WIDTH];
                for (int ox = 0; ox < s_out_w; ox++)
                    row[ox] = srow[s_sx_map[ox]];
                prev_sy = sy;
            }
            prev_row = row;
        }
    }

    host_blit_frame_async(fb, s_out_w, s_out_h);
    s_fb_idx ^= 1;
}

// ---------------------------------------------------------------------------
// Audio: gnuboy fills s_abuf (mono S16 @ 22050 Hz) during gnuboy_run() and
// fires this callback with the sample count — push model, same as Doom.
// The firmware mixer upsamples 2x to 44100 and applies volume/mute.
// ---------------------------------------------------------------------------
#define AUDIO_RATE 22050
#define ABUF_LEN   1024 // int16 entries; ~369 samples/frame at 22050 Hz

// The first ~second of frames runs slow while the PSRAM instruction cache
// warms up, so audio would reach the mixer in late bursts (audible garble —
// masked by silent logo screens on fresh boot, but front and center when a
// resume drops straight into music). Swallow pushes until the loop is warm.
#define AUDIO_WARMUP_FRAMES 60

static int16_t s_abuf[ABUF_LEN];
static int     s_audio_warmup = AUDIO_WARMUP_FRAMES;

static void audio_cb(void* buffer, size_t length) {
    if (s_audio_warmup > 0) return;
    host_audio_push((const int16_t*)buffer, (int)length, AUDIO_RATE);
}

// ---------------------------------------------------------------------------
// Input: canonical key codes -> GB pad bits. The launcher's -keymap remaps
// physical keys onto these before they reach the module (PICO-8 pattern).
// ---------------------------------------------------------------------------
static int s_pad = 0;

static int map_key(unsigned char key) {
    switch (key) {
        // Trackball pseudo-codes + WASD
        case 0x81: case 'w': return GB_PAD_UP;
        case 0x82: case 's': return GB_PAD_DOWN;
        case 0x83: case 'a': return GB_PAD_LEFT;
        case 0x84: case 'd': return GB_PAD_RIGHT;
        // Buttons
        case 0x85: case 'm': return GB_PAD_A;      // trackball click / M
        case 'n':            return GB_PAD_B;
        case 0x0D:           return GB_PAD_START;  // Enter
        case ' ':            return GB_PAD_SELECT; // Space
        default:             return 0;
    }
}

static void poll_input(void) {
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        int bit = map_key(key);
        if (!bit) continue;
        if (pressed) s_pad |= bit;
        else         s_pad &= ~bit;
    }
    gnuboy_set_pad(s_pad);
}

// ---------------------------------------------------------------------------
// Save paths: <rom minus extension> + .sav / .sta, next to the ROM.
// ---------------------------------------------------------------------------
#define PATH_MAX_LEN 256

static char s_sav_path[PATH_MAX_LEN];
static char s_sta_path[PATH_MAX_LEN];

static void make_side_path(const char* rom, const char* ext, char* out) {
    const char* dot = strrchr(rom, '.');
    const char* slash = strrchr(rom, '/');
    size_t len = strlen(rom);
    if (dot && (!slash || dot > slash))
        len = (size_t)(dot - rom);
    if (len > PATH_MAX_LEN - 8)
        len = PATH_MAX_LEN - 8;
    memcpy(out, rom, len);
    strcpy(out + len, ext);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
#define FRAME_US 16743 // 70224 clocks @ 4.194 MHz = 59.73 Hz
#define MAX_SKIP 3     // max consecutive undrawn frames when behind

int main(int argc, char** argv) {
    host_log("gameboy: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("gameboy: exit()/abort() caught, returning to launcher");
        return s_exit_code;
    }

    // --- Parse arguments ---
    const char* rom_path = NULL;
    const char* scale_mode = "fit";
    int pal = -1;
    int resume = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-pal") == 0 && i + 1 < argc) {
            pal = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-scale") == 0 && i + 1 < argc) {
            scale_mode = argv[++i];
        } else if (strcmp(argv[i], "-resume") == 0 && i + 1 < argc) {
            resume = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            // Unknown flag (-keymap/-trkball are host-consumed but stay in
            // argv): skip it and its value so the value isn't taken as the ROM
            if (i + 1 < argc) i++;
        } else {
            rom_path = argv[i];
        }
    }

    if (!rom_path) {
        host_log("gameboy: no ROM path given");
        return 1;
    }
    printf("[gameboy] rom=%s scale=%s pal=%d resume=%d\n",
           rom_path, scale_mode, pal, resume);
    printf("[gameboy] psram largest free: %u\n",
           (unsigned)host_psram_largest_free());

    // --- Bring up the emulator ---
    if (gnuboy_init(AUDIO_RATE, GB_AUDIO_MONO_S16, GB_PIXEL_565_BE,
                    video_cb, audio_cb) < 0) {
        host_log("gameboy: gnuboy_init failed");
        return 1;
    }
    gnuboy_set_framebuffer(s_gbfb);
    gnuboy_set_soundbuffer(s_abuf, ABUF_LEN);
    build_scale_maps(scale_mode);
    if (pal >= 0 && pal < GB_PALETTE_COUNT)
        gnuboy_set_palette((gb_palette_t)pal);

    if (gnuboy_load_rom_file(rom_path) < 0) {
        host_log("gameboy: ROM load failed");
        gnuboy_free_rom();
        return 1;
    }

    // Sanity guard: bail cleanly instead of letting gnuboy_reset() memset a
    // NULL bank pointer and spin the watchdog (caught a loader reloc bug once).
    if (!GB.rambanks || !GB.vbanks) {
        host_log("gameboy: GB state invalid before reset — aborting cleanly");
        gnuboy_free_rom();
        return 1;
    }
    gnuboy_reset(true);

    make_side_path(rom_path, ".sav", s_sav_path);
    make_side_path(rom_path, ".sta", s_sta_path);

    if (gnuboy_load_sram(s_sav_path) == 0)
        printf("[gameboy] loaded battery save %s\n", s_sav_path);
    if (resume && gnuboy_load_state(s_sta_path) == 0)
        printf("[gameboy] resumed state %s\n", s_sta_path);

    host_clear_screen();
    host_log("gameboy: entering main loop");

    // --- Main loop: pace to 59.73 Hz, skip draws to catch up when behind ---
    uint32_t next_us = host_get_ticks_us(); // deadline of the frame about to run
    uint32_t frames = 0, skipped = 0;
    int consec_skips = 0;

    while (!host_should_exit()) {
        poll_input();

        int32_t behind = (int32_t)(host_get_ticks_us() - next_us);
        int draw = 1;
        if (behind > (int32_t)FRAME_US && consec_skips < MAX_SKIP) {
            draw = 0;
            consec_skips++;
        } else {
            consec_skips = 0;
        }
        // Hopelessly behind (SD bank load / SRAM save hiccup): resync
        // instead of frameskip-spiraling to catch up on lost time.
        if (behind > (int32_t)(8 * FRAME_US))
            next_us = host_get_ticks_us();

        gnuboy_run(draw != 0);
        next_us += FRAME_US;
        frames++;
        if (!draw) skipped++;
        if (s_audio_warmup > 0) s_audio_warmup--;

        // Battery autosave: full save (quick_save truncates the file but
        // skips clean banks, so it would drop them — always save fully)
        if ((frames % 600) == 0 && gnuboy_sram_dirty()) {
            gnuboy_save_sram(s_sav_path, false);
            printf("[gameboy] battery autosaved (frame %u)\n", (unsigned)frames);
        }

        int32_t ahead = (int32_t)(next_us - host_get_ticks_us());
        if (ahead > 2000)
            host_sleep_ms((uint32_t)(ahead - 1000) / 1000);
        else
            host_sleep_ms(1); // always yield: watchdog + Core 1 SPI time
    }

    // --- Teardown ---
    printf("[gameboy] exiting: %u frames, %u skipped\n",
           (unsigned)frames, (unsigned)skipped);

    gnuboy_save_sram(s_sav_path, false); // no-op unless cart has battery
    if (resume) {
        if (gnuboy_save_state(s_sta_path) == 0)
            printf("[gameboy] state saved to %s\n", s_sta_path);
    }

    gnuboy_free_rom();
    host_clear_screen();
    host_log("gameboy: module done");
    return 0;
}
