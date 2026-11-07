// Rung 4: storage (plan §6.15 "littlefs/SD") — SD card over SDMMC plus a
// write/readback on the assets flash partition. This rung proves the
// hardware paths; the flash-partition filesystem choice is a decision for
// the firmware port, not here.
//
// SD facts (LilyGo lilygo_device_driver, proven on this board): the card
// sits on the ESP32-P4 default SDMMC pins — SDMMC_SLOT_CONFIG_DEFAULT()
// for this target is CLK43 CMD44 D0-D3 39-42, exactly the board wiring —
// with the default host config (slot 1, no pwr_ctrl handle) raised to
// SDMMC_FREQ_52M. Card power = expander IO15, active low, already driven
// low by the rung-2 power sequence; the shared 3300mV LDO rail is
// acquired by rung 3. Width forced to 4: microSD has 4 data lines, and
// the 8-bit default would route D4-D7 onto GPIO 45-48, which this board
// uses for other functions.
//
// A user card is NEVER formatted (format_if_mount_failed = false) and the
// card is only read; the write test targets the last 4KB of the assets
// partition, which carries no filesystem yet.

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_partition.h"

void rung4_storage(void)
{
    printf("[store] rung 4: SD over SDMMC + assets partition\n");

    printf("[store] stage: sd mount\n");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Slot 0: the card's pins ARE the slot-0 IOMUX set, and slot 1
    // belongs to the C6's SDIO link (rung 7).
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_52M;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot,
                                            &mount_cfg, &card);
    if (err != ESP_OK) {
        printf("[store] sd mount FAILED: %s (no card inserted?)\n",
               esp_err_to_name(err));
    } else {
        sdmmc_card_print_info(stdout, card);
        printf("[store] sd root:\n");
        DIR *d = opendir("/sdcard");
        if (!d) {
            printf("[store] FAIL: opendir /sdcard\n");
        } else {
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) != NULL && n < 12) {
                printf("[store]   %s\n", e->d_name);
                n++;
            }
            closedir(d);
            if (n == 0) printf("[store]   (empty)\n");
        }
    }

    printf("[store] stage: assets partition\n");
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!p) {
        printf("[store] FAIL: assets partition not found\n");
        return;
    }
    printf("[store] assets @ 0x%06lX, %lu KB\n",
           (unsigned long)p->address, (unsigned long)(p->size / 1024));

    const uint32_t off = p->size - 4096;
    uint8_t pat[64], back[64];
    for (int i = 0; i < 64; i++) pat[i] = (uint8_t)(0xA5 ^ i);
    esp_err_t e = esp_partition_erase_range(p, off, 4096);
    if (e == ESP_OK) e = esp_partition_write(p, off, pat, sizeof(pat));
    if (e == ESP_OK) e = esp_partition_read(p, off, back, sizeof(back));
    if (e == ESP_OK && memcmp(pat, back, sizeof(pat)) == 0) {
        printf("[store] assets erase/write/readback PASS\n");
    } else {
        printf("[store] assets RW FAIL: %s%s\n", esp_err_to_name(e),
               (e == ESP_OK) ? " (readback mismatch)" : "");
    }
}
