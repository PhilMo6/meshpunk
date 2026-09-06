// LoRa-protocol module memory pool.
//
// Same rationale and mechanics as the USB dynamic-driver pool (usb_pool.cpp):
// a fixed multi_heap reserved ONCE, very early in setup() — before the mesh
// allocation, the theme fonts, the 1MB Lua gap and the Lua arena — so
// .loraproto.elf segments and the protocol's own state live low in PSRAM and
// never fragment the coalescible region the ELF games need. Holds the
// module's code/data segments plus the protocol's own heap via MeshHostApi
// mem_alloc.

#include "proto_pool.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>
#include <LittleFS.h>
#include <SD.h>

#include "radio_hal.h"          // lora_proto_requested (pool sizing)
#include "../meshpunk_sync.h"   // SLog, sd_spi_take

extern bool sd_mounted;           // main.cpp — the card mounted at boot
extern void sd_spi_release();     // main.cpp

// Sized by what THIS boot will load (the pref precedes us; an idle reserve
// is a real tax on the big-game PSRAM ceiling — the skyloopers lesson):
//   meshcore PACKAGE installed+requested: 768KB — measured need ~645KB
//   (242 protocol elf + ~40 companion elf + 162 PunkMesh + 192 ARCHIVE INDEX
//   (the line item the 512 estimate missed) + tables/queues).
//   anything else: 256KB — the months-proven module size (mtlite ~150KB
//   peak) with room for BLE-protocol elfs.
#define PROTO_POOL_KB_PACKAGE 768
#define PROTO_POOL_KB_DEFAULT 256

static multi_heap_handle_t s_pool     = nullptr;
static void*               s_pool_mem = nullptr;

void proto_pool_init(void) {
    if (s_pool) return;
    size_t kb = PROTO_POOL_KB_DEFAULT;
    if (strcmp(lora_proto_requested(), "meshcore") == 0) {
        // The package may live on either drive (the loader looks in the
        // same order: internal first, then the mounted card).
        bool have = LittleFS.exists("/meshpunk/lora_protos/meshcore");
        if (!have && sd_mounted) {
            sd_spi_take();
            have = SD.exists("/meshpunk/lora_protos/meshcore");
            sd_spi_release();
        }
        if (have) kb = PROTO_POOL_KB_PACKAGE;
    }
    s_pool_mem = heap_caps_malloc(kb * 1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pool_mem) {
        SLog.printf("[proto_pool] reserve FAILED (%uKB) — LoRa protocol modules disabled\n",
                    (unsigned)kb);
        return;
    }
    s_pool = multi_heap_register(s_pool_mem, kb * 1024);
    SLog.printf("[proto_pool] %uKB @%p (low PSRAM) for LoRa protocol modules\n",
                (unsigned)kb, s_pool_mem);
}

void* proto_pool_alloc(size_t size) {
    void* p = s_pool ? multi_heap_malloc(s_pool, size) : nullptr;
    if (!p) {
        // An exhausted pool must NEVER be a silent NULL — a starved protocol
        // allocation surfaces as a mystery crash ticks later (the 5f boot
        // loop: the archive index missing from the pool budget).
        SLog.printf("[proto_pool] ALLOC FAILED size=%u free=%u\n",
                    (unsigned)size, (unsigned)proto_pool_free_bytes());
    }
    return p;
}

void proto_pool_free(void* p) {
    if (s_pool && p) multi_heap_free(s_pool, p);
}

size_t proto_pool_free_bytes(void) {
    return s_pool ? multi_heap_free_size(s_pool) : 0;
}
