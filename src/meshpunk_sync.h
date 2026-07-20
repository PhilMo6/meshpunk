#ifndef MESHPUNK_SYNC_H
#define MESHPUNK_SYNC_H

#include <Arduino.h>
#include <MeshCore.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// Shared SPI bus is used by TFT (LVGL flush), SX1262 radio, and SD card.
// Every SPI transaction on any of those must be bracketed by SPI_LOCK/UNLOCK.
// Recursive because existing call chains nest (e.g. startSendRaw -> idle,
// append_stored_msg -> trim_msg_file).
extern SemaphoreHandle_t spi_bus_mutex;

// Guards PunkMesh logical state (prefs, contacts, channels, send SM) accessed
// from Lua bindings on Core 0 while mesh_task runs on Core 1.
// Nesting order is MESH -> SPI (never the reverse). Also recursive: a few
// Lua send bindings call into methods that themselves take the lock.
extern SemaphoreHandle_t the_mesh_mutex;

// Serializes Serial (UART log) output so prints from Core 0 and Core 1 never
// interleave mid-line. Independent of the SPI bus. Recursive so a multi-call
// log line can be bracketed with SLOG_LOCK()/SLOG_UNLOCK().
extern SemaphoreHandle_t serial_mutex;

// Cross-core event plumbing.
// rx_event_queue : Core 1 (mesh_task) -> Core 0 (UI loop)
// tx_cmd_queue   : Core 0 (UI/Lua)    -> Core 1 (mesh_task)  [reserved for step 7]
// gps_event_queue: Core 1 (gps_task)  -> Core 0 (UI loop)
extern QueueHandle_t rx_event_queue;
extern QueueHandle_t tx_cmd_queue;
extern QueueHandle_t gps_event_queue;

#define SPI_LOCK()    do { if (spi_bus_mutex)  xSemaphoreTakeRecursive(spi_bus_mutex,  portMAX_DELAY); } while (0)
#define SPI_UNLOCK()  do { if (spi_bus_mutex)  xSemaphoreGiveRecursive(spi_bus_mutex);                 } while (0)
#define MESH_LOCK()   do { if (the_mesh_mutex) xSemaphoreTakeRecursive(the_mesh_mutex, portMAX_DELAY); } while (0)
#define MESH_UNLOCK() do { if (the_mesh_mutex) xSemaphoreGiveRecursive(the_mesh_mutex);                } while (0)

// Serial (log) lock. Guarded so it's a harmless no-op before meshpunk_sync_init.
#define SLOG_LOCK()   do { if (serial_mutex) xSemaphoreTakeRecursive(serial_mutex, portMAX_DELAY); } while (0)
#define SLOG_UNLOCK() do { if (serial_mutex) xSemaphoreGiveRecursive(serial_mutex);                } while (0)

// Thread-safe, NON-BLOCKING Serial log wrapper. Use SLog.printf/print/println in
// place of Serial.* so cross-core output never garbles. Mechanics:
//   * Each call is atomic (recursive serial_mutex) — held only for the buffer
//     copy (µs), never across a UART wait, so cross-core contention is µs.
//   * emit() copies into the UART TX buffer ONLY if the whole line fits
//     (availableForWrite); otherwise the line is DROPPED. write() is never
//     called when it would block, so the caller NEVER waits on the UART. The
//     UART ISR drains the (enlarged, see setTxBufferSize) buffer in the
//     background. Tradeoff: sustained logging past the drain rate drops lines
//     rather than stalling the radio/UI.
//   * printf + literal print/println are heap-free (stack format / direct copy);
//     only print(<number>) builds a small temporary.
// Multi-call log lines: render into one printf instead (see the printHex sites).
// While a T-Deck peer-link gblink session is active on the DEVICE role, the
// USB serial *is* the link cable: log lines compete with link frames for the
// CDC TX buffer, and a squeezed-out frame corrupts a GameBoy transfer. The
// bridge (tdeck_link.cpp, which owns/defines this flag) mutes logging for
// the session; frames themselves bypass SerialMux (raw Serial.write under
// SLOG_LOCK) so they are never muted.
extern volatile bool g_slog_quiet;

class SerialMux {
  // Atomic + non-blocking: emit the whole buffer iff it fits, else drop it.
  size_t emit(const uint8_t* p, size_t n) {
    if (!p || n == 0 || g_slog_quiet) return 0;
    size_t w = 0;
    SLOG_LOCK();
    if ((size_t)Serial.availableForWrite() >= n) w = Serial.write(p, n);
    SLOG_UNLOCK();
    return w;
  }
public:
  size_t printf(const char* fmt, ...) {
    char buf[224];
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return 0;
    if (len > (int)sizeof(buf)) len = sizeof(buf);   // over-long line: bounded, no heap
    return emit((const uint8_t*)buf, (size_t)len);
  }
  size_t print(const char* s)        { return emit((const uint8_t*)s, s ? strlen(s) : 0); }
  size_t print(const String& s)      { return emit((const uint8_t*)s.c_str(), s.length()); }
  template <typename... A> size_t print(A... a) { String s(a...); return emit((const uint8_t*)s.c_str(), s.length()); }
  size_t println()                   { return emit((const uint8_t*)"\r\n", 2); }
  size_t println(const char* s) {
    if (g_slog_quiet) return 0;
    size_t sl = s ? strlen(s) : 0;
    size_t w = 0;
    SLOG_LOCK();
    if ((size_t)Serial.availableForWrite() >= sl + 2) {   // line + CRLF as one atomic unit
      if (sl) Serial.write((const uint8_t*)s, sl);
      Serial.write((const uint8_t*)"\r\n", 2);
      w = sl + 2;
    }
    SLOG_UNLOCK();
    return w;
  }
  size_t println(const String& s)    { return println(s.c_str()); }
  template <typename... A> size_t println(A... a) { String s(a...); s += "\r\n"; return emit((const uint8_t*)s.c_str(), s.length()); }
};
extern SerialMux SLog;

// SD-op convenience wrappers. Today they are just SPI_LOCK/UNLOCK — the
// historical TFT-reinit poke in sd_spi_release() is now redundant because
// the mutex serializes TFT access against SD. Step 8 removes the poke for
// real; for now sd_spi_release() is the mutex-release path (see main.cpp).
inline void sd_spi_take()    { SPI_LOCK();   }
// sd_spi_release() is still declared in main.cpp for legacy callers —
// its body becomes SPI_UNLOCK() + the old TFT recovery (harmless).

// Event structs shuttled across the cores.
struct RxEvent {
  enum Kind : uint8_t { DIRECT_MSG, CHANNEL_MSG, CONTACT_UPDATE, ACK,
                        ROOM_MSG, CLI_RESPONSE, LOGIN_RESULT, STATUS_TEXT,
                        SEND_RETRY, CONN_LOST } kind;
  uint8_t  hops;          // LOGIN_RESULT: permissions byte from the server
                          // SEND_RETRY: attempt number now in flight (1-based)
  int8_t   channel_idx;   // -1 for DM; LOGIN_RESULT: 1 = success, 0 = fail
                          // SEND_RETRY: total attempts in the ladder
  bool     direct;
  char     sender[32];    // ROOM_MSG/CLI_RESPONSE/LOGIN_RESULT/STATUS_TEXT: server contact name (thread key)
  char     origin[32];    // ROOM_MSG only: resolved author display name
  char     text[160];
  uint32_t timestamp;
  float    snr;
  float    rssi;
  uint16_t path_len;
  uint8_t  path[MAX_PATH_SIZE];
  uint8_t  pkt_hash[MAX_HASH_SIZE];
  uint32_t ack;           // ACK: the expected-ack CRC this delivery matches
                          // LOGIN_RESULT: keep-alive interval in seconds (0 = none)
  int32_t  rtt;           // ACK: round-trip ms (>=0 delivered, <0 failed/timeout)
};

struct TxCommand {
  enum Kind : uint8_t { SEND_PUBLIC, SEND_DIRECT, SEND_CHANNEL, UPDATE_PREFS } kind;
  uint8_t  channel_idx;
  char     dest[32];
  char     text[160];
};

struct GpsEvent {
  enum Kind : uint8_t { TIME_LOCK, STATS } kind;
  uint32_t epoch;
  int      sats;
  float    hdop;
};

// Must be called once early in setup() before any other subsystem touches
// the mutexes/queues. Safe to call under normal Arduino init order.
void meshpunk_sync_init();

// Spawn the Core 1 mesh task. Call once after the_mesh->begin(), once the
// radio is initialized and Lua is up. Defined in meshpunk_tasks.cpp.
void meshpunk_spawn_mesh_task();

// Spawn the Core 1 GPS task. Defined in meshpunk_tasks.cpp.
void meshpunk_spawn_gps_task();

// Wake the GPS task early from its inter-cycle sleep (manual trigger).
void gps_notify_wake();

// Last-known GPS location (most recent real fix, or the boot seed; persists
// across sync cycles until a new fix replaces it). Returns false if no fix is
// available, leaving lat/lon untouched. Safe to call from the mesh task.
// Defined in main.cpp.
bool meshpunk_gps_last_fix(double* lat, double* lon);

// Apply LoRa settings to the live radio (defined in main.cpp, where the
// radio objects live). Mirrors the reference targets' radio_set_params()/
// radio_set_tx_power(). Safe from either core: the whole sequence runs under
// SPI_LOCK, and RX is re-armed by the dispatcher's next recvRaw().
void radio_apply_params(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
void radio_apply_tx_power(int8_t dbm);

// ── Clock authority tiers ────────────────────────────────────────
// The T-Deck has no battery-backed RTC; the mesh needs time ASAP, but no
// source may stomp a better one. Every clock write routes through
// meshpunk_set_clock() (defined in main.cpp): higher tier wins, equal tier
// is forward-only (GPS-fix and manual always re-apply), lower is rejected.
// A stale tier decays one step per 12h so old authority can't block fresh
// truth forever. See the [CLOCK] log lines.
#define CLOCK_TIER_SEED    0   // boot seeds: last_gps file, contacts bootstrap
#define CLOCK_TIER_VTIME   1   // GPS status-V time (module clock, no fix)
#define CLOCK_TIER_PHONE   2   // companion app via BLE
#define CLOCK_TIER_GPSFIX  3   // GPS time from a real position fix
#define CLOCK_TIER_MANUAL  4   // user typed it
bool meshpunk_set_clock(uint8_t tier, uint32_t epoch, const char* src);

// When true, mesh_task pauses its loop body (radio/BLE processing).
// Two writers, mutually exclusive by construction (no arbitration needed):
//   tdeck_link.cpp  while a GameBoy link session is live (cable session +
//                   local game); cleared on detach/cable-pull/session death.
//   usb_msc_dev.cpp while the SD card is exposed to a PC over USB MSC. A
//                   drive session requires the USB Drive app foreground (no
//                   game running, so no link session), and closing that app
//                   stops the session before anything else can launch.
extern volatile bool mesh_task_paused;

#endif
