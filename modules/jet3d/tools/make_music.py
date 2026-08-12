#!/usr/bin/env python3
"""Writes the SHIPPED skyloopers soundtrack.

This is deliberately a selector, not a second copy of the music. The tracks
live in tools/make_candidates.py, where they were auditioned and chosen; this
file only says which ones ship and under what filenames. Copying the note data
across would have meant two places to keep in step and a real chance of
shipping something subtly different from what was approved.

    menu     menu_b   neon-groove
    race1    race_i   shuffle-rock
    race2    race_j2  boogie-piano
    race3    race_m2  dorian-riffLead
    finish   finish_e funky-strut

Three race tracks, chosen at random per race by the game, so a session does
not become one loop. All five LOOP, including the finish -- the results screen
used to fall silent because the fanfare was a one-shot.

Rendered and compressed by tools/build_music.sh; run that, not this.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import midi
import make_candidates as C

# (shipping name, (slot, letter))
PICKS = (
    ("menu",   ("menu",   "b")),
    ("race1",  ("race",   "i")),
    ("race2",  ("race",   "j2")),
    ("race3",  ("race",   "m2")),
    ("finish", ("finish", "e")),
)


def find(slot, letter):
    for cand in C.CANDIDATES:
        if cand[0] == slot and cand[1] == letter:
            return cand
    raise SystemExit("no candidate %s_%s -- did it get renamed?" % (slot, letter))


def build(name, slot, letter, outdir):
    _slot, _letter, desc, bars, bpm, fn = find(slot, letter)
    track = C.head("skyloopers %s (%s)" % (name, desc), bpm, [])
    fn(track)
    path = os.path.join(outdir, name + ".mid")
    size = midi.write(path, [track])
    return desc, bars, bpm, bars * 4 * 60.0 / bpm, size


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    # A second argument asks for the loop lengths, which the render step needs:
    # a loop has to be cut at exactly its own length or the seam clicks.
    seconds_only = len(sys.argv) > 2 and sys.argv[2] == "--seconds"
    for name, (slot, letter) in PICKS:
        desc, bars, bpm, secs, size = build(name, slot, letter, outdir)
        if seconds_only:
            print("%s %.4f" % (name, secs))
        else:
            print("  %-10s %-18s %2d bars @%3d = %5.1f s  %5d bytes"
                  % (name + ".mid", desc, bars, bpm, secs, size))


main()
