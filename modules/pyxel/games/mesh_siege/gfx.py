# Mesh Siege: palette, sprites and logo (image bank 0), map art (bank 1). The
# game draws bad actor search zones into bank 2.
import pyxel
from data import *

ROADS = (T_ROAD, T_BRIDGE, T_GATE)

# Image bank 0 layout: nodes at v=0, noise frame A at v=16 and frame B one
# sprite height below, icons at v=48, online node sprites at NODE_V, logo at
# LOGO_V.
SPR_GATE = (48, 0)
SPR_ACTOR = (40, 0)
SPR_HOUSE = (8, 48)
SPR_TRUCK = (16, 48)
SPR_GW = (24, 48)
SPR_ACTOR_ICON = (32, 48)
SPR_SUN = (40, 48)
SPR_STAR = (0, 48)        # cred, 5x5

SPRITES = (
    (0, 0, (  # RELAY
        "000c0000",
        "000f0000",
        "00fff000",
        "000f0000",
        "00f1f000",
        "0f0f0f00",
        "0f1f1f00",
        "06666600")),
    (8, 0, (  # PING
        "00000f00",
        "00000f00",
        "06666f60",
        "06cccc60",
        "06cbcc60",
        "06666660",
        "06f6f660",
        "06666660")),
    (16, 0, (  # FLOOD
        "00070000",
        "0c0f0c00",
        "c00f00c0",
        "c00f00c0",
        "0c0f0c00",
        "000f0000",
        "00666000",
        "06666600")),
    (24, 0, (  # YAGI
        "00000000",
        "0f0f0f00",
        "0f0f0f00",
        "fffffffa",
        "0f0f0f00",
        "0f0f0f00",
        "000f0000",
        "00666000")),
    (32, 0, (  # SF12
        "fffffff0",
        "0fcccf00",
        "00fcf000",
        "000f0000",
        "00f7f000",
        "0f777f00",
        "fffffff0",
        "00000000")),
    (48, 0, (  # GATEWAY 16x16
        "0000000c00000000",
        "0000000f00000000",
        "00000fffff000000",
        "0000000f00000000",
        "000000fff0000000",
        "0000000f00000000",
        "0000000f00cc0000",
        "0000000f0c0c0000",
        "0666666f66666660",
        "0611111111111160",
        "061c11b11c11b160",
        "0611111111111160",
        "061b11c11b11c160",
        "0611111111111160",
        "0611117777111160",
        "0666666666666660")),
    (0, 16, (  # STATIC A
        "00f7f000",
        "0f7f7f00",
        "f7f7f7f0",
        "7f1f1f70",
        "f7f7f7f0",
        "0f7f7f00",
        "0f0f0f00",
        "00000000")),
    (0, 24, (  # STATIC B
        "007f7000",
        "07f7f700",
        "7f7f7f70",
        "f7171f70",
        "7f7f7f70",
        "07f7f700",
        "00f0f0f0",
        "00000000")),
    (8, 16, (  # CHIRP A
        "00000900",
        "00009000",
        "00999900",
        "09a99990",
        "09999990",
        "00999900",
        "00900900",
        "00000000")),
    (8, 24, (  # CHIRP B
        "00009000",
        "00000900",
        "00999900",
        "09a99990",
        "09999990",
        "00999900",
        "09000090",
        "00000000")),
    (16, 16, (  # BRICK A
        "11111110",
        "18f88f10",
        "18888810",
        "18787810",
        "18888810",
        "18f88f10",
        "11111110",
        "01000100")),
    (16, 24, (  # BRICK B
        "11111110",
        "18f88f10",
        "18888810",
        "18787810",
        "18888810",
        "18f88f10",
        "11111110",
        "00100010")),
    (24, 16, (  # JAMMER A
        "000d0000",
        "000d0000",
        "00ddd000",
        "0d888d00",
        "ddddddd0",
        "0d1d1d00",
        "0d000d00",
        "00000000")),
    (24, 24, (  # JAMMER B
        "000a0000",
        "000d0000",
        "00ddd000",
        "0d888d00",
        "ddddddd0",
        "0d1d1d00",
        "00d0d000",
        "00000000")),
    (32, 16, (  # STORM A
        "00aaa000",
        "0aa7aa00",
        "aaaaaaa0",
        "a1aaa1a0",
        "0aaaaa00",
        "00900900",
        "09009000",
        "00000000")),
    (32, 24, (  # STORM B
        "00aaa000",
        "0aa7aa00",
        "aaaaaaa0",
        "a1aaa1a0",
        "0aaaaa00",
        "09009000",
        "00900090",
        "00000000")),
    (48, 16, (  # FLARE A 16x16
        "0000009009000000",
        "0090000990000900",
        "0009009999009000",
        "0000999aa9990000",
        "00099aaaaaa99000",
        "9009aaaaaaaa9009",
        "099aa7aaaa7aa990",
        "009aa1aaaa1aa900",
        "009aaaaaaaaaa900",
        "099aaa8888aaa990",
        "9009aaaaaaaa9009",
        "00099aaaaaa99000",
        "0000999aa9990000",
        "0009009999009000",
        "0090000990000900",
        "0000009009000000")),
    (48, 32, (  # FLARE B 16x16
        "0900000000000090",
        "0090009009000900",
        "0000009999000000",
        "0000999aa9990000",
        "00099aaaaaa99000",
        "0009aaaaaaaa9000",
        "099aa7aaaa7aa990",
        "909aa1aaaa1aa909",
        "909aaaaaaaaaa909",
        "099aaa8888aaa990",
        "0009aaaaaaaa9000",
        "00099aaaaaa99000",
        "0000999aa9990000",
        "0000009999000000",
        "0090009009000900",
        "0900000000000090")),
    (40, 16, (  # ECHO A
        "00077000",
        "007cc700",
        "07c77c70",
        "0c8cc8c0",
        "0cccccc0",
        "c0c00c0c",
        "0c0cc0c0",
        "00000000")),
    (40, 24, (  # ECHO B
        "00077000",
        "007cc700",
        "07c77c70",
        "0c8cc8c0",
        "0cccccc0",
        "0c0cc0c0",
        "c0c00c0c",
        "00000000")),
    (40, 0, (  # BAD ACTOR
        "00800080",
        "00080800",
        "00008000",
        "00088800",
        "00818180",
        "00888880",
        "00810180",
        "01111110")),
    (64, 16, (  # BAD SIGNAL A
        "00800800",
        "08d88d80",
        "8dd88dd8",
        "08888880",
        "008dd800",
        "08000080",
        "80000008",
        "00000000")),
    (64, 24, (  # BAD SIGNAL B
        "08000080",
        "08d88d80",
        "8dd88dd8",
        "08888880",
        "008dd800",
        "00800800",
        "00800800",
        "00000000")),
    (24, 48, (  # GATEWAY ICON
        "0000c00",
        "0006f60",
        "0000f00",
        "6666f66",
        "61c1b16",
        "6666666")),
    (32, 48, (  # BAD ACTOR ICON
        "80008",
        "08080",
        "00800",
        "08880",
        "88888",
        "01110")),
    (40, 48, (  # SUN
        "a0a0a",
        "0aaa0",
        "aaaaa",
        "0aaa0",
        "a0a0a")),
    (0, 48, (  # CRED STAR
        "00c00",
        "0ccc0",
        "ccccc",
        "0ccc0",
        "0c0c0")),
    (8, 48, (  # HOUSE
        "00080000",
        "00888000",
        "08888800",
        "88888880",
        "0f7faf00",
        "0ff1ff00")),
    (16, 48, (  # TRUCK
        "0999cc0",
        "9999999",
        "9999999",
        "0110110",
        "0000000")),
)


NODE_V = 64       # online node sprites: level L at v = NODE_V + L * 8
LOGO_V = 96       # "MESH" and "SIEGE" as text, 8 rows apart


def setup():
    pyxel.colors.from_list(PALETTE)
    img = pyxel.images[0]
    for u, v, rows in SPRITES:
        img.set(u, v, list(rows))
    # Online node sprites per level: green link LED, yellow level pips.
    for kind in range(5):
        for lv in range(3):
            v = NODE_V + lv * 8
            img.blt(kind * 8, v, 0, kind * 8, 0, 8, 8)
            img.pset(kind * 8 + 7, v, C_GREEN)
            for i in range(lv):
                img.pset(kind * 8 + i * 2, v + 7, C_YELLOW)
    img.rect(0, LOGO_V, 24, 16, 0)
    img.text(0, LOGO_V, "MESH", C_WHITE)
    img.text(0, LOGO_V + 8, "SIEGE", C_WHITE)


# ---------------------------------------------------------------------------
# Map art, drawn once per game into image bank 1 (the whole 256x256 map).
# ---------------------------------------------------------------------------
def _tile_at(tiles, tx, ty):
    if 0 <= tx < MAP_W and 0 <= ty < MAP_H:
        return tiles[ty * MAP_W + tx]
    return -1


def render_map(level, seed):
    img = pyxel.images[1]
    tiles = level.tiles
    pyxel.rseed(seed)
    img.rect(0, 0, MAP_W * TILE, MAP_H * TILE, C_GRASS_D)
    for ty in range(MAP_H):
        for tx in range(MAP_W):
            t = tiles[ty * MAP_W + tx]
            x = tx * TILE
            y = ty * TILE
            if t == T_FLAT or t == T_GATE:
                _grass(img, x, y)
            elif t == T_VALLEY:
                _valley(img, x, y)
            elif t == T_FOREST:
                _grass(img, x, y)
                _forest(img, x, y)
            elif t == T_HILL:
                _hill(img, x, y)
            elif t == T_WATER:
                _water(img, tiles, tx, ty, x, y)
            elif t == T_MOUNT:
                _mountain(img, tiles, tx, ty, x, y)
            elif t == T_ROAD:
                _road(img, tiles, tx, ty, x, y)
            elif t == T_BRIDGE:
                _water(img, tiles, tx, ty, x, y)
                _bridge(img, tiles, tx, ty, x, y)
            elif t == T_BUILDING:
                _building(img, tiles, tx, ty, x, y)
            elif t == T_HOME:
                _grass(img, x, y)
                _home(img, x, y)
            elif t == T_SUB:
                _grass(img, x, y)
                _substation(img, x, y)


def _grass(img, x, y):
    for _ in range(3):
        img.pset(x + pyxel.rndi(0, 7), y + pyxel.rndi(0, 7), C_GRASS)
    if pyxel.rndi(0, 9) == 0:
        img.pset(x + pyxel.rndi(1, 6), y + pyxel.rndi(1, 6),
                 C_YELLOW if pyxel.rndi(0, 1) else C_WHITE)
    img.pset(x, y, C_GRASS)


def _valley(img, x, y):
    # Low ground: darker, with contour lines.
    img.rect(x, y, TILE, TILE, C_GRASS_D)
    for i in range(0, TILE, 2):
        img.pset(x + i + (y // 8) % 2, y + 6, C_NAVY)
    img.pset(x + pyxel.rndi(0, 7), y + pyxel.rndi(0, 4), C_GRASS)


def _forest(img, x, y):
    # Two or three trees.
    for cx, cy in ((2, 3), (5, 5), (5, 1)):
        if cy == 1 and pyxel.rndi(0, 1):
            continue
        img.circ(x + cx, y + cy, 2, C_GRASS)
        img.pset(x + cx - 1, y + cy - 1, C_HILL)
        img.pset(x + cx, y + cy + 2, C_BG)


def _road(img, tiles, tx, ty, x, y):
    img.rect(x, y, TILE, TILE, C_ROCK)
    img.pset(x + 3, y + 3, C_GRAY)
    img.pset(x + 4, y + 4, C_GRAY)
    # Dashes toward the roads next door.
    for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        n = _tile_at(tiles, tx + dx, ty + dy)
        if n in ROADS or n == -1:
            if dx:
                img.pset(x + 4 + dx * 2, y + 3, C_GRAY)
            else:
                img.pset(x + 3, y + 4 + dy * 2, C_GRAY)
        else:
            if dx == 1:
                img.line(x + 7, y, x + 7, y + 7, C_NAVY)
            elif dx == -1:
                img.line(x, y, x, y + 7, C_NAVY)
            elif dy == 1:
                img.line(x, y + 7, x + 7, y + 7, C_NAVY)
            else:
                img.line(x, y, x + 7, y, C_NAVY)


def _bridge(img, tiles, tx, ty, x, y):
    across = _tile_at(tiles, tx - 1, ty) in ROADS or _tile_at(tiles, tx + 1, ty) in ROADS
    if across:
        img.rect(x, y + 2, TILE, 4, C_DIRT)
        for i in range(0, TILE, 2):
            img.pset(x + i, y + 3, C_BG)
        img.line(x, y + 1, x + 7, y + 1, C_GRAY)
        img.line(x, y + 6, x + 7, y + 6, C_GRAY)
    else:
        img.rect(x + 2, y, 4, TILE, C_DIRT)
        for i in range(0, TILE, 2):
            img.pset(x + 3, y + i, C_BG)
        img.line(x + 1, y, x + 1, y + 7, C_GRAY)
        img.line(x + 6, y, x + 6, y + 7, C_GRAY)


def _building(img, tiles, tx, ty, x, y):
    img.rect(x, y, TILE, TILE, C_NAVY)
    img.rectb(x, y, TILE, TILE, C_ROCK)
    for wy in (2, 5):
        for wx in (2, 5):
            r = pyxel.rndi(0, 5)
            img.pset(x + wx, y + wy, C_YELLOW if r == 0 else (C_CYAN if r == 1 else C_BG))
    if pyxel.rndi(0, 3) == 0:
        img.pset(x + 1, y + 1, C_GRAY)


def _home(img, x, y):
    img.tri(x + 1, y + 3, x + 4, y, x + 7, y + 3, C_RED)
    img.rect(x + 2, y + 3, 5, 4, C_GRAY)
    img.pset(x + 5, y + 5, C_DIRT)
    img.line(x + 1, y + 7, x + 7, y + 7, C_BG)


def _substation(img, x, y):
    # A fenced transformer with a bolt on it.
    img.rectb(x, y + 1, 8, 7, C_GRAY)
    img.rect(x + 2, y + 3, 4, 4, C_ROCK)
    img.line(x + 4, y + 3, x + 3, y + 5, C_YELLOW)
    img.line(x + 3, y + 5, x + 4, y + 6, C_YELLOW)
    img.pset(x + 1, y, C_GRAY)
    img.pset(x + 6, y, C_GRAY)


def _hill(img, x, y):
    img.elli(x, y + 1, 8, 7, C_HILL)
    img.line(x + 2, y + 2, x + 4, y + 2, C_YELLOW)
    img.pset(x + 1, y + 3, C_YELLOW)
    img.line(x + 1, y + 7, x + 6, y + 7, C_GRASS)
    img.pset(x, y, C_GRASS)


def _water(img, tiles, tx, ty, x, y):
    img.rect(x, y, TILE, TILE, C_WATER)
    rx = x + pyxel.rndi(0, 4)
    ry = y + pyxel.rndi(1, 6)
    img.line(rx, ry, rx + 2, ry, C_NAVY)
    if pyxel.rndi(0, 2) == 0:
        img.pset(x + pyxel.rndi(0, 7), y + pyxel.rndi(0, 7), C_CYAN)
    for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        n = _tile_at(tiles, tx + dx, ty + dy)
        if n == T_WATER or n == -1:
            continue
        for i in range(0, TILE, 2):
            if dx == 1:
                img.pset(x + 7, y + i, C_GRAY)
            elif dx == -1:
                img.pset(x, y + i + 1, C_GRAY)
            elif dy == 1:
                img.pset(x + i + 1, y + 7, C_GRAY)
            else:
                img.pset(x + i, y, C_GRAY)


def _mountain(img, tiles, tx, ty, x, y):
    # A peak per tile: lit left face, shaded right face, snow on top.
    img.rect(x, y, TILE, TILE, C_ROCK)
    img.tri(x, y + 7, x + 3, y + 1, x + 3, y + 7, C_GRAY)
    img.tri(x + 4, y + 1, x + 7, y + 7, x + 4, y + 7, C_NAVY)
    img.line(x + 3, y + 1, x + 4, y + 1, C_WHITE)
    img.pset(x + 2, y + 2, C_WHITE)
    img.pset(x + 5, y + 2, C_WHITE)
    if pyxel.rndi(0, 1):
        img.pset(x + pyxel.rndi(1, 6), y + 6, C_BG)


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------
def text_c(y, s, col, cx=80):
    pyxel.text(cx - len(s) * 2, y, s, col)


def text_sh(x, y, s, col, sh=C_BG):
    pyxel.text(x + 1, y + 1, s, sh)
    pyxel.text(x, y, s, col)


def text_csh(y, s, col, sh=C_BG, cx=80):
    x = cx - len(s) * 2
    pyxel.text(x + 1, y + 1, s, sh)
    pyxel.text(x, y, s, col)


def panel(x, y, w, h, border=C_GRAY, fill=C_NAVY):
    pyxel.rect(x, y, w, h, fill)
    pyxel.rectb(x, y, w, h, border)


def logo(cx, y, scale, t):
    # "MESH" over "SIEGE", pixel-scaled, with a drop shadow.
    for i, (v, w, col) in enumerate(((LOGO_V, 15, C_CYAN), (LOGO_V + 8, 19, C_RED))):
        yy = y + i * (7 * scale + 1)
        x = cx - w // 2
        pyxel.pal(C_WHITE, C_BG)
        pyxel.blt(x + 1, yy + 1, 0, 0, v, w, 6, 0, None, scale)
        pyxel.pal(C_WHITE, col if (t // 4 + i) % 24 else C_WHITE)
        pyxel.blt(x, yy, 0, 0, v, w, 6, 0, None, scale)
    pyxel.pal()


def node_sprite(kind, x, y):
    pyxel.blt(x, y, 0, kind * 8, 0, 8, 8, 0)


def star(x, y):
    # The cred star, the height of a line of text.
    pyxel.blt(x, y, 0, SPR_STAR[0], SPR_STAR[1], 5, 5, 0)
