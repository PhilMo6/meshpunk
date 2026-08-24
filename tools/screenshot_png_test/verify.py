#!/usr/bin/env python3
"""Verify the firmware PNG writer's output against zlib.

Reads each <name>.png produced by gen.c next to its <name>.raw (big-endian
RGB565 source pixels), and checks:

  * signature, chunk order, and every chunk CRC
  * the zlib stream inflates (this is what catches a fixed-Huffman bit-packing
    bug) and its adler32 matches
  * un-filtering the scanlines reproduces the source pixels exactly, using an
    independent RGB565 -> RGB888 expansion

Exit code is non-zero if any file fails.
"""

import struct
import sys
import zlib
from pathlib import Path

W, H = 320, 240


def expand565(be_lo, be_hi):
    """One big-endian RGB565 pixel -> (r, g, b), bit-replicated like the writer."""
    v = (be_lo << 8) | be_hi          # bytes arrive swapped: undo it
    r5 = (v >> 11) & 0x1F
    g6 = (v >> 5) & 0x3F
    b5 = v & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def chunks(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    off = 8
    while off < len(data):
        (length,) = struct.unpack(">I", data[off:off + 4])
        ctype = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + length]
        (crc,) = struct.unpack(">I", data[off + 8 + length:off + 12 + length])
        want = zlib.crc32(ctype + body) & 0xFFFFFFFF
        assert crc == want, f"{ctype!r} chunk CRC {crc:08x} != {want:08x}"
        yield ctype, body
        off += 12 + length


def check(name):
    png = Path(f"{name}.png").read_bytes()
    raw = Path(f"{name}.raw").read_bytes()
    assert len(raw) == W * H * 2, f"{name}.raw is {len(raw)} bytes"

    idat = b""
    seen = []
    for ctype, body in chunks(png):
        seen.append(ctype)
        if ctype == b"IHDR":
            w, h, depth, color, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            assert (w, h) == (W, H), f"IHDR size {w}x{h}"
            assert (depth, color, comp, filt, inter) == (8, 2, 0, 0, 0), "IHDR fields"
        elif ctype == b"IDAT":
            idat += body
    assert seen[0] == b"IHDR" and seen[-1] == b"IEND", f"chunk order {seen}"
    assert seen.count(b"IDAT") >= 1, "no IDAT"

    # Strict inflate: a wrong Huffman code or a bad adler32 raises here.
    stream = zlib.decompress(idat)
    expect_len = H * (1 + W * 3)
    assert len(stream) == expect_len, f"inflated {len(stream)} want {expect_len}"

    stride = 1 + W * 3
    bad = 0
    for y in range(H):
        row = stream[y * stride:(y + 1) * stride]
        assert row[0] == 1, f"row {y} filter {row[0]} (want 1/Sub)"
        # Reverse Sub: each byte is a delta from the same channel 3 bytes back.
        line = bytearray(row[1:])
        for i in range(3, len(line)):
            line[i] = (line[i] + line[i - 3]) & 0xFF
        for x in range(W):
            src = raw[(y * W + x) * 2:(y * W + x) * 2 + 2]
            want = expand565(src[0], src[1])
            got = tuple(line[x * 3:x * 3 + 3])
            if got != want:
                if bad < 3:
                    print(f"  pixel ({x},{y}) got {got} want {want}")
                bad += 1
    assert bad == 0, f"{bad} pixels differ"

    ratio = 100.0 * len(png) / (W * H * 3)
    print(f"  OK  {name:<10} {len(png):>7} bytes  {ratio:5.1f}% of raw  "
          f"({seen.count(b'IDAT')} IDAT)")
    return True


def main():
    names = sys.argv[1:] or ["flat", "gradient", "noise", "uiish", "extremes"]
    fails = 0
    for n in names:
        try:
            check(n)
        except AssertionError as e:
            print(f"  FAIL {n}: {e}")
            fails += 1
        except Exception as e:                        # zlib errors land here
            print(f"  FAIL {n}: {type(e).__name__}: {e}")
            fails += 1
    print("all PNGs verified" if not fails else f"{fails} file(s) failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
