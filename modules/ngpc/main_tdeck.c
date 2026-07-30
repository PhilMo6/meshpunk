// Neo Geo Pocket Color emulator (RACE) ELF module for T-Deck.
// Bridges the libretro API to meshpunk host_* exports.
// NGPC display is 160x152, centered on 320x240.

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "race-src/libretro/libretro.h"
#include "race-src/graphics.h"
#include <streams/file_stream.h>

// Host imports
extern void     host_blit_frame_async(const uint16_t *rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int *pressed, unsigned char *key);
extern int      host_should_exit(void);
extern void     host_log(const char *msg);
extern void     host_audio_set_pull(void (*cb)(int16_t *out, int count), int sample_rate);

// Exit traps
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;
void exit(int code) { s_exit_code = code; longjmp(s_exit_jmp, 1); }
void abort(void) { host_log("ngpc: abort()"); s_exit_code = 1; longjmp(s_exit_jmp, 1); }

// ---------------------------------------------------------------------------
// libretro-common filestream API, backed by the host's SPI-locked stdio
// exports (fopen/fread/fwrite/fclose). RACE's flash.c uses it for .ngf
// battery saves, with plain READ or WRITE access only. libretro-common's
// own file_stream.c is not built: it delegates to vfs_implementation.c,
// whose POSIX surface (open/lseek/stat) is not in host_exports.
// filestream_vfs_init is only reached if the frontend hands the core a VFS
// interface (environment_cb doesn't), but the symbol must resolve at load.
// ---------------------------------------------------------------------------
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
    (void)hints;
    const char *m = (mode & RETRO_VFS_FILE_ACCESS_WRITE)
        ? ((mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "rb+" : "wb")
        : "rb";
    return (RFILE *)fopen(path, m);
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
    if (!stream || len < 0) return -1;
    return (int64_t)fread(data, 1, (size_t)len, (FILE *)stream);
}

int64_t filestream_write(RFILE *stream, const void *data, int64_t len)
{
    if (!stream || len < 0) return -1;
    return (int64_t)fwrite(data, 1, (size_t)len, (FILE *)stream);
}

int filestream_close(RFILE *stream)
{
    if (!stream) return -1;
    return fclose((FILE *)stream);
}

void filestream_vfs_init(const struct retro_vfs_interface_info *vfs_info)
{
    (void)vfs_info;
}

// Referenced by the RACE core but not a host export.
ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

// Display: NGPC is 160x152, double-buffered
#define NGPC_W 160
#define NGPC_H 152
static uint16_t s_blit[2][NGPC_W * NGPC_H];
static int s_fb_idx = 0;

// Video callback from libretro. Core 0 never renders (the Core-1 worker
// owns all pixel work), so retro_run only ever passes NULL here.
static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    (void)data; (void)width; (void)height; (void)pitch;
}

// Input
static int16_t s_input_state[16] = {0};

static void input_poll_cb(void)
{
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        int id = -1;
        switch (key) {
            case 'w': case 0x81: id = RETRO_DEVICE_ID_JOYPAD_UP; break;
            case 's': case 0x82: id = RETRO_DEVICE_ID_JOYPAD_DOWN; break;
            case 'a': case 0x83: id = RETRO_DEVICE_ID_JOYPAD_LEFT; break;
            case 'd': case 0x84: id = RETRO_DEVICE_ID_JOYPAD_RIGHT; break;
            // RACE's btn_map[] wires retro-pad B to the NGP A button (0x10)
            // and retro-pad A to NGP B (0x20).
            case 'm': case 0x85: id = RETRO_DEVICE_ID_JOYPAD_B; break;    // NGP A (M / trackball click)
            case 'n':            id = RETRO_DEVICE_ID_JOYPAD_A; break;    // NGP B
            case 0x0D:           id = RETRO_DEVICE_ID_JOYPAD_START; break; // Option
            case ' ':            id = RETRO_DEVICE_ID_JOYPAD_SELECT; break;
        }
        if (id >= 0 && id < 16) {
            s_input_state[id] = pressed ? 1 : 0;
        }
    }
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16) return 0;
    return s_input_state[id];
}

// Audio runs on the pull model (see race-src/neopopsound.c): the firmware
// sound task (Core 1) calls race_sound_pull_synth for exactly the samples
// the mixer needs, and Core 0 only queues chip writes. The libretro sample
// callbacks below are registration-slot stubs — retro_run's synthesis and
// push paths are compiled out in libretro.c.
static int s_audio_rate = 44100;

extern int  race_sound_pull_active;
extern void race_sound_pull_synth(int16_t *out, int count);

static void audio_sample_cb(int16_t left, int16_t right)
{
    (void)left; (void)right;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}

// ---------------------------------------------------------------------------
// Core-1 render worker (snapshot + journal). At the -vcap rate, Core 0
// publishes the 16KB VRAM window (0x8000-0xBFFF: registers, palettes, OAM,
// tile maps, patterns) plus the per-line register journal into the staging
// pair and advances the seq. The worker copies the completed buffer into
// its shadow and replays the journal line-by-line via myGraphicsRenderLine —
// the render-path globals are repointed into the shadow once at startup, so
// the body reads no live emulator state and mid-frame raster effects
// reproduce exactly.
// ---------------------------------------------------------------------------
extern unsigned short *drawBuffer;
extern void *host_spawn_task(void (*fn)(void *), void *arg, int core, int prio, int stackkb);
extern int   host_task_join(void *handle, int timeout_ms);

#define VRAM_WIN_SIZE 0x4000
static unsigned char     s_vram_shadow[VRAM_WIN_SIZE];
static unsigned char    *s_live_base = NULL;     // live window start
// Staging pair: Core 0 publishes EVERY frame into s_staging[seq & 1] and
// increments the seq; the worker copies the last completed buffer into its
// shadow and renders it. Latest-wins — if the worker is slow, it simply
// renders the newest frame next. The worker's 16KB staging->shadow copy
// takes well under a frame, so Core 0 never overwrites a buffer mid-copy.
static unsigned char     s_staging[2][VRAM_WIN_SIZE];
static race_line_regs    s_journal_staging[2][152];
static volatile uint32_t s_stage_seq = 0;
static uint32_t          s_last_publish_us = 0;
static uint32_t          s_publish_interval_us = 1000000u / 35;   // -vcap
static volatile int      s_worker_quit = 0;
static void             *s_render_worker = NULL;

// Repoint every render-path global from the live window into the shadow.
// Bounds-guarded: anything outside the window (e.g. bgTable points at a
// static array on B&W-machine carts) is left as-is.
static void repoint_render_globals(void)
{
#define RP(g) do { \
        unsigned char *p_ = (unsigned char *)(g); \
        if (p_ >= s_live_base && p_ < s_live_base + VRAM_WIN_SIZE) \
            g = (void *)(s_vram_shadow + (p_ - s_live_base)); \
    } while (0)
    RP(sprite_table); RP(pattern_table); RP(patterns);
    RP(tile_table_front); RP(tile_table_back);
    RP(palette_table); RP(bw_palette_table); RP(sprite_palette_numbers);
    RP(frame1Pri); RP(wndTopLeftX); RP(wndTopLeftY); RP(wndSizeX); RP(wndSizeY);
    RP(scrollSpriteX); RP(scrollSpriteY); RP(scrollFrontX); RP(scrollFrontY);
    RP(scrollBackX); RP(scrollBackY); RP(bgSelect); RP(oowSelect);
    RP(bgTable); RP(oowTable);
#undef RP
}

// Write line l's journaled registers through the repointed globals — i.e.
// into the shadow — so the body sees the values the game set for that line.
static void apply_line_regs(int l)
{
    const race_line_regs *j = &race_journal_snap[l];
    *wndTopLeftX   = j->wtlx;  *wndTopLeftY   = j->wtly;
    *wndSizeX      = j->wszx;  *wndSizeY      = j->wszy;
    *oowSelect     = j->oow;   *bgSelect      = j->bgsel;
    *frame1Pri     = j->f1pri;
    *scrollFrontX  = j->sfx;   *scrollFrontY  = j->sfy;
    *scrollBackX   = j->sbx;   *scrollBackY   = j->sby;
    *scrollSpriteX = j->ssx;   *scrollSpriteY = j->ssy;
}

static void render_worker(void *arg)
{
    (void)arg;
    uint32_t last = s_stage_seq;
    while (!s_worker_quit) {
        uint32_t s = s_stage_seq;
        if (s == last) { host_sleep_ms(1); continue; }
        last = s;

        int buf = (int)((s - 1) & 1);   // last COMPLETED staging buffer
        memcpy(s_vram_shadow, s_staging[buf], VRAM_WIN_SIZE);
        memcpy(race_journal_snap, s_journal_staging[buf],
               sizeof(race_journal_snap));

        // Render directly into the async blit buffer — the palette table is
        // pre-swapped, so no copy or byte-swap pass follows.
        drawBuffer = (unsigned short *)s_blit[s_fb_idx];
        for (int l = 0; l < NGPC_H; l++) {
            apply_line_regs(l);
            myGraphicsRenderLine(l);
        }
        host_blit_frame_async(s_blit[s_fb_idx], NGPC_W, NGPC_H);
        s_fb_idx ^= 1;
    }
}

// ROM's own directory — flash saves (.ngf) land next to the ROM, so the
// directory always exists. libretro.c appends the trailing slash itself.
static char s_rom_dir[256];

// Environment callback
static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = s_rom_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            // Video bit clear: retro_run skips all rendering every frame
            // (the Core-1 worker owns pixels); audio bit stays set.
            *(int *)data = 0x6;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
            return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
        }
        default:
            return false;
    }
}

// Main
#define FRAME_US 16667  // 60 Hz

int main(int argc, char **argv)
{
    host_log("ngpc: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("ngpc: exit()/abort() caught");
        host_clear_screen();
        return s_exit_code;
    }

    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { rom_path = argv[i]; break; }
    }
    if (!rom_path) { host_log("ngpc: no ROM path"); return 1; }
    printf("[ngpc] rom=%s\n", rom_path);

    // -cpuclock N (percent, 50..100): scale the emulated TLCS-900H cycles
    // per frame. NGP games idle-wait for vblank, so a moderate underclock
    // reduces interpreter work without visibly slowing most titles.
    int cpuclock_pct = 100;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-cpuclock") == 0) {
            int v = atoi(argv[i + 1]);
            if (v >= 50 && v <= 100) cpuclock_pct = v;
        }
    }

    // Save/system directory = the ROM's directory (computed before the core
    // queries it via environment_cb during retro_set_environment/retro_init).
    strncpy(s_rom_dir, rom_path, sizeof(s_rom_dir) - 1);
    s_rom_dir[sizeof(s_rom_dir) - 1] = '\0';
    char *last_slash = strrchr(s_rom_dir, '/');
    if (last_slash && last_slash != s_rom_dir) *last_slash = '\0';
    else { s_rom_dir[0] = '/'; s_rom_dir[1] = '\0'; }

    // mainrom is runtime-allocated (see race-memory.c): 4MB always — the
    // cart address decoder indexes the full 32Mbit window regardless of ROM
    // file size. Zeroed to match the old .bss array's initial state; the ROM
    // loader memsets to 0xFF and overwrites before any cart access.
    {
        extern unsigned char *mainrom;
        mainrom = (unsigned char *)malloc(4 * 1024 * 1024);
        if (!mainrom) { host_log("ngpc: mainrom alloc failed"); return 1; }
        memset(mainrom, 0, 4 * 1024 * 1024);
    }

    // Set up libretro callbacks
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_refresh_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);

    retro_init();

    // ROM: path-only load. RACE declares need_fullpath and streams the file
    // into the runtime-allocated mainrom via the filestream shim — no second
    // whole-ROM heap copy.
    struct retro_game_info game = {0};
    game.path = rom_path;
    game.data = NULL;
    game.size = 0;

    if (!retro_load_game(&game)) {
        host_log("ngpc: retro_load_game failed");
        retro_deinit();
        return 1;
    }

    // Apply the emulated-CPU clock scale (race_cycles_per_frame is
    // initialized to full speed in libretro.c).
    {
        extern int race_cycles_per_frame;
        race_cycles_per_frame = (race_cycles_per_frame * cpuclock_pct) / 100;
        printf("[ngpc] cpuclock=%d%% (%d cycles/frame)\n",
               cpuclock_pct, race_cycles_per_frame);
    }

    // "-soundoff 1" silences synthesis (the launcher's Sound OFF setting);
    // chip state still tracks so sound resumes correctly elsewhere.
    {
        extern int race_sound_measure;
        for (int i = 1; i < argc - 1; i++)
            if (strcmp(argv[i], "-soundoff") == 0)
                race_sound_measure = atoi(argv[i + 1]) ? 1 : 0;
        if (race_sound_measure)
            printf("[ngpc] sound off\n");
    }

    // Push audio at the rate the core actually runs at (default 44100).
    struct retro_system_av_info av;
    memset(&av, 0, sizeof(av));
    retro_get_system_av_info(&av);
    if (av.timing.sample_rate > 0)
        s_audio_rate = (int)av.timing.sample_rate;
    printf("[ngpc] av_info: %d.%02d fps, audio rate=%d\n",
           (int)av.timing.fps, (int)(av.timing.fps * 100) % 100, s_audio_rate);

    // Core-1 pull audio: the firmware sound task calls race_sound_pull_synth
    // for exactly the samples the mixer needs; Core 0 only queues chip
    // writes from this point on.
    race_sound_pull_active = 1;
    host_audio_set_pull(race_sound_pull_synth, s_audio_rate);

    // "-vcap N" caps the publish (video) rate — rendering bandwidth trades
    // directly against emulation speed on this shared-cache silicon.
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-vcap") == 0) {
            int v = atoi(argv[i + 1]);
            if (v >= 10 && v <= 60)
                s_publish_interval_us = 1000000u / (uint32_t)v;
        }
    }

    // Core-1 render worker. Order matters, all on Core 0 before the worker
    // exists: capture the live register pointers for the journal recorder,
    // derive the live window base, repoint the render globals into the
    // shadow, and pre-swap the RGB565 table once so every rendered pixel is
    // born in the TFT byte order (no per-frame swap pass).
    race_journal_init();
    s_live_base = sprite_table - 0x800;   // sprite_table = window + 0x800
    repoint_render_globals();
    {
        extern int totalpalette[];
        for (int i = 0; i < 32 * 32 * 32; i++) {
            unsigned v = (unsigned)totalpalette[i] & 0xFFFFu;
            totalpalette[i] = (int)(((v >> 8) | (v << 8)) & 0xFFFFu);
        }
    }
    s_render_worker = host_spawn_task(render_worker, NULL,
                                      1 /*core*/, 1 /*prio*/, 6 /*stackkb*/);
    if (!s_render_worker) {
        host_log("ngpc: render worker spawn failed");
        host_audio_set_pull(NULL, 0);
        race_sound_pull_active = 0;
        retro_unload_game();
        retro_deinit();
        return 1;
    }

    host_clear_screen();
    host_log("ngpc: entering main loop");

    // Main loop: emulate at 60 Hz; publish frames to the worker at the
    // -vcap rate.
    uint32_t next_us = host_get_ticks_us();
    while (!host_should_exit()) {
        retro_run();

        uint32_t now = host_get_ticks_us();
        if ((uint32_t)(now - s_last_publish_us) >= s_publish_interval_us) {
            // Publish this frame: VRAM window + line journal into the
            // staging buffer Core 0 owns, then advance the seq.
            uint32_t b = s_stage_seq & 1;
            memcpy(s_staging[b], s_live_base, VRAM_WIN_SIZE);
            memcpy(s_journal_staging[b], race_journal_live,
                   sizeof(s_journal_staging[0]));
            s_stage_seq++;
            // Accumulate rather than re-anchor: the publish clock keeps its
            // fractional credit, so the publish slot walks across frame
            // parity and both phases of flickered sprites keep rendering.
            s_last_publish_us += s_publish_interval_us;
            if ((uint32_t)(now - s_last_publish_us) > s_publish_interval_us)
                s_last_publish_us = now;   // fell behind: re-anchor
        }

        next_us += FRAME_US;
        int32_t ahead = (int32_t)(next_us - host_get_ticks_us());
        if (ahead > 2000)
            host_sleep_ms((uint32_t)(ahead - 1000) / 1000);
        else if (ahead < -(int32_t)(FRAME_US * 4))
            next_us = host_get_ticks_us();
        else
            host_sleep_ms(1);
    }

    host_log("ngpc: exiting");
    // Stop the worker and wait for it before any teardown — it reads the
    // staging buffers and calls the blit path.
    s_worker_quit = 1;
    host_task_join(s_render_worker, 500);
    s_render_worker = NULL;
    // Blocks until the mixer is outside the callback; after this the sound
    // task never enters module code again.
    host_audio_set_pull(NULL, 0);
    race_sound_pull_active = 0;
    retro_unload_game();
    retro_deinit();   // flashShutdown() inside writes the .ngf battery save
    host_clear_screen();
    return 0;
}
