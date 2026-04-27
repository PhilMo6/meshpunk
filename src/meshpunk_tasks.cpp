// meshpunk_tasks.cpp — Core 1 (radio domain) task bodies.
//
// Core 0 runs Arduino loop() with LVGL + Lua. Core 1 runs the mesh
// dispatcher here. They communicate through:
//   rx_event_queue  : Core 1 -> Core 0  (incoming messages for Lua)
//   tx_cmd_queue    : Core 0 -> Core 1  (reserved; not used yet)
//   gps_event_queue : Core 1 -> Core 0  (reserved; see meshpunk_gps_task)
//
// The mesh task takes MESH_LOCK around the_mesh.loop() so that Lua
// bindings on Core 0 (which also take MESH_LOCK) cannot race against
// dispatcher internals. The bus mutex (SPI_LOCK) is taken inside the
// RadioLib wrappers at the actual SPI transaction sites.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "meshpunk_sync.h"
#include "punkmesh.h"

extern PunkMesh the_mesh;

static TaskHandle_t s_mesh_task_handle = nullptr;

static void mesh_task_body(void *param) {
  Serial.printf("[TASK] mesh_task starting on core=%d\n", xPortGetCoreID());

  for (;;) {
    // MESH_LOCK serializes against Lua bindings on Core 0. Short critical
    // section — dispatcher work is bounded per call.
    MESH_LOCK();
    the_mesh.loop();
    the_mesh.getRTCClock()->tick();
    MESH_UNLOCK();

    // Yield so lower priority tasks (IDLE, watchdog) can run.
    // 2 ms tick keeps radio polling responsive without hogging Core 1.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void meshpunk_spawn_mesh_task() {
  if (s_mesh_task_handle) return;
  // Stack: mesh dispatcher + AES + storage writes fit in 8 KB comfortably;
  // 12 KB leaves slack for deep call chains.
  // Priority 2 keeps it above IDLE (0) and loopTask-equivalents (1) without
  // starving FreeRTOS internals.
  xTaskCreatePinnedToCore(
    mesh_task_body,
    "mesh_task",
    12 * 1024,
    nullptr,
    2,
    &s_mesh_task_handle,
    1 /* pinned to Core 1 */
  );
}

// ── GPS sync task ────────────────────────────────────────────────
// One-shot: polls UART for NMEA until a fix seeds the RTC (or timeout).
// Then idles forever (cheap — FreeRTOS just skips it on the scheduler).
// gps_sync_poll() is defined in main.cpp; it owns the static GPS state.

extern void gps_sync_poll();
extern bool gps_sync_is_done();

static TaskHandle_t s_gps_task_handle = nullptr;

static void gps_task_body(void *param) {
  Serial.printf("[TASK] gps_task starting on core=%d\n", xPortGetCoreID());
  // Fast poll while seeking a fix; go completely idle once done.
  while (!gps_sync_is_done()) {
    gps_sync_poll();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  Serial.println("[TASK] gps_task done — RTC seeded or timed out; task exiting.");
  s_gps_task_handle = nullptr;
  vTaskDelete(nullptr);
}

void meshpunk_spawn_gps_task() {
  if (s_gps_task_handle) return;
  // Small task: NMEA parse + UART reads only. 4 KB is plenty.
  // Priority 1 (below mesh_task).
  xTaskCreatePinnedToCore(
    gps_task_body,
    "gps_task",
    4 * 1024,
    nullptr,
    1,
    &s_gps_task_handle,
    1 /* pinned to Core 1 */
  );
}
