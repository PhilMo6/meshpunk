#include "meshpunk_sync.h"

SemaphoreHandle_t spi_bus_mutex   = nullptr;
SemaphoreHandle_t the_mesh_mutex  = nullptr;
QueueHandle_t     rx_event_queue  = nullptr;
QueueHandle_t     tx_cmd_queue    = nullptr;
QueueHandle_t     gps_event_queue = nullptr;

void meshpunk_sync_init() {
  if (spi_bus_mutex == nullptr) {
    spi_bus_mutex   = xSemaphoreCreateRecursiveMutex();
    the_mesh_mutex  = xSemaphoreCreateRecursiveMutex();
    rx_event_queue  = xQueueCreate(32, sizeof(RxEvent));
    tx_cmd_queue    = xQueueCreate(16, sizeof(TxCommand));
    gps_event_queue = xQueueCreate(8,  sizeof(GpsEvent));
  }
}
