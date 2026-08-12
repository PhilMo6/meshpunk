#!/usr/bin/env python3
"""Wraps headerless PCM in a WAV header so it can be played by double-click."""
import struct, sys
raw, wav, rate = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = open(raw, "rb").read()
with open(wav, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt ")
    f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
    f.write(b"data" + struct.pack("<I", len(d)) + d)
