#pragma once
// PSRAM allocs route to the boot-reserved protocol pool (which IS PSRAM).
#include <stddef.h>
#include "../mc_internal.h"
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT     0
static inline void* heap_caps_malloc(size_t n, int) { return MCH->mem_alloc(n); }
static inline void* heap_caps_calloc(size_t c, size_t n, int) {
    void* p = MCH->mem_alloc(c * n);
    if (p) __builtin_memset(p, 0, c * n);
    return p;
}
static inline void heap_caps_free(void* p) { if (p) MCH->mem_free(p); }
