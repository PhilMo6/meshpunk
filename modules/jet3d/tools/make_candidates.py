#!/usr/bin/env python3
"""Audition candidates for the skyloopers soundtrack.

Writes several genuinely different options per slot so they can be judged by
ear rather than rebuilt one at a time. These are NOT the shipped tracks --
tools/make_music.py holds those; the winners get copied across.

Every candidate obeys the same hardware rules as the shipped set: no pan
(mono downmix penalises panned channels), <=9 simultaneous notes, leads in
octaves 5-6 so they clear the engine's 300-1200 Hz, and bass written in
octaves 2-3 because the patch supplies its own sub-octave.

    <slot>_<letter>_<description>

All three finish candidates LOOP and run 8+ bars, per the brief.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import midi
from midi import Track, n, seq, DRUMS

Q = 480
BAR = Q * 4
E8, E16 = Q // 2, Q // 4

KICK, RIM, SNARE, TOM_LO = 36, 37, 38, 41
HAT, TOM_MID, OPENHAT, CRASH, RIDE = 42, 45, 46, 49, 51

P_SINE, P_EPIANO, P_BELL, P_GUITAR = 1, 4, 11, 30
P_BASS, P_SQUARE, P_SAW, P_PLUCK, P_PAD = 38, 80, 82, 87, 89
# Lead voices added after Phil pointed out that the same one was in nearly
# every track -- and that 82 in particular sounds like a cheap organ.
P_BRASS, P_GRIT, P_REED = 61, 85, 86

CANDIDATES = []


def candidate(slot, letter, desc, bars, bpm):
    def wrap(fn):
        CANDIDATES.append((slot, letter, desc, bars, bpm, fn))
        return fn
    return wrap


def head(name, bpm, parts):
    t = Track(name)
    t.tempo(0, bpm)
    for ch, prog, vol in parts:
        t.program(0, ch, prog)
        t.volume(0, ch, vol)
    return t


def phrase(t, ch, at, notes, vel=100, gap=26):
    for name, dur in notes:
        t.note(at, ch, n(name) if isinstance(name, str) else name,
               dur - gap, vel)
        at += dur
    return at


# ============================================================ MENU ==========

@candidate("menu", "a", "calm-drift", 8, 88)
def menu_a(t):
    """No drums, no melody. A slow pad with a bell figure drifting over it --
    the least attention-seeking option there is."""
    LEAD, BASS, PAD, BELL = 0, 1, 2, 3
    for ch, prog, vol in ((BASS, P_SINE, 104), (PAD, P_PAD, 96),
                          (BELL, P_BELL, 88)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    prog = [("a2", ["a3", "c4", "e4"]), ("f2", ["f3", "a3", "c4"]),
            ("d2", ["d3", "f3", "a3"]), ("e2", ["e3", "g3", "b3"])]
    for i, (root, chord) in enumerate(prog):
        at = i * 2 * BAR
        t.chord(at, PAD, [n(x) for x in chord], 2 * BAR - E8, 96)
        t.note(at, BASS, n(root), BAR * 2 - Q, 100)
        for k, name in enumerate(chord):
            t.note(at + Q + k * Q, BELL, n(name) + 24, Q - 40, 78 - k * 6)


@candidate("menu", "b", "neon-groove", 8, 112)
def menu_b(t):
    """A proper groove: kick and hat, pulsing bass, arpeggio, short hook. A
    menu in a racing game rather than an ambient screen."""
    LEAD, BASS, PAD, ARP = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 104), (BASS, P_BASS, 116),
                          (PAD, P_PAD, 72), (ARP, P_PLUCK, 94)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    roots = ["a2", "a2", "f2", "g2", "a2", "a2", "c3", "g2"]
    pads = [["a3", "e4"], ["a3", "e4"], ["f3", "c4"], ["g3", "d4"],
            ["a3", "e4"], ["a3", "e4"], ["e3", "c4"], ["g3", "d4"]]
    tri = [["a4", "c5", "e5"], ["a4", "c5", "e5"], ["f4", "a4", "c5"],
           ["g4", "b4", "d5"], ["a4", "c5", "e5"], ["a4", "c5", "e5"],
           ["c5", "e5", "g5"], ["g4", "b4", "d5"]]
    for bar in range(8):
        at = bar * BAR
        r = n(roots[bar])
        for k in range(8):
            t.note(at + k * E8, BASS, r + (12 if k % 4 == 3 else 0),
                   E8 - 30, 112 if k % 2 == 0 else 92)
        t.chord(at, PAD, [n(x) for x in pads[bar]], BAR - E16, 66)
        notes = [n(x) + 12 for x in tri[bar]]
        for k in range(8):
            t.note(at + k * E8, ARP, notes[k % 3], E8 - 34, 88 - (k % 2) * 12)
        for b in (0, 2):
            t.note(at + b * Q, DRUMS, KICK, E16, 100)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 8, 54 if k % 2 == 0 else 40)
        if bar % 4 == 2:
            t.note(at + Q * 3, DRUMS, SNARE, E16, 88)
    phrase(t, LEAD, 4 * BAR,
           [("e5", Q), ("d5", E8), ("c5", E8), ("a4", Q * 2),
            ("c5", Q), ("d5", E8), ("e5", E8), ("a4", Q * 2)], 102)


@candidate("menu", "c", "bright-arp", 8, 104)
def menu_c(t):
    """Major and optimistic, carried by a running arpeggio with a simple hook
    on top. Brighter than anything else on offer."""
    LEAD, BASS, PAD, ARP = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 106), (BASS, P_BASS, 110),
                          (PAD, P_PAD, 70), (ARP, P_PLUCK, 100)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # C - G - Am - F, the friendliest progression there is.
    prog = [("c3", ["e4", "g4", "c5"]), ("g2", ["d4", "g4", "b4"]),
            ("a2", ["e4", "a4", "c5"]), ("f2", ["f4", "a4", "c5"])]
    for i, (root, chord) in enumerate(prog):
        for half in range(2):
            at = (i * 2 + half) * BAR
            r = n(root)
            t.note(at, BASS, r, Q * 2 - 20, 108)
            t.note(at + Q * 2, BASS, r + 7, Q * 2 - 20, 92)
            t.chord(at, PAD, [n(x) for x in chord], BAR - E16, 64)
            notes = [n(x) for x in chord]
            pat = notes + [notes[1]] + [notes[2] + 12]
            for k in range(8):
                t.note(at + k * E8, ARP, pat[k % len(pat)], E8 - 30,
                       96 - (k % 4) * 5)
    phrase(t, LEAD, 2 * BAR,
           [("g4", E8), ("a4", E8), ("c5", Q), ("b4", E8), ("g4", E8),
            ("a4", Q * 2)], 104)
    phrase(t, LEAD, 6 * BAR,
           [("c5", E8), ("b4", E8), ("a4", Q), ("g4", E8), ("e4", E8),
            ("c4", Q * 2)], 100)


@candidate("menu", "d", "dark-hangar", 8, 84)
def menu_d(t):
    """Slow, minor and industrial: a low pulse, a metallic tick, and almost
    nothing else. Reads as a ship sitting in a dark hangar."""
    BASS, PAD, BELL = 1, 2, 3
    for ch, prog, vol in ((BASS, P_BASS, 118), (PAD, P_PAD, 88),
                          (BELL, P_BELL, 76)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    for bar in range(8):
        at = bar * BAR
        root = n(["a2", "a2", "g2", "g2", "f2", "f2", "e2", "e2"][bar])
        for k in range(4):
            t.note(at + k * Q, BASS, root, Q - 40, 116 if k == 0 else 88)
        if bar % 2 == 0:
            t.chord(at, PAD, [root + 12, root + 19], BAR * 2 - E8, 84)
        t.note(at + Q * 2 + E8, DRUMS, RIM, Q // 8, 56)
        if bar % 4 == 3:
            t.note(at + Q * 3, BELL, n("a5"), Q, 72)


# ============================================================ RACE ==========

def _riffvamp(t, lead_prog, lead_vol=112):
    """A static A vamp, pentatonic riff with the flat fifth, sixteenth bass
    with rests in it. Lead voice left as an argument for the same reason as
    the gallop: the arrangement is wanted, the original lead was not."""
    LEAD, BASS, PAD, ARP, GTR = 0, 1, 2, 3, 4
    for ch, prog, vol in ((LEAD, lead_prog, lead_vol), (BASS, P_BASS, 120),
                          (PAD, P_PAD, 54), (ARP, P_PLUCK, 92),
                          (GTR, P_GUITAR, 100)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    PATTERN = [1, 0, 1, 2, 1, 0, 3, 0, 1, 0, 1, 2, 0, 3, 1, 0]
    for bar in range(16):
        at = bar * BAR
        c = "a" if bar < 12 else ["f", "f", "g", "e"][bar - 12]
        r = n({"a": "a2", "f": "f2", "g": "g2", "e": "e2"}[c])
        power = {"a": ["a3", "e4"], "f": ["f3", "c4"],
                 "g": ["g3", "d4"], "e": ["e3", "b3"]}[c]
        for s, kind in enumerate(PATTERN):
            if not kind:
                continue
            t.note(at + s * E16, BASS,
                   r + (0 if kind == 1 else (12 if kind == 2 else 10)),
                   E16 - 22, 112 if s % 4 == 0 else 92)
        t.chord(at, PAD, [n(x) for x in power], BAR - E16, 50)
        for off in (1.5, 2.75, 3.5):
            t.chord(at + int(off * Q), GTR, [n(x) for x in power], Q // 3, 96)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 110)
        t.note(at + int(2.75 * Q), DRUMS, KICK, E16, 92)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 104)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 10, 60 if k % 4 == 0 else 42)
        if bar in (0, 8):
            t.note(at, DRUMS, CRASH, Q, 100)
    hook = [("e5", E8), ("g5", E16), ("e5", E16), ("d5", E8), ("c5", E8),
            ("a4", Q), ("c5", E16), ("d5", E16), ("e5", E8), ("d5", Q)]
    for start in (4, 8, 12):
        phrase(t, LEAD, start * BAR, hook, 106, 20)


@candidate("race", "a", "riff-vamp", 16, 168)
def race_a(t):
    """The original: the "saw" lead, kept for comparison."""
    _riffvamp(t, P_SAW)


@candidate("race", "a1", "riff-guitar", 16, 168)
def race_a1(t):
    """Riff-vamp with the distortion guitar on the hook instead."""
    _riffvamp(t, P_GUITAR, 108)


@candidate("race", "a2", "riff-grit", 16, 168)
def race_a2(t):
    """Riff-vamp with the grit lead: aggressive and vocal."""
    _riffvamp(t, P_GRIT, 106)


@candidate("race", "a3", "riff-reed", 16, 168)
def race_a3(t):
    """Riff-vamp with the reed lead: nasal and cutting, not synthetic."""
    _riffvamp(t, P_REED, 112)


@candidate("race", "b", "flat-out", 16, 178)
def race_b(t):
    """Faster and simpler: straight eighth power chords, a hammering root
    bass, and a riff that barely moves. The most relentless option."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SAW, 114), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 104)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    seq_roots = ["a2"] * 8 + ["c3", "c3", "g2", "g2", "a2", "a2", "e2", "e2"]
    for bar in range(16):
        at = bar * BAR
        r = n(seq_roots[bar])
        for k in range(8):
            t.note(at + k * E8, BASS, r, E8 - 26, 116 if k % 2 == 0 else 96)
        for k in range(8):
            t.chord(at + k * E8, GTR, [r + 12, r + 19], E8 - 40,
                    100 if k % 2 == 0 else 82)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 112)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 106)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 9, 62 if k % 2 == 0 else 46)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 104)
    riff = [("a5", E8), ("a5", E8), ("g5", E8), ("e5", E8),
            ("a5", E8), ("a5", E8), ("c6", E8), ("a5", E8)]
    for bar in range(2, 16):
        if bar % 2 == 0:
            phrase(t, LEAD, bar * BAR, riff, 108, 22)


@candidate("race", "c", "chase-groove", 16, 158)
def race_c(t):
    """Groove rather than riff: a syncopated bass that leaves holes, offbeat
    stabs, and a lead that answers instead of leading."""
    LEAD, BASS, PAD, GTR = 0, 1, 2, 4
    for ch, prog, vol in ((LEAD, P_SQUARE, 108), (BASS, P_BASS, 120),
                          (PAD, P_PAD, 58), (GTR, P_GUITAR, 96)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    HITS = [0, 3, 6, 7, 10, 12, 14]          # sixteenth positions
    cycle = ["a2", "a2", "f2", "g2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cycle[bar % 4])
        for s in HITS:
            t.note(at + s * E16, BASS, r + (12 if s in (6, 12) else 0),
                   E16 - 20, 114 if s == 0 else 94)
        t.chord(at, PAD, [r + 12, r + 19], BAR - E16, 54)
        for off in (1.75, 3.25):
            t.chord(at + int(off * Q), GTR, [r + 12, r + 19], Q // 3, 94)
        for b in (0, 2):
            t.note(at + b * Q, DRUMS, KICK, E16, 110)
        t.note(at + int(1.5 * Q), DRUMS, KICK, E16, 94)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 104)
        for k in range(16):
            if k % 2 == 0 or k % 8 == 3:
                t.note(at + k * E16, DRUMS, HAT, Q // 10,
                       58 if k % 4 == 0 else 42)
        t.note(at + int(2.75 * Q), DRUMS, RIM, Q // 8, 56)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 100)
    answer = [("a4", E16), ("c5", E16), ("d5", E8), ("c5", E8), ("a4", E8),
              ("g4", Q)]
    for bar in range(3, 16, 4):
        phrase(t, LEAD, bar * BAR + Q * 2, answer, 104, 18)


@candidate("race", "d", "phrygian-blitz", 16, 174)
def race_d(t):
    """Darkest and most aggressive: A phrygian, so the flattened second gives
    it a menacing edge, with fast repeated notes on the lead."""
    LEAD, BASS, GTR, ARP = 0, 1, 4, 3
    for ch, prog, vol in ((LEAD, P_SAW, 112), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 102), (ARP, P_PLUCK, 88)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # A - Bb - A - G: the semitone move is the whole character.
    cycle = ["a2", "bb2", "a2", "g2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cycle[bar % 4])
        for s in range(16):
            if s % 4 == 1:
                continue
            t.note(at + s * E16, BASS, r + (12 if s % 8 == 6 else 0),
                   E16 - 22, 114 if s % 4 == 0 else 92)
        for off in (0.0, 1.5, 2.5, 3.5):
            t.chord(at + int(off * Q), GTR, [r + 12, r + 19], Q // 3,
                    102 if off == 0 else 86)
        if bar >= 8:
            for k in range(16):
                t.note(at + k * E16, ARP,
                       r + 24 + [0, 1, 5, 7][k % 4], E16 - 24,
                       86 - (k % 2) * 12)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 112)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 106)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 10, 60 if k % 4 == 0 else 44)
        if bar in (0, 8):
            t.note(at, DRUMS, CRASH, Q, 104)
    # Tremolo lead: the same note hammered, which is what makes it urgent.
    for bar in range(4, 16, 4):
        at = bar * BAR
        for k, name in enumerate(["e5", "e5", "e5", "f5", "e5", "e5",
                                  "d5", "e5"]):
            t.note(at + k * E8, LEAD, n(name), E8 - 22, 108 - (k % 2) * 10)


# ========================================================== FINISH =========
# All three loop, and all three are 8 bars, per the brief.

@candidate("finish", "a", "victory-loop", 8, 132)
def finish_a(t):
    """Straight triumphant major, looping: fanfare in the first four bars,
    an answering phrase in the second four, back round."""
    LEAD, BASS, EP, BELL = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 112), (BASS, P_BASS, 108),
                          (EP, P_EPIANO, 96), (BELL, P_BELL, 92)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    chords = [["c4", "e4", "g4"], ["a3", "c4", "e4"],
              ["f3", "a3", "c4"], ["g3", "b3", "d4"],
              ["c4", "e4", "g4"], ["e4", "g4", "b4"],
              ["f3", "a3", "c4"], ["g3", "b3", "d4"]]
    roots = ["c3", "a2", "f2", "g2", "c3", "e2", "f2", "g2"]
    for bar in range(8):
        at = bar * BAR
        t.chord(at, EP, [n(x) for x in chords[bar]], BAR - E16, 96)
        t.note(at, BASS, n(roots[bar]), BAR - E16, 106)
        t.note(at, DRUMS, KICK, E16, 108)
        t.note(at + Q * 2, DRUMS, SNARE, E16, 96)
        for k in range(4):
            t.note(at + k * Q + E8, DRUMS, HAT, Q // 8, 58)
        if bar % 4 == 0:
            t.note(at, DRUMS, CRASH, Q, 102)
    phrase(t, LEAD, 0,
           [("g4", E8), ("c5", E8), ("e5", Q), ("d5", E8), ("e5", E8),
            ("g5", Q), ("e5", E8), ("d5", E8), ("c5", Q), ("g4", Q * 2),
            ("a4", Q), ("c5", Q)], 108)
    phrase(t, LEAD, 4 * BAR,
           [("e5", E8), ("g5", E8), ("c6", Q), ("b5", E8), ("g5", E8),
            ("e5", Q), ("d5", Q), ("e5", Q), ("g5", Q * 2), ("c5", Q * 2)], 108)
    for k, note in enumerate(seq("c6 e6 g6")):
        t.note(3 * BAR + Q * 2 + k * E16, BELL, note, E8, 92 - k * 6)


@candidate("finish", "b", "podium-groove", 8, 118)
def finish_b(t):
    """Relaxed and pleased with itself rather than fanfare-loud: a light
    groove you can leave running on a results screen."""
    LEAD, BASS, EP, ARP = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 104), (BASS, P_BASS, 112),
                          (EP, P_EPIANO, 94), (ARP, P_PLUCK, 92)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    prog = [("c3", ["e4", "g4", "c5"]), ("a2", ["c4", "e4", "a4"]),
            ("f2", ["a3", "c4", "f4"]), ("g2", ["b3", "d4", "g4"])]
    for bar in range(8):
        at = bar * BAR
        root, chord = prog[bar % 4]
        r = n(root)
        t.note(at, BASS, r, Q * 2 - 24, 110)
        t.note(at + Q * 2, BASS, r + 7, Q - 24, 92)
        t.note(at + Q * 3, BASS, r + 12, Q - 24, 88)
        t.chord(at + E8, EP, [n(x) for x in chord], Q * 2, 92)
        notes = [n(x) + 12 for x in chord]
        for k in range(8):
            t.note(at + k * E8, ARP, notes[k % 3], E8 - 32,
                   88 - (k % 2) * 12)
        for b in (0, 2):
            t.note(at + b * Q, DRUMS, KICK, E16, 102)
        t.note(at + Q, DRUMS, SNARE, E16, 92)
        t.note(at + Q * 3, DRUMS, SNARE, E16, 92)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 8, 52 if k % 2 == 0 else 38)
    phrase(t, LEAD, 2 * BAR,
           [("g4", E8), ("a4", E8), ("c5", Q), ("a4", E8), ("g4", E8),
            ("e4", Q * 2)], 102)
    phrase(t, LEAD, 6 * BAR,
           [("c5", E8), ("d5", E8), ("e5", Q), ("d5", E8), ("c5", E8),
            ("g4", Q * 2)], 102)


@candidate("finish", "c", "fanfare-then-vamp", 8, 126)
def finish_c(t):
    """Two bars of fanfare, then a six-bar vamp that carries the loop. Gives
    the arrival a moment without repeating it every eight bars."""
    LEAD, BASS, EP, GTR = 0, 1, 2, 4
    for ch, prog, vol in ((LEAD, P_SQUARE, 110), (BASS, P_BASS, 110),
                          (EP, P_EPIANO, 92), (GTR, P_GUITAR, 92)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # Bars 1-2: the arrival.
    for bar in (0, 1):
        at = bar * BAR
        ch = ["c4", "e4", "g4", "c5"] if bar == 0 else ["g3", "b3", "d4"]
        t.chord(at, EP, [n(x) for x in ch], BAR - E16, 100)
        t.note(at, BASS, n("c3" if bar == 0 else "g2"), BAR - E16, 108)
        t.note(at, DRUMS, CRASH, Q * 2, 104)
        t.note(at, DRUMS, KICK, E16, 110)
    phrase(t, LEAD, 0,
           [("c5", E8), ("e5", E8), ("g5", Q), ("e5", E8), ("g5", E8),
            ("c6", Q * 2)], 110)
    # Bars 3-8: the vamp.
    vamp = [("a2", ["c4", "e4", "a4"]), ("f2", ["a3", "c4", "f4"]),
            ("c3", ["e4", "g4", "c5"])]
    for i in range(6):
        at = (2 + i) * BAR
        root, chord = vamp[i % 3]
        r = n(root)
        for k in range(4):
            t.note(at + k * Q, BASS, r + (12 if k == 2 else 0), Q - 30,
                   108 if k == 0 else 90)
        for off in (0.5, 2.5):
            t.chord(at + int(off * Q), GTR, [n(x) for x in chord[:2]],
                    Q // 2, 90)
        t.chord(at, EP, [n(x) for x in chord], Q * 2, 88)
        for b in (0, 2):
            t.note(at + b * Q, DRUMS, KICK, E16, 102)
        t.note(at + Q, DRUMS, SNARE, E16, 94)
        t.note(at + Q * 3, DRUMS, SNARE, E16, 94)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 8, 52 if k % 2 == 0 else 38)
    phrase(t, LEAD, 4 * BAR,
           [("a4", E8), ("c5", E8), ("e5", Q), ("d5", Q), ("c5", Q * 2)], 104)


# ================================================== RACE, SECOND BATCH ======
# The first four were all the same animal: minor key, kick on every beat,
# power chords, saw lead. These deliberately change the GROOVE, not just the
# notes -- if three race tracks are going to ship, they have to be different
# from each other, not three shades of one idea.

@candidate("race", "e", "sunset-cruise", 16, 150)
def race_e(t):
    """MAJOR key and uplifting instead of dark: bright arpeggio, syncopated
    bass, snare on 2 and 4. Feels like open road rather than a fight."""
    LEAD, BASS, PAD, ARP = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 108), (BASS, P_BASS, 118),
                          (PAD, P_PAD, 60), (ARP, P_PLUCK, 96)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # E - C#m - A - B, the bright side of the same four-chord idea.
    cyc = [("e2", ["e4", "g#4", "b4"]), ("c#2", ["c#4", "e4", "g#4"]),
           ("a2", ["a3", "c#4", "e4"]), ("b2", ["b3", "d#4", "f#4"])]
    for bar in range(16):
        at = bar * BAR
        root, chord = cyc[bar % 4]
        r = n(root)
        # Bass on the beat and the following sixteenth: a push, not a pound.
        for b in range(4):
            t.note(at + b * Q, BASS, r, E16 - 18, 114)
            t.note(at + b * Q + E16, BASS, r + 12, E16 - 18, 88)
        t.chord(at, PAD, [n(x) for x in chord[:2]], BAR - E16, 56)
        notes = [n(x) + 12 for x in chord]
        for k in range(8):
            t.note(at + k * E8, ARP, notes[k % 3] + (12 if k >= 6 else 0),
                   E8 - 30, 94 - (k % 2) * 10)
        t.note(at, DRUMS, KICK, E16, 108)
        t.note(at + int(2.5 * Q), DRUMS, KICK, E16, 96)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 104)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 9, 58 if k % 2 == 0 else 42)
        if bar % 8 == 7:
            t.note(at + int(3.5 * Q), DRUMS, OPENHAT, E8, 84)
    tune = [("b4", E8), ("c#5", E8), ("e5", Q), ("d#5", E8), ("b4", E8),
            ("c#5", Q * 2), ("a4", E8), ("b4", E8), ("c#5", Q), ("b4", Q * 2)]
    for start in (4, 12):
        phrase(t, LEAD, start * BAR, tune, 106, 24)


@candidate("race", "f", "breakbeat", 16, 164)
def race_f(t):
    """NOT four-on-the-floor. A broken kick-and-snare pattern with a funk
    bass that leaves holes -- the groove carries this, not the riff."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SQUARE, 106), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 94)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # Sixteenth grid. Kicks and snares deliberately off the obvious spots.
    KICKS = [0, 6, 10]
    SNARES = [4, 11, 14]
    BASSHITS = [(0, 0), (3, 0), (6, 12), (8, 0), (11, 10), (14, 12)]
    cyc = ["a2", "a2", "c3", "g2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 4])
        for s, off in BASSHITS:
            t.note(at + s * E16, BASS, r + off, E16 - 18,
                   116 if s == 0 else 94)
        for s in KICKS:
            t.note(at + s * E16, DRUMS, KICK, E16, 110 if s == 0 else 96)
        for s in SNARES:
            t.note(at + s * E16, DRUMS, SNARE, E16, 104 if s == 4 else 88)
        for k in range(16):
            if k % 2 == 1:
                t.note(at + k * E16, DRUMS, HAT, Q // 10, 50)
        t.note(at + 13 * E16, DRUMS, RIM, Q // 8, 58)
        if bar % 4 == 1:
            t.chord(at + 6 * E16, GTR, [r + 12, r + 19], Q // 3, 92)
        if bar % 4 == 3:
            t.chord(at + 10 * E16, GTR, [r + 12, r + 19], Q // 3, 92)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 98)
    lick = [("a4", E16), ("c5", E16), ("d5", E16), ("e5", E8), ("d5", E16),
            ("c5", E8), ("a4", Q)]
    for bar in (4, 6, 12, 14):
        phrase(t, LEAD, bar * BAR + E8, lick, 104, 16)


@candidate("race", "g", "pulse-minimal", 16, 172)
def race_g(t):
    """Hypnotic rather than melodic: two chords, relentless sixteenth hats, an
    off-beat bass pulse and one repeating figure. Almost no tune at all."""
    BASS, ARP, PAD = 1, 3, 2
    for ch, prog, vol in ((BASS, P_BASS, 120), (ARP, P_PLUCK, 100),
                          (PAD, P_PAD, 62)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    for bar in range(16):
        at = bar * BAR
        r = n("a2" if (bar // 4) % 2 == 0 else "f2")
        # Bass on the OFF eighths: that displacement is the whole hook.
        for k in range(8):
            if k % 2 == 1:
                t.note(at + k * E8, BASS, r, E8 - 26, 112)
        t.note(at, BASS, r - 12 if r - 12 > 40 else r, E16 - 16, 100)
        t.chord(at, PAD, [r + 12, r + 19], BAR - E16, 58)
        fig = [0, 7, 12, 7, 15, 12, 7, 12]
        for k in range(8):
            t.note(at + k * E8, ARP, r + 24 + fig[k], E8 - 30,
                   96 - (k % 4) * 8)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 110)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 11,
                   60 if k % 4 == 0 else 40)
        if bar % 4 == 3:
            t.note(at + int(3.5 * Q), DRUMS, OPENHAT, E8, 86)
        if bar % 8 == 4:
            t.note(at + Q * 2, DRUMS, SNARE, E16, 96)


@candidate("race", "h", "half-time-heavy", 16, 172)
def race_h(t):
    """Written fast but FELT slow: the backbeat lands half as often while the
    hats keep sixteenths. Reads as heavy and powerful rather than hurried."""
    LEAD, BASS, GTR, PAD = 0, 1, 4, 2
    for ch, prog, vol in ((LEAD, P_SAW, 110), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 104), (PAD, P_PAD, 56)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    cyc = ["d2", "d2", "bb2", "c3"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 4])
        # Half-time: kick on 1, snare on 3 only.
        t.note(at, DRUMS, KICK, E16, 112)
        t.note(at + int(1.75 * Q), DRUMS, KICK, E16, 92)
        t.note(at + Q * 2, DRUMS, SNARE, E16, 108)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 11,
                   58 if k % 4 == 0 else 40)
        for k in (0, 3, 8, 11):
            t.note(at + k * E16, BASS, r + (12 if k in (3, 11) else 0),
                   E16 * 2 - 22, 116 if k in (0, 8) else 94)
        for off in (0.0, 2.0):
            t.chord(at + int(off * Q), GTR, [r + 12, r + 19], Q, 100)
        t.chord(at, PAD, [r + 12, r + 19], BAR - E16, 54)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q * 2, 104)
    slow = [("d5", Q * 2), ("f5", Q), ("e5", Q), ("d5", Q * 2),
            ("c5", Q * 2), ("bb4", Q * 2), ("d5", Q * 2)]
    for start in (4, 12):
        phrase(t, LEAD, start * BAR, slow, 106, 30)


@candidate("race", "i", "shuffle-rock", 16, 158)
def race_i(t):
    """A SWUNG groove: the second eighth of each beat lands late, on the
    triplet. Nothing else here swings, so this one stands well apart."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SQUARE, 106), (BASS, P_BASS, 120),
                          (GTR, P_GUITAR, 100)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    SWING = (Q * 2) // 3          # the late eighth
    cyc = ["a2", "a2", "d3", "a2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 4])
        for b in range(4):
            base = at + b * Q
            t.note(base, BASS, r, SWING - 20, 114)
            t.note(base + SWING, BASS, r + (7 if b % 2 else 12),
                   (Q - SWING) - 16, 92)
            t.note(base, DRUMS, KICK if b % 2 == 0 else HAT, E16,
                   108 if b % 2 == 0 else 50)
            t.note(base + SWING, DRUMS, HAT, Q // 10, 46)
            if b % 2 == 1:
                t.note(base, DRUMS, SNARE, E16, 104)
        for off in (1, 3):
            t.chord(at + off * Q + SWING, GTR, [r + 12, r + 19], Q // 3, 94)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 100)
        if bar % 4 == 3:
            t.note(at + Q * 3 + SWING, DRUMS, TOM_MID, E16, 88)
    riff = [("a4", 0), ("c5", 1), ("d5", 2), ("c5", 3)]
    for bar in range(4, 16):
        if bar % 2 == 0:
            at = bar * BAR
            for name, b in riff:
                t.note(at + b * Q, LEAD, n(name), SWING - 18, 104)
                t.note(at + b * Q + SWING, LEAD, n(name) + 3,
                       (Q - SWING) - 14, 88)


# ================================================ FINISH, SECOND BATCH =====
# None of the first three landed. These move away from "fanfare" entirely --
# a results screen has to be listenable, and a triumphant phrase repeating
# every eight bars is the fastest way to become tiring.

@candidate("finish", "d", "wind-down", 8, 96)
def finish_d(t):
    """Calm and reflective: the race is over, so this exhales. Pad, gentle
    arpeggio, no drums at all."""
    BASS, PAD, ARP, BELL = 1, 2, 3, 0
    for ch, prog, vol in ((BASS, P_SINE, 106), (PAD, P_PAD, 96),
                          (ARP, P_PLUCK, 88), (BELL, P_BELL, 84)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    prog = [("c3", ["e4", "g4", "c5"]), ("a2", ["c4", "e4", "a4"]),
            ("f2", ["a3", "c4", "f4"]), ("g2", ["b3", "d4", "g4"])]
    for bar in range(8):
        at = bar * BAR
        root, chord = prog[bar % 4]
        t.chord(at, PAD, [n(x) for x in chord], BAR - E8, 94)
        t.note(at, BASS, n(root), BAR - Q, 102)
        notes = [n(x) + 12 for x in chord]
        for k in range(4):
            t.note(at + k * Q, ARP, notes[k % 3], Q - 50, 84 - k * 5)
        if bar % 4 == 3:
            t.note(at + Q * 2, BELL, n("c6"), Q * 2, 82)


@candidate("finish", "e", "funky-strut", 8, 106)
def finish_e(t):
    """Cool rather than triumphant: a bass-led strut with clipped chords.
    Reads as "nicely done" instead of "CONGRATULATIONS"."""
    LEAD, BASS, EP = 0, 1, 2
    for ch, prog, vol in ((LEAD, P_SQUARE, 100), (BASS, P_BASS, 124),
                          (EP, P_EPIANO, 96)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    cyc = ["d3", "d3", "g2", "a2"]
    for bar in range(8):
        at = bar * BAR
        r = n(cyc[bar % 4])
        for s, off in ((0, 0), (3, 0), (6, 10), (8, 12), (11, 0), (14, 10)):
            t.note(at + s * E16, BASS, r + off, E16 - 16,
                   118 if s == 0 else 92)
        for off in (1.5, 3.0):
            t.chord(at + int(off * Q), EP, [r + 15, r + 19, r + 22],
                    Q // 3, 92)
        t.note(at, DRUMS, KICK, E16, 106)
        t.note(at + int(2.5 * Q), DRUMS, KICK, E16, 92)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 100)
        for k in range(16):
            if k % 2 == 1:
                t.note(at + k * E16, DRUMS, HAT, Q // 10, 48)
    lick = [("d5", E16), ("f5", E16), ("g5", E8), ("f5", E16), ("d5", E16),
            ("c5", Q)]
    for bar in (2, 6):
        phrase(t, LEAD, bar * BAR + Q * 2, lick, 100, 14)


@candidate("finish", "f", "minor-heroic", 8, 124)
def finish_f(t):
    """Triumphant WITHOUT the major-key cheese: aeolian with a bright major
    fourth, so it lands as earned rather than as a jingle."""
    LEAD, BASS, EP, GTR = 0, 1, 2, 4
    for ch, prog, vol in ((LEAD, P_SAW, 110), (BASS, P_BASS, 114),
                          (EP, P_EPIANO, 94), (GTR, P_GUITAR, 96)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # Am - F - C - G, but voiced high and played straight: heroic, not happy.
    prog = [("a2", ["a3", "e4", "a4"]), ("f2", ["f3", "c4", "f4"]),
            ("c3", ["c4", "g4", "c5"]), ("g2", ["g3", "d4", "g4"])]
    for bar in range(8):
        at = bar * BAR
        root, chord = prog[bar % 4]
        r = n(root)
        for k in range(4):
            t.note(at + k * Q, BASS, r + (12 if k == 3 else 0), Q - 34,
                   114 if k == 0 else 92)
        t.chord(at, EP, [n(x) for x in chord], Q * 2 - 20, 94)
        for off in (2.0, 3.0):
            t.chord(at + int(off * Q), GTR, [n(chord[0]), n(chord[1])],
                    Q // 2, 96)
        t.note(at, DRUMS, KICK, E16, 110)
        t.note(at + Q * 2, DRUMS, KICK, E16, 96)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 102)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 9, 54 if k % 2 == 0 else 40)
        if bar % 4 == 0:
            t.note(at, DRUMS, CRASH, Q, 102)
    tune = [("a4", Q), ("c5", E8), ("d5", E8), ("e5", Q * 2),
            ("d5", Q), ("c5", Q), ("a4", Q * 2)]
    phrase(t, LEAD, 0, tune, 108, 26)
    tune2 = [("e5", Q), ("g5", E8), ("a5", E8), ("g5", Q * 2),
             ("e5", Q), ("d5", Q), ("a4", Q * 2)]
    phrase(t, LEAD, 4 * BAR, tune2, 108, 26)


@candidate("finish", "g", "synth-outro", 8, 114)
def finish_g(t):
    """No fanfare at all -- an arpeggio and a pad, the way a synthwave track
    fades out. The most background-friendly of the finishes."""
    BASS, PAD, ARP, LEAD = 1, 2, 3, 0
    for ch, prog, vol in ((BASS, P_BASS, 112), (PAD, P_PAD, 84),
                          (ARP, P_PLUCK, 100), (LEAD, P_SQUARE, 92)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    prog = [("f2", ["f3", "a3", "c4"]), ("c3", ["e3", "g3", "c4"]),
            ("g2", ["g3", "b3", "d4"]), ("a2", ["a3", "c4", "e4"])]
    for bar in range(8):
        at = bar * BAR
        root, chord = prog[bar % 4]
        r = n(root)
        for k in range(4):
            t.note(at + k * Q, BASS, r, Q - 40, 110 if k == 0 else 88)
        t.chord(at, PAD, [n(x) for x in chord[:2]], BAR - E16, 80)
        notes = [n(x) + 12 for x in chord]
        for k in range(8):
            t.note(at + k * E8, ARP, notes[k % 3] + (12 if k >= 4 else 0),
                   E8 - 32, 96 - (k % 2) * 12)
        for b in (0, 2):
            t.note(at + b * Q, DRUMS, KICK, E16, 96)
        for k in range(4):
            t.note(at + k * Q + E8, DRUMS, HAT, Q // 8, 46)
    phrase(t, LEAD, 4 * BAR,
           [("c5", Q * 2), ("a4", Q), ("g4", Q), ("f4", Q * 2), ("g4", Q * 2)],
           92, 34)


@candidate("finish", "h", "anthem", 8, 92)
def finish_h(t):
    """Slow, steady and proud. Big chords on the beat, no busy parts -- the
    kind of thing that can sit under a results table indefinitely."""
    LEAD, BASS, EP, BELL = 0, 1, 2, 3
    for ch, prog, vol in ((LEAD, P_SQUARE, 106), (BASS, P_BASS, 116),
                          (EP, P_EPIANO, 98), (BELL, P_BELL, 86)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    prog = [("c3", ["c4", "e4", "g4"]), ("g2", ["b3", "d4", "g4"]),
            ("a2", ["a3", "c4", "e4"]), ("f2", ["f3", "a3", "c4"]),
            ("c3", ["c4", "e4", "g4"]), ("f2", ["f3", "a3", "c4"]),
            ("g2", ["g3", "b3", "d4"]), ("c3", ["c4", "e4", "g4"])]
    for bar in range(8):
        at = bar * BAR
        root, chord = prog[bar]
        r = n(root)
        t.note(at, BASS, r, Q * 2 - 30, 116)
        t.note(at + Q * 2, BASS, r + 7, Q * 2 - 30, 96)
        t.chord(at, EP, [n(x) for x in chord], Q * 2 - 24, 98)
        t.chord(at + Q * 2, EP, [n(x) for x in chord], Q * 2 - 24, 88)
        t.note(at, DRUMS, KICK, E16, 108)
        t.note(at + Q * 2, DRUMS, SNARE, E16, 98)
        if bar % 4 == 0:
            t.note(at, DRUMS, CRASH, Q * 2, 100)
    phrase(t, LEAD, 0,
           [("g4", Q * 2), ("e4", Q), ("g4", Q), ("c5", Q * 3), ("b4", Q)],
           106, 30)
    phrase(t, LEAD, 4 * BAR,
           [("c5", Q * 2), ("a4", Q), ("c5", Q), ("g4", Q * 3), ("c5", Q)],
           106, 30)
    for k, note in enumerate(seq("e6 g6")):
        t.note(7 * BAR + Q * 2 + k * E8, BELL, note, Q, 84 - k * 8)


# =================================================== RACE, THIRD BATCH ======
# Phil kept a (riff-vamp) and i (shuffle-rock) and wants a third. Both keepers
# are RIFF-DRIVEN ROCK, so these stay in that family instead of wandering off
# into electronica again -- but each one changes something structural so they
# do not blur together: the bass WALKS, or the rhythm GALLOPS, or the mode has
# a natural sixth, or the riff leaves holes.

@candidate("race", "j", "boogie-drive", 16, 172)
def race_j(t):
    """Swung like i, but the bass WALKS instead of sitting on the root -- a
    boogie line climbing through the chord. Busier and more forward."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SQUARE, 106), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 100)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    SWING = (Q * 2) // 3
    # A walking figure per chord: root, third, fifth, sixth -- the boogie
    # shape. Written as semitone offsets so it transposes with the chord.
    WALK = [0, 3, 7, 9, 10, 9, 7, 3]
    cyc = ["a2", "a2", "d3", "a2", "a2", "a2", "e3", "d3"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 8])
        for b in range(4):
            base = at + b * Q
            t.note(base, BASS, r + WALK[(b * 2) % 8], SWING - 18, 114)
            t.note(base + SWING, BASS, r + WALK[(b * 2 + 1) % 8],
                   (Q - SWING) - 14, 92)
            t.note(base, DRUMS, KICK if b % 2 == 0 else HAT, E16,
                   108 if b % 2 == 0 else 48)
            t.note(base + SWING, DRUMS, HAT, Q // 10, 44)
            if b % 2 == 1:
                t.note(base, DRUMS, SNARE, E16, 104)
        for off in (1, 3):
            t.chord(at + off * Q, GTR, [r + 12, r + 19], SWING - 30, 92)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 100)
    for bar in range(4, 16, 4):
        at = bar * BAR
        for k, off in enumerate((0, 3, 5, 7, 5, 3)):
            t.note(at + k * SWING, LEAD, n("a4") + off + 12,
                   SWING - 20, 106 - (k % 2) * 8)


@candidate("race", "k", "surf-rock", 16, 166)
def race_k(t):
    """Driving sixteenth palm-mute chug with a tremolo-picked lead -- surf, and
    surf has always sounded like going fast. Straight, not swung."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SAW, 112), (BASS, P_BASS, 120),
                          (GTR, P_GUITAR, 98)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    cyc = ["e2", "e2", "a2", "e2", "e2", "g2", "a2", "b2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 8])
        # Chugging sixteenths on the root: the palm-mute engine of the thing.
        for s in range(16):
            t.note(at + s * E16, BASS, r, E16 - 26,
                   112 if s % 4 == 0 else (94 if s % 2 == 0 else 78))
        for off in (0.0, 2.0):
            t.chord(at + int(off * Q), GTR, [r + 12, r + 19], Q // 2, 94)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 108)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 106)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 11, 56 if k % 4 == 0 else 40)
        if bar % 8 == 7:
            for k in range(4):
                t.note(at + Q * 3 + k * E16, DRUMS, TOM_MID, Q // 8, 84 + k * 5)
    # Tremolo lead: every note repeated in sixteenths, which is the surf sound.
    LINE = ["e5", "e5", "g5", "g5", "a5", "a5", "g5", "e5"]
    for bar in range(4, 16, 2):
        at = bar * BAR
        for k, name in enumerate(LINE):
            for r2 in range(2):
                t.note(at + k * E8 + r2 * E16, LEAD, n(name), E16 - 22,
                       106 - r2 * 12)


def _gallop(t, lead_prog, lead_vol=112, lead_mode="tune"):
    """The gallop arrangement. Both the lead VOICE and what the lead PLAYS are
    arguments, because the original lead was the problem twice over: a
    keyboardish patch, and a sparse tune that referred to nothing else in the
    piece. lead_mode is "tune" (the original), "none", or "riff" -- riff
    doubles the gallop itself two octaves up, so the lead is part of the
    groove rather than a visitor to it."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, lead_prog, lead_vol), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 104)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # Gallop: eighth, sixteenth, sixteenth -- positions 0,2,3 of each beat.
    GALLOP = (0, 2, 3)
    cyc = ["d2", "d2", "d2", "bb2", "c3", "c3", "a2", "a2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 8])
        for b in range(4):
            for s in GALLOP:
                t.note(at + b * Q + s * E16, BASS, r, E16 - 24,
                       114 if s == 0 else 92)
                t.chord(at + b * Q + s * E16, GTR, [r + 12, r + 19],
                        E16 - 30, 96 if s == 0 else 76)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 112)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 106)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 9, 58 if k % 2 == 0 else 42)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 104)
    if lead_mode == "tune":
        # D harmonic minor: the raised seventh is the drama.
        line = [("d5", E8), ("e5", E16), ("f5", E16), ("g5", E8), ("a5", E8),
                ("bb5", Q), ("a5", E8), ("g5", E8), ("f5", E8), ("e5", E8),
                ("d5", Q)]
        for start in (4, 12):
            phrase(t, LEAD, start * BAR, line, 108, 22)
    elif lead_mode == "riff":
        # The gallop, doubled two octaves up, alternating root and fifth. Same
        # rhythm as the bass and guitar, so it locks in instead of floating.
        for bar in range(4, 16):
            at = bar * BAR
            r = n(cyc[bar % 8])
            for b in range(4):
                for k, sixteenth in enumerate(GALLOP):
                    t.note(at + b * Q + sixteenth * E16, LEAD,
                           r + 24 + (7 if k == 2 else 0), E16 - 26,
                           104 if k == 0 else 86)


@candidate("race", "l1", "gallop-guitar", 16, 170)
def race_l1(t):
    """Gallop with the DISTORTION GUITAR carrying the lead as well as the
    chords -- one voice for the whole guitar part, which is how a real band
    would do it."""
    _gallop(t, P_GUITAR, 108)


@candidate("race", "l2", "gallop-brass", 16, 170)
def race_l2(t):
    """Gallop with a BRASS lead: the note blooms rather than arriving square,
    so the line reads as a horn section over the riff."""
    _gallop(t, P_BRASS, 114)


@candidate("race", "l3", "gallop-grit", 16, 170)
def race_l3(t):
    """Gallop with the GRIT lead -- derived-square carrier at full feedback.
    Aggressive and vocal, and about as far from an organ as two operators get."""
    _gallop(t, P_GRIT, 106)


@candidate("race", "l4", "gallop-reed", 16, 170)
def race_l4(t):
    """Gallop with a REED lead: an odd 3:1 ratio and a half-sine carrier, so
    it is nasal in a woodwind way rather than a synth way. Cuts through
    without being shrill."""
    _gallop(t, P_REED, 112)


@candidate("race", "l5", "gallop-noLead", 16, 170)
def race_l5(t):
    """The gallop with NO lead at all -- riff, chords and drums. The riff is
    the tune, which is how most driving rock actually works."""
    _gallop(t, P_GUITAR, 108, lead_mode="none")


@candidate("race", "l6", "gallop-riffLead", 16, 170)
def race_l6(t):
    """The gallop with the lead DOUBLING the gallop two octaves up, all the
    way through. Integrated with the groove rather than a separate tune."""
    _gallop(t, P_GRIT, 104, lead_mode="riff")


def _dorian(t, lead_mode="tune"):
    """Same riff-over-vamp shape as a, but DORIAN -- the natural sixth lifts it
    out of plain minor without turning it major. lead_mode as for the gallop."""
    LEAD, BASS, GTR, ARP = 0, 1, 4, 3
    for ch, prog, vol in ((LEAD, P_SAW, 112), (BASS, P_BASS, 120),
                          (GTR, P_GUITAR, 98), (ARP, P_PLUCK, 90)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # A dorian: the IV is MAJOR, which is the whole colour.
    cyc = [("a2", ["a3", "e4"]), ("a2", ["a3", "e4"]),
           ("d3", ["d4", "a4"]), ("g2", ["g3", "d4"])]
    PATTERN = [1, 0, 1, 2, 0, 1, 0, 2, 1, 0, 1, 2, 0, 1, 2, 0]
    for bar in range(16):
        at = bar * BAR
        root, power = cyc[bar % 4]
        r = n(root)
        for s, kind in enumerate(PATTERN):
            if not kind:
                continue
            t.note(at + s * E16, BASS, r + (0 if kind == 1 else 12),
                   E16 - 22, 112 if s % 4 == 0 else 92)
        for off in (1.5, 3.5):
            t.chord(at + int(off * Q), GTR, [n(x) for x in power], Q // 3, 94)
        if bar >= 8:
            for k in range(8):
                t.note(at + k * E8, ARP, n(power[0]) + 12 + [0, 4, 7, 9][k % 4],
                       E8 - 28, 88 - (k % 2) * 10)
        for b in range(4):
            t.note(at + b * Q, DRUMS, KICK, E16, 110)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 104)
        for k in range(16):
            t.note(at + k * E16, DRUMS, HAT, Q // 10, 58 if k % 4 == 0 else 42)
        if bar in (0, 8):
            t.note(at, DRUMS, CRASH, Q, 100)
    if lead_mode == "tune":
        hook = [("a4", E8), ("b4", E16), ("c5", E16), ("e5", E8), ("f#5", E8),
                ("e5", Q), ("d5", E8), ("c5", E8), ("b4", Q)]
        for start in (2, 6, 10, 14):
            phrase(t, LEAD, start * BAR, hook, 108, 20)
    elif lead_mode == "riff":
        # The bass pattern, two octaves up, on the same sixteenths.
        for bar in range(16):
            at = bar * BAR
            root, _power = cyc[bar % 4]
            r = n(root)
            for sx, kind in enumerate(PATTERN):
                if not kind:
                    continue
                t.note(at + sx * E16, LEAD, r + 24 + (0 if kind == 1 else 7),
                       E16 - 26, 102 if sx % 4 == 0 else 84)


@candidate("race", "m", "dorian-hook", 16, 162)
def race_m(t):
    """The original, with the sparse hook."""
    _dorian(t, "tune")


@candidate("race", "m1", "dorian-noLead", 16, 162)
def race_m1(t):
    """Dorian riff with no lead line at all."""
    _dorian(t, "none")


@candidate("race", "m2", "dorian-riffLead", 16, 162)
def race_m2(t):
    """Dorian with the lead doubling the bass riff two octaves up."""
    _dorian(t, "riff")


def _stomp(t, lead_mode="tune"):
    """A big riff with HOLES in it. Slower than the others and heavier for it:
    the gaps make the hits land. lead_mode as for the gallop."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_SAW, 110), (BASS, P_BASS, 124),
                          (GTR, P_GUITAR, 106)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    # The riff: two bars, and the second bar answers the first.
    RIFF_A = [(0, 0), (3, 0), (6, 3), (8, 5)]
    RIFF_B = [(0, 0), (3, 0), (6, 3), (10, 5), (12, 3), (14, 0)]
    cyc = ["e2", "e2", "e2", "e2", "g2", "g2", "a2", "a2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar % 8])
        riff = RIFF_A if bar % 2 == 0 else RIFF_B
        for s, off in riff:
            t.note(at + s * E16, BASS, r + off, E16 * 2 - 24,
                   120 if s == 0 else 98)
            t.chord(at + s * E16, GTR, [r + off + 12, r + off + 19],
                    E16 * 2 - 30, 104 if s == 0 else 84)
        t.note(at, DRUMS, KICK, E16, 112)
        t.note(at + 6 * E16, DRUMS, KICK, E16, 98)
        for b in (1, 3):
            t.note(at + b * Q, DRUMS, SNARE, E16, 108)
        for k in range(8):
            t.note(at + k * E8, DRUMS, HAT, Q // 9, 56 if k % 2 == 0 else 40)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q * 2, 104)
        if bar % 8 == 7:
            t.note(at + Q * 3, DRUMS, OPENHAT, E8, 86)
    if lead_mode == "tune":
        line = [("e5", Q), ("g5", E8), ("e5", E8), ("d5", Q), ("b4", Q),
                ("e5", E8), ("g5", E8), ("a5", Q * 2)]
        for start in (8, 12):
            phrase(t, LEAD, start * BAR, line, 106, 24)
    elif lead_mode == "riff":
        # The riff itself, two octaves up, holes and all. The gaps are the
        # point, so the lead keeps them.
        for bar in range(16):
            at = bar * BAR
            r = n(cyc[bar % 8])
            riff = RIFF_A if bar % 2 == 0 else RIFF_B
            for sx, off in riff:
                t.note(at + sx * E16, LEAD, r + off + 24, E16 * 2 - 30,
                       104 if sx == 0 else 86)


@candidate("race", "o", "stomp-riff", 16, 154)
def race_o(t):
    """The original, with the sparse line."""
    _stomp(t, "tune")


@candidate("race", "o1", "stomp-noLead", 16, 154)
def race_o1(t):
    """Stomp with no lead line -- just the riff and the space around it."""
    _stomp(t, "none")


@candidate("race", "o2", "stomp-riffLead", 16, 154)
def race_o2(t):
    """Stomp with the lead doubling the riff two octaves up, holes included."""
    _stomp(t, "riff")


# ============================== i AND j, MADE DISTINCT ======================
# Phil picked i (shuffle-rock) and j (boogie-drive) but noticed they are nearly
# the same piece: both swung, both in A, IDENTICAL drum pattern, and both leads
# are short scalar figures on the swing interval. Only the walking bass
# differed. Sharing a groove is not the problem -- sharing the key, the kit and
# the melodic shape is.
#
# So each keeps what it is and loses what it borrowed:
#   i2  stays a GUITAR SHUFFLE in A, slower, with a real blues riff that plays
#       every bar and is derived from the bass -- no separate tune.
#   j2  becomes a PIANO BOOGIE in E: different key, different kit (a train
#       beat), the walking bass IS the tune, and there is no lead at all.

@candidate("race", "i2", "shuffle-blues", 16, 152)
def race_i2(t):
    """The shuffle, slowed and made bluesier. The riff is a real minor
    pentatonic figure with the flat fifth, played EVERY bar on the guitar, so
    it is the tune rather than an occasional visitor. Ride instead of hats
    gives the shuffle its swing."""
    LEAD, BASS, GTR = 0, 1, 4
    for ch, prog, vol in ((LEAD, P_GUITAR, 106), (BASS, P_BASS, 122),
                          (GTR, P_GUITAR, 98)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    SWING = (Q * 2) // 3
    cyc = ["a2", "a2", "a2", "a2", "d3", "d3", "a2", "a2",
           "e3", "e3", "d3", "d3", "a2", "a2", "e3", "a2"]
    # A minor pentatonic with the flat fifth. Semitone offsets from the root,
    # so the riff TRANSPOSES with the chord instead of sitting still.
    RIFF = [0, 3, 5, 6, 5, 3]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar])
        for b in range(4):
            base = at + b * Q
            t.note(base, BASS, r, SWING - 18, 116)
            t.note(base + SWING, BASS, r + 7, (Q - SWING) - 14, 92)
            # Ride on both halves of the swing: this is what a shuffle rides on.
            t.note(base, DRUMS, RIDE, Q // 9, 56)
            t.note(base + SWING, DRUMS, RIDE, Q // 9, 44)
            if b % 2 == 0:
                t.note(base, DRUMS, KICK, E16, 110)
            else:
                t.note(base, DRUMS, SNARE, E16, 106)
        # The riff, every bar, in the guitar's own register.
        for k, off in enumerate(RIFF):
            pos = at + (k // 2) * Q + (SWING if k % 2 else 0)
            t.note(pos, LEAD, r + 24 + off,
                   (SWING if k % 2 == 0 else Q - SWING) - 16,
                   104 if k == 0 else 88)
        # Chord punctuation only where the riff rests.
        t.chord(at + Q * 3, GTR, [r + 12, r + 19], SWING - 24, 92)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 100)
        if bar % 4 == 3:
            t.note(at + Q * 3 + SWING, DRUMS, TOM_MID, E16, 86)


@candidate("race", "j2", "boogie-piano", 16, 178)
def race_j2(t):
    """A piano boogie, not a shuffle-rock twin: different KEY (E, not A),
    different KIT (a train beat -- snare on the swung halves), an electric
    piano comping the offbeats, and NO lead line at all. The walking bass is
    the tune, which is what a boogie is."""
    BASS, EP, GTR = 1, 2, 4
    for ch, prog, vol in ((BASS, P_BASS, 124), (EP, P_EPIANO, 104),
                          (GTR, P_GUITAR, 90)):
        t.program(0, ch, prog); t.volume(0, ch, vol)
    SWING = (Q * 2) // 3
    # The boogie walk: root, third, fifth, sixth and back -- the oldest bass
    # line in rock and roll.
    WALK = [0, 4, 7, 9, 10, 9, 7, 4]
    # E, with the classic I-IV-V turnaround at the end.
    cyc = ["e2", "e2", "e2", "e2", "a2", "a2", "e2", "e2",
           "e2", "e2", "a2", "a2", "b2", "a2", "e2", "b2"]
    for bar in range(16):
        at = bar * BAR
        r = n(cyc[bar])
        for b in range(4):
            base = at + b * Q
            t.note(base, BASS, r + WALK[(b * 2) % 8], SWING - 16, 120)
            t.note(base + SWING, BASS, r + WALK[(b * 2 + 1) % 8],
                   (Q - SWING) - 14, 96)
            # Train beat: kick on every beat, snare on the swung half. That
            # displacement is the whole feel and it is nothing like i's
            # backbeat.
            t.note(base, DRUMS, KICK, E16, 108)
            t.note(base + SWING, DRUMS, SNARE, E16, 92 if b % 2 else 100)
            t.note(base, DRUMS, HAT, Q // 10, 50)
        # Piano comping the offbeats -- the sound that makes it a boogie.
        for b in range(4):
            t.chord(at + b * Q + SWING, EP, [r + 12, r + 16, r + 19],
                    (Q - SWING) - 20, 100 if b % 2 == 0 else 84)
        if bar % 8 == 0:
            t.note(at, DRUMS, CRASH, Q, 102)
        if bar % 8 == 7:
            # A turnaround fill, mid-loop rather than at the seam.
            for k in range(3):
                t.note(at + Q * 3 + k * (SWING // 2), DRUMS, TOM_MID,
                       E16, 88 + k * 6)
            t.chord(at + Q * 3, GTR, [r + 12, r + 19], SWING - 20, 94)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    want_seconds = len(sys.argv) > 2 and sys.argv[2] == "--seconds"
    for slot, letter, desc, bars, bpm, fn in CANDIDATES:
        name = "%s_%s_%s" % (slot, letter, desc)
        secs = bars * 4 * 60.0 / bpm
        if want_seconds:
            print("%s %.4f" % (name, secs))
            continue
        parts = []
        t = head(name, bpm, parts)
        fn(t)
        size = midi.write(os.path.join(outdir, name + ".mid"), [t])
        print("  %-28s %2d bars @%3d = %5.1f s  %5d bytes"
              % (name + ".mid", bars, bpm, secs, size))


if __name__ == "__main__":
    main()
