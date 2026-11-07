// Meshpunk P4 — bring-up rung 1 (plan §6.15: "serial boot log").
// Proves the toolchain, the flash path, and the hex-mode PSRAM before any
// display/radio/Arduino code exists. Everything prints unprompted; the
// heartbeat shows the firmware is alive after the report.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

// Write/readback sweep over one large PSRAM block: catches a mis-configured
// PSRAM bus (wrong mode/speed reads back garbage) and reports bandwidth.
static void psram_sweep(void) {
  const size_t sz = 8 * 1024 * 1024;
  uint32_t *buf = (uint32_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!buf) {
    printf("[psram] FAIL: could not allocate %u bytes\n", (unsigned)sz);
    return;
  }
  const size_t words = sz / 4;
  int64_t t0 = esp_timer_get_time();
  for (size_t i = 0; i < words; i++) buf[i] = (uint32_t)(i * 2654435761u);
  int64_t t1 = esp_timer_get_time();
  size_t bad = 0;
  for (size_t i = 0; i < words; i++)
    if (buf[i] != (uint32_t)(i * 2654435761u)) bad++;
  int64_t t2 = esp_timer_get_time();
  heap_caps_free(buf);
  double wr_ms = (double)(t1 - t0) / 1000.0;
  double rd_ms = (double)(t2 - t1) / 1000.0;
  printf("[psram] sweep %uMB: %s, write %.1fms (%.0f MB/s), read %.1fms (%.0f MB/s)\n",
         (unsigned)(sz / (1024 * 1024)), bad ? "FAIL" : "PASS",
         wr_ms, (double)sz / 1048576.0 / (wr_ms / 1000.0),
         rd_ms, (double)sz / 1048576.0 / (rd_ms / 1000.0));
  if (bad) printf("[psram] %u bad words\n", (unsigned)bad);
}

void app_main(void) {
  printf("\n=== Meshpunk P4 — bring-up rung 1 ===\n");

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  printf("[chip] ESP32-P4, %d cores, rev v%d.%d\n",
         chip.cores, chip.revision / 100, chip.revision % 100);

  uint32_t flash_size = 0;
  esp_flash_get_size(NULL, &flash_size);
  printf("[flash] %lu MB\n", (unsigned long)(flash_size / (1024 * 1024)));

  size_t psram = esp_psram_get_size();
  printf("[psram] %u MB mapped\n", (unsigned)(psram / (1024 * 1024)));

  printf("[heap] internal: free %u, largest %u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  printf("[heap] psram:    free %u, largest %u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  psram_sweep();

  printf("[heap] psram after sweep: free %u, largest %u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  // Rung 2 (rung2_display.c): panel power via the XL9535, RM69A10 init
  // over DSI, color bars from the DPI frame buffer. Prints its own stages;
  // a failure there leaves the heartbeat below running.
  extern void rung2_display(void);
  rung2_display();

  // Rung 3 (rung3_touch.c): GT9895 probe + firmware info; coordinates are
  // polled in the loop below. Rung 4 (rung4_storage.c): SD card mount +
  // assets partition write/readback.
  extern void rung3_touch_init(void);
  extern void rung3_touch_poll(void);
  extern void rung4_storage(void);
  extern void rung5_radio(void);
  extern void rung6_lora_rx(void);
  extern void rung7c_c6_rom(void);
  extern void rung7_c6(void);
  extern void rung8_wifi(void);
  rung3_touch_init();
  rung4_storage();
  rung5_radio();
  rung6_lora_rx();
  rung7c_c6_rom();
  rung7_c6();
  rung8_wifi();

  int t = 0;
  int tick = 0;
  for (;;) {
    rung3_touch_poll();
    if (++tick >= 50) {
      tick = 0;
      printf("[hb] up %ds\n", t++);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
