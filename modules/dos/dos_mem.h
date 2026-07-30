/* MESHPUNK: hot-state placement for the DOS module.
 *
 * Force-included into every translation unit by build.ps1, so the vendored
 * device files need a one-word change at their allocation site and nothing else.
 *
 * Why: module allocations normally all go to PSRAM. That is right for anything
 * large, but the emulator has a handful of SMALL structures that are touched on
 * essentially every pass of the main loop -- the CPU state, the VGA registers
 * the retrace state machine walks, the PIC, the PIT, the DMA controller. They
 * sit in the same 32KB data cache that a multi-MB guest RAM is streaming
 * through, so they get evicted constantly and each eviction turns a field access
 * into an 80MHz PSRAM round-trip. Moving CPUI386 alone was a measurable win on
 * hardware, which is what motivated the rest.
 *
 * Internal RAM is the scarce pool (BLE, WiFi, TLS, task stacks). This is ONLY
 * for small, per-iteration state -- never buffers. meshpunk_hot_alloc() falls
 * back to plain malloc when internal RAM is short and logs which it got, so
 * running out degrades speed instead of breaking the module.
 */
#ifndef DOS_MEM_H
#define DOS_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *host_malloc_internal(size_t size);          /* firmware export */
void *meshpunk_hot_alloc(size_t size, const char *what);

#ifdef __cplusplus
}
#endif

#endif /* DOS_MEM_H */
