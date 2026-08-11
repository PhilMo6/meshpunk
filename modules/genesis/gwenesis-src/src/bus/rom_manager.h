/* MESHPUNK: shim for gwenesis's Game & Watch build.
 *
 * The embedded path (GNW_TARGET_*) expects the G&W rom_manager to supply
 * ROM_DATA as a pointer into memory-mapped flash, so load_cartridge() takes
 * no arguments and never copies the ROM. That suits us exactly: the module
 * points ROM_DATA at its own PSRAM buffer, which avoids the desktop path's
 * 8MB static ROM_DATA array and the whole-ROM memcpy that fills it.
 */
#ifndef _MESHPUNK_ROM_MANAGER_H_
#define _MESHPUNK_ROM_MANAGER_H_
extern unsigned char *ROM_DATA;
#endif
