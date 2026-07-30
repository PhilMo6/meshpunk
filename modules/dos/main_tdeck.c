// Entry point for the tiny386 DOS ELF module on T-Deck.
//
// Bridges tiny386's portable core onto the meshpunk firmware's host_* ELF ABI.
// No OS, no SDL, no ESP-IDF — same model as the gnuboy/NGPC/SNES modules.
// BUILD_ESP32 is deliberately NOT defined; see tiny386-src/VENDOR.md.
//
// argv: <elf_path> [-fda img] [-hda img] [-cd img] [-cfolder manifest]
//                  [-cpu 3|4|5|6] [-audio N] [-mouse N] [-smooth 0|1]
//                  [-audio N]      PERCENT of normal audio rate; 0 = off,
//                                  100 = normal. Sets how fast we drain the
//                                  guest's DMA, hence how often its audio ISR
//                                  runs: below 100 costs pitch and buys CPU,
//                                  above 100 polls more often and costs CPU.
//                  [-render 0|1]   1 = only re-render frames we will display
//                  [-native 0|1]   1 = render pixel-doubled modes at real size
//                  [-cro 0|1]      1 = mount folder-as-C: read-only
//                  [-sbdigi N]     Sound Blaster DSP: 1 on, 3 no-DMA,
//                                  2 no-IRQ, 0 off (see sb16.c) -- fabricated
//                                  card faults that make a game disable its
//                                  own digitised audio
//                  [-pitcap Hz]    cap channel-0 timer IRQ delivery (0 = off)
//                  [-mlatch 0|1]   1 = trackball click latches the mouse
//                                  button instead of following the press
//   Paths arrive in VFS form (/sd/... or /littlefs/...).
//   -keymap / -trkball / -stackkb / -kbtoggle are consumed by the host but
//   stay in argv.

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny386-src/pc.h"

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
extern uint32_t host_psram_largest_free(void);

extern int meshpunk_sb_rate_pct;   // sb16.c (percent; 100 = full)
extern int meshpunk_sb_digital;    // sb16.c (0 = DSP ports float; see there)
extern int meshpunk_pit_cap_hz;    // i8254.c (0 = off; see there)

// Our own translation units
void dos_video_init(const uint16_t *fb, int fb_w, int fb_h);
void dos_video_set_smoothing(int on);
void dos_video_set_render_gate(int on);
int  dos_video_render_due(void);
void dos_video_redraw(void *opaque, int x, int y, int w, int h);
void dos_video_flush(void);
void dos_input_key(PS2KbdState *kbd, unsigned char key, int pressed);
void dos_input_mouse_config(int speed);
void dos_input_latch_config(int on);
void dos_input_poll_mouse(PS2MouseState *mouse);
BlockDevice *dos_folderdisk_open(const char *manifest_path, int read_only);
void dos_folderdisk_flush(BlockDevice *bs);

// ---------------------------------------------------------------------------
// Exit traps — the core calls exit()/abort() on fatal paths (and assert()).
// Longjmp back to main() instead of killing the firmware.
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

void exit(int code) { s_exit_code = code; longjmp(s_exit_jmp, 1); }
void abort(void) { host_log("dos: abort()"); s_exit_code = 1; longjmp(s_exit_jmp, 1); }

// assert() in the vendored code routes here (NDEBUG is not set for it).
void __assert_func(const char *file, int line, const char *fn, const char *expr)
{
    printf("[dos] assert: %s:%d %s: %s\n", file ? file : "?", line,
           fn ? fn : "?", expr ? expr : "?");
    abort();
}

// ---------------------------------------------------------------------------
// Platform HAL required by tiny386 (declared in pc.h).
// pcmalloc() is NOT here: without BUILD_ESP32 the core #defines it to malloc.
// ---------------------------------------------------------------------------
uint32_t get_uticks(void)
{
    return host_get_ticks_us();
}

void *bigmalloc(size_t size)
{
    return malloc(size);   // host malloc is PSRAM-backed and leak-tracked
}

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) {
        printf("[dos] load_rom: cannot open %s\n", file);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    if (len <= 0) { fclose(fp); return 0; }
    printf("[dos] load_rom %s (%ld bytes)\n", file, len);
    if (backward)
        fread((char *)phys_mem + addr - len, 1, len, fp);
    else
        fread((char *)phys_mem + addr, 1, len, fp);
    fclose(fp);
    return (int)len;
}

// Small, per-iteration emulator state -> internal SRAM. See dos_mem.h for why.
// Falls back to PSRAM and says so: internal RAM is scarce and shared, and being
// slower is always better than failing to start.
void *meshpunk_hot_alloc(size_t size, const char *what)
{
    void *p = host_malloc_internal(size);
    const char *where = "internal";
    if (!p) { p = malloc(size); where = "psram"; }
    printf("[dos] hot %-4s %5u bytes -> %s\n", what, (unsigned)size,
           p ? where : "FAILED");
    return p;
}

// ---------------------------------------------------------------------------
// Guest RAM sizing.
//
// The firmware already maximises the contiguous block before we run:
// luaTearDown() closes Lua, drops the emoji/LVGL/font caches and destroys the
// Lua arena so everything coalesces, then the loader takes one contiguous
// allocation for our segments. So host_psram_largest_free() here already
// reflects both — we just walk down the rungs until one lands, in the same
// style as lua_arena_create() and the loader's own stack ladder.
// ---------------------------------------------------------------------------
// Headroom left ABOVE the guest RAM: VGA video memory (256KB) plus tiny386's
// own allocations (CPU TLB, device state). The framebuffer is NOT included --
// it is allocated before the ladder runs, so it is already accounted for in
// what host_psram_largest_free() reports. The module's blit buffers live in
// .bss and were carved out by the ELF loader before main() even started.
#define DOS_MEM_HEADROOM (768u * 1024u)
static const uint32_t MEM_RUNGS[] = { 6u << 20, 5u << 20, 4u << 20, 3u << 20, 2u << 20 };

static long alloc_guest_ram(void)
{
    uint32_t largest = host_psram_largest_free();
    printf("[dos] psram largest free: %uKB\n", (unsigned)(largest / 1024));
    for (unsigned i = 0; i < sizeof(MEM_RUNGS) / sizeof(MEM_RUNGS[0]); i++) {
        uint32_t want = MEM_RUNGS[i];
        if (want + DOS_MEM_HEADROOM > largest) continue;
        // pc_new() does the actual allocation; probe with the same allocator so
        // a rung that cannot land is rejected here rather than inside the core.
        void *probe = malloc(want);
        if (probe) {
            free(probe);
            printf("[dos] guest RAM %uMB (%uKB spare above it)\n",
                   (unsigned)(want >> 20), (unsigned)((largest - want) / 1024));
            return (long)want;
        }
        printf("[dos] %uMB failed, trying smaller...\n", (unsigned)(want >> 20));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Audio — push model.
//
// The firmware offers two: a module registers a pull callback that the sound
// task (Core 1) invokes every mixer chunk, or it pushes finished samples into a
// ring the sound task drains without entering module code at all. Doom, the
// GameBoy, the NES and the PC-XT all push. This module pushes: synthesis runs
// here on Core 0, in batches sized by us, between emulation batches.
//
// 44100 is not a free choice: every audio source in tiny386 synthesises at
// exactly this rate -- pcspk.c's PCSPK_SAMPLE_RATE, adlib.c's
// OPLCreate(3579545, 44100), and sb16.c resamples all guest formats to 44100.
// Declaring anything else makes the host resample a stream that is already at
// its target rate (22050 played everything an octave low at half speed). It is
// also the host mixer's native rate, so upsample == 1 and the interpolation
// path is skipped entirely.
// ---------------------------------------------------------------------------
#define AUDIO_RATE       44100
#define MIX_SLICE        128    // stereo frames per mixer_callback (its tmpbuf
                                // asserts free/2 <= MIXER_BUF_LEN)
#define AUDIO_MIN_BATCH  512    // ~11.6ms: batch up, per-loop pushes are pointless

// FIXED, and it has to be. The obvious-looking "adapt the cushion to the
// observed stall" was built and hw-REJECTED 2026-07-27: unplayable, worse than
// baseline. The flaw is that building cushion means pushing MORE samples than
// real time asked for -- and every one of those is generated by pumping the
// guest's DMA, which advances left_till_irq and raises MORE interrupts. Growing
// the cushion is therefore identical to turning the audio rate ABOVE 100%: it
// hands the guest extra ISR work (the single largest CPU consumer here) and
// overflows the firmware's 4096-sample ring, which then drops the excess. Both
// symptoms at once, speed and crackle.
//
// A cushion can only be grown with samples that do NOT come from the guest.
// Until something does that, these stay constants sized for the worst stall:
// the guest's own SoundBlaster handler, measured up to 29ms during dense music.
// HARD CEILING: the firmware ring, EXTERN_RING_SIZE in sound.cpp = 4096 samples
// (93ms). Steady-state occupancy is about AUDIO_PRIME and a push adds up to
// AUDIO_MAX_BATCH on top, so PRIME + MAX_BATCH must stay under it or
// sound_extern_push() silently drops the excess, which is itself a crackle.
// 2048 + 1920 = 3968 leaves a small margin. Raising these means raising the
// firmware ring too -- tried at 8192/3072/3072 and it changed nothing audible.
#define AUDIO_MAX_BATCH  1920   // ~44ms of catch-up after a stall
#define AUDIO_PRIME      2048   // ~46ms head start; the underrun protection

static PC *s_pc = 0;
static int s_audio_on = 0;
static int16_t s_mix_scratch[MIX_SLICE * 2];   // stereo, from mixer_callback
static int16_t s_push_buf[AUDIO_MAX_BATCH];    // mono, handed to the firmware
static uint32_t s_audio_last_us = 0;

// Folder-as-C: backend, at file scope so the exit()/abort() longjmp path can
// commit it too — a guest that faults must not lose the writes it already made.
static BlockDevice *s_folder_bd = 0;

// ---------------------------------------------------------------------------
// DIAGNOSTIC: WAV capture of everything we hand to the firmware.
//
// The capture point is the module/firmware boundary -- the exact buffer passed
// to host_audio_push(), after the SB16 resample and the stereo->mono downmix,
// before any firmware mixing or upsampling. So the file IS the emulator's
// finished audio: whatever is wrong upstream of the speaker is visible in it,
// and anything that looks clean here is the firmware's problem instead.
//
// 44100 Hz, mono, signed 16-bit PCM. ~88KB per second of capture.
// SD writes from the audio path are SLOW and will visibly lag the emulator.
// That is accepted for a diagnostic run; drop -DDOS_WAVCAP=1 to compile it out.
// ---------------------------------------------------------------------------
#ifdef DOS_WAVCAP
#define WAV_PATH "/sd/dos_audio.wav"
static FILE *s_wav = 0;
static uint32_t s_wav_bytes = 0;
static int s_wav_failed = 0;

static void wav_put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wav_put16(uint8_t *p, uint16_t v)
{ p[0] = v; p[1] = v >> 8; }

static void wav_write(const int16_t *samples, int n)
{
    if (s_wav_failed) return;
    if (!s_wav) {
        s_wav = fopen(WAV_PATH, "wb");
        if (!s_wav) {
            printf("[wav] cannot open " WAV_PATH "\n");
            s_wav_failed = 1;
            return;
        }
        // 44-byte placeholder; patched with real sizes in wav_close().
        uint8_t zero[44];
        memset(zero, 0, sizeof(zero));
        fwrite(zero, 1, sizeof(zero), s_wav);
        s_wav_bytes = 0;
        printf("[wav] capturing to " WAV_PATH " (44100 mono s16)\n");
    }
    fwrite(samples, sizeof(int16_t), n, s_wav);
    s_wav_bytes += (uint32_t)n * 2u;
}

// Called on BOTH exit paths -- the normal shutdown and the exit()/abort()
// longjmp -- or the header stays zeroed and the file is unreadable.
static void wav_close(void)
{
    if (!s_wav) return;
    uint8_t h[44];
    memcpy(h + 0,  "RIFF", 4);      wav_put32(h + 4,  36 + s_wav_bytes);
    memcpy(h + 8,  "WAVEfmt ", 8);  wav_put32(h + 16, 16);
    wav_put16(h + 20, 1);           /* PCM      */
    wav_put16(h + 22, 1);           /* mono     */
    wav_put32(h + 24, AUDIO_RATE);
    wav_put32(h + 28, AUDIO_RATE * 2);  /* byte rate  */
    wav_put16(h + 32, 2);               /* block align */
    wav_put16(h + 34, 16);              /* bits       */
    memcpy(h + 36, "data", 4);      wav_put32(h + 40, s_wav_bytes);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_wav);
    fclose(s_wav);
    s_wav = 0;
    printf("[wav] wrote %u samples (%u bytes) to " WAV_PATH "\n",
           (unsigned)(s_wav_bytes / 2u), (unsigned)s_wav_bytes);
}
#else
#define wav_write(s, n) ((void)0)
#define wav_close()     ((void)0)
#endif

// Called from the emulation loop. Cheap when nothing is due: one clock read.
static void dos_audio_service(void)
{
    if (!s_audio_on || !s_pc) return;

    uint32_t now = host_get_ticks_us();
    if (!s_audio_last_us) {
        // First call: back-date the anchor so the opening batch builds the
        // cushion in one go, then normal pacing maintains it.
        s_audio_last_us = now - (uint32_t)(((uint64_t)AUDIO_PRIME * 1000000u)
                                           / AUDIO_RATE);
        return;
    }

    // Produce exactly what real time has asked for since the last push -- never
    // more. Pushing extra costs guest ISR work; see the note above the defines.
    uint32_t elapsed = (uint32_t)(now - s_audio_last_us);
    uint32_t want = (uint32_t)(((uint64_t)elapsed * AUDIO_RATE) / 1000000u);
    if (want < AUDIO_MIN_BATCH) return;

    if (want > AUDIO_MAX_BATCH) {
        // Further behind than one batch can cover. Re-anchor rather than chase:
        // the ring holds 93ms, so a burst that large is partly dropped anyway.
        want = AUDIO_MAX_BATCH;
        s_audio_last_us = now;
    } else {
        // Credit accumulation rather than now-anchoring, so rounding does not
        // slowly drift the sample clock against real time.
        s_audio_last_us += (uint32_t)(((uint64_t)want * 1000000u) / AUDIO_RATE);
    }

    uint32_t done = 0;
    while (done < want) {
        uint32_t frames = want - done;
        if (frames > MIX_SLICE) frames = MIX_SLICE;

        // Fill the SoundBlaster's buffer from guest RAM before draining it.
        // Producer and consumer are the same thread here, so the race the
        // upstream source documents ("XXX: There are races") cannot happen.
        pc_audio_dma_pump(s_pc);

        memset(s_mix_scratch, 0, (size_t)frames * 2 * sizeof(int16_t));
        mixer_callback(s_pc, (uint8_t *)s_mix_scratch,
                       (int)frames * 2 * (int)sizeof(int16_t));

        // Interleaved stereo -> mono.
        for (uint32_t i = 0; i < frames; i++) {
            int32_t l = s_mix_scratch[i * 2];
            int32_t r = s_mix_scratch[i * 2 + 1];
            s_push_buf[done + i] = (int16_t)((l + r) / 2);
        }
        done += frames;    // next slice charges this mixing to pf_dma
    }

    wav_write(s_push_buf, (int)done);
    host_audio_push(s_push_buf, (int)done, AUDIO_RATE);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
#define FB_W 640
#define FB_H 480

int main(int argc, char **argv)
{
    const char *fda = 0, *hda = 0, *cd = 0, *cfolder = 0;
    int cpu_gen = 4;         // 486-class by default: same as upstream's own default
    int audio_pct = 100; // PERCENT of normal rate. 0 = off.
    int mouse_speed = 0;
    int smoothing = 1;
    int render_gate = 1;
    int native_res = 1;
    int cdrive_ro = 0;
    int sb_digital = 1;
    int pit_cap = 0;
    int m_latch = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-fda")     && i + 1 < argc) fda = argv[++i];
        else if (!strcmp(argv[i], "-hda")     && i + 1 < argc) hda = argv[++i];
        else if (!strcmp(argv[i], "-cd")      && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "-cfolder") && i + 1 < argc) cfolder = argv[++i];
        else if (!strcmp(argv[i], "-cpu")     && i + 1 < argc) cpu_gen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-audio")   && i + 1 < argc) audio_pct = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mouse")   && i + 1 < argc) mouse_speed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-smooth")  && i + 1 < argc) smoothing = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-render")  && i + 1 < argc) render_gate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-native")  && i + 1 < argc) native_res = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-cro")     && i + 1 < argc) cdrive_ro = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sbdigi")  && i + 1 < argc) sb_digital = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-pitcap")  && i + 1 < argc) pit_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mlatch")  && i + 1 < argc) m_latch = atoi(argv[++i]);
        else if (argv[i][0] == '-') { if (i + 1 < argc) i++; }  // -keymap/-trkball/-stackkb
    }
    /* -sbdigi: 1 = on, 3 = no-dma, 2 = no-irq, 0 = DSP off */
    meshpunk_sb_digital = (sb_digital >= 0 && sb_digital <= 3) ? sb_digital : 1;
    meshpunk_pit_cap_hz = (pit_cap > 0) ? pit_cap : 0;

    if (!fda && !hda && !cfolder && !cd) {
        host_log("dos: no bootable disk (-fda / -hda / -cd / -cfolder)");
        return 1;
    }

    // BIOS images live next to the module ELF; argv[0] arrives in firmware
    // "L:/..." / "S:/..." form, so convert it the same way the launcher does
    // for disk paths.
    static char dir_buf[256], bios_buf[288], vgabios_buf[288];
    const char *self = argc > 0 ? argv[0] : "";
    const char *prefix = "";
    if (self[0] == 'L' && self[1] == ':')      { prefix = "/littlefs"; self += 2; }
    else if (self[0] == 'S' && self[1] == ':') { prefix = "/sd";       self += 2; }
    snprintf(dir_buf, sizeof(dir_buf), "%s%s", prefix, self);
    char *slash = strrchr(dir_buf, '/');
    if (slash) *slash = 0;
    snprintf(bios_buf, sizeof(bios_buf), "%s/bios.bin", dir_buf);
    snprintf(vgabios_buf, sizeof(vgabios_buf), "%s/vgabios.bin", dir_buf);

    // Framebuffer first, THEN size the guest RAM. The ladder probes the heap,
    // so anything allocated after it would invalidate the measurement -- with
    // the old order the probe was optimistic by the framebuffer's 614KB and a
    // rung could be accepted that pc_new() then could not actually satisfy.
    uint16_t *fb = (uint16_t *)malloc((size_t)FB_W * FB_H * 2);
    if (!fb) {
        host_log("dos: framebuffer alloc failed");
        return 1;
    }
    memset(fb, 0, (size_t)FB_W * FB_H * 2);

    long mem_size = alloc_guest_ram();
    if (!mem_size) {
        host_log("dos: not enough contiguous PSRAM for guest RAM");
        return 1;
    }

    PCConfig conf;
    memset(&conf, 0, sizeof(conf));
    conf.bios         = bios_buf;
    conf.vga_bios     = vgabios_buf;
    conf.mem_size     = mem_size;
    conf.vga_mem_size = 256 * 1024;
    conf.width        = FB_W;
    conf.height       = FB_H;
    conf.cpu_gen      = cpu_gen;
    conf.fpu          = 1;
    conf.enable_serial = 0;
    // Force 8-dot character cells so 80-column text is 640 wide, not 720, and
    // therefore fits the framebuffer. vga.c refuses to draw a text mode larger
    // than the canvas (graphics modes are cropped instead), so without this
    // the DOS prompt would simply not render.
    conf.vga_force_8dm = 1;
    conf.fill_cmos     = 1;

    if (fda)     conf.fdd[0]   = fda;
    if (hda)     conf.disks[0] = hda;
    if (cd)    { conf.disks[2] = cd; conf.iscd[2] = 1; }

    printf("[dos] fda=%s hda=%s cd=%s cfolder=%s cpu=%d audio=%d mouse=%d\n",
           fda ? fda : "-", hda ? hda : "-", cd ? cd : "-",
           cfolder ? cfolder : "-", cpu_gen, audio_pct, mouse_speed);

    dos_video_set_smoothing(smoothing);
    dos_video_set_render_gate(render_gate);
    meshpunk_vga_native = native_res;
    dos_video_init(fb, FB_W, FB_H);
    dos_input_mouse_config(mouse_speed);
    dos_input_latch_config(m_latch);

    if (setjmp(s_exit_jmp)) {
        printf("[dos] module exited (code %d)\n", s_exit_code);
        wav_close();
        if (s_folder_bd) dos_folderdisk_flush(s_folder_bd);
        return s_exit_code;
    }

    PC *pc = pc_new(dos_video_redraw, 0, (u8 *)fb, &conf);
    if (!pc) {
        host_log("dos: pc_new failed");
        return 1;
    }
    s_pc = pc;

    // Folder-as-C: replaces the IDE backend for disk 0 after construction —
    // the config path only accepts filenames.
    if (cfolder) {
        s_folder_bd = dos_folderdisk_open(cfolder, cdrive_ro);
        if (s_folder_bd) ide_attach_bd(pc->ide, 0, s_folder_bd);
        else host_log("dos: folder C: manifest failed, continuing without it");
    }

    load_bios_and_reset(pc);
    host_clear_screen();

    // -audio is a RATE in percent, not a toggle. The guest's SoundBlaster ISR
    // only runs because we drain its DMA buffer, so its rate is ours to set:
    // draining slower means fewer interrupts and less emulated CPU spent
    // inside the game's audio handler. Pitch and speed drop with it -- that
    // work IS the audio. See meshpunk_sb_rate_pct in sb16.c.
    s_audio_on = (audio_pct != 0);
    meshpunk_sb_rate_pct = (audio_pct > 0) ? audio_pct : 100;

    host_log("dos: entering main loop");
    pc->boot_start_time = get_uticks();

    uint32_t last_yield_us = host_get_ticks_us();

    while (!host_should_exit() && pc->shutdown_state != 8) {
        pc_step(pc);

        // Inlined pc_vga_step() so the expensive half can be gated. vga_step()
        // must still run every iteration: it advances the retrace state machine
        // that drives the ST01 register games poll to wait for vblank. Only
        // vga_refresh() -- a full re-render of the guest screen into the
        // framebuffer -- is skipped, and only for frames we would not display.
        // A pending full_update always renders so a mode change is never stale.
        if (vga_step(pc->vga)) {
            if (pc->full_update || dos_video_render_due()) {
                vga_refresh(pc->vga, pc->redraw, pc->redraw_data,
                            pc->full_update != 0);
                if (pc->full_update == 2)
                    pc->full_update = 0;
                // Split the render+blit stall. These two together were measured
                // at ~30ms, long enough to drain the audio ring; servicing
                // between them halves the longest silent gap. Costs one clock
                // read on the frames that do not render.
                dos_audio_service();
            }
        }

        int pressed;
        unsigned char key;
        while (host_get_key(&pressed, &key))
            dos_input_key(pc->kbd, key, pressed);
        dos_input_poll_mouse(pc->mouse);

        dos_video_flush();
        dos_audio_service();

        // Feed the watchdog and let Core 0's other tasks (radio, blit) run.
        // pc_step() is bounded by PC_STEP_COUNT (512 here), so this is reached
        // often; upstream's desktop loop has no yield at all.
        // 16ms, not 8: host_sleep_ms(1) costs ~0.5ms of scheduler round-trip,
        // so at 8ms it was 125 yields/s = ~6% of wall time. pc_step() is bounded
        // by PC_STEP_COUNT so the watchdog still gets fed well inside its window.
        uint32_t now = host_get_ticks_us();
        if ((uint32_t)(now - last_yield_us) >= 16000) {
            last_yield_us = now;
            host_sleep_ms(1);
        }
    }

    printf("[dos] shutdown_state=%d\n", pc->shutdown_state);
    // Commit the folder disk: closes the cached handle so FatFs writes out the
    // last file's size, and trims leftover sector padding now that the guest
    // has definitely stopped writing.
    wav_close();
    if (s_folder_bd) dos_folderdisk_flush(s_folder_bd);
    return 0;
}
