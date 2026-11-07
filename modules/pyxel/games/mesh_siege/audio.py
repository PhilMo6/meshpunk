# Mesh Siege: sound effects and music. Music plays on channels 0-1; event
# sounds use channel 2 and combat sounds channel 3, each with a priority so a
# shot never cuts off an alarm.
import pyxel

MOVE = 0
BUILD = 1
UPGRADE = 2
SELL = 3
ERROR = 4
PING = 5
YAGI = 6
FLOOD = 7
KILL = 8
WAVE = 10                 # sting: a wave starts
JAM = 11
LINK_LOST = 12
BOSS = 13                 # sting: a flare arrives
SELECT = 14
BIG_KILL = 15
WIN = 16
LOSE = 17
CLEAR = 18
LINK_UP = 19
ORDER = 20
NODE_DOWN = 21
REPAIRED = 22
ZAP = 23
# 24-33 hold the music.
NULLED = 34
PINNED = 35
CLUE = 36
TAKEN = 37
ALARM = 38
HIT = 39

# sound id: (notes, tones, volumes, effects, speed, channel, priority)
SFX = {
    MOVE: ("c3", "t", "2", "f", 2, 2, 0),
    BUILD: ("c2e2g2c3", "p", "4", "n", 3, 2, 2),
    UPGRADE: ("c2g2c3e3g3", "s", "4", "n", 3, 2, 2),
    SELL: ("g2e2c2", "p", "3", "f", 4, 2, 2),
    ERROR: ("c1rc1", "s", "4", "n", 5, 2, 1),
    PING: ("a3", "p", "1", "f", 2, 3, 0),
    YAGI: ("c3g2c2", "s", "3", "f", 2, 3, 1),
    FLOOD: ("c3f2c2", "n", "3", "f", 3, 3, 1),
    KILL: ("c2c1", "n", "3", "f", 3, 3, 2),
    WAVE: ("a2re3ra3rrr", "p", "4", "nnnnf", 12, 2, 3),
    JAM: ("c3a2c3a2", "p", "3", "n", 4, 2, 3),
    LINK_LOST: ("e2c2", "p", "3", "f", 6, 2, 2),
    BOSS: ("c2rc#2rc2rg1rrr", "s", "5", "v", 14, 2, 4),
    SELECT: ("c3g3", "p", "3", "n", 3, 2, 1),
    BIG_KILL: ("c3c2c1c1", "n", "6", "f", 8, 3, 3),
    WIN: ("c2e2g2c3rg2c3", "s", "5", "nnnnnnf", 8, 2, 5),
    LOSE: ("e2rd#2rd2rc#2", "p", "5", "nnnnnnf", 12, 2, 5),
    CLEAR: ("c2e2g2e2g2c3", "p", "4", "n", 4, 2, 3),
    LINK_UP: ("c3e3", "t", "3", "n", 4, 2, 2),
    ORDER: ("g2c3", "t", "3", "n", 3, 2, 1),
    NODE_DOWN: ("a2f2d2a1", "s", "5", "f", 5, 2, 4),
    REPAIRED: ("c3e3g3", "t", "4", "n", 3, 2, 2),
    ZAP: ("c4c3c4", "n", "4", "f", 2, 3, 2),
    NULLED: ("c3a#2g2e2", "s", "5", "v", 6, 2, 4),
    PINNED: ("c3e3g3c4", "p", "5", "n", 4, 2, 4),
    CLUE: ("e3g3", "t", "3", "n", 4, 2, 2),
    TAKEN: ("g2c3e3g3c4", "s", "5", "n", 4, 2, 4),
    ALARM: ("a3e3a3e3a3e3", "s", "6", "n", 6, 2, 5),
    HIT: ("c2", "n", "3", "f", 3, 3, 1),
}


def _seq(*parts):
    # A phrase from (note, steps) pairs: the note, then rests to fill its steps.
    out = ""
    for note, steps in parts:
        out += note + "r" * (steps - 1)
    return out


# In game: quiet chimes from A minor pentatonic over a slow drone whose notes
# fade over 2 s with 2 s of silence after each. The two loops differ in
# length (32 s and 48 s), so they only line up every 96 s.
CHIMES = (
    _seq(("e3", 6), ("a3", 10), ("c4", 8), ("b3", 8)),
    _seq(("g3", 8), ("e3", 8), ("d3", 4), ("e3", 12)),
    _seq(("a3", 4), ("c4", 4), ("e4", 8), ("d4", 16)),
    _seq(("r", 8), ("g3", 6), ("a3", 6), ("e3", 12)),
    _seq(("c4", 6), ("b3", 6), ("g3", 4), ("a3", 16)),
    _seq(("r", 16), ("e4", 4), ("d4", 4), ("c4", 8)),
)
DRONE = ("a1rf1rc2rg1r", "a1re1rf1rd1r", "d1rf1ra1re1r")
# Title: slow arpeggio over a faded pad.
ARP = "a1c2e2a2e2c2a1e1 f1a1c2f2c2a1f1c1 c2e2g2c3g2e2c2g1 g1b1d2g2d2b1g1d1"
PAD = "e2c2e2d2"

S_CHIME = 40              # 40-45
S_DRONE = 24              # 24-26
S_ARP = 30
S_PAD = 31

M_GAME = 0
M_TITLE = 1

_busy = [0, 0, 0, 0]     # frame each sfx channel frees up
_prio = [0, 0, 0, 0]
enabled = True
music_on = True           # sound effects play either way
_music = -1               # the track playing
_want = -1                # the track the game is in, playing or not


def setup():
    for sid, (notes, tones, vols, fx, speed, ch, prio) in SFX.items():
        pyxel.sounds[sid].set(notes, tones, vols, fx, speed)
    for i, notes in enumerate(CHIMES):
        pyxel.sounds[S_CHIME + i].set(notes, "t", "2", "f", 20)
    for i, notes in enumerate(DRONE):
        pyxel.sounds[S_DRONE + i].set(notes, "t", "2", "f", 240)
    pyxel.musics[M_GAME].set([S_CHIME + i for i in range(len(CHIMES))],
                             [S_DRONE + i for i in range(len(DRONE))])
    pyxel.sounds[S_ARP].set(ARP, "t", "5", "n", 20)
    pyxel.sounds[S_PAD].set(PAD, "p", "3", "f", 160)
    pyxel.musics[M_TITLE].set([S_ARP], [S_PAD])


def sfx(sid):
    if not enabled:
        return
    notes, tones, vols, fx, speed, ch, prio = SFX[sid]
    now = pyxel.frame_count
    if now < _busy[ch] and prio < _prio[ch]:
        return
    pyxel.play(ch, sid)
    steps = len(pyxel.sounds[sid].notes)
    _busy[ch] = now + (steps * speed) // 4 + 1
    _prio[ch] = prio


def music(msc):
    global _music, _want
    _want = msc
    if music_on and msc != _music:
        _music = msc
        pyxel.playm(msc, loop=True)


def stop_music():
    global _music, _want
    _music = -1
    _want = -1
    pyxel.stop(0)
    pyxel.stop(1)


def set_music(on):
    global music_on, _music
    music_on = on
    if not on:
        _music = -1
        pyxel.stop(0)
        pyxel.stop(1)
    elif _want >= 0:
        music(_want)
