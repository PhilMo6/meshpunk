#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host function export entry: name → address pair
typedef struct {
    const char* name;
    void*       addr;
} elf_symbol_t;

// Sentinel for symbol table arrays
#define ELF_SYMBOL_END { NULL, NULL }

// Opaque handle for a loaded ELF module
typedef struct elf_module elf_module_t;

// Load an ELF shared object from a memory buffer.
// `data`/`size` — raw ELF file contents (caller frees after this returns).
// `exports`     — NULL-terminated array of host symbols the module may call.
// Returns a handle on success, NULL on failure (reason printed to Serial).
elf_module_t* elf_load(const void* data, size_t size,
                       const elf_symbol_t* exports);

// elf_load with a custom SEGMENT allocator (the module handle itself stays
// on the normal heap). Used by the USB driver loader to place driver
// segments in the boot-reserved low-PSRAM pool; games use plain elf_load.
// `seg_free` is remembered and used by elf_unload. NULL fns = default heap.
typedef void* (*elf_alloc_fn)(size_t size, void* ctx);
typedef void  (*elf_free_fn)(void* ptr, void* ctx);
elf_module_t* elf_load_ex(const void* data, size_t size,
                          const elf_symbol_t* exports,
                          elf_alloc_fn seg_alloc, elf_free_fn seg_free,
                          void* seg_ctx);

// Call the loaded module's entry point (its main()).
// `argc`/`argv` are forwarded as-is.
// Returns the module's return value.
int elf_run(elf_module_t* mod, int argc, char** argv);

// Look up a symbol exported by the loaded module (e.g. a callback).
// Returns the address, or NULL if not found.
void* elf_lookup(elf_module_t* mod, const char* name);
// Code addresses returned instruction-side (cross-module imports call them).
void* elf_lookup_remapped(elf_module_t* mod, const char* name);
// Run the module's C++ static constructors (.ctors reversed + .init_array).
// Explicit by design — see the definition; stack/BLE-proto loads call it,
// game modules keep their historical behavior.
void elf_run_ctors(elf_module_t* mod);
// Import fallback for dependent modules (a BLE protocol elf importing the
// loaded radio stack's symbols): consulted after host exports + self miss.
// Host sets around a dependent load, clears after; NULL disables.
void elf_set_symbol_fallback(void* (*fn)(const char* name));

// Executable range of the loaded module (instruction-side addresses) — for
// crash attribution of PCs to a resident driver module.
void elf_text_range(elf_module_t* mod, uint32_t* start, uint32_t* end);

// Unload a module: frees all memory allocated during elf_load().
void elf_unload(elf_module_t* mod);

#ifdef __cplusplus
}
#endif
