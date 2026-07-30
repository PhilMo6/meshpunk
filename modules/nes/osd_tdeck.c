// T-Deck OSD (Operating System Dependent) implementation for Nofrendo.
// Maps all platform functions to meshpunk firmware host_* exports.
// Follows the gameboy module pattern: double-buffered async blit, per-frame
// input polling, microsecond frame pacing.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <noftypes.h>
#include <event.h>
#include <gui.h>
#include <log.h>
#include <nes/nes.h>
#include <nes/nes_pal.h>
#include <nes/nesinput.h>
#include <nofconfig.h>
#include <osd.h>

// ---------------------------------------------------------------------------
// Host imports
// ---------------------------------------------------------------------------
extern void     host_blit_frame_async(const uint16_t *rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int *pressed, unsigned char *key);
extern int      host_should_exit(void);
extern void     host_log(const char *msg);
extern void     host_audio_push(const int16_t *samples, int count, int sample_rate);

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
void *mem_alloc(int size, bool prefer_fast_memory)
{
    (void)prefer_fast_memory;
    return malloc(size); // goes to PSRAM via psram_malloc
}

// ---------------------------------------------------------------------------
// Display — NES is 256×240; visible 256×224. Centered on 320×240.
// Double-buffered for async Core-1 SPI push.
// ---------------------------------------------------------------------------
#define NES_W NES_SCREEN_WIDTH   // 256
#define NES_H NES_SCREEN_HEIGHT  // 240
#define OUT_W 256
#define OUT_H 240  // full NES height = 320x240 T-Deck height

static uint16_t s_blit[2][OUT_W * OUT_H];
static int s_fb_idx = 0;

// NES palette → RGB565 (byte-swapped for TFT SPI)
static uint16_t s_pal565[256];

static int vid_init_cb(int width, int height) { (void)width; (void)height; return 0; }
static void vid_shutdown_cb(void) {}
static int vid_set_mode_cb(int width, int height) { (void)width; (void)height; return 0; }

static void vid_set_palette_cb(rgb_t *pal)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = ((pal[i].r & 0xF8) << 8) | ((pal[i].g & 0xFC) << 3) | (pal[i].b >> 3);
        s_pal565[i] = (c >> 8) | (c << 8); // byte-swap for SPI TFT
    }
}

static void vid_clear_cb(uint8 color)
{
    host_clear_screen();
}

static char s_fb_dummy[1];
static bitmap_t *s_bmp = NULL;

static bitmap_t *vid_lock_write_cb(void)
{
    s_bmp = bmp_createhw((uint8 *)s_fb_dummy, NES_W, NES_H, NES_W * 2);
    return s_bmp;
}

static void vid_free_write_cb(int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties; (void)dirty_rects;
    bmp_destroy(&s_bmp);
}

static void vid_custom_blit_cb(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties; (void)dirty_rects;

    uint16_t *dst = s_blit[s_fb_idx];
    for (int y = 0; y < OUT_H; y++) {
        uint8 *src_line = bmp->line[y];
        uint16_t *dst_line = &dst[y * OUT_W];
        for (int x = 0; x < OUT_W; x++) {
            dst_line[x] = s_pal565[src_line[x]];
        }
    }

    host_blit_frame_async(dst, OUT_W, OUT_H);
    s_fb_idx ^= 1;
}

static viddriver_t tdeck_vid_driver = {
    "T-Deck",
    vid_init_cb,
    vid_shutdown_cb,
    vid_set_mode_cb,
    vid_set_palette_cb,
    vid_clear_cb,
    vid_lock_write_cb,
    vid_free_write_cb,
    vid_custom_blit_cb,
    false
};

void osd_getvideoinfo(vidinfo_t *info)
{
    info->default_width = NES_W;
    info->default_height = NES_H;
    info->driver = &tdeck_vid_driver;
}

// ---------------------------------------------------------------------------
// Sound — NES APU outputs mono S16 at configurable rate. The play function is
// nes.apu->process, i.e. apu_process(void *buffer, int num_samples).
// ---------------------------------------------------------------------------
#define NES_SAMPLE_RATE 22050

static void (*s_play_func)(void *buffer, int size) = NULL;

void osd_setsound(void (*playfunc)(void *buffer, int size))
{
    s_play_func = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
    info->sample_rate = NES_SAMPLE_RATE;
    info->bps = 16;
}

// Pull one emulated frame of APU output and hand it to the firmware mixer.
// 22050/60 = 367.5 — alternate 367/368 so the long-run rate stays exact.
static int16_t s_abuf[NES_SAMPLE_RATE / 30];
static int s_audio_parity = 0;

void osd_do_audio(void)
{
    if (!s_play_func) return;
    int samples = (NES_SAMPLE_RATE / 60) + (s_audio_parity ^= 1);
    s_play_func(s_abuf, samples);
    host_audio_push(s_abuf, samples, NES_SAMPLE_RATE);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static uint32_t s_pad = 0;

static int map_key_to_nes(unsigned char key)
{
    switch (key) {
        // D-pad
        case 'w': case 0x81: return 0; // UP
        case 's': case 0x82: return 1; // DOWN
        case 'a': case 0x83: return 2; // LEFT
        case 'd': case 0x84: return 3; // RIGHT
        // Buttons
        case ' ':            return 4; // SELECT
        case 0x0D:           return 5; // START
        case 'm': case 0x85: return 6; // A (M / trackball click)
        case 'n':            return 7; // B
        default:             return -1;
    }
}

void osd_getinput(void)
{
    // Map event system indices
    static const int ev_map[8] = {
        event_joypad1_up, event_joypad1_down,
        event_joypad1_left, event_joypad1_right,
        event_joypad1_select, event_joypad1_start,
        event_joypad1_a, event_joypad1_b
    };

    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        int bit = map_key_to_nes(key);
        if (bit < 0) continue;

        event_t handler = event_get(ev_map[bit]);
        if (handler)
            handler(pressed ? INP_STATE_MAKE : INP_STATE_BREAK);
    }
}

void osd_getmouse(int *x, int *y, int *button)
{
    *x = *y = *button = 0;
}

// ---------------------------------------------------------------------------
// Timer — nofrendo paces emulation on nofrendo_ticks, incremented by the
// callback it installs here at `frequency` Hz. There is no timer ISR in the
// module: osd_pump_ticks(), called each pass of the nes_emulate loop,
// converts wall time from host_get_ticks_us() into callback invocations.
// Catch-up is capped at 4 periods; a longer stall (SD hiccup) drops the
// backlog instead of replaying it.
// ---------------------------------------------------------------------------
static void (*s_timer_func)(void) = NULL;
static int s_timer_freq = 60;
static uint32_t s_tick_last_us = 0;

int osd_installtimer(int frequency, void *func, int funcsize,
                     void *counter, int countersize)
{
    (void)funcsize; (void)counter; (void)countersize;
    s_timer_func = (void (*)(void))func;
    s_timer_freq = (frequency > 0) ? frequency : 60;
    s_tick_last_us = host_get_ticks_us();
    return 0;
}

void osd_pump_ticks(void)
{
    if (!s_timer_func) return;
    uint32_t period = 1000000u / (uint32_t)s_timer_freq;
    uint32_t elapsed = host_get_ticks_us() - s_tick_last_us;
    int fired = 0;
    while (elapsed >= period && fired < 4) {
        s_timer_func();
        s_tick_last_us += period;
        elapsed -= period;
        fired++;
    }
    if (fired == 4 && elapsed >= period)
        s_tick_last_us = host_get_ticks_us();
    // No tick pending: sleep instead of busy-spinning the emulate loop.
    if (fired == 0)
        host_sleep_ms(1);
}

// Polled from the nes_emulate loop and main_loop — firmware exit chord.
int osd_check_exit(void)
{
    return host_should_exit();
}

// ---------------------------------------------------------------------------
// Init / shutdown / main
// ---------------------------------------------------------------------------
static int logprint(const char *string)
{
    return printf("%s", string);
}

int osd_init(void)
{
    nofrendo_log_chain_logfunc(logprint);
    host_clear_screen();
    host_log("nes: osd_init complete");
    return 0;
}

void osd_shutdown(void)
{
    host_clear_screen();
    host_log("nes: osd_shutdown");
}

static char s_configfilename[] = "na";

int osd_main(int argc, char *argv[])
{
    config.filename = s_configfilename;
    return main_loop(argv[0], system_autodetect);
}

// ---------------------------------------------------------------------------
// Filename helpers
// ---------------------------------------------------------------------------
void osd_fullname(char *fullname, const char *shortname)
{
    strncpy(fullname, shortname, PATH_MAX);
}

char *osd_newextension(char *string, char *ext)
{
    size_t l = strlen(string);
    if (l >= 4) {
        string[l - 3] = ext[1];
        string[l - 2] = ext[2];
        string[l - 1] = ext[3];
    }
    return string;
}

int osd_makesnapname(char *filename, int len)
{
    return -1;
}

// ---------------------------------------------------------------------------
// Stubs for desktop-only nofrendo objects that are not built (intro.c is the
// embedded fallback ROM used when no filename opens; pcx.c writes
// screenshots), and libc functions referenced by the core that are not in
// the firmware's host_exports.
// ---------------------------------------------------------------------------
#include <stdarg.h>
#include <intro.h>
#include <pcx.h>

// nes_rom.c falls back to these only when the ROM file failed to open;
// failing here surfaces the normal rom_load error path.
void intro_get_header(rominfo_t *rominfo) { (void)rominfo; }
int  intro_get_rom(rominfo_t *rominfo) { (void)rominfo; return -1; }

int pcx_write(char *filename, bitmap_t *bmp, rgb_t *pal)
{
    (void)filename; (void)bmp; (void)pal;
    return -1;
}

int vsprintf(char *str, const char *fmt, va_list ap)
{
    return vsnprintf(str, (size_t)0x7FFFFFFF, fmt, ap);
}

// Via the SPI-locked fread export — never through FILE internals.
int fgetc(FILE *f)
{
    unsigned char c;
    return (fread(&c, 1, 1, f) == 1) ? (int)c : EOF;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}
