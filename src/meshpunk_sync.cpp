#include "meshpunk_sync.h"

// NOTE: the 2026-06 PSRAM heap-corruption hunt instrumentation
// (heap_hunt_report + ~13 checkpoints across the mesh/sound/UI paths)
// lived here. Removed after the bug was found and fixed (fake08 api_sfx
// channel==4 OOB — see the psram-corruption-hunt notes). The walks were
// ~12ms each holding the heap allocator lock, which audibly degraded all
// audio — if corruption hunting is ever needed again, rebuild the
// one-shot heap_caps_check_integrity + heap_caps_dump reporter, arm it
// only for targeted captures, and never judge audio/perf with it armed.

SemaphoreHandle_t spi_bus_mutex   = nullptr;
SemaphoreHandle_t the_mesh_mutex  = nullptr;
SemaphoreHandle_t serial_mutex    = nullptr;
QueueHandle_t     rx_event_queue  = nullptr;
QueueHandle_t     tx_cmd_queue    = nullptr;
QueueHandle_t     gps_event_queue = nullptr;

SerialMux SLog;

void meshpunk_sync_init() {
  if (spi_bus_mutex == nullptr) {
    spi_bus_mutex   = xSemaphoreCreateRecursiveMutex();
    the_mesh_mutex  = xSemaphoreCreateRecursiveMutex();
    serial_mutex    = xSemaphoreCreateRecursiveMutex();
    rx_event_queue  = xQueueCreate(32, sizeof(RxEvent));
    tx_cmd_queue    = xQueueCreate(16, sizeof(TxCommand));
    gps_event_queue = xQueueCreate(8,  sizeof(GpsEvent));
  }
}
