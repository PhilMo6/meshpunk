#pragma once

#include <stdint.h>

// ── Raw packet capture (Packets monitor app) ────────────────────────────────
// Protocol-agnostic wire-frame ring, lifted out of PunkMesh so capture works
// under any LoRa protocol. One entry per frame as the protocol's log hooks
// saw it.
//
// Locking (unchanged from the PunkMesh original): every function here runs
// under MESH_LOCK — the hooks via mesh_task_body's wrapper around the
// protocol loop, the start/stop/drain via the Lua bindings that take it — so the ring
// needs no lock of its own. Full ring overwrites the OLDEST entry; a burst
// can never stall the radio loop. Allocated in PSRAM only while armed.
//
// Wire constants are FROZEN copies of the MeshCore values at the lift
// (static_asserts in punkmesh.cpp): the CSV/Lua surface must not drift.
#define PKT_CAP_RING_SIZE  48

#define PKT_CAP_DIR_RX      0
#define PKT_CAP_DIR_TX      1
#define PKT_CAP_DIR_TX_FAIL 2

#define RCAP_HASH_SIZE  8     // == MeshCore MAX_HASH_SIZE at lift time
#define RCAP_FRAME_MAX  255   // == MeshCore MAX_TRANS_UNIT at lift time

struct PktCapture {
  uint32_t seq;        // monotonic; a gap in Lua means the ring dropped
  uint32_t ts;         // device RTC epoch seconds
  uint32_t ms;         // millis(), for sub-second ordering
  int16_t  snr_q4;     // SNR * 4
  int16_t  rssi;
  int16_t  score_q10;  // score * 1000; -1 when the hook had no score
  uint8_t  dir;        // PKT_CAP_DIR_*
  uint8_t  parsed;     // RX: 1 once the protocol confirmed the frame parsed
  uint8_t  len;
  uint8_t  hash[RCAP_HASH_SIZE];   // set when parsed (zero for protocols without one)
  uint8_t  raw[RCAP_FRAME_MAX];
};

namespace rcap {

bool start();                     // arm (allocates the PSRAM ring)
void stop();                      // disarm + free (safe if never armed)
bool armed();

// Claim the next slot, stamped (seq/ts/ms) and zeroed of frame data; the
// caller fills len/raw (and snr/rssi/score/hash where it has them).
// NULL when capture is off.
PktCapture* push(uint8_t dir);
// Most recently pushed entry (NULL when empty) — for a protocol that
// completes an RX entry in a later hook of the same locked section
// (MeshCore logRx).
PktCapture* newest();

// Drain, oldest first: copy into *out and consume. false when empty.
bool     pop_oldest(PktCapture* out);
uint16_t count();
uint32_t take_dropped();          // overwritten-before-read since last call

// One-call surface for protocol modules (MeshHostApi capture_frame): push a
// complete frame with signal info; parsed=1, no score, no hash.
void frame(uint8_t dir, const uint8_t* raw, int len, float snr, float rssi);

}  // namespace rcap
