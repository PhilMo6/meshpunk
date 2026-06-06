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

// ---------------------------------------------------------------------------
// Module handle
// ---------------------------------------------------------------------------

#define MAX_SEGMENTS 4

struct elf_module {
    void*       seg_mem[MAX_SEGMENTS]; // allocated memory per PT_LOAD segment
    uint32_t    seg_count;
    uint32_t    base;                  // load bias (data-side: first segment alloc - vaddr)
    Elf32_Addr  entry;                 // final entry point (instruction-side if PSRAM)

    // Code segment bounds (data-side addresses) for address translation
    uint32_t    text_start;            // data-side start of executable segment
    uint32_t    text_end;              // data-side end of executable segment

    // Dynamic symbol table (points into loaded segment memory)
    Elf32_Sym*  dynsym;
    const char* dynstr;
    uint32_t    dynsym_count;

    // Host export table (borrowed pointer, not owned)
    const elf_symbol_t* exports;
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

    return NULL;
}

// Remap an address: if it falls in the module's code segment (data-side PSRAM),
// convert it to the instruction-side address so the CPU can execute it.
static uint32_t remap_if_text(const elf_module_t* mod, uint32_t addr) {
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
                    LOG_E("unresolved symbol: %s", name);
                    return -1;
                }
                *target = (uint32_t)addr;
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

    void* load_base = elf_alloc(total_size, true);
    if (!load_base) {
        LOG_E("failed to allocate %u bytes for segments", total_size);
        free(mod);
        return NULL;
    }
    memset(load_base, 0, total_size); // zero .bss
    mod->seg_mem[0] = load_base;
    mod->seg_count = 1;
    mod->base = (uint32_t)load_base - vaddr_lo;

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
        for (int i = 0; i < ehdr->e_shnum; i++) {
            if (shdr[i].sh_type == SHT_PROGBITS &&
                (shdr[i].sh_flags & SHF_EXECINSTR)) {
                uint32_t sec_start = shdr[i].sh_addr + mod->base;
                uint32_t sec_end   = sec_start + shdr[i].sh_size;
                // Expand text range to cover all executable sections
                if (mod->text_start == 0 || sec_start < mod->text_start)
                    mod->text_start = sec_start;
                if (sec_end > mod->text_end)
                    mod->text_end = sec_end;
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

void elf_unload(elf_module_t* mod) {
    if (!mod) return;

    for (uint32_t i = 0; i < mod->seg_count; i++) {
        if (mod->seg_mem[i]) {
            heap_caps_free(mod->seg_mem[i]);
            mod->seg_mem[i] = NULL;
        }
    }
    free(mod);
}
