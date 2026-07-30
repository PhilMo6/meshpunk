// SNES emulator (snes9x) ELF module for T-Deck.
// Uses ducalex's retro-go pure-C snes9x port.
// Same pattern as NES/gameboy: double-buffered async blit, frame pacing.

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "snes9x-src/src/snes9x.h"
#include "snes9x-src/src/fxemu.h"
#include "snes9x-src/src/soundux.h"
#include "snes9x-src/src/memmap.h"
#include "snes9x-src/src/apu.h"
#include "snes9x-src/src/display.h"
#include "snes9x-src/src/gfx.h"
#include "snes9x-src/src/cpuexec.h"

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
extern void     host_audio_set_pull(void (*cb)(int16_t *out, int count), int sample_rate);
extern void    *host_spawn_task(void (*fn)(void *), void *arg, int core, int prio, int stackkb);
extern int      host_task_join(void *task, int timeout_ms);

// ---------------------------------------------------------------------------
// Exit traps
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

void exit(int code) { s_exit_code = code; longjmp(s_exit_jmp, 1); }
void abort(void) { host_log("snes: abort()"); s_exit_code = 1; longjmp(s_exit_jmp, 1); }

// ---------------------------------------------------------------------------
// Display: SNES is 256x224 (or 256x240/512 in hires). Centered on 320x240.
// ---------------------------------------------------------------------------
#define OUT_W 256
#define OUT_H 224

// snes9x renders unswapped RGB565 straight into the back buffer;
// blit_frame byte-swaps the visible rows in place and hands the buffer to
// the async blit while the next frame renders into the other one. Sized
// for extended-height carts (239 rendered rows; rows past OUT_H stay
// unsent). +16 px tail slack: the half-width mosaic/color-math drawers
// advance at half step but plot full-width pixel runs, so the last
// rendered line can spill a few pixels past the row end.
static uint16_t s_blit[2][OUT_W * SNES_HEIGHT_EXTENDED + 16];
static int s_fb_idx = 0;

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = (uint8_t *)s_blit[0];
    // +32B tail slack on each: same constraint as s_screen_buf.
    GFX.SubScreen = malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED + 32);
    GFX.ZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED + 32);
    GFX.SubZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED + 32);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
    if (GFX.SubScreen) { free(GFX.SubScreen); GFX.SubScreen = NULL; }
    if (GFX.ZBuffer) { free(GFX.ZBuffer); GFX.ZBuffer = NULL; }
    if (GFX.SubZBuffer) { free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
}

static void blit_frame(void)
{
    uint16_t *fb = s_blit[s_fb_idx];
    int h = IPPU.RenderedScreenHeight > OUT_H ? OUT_H : IPPU.RenderedScreenHeight;
    int w = IPPU.RenderedScreenWidth > OUT_W ? OUT_W : IPPU.RenderedScreenWidth;

    // Byte-swap RGB565 in place for TFT SPI, skipping rows the fast
    // renderer already wrote swapped (snes_row_swapped). Rows are
    // contiguous: GFX.Pitch/2 == OUT_W.
    for (int y = 0; y < h; y++) {
        uint16_t *row;
        if (snes_row_swapped[y])
            continue;
        row = fb + y * OUT_W;
        for (int x = 0; x < w; x++) {
            uint16_t c = row[x];
            row[x] = (uint16_t)((c >> 8) | (c << 8));
        }
    }
    memset((void *)snes_row_swapped, 0, sizeof(snes_row_swapped));

    host_blit_frame_async(fb, w, h);

    // Next frame renders into the other buffer while the DMA reads this
    // one; S9xStartScreenRefresh recomputes GFX.Delta from GFX.Screen
    // before any drawing.
    s_fb_idx ^= 1;
    GFX.Screen = (uint8_t *)s_blit[s_fb_idx];
}

// ---------------------------------------------------------------------------
// Render worker: Core 1, prio 1 (below the firmware sound and blit tasks).
// Renders the latest published journal frame through the fast path against
// its private journal copies (live VRAM/OAM/PPU reads are display-only
// races) and blits. Core 0 renders non-fast frames (modes 2-7) itself
// through the stock path, gated on s_worker_busy so the two never touch
// the blit buffers concurrently.
// ---------------------------------------------------------------------------
static volatile int s_worker_quit = 0;
static volatile int s_worker_busy = 0;
static void *s_worker = NULL;

// -render: 0 renders on the Core-1 worker, 1 renders on Core 0.
//
// The worker binds ONE frame-boundary snapshot of the palette and the BG
// tilemap/character base registers, so a game that rewrites those during
// active display (HDMA colour gradients, mid-frame base switches) draws the
// whole frame with the vblank values. The Core-0 path re-reads them per
// span, which is what the stock renderer's span splitting relies on.
// Scroll and fixed colour are journaled per line and are correct in both.
static int s_use_worker = 1;

// Leaky bucket for the frame-to-frame layout prediction (see the main loop).
// A miss costs FR_MIS_PENALTY, a correct prediction repays 1, and the worker
// is dropped above FR_MIS_TRIP and taken back below FR_MIS_CLEAR.
#define FR_MIS_PENALTY  8
#define FR_MIS_MAX     64
#define FR_MIS_TRIP    32
#define FR_MIS_CLEAR    4

static void render_worker(void *arg)
{
    (void)arg;
    uint32_t last_seq = 0;
    // Hungry means "idle, and the published buffers are free to overwrite".
    // It must NOT be set while a render is running: Core 0 publishes into
    // those same buffers, which would rewrite VRAM and sprite tables under
    // the render in progress.
    snes_worker_hungry = 1;
    while (!s_worker_quit) {
        int h;
        if (snes_frame_seq == last_seq) {
            host_sleep_ms(1);
            continue;
        }
        last_seq = snes_frame_seq;
        s_worker_busy = 1;

        h = (int)snes_worker_bind();
        if (h > SNES_HEIGHT_EXTENDED)
            h = SNES_HEIGHT_EXTENDED;
        IPPU.RenderedScreenWidth = 256;
        IPPU.RenderedScreenHeight = h;

        // Strip mode: each band goes straight from the band buffer to the
        // panel, so there is no frame buffer to fill or blit afterwards.
        fr_strip_out = 1;
        snes_fast_span(0, (uint32_t)(h - 1), snes_worker_ld(),
                       snes_worker_fx());
        s_worker_busy = 0;
        // Render finished — the published buffers may be refilled now.
        snes_worker_hungry = 1;
    }
    snes_worker_hungry = 0;
    s_worker_busy = 0;
}

// Core 0 must not run the stock renderer or blit while the worker holds
// the buffers.
static void wait_worker_idle(void)
{
    while (s_worker_busy)
        host_sleep_ms(1);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static uint32_t s_joypad = 0;

static void poll_input(void)
{
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        uint32_t mask = 0;
        switch (key) {
            case 'w': case 0x81: mask = SNES_UP_MASK; break;
            case 's': case 0x82: mask = SNES_DOWN_MASK; break;
            case 'a': case 0x83: mask = SNES_LEFT_MASK; break;
            case 'd': case 0x84: mask = SNES_RIGHT_MASK; break;
            case 'm': case 0x85: mask = SNES_A_MASK; break;     // M / trackball click
            case 'n':            mask = SNES_B_MASK; break;     // N
            case 'k':            mask = SNES_X_MASK; break;     // K (above M)
            case 'j':            mask = SNES_Y_MASK; break;     // J (above N)
            case 'q':            mask = SNES_TL_MASK; break;    // Q = L shoulder
            case 'p':            mask = SNES_TR_MASK; break;    // P = R shoulder
            case 0x0D:           mask = SNES_START_MASK; break;  // Enter
            case ' ':            mask = SNES_SELECT_MASK; break; // Space
        }
        if (mask) {
            if (pressed) s_joypad |= mask;
            else         s_joypad &= ~mask;
        }
    }
}

uint32_t S9xReadJoypad(int32_t port)
{
    return (port == 0) ? s_joypad : 0;
}

bool S9xReadMousePosition(int32_t w, int32_t *x, int32_t *y, uint32_t *b)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *b)
{
    return false;
}

bool JustifierOffscreen(void) { return true; }
void JustifierButtons(uint32_t *j) { (void)j; }

// ---------------------------------------------------------------------------
// Audio: pull model. The firmware sound task calls snes_sound_pull for mono
// samples at AUDIO_RATE; it drains the SPC700 DSP-write ring (apu.c), then
// synthesizes with S9xMixSamples (interleaved stereo) and downmixes. Runs
// outside the emu task, so pitch stays correct at any emulation speed.
// ---------------------------------------------------------------------------
#define AUDIO_RATE 22050

static int16_t s_mix_tmp[512];               // 256 stereo frames per chunk

// Sound-task context: must not block and must not allocate.
static void snes_sound_pull(int16_t *out, int count)
{
    S9xDrainAPUDSPQueue();
    while (count > 0) {
        int frames = count > 256 ? 256 : count;
        S9xMixSamples(s_mix_tmp, frames * 2);
        for (int i = 0; i < frames; i++) {
            int32_t l = s_mix_tmp[i * 2];
            int32_t r = s_mix_tmp[i * 2 + 1];
            out[i] = (int16_t)((l + r) >> 1);
        }
        out += frames;
        count -= frames;
    }
}

// ---------------------------------------------------------------------------
// SRAM battery saves: <rom>.srm next to the ROM. Loaded after LoadROM,
// written on exit and 2 seconds after the last SRAM-modifying frame
// (snes_sram_dirty, gfx.c).
// ---------------------------------------------------------------------------
extern volatile uint32_t snes_sram_dirty;

static char s_srm_path[512];
static uint32_t s_sram_size = 0;

static void build_srm_path(const char *rom)
{
    static const char ext[] = ".srm";
    int len = 0, dot = -1;
    for (; rom[len] && len < 500; len++) {
        s_srm_path[len] = rom[len];
        if (rom[len] == '.') dot = len;
        else if (rom[len] == '/') dot = -1;
    }
    if (dot < 0) dot = len;
    for (int i = 0; ext[i]; i++) s_srm_path[dot + i] = ext[i];
    s_srm_path[dot + 4] = 0;
}

static void srm_load(void)
{
    FILE *f;
    int n;

    if (!s_sram_size) { printf("[snes] cart has no SRAM\n"); return; }
    f = fopen(s_srm_path, "rb");
    if (!f) {
        printf("[snes] SRAM %uKB, no .srm yet\n", (unsigned)(s_sram_size >> 10));
        return;
    }
    n = (int)fread(Memory.SRAM, 1, s_sram_size, f);
    fclose(f);
    printf("[snes] SRAM %uKB, loaded %d bytes from .srm\n",
           (unsigned)(s_sram_size >> 10), n);
}

static void srm_save(void)
{
    FILE *f;
    if (!s_sram_size) return;
    f = fopen(s_srm_path, "wb");
    if (!f) { printf("[snes] .srm write failed: %s\n", s_srm_path); return; }
    fwrite(Memory.SRAM, 1, s_sram_size, f);
    fclose(f);
    printf("[snes] .srm saved (%u bytes)\n", (unsigned)s_sram_size);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    host_log("snes: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("snes: exit()/abort() caught");
        s_worker_quit = 1;
        if (s_worker) host_task_join(s_worker, 500);
        host_audio_set_pull(NULL, 0);
        if (s_sram_size) srm_save();
        host_clear_screen();
        return s_exit_code;
    }

    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-stackkb") == 0 && i + 1 < argc) {
            i++; // host-consumed knob; skip its value
        } else if (strcmp(argv[i], "-render") == 0 && i + 1 < argc) {
            // "0" selects the worker; anything else selects Core 0, so a
            // malformed value lands on the accurate path.
            s_use_worker = (argv[i + 1][0] == '0');
            i++;
        } else if (argv[i][0] != '-' && !rom_path) {
            rom_path = argv[i];
        }
    }
    if (!rom_path) { host_log("snes: no ROM path"); return 1; }
    printf("[snes] rom=%s\n", rom_path);

    // Initialize snes9x
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimeNTSC = 16667;
    Settings.FrameTimePAL = 20000;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_RATE;
    Settings.SoundInputRate = AUDIO_RATE;
    Settings.DisableSoundEcho = true;   // save CPU
    Settings.InterpolatedSound = false; // save CPU

    if (!S9xInitDisplay()) { host_log("snes: display init failed"); return 1; }
    if (!S9xInitMemory()) { host_log("snes: memory init failed"); return 1; }
    if (!S9xInitAPU()) { host_log("snes: APU init failed"); return 1; }
    if (!S9xInitSound(0, 0)) { host_log("snes: sound init failed"); return 1; }
    if (!S9xInitGFX()) { host_log("snes: GFX init failed"); return 1; }
    memset(snes_vram_dirty, 1, sizeof(snes_vram_dirty)); // first publish copies all

    if (!LoadROM(rom_path)) { host_log("snes: ROM load failed"); return 1; }

    s_sram_size = Memory.SRAMSize ? (uint32_t)(Memory.SRAMMask + 1) : 0;
    build_srm_path(rom_path);
    srm_load();

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
    snes_sndq_active = 1;
    host_audio_set_pull(snes_sound_pull, AUDIO_RATE);

    // Stack ladder: the band context and strip buffer (~11KB) live on the
    // worker's stack, which is internal SRAM — scarce and fragmented.
    // Core-0 rendering needs no worker, so its stack stays unallocated.
    if (s_use_worker) {
        static const int rungs[] = { 16, 14, 12 };
        for (unsigned i = 0; i < sizeof(rungs) / sizeof(rungs[0]); i++) {
            s_worker = host_spawn_task(render_worker, NULL, 1, 1, rungs[i]);
            if (s_worker) {
                printf("[snes] render worker: %dKB stack\n", rungs[i]);
                break;
            }
        }
        if (!s_worker) {
            host_log("snes: render worker spawn failed");
            host_audio_set_pull(NULL, 0);
            snes_sndq_active = 0;
            return 1;
        }
    } else {
        printf("[snes] rendering on Core 0\n");
    }

    printf("[snes] ROM buffer %uKB, %s timing\n",
           (unsigned)(Memory.ROM_AllocSize >> 10),
           Settings.PAL ? "PAL 50Hz" : "NTSC 60Hz");
    host_clear_screen();
    host_log("snes: entering main loop");

    // PAL carts run the 312-line 50Hz grid; pace to the cart's clock.
    const uint32_t frame_us = Settings.PAL ? 20000 : 16667;

    // Debounced .srm autosave: write 2s after the last SRAM-modifying frame.
    uint32_t sram_seen = snes_sram_dirty;
    uint32_t sram_change_ms = 0;
    int sram_pending = 0;

    // Main loop with frameskip
    uint32_t next_us = host_get_ticks_us();
    int skipFrames = 0;
    int maxSkip = 3;
    uint8_t prev_modes = 0;   // BG modes the last frame used
    uint8_t prev_mid = 0;     // PPU state the last frame rewrote mid-frame
    int fast_frame = 0;
    int fr_ok = 0;

    // Predicting a frame from the one before it is only sound while a game
    // holds its layout, and a lost bet draws the frame from state the frame
    // then changed. Score every prediction against what the frame turned out
    // to be and stop using the worker while the score says the premise does
    // not hold. A leaky bucket, so an isolated miss at a scene change drains
    // away while a game that cycles its layout every few frames trips and
    // stays tripped. Scoring continues while tripped — the masks are built
    // whichever path drew the frame — so a game that settles recovers.
    int fr_mis_score = 0;
    int fr_worker_trusted = 1;

    while (!host_should_exit()) {
        poll_input();

        // The worker renders a whole frame with one renderer, chosen here;
        // S9xUpdateScreen chooses per span on the Core-0 path. A frame that
        // mixes BG modes — a mode-7 banner over a mode-1 background, say —
        // therefore has to render on Core 0, or the out-of-scope lines get
        // drawn with the mode-1 layout. The mode used is only known once a
        // frame has run, so the previous frame predicts this one; games hold
        // their layout across frames, and a change costs one frame.
        // Layout the worker binds once per frame and therefore cannot
        // follow if the game rewrites it mid-frame: BG mode/tile size/BG3
        // priority, tilemap and character bases, windows, layer enable,
        // colour math, and sprite addressing. CGRAM is excluded because it
        // is journaled per line, and VRAM because games rewrite it every
        // frame (Earthworm Jim does, and renders correctly) — those writes
        // land in forced blank or ahead of the raster.
        {
            uint8_t m = prev_modes;
            int one_mode = m && (m & (uint8_t)(m - 1)) == 0;
            fast_frame = one_mode && (m & 0x03) != 0 &&
                         (prev_mid & 0xbb) == 0;
        }

        // snes_fast_ok is pure; evaluated once so the score below and the
        // branch cannot disagree about what was predicted.
        fr_ok = snes_fast_ok();

        if (s_use_worker && fr_worker_trusted && fast_frame && fr_ok) {
            // Worker renders these frames; Core 0 never draws them. The
            // publish happens inside S9xMainLoop at V-blank start.
            snes_worker_enable = 1;
            IPPU.RenderThisFrame = false;
            S9xMainLoop();
            skipFrames = 0;
        } else {
            // Stock path on Core 0 (modes 2-7, mosaic, pseudo-hires).
            bool drawFrame = (skipFrames == 0);
            snes_worker_enable = 0;
            wait_worker_idle();
            IPPU.RenderThisFrame = drawFrame;
            S9xMainLoop();
            if (drawFrame)
                blit_frame();
        }

        // Score the prediction against what the frame turned out to be. This
        // reads the prediction, not the path taken, so a tripped game keeps
        // being measured and can earn the worker back.
        if (fast_frame && fr_ok) {
            int missed = (snes_mid_mask & 0xbb) != 0 ||
                         snes_mode_mask != prev_modes;
            if (missed) {
                fr_mis_score += FR_MIS_PENALTY;
                if (fr_mis_score > FR_MIS_MAX)
                    fr_mis_score = FR_MIS_MAX;
                if (fr_mis_score >= FR_MIS_TRIP)
                    fr_worker_trusted = 0;
            } else if (fr_mis_score > 0) {
                fr_mis_score--;
                if (fr_mis_score <= FR_MIS_CLEAR)
                    fr_worker_trusted = 1;
            }
        }

        prev_modes = snes_mode_mask;
        prev_mid = snes_mid_mask;

        uint32_t now_ms = host_get_ticks_ms();

        if (snes_sram_dirty != sram_seen) {
            sram_seen = snes_sram_dirty;
            sram_change_ms = now_ms;
            sram_pending = 1;
        } else if (sram_pending && now_ms - sram_change_ms >= 2000) {
            srm_save();
            sram_pending = 0;
        }

        next_us += frame_us;

        // Frame pacing with auto-frameskip
        int32_t ahead = (int32_t)(next_us - host_get_ticks_us());
        if (ahead > 2000) {
            host_sleep_ms((uint32_t)(ahead - 1000) / 1000);
            skipFrames = 0;
        } else if (ahead < -(int32_t)(frame_us * 4)) {
            // Hopelessly behind — resync. skipFrames starts at 1, not 0:
            // resetting to 0 draws again immediately, and when one drawn
            // frame alone exceeds the resync threshold that oscillates
            // between skip bursts and back-to-back draws (visible speed
            // jitter).
            next_us = host_get_ticks_us();
            skipFrames = 1;
        } else if (ahead < 0 && skipFrames < maxSkip) {
            skipFrames++;
        } else {
            skipFrames = 0;
            host_sleep_ms(1); // always yield
        }
    }

    host_log("snes: exiting");
    s_worker_quit = 1;
    if (s_worker) host_task_join(s_worker, 500);
    // Blocks until the mixer is outside the callback; module memory is safe
    // to tear down after this returns.
    host_audio_set_pull(NULL, 0);
    snes_sndq_active = 0;
    if (sram_pending) srm_save();
    S9xDeinitDisplay();
    host_clear_screen();
    return 0;
}
