# title: Mesh Siege
# author: Meshpunk
# desc: A LoRa mesh tower defense
# license: MIT
# version: 1.0
import json
import pyxel
from data import *
import gfx
import audio
import ctl
from game import Game
from pilot import Pilot

SAVE_PATH = pyxel.user_data_dir("Meshpunk", "Mesh Siege") + "save.json"
SAVE_VERSION = 3


class App:
    def __init__(self):
        pyxel.init(160, 120, title="Mesh Siege", fps=30)
        gfx.setup()
        audio.setup()
        # Per mode, per map: most waves cleared, whether the map was won, and
        # whether it was won without the gateway ever falling.
        self.best = [[0] * len(MAPS) for _ in MODES]
        self.won = [[False] * len(MAPS) for _ in MODES]
        self.gold = [[False] * len(MAPS) for _ in MODES]
        self.save_ok = True
        self.last_map = 0
        self.mode = NORMAL
        self.music_on = True
        self.demo_map = 0
        self.load()
        audio.set_music(self.music_on)
        self.scene = None
        bot = self.bot_config()
        if bot is not None:
            self.mode = bot[2]
            self.start_game(bot[0])
            import bot as botmod
            self.scene.bot = botmod.Bot(self.scene)
            self.scene.speed = bot[1]
        else:
            self.to_title()
        pyxel.run(self.update, self.draw)

    # A file named _bot.txt next to main.py ("map steps_per_frame mode") runs
    # the test bot instead of the title screen. It is not packaged.
    def bot_config(self):
        try:
            with open("_bot.txt") as f:
                parts = [int(p) for p in f.read().split()]
        except OSError:
            return None
        return (parts + [0, 1, NORMAL][len(parts):])[:3]

    def load(self):
        try:
            with open(SAVE_PATH) as f:
                d = json.load(f)
        except OSError:
            return
        except ValueError:
            return
        # A save in any other shape (an older layout, a hand edit) is ignored.
        # Saves from before version 3 hold other maps' records.
        if d.get("v") != SAVE_VERSION:
            return
        # Maps added later start at zero.
        try:
            best = d["best"]
            won = d["won"]
            gold = d["gold"]
            for m in range(len(MODES)):
                for i in range(len(MAPS)):
                    if i < len(best[m]):
                        self.best[m][i] = int(best[m][i])
                        self.won[m][i] = bool(won[m][i])
                        self.gold[m][i] = bool(gold[m][i])
            self.last_map = int(d["last"]) % len(MAPS)
            self.mode = int(d["mode"]) % len(MODES)
            self.music_on = bool(d.get("music", True))
        except (KeyError, IndexError, TypeError, ValueError):
            self.best = [[0] * len(MAPS) for _ in MODES]
            self.won = [[False] * len(MAPS) for _ in MODES]
            self.gold = [[False] * len(MAPS) for _ in MODES]
            self.last_map = 0
            self.mode = NORMAL

    def save(self):
        try:
            with open(SAVE_PATH, "w") as f:
                json.dump({"v": SAVE_VERSION, "best": self.best, "won": self.won,
                           "gold": self.gold, "last": self.last_map, "mode": self.mode,
                           "music": self.music_on}, f)
            self.save_ok = True
        except OSError:
            self.save_ok = False

    def record(self, mi, mode, waves, won, gold=False):
        changed = False
        if waves > self.best[mode][mi]:
            self.best[mode][mi] = waves
            changed = True
        if won and not self.won[mode][mi]:
            self.won[mode][mi] = True
            changed = True
        if gold and not self.gold[mode][mi]:
            self.gold[mode][mi] = True
            changed = True
        if changed:
            self.save()

    def best_any(self, mi):
        return max(self.best[m][mi] for m in range(len(MODES)))

    def set_music(self, on):
        self.music_on = on
        audio.set_music(on)
        self.save()

    def start_game(self, mi):
        if mi != self.last_map:
            self.last_map = mi
            self.save()
        self.scene = Game(self, mi, self.mode)

    def to_title(self):
        self.scene = Title(self)

    def start_demo(self):
        mi = self.demo_map
        self.demo_map = (mi + 1) % len(MAPS)
        g = Game(self, mi, NORMAL, True)
        g.bot = Pilot(g)
        g.speed = 2
        self.scene = g

    def to_select(self):
        self.scene = MapSelect(self)

    def to_help(self):
        self.scene = Help(self)

    def update(self):
        self.scene.update()

    def draw(self):
        self.scene.draw()


# ---------------------------------------------------------------------------
# Title
# ---------------------------------------------------------------------------
TITLE_NODES = ((12, 108), (34, 99), (58, 111), (82, 101), (106, 113), (130, 102),
               (152, 112))
TITLE_LINKS = ((0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (1, 3), (3, 5))


class Title:
    ITEMS = ("PLAY", "HOW TO PLAY", "QUIT")

    DEMO_AFTER = 450          # idle frames before the demo starts

    def __init__(self, app):
        self.app = app
        self.t = 0
        self.idle = 0
        self.sel = 0
        self.noise = []
        for i in range(4):
            self.noise.append([pyxel.rndf(0, 160), pyxel.rndf(84, 104),
                               pyxel.rndf(0.2, 0.5), pyxel.rndi(0, 2)])
        audio.music(audio.M_TITLE)

    def update(self):
        self.t += 1
        self.idle += 1
        if self.idle > self.DEMO_AFTER:
            self.app.start_demo()
            return
        if ctl.up():
            self.idle = 0
            self.sel = (self.sel - 1) % len(self.ITEMS)
            audio.sfx(audio.MOVE)
        elif ctl.down():
            self.idle = 0
            self.sel = (self.sel + 1) % len(self.ITEMS)
            audio.sfx(audio.MOVE)
        elif ctl.a() or ctl.start():
            audio.sfx(audio.SELECT)
            if self.sel == 0:
                self.app.to_select()
            elif self.sel == 1:
                self.app.to_help()
            else:
                pyxel.quit()
        for n in self.noise:
            n[0] -= n[2]
            if n[0] < -8:
                n[0] = 168
                n[1] = pyxel.rndf(84, 104)

    def draw(self):
        t = self.t
        pyxel.cls(C_BG)
        # Stars
        pyxel.rseed(5)
        for _ in range(40):
            x = pyxel.rndi(0, 159)
            y = pyxel.rndi(0, 70)
            if (x + t // 10) % 7:
                pyxel.pset(x, y, C_NAVY if x % 3 else C_GRAY)
        pyxel.rseed(t)
        # Ground line
        pyxel.rect(0, 90, 160, 30, C_GRASS_D)
        pyxel.line(0, 90, 160, 90, C_GRASS)
        # The mesh
        for a, b in TITLE_LINKS:
            x0, y0 = TITLE_NODES[a]
            x1, y1 = TITLE_NODES[b]
            pyxel.line(x0, y0 - 4, x1, y1 - 4, C_GRASS)
            f = ((t + a * 7) % 40) / 40.0
            pyxel.pset(x0 + (x1 - x0) * f, y0 - 4 + (y1 - y0) * f, C_GREEN)
        for i, (x, y) in enumerate(TITLE_NODES):
            gfx.node_sprite(i % 5, x - 4, y - 8)
            pyxel.pset(x + 3, y - 8, C_GREEN)
        for n in self.noise:
            k = NOISE_KINDS[n[3]]
            v = k.v + (8 if (t // 8) % 2 else 0)
            pyxel.blt(n[0], n[1], 0, k.u, v, -8, 8, 0)
        gfx.logo(80, 19, 3, t)
        # Menu
        for i, s in enumerate(self.ITEMS):
            y = 59 + i * 9
            if i == self.sel:
                gfx.text_csh(y, "> " + s + " <", C_YELLOW)
            else:
                gfx.text_csh(y, s, C_GRAY)
        gfx.text_csh(3, "A LORA TOWER DEFENSE", C_CYAN)


# ---------------------------------------------------------------------------
# Map select
# ---------------------------------------------------------------------------
# Swatch colour per terrain code (help pages).
PREVIEW_COLORS = (C_GRASS, C_GRASS_D, C_HILL, C_GRAY, C_WATER, C_GRASS, C_ROCK, C_DIRT, C_NAVY,
                  C_CYAN, C_RED, C_YELLOW)


class MapSelect:
    def __init__(self, app):
        self.app = app
        self.sel = app.last_map
        self.t = 0
        self.levels = []
        self.mode_changed = False
        self.rendered = -1
        from level import Level
        for m in MAPS:
            self.levels.append(Level(m))
        audio.music(audio.M_TITLE)

    def update(self):
        self.t += 1
        app = self.app
        if ctl.left():
            self.sel = (self.sel - 1) % len(MAPS)
            audio.sfx(audio.MOVE)
        elif ctl.right():
            self.sel = (self.sel + 1) % len(MAPS)
            audio.sfx(audio.MOVE)
        elif ctl.up():
            app.mode = (app.mode - 1) % len(MODES)
            self.mode_changed = True
            audio.sfx(audio.MOVE)
        elif ctl.down():
            app.mode = (app.mode + 1) % len(MODES)
            self.mode_changed = True
            audio.sfx(audio.MOVE)
        elif ctl.a() or ctl.start():
            if self.mode_changed:
                app.save()
            audio.sfx(audio.SELECT)
            self.app.start_game(self.sel)
        elif ctl.b():
            self.app.to_title()

    def draw(self):
        t = self.t
        app = self.app
        m = MAPS[self.sel]
        pyxel.cls(C_BG)
        gfx.text_c(3, "CHOOSE A MAP", C_CYAN)
        # Preview: the map art (image bank 1) at a quarter size, 2 px a tile.
        px = 48
        py = 11
        lv = self.levels[self.sel]
        if self.rendered != self.sel:
            gfx.render_map(lv, 91 + self.sel)
            self.rendered = self.sel
        pyxel.blt(px - 96, py - 96, 1, 0, 0, WORLD_W, WORLD_H, None, None, 0.25)
        pyxel.rectb(px - 1, py - 1, 66, 66, C_GRAY)
        gi = lv.gate
        pyxel.rect(px + (gi % MAP_W) * 2 - 1, py + (gi // MAP_W) * 2 - 1, 4, 4,
                   C_WHITE if (t // 10) % 2 else C_CYAN)
        # Arrows
        pyxel.text(36, 41, "<", C_YELLOW if (t // 8) % 2 else C_GRAY)
        pyxel.text(121, 41, ">", C_YELLOW if (t // 8) % 2 else C_GRAY)
        title = "%d/%d  %s" % (self.sel + 1, len(MAPS), m.name)
        tx = 80 - (len(title) * 4 + 20) // 2
        gfx.text_sh(tx, 79, title, C_WHITE)
        for i in range(3):
            x = tx + len(title) * 4 + 3 + i * 6
            col = C_ORANGE if i < m.stars else C_NAVY
            pyxel.rect(x + 1, 79, 3, 5, col)
            pyxel.rect(x, 80, 5, 3, col)
        gfx.text_c(87, m.blurb, C_GRAY)
        # Mode row
        x = 80 - (len("EASY  NORMAL  HARD") * 4) // 2
        for i, (name, hp, bonus) in enumerate(MODES):
            cleared = app.won[i][self.sel]
            if i == app.mode:
                pyxel.rect(x - 2, 94, len(name) * 4 + 3, 9, C_NAVY)
                pyxel.rectb(x - 2, 94, len(name) * 4 + 3, 9, C_YELLOW)
                pyxel.text(x, 96, name, C_GREEN if cleared else C_YELLOW)
            else:
                pyxel.text(x, 96, name, C_GRASS if cleared else C_ROCK)
            x += len(name) * 4 + 8
        best = app.best[app.mode][self.sel]
        if app.won[app.mode][self.sel]:
            gold = app.gold[app.mode][self.sel]
            s = "CLEARED, GATEWAY NEVER FELL" if gold else "CLEARED  BEST %d" % best
            x = 80 - (len(s) * 4 + 8) // 2
            pyxel.circ(x + 2, 107, 3, C_YELLOW if gold else C_GRAY)
            pyxel.pset(x + 2, 107, C_WHITE)
            pyxel.text(x + 8, 105, s, C_GREEN)
        elif best:
            gfx.text_c(105, "BEST: %d WAVES" % best, C_WHITE)
        elif not app.save_ok:
            gfx.text_c(105, "SAVES NEED AN SD CARD", C_RED)
        else:
            gfx.text_c(105, "NOT PLAYED YET", C_GRAY)
        gfx.text_c(114, "A:DEPLOY  B:BACK  UP/DN:MODE", C_YELLOW)


# ---------------------------------------------------------------------------
# How to play
# ---------------------------------------------------------------------------
class Help:
    PAGES = 9

    def __init__(self, app):
        self.app = app
        self.page = 0
        self.t = 0

    def update(self):
        self.t += 1
        if ctl.left():
            self.page = max(0, self.page - 1)
            audio.sfx(audio.MOVE)
        elif ctl.right() or ctl.a():
            if self.page + 1 < self.PAGES:
                self.page += 1
                audio.sfx(audio.MOVE)
            elif ctl.a():
                self.app.to_title()
        elif ctl.b() or ctl.start():
            self.app.to_title()

    def title(self, s):
        pyxel.rect(0, 0, 160, 10, C_NAVY)
        gfx.text_csh(2, s, C_CYAN)
        pyxel.text(128, 2, "%d/%d" % (self.page + 1, self.PAGES), C_GRAY)

    def draw(self):
        t = self.t
        pyxel.cls(C_BG)
        getattr(self, "page%d" % self.page)(t)
        pyxel.rect(0, 112, 160, 8, C_NAVY)
        pyxel.text(2, 113, "<  >:PAGE", C_GRAY)
        pyxel.text(112, 113, "B:BACK", C_YELLOW)

    def lines(self, y, rows):
        for s, col in rows:
            if s:
                pyxel.text(8, y, s, col)
            y += 7

    def page0(self, t):
        self.title("THE MESH")
        # Gateway -> relay -> ping, each inside the last one's radio circle
        gx, gy = 30, 66
        rx, ry = 52, 64
        px, py = 74, 70
        pyxel.circb(gx, gy, 24, C_GRASS)
        pyxel.circb(rx, ry, 24, C_GRASS)
        pyxel.line(rx, ry - 3, gx, gy - 7, C_GREEN)
        pyxel.line(px, py - 3, rx, ry - 3, C_GREEN)
        for (x0, y0, x1, y1, o) in ((rx, ry - 3, gx, gy - 7, 0), (px, py - 3, rx, ry - 3, 11)):
            f = ((t + o) % 30) / 30.0
            pyxel.pset(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, C_WHITE)
        pyxel.blt(gx - 8, gy - 12, 0, 48, 0, 16, 16, 0)
        gfx.node_sprite(N_RELAY, rx - 4, ry - 4)
        gfx.node_sprite(N_PING, px - 4, py - 4)
        gfx.node_sprite(N_PING, 138, 60)
        pyxel.dither(0.5)
        pyxel.rect(138, 60, 8, 8, C_BG)
        pyxel.dither(1.0)
        pyxel.text(128, 70, "NO LINK", C_RED)
        self.lines(13, (("NOISE FLIES IN FROM EVERY SIDE AND", C_WHITE),
                        ("GOES FOR YOUR NODES. A NODE WORKS", C_WHITE),
                        ("ONLY LINKED: IN AN ONLINE NODE'S", C_GREEN),
                        ("RADIO CIRCLE, WITH A CLEAR VIEW.", C_GREEN)))
        self.lines(92, (("HOMES IN RANGE OF THE MESH EARN CRED.", C_YELLOW),
                        ("GATEWAY DOWN FOR 20 S = GAME OVER.", C_RED),
                        ("SURVIVE 20 WAVES. 6 HOPS AT MOST.", C_GRAY)))

    def page1(self, t):
        self.title("MONEY AND CRED")
        pyxel.text(2, 13, "$", C_YELLOW)
        gfx.star(1, 48)
        self.lines(13, (("MONEY PAYS FOR NODES, UPGRADES,", C_YELLOW),
                        ("BATTERIES, PANELS AND REPAIRS.", C_WHITE),
                        ("IT TRICKLES IN, AND DONATIONS ADD", C_WHITE),
                        ("MORE EVERY 30 S.", C_WHITE),
                        ("", 0),
                        ("CRED IS YOUR STANDING. HOMES SERVED", C_CYAN),
                        ("AND A BIG MESH EARN IT, AND SO DO", C_WHITE),
                        ("KILLS, CLEARED WAVES, EARLY CALLS AND", C_WHITE),
                        ("BAD ACTORS TAKEN DOWN. OUTAGES AND", C_WHITE),
                        ("NULLIFIED NODES COST CRED.", C_RED),
                        ("", 0),
                        ("HQ SPENDS CRED: CREWS, TRUCKS, TOOLS,", C_GREEN),
                        ("ARMOR, BIGGER AND MORE DONATIONS.", C_GREEN),
                        ("SPECIALS COST BOTH; PLAY UNLOCKS THEM.", C_YELLOW)))

    def page2(self, t):
        self.title("TERRAIN")
        rows = ((T_VALLEY, "VALLEY", "SHORT RADIO, HARD TO SEE OUT OF"),
                (T_HILL, "HILL", "RADIO+ RANGE+, SLOW TO INSTALL"),
                (T_MOUNT, "MOUNTAIN", "BEST RADIO, SLOWEST TO INSTALL"),
                (T_FOREST, "FOREST", "TREES BLOCK LOW LINKS"),
                (T_BUILDING, "ROOFTOP", "RADIO+ AND GRID POWER"),
                (T_WATER, "WATER", "RADIO CROSSES IT, TRUCKS DON'T"),
                (T_ROAD, "ROAD", "FAST FOR CREWS; BRIDGES CROSS"),
                (T_HOME, "HOMES", "USERS: CONNECT THEM FOR CRED"))
        for i, (tile, a, b) in enumerate(rows):
            y = 13 + i * 12
            pyxel.rect(6, y + 1, 9, 9, PREVIEW_COLORS[tile])
            pyxel.rectb(6, y + 1, 9, 9, C_GRAY)
            pyxel.text(19, y, a, C_YELLOW)
            pyxel.text(19, y + 6, b, C_WHITE)

    def page3(self, t):
        self.title("POWER AND CLOUDS")
        pyxel.blt(148, 43, 0, gfx.SPR_SUN[0], gfx.SPR_SUN[1], 5, 5, 0)
        self.lines(14, (("GRID NODES NEVER RUN DOWN, BUT GO", C_WHITE),
                        ("ONLY NEAR BUILDINGS, HOMES, THE", C_WHITE),
                        ("GATEWAY OR A SUBSTATION (2 TILES).", C_WHITE),
                        ("", 0),
                        ("SOLAR NODES GO ANYWHERE. SUNSHINE", C_YELLOW),
                        ("CHARGES THE BATTERY; UNDER A CLOUD", C_YELLOW),
                        ("IT ONLY DRAINS. EMPTY = DARK TILL", C_ORANGE),
                        ("IT RECHARGES.", C_ORANGE),
                        ("", 0),
                        ("SOLAR NODE MENU: BAT = BIGGER", C_GREEN),
                        ("BATTERY, SUN = FASTER PANEL.", C_GREEN),
                        ("BUILD MENU: UP/DOWN PICKS POWER.", C_GRAY)))
        x = 130 + (t // 4) % 20
        pyxel.dither(0.3)
        pyxel.elli(x - 14, 101, 28, 10, C_NAVY)
        pyxel.dither(0.6)
        pyxel.elli(x - 12, 97, 24, 8, C_WHITE)
        pyxel.dither(1.0)

    def page4(self, t):
        self.title("BAD ACTORS")
        pyxel.blt(8, 14, 0, gfx.SPR_ACTOR[0], gfx.SPR_ACTOR[1], 8, 8, 0)
        pyxel.text(20, 15, "ROGUE NODES APPEAR AS YOUR MESH", C_RED)
        self.lines(22, (("GROWS. THEIR SIGNALS NULLIFY A NODE:", C_WHITE),
                        ("IT AND ALL BEHIND IT GO DARK.", C_WHITE),
                        ("", 0),
                        ("A NULLIFIED NODE IS THE FIRST CLUE:", C_PURPLE),
                        ("PURPLE DOTS MARK WHERE IT MAY BE.", C_PURPLE),
                        ("EVERY ONLINE NODE WHOSE RADIO REACHES", C_WHITE),
                        ("IT SHRINKS THE ZONE. THREE PIN IT.", C_WHITE),
                        ("", 0),
                        ("THEN A ON IT: A CREW TAKES IT DOWN", C_GREEN),
                        ("FOR CRED. A ON A NULLIFIED NODE", C_GREEN),
                        ("SENDS A CREW TO RESET IT.", C_GREEN)))

    def page5(self, t):
        self.title("CREWS AND REPAIRS")
        pyxel.blt(8, 14, 0, gfx.SPR_TRUCK[0], gfx.SPR_TRUCK[1], 7, 5, 0)
        pyxel.text(20, 14, "BUILDS, UPGRADES, REPAIRS: EACH", C_WHITE)
        self.lines(21, (("SENDS A CREW OUT FROM THE GATEWAY.", C_WHITE),
                        ("ROADS AND BRIDGES ARE QUICK; FAR AND", C_YELLOW),
                        ("HIGH SITES TAKE LONGER.", C_YELLOW),
                        ("", 0),
                        ("NOISE AND STORMS KNOCK NODES DOWN:", C_RED),
                        ("A ON A DOWN NODE SENDS A REPAIR.", C_WHITE),
                        ("IF THE GATEWAY FALLS, A CREW RUSHES", C_WHITE),
                        ("HOME - KEEP ONE CLOSE.", C_WHITE),
                        ("", 0),
                        ("HQ (A ON THE GATEWAY): CRED BUYS MORE", C_CYAN),
                        ("CREWS, FASTER TRUCKS, TOOLS, ARMOR.", C_CYAN)))

    def page6(self, t):
        self.title("NODES")
        for i, k in enumerate(NODE_KINDS):
            y = 13 + i * 18
            gfx.node_sprite(i, 6, y + 2)
            pyxel.text(18, y, "%s $%d" % (k.name, k.cost[0]), C_CYAN)
            pyxel.text(18, y + 7, k.desc[0], C_WHITE)
        pyxel.text(6, 103, "EACH NODE UPGRADES TWICE. SELL: 70%", C_GRAY)

    def page7(self, t):
        self.title("THE NOISE")
        for i, k in enumerate(NOISE_KINDS):
            y = 12 + i * 12
            v = k.v + (k.size if (t // 8) % 2 else 0)
            if k.size == 8:
                pyxel.blt(6, y + 1, 0, k.u, v, 8, 8, 0)
            else:
                pyxel.blt(2, y - 3, 0, k.u, v, 16, 16, 0)
            pyxel.text(22, y, k.name, C_ORANGE if i == E_FLARE else C_RED)
            pyxel.text(22, y + 6, k.desc, C_WHITE)

    def page8(self, t):
        self.title("CONTROLS")
        rows = (("D-PAD", "MOVE; THE MAP SCROLLS"),
                ("A", "BUILD / NODE MENU / CONFIRM"),
                ("B", "BACK, OR FAST FORWARD"),
                ("START", "SEND THE NEXT WAVE EARLY"),
                ("Y", "WHOLE-MAP OVERVIEW"))
        for i, (a, b) in enumerate(rows):
            y = 15 + i * 9
            pyxel.text(10, y, a, C_YELLOW)
            pyxel.text(40, y, b, C_WHITE)
        pyxel.text(10, 62, "KEYS: ARROWS OR WASD MOVE", C_GRAY)
        pyxel.text(10, 69, "A = M/Z/SPACE   B = N/X/BACKSPACE", C_GRAY)
        pyxel.text(10, 76, "START = ENTER   Y = V", C_GRAY)
        pyxel.text(10, 87, "GATEWAY MENU: WAVE, HQ, SPEED, MUSIC", C_GREEN)
        pyxel.text(10, 95, "MENUS PAUSE THE ACTION.", C_GREEN)
        pyxel.text(10, 103, "AN EARLY WAVE EARNS CRED.", C_GREEN)


App()
