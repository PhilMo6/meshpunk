// T-Deck host-interface implementations for Faux86-remake (see tdeck_host.h).

#include "tdeck_host.h"
#include "faux86-src/VM.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Host function imports (resolved at ELF load time by elf_host.cpp)
// ---------------------------------------------------------------------------
extern "C" {
    void     host_blit_frame_async(const uint16_t* rgb565, int w, int h);
    uint32_t host_get_ticks_us(void);
    void     host_audio_push(const int16_t* samples, int count, int sample_rate);
    void     host_log(const char* msg);
}

namespace Faux86
{

// ---------------------------------------------------------------------------
// Logging (declared in HostSystemInterface.h, must be provided by the host)
// ---------------------------------------------------------------------------
void log(LogChannel channel, const char* message, ...)
{
    if (channel == LogVerbose)
        return; // too chatty for the serial console
    char buf[256];
    va_list args;
    va_start(args, message);
    vsnprintf(buf, sizeof(buf), message, args);
    va_end(args);
    printf("[faux86] %s\n", buf);
}

// Non-pure virtuals declared without bodies upstream (platform shells
// normally provide them).
void HostSystemInterface::init(VM* inVM) { vm = inVM; }
void HostSystemInterface::resize(uint32_t desiredWidth, uint32_t desiredHeight)
{
    scrWidth = desiredWidth;
    scrHeight = desiredHeight;
}

// ---------------------------------------------------------------------------
// FileDisk
// ---------------------------------------------------------------------------
FileDisk::FileDisk(const char* path)
{
    m_file = fopen(path, "r+b");
    if (!m_file) {
        m_file = fopen(path, "rb"); // ROMs / write-protected images
        m_readOnly = true;
    }
    if (m_file) {
        fseek(m_file, 0, SEEK_END);
        long sz = ftell(m_file);
        m_size = (sz > 0) ? (uint64_t)sz : 0;
        fseek(m_file, 0, SEEK_SET);
        printf("[faux86] disk open: %s (%u bytes%s)\n",
               path, (unsigned)m_size, m_readOnly ? ", read-only" : "");
    } else {
        printf("[faux86] disk open FAILED: %s\n", path);
    }
}

FileDisk::~FileDisk()
{
    if (m_file) {
        fflush(m_file);
        fclose(m_file);
        m_file = nullptr;
    }
}

int FileDisk::read(uint8_t* buffer, unsigned count)
{
    if (!m_file) return 0;
    return (int)fread(buffer, 1, count, m_file);
}

int FileDisk::write(const uint8_t* buffer, unsigned count)
{
    if (!m_file || m_readOnly) return 0;
    int n = (int)fwrite(buffer, 1, count, m_file);
    fflush(m_file); // sector writes are rare; keep images consistent
    return n;
}

uint64_t FileDisk::seek(uint64_t offset)
{
    if (!m_file) return 0;
    fseek(m_file, (long)offset, SEEK_SET);
    return offset;
}

// ---------------------------------------------------------------------------
// TDeckFrameBuffer
// ---------------------------------------------------------------------------
#define PANEL_W 320
#define PANEL_H 240

// If colors come out wrong on hardware, flip this to 0 (host wire format is
// byte-swapped RGB565; Video.cpp's palette tables may already match).
#define BLIT_BYTESWAP 1

static uint16_t s_blit[2][PANEL_W * PANEL_H];
static int      s_blit_idx = 0;
static uint16_t s_sx_map[PANEL_W];
static uint16_t s_sy_map[PANEL_H];

// Every ~256 frames: does the source frame contain lit pixels at all?
// Separates "emulator renders black" from "blit path broken".
__attribute__((noinline))
static void probe_frame(const uint16_t* pixels, int w, int h, int stride)
{
    static uint32_t s_probe = 0;
    if ((s_probe++ & 255) != 0) return;
    int lit = 0;
    for (int y = 0; y < h; y += 8) {
        const uint16_t* row = &pixels[y * stride];
        for (int x = 0; x < w; x += 8)
            lit += (row[x] != 0);
    }
    printf("[pcxt] probe %dx%d lit=%d\n", w, h, lit);
}

void TDeckFrameBuffer::init(uint32_t desiredWidth, uint32_t desiredHeight)
{
    // 8bpp scratch surface some legacy render paths write into
    if (!m_surface)
        m_surface = RenderSurface::create(desiredWidth, desiredHeight);
}

void TDeckFrameBuffer::resize(uint32_t desiredWidth, uint32_t desiredHeight)
{
    (void)desiredWidth; (void)desiredHeight;
    // Scale maps are rebuilt lazily from actual blit dimensions.
}

void TDeckFrameBuffer::rebuildScaleMaps(int srcW, int srcH)
{
    // Fill the whole 320x240 panel (stretch, integer NN maps). Aspect-fit
    // letterboxing wastes the scarce horizontal pixels — a killer for DOS text,
    // where 40-col mode is a tall 320x400 source that fit-scaling would shrink
    // to ~192px wide. Filling gives 40-col chars a full 8px width (readable);
    // the minor aspect distortion is the accepted trade on a screen this small.
    int outW = PANEL_W;
    int outH = PANEL_H;
    for (int x = 0; x < outW; x++)
        s_sx_map[x] = (uint16_t)((uint32_t)x * srcW / outW);
    for (int y = 0; y < outH; y++)
        s_sy_map[y] = (uint16_t)((uint32_t)y * srcH / outH);

    m_srcW = srcW; m_srcH = srcH;
    m_outW = outW; m_outH = outH;
    printf("[faux86] video mode blit %dx%d -> %dx%d\n", srcW, srcH, outW, outH);
}

void TDeckFrameBuffer::blit(uint16_t* pixels, int w, int h, int stride)
{
    if (w <= 0 || h <= 0) return;
    // Upstream's only call site (Video.cpp vga_renderThread) passes
    // VGA_FRAMEBUFFER_STRIDE, which on the 16bpp path is WIDTH * 2 — a BYTE
    // stride. The buffer's row pitch is VGA_FRAMEBUFFER_WIDTH pixels.
    stride /= (int)sizeof(uint16_t);
    if (stride < w) stride = w;
    if (w != m_srcW || h != m_srcH)
        rebuildScaleMaps(w, h);

    probe_frame(pixels, w, h, stride);

    // Frame-drop. Drawing is driven from inside the CPU emulation (Faux86's
    // Timing draw callback, ~every 62ms of emulated time). The panel shares one
    // SPI bus with the LoRa radio + SD, so a push can take 75ms+ under RX load —
    // longer than the draw interval. Blitting on every callback lets the push
    // outrun its interval and pile up inside a single exec86() call, stalling
    // the emulator (measured sim=2800ms/loop, CPU frozen). Cap real pushes to
    // ~11fps; excess draws become no-ops. This also keeps the (blocking)
    // host_blit_frame_async from ever waiting: the prior push has long finished
    // by the time the next one is issued.
    static uint32_t s_last_blit_us = 0;
    uint32_t now_us = host_get_ticks_us();
    if (s_last_blit_us && (uint32_t)(now_us - s_last_blit_us) < 90000) return;
    s_last_blit_us = now_us;

    uint16_t* fb = s_blit[s_blit_idx];
    // stride is in pixels (upstream passes the row length of its buffer)
    int prev_sy = -1;
    const uint16_t* prev_row_dst = nullptr;
    for (int oy = 0; oy < m_outH; oy++) {
        uint16_t* dst = &fb[oy * m_outW];
        int sy = s_sy_map[oy];
        if (sy == prev_sy) {
            memcpy(dst, prev_row_dst, m_outW * sizeof(uint16_t));
        } else {
            const uint16_t* src = &pixels[sy * stride];
            if (m_outW == w) {
#if BLIT_BYTESWAP
                for (int ox = 0; ox < m_outW; ox++) {
                    uint16_t c = src[ox];
                    dst[ox] = (uint16_t)((c >> 8) | (c << 8));
                }
#else
                memcpy(dst, src, m_outW * sizeof(uint16_t));
#endif
            } else {
                for (int ox = 0; ox < m_outW; ox++) {
                    uint16_t c = src[s_sx_map[ox]];
#if BLIT_BYTESWAP
                    c = (uint16_t)((c >> 8) | (c << 8));
#endif
                    dst[ox] = c;
                }
            }
            prev_sy = sy;
        }
        prev_row_dst = dst;
    }

    host_blit_frame_async(fb, m_outW, m_outH);
    s_blit_idx ^= 1;
}

// ---------------------------------------------------------------------------
// TDeckTimer — 64-bit microsecond clock over the host's 32-bit one
// ---------------------------------------------------------------------------
uint64_t TDeckTimer::getTicks()
{
    uint32_t now = host_get_ticks_us();
    if (now < m_lastLow)
        m_high++; // 32-bit wrap (~71 minutes)
    m_lastLow = now;
    return ((uint64_t)m_high << 32) | now;
}

// ---------------------------------------------------------------------------
// TDeckAudio — drain Faux86's 8-bit unsigned mono ring, push as S16
// ---------------------------------------------------------------------------
#define AUDIO_CHUNK 1024

void TDeckAudio::init(VM& vm)
{
    m_vm = &vm;
    printf("[faux86] audio bridge: %d Hz\n", (int)vm.config.audio.sampleRate);
}

void TDeckAudio::shutdown()
{
    m_vm = nullptr;
}

void TDeckAudio::pump()
{
    if (!m_vm || !m_vm->config.enableAudio) return;

    static uint8_t u8buf[AUDIO_CHUNK];
    static int16_t s16buf[AUDIO_CHUNK];

    while (m_vm->audio.isAudioBufferFilled()) {
        m_vm->audio.fillAudioBuffer(u8buf, AUDIO_CHUNK);
        for (int i = 0; i < AUDIO_CHUNK; i++)
            s16buf[i] = (int16_t)((int)u8buf[i] - 128) << 8;
        host_audio_push(s16buf, AUDIO_CHUNK, (int)m_vm->config.audio.sampleRate);
    }
}

// ---------------------------------------------------------------------------
// TDeckHostInterface
// ---------------------------------------------------------------------------
DiskInterface* TDeckHostInterface::openFile(const char* filename)
{
    FileDisk* disk = new FileDisk(filename);
    if (!disk->isValid()) {
        delete disk;
        return nullptr;
    }
    return disk;
}

} // namespace Faux86

// ---------------------------------------------------------------------------
// Real-time timing kick, called from the module main loop every iteration.
// exec86 only runs vm.timing.tick() when (totalexec & timing_interval) == 0,
// and totalexec FREEZES while the CPU is halted — so a HLT that lands on the
// wrong count starves the PIT and the IRQ0 that would wake it never fires
// (BIOS POST deadlock). Upstream avoids this with a separate timing thread;
// in our single-threaded build the loop drives it instead. All TimingScheduler
// state is elapsed-time based, so extra calls are harmless.
// ---------------------------------------------------------------------------
extern "C" __attribute__((noinline)) void pcxt_timing_kick(Faux86::VM* vm)
{
    // bSkipDraw=true: keep the PIT/IRQ0 alive (needed so a halted CPU wakes)
    // but do NOT let the kick trigger the expensive screen render. All drawing
    // is left to exec86's internal timer tick, rate-limited via frameDelay, so
    // the render never fires more than once per frame interval.
    vm->timing.tick(true);
}

// ---------------------------------------------------------------------------
// Bring-up diagnostic, called from the module main loop every ~8ms: prints
// CS:IP (F000=BIOS, C000=video ROM, 07C0=boot sector) and the executed-
// instruction delta every ~3s. noinline + separate TU: inline code growth
// re-trips the BFD elf32-xtensa link assertion.
// ---------------------------------------------------------------------------
// Main-loop phase timer: prints which phase (simulate / timing kick / input /
// audio) ate the wall-clock time when any exceeds 150ms. Rate-limited to 1/s.
extern "C" __attribute__((noinline)) void pcxt_phase(uint32_t sim, uint32_t kick, uint32_t poll, uint32_t pump)
{
    if (sim < 150000 && kick < 150000 && poll < 150000 && pump < 150000) return;
    static uint32_t s_last = 0;
    uint32_t now = host_get_ticks_us();
    if (s_last && (uint32_t)(now - s_last) < 1000000) return;
    s_last = now ? now : 1;
    printf("[pcxt] phase sim=%ums kick=%ums poll=%ums pump=%ums\n",
           sim / 1000, kick / 1000, poll / 1000, pump / 1000);
}

extern "C" __attribute__((noinline)) void pcxt_cpu_status(Faux86::VM* vm, uint32_t now_us, uint32_t loops)
{
    static uint32_t s_last_us = 0;
    static uint32_t s_last_exec = 0;
    static uint32_t s_last_loops = 0;
    uint32_t exec_now = (uint32_t)vm->cpu.totalexec;
    if (s_last_us != 0 && (uint32_t)(now_us - s_last_us) < 3000000) return;
    if (s_last_us != 0) {
        uint32_t dt_ms = (uint32_t)(now_us - s_last_us) / 1000;
        uint32_t kips = dt_ms ? (exec_now - s_last_exec) / dt_ms : 0;
        uint16_t cs = vm->cpu.segregs[1] /* CS */;
        uint16_t ip = vm->cpu.ip;
        uint32_t phys = ((uint32_t)cs << 4) + ip;
        // hltstate/ifl are private; infer halt from the byte before IP (HLT
        // leaves IP just past the F4) and dump opcode context at IP.
        uint8_t opm1 = vm->memory.readByte(phys - 1);
        uint8_t op0  = vm->memory.readByte(phys);
        uint8_t op1  = vm->memory.readByte(phys + 1);
        uint8_t op2  = vm->memory.readByte(phys + 2);
        printf("[pcxt] cpu %04X:%04X [%02X]%02X %02X %02X %s ~%uk/s "
               "dloops=%u boot=%d\n",
               cs, ip, opm1, op0, op1, op2,
               (opm1 == 0xF4) ? "HALTED" : "run",
               kips, loops - s_last_loops, (int)vm->cpu.didbootstrap);
    }
    s_last_us = now_us ? now_us : 1;
    s_last_exec = exec_now;
    s_last_loops = loops;
}
