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
//   -unixtime N  : current unix time from the launcher; drives MBC3 cart-clock
//                  catch-up (libc time() reads a clock nothing sets). 0/absent
//                  = unknown, catch-up is skipped

#include "gnuboy-src/gnuboy.h"

#include <setjmp.h>
#include <stdarg.h>
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
extern uint32_t host_get_ticks_us(void);
extern uint32_t host_get_ticks_ms(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);
extern void     host_audio_push(const int16_t* samples, int count, int sample_rate);
extern uint32_t host_psram_largest_free(void);
// T-Deck peer link, gblink service (src/tdeck_link.cpp). Requires firmware
// FW_API >= 2; on older firmware the module fails to load (UND audit).
// Status: low 2 bits = level (0 none / 1 session / 2 session + remote game),
// bit 0x4 = this deck is the USB-host side of the cable (role).
extern int      host_link_status(void);
extern int      host_link_gb_send(int cmd, int data_ctrl, unsigned ts);
extern int      host_link_gb_poll(unsigned* ts_out);
extern int      host_link_gb_wait(unsigned timeout_ms);

// ---------------------------------------------------------------------------
// Wall clock for the MBC3 cart-clock catch-up (gnuboy_rtc_catchup). Seeded
// by the launcher's -unixtime; ms ticks wrap at 49.7 days, far past any
// session. 0 = launcher sent nothing usable, gnuboy skips the catch-up.
// ---------------------------------------------------------------------------
static uint32_t s_epoch0 = 0;   // unix time at launch
static uint32_t s_ticks0 = 0;   // host ms tick at launch

uint32_t gb_host_unix_time(void)
{
    if (!s_epoch0) return 0;
    return s_epoch0 + (host_get_ticks_ms() - s_ticks0) / 1000;
}

// ---------------------------------------------------------------------------
// Link debug log — every number the lockstep decides on, written to SD so a
// frozen deck can be diagnosed post-mortem. Lines accumulate in a PSRAM ring
// from module start; once a link session reveals our role (host bit in the
// status), the ring drains to <romdir>/gblink_host.log or gblink_slave.log
// (truncated each run = always the latest run), flushing ~1/s during play
// and every ~0.5s inside the lockstep stall, so even a hard-frozen deck
// leaves its final state on the card. Solo play never touches the SD.
// (Run 7 ran with ZERO in-play SD I/O and still froze — the flush traffic
// is exonerated as the freeze trigger, so the forensics came back.)
// 512KB (was 64KB): the dense battle-start block flushed to SD slower than
// it filled and the ring overran TWICE (2 "bytes dropped" markers), blinding
// the cross-check in the exact region that corrupts. PSRAM is plentiful.
// Gated by GBLINK_DEBUG (hw.h) — OFF for release; see that flag's comment.
// ---------------------------------------------------------------------------
#if GBLINK_DEBUG
#define DL_CAP      (512 * 1024)
#define DL_PATH_MAX 256

static char*       s_dl_buf = NULL;
static unsigned    s_dl_w = 0, s_dl_f = 0;   /* written/flushed, monotonic */
static unsigned    s_dl_drop = 0;            /* ring-overrun bytes lost    */
static char        s_dl_path[DL_PATH_MAX];
static const char* s_dl_rom = NULL;          /* set in main() before use   */
static int         s_dl_started = 0;         /* log file created           */
static int         s_dl_off = 0;             /* disabled (no PSRAM)        */

void gb_dlog(const char* fmt, ...) {
    if (s_dl_off) return;
    if (!s_dl_buf) {
        s_dl_buf = (char*)malloc(DL_CAP);
        if (!s_dl_buf) { s_dl_off = 1; return; }
    }
    char line[192];
    unsigned us = host_get_ticks_us();
    int n = snprintf(line, sizeof line, "%u.%03u ",
                     us / 1000000u, (us / 1000u) % 1000u);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (unsigned)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    for (int i = 0; i < n; i++)
        s_dl_buf[(s_dl_w + (unsigned)i) % DL_CAP] = line[i];
    s_dl_w += (unsigned)n;
    if (s_dl_w - s_dl_f > DL_CAP) {          /* overran unflushed data */
        unsigned over = s_dl_w - s_dl_f - DL_CAP;
        s_dl_f += over;
        s_dl_drop += over;
    }
}

void gb_dlog_flush(void) {
    if (s_dl_off || !s_dl_buf) return;
    if (!s_dl_started) {
        int st = host_link_status();
        if (!(st & 3)) return;               /* role unknown until a session */
        const char* slash = s_dl_rom ? strrchr(s_dl_rom, '/') : NULL;
        size_t dl = slash ? (size_t)(slash - s_dl_rom + 1) : 0;
        if (dl > DL_PATH_MAX - 24) dl = DL_PATH_MAX - 24;
        memcpy(s_dl_path, s_dl_rom, dl);
        strcpy(s_dl_path + dl, (st & 4) ? "gblink_host.log" : "gblink_slave.log");
        FILE* f = fopen(s_dl_path, "wb");    /* truncate: latest run only */
        if (!f) return;                      /* SD hiccup: retry next flush */
        char hdr[DL_PATH_MAX + 48];
        int hn = snprintf(hdr, sizeof hdr, "=== gblink %s rom=%s ===\n",
                          (st & 4) ? "HOST" : "SLAVE",
                          s_dl_rom ? s_dl_rom : "?");
        fwrite(hdr, 1, (size_t)hn, f);
        fclose(f);
        s_dl_started = 1;
    }
    if (s_dl_f == s_dl_w && !s_dl_drop) return;
    FILE* f = fopen(s_dl_path, "ab");
    if (!f) return;                          /* transient: data kept, retry */
    if (s_dl_drop) {
        char m[48];
        int mn = snprintf(m, sizeof m, "[... %u bytes dropped ...]\n", s_dl_drop);
        fwrite(m, 1, (size_t)mn, f);
        s_dl_drop = 0;
    }
    while (s_dl_f != s_dl_w) {
        unsigned idx = s_dl_f % DL_CAP;
        unsigned chunk = s_dl_w - s_dl_f;
        if (chunk > DL_CAP - idx) chunk = DL_CAP - idx;
        fwrite(s_dl_buf + idx, 1, chunk, f);
        s_dl_f += chunk;
    }
    fclose(f);
}
#endif  /* GBLINK_DEBUG */


// ---------------------------------------------------------------------------
// Link-cable glue — the BGB protocol's client side (bgb.bircd.org/bgblink).
// Command values mirror TDL_GB_*: 1 ATTACH, 2 DETACH, 3 SYNC1 [data,ctrl,ts],
// 4 SYNC2 [data,0x80], 5 SYNC3 [1] not-transferring ack, 6 TSYNC [0,ts],
// 7 RESET (timestamp epoch restart, see below).
//
// This layer owns the LOCKSTEP state: it tracks the peer's 2MiHz emulated
// clock (from TSYNC and SYNC1 timestamps), holds a SYNC1 from the future
// until our own clock reaches it (so transfers happen at the same emulated
// moment on both sides — the fix for every arming race), and answers the
// frame loop's "may I run another frame?" via gb_link_should_stall() (never
// run more than GB_LINK_WINDOW ahead of the peer).
//
// TIMEKEEPING: both sides compare RAW timestamps — no per-side offset. BGB
// "maintains the difference" learned once per TCP connection, a mutual event
// on a transport that never silently drops. Ours flaps one-sidedly; the
// per-side learned offset (a running minimum) let the two baselines drift
// mutually inconsistent until BOTH decks believed they were ahead — a
// permanent two-sided freeze, confirmed on hardware by the gblink logs. So
// instead the CLOCKS themselves share an epoch: whenever the pairing forms
// or re-forms (we see the peer's game appear), we zero our clock and send
// RESET; receiving RESET zeroes ours (without re-announcing). Both clocks
// restart within a transport latency of each other, raw comparison is then
// sound, and any flap free-run is forgiven at the next re-pair. A mutual
// "both ahead" state is impossible by construction because both decks
// compare the same two numbers.
// ---------------------------------------------------------------------------

#define GB_TS_MASK      0x7FFFFFFFu     /* BGB timestamps are 31-bit */
#define GB_LINK_WINDOW  140448l         /* ~4 frames of 2MiHz clocks */

/* Tight window (~4ms) while an exchange is LIVE: armed as slave, or any
 * SYNC1 received within the last ~2 frames. The ripeness hold only protects
 * the deck that runs BEHIND the master; a deck running AHEAD processes
 * transfers stamped in its past at its own later state. Gen1's protocol
 * assumes frame-exact phase-entry ordering (guaranteed on real hardware by
 * the shared wall clock): hw run 13 showed the master's block-start gate
 * satisfied by a stale parked $FD while the receiving game — up to 4 frames
 * offset under the normal window — hadn't entered its receive loop yet, so
 * the first ~60 data bytes echoed back unheard (the partially-wrong battle
 * data), and a missed phase byte left both games livelocked in different
 * phases (60 vs FD forever). Keeping BOTH decks within ~4ms whenever
 * transfers are flowing restores the entry order; idle play (no SYNC1 for
 * 2+ frames) relaxes to the normal window. */
#define GB_LINK_TIGHT_WINDOW  8192l
#define GB_LINK_ACT_DECAY     70224l    /* ~2 frames: exchange considered live */

/* Rendezvous: a master transfer must complete against a genuinely ARMED
 * slave, not an echo. When a ripe SYNC1 arrives while we're unarmed mid-
 * exchange, HOLD it (don't hardware-shift) and let our CPU run forward to
 * its arm point — the armed path then answers with the real byte. Safe
 * because the master blocks per byte (one outstanding transfer), so we only
 * ever hold ONE. Bounded: if the game doesn't arm within ~1 frame it's a
 * genuine idle gap (overworld probe / left the exchange loop) and we fall
 * back to the hardware-shift. hw run 14: 543 unarmed shifts, 257 echoing the
 * master's prior byte = the "corrupted battle". */
#define GB_LINK_HOLD_MAX  35112l        /* ~1 frame before the shift fallback */

/* Peer clock: the largest timestamp heard this epoch (bursty delivery must
 * not regress it), with a backward jump >1s accepted as the peer restarting
 * its epoch (its RESET may still be in flight behind queued traffic). */
static unsigned s_peer_raw      = 0;
static int      s_peer_ts_valid = 0;

/* Exchange-activity tracking for the tight window: our clock when the last
 * SYNC1 arrived. Reset at pair latch (old-epoch stamp would be garbage). */
static unsigned s_last_s1_clk = 0;
static int      s_s1_seen     = 0;

/* Rendezvous hold state (see GB_LINK_HOLD_MAX). s_hold_active = a ripe SYNC1
 * at the queue head is being held for the game to arm; s_hold_clk = our
 * clock when the hold began (for the fallback timeout). Reset at pair latch. */
static int      s_hold_active = 0;
static unsigned s_hold_clk    = 0;

/* SYNC event fifo. The firmware ring is ALWAYS drained fully (TSYNCs are
 * consumed inline) so a held event can never starve the peer clock or hide
 * a reply queued behind it — the previous one-slot hold deadlocked a
 * master whose clock was frozen. */
#define GB_PQ 8
static int      s_pq_ev[GB_PQ];
static unsigned s_pq_ts[GB_PQ];
static int      s_pq_h = 0, s_pq_t = 0;

/* Signed a-b in 31-bit timestamp space (wrap-safe). */
static long ts_delta(unsigned a, unsigned b) {
    return ((long)(int)(((a - b) & GB_TS_MASK) << 1)) >> 1;
}

int  gb_link_cable(void)     { return (host_link_status() & 3) >= 1; }
int  gb_link_peer_game(void) { return (host_link_status() & 3) >= 2; }
void gb_link_idle(void)      { host_link_gb_wait(1); }  /* block until a link event or 1ms */

/* Exit-chord check for cpu.c's blocking completion wait (host veneer). */
int gb_link_should_exit(void) { return host_should_exit(); }

#if GBLINK_DEBUG
static const char* dl_cmd(int c) {
    switch (c) {
        case 1: return "ATTACH"; case 2: return "DETACH";
        case 3: return "SYNC1";  case 4: return "SYNC2";
        case 5: return "SYNC3";  case 6: return "TSYNC";
        case 7: return "RESET";
        default: return "?";
    }
}

/* One log line per link event, with the full lockstep snapshot at that
 * instant: our clock, the peer's clock (shared epoch, compared raw), the
 * signed delta the window is judged on, master-wait/slave-armed flags, and
 * the event-queue depth. */
static void dlog_ev(const char* tag, int cmd, int dc, unsigned ts) {
    unsigned clk = gb_link_clock & GB_TS_MASK;
    gb_dlog("%s %-6s d=%02x c=%02x ts=%08x | clk=%08x peer=%08x D=%+ld W%d S%d q%d",
            tag, dl_cmd(cmd), dc & 0xFF, (dc >> 8) & 0xFF, ts, clk, s_peer_raw,
            s_peer_ts_valid ? ts_delta(clk, s_peer_raw) : 0l,
            gb_link_wait, gb_link_slave,
            (s_pq_h - s_pq_t + GB_PQ) % GB_PQ);
}
#else
#define dlog_ev(...) ((void)0)
#endif

void gb_link_send(int cmd, int data_ctrl, unsigned ts) {
    dlog_ev("TX", cmd, data_ctrl, ts & GB_TS_MASK);
    host_link_gb_send(cmd, data_ctrl, ts & GB_TS_MASK);
}

static void gb_track_peer(unsigned raw) {
    if (!s_peer_ts_valid) {
        s_peer_ts_valid = 1;
        s_peer_raw = raw;
        return;
    }
    long d = ts_delta(raw, s_peer_raw);
    /* Largest-heard wins (burst delivery must not regress the estimate);
     * a backward jump >1s means the peer restarted its epoch and its RESET
     * is still queued behind this traffic — accept the restart now. */
    if (d > 0) {
        s_peer_raw = raw;
    } else if (d < -2097152l) {
        gb_dlog("PEER epoch restart: %08x -> %08x", s_peer_raw, raw);
        s_peer_raw = raw;
    }
}

/* Zero our timestamp clock: the pairing (both games attached, this epoch)
 * just formed or re-formed. announce=1 broadcasts RESET so the peer zeroes
 * too (mutual by construction); receiving a RESET latches WITHOUT
 * re-announcing, which terminates the exchange. A master mid-transfer
 * re-stamps its outstanding SYNC1 onto the new epoch and re-sends it —
 * sound because the transport delivers in order: the re-sent SYNC1 arrives
 * AFTER the RESET that flushes the peer's stale old-epoch state. */
static void gb_pair_latch(const char* why, int announce) {
    gb_dlog("PAIR latch (%s): clock %08x -> 0, queue flushed",
            why, gb_link_clock & GB_TS_MASK);
    gb_link_clock   = 0;
    s_peer_raw      = 0;
    s_peer_ts_valid = 0;
    s_s1_seen       = 0;           /* old-epoch activity stamp is garbage */
    s_hold_active   = 0;           /* held SYNC1 belonged to the old epoch */
    gb_link_rx_active = 0;         /* abandon any mid-epoch slave receive */
    s_pq_h = s_pq_t = 0;           /* queued events carry old-epoch stamps */
    if (announce)
        gb_link_send(7 /*RESET*/, 0, 0);
    if (gb_link_wait) {
        gb_link_s1_ts = 0;
        gb_link_send(3 /*SYNC1*/, gb_link_s1_dc, gb_link_s1_ts);
    }
}

/* Fully drain the firmware ring: timestamps feed the peer clock, SYNC
 * events queue here in arrival order. */
static void drain_host(void) {
    unsigned ts;
    int ev;
    while ((ev = host_link_gb_poll(&ts)) >= 0) {
        int cmd = ev >> 16;
        dlog_ev("RX", cmd, ev, ts);
        if (cmd == 7 /*RESET*/) { gb_pair_latch("rx-reset", 0); continue; }
        if (cmd == 6 /*TSYNC*/) { gb_track_peer(ts); continue; }
        if (cmd == 3 /*SYNC1*/) {
            gb_track_peer(ts);
            s_last_s1_clk = gb_link_clock & GB_TS_MASK;   /* exchange is live */
            s_s1_seen     = 1;
        }
        int n = (s_pq_h + 1) % GB_PQ;
        if (n != s_pq_t) {
            s_pq_ev[s_pq_h] = ev;
            s_pq_ts[s_pq_h] = ts;
            s_pq_h = n;
        } else {
            gb_dlog("RX DROP %s: event queue full", dl_cmd(cmd));
        }
    }
}

/* Next actionable link event as (cmd<<16)|(ctrl<<8)|data, or -1. A SYNC1
 * whose timestamp is still in our future is held until our clock reaches
 * it (transfers land at the same emulated moment on both sides) — but
 * ONLY while no master transfer is pending: a pending master must see a
 * colliding SYNC1 immediately to answer it (master/master collision). */
static unsigned s_last_ev_ts = 0;

int gb_link_poll(void) {
    drain_host();
    if (s_pq_h == s_pq_t) { s_hold_active = 0; return -1; }
    int ev = s_pq_ev[s_pq_t];
    unsigned clk = gb_link_clock & GB_TS_MASK;
    if (!gb_link_wait && (ev >> 16) == 3 /*SYNC1*/) {
        long fut = ts_delta(clk, s_pq_ts[s_pq_t]);
        if (fut < 0) {                   /* future SYNC1: not ripe yet */
            s_hold_active = 0;
            static unsigned held_logged = 0;
            if (s_pq_ts[s_pq_t] != held_logged) {
                held_logged = s_pq_ts[s_pq_t];
                gb_dlog("HOLD SYNC1 ts=%08x future by %ld", held_logged, -fut);
            }
            return -1;
        }
        /* RENDEZVOUS: ripe SYNC1 but we're not armed. Rather than shift an
         * echo, hold it so the CPU runs forward to its arm point and the
         * armed path answers the real byte. Bounded by GB_LINK_HOLD_MAX —
         * on timeout, release it to the shift fallback (genuine idle gap). */
        if (!gb_link_slave) {
            if (!s_hold_active) {
                s_hold_active = 1;
                s_hold_clk = clk;
                gb_dlog("SRV hold ts=%08x (await arm)", s_pq_ts[s_pq_t]);
            } else if (ts_delta(clk, s_hold_clk) >= GB_LINK_HOLD_MAX) {
                gb_dlog("SRV hold timeout %ld -> shift fallback",
                        ts_delta(clk, s_hold_clk));
                s_hold_active = 0;       /* fall through: return it to shift */
            }
            if (s_hold_active) return -1;
        }
    }
    if (s_hold_active) {
        /* Armed (or non-SYNC1 head): the hold resolved. */
        gb_dlog("SRV armed-after-hold %ld cyc", ts_delta(clk, s_hold_clk));
        s_hold_active = 0;
    }
    s_last_ev_ts = s_pq_ts[s_pq_t];
    s_pq_t = (s_pq_t + 1) % GB_PQ;
    return ev;
}

/* Raw timestamp of the last event gb_link_poll returned — the SYNC1 dedup
 * key for the retransmit path (cpu.c). */
unsigned gb_link_last_ts(void) {
    return s_last_ev_ts;
}

/* Feed the peer clock while the frame loop is stalled at the window. */
void gb_link_pump_ts(void) {
    drain_host();
}

/* Lockstep: true while we are more than the window ahead of the peer.
 * Raw comparison — both clocks share the RESET-latched epoch. The window is
 * DYNAMIC: tight while slave-armed OR while an exchange is live (SYNC1 seen
 * within ~2 frames — see GB_LINK_TIGHT_WINDOW), normal otherwise.
 * Deadlock-free: stalled ⇒ we are ahead ⇒ any incoming SYNC1 is stamped in
 * our past ⇒ ripe ⇒ the stall loop's service answers it. */
int gb_link_should_stall(void) {
    int st = host_link_status();
    if ((st & 3) < 2 || !s_peer_ts_valid) return 0;   /* no peer game */
    /* RENDEZVOUS: while a ripe SYNC1 is held for arm, NEVER stall — the CPU
     * must run the few hundred cycles forward to its arm point to answer it.
     * The master is frozen in its per-byte block, so our running slightly
     * ahead here is exactly what lets the transfer complete (breaks the
     * would-be deadlock). Bounded: the hold clears the moment we arm.
     * Same for a deferred slave receive (gb_link_rx_active): the CPU must
     * run the ~976us forward to fire the completion IRQ (bounded 1952 cyc). */
    if (s_hold_active || gb_link_rx_active) return 0;
    unsigned clk = gb_link_clock & GB_TS_MASK;
    long ahead = ts_delta(clk, s_peer_raw);
    int tight = gb_link_slave ||
                (s_s1_seen && ts_delta(clk, s_last_s1_clk) < GB_LINK_ACT_DECAY);
    return ahead > (tight ? GB_LINK_TIGHT_WINDOW : GB_LINK_WINDOW);
}

/* Extra GB.serial cycles added to the USB-host deck's transfers only (see
 * gb_link_xfer_cycles). Grows by GB_LINK_SKEW_STEP per master<->slave role flip
 * (capped at GB_LINK_SKEW_MAX), decays by STEP per clean SYNC2. 0 unless the
 * game is rapidly flipping its serial role (F-1 Race auto-negotiation). */
#define GB_LINK_SKEW_STEP  128
#define GB_LINK_SKEW_MAX   2048

static int s_neg_skew  = 0;   /* live crystal skew (cycles); 0 unless negotiating */
static int s_last_role = 0;   /* 0 none, 1 slave, 2 master — role-flip detector   */

/* Called at each serial-role arm with a cable present (hw.c RI_SC). Grows the
 * skew by GB_LINK_SKEW_STEP on a master<->slave flip. */
void gb_link_note_role(int is_master) {
    int role = is_master ? 2 : 1;
    if (s_last_role && role != s_last_role) {
        s_neg_skew += GB_LINK_SKEW_STEP;
        if (s_neg_skew > GB_LINK_SKEW_MAX) s_neg_skew = GB_LINK_SKEW_MAX;
        gb_dlog("SKEW flip r=%d -> %d", role, s_neg_skew);
    }
    s_last_role = role;
}

/* Called on each clean SYNC2 answer (cpu.c master wait). Reduces the skew by
 * GB_LINK_SKEW_STEP, floor 0. */
void gb_link_decay_skew(void) {
    s_neg_skew -= GB_LINK_SKEW_STEP;
    if (s_neg_skew < 0) s_neg_skew = 0;
}

/* Serial transfer duration in GB.serial (2MiHz) cycles. 1952 for single-player,
 * cooperative games, and the device deck; 1952 + s_neg_skew for the USB-host
 * deck. Called at each transfer arm (hw.c master arm, cpu.c slave completion). */
int gb_link_xfer_cycles(void) {
    if (!gb_link_cable() || s_neg_skew == 0) return 1952;
    return (host_link_status() & 4) ? (1952 + s_neg_skew) : 1952;
}

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
        } else if (strcmp(argv[i], "-unixtime") == 0 && i + 1 < argc) {
            s_epoch0 = (uint32_t)strtoul(argv[++i], NULL, 10);
            s_ticks0 = host_get_ticks_ms();
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
    printf("[gameboy] rom=%s scale=%s pal=%d resume=%d unixtime=%u\n",
           rom_path, scale_mode, pal, resume, (unsigned)s_epoch0);
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
    // After BOTH loads: a savestate restores the rtc counters too, so an
    // earlier catch-up would be overwritten. Prints itself when it acts.
    gnuboy_rtc_catchup();

    host_clear_screen();
    host_log("gameboy: entering main loop");
#if GBLINK_DEBUG
    s_dl_rom = rom_path;                    // debug log lives next to the ROM
#endif
    gb_link_send(1 /*ATTACH*/, 0, 0);       // announce the game to a link peer

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

        // Link status each frame: pairing edges drive the epoch latch, and
        // the debug log gets transitions immediately + a 1 Hz heartbeat.
        {
            static int st_last = -1;
            int st = host_link_status();
            if (st != st_last) {
                gb_dlog("LINK st %d -> %d", st_last, st);
                int was_paired = st_last >= 0 && (st_last & 3) >= 2;
                int now_paired = (st & 3) >= 2;
                st_last = st;
                if (now_paired && !was_paired)
                    gb_pair_latch("pair-up", 1);
                else if (was_paired && !now_paired) {
                    gb_dlog("PAIR lost");
                    s_peer_ts_valid = 0;
                }
                gb_dlog_flush();
            }
            if ((frames % 60) == 0) {
                gb_dlog("HB f=%u clk=%08x peer=%08x D=%+ld W%d S%d q%d",
                        (unsigned)frames, gb_link_clock & GB_TS_MASK,
                        s_peer_raw,
                        s_peer_ts_valid
                            ? ts_delta(gb_link_clock & GB_TS_MASK, s_peer_raw)
                            : 0l,
                        gb_link_wait, gb_link_slave,
                        (s_pq_h - s_pq_t + GB_PQ) % GB_PQ);
                gb_dlog_flush();
            }
        }

        // Broadcast our clock once per frame whenever the CABLE is present —
        // not just while paired. A deck that lost the pairing and played on
        // solo used to go TSYNC-silent while its ATTACH keepalives kept the
        // peer's lease alive: the peer sat pinned in a lockstep stall
        // waiting for clock updates that never came (run-4's zombie stall).
        // A solo deck's clock keeps a stalled peer converging and releasing.
        if (gb_link_cable())
            gb_link_send(6 /*TSYNC*/, 0, gb_link_clock);

        // BGB lockstep: never run more than the window ahead of the peer —
        // stall REAL time here (their clock catches up via TSYNCs; ours is
        // frozen). While stalled, keep answering transfers so a blocked
        // peer master isn't deadlocked on us. The exit chord must break the
        // stall too: a wedged link froze the deck so hard only a power
        // cycle helped.
        if (gb_link_peer_game()) {
            unsigned stall_ms = 0;
            int stalled = gb_link_should_stall();
            if (stalled)
                gb_dlog("STALL begin clk=%08x peer=%08x D=%+ld",
                        gb_link_clock & GB_TS_MASK, s_peer_raw,
                        ts_delta(gb_link_clock & GB_TS_MASK, s_peer_raw));
            while (gb_link_should_stall() && gb_link_peer_game()
                   && !host_should_exit()) {
                gb_link_pump_ts();
                gb_link_service();
                host_link_gb_wait(1);  /* block until a link event or 1ms */
                // KEEP REPORTING the frozen clock while stalled — the peer
                // can only converge onto it if it keeps hearing it. Going
                // silent here deadlocked both sides into a mutual stall
                // (each waiting for the other's next timestamp), which the
                // lease then broke and re-trapped in a stutter cycle.
                if ((++stall_ms & 31) == 0)
                    gb_link_send(6 /*TSYNC*/, 0, gb_link_clock);
                if ((stall_ms & 511) == 0) {
                    gb_dlog("STALL %ums peer=%08x D=%+ld", stall_ms, s_peer_raw,
                            ts_delta(gb_link_clock & GB_TS_MASK, s_peer_raw));
                    gb_dlog_flush();
                }
            }
            // Flush only after REAL stalls (>=100ms): the healthy lockstep
            // rides the window with 2-16ms micro-stalls many times a second,
            // and flushing each one is 20-60 SD open/append/close per sec —
            // pointless churn (though run 7 proved it harmless).
            if (stalled) {
                gb_dlog("STALL end %ums %s", stall_ms,
                        host_should_exit()   ? "exit-chord" :
                        gb_link_peer_game()  ? "converged"  : "peer-gone");
                if (stall_ms >= 100)
                    gb_dlog_flush();
            }
        }

        // Battery autosave: full save (quick_save truncates the file but
        // skips clean banks, so it would drop them — always save fully).
        // DEFERRED while a link peer is in a game: the SD write stalls the
        // emulator for hundreds of ms; under lockstep that's harmless (the
        // peer just waits with us) but it's still a visible mutual hiccup.
        // SRAM saves on exit, and autosave resumes when the peer detaches.
        if ((frames % 600) == 0 && gnuboy_sram_dirty() && !gb_link_peer_game()) {
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

    gb_link_send(2 /*DETACH*/, 0, 0);       // normal exit; elf_host also
                                            // detaches on the longjmp path
    gb_dlog("EXIT f=%u skip=%u", (unsigned)frames, (unsigned)skipped);
    gb_dlog_flush();
    gnuboy_free_rom();
    host_clear_screen();
    host_log("gameboy: module done");
    return 0;
}
