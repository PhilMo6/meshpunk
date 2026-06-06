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

// SD-op convenience wrappers. Today they are just SPI_LOCK/UNLOCK — the
// historical TFT-reinit poke in sd_spi_release() is now redundant because
// the mutex serializes TFT access against SD. Step 8 removes the poke for
// real; for now sd_spi_release() is the mutex-release path (see main.cpp).
inline void sd_spi_take()    { SPI_LOCK();   }
// sd_spi_release() is still declared in main.cpp for legacy callers —
// its body becomes SPI_UNLOCK() + the old TFT recovery (harmless).

// Event structs shuttled across the cores.
struct RxEvent {
  enum Kind : uint8_t { DIRECT_MSG, CHANNEL_MSG, CONTACT_UPDATE } kind;
  uint8_t  hops;
  int8_t   channel_idx;   // -1 for DM
  bool     direct;
  char     sender[32];
  char     text[160];
  uint32_t timestamp;
  float    snr;
  float    rssi;
  uint16_t path_len;
  uint8_t  path[MAX_PATH_SIZE];
  uint8_t  pkt_hash[MAX_HASH_SIZE];
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

// When true, mesh_task pauses its loop body (radio/BLE processing).
// Set by elf_host during module execution to isolate Core 1 activity.
extern volatile bool mesh_task_paused;

#endif
