/* MESHPUNK: hot-code placement for the DOS module.
 *
 * A function tagged MESHPUNK_HOT is linked into a section named ".iram.text",
 * which the firmware's ELF loader allocates with MALLOC_CAP_EXEC |
 * MALLOC_CAP_INTERNAL and copies into internal SRAM after relocation. Code
 * there is fetched directly rather than through the cache, so it neither
 * misses for itself nor evicts anything else.
 *
 * That second property is the point here. The audio synthesis interleaves with
 * the interpreter on Core 0, and the two have no working set in common: a
 * ~3KB synthesis pass between emulation batches evicts interpreter lines from
 * the 16KB instruction cache that will be wanted again immediately. Placing
 * the synthesis in SRAM removes it from the cache's contents entirely, so it
 * costs the interpreter nothing beyond the cycles it actually runs for.
 *
 * Rules, learned the hard way on the SNES module:
 *   - The file MUST be built with -mtext-section-literals. l32r is PC-relative
 *     and fixed at link time, so the section has to be self-contained; the
 *     default puts literals in .literal, which stays in PSRAM and breaks every
 *     constant load once the code moves.
 *   - Do NOT tag small helpers. An explicit section placement stops GCC
 *     inlining them ANYWHERE, turning a folded-in helper into a real call in
 *     the innermost loop. Give those MESHPUNK_INLINE instead: inlined into a
 *     tagged caller, their code lands in the section anyway.
 *   - Internal SRAM is the scarce pool and is shared with task stacks. The
 *     loader falls back to PSRAM if the allocation fails, so this degrades
 *     rather than breaking.
 */
#ifndef DOS_HOT_H
#define DOS_HOT_H

#ifdef MESHPUNK_HOT_IRAM
#define MESHPUNK_HOT     __attribute__((section(".iram.text")))
#else
#define MESHPUNK_HOT
#endif

#define MESHPUNK_INLINE  inline __attribute__((always_inline))

#endif /* DOS_HOT_H */
