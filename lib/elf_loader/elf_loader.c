// Minimal ELF loader for ESP32-S3 — loads ET_DYN shared objects into PSRAM.
// Based on patterns from Espressif's elf_loader component (Apache-2.0).

#include "elf_loader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// ELF format definitions (subset needed for our loader)
// ---------------------------------------------------------------------------

#define EI_NIDENT   16
#define ELFMAG      "\177ELF"

// ELF types
#define ET_DYN      3

// Program header types
#define PT_LOAD     1
#define PT_DYNAMIC  2

// Section header types
#define SHT_DYNSYM  11
#define SHT_STRTAB  3

// Dynamic entry tags
#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_JMPREL   23
#define DT_PLTRELSZ 2
#define DT_PLTREL   20
#define DT_RELA_VAL 7   // DT_PLTREL value indicating RELA

// Xtensa dynamic relocation types (from Xtensa ELF ABI)
#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_RTLD       2
#define R_XTENSA_GLOB_DAT   3
#define R_XTENSA_JMP_SLOT   4
#define R_XTENSA_RELATIVE   5

// Segment flags
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word  p_type;
    Elf32_Off   p_offset;
    Elf32_Addr  p_vaddr;
    Elf32_Addr  p_paddr;
    Elf32_Word  p_filesz;
    Elf32_Word  p_memsz;
    Elf32_Word  p_flags;
    Elf32_Word  p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word  st_name;
    Elf32_Addr  st_value;
    Elf32_Word  st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half  st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr  r_offset;
    Elf32_Word  r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

// Section header types and flags
#define SHT_PROGBITS 1
#define SHF_EXECINSTR 0x4

typedef struct {
    Elf32_Word  sh_name;
    Elf32_Word  sh_type;
    Elf32_Word  sh_flags;
    Elf32_Addr  sh_addr;
    Elf32_Off   sh_offset;
    Elf32_Word  sh_size;
    Elf32_Word  sh_link;
    Elf32_Word  sh_info;
    Elf32_Word  sh_addralign;
    Elf32_Word  sh_entsize;
} Elf32_Shdr;

#define ELF32_R_SYM(i)     ((i) >> 8)
#define ELF32_R_TYPE(i)    ((unsigned char)(i))
#define ELF32_ST_BIND(i)   ((i) >> 4)
#define STB_WEAK           2

// ---------------------------------------------------------------------------
// Module handle
// ---------------------------------------------------------------------------

#define MAX_SEGMENTS 4

// Executable section a module can define to have its hot code run from
// internal SRAM instead of PSRAM.
#define HOT_SECTION ".iram.text"

struct elf_module {
    void*       seg_mem[MAX_SEGMENTS]; // allocated memory per PT_LOAD segment
    uint32_t    seg_count;
    uint32_t    base;                  // load bias (data-side: first segment alloc - vaddr)
    Elf32_Addr  entry;                 // final entry point (instruction-side if PSRAM)

    // Code segment bounds (data-side addresses) for address translation
    uint32_t    text_start;            // data-side start of executable segment
    uint32_t    text_end;              // data-side end of executable segment

    // Hot code section (.iram.text), relocated into internal SRAM
    void*       hot_mem;               // internal-RAM copy, NULL if not present
    uint32_t    hot_lo;                // data-side start of the section in PSRAM
    uint32_t    hot_hi;                // data-side end of the section in PSRAM
    int32_t     hot_delta;             // hot_lo-relative addr + delta = SRAM addr

    // Dynamic symbol table (points into loaded segment memory)
    Elf32_Sym*  dynsym;
    const char* dynstr;
    uint32_t    dynsym_count;

    // C++ static-constructor lists (.ctors reversed / .init_array forward),
    // recorded at load (pointers into the loaded segment, entries already
    // relocated data-side). Run only when the caller asks (elf_run_ctors):
    // the radio-stack/BLE-proto loaders do; game modules keep their own
    // historical self-init behavior untouched.
    uint32_t*   ctors;                 // .ctors entries (run in REVERSE)
    uint32_t    ctors_count;
    uint32_t*   init_array;            // .init_array entries (run forward)
    uint32_t    init_array_count;

    // Host export table (borrowed pointer, not owned)
    const elf_symbol_t* exports;

    // Custom segment allocator (elf_load_ex). NULL free_fn = default
    // heap_caps_free. Stored so elf_unload returns the block to the right
    // allocator (e.g. the USB driver pool).
    elf_free_fn seg_free;
    void*       seg_ctx;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* TAG = "elf_loader";

#define LOG_E(fmt, ...) printf("[%s] ERROR: " fmt "\n", TAG, ##__VA_ARGS__)
#define LOG_I(fmt, ...) printf("[%s] " fmt "\n", TAG, ##__VA_ARGS__)

// ESP32-S3 PSRAM address bus translation.
// Data bus:        0x3C000000–0x3DFFFFFF
// Instruction bus: 0x42000000–0x43FFFFFF  (same physical PSRAM)
#define PSRAM_DATA_LOW   0x3C000000
#define PSRAM_DATA_HIGH  0x3E000000
#define PSRAM_D2I_OFFSET 0x06000000  // add to data addr → instruction addr

static inline int is_psram_data_addr(uint32_t addr) {
    return addr >= PSRAM_DATA_LOW && addr < PSRAM_DATA_HIGH;
}

static inline uint32_t psram_data_to_inst(uint32_t data_addr) {
    return data_addr + PSRAM_D2I_OFFSET;
}

static void* elf_alloc(size_t size, bool executable) {
    (void)executable;
    // Allocate from PSRAM. Code execution uses instruction-bus address
    // translation (data addr + 0x06000000 = instruction addr).
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    void* p = heap_caps_malloc(size, caps);
    if (!p) {
        caps = MALLOC_CAP_8BIT;
        p = heap_caps_malloc(size, caps);
    }
    return p;
}

// Forward declaration — defined after module struct
static uint32_t remap_if_text(const elf_module_t* mod, uint32_t addr);

// Cross-module import fallback: a dependent module may import symbols an
// already-loaded module exports (the BLE-slot protocol elf importing the
// radio stack elf's). Consulted only after the host exports and the module
// itself miss; the host sets it around a dependent load and clears it after.
static void* (*s_symbol_fallback)(const char* name) = NULL;

void elf_set_symbol_fallback(void* (*fn)(const char* name)) {
    s_symbol_fallback = fn;
}

static void* resolve_symbol(const elf_module_t* mod,
                             const Elf32_Sym* sym,
                             const char* name) {
    // Check host exports first
    if (mod->exports) {
        for (const elf_symbol_t* e = mod->exports; e->name; e++) {
            if (strcmp(e->name, name) == 0)
                return e->addr;
        }
    }

    // Check if the symbol is defined in the module itself.
    // Remap if it's in .text (could be a function pointer for JMP_SLOT).
    if (sym->st_shndx != 0 && sym->st_value != 0) {
        uint32_t addr = sym->st_value + mod->base;
        return (void*)remap_if_text(mod, addr);
    }

    if (s_symbol_fallback)
        return s_symbol_fallback(name);

    return NULL;
}

// Remap an address: if it falls in the module's code segment (data-side PSRAM),
// convert it to the instruction-side address so the CPU can execute it.
// Addresses inside .iram.text resolve to the internal-SRAM copy instead;
// internal SRAM is one address space, so no instruction-bus offset applies.
static uint32_t remap_if_text(const elf_module_t* mod, uint32_t addr) {
    if (mod->hot_mem && addr >= mod->hot_lo && addr < mod->hot_hi) {
        return (uint32_t)((int32_t)addr + mod->hot_delta);
    }
    if (addr >= mod->text_start && addr < mod->text_end &&
        is_psram_data_addr(addr)) {
        return psram_data_to_inst(addr);
    }
    return addr;
}

// ---------------------------------------------------------------------------
// Relocation processing
// ---------------------------------------------------------------------------

static int process_rela_section(elf_module_t* mod, void* load_base,
                                 Elf32_Addr vaddr_lo,
                                 Elf32_Addr rela_addr, uint32_t rela_size,
                                 uint32_t rela_entsize) {
    if (!rela_addr || !rela_size) return 0;

    uint32_t count = rela_size / rela_entsize;
    Elf32_Rela* rela = (Elf32_Rela*)((uint8_t*)load_base + (rela_addr - vaddr_lo));

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sym_idx = ELF32_R_SYM(rela[i].r_info);
        uint32_t type    = ELF32_R_TYPE(rela[i].r_info);
        uint32_t* target = (uint32_t*)(rela[i].r_offset + mod->base);

        switch (type) {
            case R_XTENSA_NONE:
            case R_XTENSA_RTLD:
                break;

            case R_XTENSA_RELATIVE: {
                // The Xtensa linker stores the pre-relocation virtual address
                // at the target location and sets the RELA addend to 0.
                // Add the load bias to get the data-side address.
                uint32_t val;
                if (rela[i].r_addend != 0) {
                    val = (uint32_t)(rela[i].r_addend + mod->base);
                } else {
                    val = *target + mod->base;
                }
                // If this points into .text, convert to instruction-side
                // so indirect calls (L32R + CALLX) work. Pointers to .rodata
                // stay data-side since they're accessed via load/store.
                *target = remap_if_text(mod, val);
                break;
            }

            case R_XTENSA_GLOB_DAT:
            case R_XTENSA_JMP_SLOT: {
                if (!mod->dynsym || !mod->dynstr) {
                    LOG_E("relocation needs symbol table but none found");
                    return -1;
                }
                Elf32_Sym* sym = &mod->dynsym[sym_idx];
                const char* name = mod->dynstr + sym->st_name;
                void* addr = resolve_symbol(mod, sym, name);
                if (!addr) {
                    // Weak undefined resolves to NULL per the ELF spec
                    // (modules use this for optional __init_array_* refs).
                    if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                        *target = 0;
                        break;
                    }
                    LOG_E("unresolved symbol: %s", name);
                    return -1;
                }
                // The Xtensa linker emits GLOB_DAT with non-zero addends for
                // literals addressing members of a global object (e.g. gnuboy's
                // &GB.ioregs = GB+8). Dropping the addend aliases every such
                // literal to the base symbol — gnuboy's gb_hw_reset then
                // memset()s over its own struct and crashes on the nulled
                // pointers. JMP_SLOT addends are 0 in practice; adding is a
                // no-op there and correct per RELA semantics.
                *target = (uint32_t)addr + rela[i].r_addend;
                break;
            }

            case R_XTENSA_32: {
                if (sym_idx && mod->dynsym && mod->dynstr) {
                    Elf32_Sym* sym = &mod->dynsym[sym_idx];
                    const char* name = mod->dynstr + sym->st_name;
                    void* addr = resolve_symbol(mod, sym, name);
                    if (!addr) {
                        LOG_E("unresolved symbol (R_XTENSA_32): %s", name);
                        return -1;
                    }
                    *target = (uint32_t)addr + rela[i].r_addend;
                } else {
                    *target += mod->base;
                }
                break;
            }

            default:
                LOG_E("unsupported relocation type %u at offset 0x%08x",
                      type, rela[i].r_offset);
                return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

elf_module_t* elf_load(const void* data, size_t size,
                       const elf_symbol_t* exports) {
    return elf_load_ex(data, size, exports, NULL, NULL, NULL);
}

elf_module_t* elf_load_ex(const void* data, size_t size,
                          const elf_symbol_t* exports,
                          elf_alloc_fn seg_alloc, elf_free_fn seg_free,
                          void* seg_ctx) {
    const uint8_t* raw = (const uint8_t*)data;

    // Validate ELF header
    if (size < sizeof(Elf32_Ehdr)) {
        LOG_E("file too small for ELF header");
        return NULL;
    }
    const Elf32_Ehdr* ehdr = (const Elf32_Ehdr*)raw;
    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) {
        LOG_E("not an ELF file");
        return NULL;
    }
    if (ehdr->e_type != ET_DYN) {
        LOG_E("not a shared object (ET_DYN), type=%d", ehdr->e_type);
        return NULL;
    }

    // Allocate module handle
    elf_module_t* mod = (elf_module_t*)calloc(1, sizeof(elf_module_t));
    if (!mod) {
        LOG_E("out of memory for module handle");
        return NULL;
    }
    mod->exports = exports;

    // First pass: find total virtual address range across all PT_LOAD segments
    const Elf32_Phdr* phdr = (const Elf32_Phdr*)(raw + ehdr->e_phoff);
    Elf32_Addr vaddr_lo = UINT32_MAX, vaddr_hi = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_vaddr < vaddr_lo)
                vaddr_lo = phdr[i].p_vaddr;
            Elf32_Addr end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (end > vaddr_hi)
                vaddr_hi = end;
        }
    }
    if (vaddr_lo == UINT32_MAX) {
        LOG_E("no PT_LOAD segments found");
        free(mod);
        return NULL;
    }

    // Allocate a single contiguous block for all segments.
    // This simplifies relocation since relative offsets between segments
    // are preserved from the linker's layout.
    uint32_t total_size = vaddr_hi - vaddr_lo;
    LOG_I("total load size: %u bytes (vaddr 0x%08x..0x%08x)",
          total_size, vaddr_lo, vaddr_hi);

    void* load_base = seg_alloc ? seg_alloc(total_size, seg_ctx)
                                : elf_alloc(total_size, true);
    if (!load_base) {
        LOG_E("failed to allocate %u bytes for segments", total_size);
        free(mod);
        return NULL;
    }
    memset(load_base, 0, total_size); // zero .bss
    mod->seg_mem[0] = load_base;
    mod->seg_count = 1;
    mod->base = (uint32_t)load_base - vaddr_lo;
    mod->seg_free = seg_free;
    mod->seg_ctx  = seg_ctx;

    LOG_I("load base: %p, bias: 0x%08x", load_base, mod->base);

    // Second pass: copy PT_LOAD segment data
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint8_t* dest = (uint8_t*)load_base + (phdr[i].p_vaddr - vaddr_lo);
        if (phdr[i].p_filesz > 0) {
            if (phdr[i].p_offset + phdr[i].p_filesz > size) {
                LOG_E("segment %d extends past file end", i);
                elf_unload(mod);
                return NULL;
            }
            memcpy(dest, raw + phdr[i].p_offset, phdr[i].p_filesz);
        }

        LOG_I("loaded segment %d: vaddr=0x%08x memsz=%u filesz=%u flags=0x%x",
              i, phdr[i].p_vaddr, phdr[i].p_memsz, phdr[i].p_filesz, phdr[i].p_flags);
    }

    // Find .text section bounds from section headers.
    // We need the actual .text section (SHF_EXECINSTR), NOT the whole R+X
    // segment, because .rodata shares the segment but must stay data-side.
    mod->text_start = 0;
    mod->text_end = 0;
    if (ehdr->e_shoff && ehdr->e_shnum) {
        const Elf32_Shdr* shdr = (const Elf32_Shdr*)(raw + ehdr->e_shoff);
        const char* shstr = NULL;
        if (ehdr->e_shstrndx < ehdr->e_shnum)
            shstr = (const char*)(raw + shdr[ehdr->e_shstrndx].sh_offset);
        for (int i = 0; i < ehdr->e_shnum; i++) {
            // C++ static-constructor lists: record their loaded locations
            // (entries are relocated with the segment). elf_run_ctors()
            // executes them on the caller's explicit request.
            if (shstr && shdr[i].sh_size >= 4) {
                const char* nm = shstr + shdr[i].sh_name;
                if (strcmp(nm, ".ctors") == 0) {
                    mod->ctors = (uint32_t*)(shdr[i].sh_addr + mod->base);
                    mod->ctors_count = shdr[i].sh_size / 4;
                } else if (strcmp(nm, ".init_array") == 0) {
                    mod->init_array = (uint32_t*)(shdr[i].sh_addr + mod->base);
                    mod->init_array_count = shdr[i].sh_size / 4;
                }
            }
            if (shdr[i].sh_type == SHT_PROGBITS &&
                (shdr[i].sh_flags & SHF_EXECINSTR)) {
                uint32_t sec_start = shdr[i].sh_addr + mod->base;
                uint32_t sec_end   = sec_start + shdr[i].sh_size;
                // Expand text range to cover all executable sections
                if (mod->text_start == 0 || sec_start < mod->text_start)
                    mod->text_start = sec_start;
                if (sec_end > mod->text_end)
                    mod->text_end = sec_end;

                // The ESP32-S3's 16KB instruction cache fronts PSRAM and
                // flash only and is shared by both cores. Code copied into
                // internal SRAM is fetched directly: it takes no cache
                // misses and evicts nothing the other core is using. A
                // module opts a section in by naming it HOT_SECTION; the
                // section must be self-contained (literals included, which
                // needs -mtext-section-literals) because moving it changes
                // every PC-relative distance to anything outside it.
                if (shstr && !mod->hot_mem && shdr[i].sh_size &&
                    strcmp(shstr + shdr[i].sh_name, HOT_SECTION) == 0) {
                    // Rounded up: the copy below stores whole words, so a
                    // trailing partial word needs a destination.
                    void* hot = heap_caps_malloc((shdr[i].sh_size + 3) & ~3u,
                                                 MALLOC_CAP_EXEC |
                                                 MALLOC_CAP_INTERNAL);
                    if (hot) {
                        mod->hot_mem   = hot;
                        mod->hot_lo    = sec_start;
                        mod->hot_hi    = sec_end;
                        mod->hot_delta = (int32_t)((uint32_t)hot - sec_start);
                        LOG_I("%s: %u bytes -> internal SRAM at %p",
                              HOT_SECTION, shdr[i].sh_size, hot);
                    } else {
                        LOG_I("%s: %u bytes stays in PSRAM (no internal RAM)",
                              HOT_SECTION, shdr[i].sh_size);
                    }
                }
            }
        }
    }
    LOG_I(".text section (data-side): 0x%08x–0x%08x (%u bytes)",
          mod->text_start, mod->text_end, mod->text_end - mod->text_start);

    // Find DYNAMIC segment
    const Elf32_Dyn* dynamic = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic = (const Elf32_Dyn*)((uint8_t*)load_base + (phdr[i].p_vaddr - vaddr_lo));
            break;
        }
    }

    if (!dynamic) {
        LOG_I("no DYNAMIC segment — no relocations needed");
        mod->entry = remap_if_text(mod, ehdr->e_entry + mod->base);
        return mod;
    }

    // Parse DYNAMIC entries
    Elf32_Addr  dt_symtab    = 0;
    Elf32_Addr  dt_strtab    = 0;
    Elf32_Addr  dt_rela      = 0;
    Elf32_Word  dt_relasz    = 0;
    Elf32_Word  dt_relaent   = sizeof(Elf32_Rela);
    Elf32_Addr  dt_jmprel    = 0;
    Elf32_Word  dt_pltrelsz  = 0;

    for (const Elf32_Dyn* d = dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:    dt_symtab   = d->d_un.d_ptr; break;
            case DT_STRTAB:    dt_strtab   = d->d_un.d_ptr; break;
            case DT_RELA:      dt_rela     = d->d_un.d_ptr; break;
            case DT_RELASZ:    dt_relasz   = d->d_un.d_val; break;
            case DT_RELAENT:   dt_relaent  = d->d_un.d_val; break;
            case DT_JMPREL:    dt_jmprel   = d->d_un.d_ptr; break;
            case DT_PLTRELSZ:  dt_pltrelsz = d->d_un.d_val; break;
        }
    }

    // Set up dynamic symbol table pointers (they point into the loaded segment)
    if (dt_symtab) {
        mod->dynsym = (Elf32_Sym*)((uint8_t*)load_base + (dt_symtab - vaddr_lo));
    }
    if (dt_strtab) {
        mod->dynstr = (const char*)((uint8_t*)load_base + (dt_strtab - vaddr_lo));

        // Estimate dynsym count from strtab offset (symtab ends where strtab begins)
        if (dt_symtab && dt_strtab > dt_symtab) {
            mod->dynsym_count = (dt_strtab - dt_symtab) / sizeof(Elf32_Sym);
        }
    }

    // Process relocations
    if (process_rela_section(mod, load_base, vaddr_lo,
                             dt_rela, dt_relasz, dt_relaent) != 0) {
        elf_unload(mod);
        return NULL;
    }
    if (process_rela_section(mod, load_base, vaddr_lo,
                             dt_jmprel, dt_pltrelsz, dt_relaent) != 0) {
        elf_unload(mod);
        return NULL;
    }

    // Copy the hot section after relocation so the SRAM copy carries the
    // patched literals. Calls into it were already pointed here by
    // remap_if_text; the PSRAM original is left in place and unreferenced.
    // Instruction-bus SRAM permits only aligned 32-bit access — a byte or
    // halfword store there raises LoadStoreError — so this copies whole
    // words rather than calling memcpy on the destination.
    if (mod->hot_mem) {
        const uint8_t* src = (const uint8_t*)mod->hot_lo;
        uint32_t* dst      = (uint32_t*)mod->hot_mem;
        uint32_t  sz       = mod->hot_hi - mod->hot_lo;
        uint32_t  words    = sz >> 2;
        for (uint32_t k = 0; k < words; k++) {
            uint32_t w;
            memcpy(&w, src + k * 4, 4);   // source is PSRAM: byte access is fine
            dst[k] = w;
        }
        if (sz & 3) {
            uint32_t w = 0;
            memcpy(&w, src + words * 4, sz & 3);
            dst[words] = w;
        }
    }

    // Flush data cache so instruction cache sees the new code.
    // On ESP32-S3 with PSRAM, this is critical for code execution.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    extern void Cache_WriteBack_All(void);
    Cache_WriteBack_All();
#endif

    mod->entry = remap_if_text(mod, ehdr->e_entry + mod->base);
    LOG_I("entry point: %p (data-side was %p)",
          (void*)mod->entry, (void*)(ehdr->e_entry + mod->base));

    return mod;
}

int elf_run(elf_module_t* mod, int argc, char** argv) {
    if (!mod || !mod->entry) return -1;

    typedef int (*entry_fn_t)(int argc, char** argv);
    entry_fn_t entry = (entry_fn_t)mod->entry;

    LOG_I("calling entry point at %p", (void*)mod->entry);
    return entry(argc, argv);
}

void* elf_lookup(elf_module_t* mod, const char* name) {
    if (!mod || !mod->dynsym || !mod->dynstr || !name) return NULL;

    for (uint32_t i = 0; i < mod->dynsym_count; i++) {
        if (mod->dynsym[i].st_name == 0) continue;
        const char* sym_name = mod->dynstr + mod->dynsym[i].st_name;
        if (strcmp(sym_name, name) == 0 && mod->dynsym[i].st_value != 0) {
            return (void*)(mod->dynsym[i].st_value + mod->base);
        }
    }
    return NULL;
}

// elf_lookup with code addresses returned INSTRUCTION-side: what a
// cross-module import must receive (calls jump straight to it). Data
// symbols pass through unchanged — remap_if_text only touches the code
// segment range.
void* elf_lookup_remapped(elf_module_t* mod, const char* name) {
    void* a = elf_lookup(mod, name);
    return a ? (void*)remap_if_text(mod, (uint32_t)a) : NULL;
}

// Run the module's C++ static constructors. Without this, no crt exists to
// do it and any static object with a vtable keeps a NULL vptr — surfacing
// as a LoadProhibited on the first virtual call (a NULL+slot-offset vtable
// read; the 5f companion boot loop). GNU order: .ctors REVERSED, then
// .init_array forward. Entries are relocated data-side; calls need
// instruction-side. 0 / -1 crt sentinels are skipped defensively (these
// links carry none). Explicit-call design: game modules keep their
// historical no-ctors behavior; the stack/BLE-proto loaders invoke this.
void elf_run_ctors(elf_module_t* mod) {
    if (!mod) return;
    int ran = 0;
    if (mod->ctors) {
        for (int32_t j = (int32_t)mod->ctors_count - 1; j >= 0; j--) {
            uint32_t fp = mod->ctors[j];
            if (fp == 0 || fp == 0xFFFFFFFFu) continue;
            ((void (*)(void))remap_if_text(mod, fp))();
            ran++;
        }
    }
    if (mod->init_array) {
        for (uint32_t j = 0; j < mod->init_array_count; j++) {
            uint32_t fp = mod->init_array[j];
            if (fp == 0 || fp == 0xFFFFFFFFu) continue;
            ((void (*)(void))remap_if_text(mod, fp))();
            ran++;
        }
    }
    if (ran) LOG_I("ran %d static ctor(s)", ran);
}

void elf_text_range(elf_module_t* mod, uint32_t* start, uint32_t* end) {
    if (!mod) { if (start) *start = 0; if (end) *end = 0; return; }
    // Report instruction-side addresses when the segment lives in PSRAM
    // (that's what a fault PC will show).
    uint32_t s = mod->text_start, e = mod->text_end;
    if (is_psram_data_addr(s)) { s = psram_data_to_inst(s); e = psram_data_to_inst(e); }
    if (start) *start = s;
    if (end)   *end   = e;
}

void elf_unload(elf_module_t* mod) {
    if (!mod) return;

    // Always heap_caps_free: the hot section is allocated here, not by the
    // caller's segment allocator.
    if (mod->hot_mem) {
        heap_caps_free(mod->hot_mem);
        mod->hot_mem = NULL;
    }

    for (uint32_t i = 0; i < mod->seg_count; i++) {
        if (mod->seg_mem[i]) {
            if (mod->seg_free) mod->seg_free(mod->seg_mem[i], mod->seg_ctx);
            else               heap_caps_free(mod->seg_mem[i]);
            mod->seg_mem[i] = NULL;
        }
    }
    free(mod);
}
