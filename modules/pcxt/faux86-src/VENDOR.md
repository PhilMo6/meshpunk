# Vendored Faux86-remake

Source: https://github.com/ArnoldUK/Faux86-remake (lineage: Fake86 by Mike Chambers
→ Faux86 by James Howard → remake by ArnoldUK)
Commit: `7eea4787f6b3bb4ed351d58d63b9a81c92338b55` (master, fetched 2026-07-03)
License: GPL-2 (see LICENSE).

Vendored: the complete upstream `src/` (65 files). Not all files are compiled —
see build.ps1 $EXCLUDE for the platform/network pieces we skip.

Runtime assets copied from upstream `data/` into data/lua/apps/Games/PC-XT/:
pcxtbios.bin, videorom.bin, cgachar.bin, asciicga.dat, asciivga.dat, rombasic.bin.
Boot disk images (dosboot.img / fd0.img, 1.44MB each) are NOT packed into
LittleFS (flash budget) — user copies them to S:/dos/ on the SD card.
MOUSE.COM (serial mouse driver) ships inside dosboot.img usage docs upstream.

In-file source edits ARE sanctioned for this module (tag them `MESHPUNK`).
Local modifications — list every in-file edit here as it is made:
- DriveManager.cpp `insertDisk()`: fixed the hard-disk end-cylinder parse
  (`byte7 | ((byte6 & 0xC0) << 2)`, +1 for count). The original folded the
  sector bits into the cylinder, producing garbage geometry for standard MBRs
  and blocking usable C: hard disks. Tagged MESHPUNK.

Build-level adaptation (not source edits): -DARDUINO selects the upstream
embedded path; -include cstddef/cstdlib/cstring/cstdio supplies the libc
basics the ARDUINO path expects from Arduino.h; -Wl,--no-relax works around
a BFD assertion (elf32-xtensa.c:3288/3299, esp-2021r2 binutils) that this
codebase triggers during xtensa linker relaxation. The module's weak
__init_array_start/end refs are ALSO part of the link stability — see
main_tdeck.cpp run_static_ctors comment before touching either.
