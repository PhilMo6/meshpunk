// Entry point for the jet3d ELF module on T-Deck: a Jet software rasteriser
// driven by a private Lua interpreter.
//
// argv: -game <path/to/main.lua>  [-band <rows>] [-fps <cap>]
//
// Rendering: the module owns no full-screen framebuffer. It rasterises one
// horizontal band at a time into internal SRAM and pushes each finished band
// with host_blit_rect(), so the rasteriser's pixel writes never touch PSRAM.
// Scene::prepareFrame() runs the transform and depth sort once per frame; only
// the render-queue walk in Scene::rasterizeBand() repeats per band.

#include "Jet.hpp"
#include "engine/jet_lua.h"
#include "engine/jet_overlay.h"
#include "engine/jet_audio.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

using namespace Renderer;

// ---------------------------------------------------------------------------
// Host imports
// ---------------------------------------------------------------------------
extern "C" {
    void     host_clear_screen(void);
    void     host_blit_rect(const uint16_t* rgb565, int x, int y, int w, int h);
    void     host_blit_rect_async(const uint16_t* rgb565, int x, int y, int w, int h);
    void     host_blit_wait(void);
    void*    host_spawn_task(void (*fn)(void*), void* arg, int core,
                             int prio, int stackkb);
    int      host_task_join(void* task, int timeout_ms);
    uint32_t host_get_ticks_ms(void);
    uint32_t host_get_ticks_us(void);
    void     host_sleep_ms(uint32_t ms);
    int      host_get_key(int* pressed, unsigned char* key);
    void     host_trackball_read(int* dx, int* dy, int* click);
    int      host_should_exit(void);
    void     host_log(const char* msg);
    void*    host_malloc_internal(size_t size);
    uint32_t host_psram_largest_free(void);
}

static const int SCREEN_W = 320;
static const int SCREEN_H = 240;

// All module output goes through host_log(), which routes to SerialMux and
// takes SLOG_LOCK. The exported raw stdio (printf/puts/fprintf) writes to the
// UART WITHOUT that lock, so a module using it races the firmware tasks that do
// take it — including the mesh task logging RX on Core 1 while this module runs
// on Core 0. Do not reintroduce printf here.
static void mlog(const char* fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // host_log's SerialMux call appends the newline, so drop a trailing one
    // rather than emitting a blank line per entry.
    if (n > 0) {
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        if (buf[n - 1] == '\n') buf[n - 1] = '\0';
    }
    host_log(buf);
}

// ---------------------------------------------------------------------------
// exit() trap (abort() lives in cxxstubs.cpp and routes here)
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

extern "C" void exit(int code)
{
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

// ---------------------------------------------------------------------------
// C++ static constructors. The ELF loader does not run them; walk the tables
// manually.
//
// Two tables are handled because this Xtensa toolchain is configured without
// --enable-initfini-array: it emits constructors into .ctors, and the linker
// then defines no __init_array_start/end for them. Those weak symbols resolve
// to NULL through the loader, so walking only __init_array_* would skip every
// constructor in silence. ctors_begin.cpp / ctors_end.cpp bracket .ctors by
// link order; the __init_array_* pair stays weak so a toolchain that emits
// .init_array instead still works.
//
// Ordering differs: .init_array runs front to back, .ctors back to front (the
// linker emits it in reverse). Both marker slots hold NULL and are skipped.
// ---------------------------------------------------------------------------
typedef void (*init_fn_t)(void);
extern init_fn_t __init_array_start[] __attribute__((weak));
extern init_fn_t __init_array_end[]   __attribute__((weak));
extern init_fn_t __meshpunk_ctors_begin;
extern init_fn_t __meshpunk_ctors_end_marker;

static void run_static_ctors(void)
{
    int n = 0;

    if (__init_array_start && __init_array_end) {
        for (init_fn_t* f = __init_array_start; f < __init_array_end; f++) {
            if (*f) { (*f)(); n++; }
        }
    }

    init_fn_t* lo = &__meshpunk_ctors_begin + 1;
    init_fn_t* hi = &__meshpunk_ctors_end_marker;
    for (init_fn_t* f = hi; f-- > lo; ) {
        if (*f) { (*f)(); n++; }
    }

    mlog("[jet3d] ran %d static constructors\n", n);
}

// ---------------------------------------------------------------------------
// Band buffer
// ---------------------------------------------------------------------------
// Jet's ESP32-S3 clear and span fills use 128-bit EE.VST.128.XP stores, which
// require a 16-byte aligned destination. Each band row starts at
// base + row * SCREEN_W * 2 = base + row * 640, and 640 is a multiple of 16,
// so aligning the base aligns every row.
static uint16_t* s_bandRaw[4] = { nullptr, nullptr, nullptr, nullptr };
static uint16_t* s_bandBuf[4] = { nullptr, nullptr, nullptr, nullptr };
static int       s_bandRows   = 0;
static bool      s_bandInPsram = false;

// Double buffering. With two bands the finished one is handed to
// host_blit_rect_async and the next is rasterised into the other while the
// first goes out on the wire. The push is CPU-driven — there is no DMA on this
// bus — but it spends nearly all of its time spinning on the SPI busy flag, so
// running it on Core 1 costs this core's rasteriser very little and hides most
// of a fixed ~21ms panel transfer behind work we were doing anyway.
//
// host_blit_rect_async returns once the PREVIOUS push has completed, which is
// exactly when the buffer handed over before that becomes safe to reuse — so
// two buffers are sufficient provided they alternate.
static bool s_dbufWanted = false;
static int  s_bandBufs   = 1;   // buffers actually allocated: 1 = synchronous
static int  s_bandCur    = 0;   // buffer the next band rasterises into

// Band interlacing: draw every other band and alternate which set each frame,
// so a whole frame costs half the clears, band walks, byte swaps and panel
// pushes. The skipped rows are not reconstructed — the panel keeps the pixels
// it already holds, so nothing has to be retained on this side.
//
// This is NOT Jet's interlacedMode. That one draws every other ROW into a
// full-stride buffer and needs the buffer to persist between frames so the
// other field survives; one band buffer reused across every band cannot do
// that, so the off-parity rows would show the clear and comb against the sky.
// Interlacing whole bands has no such requirement because each band that is
// drawn is drawn completely.
//
// Cost: a band refreshes at half the frame rate, which reads as horizontal
// banding while the view moves quickly, and gets finer as the band shrinks.
// prepareFrame() is per-FRAME work (transform, cull, depth sort) and does not
// halve — only the per-band work does.
static bool s_interlace = false;
static int  s_field     = 0;      // which set of bands the next frame draws

// Checkerboard: the rasteriser draws only one (x^y) parity of pixels and the
// parity alternates each frame, then reconstructCheckerboard() rebuilds the
// skipped pixels from their two horizontal neighbours before the band is
// blitted. Halves the per-pixel loop work -- the dominant cost in the frame --
// without halving the fill work, because flat spans still fill their whole run
// (same colour either way, and a strided fill would give up the 128-bit
// stores).
//
// Unlike band interlacing this needs nothing to survive between frames, so
// there is no band drag: every band is complete and current every frame. The
// cost is horizontal detail, which is why it can be run alongside interlacing
// or instead of it.
static bool s_checker = false;

// Divisors of SCREEN_H, descending. A band must divide the panel height
// exactly so the render loop covers it without a partial final strip.
static const int kBandRows[] = { 240, 120, 80, 60, 48, 40, 30, 24, 20, 16, 15,
                                 12, 10, 8, 6, 5, 4, 3, 2, 1 };

// Take `n` buffers of `r` rows, all or nothing: a partial success would leave
// the double-buffered path with one buffer and hand the blit task a pointer
// the rasteriser is still writing.
static bool try_alloc_bands(int r, int n, bool internal)
{
    const size_t bytes = (size_t)SCREEN_W * (size_t)r * 2u + 16u;
    for (int i = 0; i < n; i++) {
        void* p = internal ? host_malloc_internal(bytes) : malloc(bytes);
        if (!p) {
            for (int j = 0; j < i; j++) {
                free(s_bandRaw[j]);
                s_bandRaw[j] = nullptr;
                s_bandBuf[j] = nullptr;
            }
            return false;
        }
        s_bandRaw[i] = (uint16_t*)p;
        s_bandBuf[i] = (uint16_t*)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
    }
    return true;
}

static bool alloc_band(int rows, int bufs)
{
    // Internal SRAM is the scarce pool (shared with BLE/WiFi/TLS) and it
    // fragments, so a 25KB request can fail while plenty of internal RAM is
    // free overall. The rasteriser's pixel writes are the hot path and PSRAM
    // makes every one of them slow, so a SMALLER internal band beats a large
    // PSRAM one: halving the band only adds one more host_blit_rect per frame.
    // Step down through the divisors before giving up on internal RAM.
    //
    // Priority when two buffers were asked for and internal RAM is short:
    // ONE INTERNAL BUFFER BEATS TWO IN PSRAM. Double buffering hides the panel
    // transfer, but moving every rasteriser pixel write to PSRAM costs more
    // than the transfer it would hide.
    for (int n = bufs; n >= 1; n--) {
        for (size_t i = 0; i < sizeof(kBandRows) / sizeof(kBandRows[0]); ++i) {
            int r = kBandRows[i];
            if (r > rows) continue;
            if (try_alloc_bands(r, n, true)) {
                s_bandRows    = r;
                s_bandBufs    = n;
                s_bandInPsram = false;
                if (r != rows || n != bufs) {
                    mlog("[jet3d] internal SRAM could not fit %d x %d rows;"
                           " using %d x %d\n", bufs, rows, n, r);
                }
                return true;
            }
        }
    }

    // No internal band of any size: fall back to PSRAM at the requested size,
    // single-buffered. Still renders, just with the pixel writes going to slow
    // memory — and with nothing to gain from overlapping the push.
    if (try_alloc_bands(rows, 1, false)) {
        s_bandRows    = rows;
        s_bandBufs    = 1;
        s_bandInPsram = true;
        return true;
    }
    return false;
}

static void free_band(void)
{
    // A push handed to host_blit_rect_async may still be reading one of these.
    // The host's own drain runs only after this module has returned, which is
    // far too late for memory the module frees itself.
    host_blit_wait();
    for (int i = 0; i < 4; i++) {
        if (s_bandRaw[i]) { free(s_bandRaw[i]); s_bandRaw[i] = nullptr; }
        s_bandBuf[i] = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
// The framebuffer pointer handed to the Scene for each band is a virtual base:
// bandBuf - y0 * SCREEN_W, so a write to absolute row y lands at
// bandBuf[(y - y0) * SCREEN_W]. Jet documents this usage on prepareFrame().
// Diagnostic: capture one finished scanline straight out of the band buffer,
// before it is blitted, so the shading ramp can be read as numbers instead of
// judged by eye. -1 = inactive. Armed from Lua via jet.dumprow(y).
static int s_dumpRow = -1;

void jet_request_row_dump(int y)
{
    if (y >= 0 && y < SCREEN_H) s_dumpRow = y;
}

// Prints the row as "x:RRGGBB" runs, collapsing repeats, so a long line fits
// the log and a non-monotonic ramp is obvious.
static void dump_row(const uint16_t* row)
{
    char buf[220];
    int used = 0;
    uint16_t prev = row[0];
    int runStart = 0;
    mlog("[jet3d] row %d dump (RGB565 -> r,g,b):\n", s_dumpRow);
    for (int x = 1; x <= SCREEN_W; x++) {
        if (x < SCREEN_W && row[x] == prev) continue;
        int r = (prev >> 11) & 0x1F, g = (prev >> 5) & 0x3F, b = prev & 0x1F;
        int n = snprintf(buf + used, sizeof(buf) - used,
                         "%d-%d:%d,%d,%d  ", runStart, x - 1, r, g, b);
        if (n < 0 || used + n >= (int)sizeof(buf) - 1) {
            buf[used] = '\0';
            mlog("  %s\n", buf);
            used = 0;
            n = snprintf(buf, sizeof(buf), "%d-%d:%d,%d,%d  ",
                         runStart, x - 1, r, g, b);
        }
        used += n;
        if (x < SCREEN_W) { prev = row[x]; runStart = x; }
    }
    if (used > 0) { buf[used] = '\0'; mlog("  %s\n", buf); }
    s_dumpRow = -1;
}

// Per-phase frame timing, accumulated in microseconds and reported alongside
// the frame rate. Which phase dominates decides the next optimisation: a
// blit-bound frame wants the transfer overlapped with rasterising (or fewer
// pixels pushed), a raster-bound one wants the span loops in .iram.text.
static uint32_t s_tPrepare = 0, s_tRaster = 0, s_tBlit = 0, s_tLua = 0;
// Prepare-phase breakdown (parallel path only): Begin / core-0 slice /
// core-0 wait-for-worker / worker slice (its own clock) / End(sort+bins).
static uint32_t s_tPrepBegin = 0, s_tPrepS0 = 0, s_tPrepWait = 0,
                s_tPrepS1 = 0, s_tPrepEnd = 0;
static volatile uint32_t s_wSliceUs = 0;   // worker's last slice duration

// Frames folded into those four sums since the last fps-window reset. They are
// ACCUMULATORS, not per-frame values, so anything reporting them outside the
// window log has to divide — the first screenshots printed the raw sums and
// they read as absurdly slow frames.
static uint32_t s_tFrames = 0;

// host_blit_rect pushes with TFT_eSPI's swap argument false, so the bytes reach
// the panel exactly as the module wrote them. The panel takes RGB565
// big-endian; Jet composes pixels as native little-endian uint16, so every
// pixel has to be swapped on the way out. Other modules meet the same contract
// by construction (gnuboy renders pre-swapped, NGPC pre-swaps its palette
// table), but Jet computes colour arithmetically per pixel, so there is no
// table to pre-swap and the band gets a pass instead.
//
// Two pixels per 32-bit word. Doing it as a pass rather than at each pixel
// store is deliberate: the clear alone writes every pixel in the band, so a
// per-store swap costs at least twice the operations and would need patching
// into a dozen sites inside drawTriangle.
//
// The band base is 16-byte aligned and the word count is 160 * rows, so it is
// always a multiple of four; the unroll keeps four independent chains in flight
// instead of stalling on each load-modify-store. The tail loop is kept for
// safety if the band geometry ever changes.
static inline void swap_band_bytes(uint16_t* p, int pixels)
{
    uint32_t* w = (uint32_t*)p;
    int i = 0;
    const int words = pixels >> 1;
    const int quads = words & ~3;

    for (; i < quads; i += 4) {
        const uint32_t a = w[i], b = w[i + 1], c = w[i + 2], d = w[i + 3];
        w[i]     = ((a & 0x00FF00FFu) << 8) | ((a & 0xFF00FF00u) >> 8);
        w[i + 1] = ((b & 0x00FF00FFu) << 8) | ((b & 0xFF00FF00u) >> 8);
        w[i + 2] = ((c & 0x00FF00FFu) << 8) | ((c & 0xFF00FF00u) >> 8);
        w[i + 3] = ((d & 0x00FF00FFu) << 8) | ((d & 0xFF00FF00u) >> 8);
    }
    for (; i < words; ++i) {
        const uint32_t v = w[i];
        w[i] = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    }
}

// ---------------------------------------------------------------------------
// Dual-core band rendering, SINGLE-SUBMITTER form. Core 0 (this task) and
// a Core-1 worker both pull band indices from a per-parity atomic counter
// and run Scene::renderBandTo into their own buffer pairs (core 0: 0/1,
// worker: 2/3) — but ONLY Core 0 byte-swaps and submits to
// host_blit_rect_async. The worker publishes composed bands through
// s_bandReadySlot and Core 0 drains them between its own bands, so exactly
// one task ever touches the blit queue (see compose_band/submit_band).
// The worker idles on a frame sequence number (the SNES module's proven
// mailbox pattern) and is joined before teardown.
// ---------------------------------------------------------------------------
static void*    s_worker     = nullptr;
static Scene*   s_workerScene = nullptr;
static volatile uint32_t s_frameSeq = 0;
static volatile uint32_t s_prepSeq  = 0;   // phase 1: prepare slice
static volatile uint32_t s_prepDone = 0;   // = s_prepSeq when worker's slice done
static volatile int s_workerQuit = 0;
// MESHPUNK straggler fix (text-warp root cause, 2026-08-05): claim and
// done state are PER FRAME PARITY, and every claim re-checks s_frameSeq.
// band_who instrumentation proved one worker render per frame executed
// under a stale claim loop (outside its frame's capture window, blitting
// stale-queue output — the band-quantised text warp). A stale loop now
// fails the seq check before claiming, and even in the publish race it
// can only touch its OWN parity's exhausted counter — never the new
// frame's slots.
static volatile int s_bandNextP[2]  = { 0, 0 };
static volatile int s_frameCount = 0;   // bands this frame
static volatile int s_frameFirst = 0;   // first band index (interlace field)
static volatile int s_frameStep  = 1;

// SINGLE-SUBMITTER PIPELINE. Under two concurrent submitters the
// post-composition path could blit a stale band (band-quantised text
// corruption), so the worker COMPOSES ONLY: it renders bands into its two
// buffers and publishes them; Core 0 performs every byte-swap and every
// blit submission, in order, as the sole submitter.
//
// Worker-buffer lifetime: host_blit_rect_async(X) returns once the push
// BEFORE X completed, so after every submit that returns, the buffer
// submitted before it is free. Core 0 tracks that one buffer and clears
// the worker's busy flag when it was a worker buffer.
static volatile uint8_t s_bandReadySlot[64];   // 0 = none, else buf idx+1
static volatile uint8_t s_wBufBusy[2] = { 0, 0 };

// Compose one band into `buf` — everything EXCEPT swap+submit.
static void compose_band(Scene& scene, int b, uint16_t* buf)
{
    const int y0 = b * s_bandRows;
    const int y1 = y0 + s_bandRows;
    scene.renderBandTo(buf, y0, y1);
    jet_ovl_draw_band(buf, SCREEN_W, y0, y1);
    if (s_dumpRow >= y0 && s_dumpRow < y1)
        dump_row(buf + (size_t)(s_dumpRow - y0) * SCREEN_W);
}

// Core 0 only: submit a display-ready band, then release the buffer whose
// push the returning submit proved complete. wslot = worker buffer index
// (0/1) when submitting a worker band, -1 for Core 0's own. doSwap: Core
// 0's own bands are swapped here; WORKER bands arrive pre-swapped — the
// worker does every bit of render work (compose + byte-swap) before
// publishing, so the only thing this core ever does to a worker band is
// the submit call itself (the single-submitter rule).
static int s_prevSubmitWSlot = -1;
static void submit_band(int b, uint16_t* buf, int wslot, bool doSwap)
{
    const int y0 = b * s_bandRows;
    if (doSwap) swap_band_bytes(buf, SCREEN_W * s_bandRows);
    host_blit_rect_async(buf, 0, y0, SCREEN_W, s_bandRows);
    if (s_prevSubmitWSlot >= 0)
        __atomic_store_n(&s_wBufBusy[s_prevSubmitWSlot], 0, __ATOMIC_RELEASE);
    s_prevSubmitWSlot = wslot;
}

// WORKER claim loop: compose into its two buffers and publish; no swap,
// no submit. Keeps the straggler defences (per-parity counter + a seq
// re-check before every claim).
static void worker_pull_bands(Scene& scene, uint32_t seqTag)
{
    int flip = 0;
    const int par = (int)(seqTag & 1u);
    for (;;) {
        if (__atomic_load_n(&s_frameSeq, __ATOMIC_ACQUIRE) != seqTag) return;
        // Core 0 preparing the NEXT frame (prepSeq moved past this frame)
        // also ends this loop: frame seqTag's bands are already complete by
        // then, and the worker's prep slice is now the urgent work. Without
        // this exit the busy-wait below livelocked: its release comes from
        // Core 0 SUBMITTING, but Core 0 was in the next frame's 250ms
        // prepare wait — each core waiting on the other, 3.4 fps forever
        // (hw 2026-08-05, entered via a GPS-burst starvation on core 1).
        if (__atomic_load_n(&s_prepSeq, __ATOMIC_ACQUIRE) != seqTag) return;
        // A buffer is reusable once Core 0's post-submit release cleared it.
        while (__atomic_load_n(&s_wBufBusy[flip], __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&s_frameSeq, __ATOMIC_ACQUIRE) != seqTag)
                return;
            if (__atomic_load_n(&s_prepSeq, __ATOMIC_ACQUIRE) != seqTag)
                return;
            if (s_workerQuit) return;
            host_sleep_ms(1);
        }
        const int n = __atomic_fetch_add((int*)&s_bandNextP[par], 1,
                                         __ATOMIC_RELAXED);
        if (n >= s_frameCount) return;
        const int b = s_frameFirst + n * s_frameStep;
        compose_band(scene, b, s_bandBuf[2 + flip]);
        // Pre-swap here so the published band is fully display-ready and
        // Core 0 never spends its loop on worker pixels.
        swap_band_bytes(s_bandBuf[2 + flip], SCREEN_W * s_bandRows);
        __atomic_store_n(&s_wBufBusy[flip], 1, __ATOMIC_RELAXED);
        // Publish AFTER the busy flag: Core 0 acquires the slot, so it sees
        // both the composed pixels and the busy state.
        if (b >= 0 && b < 64)
            __atomic_store_n(&s_bandReadySlot[b], (uint8_t)(3 + flip),
                             __ATOMIC_RELEASE);
        flip ^= 1;
    }
}

// Core 0: submit any worker bands published so far. Returns bands drained.
static int drain_worker_bands(void)
{
    int drained = 0;
    const int nb = s_frameCount;
    for (int i = 0; i < nb; ++i) {
        const int b = s_frameFirst + i * s_frameStep;
        if (b < 0 || b >= 64) continue;
        const uint8_t slot = __atomic_load_n(&s_bandReadySlot[b],
                                             __ATOMIC_ACQUIRE);
        if (!slot) continue;
        submit_band(b, s_bandBuf[slot - 1], (int)(slot - 3), false);
        __atomic_store_n(&s_bandReadySlot[b], 0, __ATOMIC_RELAXED);
        ++drained;
    }
    return drained;
}

static void band_worker(void*)
{
    uint32_t lastPrep = 0, lastBand = 0;
    while (!s_workerQuit) {
        bool worked = false;

        // Phase 1: the prepare slice (odd-indexed objects into the W emit
        // buffers). After finishing, spin briefly for the band publish —
        // it follows within Core 0's sort, a couple of ms at most, and the
        // 1ms sleep quantum would waste a visible slice of a 40ms frame.
        const uint32_t ps = __atomic_load_n(&s_prepSeq, __ATOMIC_ACQUIRE);
        if (ps != lastPrep && s_workerScene) {
            lastPrep = ps;
            const uint32_t st0 = host_get_ticks_us();
            s_workerScene->prepareObjectSlice(1, 2);
            // Written before the prepDone release-store, so Core 0's
            // acquire on prepDone also acquires this value.
            s_wSliceUs = host_get_ticks_us() - st0;
            __atomic_store_n(&s_prepDone, ps, __ATOMIC_RELEASE);
            for (volatile int spin = 0; spin < 200000; ++spin) {
                if (__atomic_load_n(&s_frameSeq, __ATOMIC_ACQUIRE)
                    != lastBand) break;
            }
            worked = true;
        }

        // Phase 2: band rastering.
        const uint32_t bs = __atomic_load_n(&s_frameSeq, __ATOMIC_ACQUIRE);
        if (bs != lastBand && s_workerScene) {
            lastBand = bs;
            worker_pull_bands(*s_workerScene, bs);
            worked = true;
        }

        if (!worked) host_sleep_ms(1);
    }
}

// Must run before free_band() and before the Scene leaves scope: a live
// worker would otherwise raster into freed buffers / a dead scene.
static void stop_worker(void)
{
    if (!s_worker) return;
    s_workerQuit = 1;
    host_task_join(s_worker, 2000);
    s_worker = nullptr;
    s_workerScene = nullptr;
}

static void render_frame_parallel(Scene& scene)
{
    const int nBands = SCREEN_H / s_bandRows;
    const bool il    = s_interlace && nBands >= 2;

    // Prepare, split across both cores: Begin (single-threaded setup), then
    // Core 0 takes the even objects while the worker takes the odd ones into
    // its own emit buffers, then End merges tallies and sorts both. The
    // wait-spin is short — the slices are similar sizes by construction.
    uint32_t t0 = host_get_ticks_us();
    scene.prepareFrameBegin();
    const uint32_t tb = host_get_ticks_us();
    s_tPrepBegin += tb - t0;
    // Re-arm the worker's slice AFTER Begin's queue clears and just before
    // the publish: a stale slice waking during Begin still sees the abort.
    scene.prepAbort = false;
    __atomic_add_fetch((uint32_t*)&s_prepSeq, 1, __ATOMIC_RELEASE);
    scene.prepareObjectSlice(0, 2);
    const uint32_t ts0 = host_get_ticks_us();
    s_tPrepS0 += ts0 - tb;
    {
        // Bounded in practice by the worker's near-equal slice. The timeout
        // is a dead-worker escape: drawing a frame without the odd-indexed
        // objects (and logging) beats spinning into the watchdog. Do NOT
        // run the worker's slice here on timeout — if it were merely
        // starved and woke later, two cores would emit into one vector.
        const uint32_t w0 = host_get_ticks_us();
        while (__atomic_load_n(&s_prepDone, __ATOMIC_ACQUIRE)
               != __atomic_load_n(&s_prepSeq, __ATOMIC_RELAXED)) {
            if (host_get_ticks_us() - w0 > 250000u) {
                mlog("[jet3d] prepare worker timeout\n");
                // Tell a mid-slice worker to stop emitting, then grant a
                // short grace to acknowledge — otherwise its late emissions
                // race the NEXT frame's Begin clearing the same queue
                // (observed on hw as slice1 tri counts with prepDrawn 0).
                // The flag stays set until just before the next prepSeq
                // publish, so a worker waking late aborts a STALE slice
                // instantly instead of running it against cleared queues.
                scene.prepAbort = true;
                const uint32_t g0 = host_get_ticks_us();
                while (__atomic_load_n(&s_prepDone, __ATOMIC_ACQUIRE)
                       != __atomic_load_n(&s_prepSeq, __ATOMIC_RELAXED)) {
                    if (host_get_ticks_us() - g0 > 50000u) break;
                }
                break;
            }
        }
    }
    const uint32_t tw = host_get_ticks_us();
    s_tPrepWait += tw - ts0;
    s_tPrepS1 += s_wSliceUs;
    scene.prepareFrameEnd();
    const uint32_t te = host_get_ticks_us();
    s_tPrepEnd += te - tw;
    s_tPrepare += te - t0;

    for (int i = 0; i < 64; ++i) s_bandReadySlot[i] = 0;
    s_frameFirst = il ? (s_field & 1) : 0;
    s_frameStep  = il ? 2 : 1;
    s_frameCount = il ? ((nBands - s_frameFirst + 1) / 2) : nBands;
    // Straggler-safe publish: the NEW frame's parity claim slot is reset
    // before the sequence goes live; a stale worker loop fails its seq
    // re-check before claiming and can only ever address its OWN parity's
    // exhausted counter in the publish race.
    const uint32_t newSeq =
        __atomic_load_n(&s_frameSeq, __ATOMIC_RELAXED) + 1u;
    const int par = (int)(newSeq & 1u);
    __atomic_store_n((int*)&s_bandNextP[par], 0, __ATOMIC_RELAXED);
    __atomic_store_n((uint32_t*)&s_frameSeq, newSeq, __ATOMIC_RELEASE);

    t0 = host_get_ticks_us();
    // SINGLE SUBMITTER: Core 0 claims and composes its own bands (into
    // buffers 0/1), submits them inline, and drains the worker's published
    // bands between claims. The frame is complete when every band has been
    // SUBMITTED (own + drained) — the worker never touches the blit path.
    {
        int submitted = 0;
        int flip = 0;
        for (;;) {
            submitted += drain_worker_bands();
            const int n = __atomic_fetch_add((int*)&s_bandNextP[par], 1,
                                             __ATOMIC_RELAXED);
            if (n >= s_frameCount) break;
            const int b = s_frameFirst + n * s_frameStep;
            uint16_t* buf = s_bandBuf[flip];
            compose_band(scene, b, buf);
            submit_band(b, buf, -1, true);
            flip ^= 1;
            ++submitted;
        }
        // Claims exhausted; drain the worker's remaining bands as they land.
        while (submitted < s_frameCount) {
            const int d = drain_worker_bands();
            if (d) { submitted += d; continue; }
            host_sleep_ms(1);
        }
    }
    s_tRaster += host_get_ticks_us() - t0;

    if (il) s_field ^= 1;
    scene.advanceFrameCounter();
}

static void render_frame(Scene& scene)
{
    if (s_worker) {
        render_frame_parallel(scene);
        goto frame_tail;
    }
    {
    // alloc_band only ever returns a divisor of SCREEN_H, so the panel is a
    // whole number of bands and the last band never straddles the bottom edge.
    const int nBands = SCREEN_H / s_bandRows;

    // Interlacing needs at least two bands to alternate between. With a single
    // full-height band the odd frames would select no band at all, which would
    // also skip prepareFrame() and leave the scene un-transformed.
    const bool il    = s_interlace && nBands >= 2;
    const int  step  = il ? 2 : 1;
    const int  first = il ? (s_field & 1) : 0;

    // prepareFrame() runs exactly once per frame and internally clears through
    // clearBuffers(), scoped to whichever band bounds clearBand() last set. It
    // therefore has to run on the first band this frame actually DRAWS, which
    // under interlacing is not necessarily band 0.
    bool prepared = false;

    for (int b = first; b < nBands; b += step) {
        const int y0 = b * s_bandRows;
        const int y1 = y0 + s_bandRows;
        uint16_t* const buf = s_bandBuf[s_bandCur];

        scene.setFramebuffer(buf - (size_t)y0 * SCREEN_W);

        // clearBand() sets the rasteriser's yBandMin/yBandMax, which also
        // scopes the clear prepareFrame() performs on the first band.
        uint32_t t0 = host_get_ticks_us();
        scene.clearBand(y0, y1);
        if (!prepared) {
            scene.prepareFrame();
            prepared = true;
            s_tPrepare += host_get_ticks_us() - t0;
            t0 = host_get_ticks_us();
        }

        scene.rasterizeBand(y0, y1);

        // Rebuild the pixels checkerboard skipped, before anything that writes
        // the framebuffer outside the rasteriser. drawSprites() and the overlay
        // blit directly, so they are not checkerboarded and running after them
        // would blur sprites and text. No-ops when the mode is off.
        scene.reconstructCheckerboard();

        // Screen-space passes, both clipped to this band. clearBand() above
        // left the rasteriser's yBandMin/yBandMax set to [y0, y1), which is
        // what drawSprites() clips against (rasterizeBand only sets them on its
        // own local copy). Overlay draws last so text sits on top of sprites.
        scene.drawSprites();
        jet_ovl_draw_band(buf, SCREEN_W, y0, y1);

        s_tRaster += host_get_ticks_us() - t0;

        // Dump before the swap so the log reports the colours as composed.
        if (s_dumpRow >= y0 && s_dumpRow < y1) {
            dump_row(buf + (size_t)(s_dumpRow - y0) * SCREEN_W);
        }

        // Double-buffered, this interval is the byte swap plus however much of
        // the PREVIOUS push has not finished yet — i.e. the part of the panel
        // transfer that rasterising failed to hide. It is the number worth
        // watching: it should fall toward the swap cost alone as raster grows.
        t0 = host_get_ticks_us();
        swap_band_bytes(buf, SCREEN_W * (y1 - y0));
        if (s_bandBufs > 1) {
            host_blit_rect_async(buf, 0, y0, SCREEN_W, y1 - y0);
            s_bandCur ^= 1;
        } else {
            host_blit_rect(buf, 0, y0, SCREEN_W, y1 - y0);
        }
        s_tBlit += host_get_ticks_us() - t0;
    }
    if (il) s_field ^= 1;

    // Scene::render() ends by incrementing frameCounter; the band loop replaces
    // render() and so has to do it too. prepareFrame() derives the checkerboard
    // parity from this counter, so without it the same parity would be drawn
    // every frame and the other half would never be anything but reconstructed.
    // ParticleSystem also reads it as (frameCounter - 1), which stays correct
    // because this sits at the same point in the frame render() used.
    scene.advanceFrameCounter();
    }

frame_tail:
    ++s_tFrames;
    scene.advanceFrameCounter();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void pump_input(void)
{
    int pressed = 0;
    unsigned char key = 0;
    while (host_get_key(&pressed, &key)) {
        jet_lua_set_key(key, pressed);
    }

    int dx = 0, dy = 0, click = 0;
    host_trackball_read(&dx, &dy, &click);
    if (dx || dy || click) jet_lua_add_trackball(dx, dy, click);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    const char* gamePath = nullptr;
    int bandRows = 5;   // matches the launcher default; dual-core wants
                        // many small bands (see the worker comments)
    int fpsCap   = 0;      // 0 = uncapped
    int fogOn    = 1;      // -fog 0 overrides the game's fog calls

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-game") && i + 1 < argc)      gamePath = argv[++i];
        else if (!strcmp(argv[i], "-band") && i + 1 < argc) bandRows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-fps") && i + 1 < argc)  fpsCap   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-interlace") && i + 1 < argc)
            s_interlace = atoi(argv[++i]) != 0;
        else if (!strcmp(argv[i], "-checkerboard") && i + 1 < argc)
            s_checker = atoi(argv[++i]) != 0;
        else if (!strcmp(argv[i], "-dbuf") && i + 1 < argc)
            s_dbufWanted = atoi(argv[++i]) != 0;
        else if (!strcmp(argv[i], "-fog") && i + 1 < argc)
            fogOn = atoi(argv[++i]) != 0;
    }

    if (!gamePath) {
        host_log("[jet3d] no -game <main.lua> argument");
        return 1;
    }

    // SCREEN_H must be a whole number of bands for the loop to cover the panel
    // without a partial final band straddling the bottom edge.
    if (bandRows < 1) bandRows = 1;
    if (bandRows > SCREEN_H) bandRows = SCREEN_H;
    while (SCREEN_H % bandRows) bandRows--;

    if (setjmp(s_exit_jmp)) {
        // exit()/abort() from anywhere below lands here.
        jet_lua_close();
        stop_worker();
        jet_audio_shutdown();
        free_band();
        return s_exit_code;
    }

    run_static_ctors();

    // Dual-core band rendering needs FOUR buffers (two per core). alloc_band
    // prefers keeping the buffer count and shrinking rows, so a small band
    // setting (5-10 rows) gets all four in internal SRAM easily. Fewer than
    // four falls back to the serial path with the classic 2/1 layout.
    if (!alloc_band(bandRows, 4)) {
        host_log("[jet3d] band buffer allocation failed");
        return 1;
    }
    if (s_bandBufs < 4 && s_bandBufs != (s_dbufWanted ? 2 : 1)) {
        // Odd count (e.g. 3): rebuild as the serial layout expects.
        free_band();
        if (!alloc_band(bandRows, s_dbufWanted ? 2 : 1)) {
            host_log("[jet3d] band buffer allocation failed");
            return 1;
        }
    }
    mlog("[jet3d] band %d rows x%d (%d bytes) in %s, %d bands, interlace %s,"
           " checkerboard %s, double buffer %s, psram free %u\n",
           s_bandRows, s_bandBufs, SCREEN_W * s_bandRows * 2 * s_bandBufs,
           s_bandInPsram ? "PSRAM" : "internal SRAM",
           SCREEN_H / s_bandRows,
           (s_interlace && SCREEN_H / s_bandRows >= 2) ? "on" : "off",
           s_checker ? "on" : "off",
           s_bandBufs > 1 ? "on" : "off",
           (unsigned)host_psram_largest_free());

    host_clear_screen();

    // No z-buffer: JetConfig.hpp sets Z_BUFFERING 0, so depth order comes from
    // the per-triangle sort instead.
    Scene scene(s_bandBuf[0], nullptr, SCREEN_W, SCREEN_H);

    // Checkerboard is a rasteriser flag, so it has to be set on the Rasterizer
    // the Scene built for itself rather than passed to the constructor.
    if (Rasterizer* rz = scene.getRenderer()) {
        rz->checkerboardMode = s_checker;
    }

    // Per-band triangle binning (Scene::bandBinEntries): prepareFrameEnd bins
    // the sorted queue by band so each band iterates only its own triangles
    // instead of walking the whole queue. Serial and dual-core paths both use
    // the same aligned band calls, so both benefit.
    scene.setBandRows(s_bandRows);

    // Spawn the Core-1 band worker (see band_worker). Requires the 4-buffer
    // layout; prepareFrame's internal clear is disabled because renderBandTo
    // clears each band itself — leaving it on would scribble through a stale
    // shared framebuffer pointer while bands render elsewhere.
    if (s_bandBufs == 4) {
        s_workerScene = &scene;
        static const int rungs[] = { 12, 8 };
        for (unsigned i = 0; i < sizeof(rungs) / sizeof(rungs[0]); ++i) {
            s_worker = host_spawn_task(band_worker, nullptr, 1, 1, rungs[i]);
            if (s_worker) break;
        }
        if (s_worker) {
            scene.setClearBuffer(false);
            mlog("[jet3d] dual-core band rendering ON (%d bands of %d rows)\n",
                 SCREEN_H / s_bandRows, s_bandRows);
        } else {
            host_log("[jet3d] worker spawn failed - serial rendering");
        }
    }

    // MESHPUNK: claim the render queue NOW, while the heap is fresh and its
    // largest free block is ~5.9 MB. Left to grow on demand it doubles, and a
    // 4096 -> 8192 step wants 1,088 KB contiguous while still holding the old
    // 544 KB — which is how a 256,000-unit world died on the first frame after
    // a rebuild, with only 1.7 MB of fragmented largest block left.
    //
    // 8,192 rather than 4,096: a tiled clipmap world holds 76 tiles at 120
    // triangles each = 9,120, and a look-straight-down view puts nearly all of
    // them in frustum at once. The cap is a safety net, not a budget — but a
    // net that engages during ordinary play leaves visible holes, and 4,096 did.
    // Claimed once up front, so the reservation itself can never fragment.
    {
        const size_t kQueue = 8192;
        scene.reserveRenderQueue(kQueue);
        mlog("[jet3d] render queue reserved: %u triangles, %u KB (%u B each)\n",
             (unsigned)kQueue,
             (unsigned)(kQueue * Scene::renderTriBytes() / 1024),
             (unsigned)Scene::renderTriBytes());
    }

    jet_audio_init();

    jet_lua_set_fog_enabled(fogOn);
    if (!fogOn) mlog("[jet3d] fog disabled by launcher\n");

    if (!jet_lua_open(&scene, SCREEN_W, SCREEN_H)) {
        free_band();
        return 1;
    }

    mlog("[jet3d] loading %s\n", gamePath);
    if (!jet_lua_run_file(gamePath)) {
        jet_lua_close();
        stop_worker();
        jet_audio_shutdown();
        free_band();
        return 1;
    }
    if (!jet_lua_call_load()) {
        jet_lua_close();
        stop_worker();
        jet_audio_shutdown();
        free_band();
        return 1;
    }

    const uint32_t startMs   = host_get_ticks_ms();
    uint32_t       lastUs    = host_get_ticks_us();
    uint32_t       fpsWindow = startMs;
    int            fpsFrames = 0;
    float          fps       = 0.0f;
    const uint32_t minFrameUs = fpsCap > 0 ? (uint32_t)(1000000 / fpsCap) : 0;

    while (!host_should_exit() && !jet_lua_wants_quit()) {
        uint32_t nowUs = host_get_ticks_us();
        float dt = (float)(nowUs - lastUs) / 1000000.0f;
        lastUs = nowUs;
        // A long stall (SD read, radio burst) would otherwise hand the game a
        // dt large enough to tunnel objects through each other.
        if (dt > 0.1f) dt = 0.1f;

        pump_input();

        jet_lua_set_timing(fps, (float)(host_get_ticks_ms() - startMs) / 1000.0f);
        uint32_t tl = host_get_ticks_us();
        if (!jet_lua_call_frame(dt)) break;
        s_tLua += host_get_ticks_us() - tl;

        // Refill the audio ring BEFORE the long render rather than after. The
        // ring holds ~186ms and a heavy frame costs 65ms, so topping up first
        // leaves the most headroom across the blocking part of the loop.
        jet_audio_service(host_get_ticks_us());

        render_frame(scene);

        fpsFrames++;
        uint32_t nowMs = host_get_ticks_ms();
        if (nowMs - fpsWindow >= 1000) {
            fps = (float)fpsFrames * 1000.0f / (float)(nowMs - fpsWindow);
            // A cap that engages silently is worse than the crash it replaces:
            // the frame is genuinely incomplete.
            if (scene.lastFrameDroppedTriangles > 0)
                mlog("[jet3d] *** RENDER QUEUE FULL: dropped %d triangles this"
                     " frame (cap %u) — frame NOT fully drawn\n",
                     scene.lastFrameDroppedTriangles,
                     (unsigned)scene.maxQueuedTriangles);
            s_tLua = s_tPrepare = s_tRaster = s_tBlit = 0;
            s_tPrepBegin = s_tPrepS0 = s_tPrepWait = s_tPrepS1 = s_tPrepEnd = 0;
            s_tFrames = 0;
            fpsWindow = nowMs;
            fpsFrames = 0;
        }

        if (minFrameUs) {
            uint32_t spent = host_get_ticks_us() - nowUs;
            if (spent < minFrameUs) host_sleep_ms((minFrameUs - spent) / 1000);
        } else {
            // Yield so the mesh, sound and input tasks still get scheduled.
            host_sleep_ms(1);
        }
    }

    jet_lua_close();
    stop_worker();
    jet_audio_shutdown();
    free_band();
    host_clear_screen();
    mlog("[jet3d] exit\n");
    return 0;
}
