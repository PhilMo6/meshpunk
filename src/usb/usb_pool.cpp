// USB dynamic-driver memory pool.
//
// A small fixed multi_heap reserved ONCE at boot, in setup() BEFORE the
// first luaBringUp() — i.e. before the theme fonts, the 1MB Lua gap, and the
// Lua arena stack above it in PSRAM. Driver .drv.elf segments load into this
// pool and are freed back to it on unload, so drivers can come and go at ANY
// session time without ever fragmenting the coalescible region the ELF games
// need ([resident pool][fonts][gap][arena][Map reserve] — see the layout
// comment at luaBringUp in main.cpp).
//
// Fixed-size and unconditional (not scan-based): 96KB of the 8MB PSRAM is
// invisible next to the 1MB gap, and reserving it always means the first
// driver install works without a reboot. Alloc/free run in usb_task only
// (load at attach, unload at detach), but multi_heap carries its own lock
// so a future caller elsewhere wouldn't corrupt it.

#include "../usb_manager.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>

#define USB_DRV_POOL_KB 96

static multi_heap_handle_t s_pool     = nullptr;
static void*               s_pool_mem = nullptr;

void usb_driver_pool_init(void) {
    if (s_pool) return;
    s_pool_mem = heap_caps_malloc(USB_DRV_POOL_KB * 1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pool_mem) {
        printf("[usb_pool] reserve FAILED (%uKB) — dynamic USB drivers disabled\n",
               (unsigned)USB_DRV_POOL_KB);
        return;
    }
    s_pool = multi_heap_register(s_pool_mem, USB_DRV_POOL_KB * 1024);
    printf("[usb_pool] %uKB @%p (below fonts/gap/arena) for dynamic USB drivers\n",
           (unsigned)USB_DRV_POOL_KB, s_pool_mem);
}

void* usb_pool_alloc(size_t size) {
    return s_pool ? multi_heap_malloc(s_pool, size) : nullptr;
}

void usb_pool_free(void* p) {
    if (s_pool && p) multi_heap_free(s_pool, p);
}

size_t usb_pool_free_bytes(void) {
    return s_pool ? multi_heap_free_size(s_pool) : 0;
}
