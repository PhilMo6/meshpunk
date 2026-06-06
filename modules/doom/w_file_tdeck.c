// WAD file reader backed by a PSRAM buffer.
// The entire WAD is preloaded into PSRAM at startup by the host,
// so there are no SD card SPI operations during gameplay.

#include "doomgeneric-src/doomgeneric/w_file.h"
#include "doomgeneric-src/doomgeneric/z_zone.h"

#include <string.h>
#include <stdint.h>

// Host functions
extern void* host_read_file(const char* path, uint32_t* out_size);
extern void  host_log(const char* msg);
extern int   printf(const char*, ...);

typedef struct {
    wad_file_t wad;
    uint8_t*   data;
    uint32_t   size;
} mem_wad_file_t;

extern wad_file_class_t stdc_wad_file; // name must match what w_file.c expects

static wad_file_t* W_Mem_OpenFile(char* path) {
    printf("[doom_wad] opening WAD: %s\n", path);

    uint32_t size = 0;
    void* data = host_read_file(path, &size);
    if (!data) {
        printf("[doom_wad] failed to load WAD into PSRAM\n");
        return NULL;
    }
    printf("[doom_wad] WAD loaded: %u bytes in PSRAM\n", size);

    mem_wad_file_t* result = Z_Malloc(sizeof(mem_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = data;   // tell Doom the WAD is memory-mapped
    result->wad.length = size;
    result->data = data;
    result->size = size;

    return &result->wad;
}

static void W_Mem_CloseFile(wad_file_t* wad) {
    // Don't free the PSRAM buffer here — it's freed when the module exits
    // and elf_unload frees all PSRAM.
    Z_Free(wad);
}

static size_t W_Mem_Read(wad_file_t* wad, unsigned int offset,
                          void* buffer, size_t buffer_len) {
    mem_wad_file_t* mem = (mem_wad_file_t*)wad;

    if (offset >= mem->size) return 0;
    if (offset + buffer_len > mem->size)
        buffer_len = mem->size - offset;

    memcpy(buffer, mem->data + offset, buffer_len);
    return buffer_len;
}

// Must be named stdc_wad_file — that's what w_file.c references
wad_file_class_t stdc_wad_file = {
    W_Mem_OpenFile,
    W_Mem_CloseFile,
    W_Mem_Read,
};
