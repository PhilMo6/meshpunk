/**
 * @file lv_mem_psram.c
 *
 * PSRAM-backed memory allocator for LVGL.
 *
 * Routes all LVGL widget/style/object allocations to ESP32-S3 PSRAM
 * via heap_caps_malloc(), freeing internal SRAM for BLE/WiFi/DMA.
 *
 * The display draw buffers (buf1, buf2) are separately allocated with
 * ps_malloc() in setupLvgl() -- this file handles everything else
 * LVGL allocates internally (widgets, styles, groups, etc.).
 */

#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <esp_heap_caps.h>
#include <string.h>

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_mem_init(void)
{
    /* PSRAM is initialized by the ESP-IDF startup code; nothing to do. */
}

void lv_mem_deinit(void)
{
    /* Nothing to tear down. */
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;  /* Not supported -- we use the global PSRAM heap. */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void * lv_malloc_core(size_t size)
{
    void * p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}

void * lv_realloc_core(void * p, size_t new_size)
{
    void * np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return np;
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    memset(mon_p, 0, sizeof(lv_mem_monitor_t));

    mon_p->total_size        = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size         = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    if(mon_p->total_size > 0) {
        mon_p->used_pct = 100 - (uint8_t)((uint64_t)100U * mon_p->free_size / mon_p->total_size);
    }

    if(mon_p->free_size > 0) {
        mon_p->frag_pct = 100 - (uint8_t)((uint64_t)100U * mon_p->free_biggest_size / mon_p->free_size);
    }
}

lv_result_t lv_mem_test_core(void)
{
    /* Basic sanity: try a small allocation and free it. */
    void * p = heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(p == NULL) return LV_RESULT_INVALID;
    heap_caps_free(p);
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
