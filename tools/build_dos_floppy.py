"""Add XMS + Sound Blaster support to the FreeDOS boot floppies.

MS-Pac PC (and other DOS games that use digitized sound) needs two things the
stock FreeDOS floppy edition does not provide:

  * an XMS manager, because the game loads ~840KB of samples and refuses to
    start with sound otherwise ("XMS driver not installed")
  * the BLASTER environment variable, since it drives a Sound Blaster Pro
    directly and does not fall back to the PC speaker

tiny386 emulates the card at port 0x220, IRQ 5, DMA 1 (see pc.c's
sb16_new(0x220, 5, ...) and sb16.c's s->dma = 1), which is exactly the
canonical A220 I5 D1.

Writes a NEW image next to the original; the source images are never modified.

    python tools/build_dos_floppy.py <himemx.exe path>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fat12 import Fat12, name83   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FREEDOS = os.path.join(HERE, "..", "freedos")

# Applied to every menu entry, matching how the existing SHELL= line in this
# file is scoped. DEVICE order matters, so it goes ahead of SHELL.
CONFIG_LINE = "123456?DEVICE=\\HIMEMX.EXE\r\n"
BLASTER_LINE = "SET BLASTER=A220 I5 D1\r\n"


def patch(src, dst, himemx):
    f = Fat12(src)
    print("%s: %d free clusters (%d KB)"
          % (os.path.basename(src), len(f.free_clusters()),
             len(f.free_clusters()) * f.clus_size // 1024))

    # ---- 1. the driver itself ----
    f.write_file(name83("HIMEMX.EXE"), himemx)
    print("  + HIMEMX.EXE (%d bytes)" % len(himemx))

    # ---- 2. load it from CONFIG.SYS ----
    cfg = f.read_file(name83("FDCONFIG.SYS")).decode("latin-1")
    if "HIMEMX" in cfg.upper():
        print("  = FDCONFIG.SYS already loads an XMS driver")
    else:
        # Ahead of the SHELL line, which is the last thing the kernel acts on.
        idx = cfg.find("123456?SHELL=")
        if idx < 0:
            raise RuntimeError("no SHELL= line found in FDCONFIG.SYS")
        cfg = cfg[:idx] + CONFIG_LINE + cfg[idx:]
        f.write_file(name83("FDCONFIG.SYS"), cfg.encode("latin-1"))
        print("  + FDCONFIG.SYS: %s" % CONFIG_LINE.strip())

    # ---- 3. the BLASTER variable ----
    auto = f.read_file(name83("FDAUTO.BAT")).decode("latin-1")
    if "BLASTER" in auto.upper():
        print("  = FDAUTO.BAT already sets BLASTER")
    else:
        # Beside the other SET lines so the file still reads sensibly.
        anchor = "SET DOSDIR=%DOSDRV%\\FREEDOS\r\n"
        idx = auto.find(anchor)
        if idx < 0:
            raise RuntimeError("no SET DOSDIR line found in FDAUTO.BAT")
        idx += len(anchor)
        auto = auto[:idx] + BLASTER_LINE + auto[idx:]
        f.write_file(name83("FDAUTO.BAT"), auto.encode("latin-1"))
        print("  + FDAUTO.BAT: %s" % BLASTER_LINE.strip())

    f.save(dst)
    print("  -> %s" % os.path.basename(dst))

    # ---- verify by re-reading the written image ----
    g = Fat12(dst)
    got = g.read_file(name83("HIMEMX.EXE"))
    assert got == himemx, "HIMEMX.EXE did not round-trip"
    assert "HIMEMX" in g.read_file(name83("FDCONFIG.SYS")).decode("latin-1").upper()
    assert "BLASTER" in g.read_file(name83("FDAUTO.BAT")).decode("latin-1").upper()
    for e in g.entries():          # every chain must still be walkable
        if e["clus"]:
            g.chain(e["clus"])
    print("  verified: %d entries, %d free clusters left"
          % (len(g.entries()), len(g.free_clusters())))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    with open(sys.argv[1], "rb") as fh:
        himemx = fh.read()
    if himemx[:2] != b"MZ":
        raise SystemExit("not a DOS executable: %s" % sys.argv[1])

    for base in ("fd40boot", "fdboot"):
        src = os.path.join(FREEDOS, base + ".img")
        if not os.path.exists(src):
            print("skip %s (not found)" % src)
            continue
        patch(src, os.path.join(FREEDOS, base + "-xms.img"), himemx)
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
