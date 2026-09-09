// T-Deck ↔ T-Deck peer link bridge — see tdeck_link.h for the contract.
//
// Threading model: ALL protocol state (session, parser, timers) is owned by
// one Core-1 task (tdl_task). The USB driver's RX completions (usb_task,
// also Core 1) push raw bytes into an SPSC ring the task drains; the ELF
// module's sends (Core 0) build frames and hand them to the active backend
// directly (device: atomic Serial write under the SLog lock; host: the
// driver's send() → dual-context pipe_xfer). Incoming gblink SYNC events
// land in a second SPSC ring the module polls — the same produce/consume
// discipline as elf_host's key ring.

#include "tdeck_link.h"

#include <Arduino.h>
#include <esp_heap_caps.h>   // dgram ring lives in PSRAM
#include "meshpunk_sync.h"

// ── Frame constants ──────────────────────────────────────────────────────────

#define TDL_MAGIC        0xA5
// 57 makes the largest frame exactly 64 bytes = one full-speed bulk packet,
// the most the tdeck USB driver's link_send() accepts per pipe transfer
// (it rejects anything over the endpoint's 64-byte max packet size). gblink
// frames stay tiny; the dgram service fills frames to this limit.
#define TDL_MAX_PAYLOAD  57
#define TDL_HDR          5                      // magic svc cmd seq len
#define TDL_CRC_LEN      2                      // CRC-16-CCITT, little-endian
#define TDL_MAX_FRAME    (TDL_HDR + TDL_MAX_PAYLOAD + TDL_CRC_LEN)

// Reliable-delivery: one frame in flight per service, retransmitted until
// acked. 10ms ≈ 2-3× the healthy ACK RTT (~3-5ms); the live-game 1ms task tick
// quantizes it.
#define TDL_RETX_MS      10
#define TDL_REL_FIFO     8

#define TDL_PING_MS      2000
// Generous: launching an ELF game (82KB module + ROM banks off SD) starves
// the link task for several seconds; a short timeout would drop the session
// right as the game starts. Self-heal still recovers a genuinely lost peer.
#define TDL_TIMEOUT_MS   15000
#define TDL_HELLO_MS     1000
#define TDL_LEASE_MS     3000    // remote-game lease (refreshed by ATTACH)

// ── Shared state (volatile flags read cross-task; rings SPSC) ───────────────

static volatile bool s_session       = false;   // HELLO exchanged, peer alive
static volatile bool s_remote_gb     = false;   // peer's game ATTACHed
static volatile bool s_local_gb      = false;   // our game running
static volatile bool s_usb_backend   = false;   // host role: driver registered
static volatile bool s_dgram_open    = false;   // a module listens on svc 2

// Volatile: tdeck_link_status() (module task, Core 0) evaluates the
// session/lease timeouts from these at READ time, so a starved tdl_task
// can't pin a stale "peer present". Written by tdl_task at parse time.
static volatile uint32_t s_last_rx_ms            = 0;
static volatile uint32_t s_last_remote_attach_ms = 0;

// Log mute (declared in meshpunk_sync.h): device role + live session + our
// game running = the serial port is a busy link cable; log lines competing
// for the CDC buffer can squeeze out frames (tdl_send drops when full) and
// one lost frame corrupts a GameBoy transfer. Host role never mutes (its
// frames ride the USB driver pipes; its Serial is dead in host mode anyway).
volatile bool g_slog_quiet = false;

static void update_quiet() {
    // A linked game = a GameBoy game attached OR a module listening on the
    // dgram service (Doom netplay over the cable): same wire, same policy.
    bool linked_game = s_local_gb || s_dgram_open;
    g_slog_quiet = !s_usb_backend && s_session && linked_game;
    // Mesh radio pause (both roles): a live cable session + our game running
    // = a link session — park the mesh dispatcher (radio SPI polling,
    // storage writes, BLE loop) so the link has the SPI bus and Core 1 to
    // itself. Everything stays in memory; the mesh task keeps ticking the
    // RTC and resumes the moment the game detaches, the cable is pulled, or
    // the session dies (elf_host's force-DETACH covers the crash path). The
    // radio stays in RX — a packet flagged mid-pause is serviced on resume.
    mesh_task_paused = s_session && linked_game;
}

static bool (*s_usb_send)(const uint8_t*, uint32_t) = nullptr;

// Raw RX ring: producer = usb_task (host role) — device role reads Serial
// directly in the task, no ring needed. Consumer = tdl_task. Sized to ride
// out multi-second tdl_task starvation without dropping mid-frame bytes
// (drops here were the splice-corruption generator on the device side).
#define RAWQ_SIZE 4096
static uint8_t          s_rawq[RAWQ_SIZE];
static volatile int     s_rawq_head = 0, s_rawq_tail = 0;
static portMUX_TYPE     s_rawq_mux = portMUX_INITIALIZER_UNLOCKED;

// gblink event ring: producer = tdl_task (core 1), consumer = ELF module
// task (core 0). Full BGB records: command, data (b2), control (b3),
// timestamp (i1).
#define GBQ_SIZE 32
struct GbEv {
    uint8_t  cmd;
    uint8_t  data;
    uint8_t  ctrl;
    uint32_t ts;
};
static GbEv             s_gbq[GBQ_SIZE];
static volatile int     s_gbq_head = 0, s_gbq_tail = 0;
static portMUX_TYPE     s_gbq_mux = portMUX_INITIALIZER_UNLOCKED;

// dgram service ring: producer = tdl_task (Core 1, reassembled datagrams),
// consumer = ELF module task (Core 0). Slots + the reassembly buffer live in
// PSRAM, allocated once at boot (~26KB) so the ring never has to be freed
// underneath a producer mid-copy: open/close only flip s_dgram_open and
// reset the indices. Each slot is one whole datagram.
#define DGQ_SIZE 16
struct DgSlot {
    uint16_t len;
    uint8_t  data[TDL_DGRAM_MAX];
};
static DgSlot*          s_dgq = nullptr;
static volatile int     s_dgq_head = 0, s_dgq_tail = 0;
static portMUX_TYPE     s_dgq_mux = portMUX_INITIALIZER_UNLOCKED;
// (s_dgram_open lives with the session flags above: update_quiet reads it.)
// Fragment reassembly — tdl_task only.
#define TDL_DGRAM_LAST  0x80                    // cmd bit 7: last fragment
static uint8_t*         s_dg_asm = nullptr;     // TDL_DGRAM_MAX bytes
static uint32_t         s_dg_asm_len = 0;
static int              s_dg_asm_next = -1;     // expected index, -1 = idle

// ── Frame build + backend send (callable from any task) ─────────────────────

// CRC-16-CCITT (poly 0x1021, init 0xFFFF). ~180 bit-steps for a max frame —
// cheap enough to run inside a spinlock section.
static uint16_t tdl_crc16(const uint8_t* d, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021)
                             : (uint16_t)(c << 1);
    }
    return c;
}

// Serialize a frame into buf (>= TDL_MAX_FRAME); returns total length.
static uint32_t tdl_build(uint8_t* f, uint8_t svc, uint8_t cmd, uint8_t seq,
                          const uint8_t* payload, uint8_t len) {
    f[0] = TDL_MAGIC; f[1] = svc; f[2] = cmd; f[3] = seq; f[4] = len;
    for (uint8_t i = 0; i < len; i++) f[TDL_HDR + i] = payload[i];
    uint16_t crc = tdl_crc16(f, TDL_HDR + len);
    f[TDL_HDR + len]     = (uint8_t)crc;
    f[TDL_HDR + len + 1] = (uint8_t)(crc >> 8);
    return (uint32_t)(TDL_HDR + len + TDL_CRC_LEN);
}

// Transport only — no framing knowledge. Whole frames, atomic-or-dropped:
// the device role's availableForWrite guard drops a frame it can't write in
// full (never truncates), and the reliable layer's retransmit covers drops.
//
// SERIALIZED — senders live on two cores (module task: TSYNCs + SYNC
// traffic; tdl_task: acks, keepalives, retransmits). The USB-host role's
// link socket drives ONE bulk-OUT pipe with ONE transfer object; two
// unserialized callers memcpy'd the same pipe buffer and DOUBLE-SUBMITTED
// the same usb transfer into the host stack — corrupting its transfer list
// and hard-freezing the whole deck (hw runs 6-8: always the USB-host deck,
// crash point shifted by log-flush timing = the race signature; v2's ack
// traffic made collisions routine because the frame that triggers our ack
// also triggers the module's next send). The device role's Serial branch
// was always accidentally safe under SLOG_LOCK; now both roles are
// explicitly serialized here. Hold time is bounded: pipe_xfer times out at
// 250ms, Serial writes are availableForWrite-gated and never block.
static SemaphoreHandle_t s_send_mux = nullptr;

// s_tdl_task: notified (xTaskNotifyGive) on RX arrival so tdl_task drains it
// without waiting for its poll timeout. s_gb_wake: given on each gblink-event
// delivery; the ELF module's master-wait / lockstep-stall loops block on it.
static TaskHandle_t      s_tdl_task = nullptr;
static SemaphoreHandle_t s_gb_wake  = nullptr;

static bool tdl_backend_send(const uint8_t* f, uint32_t n) {
    bool ok = false;
    if (s_send_mux) xSemaphoreTake(s_send_mux, portMAX_DELAY);
    if (s_usb_backend && s_usb_send) {          // host role
        ok = s_usb_send(f, n);
    } else {
        // Device role: the peer host reads our USB-Serial-JTAG TX. Atomic
        // under the SLog lock (never interleaves with a log line),
        // non-blocking.
        SLOG_LOCK();
        if ((size_t)Serial.availableForWrite() >= n) {
            Serial.write(f, n);
            ok = true;
        }
        SLOG_UNLOCK();
    }
    if (s_send_mux) xSemaphoreGive(s_send_mux);
    return ok;
}

// Fire-and-forget send (seq 0): superseding/idempotent traffic only.
static bool tdl_send(uint8_t svc, uint8_t cmd,
                     const uint8_t* payload, uint8_t len) {
    if (len > TDL_MAX_PAYLOAD) return false;
    uint8_t f[TDL_MAX_FRAME];
    uint32_t n = tdl_build(f, svc, cmd, 0, payload, len);
    return tdl_backend_send(f, n);
}

static bool tdl_send1(uint8_t svc, uint8_t cmd, uint8_t b) {
    return tdl_send(svc, cmd, &b, 1);
}

// ── Reliable channel: window-1 seq/ack/retransmit per service ───────────────
//
// Exactly-once, in-order delivery for the frames the gblink layer cannot
// afford to lose (SYNC1/2/3, RESET). One frame in flight; a small FIFO
// preserves order behind it; the 5ms task tick drives retransmission. TX may
// come from the ELF task (Core 0) while acks/ticks run on tdl_task (Core 1),
// so state lives under a spinlock — transport writes happen OUTSIDE it.

struct RelFrame {
    uint8_t len;
    uint8_t buf[TDL_MAX_FRAME];
};

struct RelChan {
    uint8_t      svc;
    uint8_t      tx_seq;             // last assigned (0 = none yet)
    volatile uint8_t rx_seq;         // last delivered (0 = none; no frame
                                     // carries seq 0, so 0 never dedups)
    RelFrame     fifo[TDL_REL_FIFO]; // [ft] = oldest = the in-flight frame
    int          fh, ft, count;
    bool         inflight;           // fifo[ft] has been transmitted
    uint32_t     deadline;           // next retransmit time for fifo[ft]
    portMUX_TYPE mux;
};

static RelChan s_rel_gb = {
    TDL_SVC_GBLINK, 0, 0, {}, 0, 0, 0, false, 0,
    portMUX_INITIALIZER_UNLOCKED
};

// (Re)transmit the oldest queued frame. Copies out under the lock, sends
// outside it (the backend takes the SLog mutex / crosses into the driver).
static void rel_tx_tail(RelChan& ch, uint32_t now) {
    uint8_t buf[TDL_MAX_FRAME];
    uint32_t n = 0;
    portENTER_CRITICAL(&ch.mux);
    if (ch.count > 0) {
        n = ch.fifo[ch.ft].len;
        memcpy(buf, ch.fifo[ch.ft].buf, n);
        ch.inflight = true;
        ch.deadline = now + TDL_RETX_MS;
    }
    portEXIT_CRITICAL(&ch.mux);
    if (n) tdl_backend_send(buf, n);
}

// Queue a reliable frame (any task). Kicks the transmitter when it was idle.
static bool rel_push(RelChan& ch, uint8_t cmd,
                     const uint8_t* payload, uint8_t len) {
    if (len > TDL_MAX_PAYLOAD) return false;
    bool kick = false;
    portENTER_CRITICAL(&ch.mux);
    if (ch.count == TDL_REL_FIFO) {             // never expected: gblink is
        portEXIT_CRITICAL(&ch.mux);             // ping-pong, depth stays <=2
        return false;
    }
    ch.tx_seq = (uint8_t)(ch.tx_seq == 255 ? 1 : ch.tx_seq + 1);
    RelFrame& slot = ch.fifo[ch.fh];
    slot.len = (uint8_t)tdl_build(slot.buf, ch.svc, cmd, ch.tx_seq,
                                  payload, len);
    ch.fh = (ch.fh + 1) % TDL_REL_FIFO;
    kick = (ch.count++ == 0);
    portEXIT_CRITICAL(&ch.mux);
    if (kick) rel_tx_tail(ch, millis());
    return true;
}

// Peer confirmed receipt of the in-flight frame: pop it, launch the next.
static void rel_on_ack(RelChan& ch, uint8_t seq) {
    bool next = false;
    portENTER_CRITICAL(&ch.mux);
    if (ch.count > 0 && ch.inflight && ch.fifo[ch.ft].buf[3] == seq) {
        ch.ft = (ch.ft + 1) % TDL_REL_FIFO;
        ch.count--;
        ch.inflight = false;
        next = (ch.count > 0);
    }
    portEXIT_CRITICAL(&ch.mux);
    if (next) rel_tx_tail(ch, millis());
}

// Retransmit driver (tdl_task tick).
static void rel_tick(RelChan& ch, uint32_t now) {
    bool retx = false;
    portENTER_CRITICAL(&ch.mux);
    retx = (ch.count > 0 && ch.inflight &&
            (int32_t)(now - ch.deadline) >= 0);
    portEXIT_CRITICAL(&ch.mux);
    if (retx) rel_tx_tail(ch, now);
}

// Session boundary: both sides forget all sequencing state, so a rebooted
// peer's fresh seq numbers can never collide with stale dedup history.
static void rel_reset(RelChan& ch) {
    portENTER_CRITICAL(&ch.mux);
    ch.fh = ch.ft = ch.count = 0;
    ch.inflight = false;
    ch.tx_seq = 0;
    ch.rx_seq = 0;
    portEXIT_CRITICAL(&ch.mux);
}

// ── Public: module-facing gblink service ─────────────────────────────────────

bool tdeck_link_gb_send(uint8_t cmd, uint16_t data_ctrl, uint32_t ts) {
    if (cmd == TDL_GB_ATTACH) { s_local_gb = true;  update_quiet(); }
    if (cmd == TDL_GB_DETACH) { s_local_gb = false; update_quiet(); }
    if (!s_session) return false;               // flag kept; announced on HELLO
    uint8_t data = (uint8_t)data_ctrl;
    uint8_t ctrl = (uint8_t)(data_ctrl >> 8);
    uint8_t pl[6];
    switch (cmd) {
        case TDL_GB_SYNC1:                       // [data, control, ts32]
            pl[0] = data; pl[1] = ctrl;
            pl[2] = (uint8_t)ts;         pl[3] = (uint8_t)(ts >> 8);
            pl[4] = (uint8_t)(ts >> 16); pl[5] = (uint8_t)(ts >> 24);
            return rel_push(s_rel_gb, cmd, pl, 6);
        case TDL_GB_SYNC2:                       // [data, 0x80]
            pl[0] = data; pl[1] = ctrl;
            return rel_push(s_rel_gb, cmd, pl, 2);
        case TDL_GB_SYNC3:                       // [1] ack
            pl[0] = data;
            return rel_push(s_rel_gb, cmd, pl, 1);
        case TDL_GB_RESET:                       // epoch restart: exactly-once
            return rel_push(s_rel_gb, cmd, nullptr, 0);
        case TDL_GB_TSYNC:                       // [0, ts32] — superseded in
            pl[0] = 0;                           // 16ms, so best-effort
            pl[1] = (uint8_t)ts;         pl[2] = (uint8_t)(ts >> 8);
            pl[3] = (uint8_t)(ts >> 16); pl[4] = (uint8_t)(ts >> 24);
            return tdl_send(TDL_SVC_GBLINK, cmd, pl, 5);
        default:                                 // ATTACH / DETACH: keepalive
            return tdl_send(TDL_SVC_GBLINK, cmd, nullptr, 0);
    }
}

// TSYNC lives in a latest-value slot, NOT the event ring: only the newest
// peer clock matters, and 60/s of them through the shared ring could evict
// a real transfer while the module wasn't draining (SD log flush) — which,
// pre-fix, got acked-and-dropped = permanent desync. The ring now carries
// only real events (SYNC1/2/3, RESET — ping-pong keeps it nearly empty).
static volatile uint32_t s_ts_latest = 0;
static volatile uint32_t s_ts_gen    = 0;   // bumped per arrival (tdl_task)
static uint32_t          s_ts_taken  = 0;   // consumer-side last-seen gen

int tdeck_link_gb_poll(uint32_t* ts_out) {
    if (s_gbq_head != s_gbq_tail) {         // real events first
        __sync_synchronize();
        GbEv e = s_gbq[s_gbq_tail];
        s_gbq_tail = (s_gbq_tail + 1) % GBQ_SIZE;
        if (ts_out) *ts_out = e.ts;
        return ((int)e.cmd << 16) | ((int)e.ctrl << 8) | e.data;
    }
    uint32_t g = s_ts_gen;
    if (g != s_ts_taken) {                  // fresh peer clock
        s_ts_taken = g;
        if (ts_out) *ts_out = s_ts_latest;
        return (int)TDL_GB_TSYNC << 16;
    }
    return -1;
}

int tdeck_link_status() {
    // Low 2 bits: 0 = no channel; 1 = session up (cable present: the firmware
    // answers SYNC1 with the not-transferring ack when no game is attached, so
    // transfers always resolve); 2 = session + the peer's game attached
    // (the module applies the BGB lockstep window at this level).
    // Bit 2 (0x4): this deck is the USB-host side of the cable (role — only
    // meaningful while a session is up). Callers wanting the level mask & 3.
    //
    // The session/lease timeouts are evaluated HERE, at read time, from the
    // last-refresh stamps — not just by tdl_task's periodic transitions. On
    // hw a starved-to-death tdl_task pinned status at 2 for 28 seconds and
    // froze a lockstep stall that a live lease would have released in 3;
    // this read-time view degrades that to a normal "peer gone". tdl_task
    // still owns the real state transitions and their log lines.
    if (!s_session) return 0;
    uint32_t now = millis();
    if (now - s_last_rx_ms >= TDL_TIMEOUT_MS) return 0;   // peer silent
    int st = (s_remote_gb && now - s_last_remote_attach_ms < TDL_LEASE_MS)
                 ? 2 : 1;
    return st | (s_usb_backend ? 4 : 0);
}

// ── Public: USB-host backend hooks (usb_task context) ───────────────────────

void tdeck_link_usb_register(bool (*send)(const uint8_t*, uint32_t)) {
    s_usb_send    = send;
    s_usb_backend = true;
}

void tdeck_link_usb_unregister() {
    if (s_session) tdl_send(TDL_SVC_CTRL, TDL_C_BYE, nullptr, 0);
    s_usb_backend = false;
    s_usb_send    = nullptr;
    s_session     = false;
    s_remote_gb   = false;
    rel_reset(s_rel_gb);
    update_quiet();
}

void tdeck_link_usb_rx(const uint8_t* d, uint32_t n) {
    portENTER_CRITICAL(&s_rawq_mux);
    for (uint32_t i = 0; i < n; i++) {
        int next = (s_rawq_head + 1) % RAWQ_SIZE;
        if (next == s_rawq_tail) break;         // full: drop the rest
        s_rawq[s_rawq_head] = d[i];
        __sync_synchronize();
        s_rawq_head = next;
    }
    portEXIT_CRITICAL(&s_rawq_mux);
    // Wake tdl_task to drain this RX immediately.
    if (s_tdl_task) xTaskNotifyGive(s_tdl_task);
}

// Block until a gblink event is delivered to the queue, or timeout_ms elapses.
// Called from the ELF module's master-wait and lockstep-stall loops (Core 0).
int tdeck_link_gb_wait(uint32_t timeout_ms) {
    if (s_gb_wake) xSemaphoreTake(s_gb_wake, pdMS_TO_TICKS(timeout_ms));
    return 0;
}

// ── Public: module-facing dgram service ─────────────────────────────────────

bool tdeck_link_dgram_open() {
    if (!s_dgq) return false;                   // boot allocation failed
    portENTER_CRITICAL(&s_dgq_mux);
    s_dgq_head = s_dgq_tail = 0;                // drop anything stale
    s_dgram_open = true;
    portEXIT_CRITICAL(&s_dgq_mux);
    update_quiet();
    return true;
}

void tdeck_link_dgram_close() {
    s_dgram_open = false;
    update_quiet();
}

// Fire-and-forget: every fragment rides a seq-0 frame. A fragment the
// backend can't take (CDC buffer full, pipe busy) loses the whole datagram —
// UDP semantics, which is what the module protocols on top expect.
bool tdeck_link_dgram_send(const uint8_t* d, uint32_t n) {
    if (!s_session || n == 0 || n > TDL_DGRAM_MAX) return false;
    uint32_t off = 0;
    uint8_t  idx = 0;
    while (off < n) {
        uint32_t chunk = n - off;
        if (chunk > TDL_MAX_PAYLOAD) chunk = TDL_MAX_PAYLOAD;
        bool last = (off + chunk == n);
        uint8_t cmd = (uint8_t)(idx | (last ? TDL_DGRAM_LAST : 0));
        if (!tdl_send(TDL_SVC_DGRAM, cmd, d + off, (uint8_t)chunk)) return false;
        off += chunk;
        idx++;
    }
    return true;
}

int tdeck_link_dgram_recv(uint8_t* buf, uint32_t max) {
    if (!s_dgq || s_dgq_head == s_dgq_tail) return -1;
    __sync_synchronize();
    const DgSlot& s = s_dgq[s_dgq_tail];
    uint32_t len = s.len;
    uint32_t n = len < max ? len : max;
    memcpy(buf, s.data, n);
    portENTER_CRITICAL(&s_dgq_mux);
    s_dgq_tail = (s_dgq_tail + 1) % DGQ_SIZE;
    portEXIT_CRITICAL(&s_dgq_mux);
    return (int)len;
}

// ── Protocol (tdl_task only) ─────────────────────────────────────────────────

// Returns false when the ring is full. For reliable frames the caller MUST
// withhold the ACK on failure — an acked-but-dropped transfer is
// unrecoverable (the sender rightly stops retransmitting; hw run 3's desync).
static bool gbq_push(uint8_t cmd, uint8_t data, uint8_t ctrl, uint32_t ts) {
    bool ok = false;
    portENTER_CRITICAL(&s_gbq_mux);
    int next = (s_gbq_head + 1) % GBQ_SIZE;
    if (next != s_gbq_tail) {
        s_gbq[s_gbq_head].cmd  = cmd;
        s_gbq[s_gbq_head].data = data;
        s_gbq[s_gbq_head].ctrl = ctrl;
        s_gbq[s_gbq_head].ts   = ts;
        __sync_synchronize();
        s_gbq_head = next;
        ok = true;
    }
    portEXIT_CRITICAL(&s_gbq_mux);
    // Wake the module's wait/stall loop (tdeck_link_gb_wait).
    if (ok && s_gb_wake) xSemaphoreGive(s_gb_wake);
    return ok;
}

// Queue one reassembled datagram for the module. The producer owns the slot
// at head until it publishes the index, so the copy runs outside the lock.
// Full ring = drop (the module protocol resends; same as a lost UDP packet).
static bool dgq_push(const uint8_t* d, uint32_t n) {
    int head = s_dgq_head;
    int next = (head + 1) % DGQ_SIZE;
    if (next == s_dgq_tail) return false;
    DgSlot& s = s_dgq[head];
    s.len = (uint16_t)n;
    memcpy(s.data, d, n);
    __sync_synchronize();
    portENTER_CRITICAL(&s_dgq_mux);
    bool ok = s_dgram_open;                     // closed meanwhile: discard
    if (ok) s_dgq_head = next;
    portEXIT_CRITICAL(&s_dgq_mux);
    return ok;
}

// One dgram fragment off the wire (tdl_task). Fragments arrive in order on
// a serial link, so reassembly is a running index: 0 starts a datagram, any
// other mismatch (a lost fragment) discards it until the next index 0.
static void dgram_on_fragment(uint8_t cmd, const uint8_t* p, uint8_t len) {
    if (!s_dgram_open || !s_dg_asm) { s_dg_asm_next = -1; return; }
    int  idx  = cmd & 0x7F;
    bool last = (cmd & TDL_DGRAM_LAST) != 0;
    if (idx == 0) { s_dg_asm_len = 0; s_dg_asm_next = 0; }
    if (idx != s_dg_asm_next) { s_dg_asm_next = -1; return; }
    if (s_dg_asm_len + len > TDL_DGRAM_MAX) { s_dg_asm_next = -1; return; }
    memcpy(s_dg_asm + s_dg_asm_len, p, len);
    s_dg_asm_len += len;
    s_dg_asm_next++;
    if (last) {
        dgq_push(s_dg_asm, s_dg_asm_len);
        s_dg_asm_next = -1;
    }
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t s_last_ping_ms   = 0;
static uint32_t s_last_hello_ms  = 0;
static uint32_t s_last_attach_ms = 0;

static void session_up(const char* how) {
    if (!s_session) {
        SLog.printf("[tdl] peer session up (%s)\r\n", how);   // before mute
        rel_reset(s_rel_gb);        // fresh sequencing per session, both ends
        s_session = true;
        update_quiet();
    }
    // (Re)announce our game state so a session established mid-game links up.
    if (s_local_gb) tdl_send(TDL_SVC_GBLINK, TDL_GB_ATTACH, nullptr, 0);
}

static void session_down(const char* why) {
    bool was = s_session;
    s_session   = false;
    s_remote_gb = false;
    rel_reset(s_rel_gb);            // in-flight frames die with the session
    s_dg_asm_next = -1;             // a half-built datagram dies with it too
    update_quiet();
    if (was) SLog.printf("[tdl] peer session down (%s)\r\n", why); // post-unmute
}

// Reliable gblink delivery. Returns true only when the event actually
// reached its consumer (queued for the module, or answered/absorbed by the
// firmware itself) — the caller sends the ACK on success and stays SILENT on
// failure, so the peer's retransmit timer retries into a drained queue.
static bool gblink_deliver_rel(uint8_t cmd, const uint8_t* p, uint8_t len) {
    switch (cmd) {
        case TDL_GB_SYNC1:
            // BGB rule: a SYNC1 is ALWAYS answered. With no local game
            // attached the module isn't polling, so the firmware itself
            // answers the "not transferring" ack — the peer's master reads
            // open-bus 0xFF promptly instead of stalling on an empty deck.
            // The answer rides the reliable channel (the master's clock is
            // frozen on it); queuing that answer IS the delivery.
            if (!s_local_gb) {
                uint8_t ack[1] = { 1 };
                return rel_push(s_rel_gb, TDL_GB_SYNC3, ack, 1);
            }
            if (len < 6) return true;           // malformed: swallow
            return gbq_push(cmd, p[0], p[1], le32(p + 2));
        case TDL_GB_SYNC2:
            if (len < 2) return true;
            return gbq_push(cmd, p[0], p[1], 0);
        case TDL_GB_SYNC3:
            if (len < 1) return true;
            return gbq_push(cmd, p[0], 0, 0);
        case TDL_GB_RESET:
            // Clock-epoch restart: only the module cares (and only while a
            // game runs here; an idle deck has no clock to reset).
            if (!s_local_gb) return true;
            return gbq_push(cmd, 0, 0, 0);
        default:
            return true;                        // unknown: ack + ignore
    }
}

static void on_frame(uint8_t svc, uint8_t cmd,
                     const uint8_t* p, uint8_t len) {
    if (svc == TDL_SVC_CTRL) {
        switch (cmd) {
            case TDL_C_HELLO:
                if (len >= 1 && p[0] != TDL_PROTO_VERSION) {
                    SLog.printf("[tdl] peer proto %u != %u — ignoring\r\n",
                                p[0], TDL_PROTO_VERSION);
                    session_down("proto mismatch");
                    return;
                }
                // Reply so the initiator learns we're here (device role never
                // initiates; only reply when WE didn't, to avoid a loop).
                if (!s_usb_backend) tdl_send1(TDL_SVC_CTRL, TDL_C_HELLO, TDL_PROTO_VERSION);
                break;
            case TDL_C_PING: tdl_send(TDL_SVC_CTRL, TDL_C_PONG, nullptr, 0); break;
            case TDL_C_PONG: break;
            case TDL_C_BYE:  session_down("bye"); break;
            case TDL_C_ACK:                      // reliable-frame receipt
                if (len >= 2 && p[0] == TDL_SVC_GBLINK)
                    rel_on_ack(s_rel_gb, p[1]);
                break;
            default: break;
        }
    } else if (svc == TDL_SVC_GBLINK) {
        // Best-effort gblink traffic only — the reliable commands
        // (SYNC1/2/3, RESET) are dispatched through gblink_deliver_rel by
        // the parser, where delivery success gates the ACK.
        switch (cmd) {
            case TDL_GB_ATTACH:
                s_remote_gb = true;
                s_last_remote_attach_ms = millis();
                break;
            case TDL_GB_DETACH: s_remote_gb = false; break;
            case TDL_GB_TSYNC:
                if (len >= 5) { s_ts_latest = le32(p + 1); s_ts_gen = s_ts_gen + 1; }
                break;
            default: break;
        }
    } else if (svc == TDL_SVC_DGRAM) {
        dgram_on_fragment(cmd, p, len);
    }
    // unknown services: ignored (forward compatibility)
}

// Resync-scanning parser: tolerates SLog ASCII and line noise around frames.
static uint8_t s_fbuf[TDL_MAX_FRAME];
static int     s_flen = 0;

static void parse_byte(uint8_t b) {
    if (s_flen == 0) {
        if (b != TDL_MAGIC) return;             // hunt for magic
        s_fbuf[s_flen++] = b;
        return;
    }
    s_fbuf[s_flen++] = b;
    if (s_flen == TDL_HDR && s_fbuf[4] > TDL_MAX_PAYLOAD) {
        s_flen = 0;                             // bogus length: resync
        return;
    }
    if (s_flen >= TDL_HDR &&
        s_flen == TDL_HDR + s_fbuf[4] + TDL_CRC_LEN) {
        uint16_t want = (uint16_t)s_fbuf[s_flen - 2] |
                        ((uint16_t)s_fbuf[s_flen - 1] << 8);
        if (tdl_crc16(s_fbuf, s_flen - TDL_CRC_LEN) == want) {
            uint8_t svc = s_fbuf[1], cmd = s_fbuf[2], seq = s_fbuf[3];
            s_last_rx_ms = millis();            // any valid frame = liveness
            // Self-heal: ANY valid frame proves the peer is alive, so it
            // (re)opens the session — a one-off startup timeout once latched
            // it DOWN forever. BYE alone means "going away" and must not
            // revive it. (svc-qualified: gblink SYNC2 shares BYE's cmd id.)
            if (!s_session && !(svc == TDL_SVC_CTRL && cmd == TDL_C_BYE))
                session_up("rx");
            if (seq == 0) {
                on_frame(svc, cmd, &s_fbuf[TDL_HDR], s_fbuf[4]);
            } else if (svc == TDL_SVC_GBLINK) {
                // Reliable frame. The ACK means "delivered to the consumer",
                // NOT "seen by this parser": a full event queue withholds it
                // so the peer's retransmit retries into a drained queue. A
                // duplicate (our ack was lost) is re-acked without
                // re-delivering — window-1 makes the last-delivered seq the
                // only possible duplicate.
                uint8_t a[2] = { svc, seq };
                if (s_rel_gb.rx_seq == seq) {
                    tdl_send(TDL_SVC_CTRL, TDL_C_ACK, a, 2);
                } else if (gblink_deliver_rel(cmd, &s_fbuf[TDL_HDR],
                                              s_fbuf[4])) {
                    s_rel_gb.rx_seq = seq;
                    tdl_send(TDL_SVC_CTRL, TDL_C_ACK, a, 2);
                }
            } else {
                // Reliable frame for a service without a channel yet:
                // ack + hand through (only we author peers today).
                uint8_t a[2] = { svc, seq };
                tdl_send(TDL_SVC_CTRL, TDL_C_ACK, a, 2);
                on_frame(svc, cmd, &s_fbuf[TDL_HDR], s_fbuf[4]);
            }
        }
        s_flen = 0;                             // bad CRC: drop + resync
    }
}

static void tdl_task_body(void*) {
    for (;;) {

        // Drain whichever byte source is live.
        if (s_usb_backend) {
            while (s_rawq_head != s_rawq_tail) {
                __sync_synchronize();
                uint8_t b = s_rawq[s_rawq_tail];
                s_rawq_tail = (s_rawq_tail + 1) % RAWQ_SIZE;
                parse_byte(b);
            }
        } else {
            // Device role: our USB-Serial-JTAG RX. Harmless when the PHY is
            // switched to OTG host mode (reads simply return nothing). The
            // guard clears a full post-starvation backlog in one pass (the
            // enlarged CDC RX buffer can hold seconds of link traffic).
            int guard = 4096;
            while (guard-- > 0 && Serial.available() > 0) {
                int c = Serial.read();
                if (c >= 0) parse_byte((uint8_t)c);
            }
        }

        uint32_t now = millis();

        rel_tick(s_rel_gb, now);                // reliable-frame retransmits
        if (s_usb_backend) {
            if (!s_session && now - s_last_hello_ms >= TDL_HELLO_MS) {
                s_last_hello_ms = now;
                tdl_send1(TDL_SVC_CTRL, TDL_C_HELLO, TDL_PROTO_VERSION);
            }
            if (s_session && now - s_last_ping_ms >= TDL_PING_MS) {
                s_last_ping_ms = now;
                tdl_send(TDL_SVC_CTRL, TDL_C_PING, nullptr, 0);
            }
        }
        if (s_session && now - s_last_rx_ms >= TDL_TIMEOUT_MS)
            session_down("timeout");

        // ATTACH keepalive: game-presence is otherwise only announced once
        // (game start / own session recovery), so a single one-sided session
        // flap left the two decks permanently disagreeing about whether a
        // peer game exists — one played on "unconnected" while the other
        // stalled forever at the lockstep window. Refreshing the flag every
        // second makes the state converge after any flap.
        if (s_session && s_local_gb && now - s_last_attach_ms >= 1000) {
            s_last_attach_ms = now;
            tdl_send(TDL_SVC_GBLINK, TDL_GB_ATTACH, nullptr, 0);
        }
        // ...and the receive side treats it as a LEASE: no refresh for 3s
        // (lost DETACH, peer reset, one-sided flap) = peer game gone. A
        // stale "peer present" flag pinned a deck at the lockstep window
        // forever; a stale "absent" is healed by the next keepalive.
        if (s_remote_gb && now - s_last_remote_attach_ms >= TDL_LEASE_MS) {
            s_remote_gb = false;
            SLog.printf("[tdl] peer game lease expired\r\n");
        }

        // Poll fast (1ms) during a live link game, idle-slow (5ms) otherwise.
        // Frames are SENT immediately (module task), but the peer only drains
        // RX + acks + delivers on THIS tick — so a 5ms tick added up to two
        // tick-delays (~10ms) of round-trip per byte. That is the per-byte
        // block that stalls the emulator (no frame/audio runs while a master
        // waits), starving sound on heavy transfers (Pokemon data blocks) and
        // glitching poll-heavy menus (F-1 Race). 1ms cuts it to ~2ms. Safe:
        // during a linked game the mesh is paused (update_quiet), so Core 1
        // has the headroom, and this loop is microseconds of work per wake.
        // Wake on RX (xTaskNotifyGive from tdeck_link_usb_rx) or after the
        // timeout for retransmit/keepalive housekeeping.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(
            (s_session && (s_local_gb || s_dgram_open)) ? 1 : 5));
    }
}

void tdeck_link_init() {
    // Send serializer — must exist before the task (and any module send).
    s_send_mux = xSemaphoreCreateMutex();
    s_gb_wake  = xSemaphoreCreateBinary();   // module wait/stall wakeup
    // dgram ring + reassembly buffer: PSRAM, kept for the firmware's
    // lifetime (see the DgSlot comment). Internal SRAM untouched.
    s_dgq    = (DgSlot*)heap_caps_malloc(sizeof(DgSlot) * DGQ_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dg_asm = (uint8_t*)heap_caps_malloc(TDL_DGRAM_MAX,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dgq || !s_dg_asm) SLog.println("[tdl] dgram ring alloc failed");
    // 6KB, not 3KB: the deepest chain (parse_byte -> on_frame -> session_up
    // -> SLog.printf + the ATTACH re-announce through Serial/HWCDC) runs
    // printf machinery, and 3072 overflowed into the neighboring heap block
    // — task stacks ARE heap blocks — trampling the GPS task's stack-local
    // File with tdl frame bytes (the 0xa5000101 LoadProhibited in
    // fs::File::position(): _p overwritten with ATTACH-frame bytes
    // A5 01 01 00 A5 at offset 1).
    // Priority 4: above elf_blit(3)/sound(3)/mesh(2), below elf_input(5),
    // tied with usb_mgr(4). At priority 2 this loop — microseconds of work
    // every 5ms — sat under the blit task's ~11.5ms blocking SPI push per
    // frame (~70% of Core 1 during a game) and starved for 0.5-2.5s bursts:
    // acks/keepalives froze, answer latency spiked, leases falsely expired
    // (hw runs 3-4). Same rationale as elf_input's: tiny latency-critical
    // work preempts bulk work whose buffers absorb the jitter.
    xTaskCreatePinnedToCore(tdl_task_body, "tdl_link", 6144,
                            nullptr, 4, &s_tdl_task, 1 /* Core 1 */);
}
