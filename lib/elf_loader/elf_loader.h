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

// Call the loaded module's entry point (its main()).
// `argc`/`argv` are forwarded as-is.
// Returns the module's return value.
int elf_run(elf_module_t* mod, int argc, char** argv);

// Look up a symbol exported by the loaded module (e.g. a callback).
// Returns the address, or NULL if not found.
void* elf_lookup(elf_module_t* mod, const char* name);

// Unload a module: frees all memory allocated during elf_load().
void elf_unload(elf_module_t* mod);

#ifdef __cplusplus
}
#endif
