#pragma once

// Package-side meshpunk_sync: the firmware header's surface, re-expressed
// for the module environment.
//
//  - MESH_LOCK/UNLOCK: the firmware's recursive mesh mutex via the
//    mesh_lock/mesh_unlock exports (same mutex, same recursion semantics —
//    loop() already runs under it, bindings take it themselves).
//  - SPI_LOCK / sd_spi_take: NO-OPS. Every stdio call the module makes
//    resolves to the host's proto_* wrappers, which take the SPI bus lock
//    (and PSRAM-bounce) internally — the Arduino-era per-call ceremony has
//    nothing left to protect here. The radio is behind the host HAL, which
//    locks likewise.
//  - UsbFlashGuard: no-op at this layer (tracked as a host-side stdio
//    concern — see the campaign ledger).
//  - SLog: the shim Serial (line-buffered onto H->log).
//  - FreeRTOS queue surface: real kernel objects via host exports; the
//    RxEvent queue is module-owned (created in the package init).

#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"

// ── Mesh mutex (firmware-owned, recursive) ──────────────────────────────────
extern "C" void mesh_lock(void);
extern "C" void mesh_unlock(void);
#define MESH_LOCK()   mesh_lock()
#define MESH_UNLOCK() mesh_unlock()

// ── SPI bus: locked inside every host stdio/radio call ──────────────────────
#define SPI_LOCK()    do {} while (0)
#define SPI_UNLOCK()  do {} while (0)
inline void sd_spi_take()    {}
inline void sd_spi_release() {}

// ── FreeRTOS (kernel objects are host-side; C API via exports) ──────────────
extern "C" {
typedef void* QueueHandle_t;
typedef int   BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

BaseType_t    xQueueGenericSend(QueueHandle_t q, const void* item,
                                TickType_t wait, BaseType_t pos);
BaseType_t    xQueueReceive(QueueHandle_t q, void* buf, TickType_t wait);
QueueHandle_t xQueueGenericCreate(UBaseType_t len, UBaseType_t item_size,
                                  uint8_t type);
void          vTaskDelay(TickType_t ticks);
}

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define queueSEND_TO_BACK 0
#define queueQUEUE_TYPE_BASE ((uint8_t)0U)
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFuL)
// 1000 Hz tick on this firmware: 1 tick = 1 ms.
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define xQueueSend(q, item, wait) \
    xQueueGenericSend((q), (item), (wait), queueSEND_TO_BACK)
#define xQueueCreate(len, item_size) \
    xQueueGenericCreate((len), (item_size), queueQUEUE_TYPE_BASE)

// Module-owned (created in the package init; the firmware's rx_event_queue
// equivalent lives HERE now — lua_tick drains it).
extern QueueHandle_t rx_event_queue;

// The RX→UI event record, verbatim from the firmware header (the drain side
// and lib/mesh/messages.lua depend on these exact fields).
#include <MeshCore.h>   // MAX_PATH_SIZE / MAX_HASH_SIZE
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

// ── Log (shim Serial: line-buffered onto H->log) ────────────────────────────
#define SLog Serial
