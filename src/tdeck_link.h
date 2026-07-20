#pragma once

// ── T-Deck ↔ T-Deck peer link ────────────────────────────────────────────────
// A service-multiplexed byte channel between two T-Decks over one USB cable.
// Role A (USB host mode) reaches the peer through the dynamic `tdeck` USB
// driver (link socket in the driver ABI); role B (normal USB device) is
// reached through its native USB-Serial-JTAG CDC — this bridge pumps that
// port on a small Core-1 task. Sessions are automatic: the host side sends
// HELLO whenever its driver attaches; keepalives detect cable loss.
//
// Frame: [0xA5 | svc | cmd | seq | len | payload... | crc16_lo | crc16_hi]
// CRC-16-CCITT over everything before the CRC. The device side's TX is
// shared with SLog logging, so the parser resync-scans and drops anything
// failing magic/length/CRC, and frame TX takes the SLog lock so a frame is
// never interleaved mid-line. (The old 1-byte XOR csum had a fatal blind
// spot: a frame truncated by RX-buffer overflow spliced onto the identical
// header of the next frame XORs itself out — hw runs accepted forged TSYNC
// timestamps that way. CRC-16 has no such self-cancelling structure.)
//
// RELIABILITY — the link is lossy (RX overflow under task starvation, TX
// drop when the CDC buffer is full), which every BGB-style protocol upstream
// assumes never happens (BGB rides TCP). So the bridge supplies the missing
// guarantees at the FRAME level:
//   seq == 0  : fire-and-forget. For superseding/idempotent traffic where
//               only the latest matters (TSYNC, ATTACH keepalive, DETACH,
//               session control). Loss is healed by the next one.
//   seq 1..255: reliable, exactly-once, in-order. Receiver replies
//               ACK [svc, seq] (svc 0), dedups by last-delivered seq;
//               sender keeps ONE frame in flight per service (window-1
//               matches gblink's inherent ping-pong: the master's clock
//               freezes until each answer), retransmits every ~30ms until
//               acked, feeds from a small FIFO so order is preserved.
//               Session up/down resets all reliability state on both sides.
//
// Services: 0 = control (session), 1 = gblink (GameBoy link cable — see the
// gnuboy module). Future: file transfer, remote serial (reliable delivery
// comes free from the seq/ack layer).

#include <stdint.h>
#include <stdbool.h>

// Service ids
#define TDL_SVC_CTRL    0
#define TDL_SVC_GBLINK  1

// Control commands (svc 0)
#define TDL_C_HELLO     1     // payload: [proto_version]
#define TDL_C_PING      2
#define TDL_C_PONG      3
#define TDL_C_BYE       4
#define TDL_C_ACK       5     // payload: [svc, seq] — reliable-frame receipt

// gblink commands (svc 1) — mirrored in the gameboy module's glue.
// The SYNC exchange follows BGB's link protocol: the master sends SYNC1; the
// peer ALWAYS answers — SYNC2 with its byte if it's mid-transfer (armed as
// slave), or SYNC3 "not transferring" if it isn't — so the master never has
// to time out guessing whether a reply is coming.
// gblink is a faithful mapping of the BGB link protocol
// (https://bgb.bircd.org/bgblink.html) onto our framed transport. BGB's b2/b3
// fields ride the payload; BGB's i1 timestamp (2MiHz emulated clocks, 31-bit)
// rides as 4 LE bytes. BGB multiplexes its sync3 on b2 (1 = "got sync1, not
// transferring" ack; 0 = timestamp share) — we give the timestamp form its
// own command id (TSYNC) purely for parser clarity; the semantics are BGB's.
// The lockstep rules live in the gameboy module: emulators exchange TSYNC
// continuously and never run more than a window ahead of the peer's clock; a
// SYNC1 is processed at its timestamp; the master's completion is
// NON-BLOCKING (hardware model: SC bit7 stays set, the CPU keeps running,
// the serial IRQ fires when the SYNC2/SYNC3 answer arrives — the window
// bounds the overrun). Blocking-with-frozen-clock skewed the two timelines
// apart and deadlocked; only the frame-loop window ever stalls real time.
#define TDL_GB_ATTACH   1     // a GameBoy game is running on the sender
#define TDL_GB_DETACH   2
#define TDL_GB_SYNC1    3     // payload: [data, control, ts32] (6 bytes)
#define TDL_GB_SYNC2    4     // payload: [data, 0x80]          (2 bytes)
#define TDL_GB_SYNC3    5     // payload: [1] — got SYNC1, not transferring
#define TDL_GB_TSYNC    6     // payload: [0, ts32] — BGB sync3 timestamp form
#define TDL_GB_RESET    7     // no payload — sender restarted its timestamp
                              // clock (pairing formed/re-formed); receiver
                              // restarts its own. BGB learns "the difference"
                              // once per TCP connection; our transport can
                              // flap one-sidedly, so the re-latch is explicit
                              // and mutual (see the gameboy module's glue).

// Reliability classes (enforced in tdeck_link_gb_send / on_frame):
//   reliable   : SYNC1, SYNC2, SYNC3, RESET — game data + epoch events,
//                exactly-once in-order or the emulators desync.
//   best-effort: TSYNC (next one comes in 16ms), ATTACH (1s keepalive),
//                DETACH (lease expiry is the backstop).

#define TDL_PROTO_VERSION 2   // v2: seq/ack reliability + CRC-16 framing

// Boot init: state + the Core-1 pump task (device-role serial + timers).
void tdeck_link_init();

// USB-host backend, driven by the dynamic `tdeck` driver through the ABI
// link socket (usb_task context). send() must accept a whole frame.
void tdeck_link_usb_register(bool (*send)(const uint8_t* d, uint32_t n));
void tdeck_link_usb_unregister();
void tdeck_link_usb_rx(const uint8_t* d, uint32_t n);

// Status, low 2 bits: 0 = no channel, 1 = peer session up (cable present —
// transfers resolve via the peer's firmware even with no game there), 2 =
// session + remote GameBoy attached (lockstep window applies). Bit 2 (0x4):
// this deck is the USB-host side of the cable (role; only set while a
// session is up). Callers wanting the level compare with (status & 3).
// Safe from any task.
int tdeck_link_status();

// gblink service (wrapped by host_link_* exports in elf_host.cpp; called
// from the ELF module's Core-0 task). send: data_ctrl = (control<<8)|data
// (BGB b3/b2), ts = the sender's 2MiHz emulated clock (SYNC1/TSYNC only).
// ATTACH/DETACH take 0,0 and also track the local-game flag (re-announced
// on session establishment). poll: next incoming gblink event as
// (cmd<<16)|(ctrl<<8)|data with its timestamp in *ts_out, or -1.
bool tdeck_link_gb_send(uint8_t cmd, uint16_t data_ctrl, uint32_t ts);
int  tdeck_link_gb_poll(uint32_t* ts_out);

// Block until a gblink event is delivered or timeout_ms elapses. Called from
// the ELF module's master-wait / lockstep-stall loops (Core 0). Returns 0.
int  tdeck_link_gb_wait(uint32_t timeout_ms);
