"""A very small standard-MIDI writer, with no dependencies.

Enough to author the skyloopers soundtrack: tempo, programme changes, notes,
channel volume and pan. Format 1, one track per part, which is what the OPL
player expects to walk.
"""
import struct


def _varint(n):
    if n < 0:
        raise ValueError("negative delta")
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


DRUMS = 9          # MIDI's percussion channel


class Track:
    """Events are added with absolute times in ticks and sorted on render, so
    parts can be written in whatever order reads best."""

    def __init__(self, name=""):
        self.events = []        # (tick, order, bytes)
        self.name = name
        self._seq = 0

    def _add(self, tick, data, order=1):
        self._seq += 1
        # order keeps note-offs ahead of note-ons at the same tick, so a
        # repeated note retriggers instead of being cut by its predecessor.
        self.events.append((int(tick), order, self._seq, bytes(data)))

    def program(self, tick, ch, prog):
        self._add(tick, [0xC0 | ch, prog & 0x7F], order=0)

    def volume(self, tick, ch, vol):
        self._add(tick, [0xB0 | ch, 7, max(0, min(127, int(vol)))], order=0)

    def pan(self, tick, ch, pan):
        self._add(tick, [0xB0 | ch, 10, max(0, min(127, int(pan)))], order=0)

    def tempo(self, tick, bpm):
        us = int(60_000_000 / bpm)
        self._add(tick, [0xFF, 0x51, 0x03,
                         (us >> 16) & 0xFF, (us >> 8) & 0xFF, us & 0xFF],
                  order=0)

    def note(self, tick, ch, note, dur, vel=100):
        note = int(note)
        if not 0 <= note <= 127:
            raise ValueError("note %d out of range" % note)
        self._add(tick, [0x90 | ch, note, max(1, min(127, int(vel)))], order=1)
        self._add(tick + dur, [0x80 | ch, note, 0], order=0)

    def chord(self, tick, ch, notes, dur, vel=100):
        for n in notes:
            self.note(tick, ch, n, dur, vel)

    def render(self):
        body = bytearray()
        if self.name:
            nm = self.name.encode("ascii", "replace")[:127]
            body += _varint(0) + bytes([0xFF, 0x03, len(nm)]) + nm
        last = 0
        for tick, _order, _seq, data in sorted(self.events,
                                               key=lambda e: (e[0], e[1], e[2])):
            body += _varint(tick - last) + data
            last = tick
        body += _varint(0) + bytes([0xFF, 0x2F, 0x00])
        return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def write(path, tracks, ppq=480):
    data = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), ppq)
    for t in tracks:
        data += t.render()
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


# --- note helpers ---------------------------------------------------------

_NAMES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}


def n(spec):
    """'c4' -> 60, 'f#3' -> 54, 'eb5' -> 75."""
    s = spec.strip().lower()
    v = _NAMES[s[0]]
    i = 1
    while i < len(s) and s[i] in "#b":
        v += 1 if s[i] == "#" else -1
        i += 1
    return v + (int(s[i:]) + 1) * 12


def seq(spec):
    return [n(x) for x in spec.split()]
