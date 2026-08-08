// Entry point + platform glue for the SMS Plus (Sega Game Gear / Master
// System / SG-1000) ELF module on T-Deck. Same pattern as the gnuboy and
// NGPC modules: trap exit(), own the main loop, poll input, blit async,
// push audio, yield every frame.
//
// argv: <rom_vfs_path> [-scale 1x|fit|full]
//   rom_vfs_path : /sd/... or /littlefs/...
//   -scale       : 1x = native centered, fit = aspect-true upscale
//                  (default), full = 320x240 stretch
//
// The launcher's -keymap remaps physical keys onto the canonical codes below
// before they reach this module, and -trkball/-stackkb are consumed by the
// host, so both are ignored here.

#include "smsplus-src/shared.h"

#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Host function imports (resolved at ELF load time by elf_host.cpp)
// ---------------------------------------------------------------------------
extern void     host_blit_frame_async(const uint16_t *rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int *pressed, unsigned char *key);
extern int      host_should_exit(void);
extern void     host_log(const char *msg);
extern void     host_audio_push(const int16_t *samples, int count, int sample_rate);

// ---------------------------------------------------------------------------
// Exit traps — the core calls abort() on a failed allocation.
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int     s_exit_code = 0;

void exit(int code) { s_exit_code = code; longjmp(s_exit_jmp, 1); }
void abort(void) { host_log("sega8: abort()"); s_exit_code = 1; longjmp(s_exit_jmp, 1); }

// ---------------------------------------------------------------------------
// CRC32 (IEEE, reflected) for loadrom.c's per-game database. Upstream stubs
// this to 0 outside retro-go, which quietly disables every per-game fix.
// Table is built once on first use rather than stored: 1KB of PSRAM beats
// 1KB in the module image, and it runs exactly once per ROM load.
// ---------------------------------------------------------------------------
uint32 meshpunk_crc32(uint32 crc, const uint8 *buf, uint32 len)
{
    static uint32 table[256];
    static int    built = 0;

    if (!built) {
        for (uint32 i = 0; i < 256; i++) {
            uint32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }

    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------------------
// Display. The core renders 8-bit palette indices into a 256x192 buffer and
// reports the visible window in bitmap.viewport — 160x144 at x=48 for Game
// Gear, the full 256x192 for Master System. We convert the cropped region
// through a 256-entry RGB565 table into one of two blit buffers and hand it
// to the Core-1 SPI task.
//
// palette_sync() already byte-swaps each entry (color << 8 | color >> 8), so
// the table is in TFT wire order and no per-pixel swap is needed here.
// ---------------------------------------------------------------------------
#define OUT_MAX_W 320
#define OUT_MAX_H 240

// Rows the core can write. SMS_HEIGHT is only the base 192-line mode; the VDP
// also does 224 and 240, and render_line's row index is bounded by
// viewport.h, which follows vdp.height. Sizing this at SMS_HEIGHT overflows
// the buffer by up to 12KB on an extended-mode Master System game. Game Gear
// never exceeds its fixed 144.
#define FB_H 240

static uint8_t  *s_indexed = NULL;              // 256xFB_H, 8bpp, core writes
static uint16_t  s_palette[256];                // BE RGB565, 32 entries x8
static uint16_t  s_blit[2][OUT_MAX_W * OUT_MAX_H];
static int       s_fb_idx = 0;

static int       s_src_x = 0, s_src_w = GG_WIDTH, s_src_h = GG_HEIGHT;
static int       s_out_w = 0, s_out_h = 0;
static const char *s_scale_mode = "fit";

static uint8_t   s_sx_map[OUT_MAX_W];           // output col -> source col
static uint8_t   s_sy_map[OUT_MAX_H];           // output row -> source row

// Recomputed whenever the core reports a viewport change (a game switching
// between 192- and 224-line modes does this mid-run).
static void build_scale_maps(void)
{
    if (strcmp(s_scale_mode, "1x") == 0) {
        s_out_w = s_src_w;
        s_out_h = s_src_h;
    } else if (strcmp(s_scale_mode, "full") == 0) {
        s_out_w = OUT_MAX_W;
        s_out_h = OUT_MAX_H;
    } else {
        // Aspect-true: the largest whole-percent scale that fits 320x240.
        // Master System lands on exactly 1.25x (320x240); Game Gear on 1.66x
        // (265x239), which nearly fills the panel but samples unevenly --
        // "1x" is there for anyone who prefers clean pixels to screen fill.
        int sx = OUT_MAX_W * 100 / s_src_w;
        int sy = OUT_MAX_H * 100 / s_src_h;
        int s  = (sx < sy) ? sx : sy;
        s_out_w = s_src_w * s / 100;
        s_out_h = s_src_h * s / 100;
        if (s_out_w > OUT_MAX_W) s_out_w = OUT_MAX_W;
        if (s_out_h > OUT_MAX_H) s_out_h = OUT_MAX_H;
    }

    for (int ox = 0; ox < s_out_w; ox++)
        s_sx_map[ox] = (uint8_t)(ox * s_src_w / s_out_w);
    for (int oy = 0; oy < s_out_h; oy++)
        s_sy_map[oy] = (uint8_t)(oy * s_src_h / s_out_h);
}

static void sync_viewport(void)
{
    s_src_x = bitmap.viewport.x;
    s_src_w = bitmap.viewport.w;
    s_src_h = bitmap.viewport.h;
    // Clamped as a window, not as three independent numbers: the row read in
    // blit_frame() runs from x to x+w, so it is x+w that has to stay inside
    // the pitch.
    if (s_src_x < 0 || s_src_x >= SMS_WIDTH) s_src_x = 0;
    if (s_src_w < 1) s_src_w = SMS_WIDTH;
    if (s_src_x + s_src_w > SMS_WIDTH) s_src_w = SMS_WIDTH - s_src_x;
    if (s_src_h < 1 || s_src_h > FB_H) s_src_h = FB_H;
    bitmap.viewport.changed = 0;
    build_scale_maps();
    printf("[sega8] viewport %dx%d at x=%d -> %dx%d (%s)\n",
           s_src_w, s_src_h, s_src_x, s_out_w, s_out_h, s_scale_mode);
}

static void blit_frame(void)
{
    uint16_t *fb = s_blit[s_fb_idx];

    int prev_sy = -1;
    const uint16_t *prev_row = NULL;
    for (int oy = 0; oy < s_out_h; oy++) {
        uint16_t *row = &fb[oy * s_out_w];
        int sy = s_sy_map[oy];
        if (sy == prev_sy) {
            memcpy(row, prev_row, (size_t)s_out_w * sizeof(uint16_t));
        } else {
            const uint8_t *srow = s_indexed + (size_t)sy * SMS_WIDTH + s_src_x;
            for (int ox = 0; ox < s_out_w; ox++)
                row[ox] = s_palette[srow[s_sx_map[ox]]];
            prev_sy = sy;
        }
        prev_row = row;
    }

    host_blit_frame_async(fb, s_out_w, s_out_h);
    s_fb_idx ^= 1;
}

// ---------------------------------------------------------------------------
// Input. Canonical key codes (the launcher's -keymap produces these) mapped
// onto the core's pad/system bits. Game Gear and Master System disagree on
// what the console buttons mean, so the START/PAUSE pair swaps with the
// hardware — the same split retro-go's own frontend makes.
// ---------------------------------------------------------------------------
static int s_pad = 0;
static int s_start = 0, s_pause = 0;

static void poll_input(void)
{
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        int bit = 0;
        switch (key) {
            case 'w': case 0x81: bit = INPUT_UP;      break;
            case 's': case 0x82: bit = INPUT_DOWN;    break;
            case 'a': case 0x83: bit = INPUT_LEFT;    break;
            case 'd': case 0x84: bit = INPUT_RIGHT;   break;
            case 'm': case 0x85: bit = INPUT_BUTTON2; break;  // "2" / trackball click
            case 'n':            bit = INPUT_BUTTON1; break;  // "1"
            case 0x0D: s_start = pressed ? 1 : 0; continue;   // Enter
            case 0x08: s_pause = pressed ? 1 : 0; continue;   // Backspace
            default: continue;
        }
        if (pressed) s_pad |= bit;
        else         s_pad &= ~bit;
    }

    input.pad[0] = (uint8)s_pad;
    input.pad[1] = 0;
    input.system = 0;

    if (IS_GG) {
        if (s_start) input.system |= INPUT_START;
        if (s_pause) input.system |= INPUT_PAUSE;
    } else {
        // Master System / SG-1000: Pause is the console button, and the
        // core's INPUT_START is the reset line's companion.
        if (s_start) input.system |= INPUT_PAUSE;
        if (s_pause) input.system |= INPUT_START;
    }
}

// ---------------------------------------------------------------------------
// Audio. Read the raw PSG streams rather than the core's mixer: sound_init()
// nulls snd.mixer_callback on entry and then gates the snd.output[]
// allocation on that same pointer already equalling sound_mixer_callback,
// with the self-assignment that would satisfy it commented out. The
// condition can never hold, so snd.output[] is always NULL and the mixer
// never runs. snd.stream[] is allocated unconditionally and filled by
// SN76489_Update every line, so that is the real output; retro-go's own
// frontend reads it the same way, gain included.
//
// Gain 2.75 per channel (retro-go's figure), halved for the mono sum, is
// exactly 11/8 -- kept in integers, and clamped because two summed channels
// at that gain can leave int16 range.
// ---------------------------------------------------------------------------
#define AUDIO_RATE 22050
#define ABUF_MAX   1024

// The first second of frames runs slow while the PSRAM instruction cache
// warms, so audio would reach the mixer in late bursts. Swallow until warm
// (same reasoning and duration as the gnuboy module).
#define AUDIO_WARMUP_FRAMES 60

static int16_t s_abuf[ABUF_MAX];
static int     s_audio_warmup = AUDIO_WARMUP_FRAMES;

static void push_audio(void)
{
    if (s_audio_warmup > 0) { s_audio_warmup--; return; }
    if (!snd.enabled || !snd.stream[0] || !snd.stream[1]) return;

    int n = snd.sample_count;
    if (n > ABUF_MAX) n = ABUF_MAX;
    for (int i = 0; i < n; i++) {
        int v = ((int)snd.stream[0][i] + (int)snd.stream[1][i]) * 11 / 8;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s_abuf[i] = (int16_t)v;
    }

    host_audio_push(s_abuf, n, AUDIO_RATE);
}

// ---------------------------------------------------------------------------
// Battery saves. cart.sram is a flat 32KB block the core allocates at load;
// sms.save is set the moment a cart maps it, which is the same "this game
// has a battery" signal the SNES module keys off. The .srm sits next to the
// ROM so the directory always exists.
// ---------------------------------------------------------------------------
#define SRAM_BYTES 0x8000

static char s_srm_path[288];

static void build_srm_path(const char *rom_path)
{
    size_t n = strlen(rom_path);
    if (n > sizeof(s_srm_path) - 8) n = sizeof(s_srm_path) - 8;
    memcpy(s_srm_path, rom_path, n);
    s_srm_path[n] = '\0';

    char *dot = strrchr(s_srm_path, '.');
    char *sep = strrchr(s_srm_path, '/');
    if (dot && (!sep || dot > sep)) *dot = '\0';
    strcat(s_srm_path, ".srm");
}

// sms.save says "this cart has a battery", not "it changed" — it latches on
// at the first SRAM mapping and never clears. Checksumming the block is what
// actually tells us whether a write is worth doing; without it a battery game
// would rewrite the card on every flush tick for as long as it ran.
static uint32 sram_sum(void)
{
    const uint32 *p = (const uint32 *)cart.sram;
    uint32 h = 2166136261u;
    for (int i = 0; i < SRAM_BYTES / 4; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32 s_srm_sum = 0;

// Seeds the checksum from what was on disk, so the first flush tick after a
// load does not rewrite the file with the bytes it just read.
static void srm_load(void)
{
    FILE *f = fopen(s_srm_path, "rb");
    if (f) {
        size_t got = fread(cart.sram, 1, SRAM_BYTES, f);
        fclose(f);
        printf("[sega8] loaded %u bytes of SRAM\n", (unsigned)got);
    } else {
        printf("[sega8] no .srm yet\n");
    }
    s_srm_sum = sram_sum();
}

static void srm_save(void)
{
    if (!sms.save || !cart.sram) return;
    uint32 now = sram_sum();
    if (now == s_srm_sum) return;
    FILE *f = fopen(s_srm_path, "wb");
    if (!f) { host_log("sega8: .srm open failed"); return; }
    size_t put = fwrite(cart.sram, 1, SRAM_BYTES, f);
    fclose(f);
    s_srm_sum = now;
    printf("[sega8] saved %u bytes of SRAM\n", (unsigned)put);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static int ext_is(const char *path, const char *ext)
{
    size_t pl = strlen(path), el = strlen(ext);
    return pl > el && strcasecmp(path + pl - el, ext) == 0;
}

int main(int argc, char **argv)
{
    host_log("sega8: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("sega8: exit()/abort() caught");
        host_clear_screen();
        return s_exit_code;
    }

    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { rom_path = argv[i]; break; }
    }
    if (!rom_path) { host_log("sega8: no ROM path"); return 1; }
    printf("[sega8] rom=%s\n", rom_path);

    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-scale") == 0) s_scale_mode = argv[i + 1];
    }

    system_reset_config();
    option.sndrate  = AUDIO_RATE;
    option.overscan = 0;
    option.extra_gg = 0;

    // Force the console from the file extension rather than trusting header
    // detection. The core's auto path keys off a region nibble at 0x7fff
    // backed by a 93-entry CRC table; a Game Gear dump that satisfies
    // neither runs as a Master System, with the wrong palette depth and no
    // viewport crop. The extension is the one thing we always know.
    if (ext_is(rom_path, ".gg"))                                option.console = 3;
    else if (ext_is(rom_path, ".sg") || ext_is(rom_path, ".sg1000")) option.console = 5;
    else                                                        option.console = 0;

    if (!load_rom_file(rom_path)) {
        host_log("sega8: ROM load failed");
        return 1;
    }
    printf("[sega8] %uKB, crc=%08x, mapper=%d\n",
           (unsigned)(cart.size / 1024), (unsigned)cart.crc, (int)cart.mapper);

    s_indexed = (uint8_t *)calloc(1, SMS_WIDTH * FB_H);
    if (!s_indexed) { host_log("sega8: framebuffer alloc failed"); return 1; }

    bitmap.width  = SMS_WIDTH;
    bitmap.height = FB_H;
    bitmap.pitch  = SMS_WIDTH;
    bitmap.data   = s_indexed;

    system_poweron();

    printf("[sega8] console=%02x display=%s save=%d\n",
           (unsigned)sms.console,
           (sms.display == DISPLAY_NTSC) ? "NTSC" : "PAL",
           (int)sms.save);

    build_srm_path(rom_path);
    srm_load();

    sync_viewport();
    // The palette is only copied when the core marks it dirty, so seed it
    // once here: a game that sets its palette before the first blit would
    // otherwise show one black frame.
    render_copy_palette(s_palette);

    host_clear_screen();
    host_log("sega8: entering main loop");

    const uint32_t frame_us = (sms.display == DISPLAY_NTSC)
                            ? (1000000u / FPS_NTSC)
                            : (1000000u / FPS_PAL);
    uint32_t next_us = host_get_ticks_us();
    int      skip = 0;
    uint32_t next_srm_us = host_get_ticks_us() + 10000000u;

    while (!host_should_exit()) {
        poll_input();

        system_frame(skip);

        if (!skip) {
            if (bitmap.viewport.changed) sync_viewport();
            render_copy_palette(s_palette);
            blit_frame();
        }
        push_audio();

        // Periodic flush so a battery survives a yanked power lead; srm_save
        // checksums first, so a game that is not writing costs one pass over
        // 32KB every 10s and no card traffic at all.
        if ((int32_t)(host_get_ticks_us() - next_srm_us) >= 0) {
            srm_save();
            next_srm_us = host_get_ticks_us() + 10000000u;
        }

        next_us += frame_us;
        int32_t ahead = (int32_t)(next_us - host_get_ticks_us());
        if (ahead > 2000) {
            skip = 0;
            host_sleep_ms((uint32_t)(ahead - 1000) / 1000);
        } else if (ahead < -(int32_t)(frame_us * 4)) {
            next_us = host_get_ticks_us();   // fell far behind: re-anchor
            skip = 0;
        } else {
            // Behind but recoverable: drop the next frame's rendering only.
            skip = (ahead < 0) ? 1 : 0;
            host_sleep_ms(1);
        }
    }

    host_log("sega8: exiting");
    srm_save();
    system_poweroff();
    system_shutdown();
    host_clear_screen();
    return 0;
}
