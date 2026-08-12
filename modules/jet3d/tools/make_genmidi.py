#!/usr/bin/env python3
"""Builds the OPL instrument bank (GENMIDI format) for skyloopers.

We author this rather than shipping Doom's GENMIDI, which is proprietary.
Only the programmes the soundtrack actually uses are voiced; everything else
gets a neutral default so a stray programme change cannot produce silence.

GENMIDI layout, as read by i_oplmusic.c:
    8 bytes   "#OPL_II#"
    128 x 36  melodic instruments
    47  x 36  percussion, for MIDI notes 35..81
    128 x 32  melodic names
    47  x 32  percussion names

Each 36-byte instrument:
    u16 flags          1 = fixed pitch, 4 = double voice
    u8  fine_tuning    0x80 = none
    u8  fixed_note
    2 x 16-byte voice

Each 16-byte voice:
    6 bytes modulator, 1 byte feedback/connection,
    6 bytes carrier,   1 byte unused, s16 base note offset

Each 6-byte operator maps straight onto OPL registers:
    tremolo   reg 0x20  bit7 AM, bit6 vibrato, bit5 sustaining envelope,
                        bit4 key-scale rate, bits3-0 frequency multiplier
    attack    reg 0x60  attack rate << 4 | decay rate
    sustain   reg 0x80  sustain level << 4 | release rate
    waveform  reg 0xE0  see WAVES below
    scale     reg 0x40  key-scale level in bits7-6
    level     reg 0x40  total level in bits5-0

Facts that decide how these are written, all of them easy to get backwards:
  * RATES: 15 is the FASTEST attack/decay/release, 0 means "never changes".
  * SUSTAIN LEVEL: 0 is LOUDEST, 15 is quietest -- it is an attenuation.
  * TOTAL LEVEL: 0 is loudest, 63 is silent, 0.75 dB a step.
  * MULTIPLIER: 0 means HALF frequency, which is how you get a sub-octave
    without writing lower notes. 1-10 are literal, then 11/13/15 give
    10x/12x/15x.
  * FEEDBACK is exponential, not linear: 1..7 are pi/16, pi/8, pi/4, pi/2,
    pi, 2pi, 4pi. 7 is a scream, not "a bit more than 6". Feedback on the
    modulator is also how a sine operator is pushed toward a sawtooth.
  * KEY SCALE LEVEL attenuates as pitch rises (1.5/3/6 dB per octave). A
    lead written across two octaves needs it or the top notes shout.
  * A NON-SUSTAINING envelope (bit5 clear) starts releasing the moment decay
    ends, which is what makes something read as struck rather than held.

We run the chip in OPL3 mode (see opl_tdeck.c), so waveforms 4-7 are
available. 6 is a real square -- worth far more than any two-operator
approximation of one.
"""
import struct, sys

HEADER = b"#OPL_II#"
NUM_MELODIC, NUM_PERC = 128, 47

# 0-3 are OPL2; 4-7 need OPL3 mode.
W_SINE, W_HALF, W_ABS, W_PULSE = 0, 1, 2, 3
W_SINE_EVEN, W_ABS_EVEN, W_SQUARE, W_DERIVED_SQUARE = 4, 5, 6, 7


def op(mult=1, sustaining=True, vibrato=False, tremolo=False, ksr=False,
       attack=15, decay=4, sustain=2, release=5, wave=W_SINE, ksl=0, level=0):
    t = (0x80 if tremolo else 0) | (0x40 if vibrato else 0) \
        | (0x20 if sustaining else 0) | (0x10 if ksr else 0) | (mult & 0x0F)
    return bytes([t,
                  ((attack & 0xF) << 4) | (decay & 0xF),
                  ((sustain & 0xF) << 4) | (release & 0xF),
                  wave & 0x07,
                  (ksl & 0x03) << 6,
                  level & 0x3F])


# offset defaults to -12 because the player sounds a MIDI note an octave
# above where it is written: a near-pure sine asked for c4 (261.63Hz) came
# out at 522.9Hz. Real GENMIDI banks carry the same correction per voice --
# base_note_offset is exactly what it is for.
def voice(mod, car, feedback=0, additive=False, offset=-12):
    return mod + bytes([((feedback & 0x07) << 1) | (1 if additive else 0)]) \
        + car + bytes([0]) + struct.pack("<h", offset)


def instr(v, flags=0, fine=0x80, fixed=0):
    return struct.pack("<HBB", flags, fine, fixed) + v + v


# --- the palette ----------------------------------------------------------
# Carrier level stays near 0: the player scales the carrier by note velocity
# and channel volume, so baking attenuation in would fight it. The
# MODULATOR's level is the FM index -- a lower number is a brighter, more
# harmonically dense tone, and it is the main character control.

PATCHES = {}

# Synth bass. Modulator at half frequency (mult 0) adds a sub-octave under
# the note, which is how this stays audible on a speaker with no low end:
# what you hear is weight the fundamental alone would not deliver. Feedback
# 5 (pi) pushes the modulator toward a saw so it has bite as well.
PATCHES[38] = voice(
    op(mult=0, attack=15, decay=6, sustain=3, release=6, wave=W_SINE,
       level=0x10),
    op(mult=1, attack=15, decay=8, sustain=2, release=7, wave=W_SINE,
       level=0x00),
    feedback=5)
PATCHES[39] = PATCHES[38]
PATCHES[33] = PATCHES[38]

# Square lead. A REAL square on the carrier (OPL3 waveform 6), lightly
# modulated so it keeps the square's edge instead of being smeared into FM
# hash. KSL 1 keeps the top of a two-octave line from shouting.
PATCHES[80] = voice(
    op(mult=1, attack=15, decay=5, sustain=3, release=5, wave=W_SINE,
       level=0x22),
    op(mult=1, attack=15, decay=3, sustain=1, release=4, wave=W_SQUARE,
       ksl=1, level=0x00),
    feedback=2)
PATCHES[81] = PATCHES[80]

# Saw-ish lead: modulator a fifth above (mult 3) at a strong index gives a
# dense, reedy tone that reads as a saw without a saw waveform existing.
PATCHES[82] = voice(
    op(mult=3, attack=15, decay=5, sustain=2, release=5, wave=W_SINE,
       level=0x14),
    op(mult=1, attack=15, decay=4, sustain=1, release=5, wave=W_SINE,
       ksl=1, level=0x00),
    feedback=5)

# Warm pad. Slow attack, long release, gentle index, and vibrato so a held
# chord is never completely still. Deliberately dull -- it is a bed.
PATCHES[89] = voice(
    op(mult=1, attack=6, decay=2, sustain=1, release=2, wave=W_SINE,
       level=0x26),
    op(mult=1, vibrato=True, attack=5, decay=1, sustain=0, release=2,
       wave=W_SINE, ksl=1, level=0x00),
    feedback=1)
PATCHES[90] = PATCHES[89]
PATCHES[51] = PATCHES[89]

# Electric piano: struck, not held, so both operators use a non-sustaining
# envelope and the note decays on its own however long the key is down.
PATCHES[4] = voice(
    op(mult=1, sustaining=False, attack=15, decay=7, sustain=5, release=8,
       wave=W_SINE, level=0x18),
    op(mult=1, sustaining=False, attack=15, decay=6, sustain=4, release=8,
       wave=W_SINE, ksl=1, level=0x00),
    feedback=3)
PATCHES[5] = PATCHES[4]

# Distortion guitar. Feedback 6 (2pi) is enough to growl; 7 is 4pi and turns
# into a scream that buries everything else in the mix.
PATCHES[30] = voice(
    op(mult=1, attack=15, decay=6, sustain=2, release=5, wave=W_PULSE,
       level=0x10),
    op(mult=1, attack=15, decay=5, sustain=1, release=5,
       wave=W_DERIVED_SQUARE, ksl=1, level=0x00),
    feedback=6)
PATCHES[29] = PATCHES[30]

# Bell / pluck. An inharmonic ratio (7:1) is what makes a bell a bell, and a
# non-sustaining envelope makes it ring down rather than hold.
PATCHES[11] = voice(
    op(mult=7, sustaining=False, attack=15, decay=6, sustain=7, release=7,
       wave=W_SINE, level=0x1E),
    op(mult=1, sustaining=False, attack=15, decay=4, sustain=5, release=6,
       wave=W_SINE, ksl=1, level=0x00),
    feedback=0)
PATCHES[98] = PATCHES[11]

# --- LEAD VOICES ---------------------------------------------------------
# There were only two leads (80 square, 82 "saw") and nearly every track used
# one of them, which made the whole soundtrack sound like the same band. Worse,
# 82 is a SINE carrier with a fifth-above modulator, and that is the standard
# recipe for a cheap electric organ -- "keyboardish", correctly.
# These three are deliberately not that.

# 61 BRASS lead: a moderate index with a slow-ish attack on both operators, so
# the note blooms instead of arriving square. Reads as a horn, not a keyboard.
PATCHES[61] = voice(
    op(mult=1, attack=11, decay=5, sustain=2, release=5, wave=W_SINE,
       level=0x16),
    op(mult=1, attack=12, decay=4, sustain=1, release=5, wave=W_SINE,
       ksl=1, level=0x00),
    feedback=3)
PATCHES[62] = PATCHES[61]

# 85 GRIT lead: a derived-square carrier hammered by a strong second-harmonic
# modulator at maximum feedback. Aggressive and vocal -- what a metal lead
# wants, and about as far from an organ as two operators reach.
PATCHES[85] = voice(
    op(mult=2, attack=15, decay=4, sustain=1, release=5, wave=W_PULSE,
       level=0x0A),
    op(mult=1, attack=15, decay=3, sustain=0, release=4,
       wave=W_DERIVED_SQUARE, ksl=1, level=0x00),
    feedback=7)

# 86 REED lead: an odd 3:1 ratio with a half-sine carrier. Nasal and thin in a
# woodwind way rather than a synth way, and it cuts through a busy mix.
PATCHES[86] = voice(
    op(mult=3, attack=14, decay=5, sustain=3, release=5, wave=W_SINE,
       level=0x18),
    op(mult=1, attack=15, decay=4, sustain=1, release=5, wave=W_HALF,
       ksl=1, level=0x00),
    feedback=4)

# Plucked arpeggio voice: short, bright, and out of the lead's way. Used for
# the running figures rather than for melody.
PATCHES[87] = voice(
    op(mult=2, sustaining=False, attack=15, decay=8, sustain=6, release=8,
       wave=W_SINE, level=0x1A),
    op(mult=1, sustaining=False, attack=15, decay=7, sustain=4, release=8,
       wave=W_SQUARE, ksl=2, level=0x00),
    feedback=3)

# Programme 1 is a near-pure sine: the modulator is attenuated to silence and
# there is no feedback, so the carrier is heard alone. It doubles as a clean
# sub-bass voice and as the reference when checking that a note comes out at
# the pitch it was written at.
PATCHES[1] = voice(
    op(mult=1, attack=15, decay=4, sustain=1, release=5, wave=W_SINE,
       level=0x3F),
    op(mult=1, attack=15, decay=4, sustain=1, release=5, wave=W_SINE,
       level=0x00),
    feedback=0)

DEFAULT = voice(
    op(mult=1, attack=15, decay=4, sustain=2, release=5, wave=W_SINE,
       level=0x18),
    op(mult=1, attack=15, decay=4, sustain=2, release=5, wave=W_SINE,
       ksl=1, level=0x00),
    feedback=4)

# --- percussion -----------------------------------------------------------
# Fixed pitch (flag 1): the player ignores the played note and uses
# fixed_note, so a kick is a kick wherever it lands on the drum map. Every
# drum uses a NON-SUSTAINING envelope, which is the whole difference between
# a hit and a held tone. Cymbals and snares fake noise with a very high
# multiplier -- at 15x the operator is so far above the fundamental that the
# result is inharmonic enough to read as noise.


def perc(v, note):
    # Fixed-pitch instruments skip base_note_offset entirely (see
    # FrequencyForVoice), so they carry the octave correction here instead.
    return struct.pack("<HBB", 0x0001, 0x80, note - 12) + v + v


PERC = {}

# Kick: mult 0 on the carrier puts the thump half an octave down, and a fast
# decay with no sustain gives the pitch-drop thud a kick drum has.
_kick = voice(
    op(mult=1, sustaining=False, attack=15, decay=10, sustain=6, release=10,
       wave=W_SINE, level=0x0A),
    op(mult=0, sustaining=False, attack=15, decay=11, sustain=8, release=11,
       wave=W_SINE, level=0x00),
    feedback=5)
PERC[35] = perc(_kick, 26)
PERC[36] = perc(_kick, 26)

# Snare: square carrier plus a detuned high modulator. The square supplies
# the body, the 15x modulator the rasp.
_snare = voice(
    op(mult=15, sustaining=False, attack=15, decay=11, sustain=5, release=11,
       wave=W_PULSE, level=0x08),
    op(mult=13, sustaining=False, attack=15, decay=10, sustain=4, release=11,
       wave=W_SQUARE, level=0x00),
    feedback=6)
PERC[38] = perc(_snare, 58)
PERC[40] = perc(_snare, 60)

# Hats: the same voice at two decay rates. Nothing distinguishes a closed hat
# from an open one except how fast it stops.
def _hat(decay, release):
    return voice(
        op(mult=15, sustaining=False, attack=15, decay=decay, sustain=8,
           release=release, wave=W_PULSE, level=0x12),
        op(mult=13, sustaining=False, attack=15, decay=decay, sustain=7,
           release=release, wave=W_DERIVED_SQUARE, level=0x00),
        feedback=6)


PERC[42] = perc(_hat(13, 13), 84)
PERC[44] = perc(_hat(14, 14), 84)
PERC[46] = perc(_hat(8, 8), 84)

# Crash and ride: long, bright, inharmonic.
_crash = voice(
    op(mult=15, sustaining=False, attack=15, decay=5, sustain=3, release=5,
       wave=W_PULSE, level=0x13),
    op(mult=11, sustaining=False, attack=15, decay=4, sustain=2, release=5,
       wave=W_DERIVED_SQUARE, level=0x00),
    feedback=6)
PERC[49] = perc(_crash, 80)
PERC[57] = perc(_crash, 80)
PERC[51] = perc(_crash, 88)

# Toms, tuned across the kit.
for _n, _note in ((41, 36), (43, 40), (45, 45), (47, 50), (48, 55), (50, 59)):
    PERC[_n] = perc(voice(
        op(mult=1, sustaining=False, attack=15, decay=9, sustain=6,
           release=9, wave=W_SINE, level=0x12),
        op(mult=1, sustaining=False, attack=15, decay=9, sustain=5,
           release=9, wave=W_SINE, level=0x00),
        feedback=4), _note)

# Rimshot / clap for ghost notes: very short, quiet, and out of the way.
PERC[37] = perc(voice(
    op(mult=15, sustaining=False, attack=15, decay=13, sustain=9, release=13,
       wave=W_PULSE, level=0x18),
    op(mult=12, sustaining=False, attack=15, decay=13, sustain=8, release=13,
       wave=W_SQUARE, level=0x00),
    feedback=5), 70)
PERC[39] = PERC[37]

DEFAULT_PERC = perc(_hat(13, 13), 84)


def build():
    out = bytearray(HEADER)
    for i in range(NUM_MELODIC):
        out += instr(PATCHES.get(i, DEFAULT))
    for i in range(NUM_PERC):
        out += PERC.get(i + 35, DEFAULT_PERC)
    for i in range(NUM_MELODIC):
        out += (b"prog%d" % i).ljust(32, b"\0")
    for i in range(NUM_PERC):
        out += (b"perc%d" % (i + 35)).ljust(32, b"\0")
    return bytes(out)


def main():
    data = build()
    expect = 8 + (NUM_MELODIC + NUM_PERC) * 36 + (NUM_MELODIC + NUM_PERC) * 32
    assert len(data) == expect, (len(data), expect)
    path = sys.argv[1]
    with open(path, "w", newline="\n") as f:
        f.write("// GENERATED by tools/make_genmidi.py -- do not hand-edit.\n")
        f.write("// The OPL instrument bank, in GENMIDI format. Authored for\n")
        f.write("// skyloopers rather than taken from a WAD: Doom's GENMIDI is\n")
        f.write("// proprietary, and voicing only what the soundtrack uses\n")
        f.write("// keeps this to something we can reason about and retune.\n\n")
        f.write("#ifndef JET_GENMIDI_BANK_H\n#define JET_GENMIDI_BANK_H\n\n")
        f.write("static const unsigned char jet_genmidi_bank[] = {\n")
        for i in range(0, len(data), 16):
            row = ", ".join("0x%02x" % b for b in data[i:i + 16])
            f.write("    %s,\n" % row)
        f.write("};\n\n")
        f.write("static const unsigned int jet_genmidi_bank_len = %d;\n\n"
                % len(data))
        f.write("#endif\n")
    print("wrote %s  (%d bytes of bank, %d voiced programmes, %d percussion)"
          % (path, len(data), len(PATCHES), len(PERC)))


main()
