// Entry point + platform glue for the Gwenesis (Sega Genesis / Mega Drive)
// ELF module on T-Deck. Same pattern as the gnuboy, NGPC and Sega8 modules:
// trap exit(), own the main loop, poll input, blit async, push audio, yield
// every frame.
//
// argv: <rom_vfs_path> [-fs N] [-idle 0|1]
//   rom_vfs_path : /sd/... or /littlefs/...
//   -fs N        : render 1 frame in N+1 (0 = every frame). The 68000, Z80,
//                  VDP and FM all still run every frame; only rendering and
//                  the blit are skipped. Default 2.
//   -idle 0|1    : idle-loop skip (default on). Off is slower but immune to a
//                  loop shape the detector reads wrong; see m68kcpu.h.
//
// The launcher's -keymap remaps physical keys onto the canonical codes below
// before they reach this module, and -trkball/-stackkb are consumed by the
// host, so both are ignored here.

#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "gwenesis-src/src/bus/gwenesis_bus.h"
#include "gwenesis-src/src/io/gwenesis_io.h"
#include "gwenesis-src/src/vdp/gwenesis_vdp.h"
#include "gwenesis-src/src/sound/z80inst.h"
#include "gwenesis-src/src/sound/ym2612.h"
#include "gwenesis-src/src/sound/gwenesis_sn76489.h"
#include "gwenesis-src/src/cpus/M68K/m68k.h"

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
extern void     host_audio_set_pull(void (*cb)(int16_t *out, int count), int sample_rate);
extern uint32_t host_psram_largest_free(void);

// Idle-loop skip toggle, m68kcpu.c. Launcher-controlled: it is the largest
// win in this port and the part most likely to misread a game's loop shape.
extern void     m68k_set_idle_skip(int on);

// Cartridge SRAM, m68kcpu.c. Gwenesis had none; the window is served from the
// 68000 read/write fast path because that path bypasses the bus decode.
extern void     m68k_set_sram(uint8_t *buf, uint32_t start, uint32_t end);
extern int      m68k_sram_dirty(void);
extern void     m68k_sram_clear_dirty(void);

// ---------------------------------------------------------------------------
// Exit traps
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int     s_exit_code = 0;

void exit(int code) { s_exit_code = code; longjmp(s_exit_jmp, 1); }
void abort(void) { host_log("genesis: abort()"); s_exit_code = 1; longjmp(s_exit_jmp, 1); }

// ---------------------------------------------------------------------------
// Emulator globals the core declares extern and expects the frontend to own.
// (gwenesis keeps its clocks and audio ring in the port layer so the port can
// decide their storage; retro-go's main.c does exactly the same.)
// ---------------------------------------------------------------------------
// Rate the firmware mixer is handed. 44100 is its own rate, so it applies an
// upsample factor of 1 and asks for a full CHUNK (256) per call; the mixer
// only does integer upsampling, and every lower option would throw away chip
// bandwidth we have already paid to synthesise.
#define AUDIO_RATE    44100

// What the chips actually run at: one sample per AUDIO_FREQ_DIVISOR ticks of
// system clock, and 262 lines * 3420 cycles * 60 frames is a second of those.
// Comes out at 53283Hz, which genesis_sound_pull() resamples to AUDIO_RATE.
#define NATIVE_RATE   (MCYCLES_PER_FRAME_NTSC * 60 / AUDIO_FREQ_DIVISOR)
#define RESAMPLE_STEP ((uint32_t)(((uint32_t)NATIVE_RATE << 16) / AUDIO_RATE))

// The chip rings hold one mixer chunk's worth of native samples, not a frame:
// 256 output samples need 256 * 53283/44100 = 310 of them. Frame length no
// longer enters into it, so PAL needs no separate size.
#define AUDIO_BUFLEN  512

int system_clock;
int scan_line;

// Memory the embedded path expects the platform to own. gwenesis declares
// M68K_RAM as a pointer and takes ROM_DATA from the G&W rom_manager, so both
// are ours to place; VRAM is bound to emulator_framebuffer at load time.
// This is why load_cartridge() takes no arguments on this path — the ROM is
// referenced where it already sits rather than copied into a static array.
unsigned char *ROM_DATA = NULL;
uint8_t        emulator_framebuffer[1024 * 64];

int16_t gwenesis_sn76489_buffer[AUDIO_BUFLEN];
int     sn76489_index;
int     sn76489_clock;
int16_t gwenesis_ym2612_buffer[AUDIO_BUFLEN];
int     ym2612_index;
int     ym2612_clock;

// Save states are not wired up; the core still references these symbols from
// its own save/load paths, so they resolve to no-ops rather than being cut.
typedef struct SaveState SaveState;
SaveState *saveGwenesisStateOpenForRead(const char *name) { (void)name; return (SaveState *)1; }
SaveState *saveGwenesisStateOpenForWrite(const char *name) { (void)name; return (SaveState *)1; }
int  saveGwenesisStateGet(SaveState *s, const char *tag) { (void)s; (void)tag; return 0; }
void saveGwenesisStateSet(SaveState *s, const char *tag, int v) { (void)s; (void)tag; (void)v; }
void saveGwenesisStateGetBuffer(SaveState *s, const char *tag, void *buf, int len)
{ (void)s; (void)tag; (void)buf; (void)len; }
void saveGwenesisStateSetBuffer(SaveState *s, const char *tag, void *buf, int len)
{ (void)s; (void)tag; (void)buf; (void)len; }

// Input is pushed with gwenesis_io_pad_press/release_button, so the core's
// pull hook has nothing to do.
void gwenesis_io_get_buttons(void) {}

// ---------------------------------------------------------------------------
// Display. In the embedded build the VDP writes RGB565 shorts straight into
// this buffer at a fixed SCREEN_WIDTH stride, and gwenesis_vdp_mem.c is
// patched to byte-swap each palette entry as it is computed, so what lands
// here is already in TFT wire order. The delivered frame is always 320 wide:
// 320x224 (NTSC) or 320x240 (PAL), with H32's 256 pixels upscaled by the VDP.
// ---------------------------------------------------------------------------
// Two buffers, used as alternating render targets: the VDP draws straight
// into the one we are about to hand to the SPI task, so a displayed frame is
// never copied. The other is free because the task is still reading it.
//
// There is no scaler here. 320 wide against a 320x240 panel is already the
// right presentation and the host centres it — a scaler would cost a third
// full-size buffer to gain a 224->240 vertical stretch nobody asked for.
#define GEN_MAX_H  240

static uint16_t s_fb[2][SCREEN_WIDTH * GEN_MAX_H];
static int      s_fb_idx = 0;
static int      s_last_w = -1, s_last_h = -1;

// Rows are always SCREEN_WIDTH (320) wide and contiguous, in BOTH video modes.
// H32 does not produce narrow rows: gwenesis_vdp_gfx.c renders its 256 pixels
// into a scratch line and runs blit_4to5_line() over it, so what reaches this
// buffer is already upscaled to 320 — which is also the right presentation,
// since H32 and H40 occupy the same width on a real display.
//
// src_w is therefore the VDP's SOURCE width, carried for the log line only.
// Using it as the blit width crops the right 64 pixels off an H32 frame.
static void blit_frame(int src_w, int src_h)
{
    if (src_w != s_last_w || src_h != s_last_h) {
        printf("[genesis] video %dx%d (blit %dx%d)\n",
               src_w, src_h, SCREEN_WIDTH, src_h);
        s_last_w = src_w;
        s_last_h = src_h;
    }

    host_blit_frame_async(s_fb[s_fb_idx], SCREEN_WIDTH, src_h);
    s_fb_idx ^= 1;
}

// ---------------------------------------------------------------------------
// Input. Canonical key codes (the launcher's -keymap produces these) mapped
// onto gwenesis pad button indices (PAD_* come from gwenesis_io.h). The
// Genesis pad is 3-button + Start.
// ---------------------------------------------------------------------------
static void poll_input(void)
{
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        int btn = -1;
        switch (key) {
            case 'w': case 0x81: btn = PAD_UP;    break;
            case 's': case 0x82: btn = PAD_DOWN;  break;
            case 'a': case 0x83: btn = PAD_LEFT;  break;
            case 'd': case 0x84: btn = PAD_RIGHT; break;
            case 'n':            btn = PAD_A; break;
            case 'm': case 0x85: btn = PAD_B; break;
            case 'k':            btn = PAD_C; break;
            case 0x0D:           btn = PAD_S; break;   // Start
            default: continue;
        }
        if (pressed) gwenesis_io_pad_press_button(0, btn);
        else         gwenesis_io_pad_release_button(0, btn);
    }
}

// ---------------------------------------------------------------------------
// Audio: pull model. Both chips emit mono, one int16 per sample; the firmware
// sound task asks for a fixed number of samples and gets exactly that many.
// The mixer owns volume and mute.
//
// A push model hands over a frame of samples in one burst
// every 17ms while the mixer consumed 128 every 5.8ms. Its ring drained to
// empty before the next burst arrived, and the mixer adds nothing to output
// slots it cannot fill -- a hole in the waveform every frame, at any frame
// rate. Pull removes the coupling: emulation speed changes how much emulated
// time a chunk covers, never how many samples the mixer receives.
// ---------------------------------------------------------------------------
#define AUDIO_WARMUP_FRAMES 60

static volatile int s_audio_warmup = AUDIO_WARMUP_FRAMES;

// ---------------------------------------------------------------------------
// Register-write queue.
//
// FM synthesis was ~20% of Core 0's frame. The chips belong to the firmware
// sound task now: the CPUs only record each register write with the system
// clock it happened at, and the pull callback replays them in order, running
// the chip up to each write's timestamp before applying it — so the audio
// hears the same sequence at the same times it would have on one core.
//
// The exception is the status register. Games poll the YM2612's timer flags
// for music timing, so a read cannot wait for the sound task. The callback
// publishes the byte and the CPUs read that copy: a single byte cannot tear,
// and the value is at worst one chunk stale.
//
// system_clock restarts at 0 every frame, so a write's own timestamp is only
// meaningful within its frame. s_frame_base accumulates whole frames and the
// CPUs add the in-frame offset, giving one timeline the sound task can walk.
// It wraps every ~80 seconds, which is why every comparison against it is a
// signed difference rather than a magnitude test.
// ---------------------------------------------------------------------------
#define SND_EVT_MAX  4096
#define SND_EVT_MASK (SND_EVT_MAX - 1)

typedef struct { uint8_t kind; uint8_t addr; uint8_t value; uint32_t clock; } snd_evt_t;
enum { EVT_YM = 0, EVT_PSG = 1 };

static snd_evt_t         s_evt[SND_EVT_MAX];
static volatile uint32_t s_evt_head = 0;    // written by the CPUs
static volatile uint32_t s_evt_tail = 0;    // written by the sound task
static uint32_t          s_frame_base = 0;  // CPU side only
static volatile uint32_t s_emu_clock = 0;   // published at each frame end
static volatile uint8_t  s_ym_status = 0;   // published by the sound task
static uint32_t          s_audio_clock = 0; // sound task only
static uint32_t          s_evt_dropped = 0;

// Resampler carry: the two native samples the current output position sits
// between, and where between them it sits in 16.16.
static uint32_t s_res_phase = 0;
static int32_t  s_res_a = 0, s_res_b = 0;

static inline void snd_queue(uint8_t kind, uint8_t addr, uint8_t value, int target)
{
    uint32_t h = s_evt_head;
    if (h - s_evt_tail >= SND_EVT_MAX) { s_evt_dropped++; return; }
    snd_evt_t *e = &s_evt[h & SND_EVT_MASK];
    e->kind  = kind;
    e->addr  = addr;
    e->value = value;
    e->clock = s_frame_base + (uint32_t)target;
    __asm__ volatile("" ::: "memory");   // fill the slot before claiming it
    s_evt_head = h + 1;
}

// Called from the 68000/Z80 through the headers; see the renames in ym2612.c.
void YM2612Write(unsigned int a, unsigned int v, int target)
{
    snd_queue(EVT_YM, (uint8_t)(a & 0x3), (uint8_t)(v & 0xff), target);
}

void gwenesis_SN76489_Write(int data, int target)
{
    snd_queue(EVT_PSG, 0, (uint8_t)(data & 0xff), target);
}

unsigned int YM2612Read(int target)
{
    (void)target;
    return s_ym_status;
}

// Sound-task context: must not block and must not allocate.
static void genesis_sound_pull(int16_t *out, int count)
{
    if (count <= 0) return;

    // Native samples this chunk steps through: one per crossing of a sample
    // boundary by the output phase. Not one more — the interpolator's forward
    // sample is s_res_b, which carries over, so synthesising an extra here
    // would advance the chips past a sample nothing ever reads.
    uint32_t phase = s_res_phase;
    int need = (int)((phase + (uint32_t)count * RESAMPLE_STEP) >> 16);
    if (need > AUDIO_BUFLEN) need = AUDIO_BUFLEN;

    // s_audio_clock decides only where a queued write lands inside the chunk —
    // the chips synthesise `need` samples forward either way — so re-anchoring
    // it costs nothing. Without it the clock drifts ahead of the emulator
    // whenever the frame rate is under 60, every write then arrives late, and
    // a frame of DAC writes collapses onto sample 0 with the drums in it.
    uint32_t emu = s_emu_clock;
    int32_t  lag = (int32_t)(emu - s_audio_clock);
    if (lag < 0 || lag > (int32_t)(3 * MCYCLES_PER_FRAME_NTSC))
        s_audio_clock = emu - MCYCLES_PER_FRAME_NTSC;

    uint32_t span = (uint32_t)need * AUDIO_FREQ_DIVISOR;
    uint32_t end  = s_audio_clock + span;

    // Chip-local time restarts at 0 each chunk: ym2612_run indexes the buffers
    // from ym2612_clock, and they only hold this chunk.
    ym2612_clock  = 0;
    ym2612_index  = 0;
    sn76489_clock = 0;
    sn76489_index = 0;

    uint32_t tail = s_evt_tail;
    uint32_t head = s_evt_head;
    __asm__ volatile("" ::: "memory");   // claim the range before reading it
    while (tail != head) {
        const snd_evt_t *e = &s_evt[tail & SND_EVT_MASK];
        if ((int32_t)(e->clock - end) >= 0) break;   // belongs to a later chunk
        int32_t local = (int32_t)(e->clock - s_audio_clock);
        if (local < 0) local = 0;                    // arrived late; apply now
        if (e->kind == EVT_YM) ym2612_write_chip(e->addr, e->value, local);
        else                   sn76489_write_chip(e->value, local);
        tail++;
    }
    s_evt_tail = tail;

    gwenesis_SN76489_run((int)span);
    ym2612_run((int)span);
    s_ym_status = (uint8_t)ym2612_read_chip((int)span);
    s_audio_clock = end;

    // Each chip only writes up to its own index. They advance off the same
    // clock so they normally match, but the PSG is treated as silent past its
    // own index rather than trusted to be in step.
    int n  = ym2612_index;  if (n > need) n = need;
    int ns = sn76489_index; if (ns > n) ns = n;
    if (s_audio_warmup > 0) n = ns = 0;

    // Native rate down to the mixer's, linear. The chips have no resampler —
    // their phase, LFO and envelope generators advance a fixed step per sample
    // — so the conversion has to happen here and not by retuning the divisor.
    int32_t a = s_res_a, b = s_res_b;
    int k = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = a + (((b - a) * (int32_t)((phase >> 8) & 0xff)) >> 8);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out[i] = (int16_t)v;

        phase += RESAMPLE_STEP;
        while (phase >= 0x10000u) {
            phase -= 0x10000u;
            a = b;
            if (k < n) {
                int32_t s = (int32_t)gwenesis_ym2612_buffer[k];
                if (k < ns) s += (int32_t)gwenesis_sn76489_buffer[k];
                b = s;
            } else {
                b = 0;
            }
            k++;
        }
    }
    s_res_phase = phase;
    s_res_a = a;
    s_res_b = b;

}

// ---------------------------------------------------------------------------
// Battery saves: <rom>.srm next to the ROM, same convention as the SNES module.
//
// The cartridge header declares the window: "RA" at 0x1B0, then big-endian
// start and end longs at 0x1B4 and 0x1B8. Those offsets are read through the
// ROM buffer AFTER the byte swap, so the ^1 matches FETCH8ROM.
//
// Bytes are stored at their address offset rather than packed. Most carts wire
// the chip to one byte lane only (odd, usually), which would halve the useful
// size if packed, and the lane flag in the header is not consistently set.
// Wasting at most 32KB of PSRAM avoids having to trust it.
// ---------------------------------------------------------------------------
#define SRAM_FLUSH_MS 2000

static uint8_t *s_sram = NULL;
static uint32_t s_sram_start = 0, s_sram_size = 0;
static char     s_srm_path[512];
static uint32_t s_sram_flush_at = 0;   // 0 = nothing pending

static uint8_t rom_hdr8(const uint8_t *rom, uint32_t off)
{
    return rom[off ^ 1];               // matches FETCH8ROM's swap
}

static uint32_t rom_hdr32(const uint8_t *rom, uint32_t off)
{
    return ((uint32_t)rom_hdr8(rom, off) << 24)
         | ((uint32_t)rom_hdr8(rom, off + 1) << 16)
         | ((uint32_t)rom_hdr8(rom, off + 2) << 8)
         |  (uint32_t)rom_hdr8(rom, off + 3);
}

static void build_srm_path(const char *rom)
{
    static const char ext[] = ".srm";
    int len = 0, dot = -1;
    while (rom[len] && len < (int)sizeof(s_srm_path) - 5) {
        if (rom[len] == '.') dot = len;
        s_srm_path[len] = rom[len];
        len++;
    }
    if (dot < 0) dot = len;
    for (int i = 0; ext[i]; i++) s_srm_path[dot + i] = ext[i];
    s_srm_path[dot + 4] = 0;
}

// Called after the ROM is loaded and byte-swapped, before load_cartridge().
static void sram_init(const uint8_t *rom, uint32_t rom_size, const char *rom_path)
{
    if (rom_hdr8(rom, 0x1B0) != 'R' || rom_hdr8(rom, 0x1B1) != 'A') {
        printf("[genesis] cart has no SRAM\n");
        return;
    }
    uint32_t start = rom_hdr32(rom, 0x1B4);
    uint32_t end   = rom_hdr32(rom, 0x1B8);
    if (end < start || (end - start) > 0xFFFF) {
        printf("[genesis] SRAM header bad (%06x..%06x), ignoring\n",
               (unsigned)start, (unsigned)end);
        return;
    }
    // Carts whose SRAM window sits inside the ROM need the 0xA130F1 bank
    // register to swap it in, which is not decoded. Mapping it unconditionally
    // would shadow real ROM and break the game, so leave it off and say so.
    if (start < rom_size) {
        printf("[genesis] SRAM %06x..%06x overlaps a %uKB ROM — needs bank "
               "register, saves disabled\n",
               (unsigned)start, (unsigned)end, (unsigned)(rom_size >> 10));
        return;
    }

    s_sram_size  = end - start + 1;
    s_sram_start = start;
    s_sram = (uint8_t *)calloc(1, s_sram_size);
    if (!s_sram) {
        printf("[genesis] SRAM alloc failed (%u bytes)\n", (unsigned)s_sram_size);
        s_sram_size = 0;
        return;
    }

    build_srm_path(rom_path);
    FILE *f = fopen(s_srm_path, "rb");
    if (f) {
        int n = (int)fread(s_sram, 1, s_sram_size, f);
        fclose(f);
        printf("[genesis] SRAM %06x..%06x (%uKB), loaded %d bytes\n",
               (unsigned)start, (unsigned)end,
               (unsigned)(s_sram_size >> 10), n);
    } else {
        printf("[genesis] SRAM %06x..%06x (%uKB), no .srm yet\n",
               (unsigned)start, (unsigned)end, (unsigned)(s_sram_size >> 10));
    }
    m68k_set_sram(s_sram, start, end);
}

static void sram_save(void)
{
    if (!s_sram) return;
    FILE *f = fopen(s_srm_path, "wb");
    if (!f) { printf("[genesis] .srm write failed: %s\n", s_srm_path); return; }
    fwrite(s_sram, 1, s_sram_size, f);
    fclose(f);
    printf("[genesis] .srm saved (%u bytes)\n", (unsigned)s_sram_size);
}

// Writes come in bursts, so the flush waits for them to stop rather than
// firing per write. Called once a frame; the SD write itself is not cheap.
static void sram_tick(uint32_t now_ms)
{
    if (!s_sram) return;
    if (m68k_sram_dirty()) {
        m68k_sram_clear_dirty();
        s_sram_flush_at = now_ms + SRAM_FLUSH_MS;
        return;
    }
    if (s_sram_flush_at && (int32_t)(now_ms - s_sram_flush_at) >= 0) {
        s_sram_flush_at = 0;
        sram_save();
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    host_log("genesis: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("genesis: exit()/abort() caught");
        host_clear_screen();
        return s_exit_code;
    }

    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { rom_path = argv[i]; break; }
    }
    if (!rom_path) { host_log("genesis: no ROM path"); return 1; }
    printf("[genesis] rom=%s\n", rom_path);

    // Frameskip 2 is the shipping default: the emulator holds 60fps of
    // emulated frames at a third of the render cost, which is where the
    // hardware actually sits.
    int frameskip = 2;
    int idle_skip = 1;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-fs") == 0) {
            int v = 0;
            for (const char *p = argv[i + 1]; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (*p - '0');
            if (v >= 0 && v <= 4) frameskip = v;
        } else if (strcmp(argv[i], "-idle") == 0) {
            idle_skip = (argv[i + 1][0] != '0');
        }
    }
    m68k_set_idle_skip(idle_skip);

    // ROM: read whole into PSRAM. load_cartridge takes ownership of the
    // buffer (the bus keeps pointing into it for the whole run), so it is
    // deliberately not freed here.
    FILE *f = fopen(rom_path, "rb");
    if (!f) { host_log("genesis: cannot open ROM"); return 1; }
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (rom_size <= 0 || rom_size > 8 * 1024 * 1024) {
        fclose(f);
        host_log("genesis: bad ROM size");
        return 1;
    }
    printf("[genesis] rom %uKB, psram largest %uKB\n",
           (unsigned)(rom_size / 1024),
           (unsigned)(host_psram_largest_free() / 1024));

    unsigned char *rom = (unsigned char *)malloc((size_t)rom_size);
    if (!rom) {
        fclose(f);
        printf("[genesis] ROM alloc of %u bytes failed (largest block %u)\n",
               (unsigned)rom_size, (unsigned)host_psram_largest_free());
        return 1;
    }
    size_t got = fread(rom, 1, (size_t)rom_size, f);
    fclose(f);
    if (got != (size_t)rom_size) { host_log("genesis: short ROM read"); return 1; }

    // Genesis ROM files are big-endian, but FETCH16ROM is a native (little-
    // endian) 16-bit read, so the image has to be byte-swapped in place. The
    // desktop build does this inside load_cartridge under ROM_SWAP; the
    // embedded path skips it because the Game & Watch build pre-swaps the ROM
    // into flash when the firmware is made. We hand it a raw file, so it is
    // ours to do -- without it the 68000 executes byte-swapped opcodes and
    // dies immediately (the cart header reads as "AETRWHRO MIJ M").
    for (size_t i = 0; i + 1 < (size_t)rom_size; i += 2) {
        unsigned char t = rom[i];
        rom[i]     = rom[i + 1];
        rom[i + 1] = t;
    }

    // Bind the platform memory before load_cartridge(): it zeroes M68K_RAM
    // and reads the cart header through ROM_DATA.
    extern unsigned char *M68K_RAM;
    M68K_RAM = (unsigned char *)calloc(1, MAX_RAM_SIZE);
    if (!M68K_RAM) { host_log("genesis: work RAM alloc failed"); return 1; }
    ROM_DATA = rom;

    // After the swap so the header reads correctly, and before power_on() so a
    // restored .srm is already in place the first time the game looks at it.
    sram_init(rom, (uint32_t)rom_size, rom_path);

    load_cartridge();
    power_on();
    reset_emulation();

    // Registered before the first frame so the queue always has a consumer;
    // the callback holds its output at silence until the warmup elapses.
    host_audio_set_pull(genesis_sound_pull, AUDIO_RATE);

    host_clear_screen();
    host_log("genesis: entering main loop");

    extern unsigned char gwenesis_vdp_regs[0x20];
    extern unsigned int  gwenesis_vdp_status;
    extern int           hint_pending;
    extern int           screen_width, screen_height;

    const uint32_t frame_us = 1000000u / 60;
    uint32_t next_us = host_get_ticks_us();
    int      skip_ctr = 0;
    uint32_t yield_ctr = 0;


    while (!host_should_exit()) {
        poll_input();

        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter    = gwenesis_vdp_regs[10];
        int draw            = (skip_ctr == 0);

        screen_width  = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;

        // Point the VDP at whichever buffer the SPI task is not reading, so
        // the frame it renders is the frame we hand over — no copy.
        gwenesis_vdp_set_buffer(s_fb[s_fb_idx]);
        gwenesis_vdp_render_config();

        // Per-frame clock reset. The audio-side clocks and indices belong to
        // the sound task now and are reset per chunk there.
        system_clock  = 0;
        zclk          = 0;
        scan_line     = 0;


        while (scan_line < lines_per_frame) {
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);

            // Sound synthesis happens on the firmware sound task; the CPUs
            // only record register writes, which the wrappers above do.
            if (draw && scan_line < screen_height)
                gwenesis_vdp_render_line(scan_line);

            // The line-interrupt counter reloads outside the active area.
            if ((scan_line == 0) || (scan_line > screen_height))
                hint_counter = REG10_LINE_COUNTER;

            if (--hint_counter < 0) {
                if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                    hint_pending = 1;
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                        m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;

            // V-blank starts at the end of the last rendered line.
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                z80_irq_line(1);
            }
            if (scan_line == (screen_height + 1))
                z80_irq_line(0);

            system_clock += VDP_CYCLES_PER_LINE;
        }

        // Rebase 68000 cycles for the next frame.
        m68k.cycles -= system_clock;

        if (draw)
            blit_frame(screen_width, screen_height);

        // Advance the timeline the CPUs stamp writes with, and publish it so
        // the sound task can keep its own clock anchored to ours.
        s_frame_base += (uint32_t)system_clock;
        s_emu_clock   = s_frame_base;
        if (s_audio_warmup > 0) s_audio_warmup--;

        sram_tick(host_get_ticks_us() / 1000u);

        skip_ctr = (skip_ctr > 0) ? (skip_ctr - 1) : frameskip;

        // Sleep only when genuinely ahead. Below full speed every frame lands
        // in the last branch, and host_sleep_ms(1) is a vTaskDelay of one tick
        // that blocks to the next 1ms boundary — half a millisecond of this
        // core, per frame, given away while already losing. Yielding one frame
        // in four still leaves ~11 a second, which is enough to keep the
        // watchdog fed and the other Core-0 tasks serviced.
        next_us += frame_us;
        int32_t ahead = (int32_t)(next_us - host_get_ticks_us());
        if (ahead > 2000)
            host_sleep_ms((uint32_t)(ahead - 1000) / 1000);
        else if (ahead < -(int32_t)(frame_us * 4))
            next_us = host_get_ticks_us();   // fell far behind: re-anchor
        else if ((++yield_ctr & 3) == 0)
            host_sleep_ms(1);
    }

    host_log("genesis: exiting");
    // Unregister before anything the callback touches goes away. The firmware
    // takes the mixer mutex to do it, so no call can still be in flight after.
    host_audio_set_pull(NULL, 0);
    // Unconditional, not only when a flush is pending: the last writes may be
    // inside the 2s window when the player quits, which is exactly when a save
    // has just been made.
    sram_save();
    if (s_evt_dropped)
        printf("[genesis] %u sound events dropped (queue full)\n",
               (unsigned)s_evt_dropped);
    host_clear_screen();
    return 0;
}
