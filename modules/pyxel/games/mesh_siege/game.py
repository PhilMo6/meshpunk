# Mesh Siege: one game on one map.
import math
import pyxel
from data import *
from level import Level, tile_center, tile_of, INF
import gfx
import audio
import ctl

PREP = 0          # counting down to the next wave
WAVE = 1          # a wave is running

M_PLAY = 0        # cursor on the map
M_BUILD = 1       # build menu open
M_NODE = 2        # node menu open
M_OVER = 3        # mesh down
M_WIN = 4         # last wave cleared
M_GATE = 5        # gateway menu
M_HQ = 6          # HQ upgrades
M_ACTOR = 7       # bad actor menu
M_MAP = 8         # whole-map overview
M_SPEC = 9        # a node's specials list

GATE_OPTS = ("SEND WAVE", "HQ", "SPEED", "MUSIC", "QUIT")
DEMO_FRAMES = 2700        # title-screen demo length (90 s)

# Crew states
C_IDLE = 0        # parked at the gateway
C_OUT = 1         # driving to a job
C_WORK = 2        # on the job
C_HOME = 3        # driving back to the gateway

J_BUILD = 0
J_UPGRADE = 1
J_REPAIR = 2
J_RESET = 3
J_BATTERY = 4
J_PANEL = 5
J_TAKEDOWN = 6
J_SPECIAL = 7
JOB_NAMES = ("INSTALLING", "UPGRADING", "REPAIRING", "RESETTING", "NEW BATTERY", "NEW PANEL",
             "TAKING DOWN", "FITTING")

# Node menu options
O_UP = 0
O_BAT = 1
O_PANEL = 2
O_FIX = 3
O_SPEC = 4
O_SELL = 5
OPTS_SOLAR = (O_UP, O_BAT, O_PANEL, O_FIX, O_SPEC, O_SELL)
OPTS_GRID = (O_UP, O_FIX, O_SPEC, O_SELL)
OPT_LABELS = ("UP", "BAT", "SUN", "FIX", "SPEC", "SELL")
HQ_SHOW = 5                       # HQ list rows on screen at once

SIDES = ("N", "S", "E", "W")

# First game on the first map: a tip after these waves.
TIPS = {
    1: "HOMES SERVED EARN CRED, NOT MONEY",
    2: "NO GRID OUT THERE? SOLAR GOES ANYWHERE",
    3: "HILLS REACH FARTHER, BUILD SLOWER",
    4: "A DOWN NODE NEEDS A CREW: A ON IT",
    5: "CRED BUYS HQ UPGRADES: A ON GATEWAY",
}
NOTE_TIME = 90                    # frames a message stays in the strip
TIP_TIME = 120
ALARM_TIME = 90                   # frames an arrow points at a node that was hit

BAR_Y = MAP_Y + VIEW_H            # bottom bar
EDGE = 24                         # the view keeps the cursor this far from its edges
OV_X = 4                          # overview: map at 3 px per tile
OV_Y = 12
OV_S = 0.375


class Node:
    def __init__(self, g, kind, t, power):
        self.kind = kind
        self.t = t
        self.tx = t % MAP_W
        self.ty = t // MAP_W
        self.x, self.y = tile_center(self.tx, self.ty)
        self.terrain = g.level.terrain_at(t)
        self.power = power
        self.level = 0
        self.bat_lv = 0
        self.pv_lv = 0
        self.spec = [False] * len(NODE_SPECS)    # specials fitted, by S_*
        self.spent = 0
        self.cd = 0
        self.built = kind == N_GATE
        self.down = False
        self.nulled = False       # hit by a bad actor: off the mesh until reset
        self.flat = False         # solar battery empty
        self.shaded = False
        self.gone = False
        self.job = None           # the job this node is waiting on
        self.online = False
        self.was_online = False
        self.dark = False         # dropped off the mesh after being online
        self.jammed = False
        self.hops = 99
        self.parent = None
        self.covers = []
        self.load = 0             # online nodes linked through this one, itself too
        self.fx = 0
        self.lost = 0
        self.hurt_t = 0
        self.alarm = 0            # frames an off-screen arrow points here after a hit
        self.kills = 0
        self.phase = pyxel.rndi(0, 39)
        self.maxhp = 0
        self.hp = 0
        self.cap = 0
        self.bat = 0.0
        self.apply(g)
        self.hp = self.maxhp
        self.bat = float(self.cap)

    def apply(self, g):
        te = self.terrain
        if self.kind == N_GATE:
            radio, rng, dmg, cd, slow, drain = GATE_RADIO, 0, 0, 0, 0, 0.0
            hp = int(GATE_HP * ARMOR_HP[g.hq[HQ_ARMOR]])
        else:
            k = NODE_KINDS[self.kind]
            lv = self.level
            radio = k.radio[lv] * te.radio
            if self.spec[S_MAST]:
                radio *= MAST_RADIO
            radio = int(radio + 0.5)
            rng = int(k.rng[lv] * te.rng + 0.5) if k.rng[lv] else 0
            dmg, cd, slow, drain = k.dmg[lv], k.cd[lv], k.slow[lv], k.drain
            hp = k.hp[lv] * ARMOR_HP[g.hq[HQ_ARMOR]]
            if self.spec[S_CASE]:
                hp *= CASE_HP
            hp = int(hp)
        self.radio = radio
        self.radio2 = radio * radio
        self.rng = rng
        self.rng2 = rng * rng
        self.dmg = dmg
        self.cdmax = cd
        self.slow = slow
        self.drain = drain
        self.height = te.elev + MASTS[self.kind]
        if self.spec[S_MAST]:
            self.height += MAST_UP
        self.hard = (self.kind == N_RELAY and self.level == 2) or self.spec[S_HARD]
        if self.built and not self.down:
            self.hp += hp - self.maxhp
        self.maxhp = hp
        cap = BAT_CAP[self.bat_lv] if self.power == P_SOLAR else 0
        self.bat += cap - self.cap
        self.cap = cap


class Noise:
    def __init__(self, kind, hp):
        k = NOISE_KINDS[kind]
        self.kind = kind
        self.k = k
        self.hp = hp
        self.maxhp = hp
        self.speed = k.speed
        self.armor = k.armor
        self.jam = k.jam
        self.jam2 = k.jam * k.jam
        self.reward = k.reward
        self.u = k.u
        self.v = k.v
        self.size = k.size
        self.slow = 0.0
        self.slowt = 0
        self.alive = True
        self.hit = 0
        self.flip = False
        self.anim = pyxel.rndi(0, 15)
        self.x = 0.0
        self.y = 0.0
        self.vx = 0.0             # storms: heading across the map
        self.vy = 0.0
        self.target = None
        self.attack = False       # smashing its target (bricks, flares)
        self.smash = -1           # frames of smashing left (-1: not started)
        self.lost_t = 0           # frames spent with no target
        self.park = 0             # jammer: frames of jamming left
        self.hits = 0             # echo: nodes hit so far
        self.last = None          # echo: the node it hit last
        self.zap = ZAP_EVERY
        self.stun = 0             # frames left hanging dazed (storm splits)
        self.retry = 0            # frames until a noise with no target looks again
        self.rem = INF            # squared distance to its target (tower targeting)
        self.actor = None         # bad signal: the bad actor that sent it
        self.seen = True          # inside the mesh's radio coverage


class Actor:
    # A bad actor: a rogue node somewhere on the map.
    def __init__(self, t):
        self.t = t
        self.tx = t % MAP_W
        self.ty = t // MAP_W
        self.x, self.y = tile_center(self.tx, self.ty)
        self.found = False
        self.hits = []            # tiles of the nodes its signals nullified
        self.heard = []           # [node, x, y, distance] per node that has heard it
        self.zone = None          # tiles it may stand on, once there is a clue
        self.send = BAD_SEND
        self.job = None
        self.gone = False
        self.flash = 0            # frames the off-screen alert stays up


class Shot:
    def __init__(self, x, y, target, dmg, src):
        self.x = x
        self.y = y
        self.t = target
        self.tx = target.x
        self.ty = target.y
        self.dmg = dmg
        self.src = src
        self.alive = True


class Job:
    def __init__(self, kind, cost, node=None, actor=None, spec=-1, cred=0):
        self.kind = kind
        self.cost = cost          # dollars paid for it
        self.cred = cred          # cred paid for it
        self.spec = spec          # J_SPECIAL: which node special
        self.node = node
        self.actor = actor
        self.t = node.t if node is not None else actor.t
        self.crew = None


class Crew:
    def __init__(self, g):
        self.x = float(g.gate.x)
        self.y = float(g.gate.y)
        self.tile = g.level.gate
        self.to = g.level.gate
        self.path = []
        self.pi = 0
        self.px = self.x
        self.py = self.y
        self.tf = 1.0
        self.state = C_IDLE
        self.job = None
        self.work = 0
        self.work_total = 1
        self.paused = False       # noise on the site: no work gets done
        self.flip = False


class Game:
    def __init__(self, app, map_index, mode, demo=False):
        self.app = app
        self.mi = map_index
        self.mode_i = mode
        self.demo = demo
        self.demo_t = 0
        self.demo_end = 0
        self.mapdef = MAPS[map_index]
        self.level = Level(self.mapdef)
        gfx.render_map(self.level, 91 + map_index)
        self.hp_scale = self.mapdef.hp_scale * MODES[mode][1]
        self.money = self.mapdef.money + MODES[mode][2]
        self.cred = 0
        self.wave = 0
        self.phase = PREP
        self.countdown = FIRST_COUNTDOWN
        self.mode = M_PLAY
        self.speed = 1
        self.hq = [0] * len(HQ)
        self.nodes = []
        self.grid = [None] * (MAP_W * MAP_H)
        self.gate = Node(self, N_GATE, self.level.gate, P_GRID)
        self.allnodes = [self.gate]
        self.crews = [Crew(self) for _ in range(CREWS_START)]
        self.jobs = []
        self.enemies = []
        self.shots = []
        self.rings = []       # [x, y, r, rmax, col]
        self.beams = []       # [x1, y1, x2, y2, frames left, col]
        self.parts = []       # [x, y, vx, vy, frames left, col]
        self.spawn_q = []
        self.spawn_i = 0
        self.wave_t = 0
        self.hp_mult = 1.0
        self.seen = []
        self.kills = 0
        self.built = 0
        self.nulls = 0
        self.t = 0
        self.any_jammed = False
        self.endless = False
        self.msg = ("", 0, "")    # (text, colour, more text in white)
        self.msg_t = 0
        self.shake = 0
        self.gate_hit = 0
        self.sel = N_PING
        self.power_sel = P_GRID
        self.opt = 0
        self.hq_sel = 0
        self.deny = 0
        self.linkable = None
        self.link_key = -1
        self.bot = None
        self.link_gen = 0
        self.cl_key = -1
        self.cl_val = (None, 0)
        self.confirm = False
        self.hints = map_index == 0 and app.best_any(0) == 0 and not demo
        self.flares = 0
        self.boss = None
        self.next_kinds = wave_kinds(1)
        self.next_sides = wave_sides(1)
        self.online = 0
        self.packets = 0
        self.users = 0
        self.best_users = 0
        self.home_on = [False] * len(self.level.homes)
        self.income_t = 0
        self.income_flash = 0
        self.donate_t = 0
        # Specials: unlocked and owned, and what unlocks the node specials.
        self.spec_open = [False] * len(NODE_SPECS)
        self.hqs_open = [False] * len(HQ_SPECS)
        self.hqs_own = [False] * len(HQ_SPECS)
        self.takedowns = 0
        self.high_builds = 0
        self.jam_kills = 0
        self.downs_fixed = 0
        self.spec_sel = 0
        self.hq_top = 0
        self.actors = []
        self.installs = 0
        self.install_wave = -1
        self.zones_dirty = True
        self.said = set()     # one-off messages already shown
        self.clouds = []      # [x, y, rx, ry]
        self.wind = (0.0, 0.0)
        self.gate_down = 0    # frames left to get the gateway back up
        self.fell = False     # the gateway went down this game
        self.cam_x = 0.0
        self.cam_y = 0.0
        self.cam_tx = 0.0
        self.cam_ty = 0.0
        self.look_at(self.level.gate % MAP_W, self.level.gate // MAP_W)
        self.cam_x = self.cam_tx
        self.cam_y = self.cam_ty
        self.map_x = 0
        self.map_y = 0
        self.set_wind()
        cmax = self.mapdef.clouds[0]
        for _ in range(cmax):
            self.spawn_cloud(False)
        self.rebuild_links()
        if self.hints:
            self.note("WELCOME: BUILD NODES NEAR THE GATEWAY", C_CYAN, TIP_TIME)
        audio.music(audio.M_GAME)

    # -----------------------------------------------------------------
    # View
    # -----------------------------------------------------------------
    def follow(self, snap=False):
        # Keep the cursor EDGE pixels inside the view.
        x = self.cx * TILE
        y = self.cy * TILE
        tx = self.cam_tx
        ty = self.cam_ty
        if x < tx + EDGE:
            tx = x - EDGE
        elif x + TILE > tx + VIEW_W - EDGE:
            tx = x + TILE - VIEW_W + EDGE
        if y < ty + EDGE:
            ty = y - EDGE
        elif y + TILE > ty + VIEW_H - EDGE:
            ty = y + TILE - VIEW_H + EDGE
        self.cam_tx = float(max(0, min(WORLD_W - VIEW_W, tx)))
        self.cam_ty = float(max(0, min(WORLD_H - VIEW_H, ty)))
        if snap:
            self.cam_x = self.cam_tx
            self.cam_y = self.cam_ty

    def ease_view(self):
        dx = self.cam_tx - self.cam_x
        dy = self.cam_ty - self.cam_y
        if dx * dx + dy * dy < 0.5:
            self.cam_x = self.cam_tx
            self.cam_y = self.cam_ty
        else:
            self.cam_x += dx * 0.25
            self.cam_y += dy * 0.25

    def look_at(self, tx, ty):
        self.cx = max(0, min(MAP_W - 1, tx))
        self.cy = max(0, min(MAP_H - 1, ty))
        self.cam_tx = float(max(0, min(WORLD_W - VIEW_W, self.cx * TILE + 4 - VIEW_W // 2)))
        self.cam_ty = float(max(0, min(WORLD_H - VIEW_H, self.cy * TILE + 4 - VIEW_H // 2)))

    # -----------------------------------------------------------------
    # Mesh
    # -----------------------------------------------------------------
    def rebuild_links(self):
        lv = self.level
        nodes = self.nodes
        self.allnodes = [self.gate] + nodes
        for a in self.allnodes:
            if not a.built:
                a.covers = []
                continue
            r2 = a.radio2
            ax = a.x
            ay = a.y
            cov = []
            for b in nodes:
                if b is a or not b.built:
                    continue
                dx = b.x - ax
                dy = b.y - ay
                if dx * dx + dy * dy <= r2 and lv.los(a.t, a.height, b.t, b.height):
                    cov.append(b)
            a.covers = cov
        self.relink()

    def works(self, n):
        return n.built and not n.down and not n.nulled and not n.flat and not n.jammed

    def relink(self):
        nodes = self.nodes
        for n in nodes:
            n.was_online = n.online
            n.online = False
            n.hops = 99
            n.parent = None
            n.load = 0
        g = self.gate
        g.load = 0
        g.hops = 0
        g.online = self.works(g)
        q = []
        if g.online:
            q.append(g)
        i = 0
        while i < len(q):
            u = q[i]
            i += 1
            if u.hops >= MAX_HOPS:
                continue
            h = u.hops + 1
            for v in u.covers:
                if not v.online and self.works(v):
                    v.online = True
                    v.hops = h
                    v.parent = u
                    q.append(v)
        # Each node's load: itself plus everything linked through it.
        for j in range(len(q) - 1, -1, -1):
            u = q[j]
            u.load += 1
            if u.parent is not None:
                u.parent.load += u.load
        lost = False
        back = False
        online = 0
        for n in nodes:
            if n.online:
                online += 1
                if n.dark:
                    n.dark = False
                    n.lost = 0
                    back = True
                    self.rings.append([n.x, n.y, 1, 8, C_GREEN])
            elif n.was_online:
                n.lost = 36
                n.dark = True
                lost = True
        self.online = online
        if lost:
            audio.sfx(audio.LINK_LOST)
        elif back:
            audio.sfx(audio.LINK_UP)
        self.linkable = None
        self.link_gen += 1
        self.update_users()
        self.update_hearing()

    def update_users(self):
        lv = self.level
        hh = TERRAIN[T_HOME].elev + HOME_ANTENNA
        count = 0
        up = self.gate.online
        for i in range(len(lv.homes)):
            h = lv.homes[i]
            hx, hy = tile_center(h % MAP_W, h // MAP_W)
            on = False
            if up:
                for n in self.allnodes:
                    if not n.online:
                        continue
                    dx = hx - n.x
                    dy = hy - n.y
                    if dx * dx + dy * dy <= n.radio2 and lv.los(n.t, n.height, h, hh):
                        on = True
                        break
            self.home_on[i] = on
            if on:
                count += 1
        self.users = count
        if count > self.best_users:
            self.best_users = count
            self.check_unlocks()

    def link_for(self, tx, ty, mast):
        # Best online parent for a node with this mast on (tx, ty).
        lv = self.level
        t = ty * MAP_W + tx
        height = lv.terrain_at(t).elev + mast
        x, y = tile_center(tx, ty)
        best = None
        for u in self.allnodes:
            if not u.online or u.hops >= MAX_HOPS:
                continue
            if best is not None and u.hops >= best.hops:
                continue
            dx = x - u.x
            dy = y - u.y
            if dx * dx + dy * dy <= u.radio2 and lv.los(u.t, u.height, t, height):
                best = u
        if best is None:
            return None, 0
        return best, best.hops + 1

    def cursor_link(self):
        # link_for() under the cursor for the selected kind, recomputed only
        # when it can change.
        key = (self.link_gen * 1024 + self.cy * MAP_W + self.cx) * 8 + self.sel
        if key != self.cl_key:
            self.cl_key = key
            self.cl_val = self.link_for(self.cx, self.cy, MASTS[self.sel])
        return self.cl_val

    def linkable_tiles(self):
        # Empty buildable tiles in and around the view that would link.
        x0 = max(0, int(self.cam_tx) // TILE - 2)
        y0 = max(0, int(self.cam_ty) // TILE - 2)
        key = (self.link_gen * 64 + x0) * 64 + y0
        if self.linkable is None or key != self.link_key:
            self.link_key = key
            out = []
            lv = self.level
            for ty in range(y0, min(MAP_H, y0 + VIEW_H // TILE + 4)):
                for tx in range(x0, min(MAP_W, x0 + VIEW_W // TILE + 4)):
                    if lv.buildable(tx, ty) and self.grid[ty * MAP_W + tx] is None:
                        p, h = self.link_for(tx, ty, 0.5)
                        if p is not None:
                            out.append((tx, ty))
            self.linkable = out
        return self.linkable

    def node_at(self, tx, ty):
        return self.grid[ty * MAP_W + tx]

    def actor_at(self, tx, ty):
        t = ty * MAP_W + tx
        for a in self.actors:
            if a.found and not a.gone and a.t == t:
                return a
        return None

    # -----------------------------------------------------------------
    # Power and clouds
    # -----------------------------------------------------------------
    def set_wind(self):
        a = pyxel.rndi(0, 7) * 0.7854
        s = pyxel.rndf(CLOUD_SPEED[0], CLOUD_SPEED[1])
        self.wind = (math.cos(a) * s, math.sin(a) * s)

    def spawn_cloud(self, upwind):
        cmax, rmin, rmax = self.mapdef.clouds
        rx = pyxel.rndi(rmin, rmax)
        ry = rx * 3 // 5
        x = pyxel.rndf(0, WORLD_W)
        y = MAP_Y + pyxel.rndf(0, WORLD_H)
        if upwind:
            wx, wy = self.wind
            if abs(wx) >= abs(wy):
                x = -rx if wx > 0 else WORLD_W + rx
            else:
                y = MAP_Y - ry if wy > 0 else MAP_Y + WORLD_H + ry
        self.clouds.append([x, y, rx, ry])

    def move_clouds(self):
        wx, wy = self.wind
        keep = []
        for c in self.clouds:
            c[0] += wx
            c[1] += wy
            if (-c[2] * 2 < c[0] < WORLD_W + c[2] * 2 and
                    MAP_Y - c[3] * 2 < c[1] < MAP_Y + WORLD_H + c[3] * 2):
                keep.append(c)
        self.clouds = keep
        want = min(8, self.mapdef.clouds[0] + self.wave // 7)
        if len(keep) < want and self.t % 60 == 0:
            self.spawn_cloud(True)

    def update_power(self):
        changed = False
        clouds = self.clouds
        for n in self.nodes:
            if n.power != P_SOLAR or not n.built:
                continue
            sh = False
            for c in clouds:
                dx = (n.x - c[0]) / c[2]
                dy = (n.y - c[1]) / c[3]
                if dx * dx + dy * dy <= 1.0:
                    sh = True
                    break
            n.shaded = sh
            use = n.drain * POWER_EVERY
            gain = 0.0 if sh else use * PANEL_RATE[n.pv_lv]
            if n.flat or n.down:
                use = 0.0
            b = n.bat + gain - use
            if b < 0.0:
                b = 0.0
            elif b > n.cap:
                b = float(n.cap)
            n.bat = b
            if not n.flat and b <= 0.0:
                n.flat = True
                changed = True
                if "flat" not in self.said:
                    self.said.add("flat")
                    self.note("BATTERY FLAT: SHADE STOPS SOLAR", C_ORANGE)
            elif n.flat and b >= n.cap * BAT_WAKE:
                n.flat = False
                changed = True
        if changed:
            self.relink()

    # -----------------------------------------------------------------
    # Bad actors
    # -----------------------------------------------------------------
    def maybe_install(self):
        if self.installs >= ACTOR_CAP[self.mode_i] or self.install_wave == self.wave:
            return
        th = self.mapdef.actors
        if self.installs >= len(th):
            return
        count = 0
        for n in self.nodes:
            if n.built:
                count += 1
        if count < th[self.installs]:
            return
        t = self.actor_spot()
        if t < 0:
            return
        a = Actor(t)
        a.send = BAD_SEND // 2
        self.actors.append(a)
        self.installs += 1
        self.install_wave = self.wave

    def actor_spot(self):
        # Where a new bad actor goes: off the mesh's air, near the branches
        # it can do the most harm to.
        lv = self.level
        built = [n for n in self.allnodes if n.built]
        online = [n for n in self.allnodes if n.online]
        targets = [n for n in self.nodes if n.built]
        r2 = BAD_RANGE * BAD_RANGE
        cands = []
        off = pyxel.rndi(0, 1)
        for ty in range(off, MAP_H, 2):
            for tx in range(off, MAP_W, 2):
                t = ty * MAP_W + tx
                if not lv.reach[t] or lv.tiles[t] == T_WATER or self.grid[t] is not None:
                    continue
                x, y = tile_center(tx, ty)
                ok = True
                for n in built:
                    dx = n.x - x
                    dy = n.y - y
                    if dx * dx + dy * dy < 576:
                        ok = False
                        break
                if not ok:
                    continue
                for n in online:
                    dx = n.x - x
                    dy = n.y - y
                    if dx * dx + dy * dy <= n.radio2:
                        ok = False
                        break
                if not ok:
                    continue
                score = 0
                for n in targets:
                    dx = n.x - x
                    dy = n.y - y
                    if dx * dx + dy * dy <= r2:
                        score += n.load + 1
                if score:
                    cands.append((score, t))
        if not cands:
            return -1
        cands.sort(key=lambda c: -c[0])
        top = cands[:max(1, len(cands) // 4)]
        return top[pyxel.rndi(0, len(top) - 1)][1]

    def actor_target(self, a):
        # The node in reach that carries the most of the mesh; signed
        # firmware keeps a node out of its reach.
        best = None
        bk = -1
        r2 = BAD_RANGE * BAD_RANGE
        for n in self.nodes:
            if not n.built or n.down or n.nulled or n.spec[S_SIGNED]:
                continue
            dx = n.x - a.x
            dy = n.y - a.y
            d2 = dx * dx + dy * dy
            if d2 <= r2:
                k = n.load * 100000 - d2
                if k > bk:
                    best = n
                    bk = k
        return best

    def update_actors(self):
        if self.phase != WAVE:
            return
        for a in self.actors:
            if a.gone:
                continue
            a.send -= 1
            if a.send <= 0:
                a.send = max(BAD_SEND_MIN, BAD_SEND - 15 * self.wave)
                n = self.actor_target(a)
                if n is not None:
                    e = Noise(E_BAD, NOISE_KINDS[E_BAD].hp * self.hp_mult)
                    e.x = float(a.x)
                    e.y = float(a.y)
                    e.actor = a
                    e.target = n
                    e.seen = False
                    self.enemies.append(e)

    def nullify(self, n, a):
        n.nulled = True
        n.alarm = ALARM_TIME
        self.nulls += 1
        self.lose_cred(NULL_CRED)
        self.burst(n.x, n.y, 10, C_PURPLE)
        self.rings.append([n.x, n.y, 1, 12, C_PURPLE])
        audio.sfx(audio.NULLED)
        self.relink()
        if a is not None and not a.gone:
            if n.t not in a.hits:
                a.hits.append(n.t)
                a.flash = 150
                self.actor_zone(a)
        if "null" not in self.said:
            self.said.add("null")
            self.note("NODE NULLIFIED!", C_PURPLE, NOTE_TIME, " PURPLE DOTS: BAD ACTOR")
        for e in self.enemies:
            if e.target is n and e.kind == E_BAD:
                self.retarget(e)

    def heard_needed(self):
        return 2 if self.hqs_own[H_DF] else BAD_HEARD

    def pin(self, a):
        a.found = True
        a.flash = 240
        self.zones_dirty = True
        self.rings.append([a.x, a.y, 1, 24, C_RED])
        audio.sfx(audio.PINNED)
        self.note("BAD ACTOR PINNED!", C_YELLOW, NOTE_TIME, " A ON IT TO TAKE DOWN")

    def update_hearing(self):
        # Each online node whose radio reaches a hidden bad actor takes a
        # bearing on it: a distance ring that stays a clue for good.
        # heard_needed() of them pin it.
        for a in self.actors:
            if a.found or a.gone:
                continue
            new = False
            for n in self.allnodes:
                if not n.online:
                    continue
                dx = n.x - a.x
                dy = n.y - a.y
                d2 = dx * dx + dy * dy
                if d2 > n.radio2:
                    continue
                known = False
                for h in a.heard:
                    if h[0] is n:
                        known = True
                        break
                if not known:
                    a.heard.append([n, n.x, n.y, math.sqrt(d2)])
                    new = True
            if not new:
                continue
            if len(a.heard) >= self.heard_needed():
                self.pin(a)
            else:
                self.actor_zone(a)
                a.flash = 150
                audio.sfx(audio.CLUE)

    def actor_zone(self, a):
        # Tiles the bad actor may stand on: within its reach of every node
        # it hit, on the distance ring of every node that hears it.
        self.zones_dirty = True
        if not a.hits and not a.heard:
            a.zone = None
            return
        lv = self.level
        r2 = BAD_RANGE * BAD_RANGE
        hits = [tile_center(t % MAP_W, t // MAP_W) for t in a.hits]
        rings = []
        for n, hx, hy, d in a.heard:
            lo = max(0.0, d - BAD_RING)
            hi = d + BAD_RING
            rings.append((hx, hy, lo * lo, hi * hi))
        if hits:
            cx, cy = hits[0]
            r = BAD_RANGE
        else:
            cx, cy = rings[0][0], rings[0][1]
            r = int(math.sqrt(rings[0][3])) + 1
        tx0 = max(0, (cx - r) // TILE)
        tx1 = min(MAP_W - 1, (cx + r) // TILE)
        ty0 = max(0, (cy - MAP_Y - r) // TILE)
        ty1 = min(MAP_H - 1, (cy - MAP_Y + r) // TILE)
        zone = []
        for ty in range(ty0, ty1 + 1):
            for tx in range(tx0, tx1 + 1):
                t = ty * MAP_W + tx
                if lv.tiles[t] == T_WATER:
                    continue
                x, y = tile_center(tx, ty)
                ok = True
                for hx, hy in hits:
                    if (x - hx) * (x - hx) + (y - hy) * (y - hy) > r2:
                        ok = False
                        break
                if ok:
                    for rx, ry, lo2, hi2 in rings:
                        d2 = (x - rx) * (x - rx) + (y - ry) * (y - ry)
                        if d2 < lo2 or d2 > hi2:
                            ok = False
                            break
                if ok:
                    zone.append(t)
        a.zone = zone

    def draw_zones_layer(self):
        # Search zones, baked into image bank 2 (world sized) when they change.
        img = pyxel.images[2]
        img.cls(0)
        for a in self.actors:
            if a.found or a.gone or a.zone is None:
                continue
            for t in a.zone:
                x = (t % MAP_W) * TILE
                y = (t // MAP_W) * TILE
                img.pset(x + 2, y + 2, C_PURPLE)
                img.pset(x + 6, y + 5, C_PURPLE)
        self.zones_dirty = False

    # -----------------------------------------------------------------
    # Orders and crews
    # -----------------------------------------------------------------
    def job_frames(self, job):
        tool = TOOL_TIME[self.hq[HQ_TOOLS]]
        k = job.kind
        if k == J_TAKEDOWN:
            return int(TAKEDOWN_WORK * tool) + 1
        n = job.node
        if k == J_RESET:
            base = RESET_WORK
        elif k == J_BATTERY or k == J_PANEL:
            base = POWER_WORK
        elif k == J_SPECIAL:
            base = SPEC_WORK
        elif n.kind == N_GATE:
            base = GATE_REPAIR_WORK
        else:
            base = NODE_KINDS[n.kind].install * n.terrain.install
            if k == J_UPGRADE:
                base *= UPGRADE_WORK
            elif k == J_REPAIR:
                base *= 0.2 + 0.6 * (1.0 - n.hp / n.maxhp)
        if k == J_REPAIR and self.hqs_own[H_SPARE]:
            base *= 0.5
        return int(base * tool) + 1

    def travel_frames(self, t):
        # Driving time from the gateway, for the build menu's estimate.
        d = self.level.route[0][t]
        if d >= INF:
            return 0
        return int(d * 0.8 / (CREW_SPEED * TRUCK_SPEED[self.hq[HQ_TRUCKS]]))

    def free_crews(self):
        c = 0
        for cr in self.crews:
            if cr.state == C_IDLE or cr.state == C_HOME:
                c += 1
        return c

    def queue(self, job):
        if job.node is not None:
            job.node.job = job
        if job.actor is not None:
            job.actor.job = job
        self.jobs.append(job)
        self.dispatch()

    def order_build(self, kind, tx, ty, power):
        cost = NODE_KINDS[kind].cost[0]
        t = ty * MAP_W + tx
        lv = self.level
        if (self.money < cost or not lv.buildable(tx, ty) or self.grid[t] is not None or
                (power == P_GRID and not lv.power[t])):
            self.denied()
            return None
        n = Node(self, kind, t, power)
        n.spent = cost
        self.money -= cost
        self.nodes.append(n)
        self.allnodes.append(n)
        self.grid[t] = n
        self.linkable = None
        self.queue(Job(J_BUILD, cost, n))
        audio.sfx(audio.ORDER)
        if self.hints and self.wave == 0 and self.built == 0:
            self.note("CREW DISPATCHED: LIVE WHEN THEY'RE DONE", C_CYAN, TIP_TIME)
        return n

    def upgrade_cost(self, n):
        if n.level >= 2:
            return 0
        return NODE_KINDS[n.kind].cost[n.level + 1]

    def battery_cost(self, n):
        if n.power != P_SOLAR or n.bat_lv >= 2:
            return 0
        return BAT_COST[n.bat_lv]

    def panel_cost(self, n):
        if n.power != P_SOLAR or n.pv_lv >= 2:
            return 0
        return PANEL_COST[n.pv_lv]

    def can_fix(self, n):
        return n.built and (n.nulled or n.hp < n.maxhp)

    def repair_cost(self, n):
        if n.nulled:
            return RESET_COST
        if not n.built or n.hp >= n.maxhp:
            return 0
        c = int(n.spent * REPAIR_COST * (1.0 - n.hp / n.maxhp))
        if c < 5:
            c = 5
        if self.hqs_own[H_SPARE]:
            c = (c + 1) // 2
        return c

    def order(self, n, kind, cost):
        # cost 0 means there is no such job to do, except for a reset.
        if ((cost == 0 and kind != J_RESET) or self.money < cost or n.job is not None or
                not n.built):
            self.denied()
            return False
        self.money -= cost
        if kind != J_REPAIR and kind != J_RESET:
            n.spent += cost
        self.queue(Job(kind, cost, n))
        audio.sfx(audio.ORDER)
        return True

    def order_upgrade(self, n):
        return self.order(n, J_UPGRADE, self.upgrade_cost(n))

    def order_repair(self, n):
        if not self.can_fix(n):
            self.denied()
            return False
        return self.order(n, J_RESET if n.nulled else J_REPAIR, self.repair_cost(n))

    def order_battery(self, n):
        return self.order(n, J_BATTERY, self.battery_cost(n))

    def order_panel(self, n):
        return self.order(n, J_PANEL, self.panel_cost(n))

    def order_takedown(self, a):
        if a.job is not None or not a.found or a.gone:
            self.denied()
            return False
        self.queue(Job(J_TAKEDOWN, 0, actor=a))
        audio.sfx(audio.ORDER)
        return True

    def can_special(self, n, i):
        name, desc, cost, cred, need, how = NODE_SPECS[i]
        return (self.spec_open[i] and not n.spec[i] and n.built and n.job is None and
                self.money >= cost and self.cred >= cred)

    def order_special(self, n, i):
        if not self.can_special(n, i):
            self.denied()
            return False
        name, desc, cost, cred, need, how = NODE_SPECS[i]
        self.money -= cost
        self.cred -= cred
        n.spent += cost
        self.queue(Job(J_SPECIAL, cost, n, spec=i, cred=cred))
        audio.sfx(audio.ORDER)
        return True

    def sell_value(self, n):
        pending = n.job.cost if n.job is not None else 0
        if not n.built:
            return n.spent
        return int((n.spent - pending) * SELL_RATE) + pending

    def sell(self, n):
        self.money += self.sell_value(n)
        job = n.job
        if job is not None:
            self.cred += job.cred
            if job in self.jobs:
                self.jobs.remove(job)
            c = job.crew
            if c is not None:
                c.job = None
                self.send_home(c)
        n.gone = True
        self.nodes.remove(n)
        self.grid[n.t] = None
        if n.built:
            self.rebuild_links()
        else:
            self.allnodes = [self.gate] + self.nodes
            self.linkable = None
        for e in self.enemies:
            if e.target is n:
                self.retarget(e)
        self.burst(n.x, n.y, 6, C_GRAY)
        audio.sfx(audio.SELL)

    def denied(self):
        self.deny = 12
        audio.sfx(audio.ERROR)

    def buy_hq(self, i):
        # HQ tracks cost cred.
        costs = HQ[i][2]
        lv = self.hq[i]
        if lv >= len(costs) or self.cred < costs[lv]:
            self.denied()
            return
        self.cred -= costs[lv]
        self.hq[i] = lv + 1
        if i == HQ_CREWS:
            self.crews.append(Crew(self))
            self.dispatch()
        elif i == HQ_ARMOR:
            for n in self.allnodes:
                n.apply(self)
        audio.sfx(audio.UPGRADE)
        self.check_unlocks()

    def buy_hq_special(self, i):
        name, desc, cost, cred, rank, homes = HQ_SPECS[i]
        if (self.hqs_own[i] or not self.hqs_open[i] or self.money < cost or
                self.cred < cred):
            self.denied()
            return
        self.money -= cost
        self.cred -= cred
        self.hqs_own[i] = True
        if i == H_DF:
            for a in self.actors:
                if not a.found and not a.gone and len(a.heard) >= self.heard_needed():
                    self.pin(a)
        elif i == H_UPLINK and self.gate.down:
            self.gate_down += GATE_DOWN_LIMIT
        audio.sfx(audio.UPGRADE)

    def unlock_counts(self):
        # Per node special (S_*): what the player has done toward it.
        return (self.takedowns, self.high_builds, self.jam_kills, self.downs_fixed)

    def check_unlocks(self):
        # Node specials unlock by what the player has done, HQ specials by HQ
        # rank (levels bought) and the most homes served at once.
        done = self.unlock_counts()
        for i in range(len(NODE_SPECS)):
            if not self.spec_open[i] and done[i] >= NODE_SPECS[i][4]:
                self.spec_open[i] = True
                self.note("UNLOCKED: " + NODE_SPECS[i][0], C_YELLOW, NOTE_TIME,
                          " NODE MENU: SPEC")
        rank = sum(self.hq)
        for i in range(len(HQ_SPECS)):
            s = HQ_SPECS[i]
            if not self.hqs_open[i] and rank >= s[4] and self.best_users >= s[5]:
                self.hqs_open[i] = True
                self.note("UNLOCKED: " + s[0], C_YELLOW, NOTE_TIME, " AT HQ")

    def spec_lock(self, i):
        # What still unlocks node special i.
        name, desc, cost, cred, need, how = NODE_SPECS[i]
        if need > 1:
            return "LOCKED: %s %d/%d" % (how, self.unlock_counts()[i], need)
        return "LOCKED: " + how

    def hqs_lock(self, i):
        s = HQ_SPECS[i]
        return "LOCKED: HQ RANK %d/%d  HOMES %d/%d" % (
            min(sum(self.hq), s[4]), s[4], min(self.best_users, s[5]), s[5])

    def auto_repair(self):
        # NET MONITOR: down and nullified nodes get a crew unasked, once the
        # repair can be paid for.
        for n in self.nodes:
            if n.built and n.job is None and (n.down or n.nulled):
                c = self.repair_cost(n)
                if self.money >= c:
                    self.order(n, J_RESET if n.nulled else J_REPAIR, c)

    def lose_cred(self, c):
        self.cred = self.cred - c if self.cred > c else 0

    def gate_limit(self):
        # Frames the gateway may stay down.
        return GATE_DOWN_LIMIT * 2 if self.hqs_own[H_UPLINK] else GATE_DOWN_LIMIT

    def dispatch(self):
        # Give queued jobs to crews that are free.
        if not self.jobs:
            return
        for c in self.crews:
            if not self.jobs:
                return
            if c.state == C_IDLE or c.state == C_HOME:
                self.send(c, self.jobs.pop(0))

    def send(self, c, job):
        c.job = job
        job.crew = c
        c.state = C_OUT
        self.drive(c, job.t)

    def send_home(self, c):
        c.job = None
        if self.jobs:
            self.send(c, self.jobs.pop(0))
            return
        c.state = C_HOME
        self.drive(c, self.level.gate)

    def drive(self, c, dest):
        c.path = self.level.path(c.tile, dest)
        c.pi = 0
        if c.path:
            self.crew_step(c)
        else:
            self.crew_arrived(c)

    def crew_step(self, c):
        nt = c.path[c.pi]
        c.to = nt
        cx, cy = tile_center(nt % MAP_W, nt // MAP_W)
        c.px = cx
        c.py = cy + 1
        if cx != c.x:
            c.flip = cx < c.x
        c.tf = 10.0 / self.level.crew[nt]

    def crew_arrived(self, c):
        if c.state == C_OUT:
            c.state = C_WORK
            c.work = 0
            c.work_total = self.job_frames(c.job)
        else:
            c.state = C_IDLE

    def finish_job(self, c):
        job = c.job
        k = job.kind
        if k == J_TAKEDOWN:
            a = job.actor
            a.job = None
            a.gone = True
            self.zones_dirty = True
            cred = bad_cred(self.wave)
            self.cred += cred
            self.takedowns += 1
            self.burst(a.x, a.y, 16, C_RED)
            self.rings.append([a.x, a.y, 1, 20, C_YELLOW])
            audio.sfx(audio.TAKEN)
            self.note("BAD ACTOR OFF THE AIR", C_GREEN, NOTE_TIME, " +%d CRED" % cred)
            self.check_unlocks()
            self.send_home(c)
            return
        n = job.node
        n.job = None
        if k == J_BUILD:
            n.built = True
            n.hp = n.maxhp
            n.bat = float(n.cap)
            self.built += 1
            self.rebuild_links()
            self.rings.append([n.x, n.y, 1, 10, C_GREEN if n.online else C_RED])
            audio.sfx(audio.BUILD)
            if self.hints and self.wave == 0 and self.built == 1:
                if n.online:
                    self.note("NODE ONLINE! START SENDS THE WAVE", C_GREEN, TIP_TIME)
                else:
                    self.note("NO LINK: BUILD IN A GREEN CIRCLE", C_RED, TIP_TIME)
            if self.level.tiles[n.t] in HIGH_GROUND:
                self.high_builds += 1
                self.check_unlocks()
            self.maybe_install()
        elif k == J_UPGRADE:
            n.level += 1
            n.apply(self)
            n.hp = n.maxhp
            n.down = False
            self.rebuild_links()
            self.rings.append([n.x, n.y, 1, 12, C_YELLOW])
            audio.sfx(audio.UPGRADE)
        elif k == J_BATTERY or k == J_PANEL:
            if k == J_BATTERY:
                n.bat_lv += 1
                n.apply(self)
            else:
                n.pv_lv += 1
            self.rings.append([n.x, n.y, 1, 10, C_YELLOW])
            audio.sfx(audio.UPGRADE)
        elif k == J_RESET:
            n.nulled = False
            self.relink()
            self.rings.append([n.x, n.y, 1, 10, C_CYAN])
            audio.sfx(audio.REPAIRED)
        elif k == J_SPECIAL:
            n.spec[job.spec] = True
            n.apply(self)
            self.rebuild_links()
            self.rings.append([n.x, n.y, 1, 12, C_YELLOW])
            audio.sfx(audio.UPGRADE)
        else:
            was_down = n.down
            n.hp = n.maxhp
            n.down = False
            if n is self.gate:
                self.gate_down = 0
                self.note("GATEWAY BACK UP", C_GREEN)
            self.relink()
            self.rings.append([n.x, n.y, 1, 10, C_CYAN])
            audio.sfx(audio.REPAIRED)
            if was_down:
                self.downs_fixed += 1
                self.check_unlocks()
        self.send_home(c)

    def move_crews(self):
        speed = CREW_SPEED * TRUCK_SPEED[self.hq[HQ_TRUCKS]]
        for c in self.crews:
            st = c.state
            if st == C_IDLE:
                continue
            if st == C_WORK:
                c.paused = not self.site_safe(c.job)
                if c.paused:
                    continue
                c.work += 1
                if c.work >= c.work_total:
                    self.finish_job(c)
                continue
            sp = speed * c.tf
            dx = c.px - c.x
            dy = c.py - c.y
            d2 = dx * dx + dy * dy
            if d2 > sp * sp:
                f = sp / math.sqrt(d2)
                c.x += dx * f
                c.y += dy * f
                continue
            c.x = c.px
            c.y = c.py
            c.tile = c.to
            c.pi += 1
            if c.pi >= len(c.path):
                self.crew_arrived(c)
            else:
                self.crew_step(c)

    def site_safe(self, job):
        # Crews down tools while noise is on top of their site.
        x, y = tile_center(job.t % MAP_W, job.t // MAP_W)
        r2 = CREW_SAFE * CREW_SAFE
        for e in self.enemies:
            if e.alive and e.k.aim != AIM_WIND:
                dx = e.x - x
                dy = e.y - y
                if dx * dx + dy * dy <= r2:
                    return False
        return True

    def node_down(self, n):
        n.down = True
        n.hp = 0
        n.alarm = ALARM_TIME
        self.burst(n.x, n.y, 10, C_RED)
        self.shake = 4
        audio.sfx(audio.NODE_DOWN)
        if n is self.gate:
            self.gate_down = self.gate_limit()
            self.fell = True
            self.gate_hit = 16
            self.rescue_gateway()
            self.note("GATEWAY DOWN!", C_RED, NOTE_TIME,
                      " %d S TO FIX IT" % (self.gate_down // 30))
            audio.sfx(audio.ALARM)
        self.relink()
        for e in self.enemies:
            if e.target is n:
                self.retarget(e)

    def rescue_gateway(self):
        # The gateway's repair skips the queue: a free crew takes it, or else
        # the crew nearest home drops what it is doing.
        g = self.gate
        if g.job is not None:
            return
        job = Job(J_REPAIR, 0, g)
        g.job = job
        for c in self.crews:
            if c.state == C_IDLE or c.state == C_HOME:
                self.send(c, job)
                return
        best = None
        bd = INF
        for c in self.crews:
            dx = c.x - g.x
            dy = c.y - g.y
            d = dx * dx + dy * dy
            if d < bd:
                best = c
                bd = d
        old = best.job
        if old is not None:
            old.crew = None
            self.jobs.insert(0, old)
        self.send(best, job)

    # -----------------------------------------------------------------
    # Waves
    # -----------------------------------------------------------------
    def can_call(self):
        return self.phase == PREP

    def start_wave(self, early):
        if early:
            self.cred += self.countdown // EARLY_EVERY
        self.wave += 1
        self.phase = WAVE
        self.wave_t = 0
        self.hp_mult = wave_hp_mult(self.wave) * self.hp_scale
        self.set_wind()
        q = []
        for kind, count, gap, delay, side in wave_groups(self.wave):
            for i in range(count):
                q.append((delay + i * gap, kind, side))
        q.sort(key=lambda e: e[0])
        self.spawn_q = q
        self.spawn_i = 0
        self.next_kinds = wave_kinds(self.wave + 1)
        self.next_sides = wave_sides(self.wave + 1)
        new = None
        for k in wave_kinds(self.wave):
            if k not in self.seen:
                self.seen.append(k)
                if new is None:
                    new = k
        if self.wave == LAST_WAVE and not self.endless:
            self.note("FINAL WAVE:", C_ORANGE, NOTE_TIME, " TWO FLARES INBOUND")
        elif new is not None and self.wave > 1:
            nk = NOISE_KINDS[new]
            self.note(nk.name, C_ORANGE if new == E_FLARE else C_RED, NOTE_TIME,
                      " - " + nk.desc)
        audio.sfx(audio.WAVE)
        self.maybe_install()

    def spawn(self, kind, side):
        k = NOISE_KINDS[kind]
        e = Noise(kind, k.hp * self.hp_mult)
        if side == "A":
            side = SIDES[pyxel.rndi(0, 3)]
        if side == "N" or side == "S":
            e.x = pyxel.rndf(8, WORLD_W - 8)
            e.y = float(MAP_Y - 8 if side == "N" else MAP_Y + WORLD_H + 8)
            e.vy = 1.0 if side == "N" else -1.0
            e.vx = pyxel.rndf(-0.4, 0.4)
        else:
            e.x = float(-8 if side == "W" else WORLD_W + 8)
            e.y = MAP_Y + pyxel.rndf(8, WORLD_H - 8)
            e.vx = 1.0 if side == "W" else -1.0
            e.vy = pyxel.rndf(-0.4, 0.4)
        d = math.sqrt(e.vx * e.vx + e.vy * e.vy)
        e.vx /= d
        e.vy /= d
        self.enemies.append(e)
        self.retarget(e)
        if kind == E_FLARE:
            self.flares += 1
            self.boss = e
            audio.sfx(audio.BOSS)

    def spawn_split(self, x, y, hp):
        # Chirps from a storm, hanging dazed before they pick a node.
        for i in range(STORM_SPLIT):
            e = Noise(E_CHIRP, hp)
            e.x = float(x + pyxel.rndi(-3, 3))
            e.y = float(y + pyxel.rndi(-3, 3))
            e.stun = SPLIT_STUN
            self.enemies.append(e)
            self.retarget(e)

    def nearest_node(self, e, skip=None):
        best = None
        bd = INF
        for n in self.allnodes:
            if not n.built or n.down or n is skip:
                continue
            dx = n.x - e.x
            dy = n.y - e.y
            d = dx * dx + dy * dy
            if d < bd:
                best = n
                bd = d
        return best

    def retarget(self, e):
        # Point a noise at the node it is after.
        e.attack = False
        e.target = None
        aim = e.k.aim
        if aim == AIM_WIND:
            return
        if e.kind == E_BAD:
            a = e.actor
            if a is not None and not a.gone:
                e.target = self.actor_target(a)
        elif aim == AIM_GATE:
            # Even a downed gateway: camping on it keeps the crews off.
            e.target = self.gate
        elif aim == AIM_BRANCH:
            best = None
            bk = -1
            for n in self.nodes:
                if n.online and not n.hard:
                    dx = n.x - e.x
                    dy = n.y - e.y
                    k = n.load * 100000 - int(dx * dx + dy * dy)
                    if k > bk:
                        best = n
                        bk = k
            e.target = best
        elif aim == AIM_BOUNCE:
            e.target = self.nearest_node(e, e.last)
        else:
            e.target = self.nearest_node(e)
        if e.target is None:
            e.retry = 30

    def flare_gone(self, e):
        self.flares -= 1
        if self.boss is e:
            self.boss = None
            for o in self.enemies:
                if o.alive and o.kind == E_FLARE:
                    self.boss = o
                    break

    def wave_done(self):
        self.phase = PREP
        self.countdown = COUNTDOWN
        self.cred += wave_cred(self.wave)
        if not self.demo:
            self.app.record(self.mi, self.mode_i, self.wave, False)
        if self.wave >= LAST_WAVE and not self.endless:
            self.mode = M_WIN
            if not self.demo:
                self.app.record(self.mi, self.mode_i, self.wave, True, not self.fell)
            audio.stop_music()
            audio.sfx(audio.WIN)
            return
        tip = TIPS.get(self.wave) if self.hints else None
        if tip is not None:
            self.note(tip, C_CYAN, TIP_TIME)
        audio.sfx(audio.CLEAR)

    def game_over(self):
        self.mode = M_OVER
        if not self.demo:
            self.app.record(self.mi, self.mode_i, self.wave - 1, False)
        audio.stop_music()
        audio.sfx(audio.LOSE)

    # -----------------------------------------------------------------
    # Simulation
    # -----------------------------------------------------------------
    def step(self):
        self.t += 1
        if self.t % 30 == 0:
            self.packets += self.online
        self.income_t += 1
        if self.income_t >= INCOME_EVERY:
            self.income_t = 0
            self.money += INCOME
        self.donate_t += 1
        if self.donate_t >= DONATE_EVERY[self.hq[HQ_FREQ]]:
            self.donate_t = 0
            self.money += DONATE_AMOUNT[self.hq[HQ_DONATE]]
            self.income_flash = 20
        if self.t % CRED_EVERY == 0:
            self.cred += self.users + self.online // CRED_NODES
        if self.t % 30 == 0 and self.hqs_own[H_MONITOR]:
            self.auto_repair()
        if self.phase == PREP:
            if self.countdown > 0:
                self.countdown -= 1
            if self.countdown == 0:
                self.start_wave(False)
        else:
            self.wave_t += 1
            q = self.spawn_q
            while self.spawn_i < len(q) and q[self.spawn_i][0] <= self.wave_t:
                g = q[self.spawn_i]
                self.spawn(g[1], g[2])
                self.spawn_i += 1
        if self.gate.down:
            self.gate_down -= 1
            if self.gate_down % OUTAGE_EVERY == 0:
                self.lose_cred(OUTAGE_CRED)
            if self.gate_down <= 0:
                self.game_over()
                return
        self.move_crews()
        self.move_clouds()
        if self.t % POWER_EVERY == 0:
            self.update_power()
        self.update_actors()
        self.move_noise()
        if self.mode == M_OVER:
            return
        if self.t % 4 == 0:
            self.update_jam()
            self.update_seen()
        self.fire()
        self.move_shots()
        self.update_fx()
        en = self.enemies
        for e in en:
            if not e.alive:
                self.enemies = [e for e in en if e.alive]
                break
        if self.phase == WAVE and self.spawn_i >= len(self.spawn_q) and not self.enemies:
            self.wave_done()

    def move_noise(self):
        scale = 1.0 + 0.04 * self.wave
        smash = scale / 30.0
        g = self.gate
        for e in self.enemies:
            if not e.alive:
                continue
            sp = e.speed
            if e.slowt > 0:
                e.slowt -= 1
                sp *= 1.0 - e.slow
                if e.slowt == 0:
                    e.slow = 0.0
            if e.hit:
                e.hit -= 1
            if e.stun:
                e.stun -= 1
                continue
            if e.k.aim == AIM_WIND:
                self.ride_wind(e, sp)
                continue
            if e.park:
                e.park -= 1
                if e.park == 0:
                    # The jammer's battery is spent.
                    e.alive = False
                    self.burst(e.x, e.y, 6, C_PURPLE)
                continue
            if e.smash > 0:
                e.smash -= 1
                if e.smash == 0:
                    # The brick crumbles, the flare burns out.
                    e.alive = False
                    self.burst(e.x, e.y, 6, C_ORANGE)
                    if e.kind == E_FLARE:
                        self.flare_gone(e)
                    continue
            tgt = e.target
            if tgt is not None and (tgt.gone or
                                    (e.kind == E_BAD and (tgt.nulled or tgt.spec[S_SIGNED])) or
                                    (tgt.down and (tgt is not g or e.k.aim != AIM_GATE))):
                self.retarget(e)
                tgt = e.target
            if tgt is None:
                e.lost_t += 1
                if e.kind == E_BAD or e.lost_t >= LOST_TIME:
                    e.alive = False
                    self.burst(e.x, e.y, 4, C_GRAY)
                    continue
                e.retry -= 1
                if e.retry <= 0:
                    self.retarget(e)
                    if e.target is None:
                        e.retry = 30
                # Hang about near the gateway until something stands again.
                dx = g.x - e.x
                dy = g.y - e.y
                d2 = dx * dx + dy * dy
                if d2 > 400.0:
                    d = math.sqrt(d2)
                    e.x += dx * sp * 0.5 / d
                    e.y += dy * sp * 0.5 / d
                continue
            e.lost_t = 0
            if e.attack:
                if not tgt.down:
                    tgt.hp -= e.k.dmg * smash
                    tgt.hurt_t = 4
                    tgt.alarm = ALARM_TIME
                    if tgt is g:
                        self.gate_hit = 4
                    if tgt.hp <= 0:
                        self.node_down(tgt)
                continue
            dx = tgt.x - e.x
            dy = tgt.y - e.y
            d2 = dx * dx + dy * dy
            e.rem = d2
            k = e.kind
            if k == E_JAMMER:
                reach = JAM_REACH
            elif e.k.aim == AIM_GATE:
                reach = PARK_REACH
            else:
                reach = HIT_REACH
            if d2 <= reach * reach:
                self.arrive(e, tgt, scale)
                continue
            d = math.sqrt(d2)
            e.x += dx * sp / d
            e.y += dy * sp / d
            e.flip = dx < 0

    def arrive(self, e, n, scale):
        k = e.kind
        if k == E_JAMMER:
            e.park = JAM_PARK
            return
        if e.k.aim == AIM_GATE:
            e.attack = True
            if e.smash < 0:
                e.smash = FLARE_TIME if k == E_FLARE else SMASH_TIME
            return
        if k == E_BAD:
            e.alive = False
            self.nullify(n, e.actor)
            return
        if n.down:
            e.alive = False
            return
        n.hp -= e.k.dmg * scale
        n.hurt_t = 6
        n.alarm = ALARM_TIME
        if n is self.gate:
            self.gate_hit = 6
        self.burst(e.x, e.y, 4, C_ORANGE)
        audio.sfx(audio.HIT)
        if n.hp <= 0:
            self.node_down(n)
        if k == E_ECHO:
            e.hits += 1
            e.last = n
            if e.hits < ECHO_HITS:
                self.retarget(e)
                return
        e.alive = False

    def ride_wind(self, e, sp):
        e.x += e.vx * sp
        e.y += e.vy * sp
        e.flip = e.vx < 0
        e.rem = INF
        if (e.x < -16 or e.x > WORLD_W + 16 or e.y < MAP_Y - 16 or
                e.y > MAP_Y + WORLD_H + 16):
            e.alive = False
            return
        e.zap -= 1
        if e.zap <= 0:
            e.zap = ZAP_EVERY
            self.storm_zap(e)

    def storm_zap(self, e):
        r2 = ZAP_RANGE * ZAP_RANGE
        for n in self.allnodes:
            if not n.built or n.down:
                continue
            dx = n.x - e.x
            dy = n.y - e.y
            if dx * dx + dy * dy <= r2:
                n.hp -= e.k.dmg * (1.0 + 0.04 * self.wave)
                n.hurt_t = 6
                n.alarm = ALARM_TIME
                self.beams.append([e.x, e.y + 3, n.x, n.y - 3, 6, C_YELLOW])
                audio.sfx(audio.ZAP)
                if n.hp <= 0:
                    self.node_down(n)
                return

    def update_seen(self):
        # Bad signals show only inside the mesh's radio coverage.
        for e in self.enemies:
            if e.kind != E_BAD or not e.alive:
                continue
            s = False
            for n in self.allnodes:
                if n.online:
                    dx = n.x - e.x
                    dy = n.y - e.y
                    if dx * dx + dy * dy <= n.radio2:
                        s = True
                        break
            e.seen = s

    def update_jam(self):
        jammers = None
        for e in self.enemies:
            if e.jam and e.alive:
                if jammers is None:
                    jammers = []
                jammers.append(e)
        if jammers is None and not self.any_jammed:
            return
        changed = False
        anyj = False
        newj = False
        # The gateway is wired to the uplink: jamming cannot cut the mesh's
        # root, only the nodes round it.
        for n in self.nodes:
            j = False
            if jammers is not None and not n.hard and n.built:
                nx = n.x
                ny = n.y
                for e in jammers:
                    dx = e.x - nx
                    dy = e.y - ny
                    if dx * dx + dy * dy <= e.jam2:
                        j = True
                        break
            if j:
                anyj = True
            if j != n.jammed:
                n.jammed = j
                changed = True
                if j:
                    newj = True
        self.any_jammed = anyj
        if changed:
            if newj:
                audio.sfx(audio.JAM)
            self.relink()

    def first_in(self, n):
        # Jammers first, then whatever is closest to its target.
        best = None
        bk = INF
        r2 = n.rng2
        nx = n.x
        ny = n.y
        for e in self.enemies:
            if e.alive and e.seen:
                k = e.rem
                if e.jam:
                    k -= 1 << 28
                if k < bk:
                    dx = e.x - nx
                    dy = e.y - ny
                    if dx * dx + dy * dy <= r2:
                        best = e
                        bk = k
        return best

    def toughest_in(self, n):
        best = None
        bh = 0
        r2 = n.rng2
        nx = n.x
        ny = n.y
        for e in self.enemies:
            if e.alive and e.seen and e.hp > bh:
                dx = e.x - nx
                dy = e.y - ny
                if dx * dx + dy * dy <= r2:
                    best = e
                    bh = e.hp
        return best

    def fire(self):
        enemies = self.enemies
        for n in self.nodes:
            if n.fx:
                n.fx -= 1
            if n.lost:
                n.lost -= 1
            if n.hurt_t:
                n.hurt_t -= 1
            if n.alarm:
                n.alarm -= 1
            if n.cd > 0:
                n.cd -= 1
                continue
            if not n.online or not enemies:
                continue
            k = n.kind
            if k == N_PING:
                t = self.first_in(n)
                if t is not None:
                    self.shots.append(Shot(n.x + 1, n.y - 3, t, n.dmg, n))
                    n.cd = n.cdmax
                    n.fx = 3
                    audio.sfx(audio.PING)
                else:
                    n.cd = 4
            elif k == N_FLOOD:
                r2 = n.rng2
                nx = n.x
                ny = n.y
                hit = False
                for e in enemies:
                    if e.alive and e.seen:
                        dx = e.x - nx
                        dy = e.y - ny
                        if dx * dx + dy * dy <= r2:
                            hit = True
                            self.hurt(e, n.dmg, False, n)
                if hit:
                    self.rings.append([nx, ny, 2, n.rng, C_CYAN])
                    n.cd = n.cdmax
                    audio.sfx(audio.FLOOD)
                else:
                    n.cd = 4
            elif k == N_YAGI:
                t = self.toughest_in(n)
                if t is not None:
                    self.beams.append([n.x + 3, n.y - 1, t.x, t.y, 6, C_WHITE])
                    self.hurt(t, n.dmg, True, n)
                    n.cd = n.cdmax
                    audio.sfx(audio.YAGI)
                else:
                    n.cd = 4
            elif k == N_SLOW:
                r2 = n.rng2
                nx = n.x
                ny = n.y
                slow = n.slow
                dmg = n.dmg
                for e in enemies:
                    if e.alive and e.seen:
                        dx = e.x - nx
                        dy = e.y - ny
                        if dx * dx + dy * dy <= r2:
                            if e.slow < slow:
                                e.slow = slow
                            e.slowt = 12
                            if dmg:
                                self.hurt(e, dmg, True, n)
                n.cd = n.cdmax
        g = self.gate
        if g.hurt_t:
            g.hurt_t -= 1
        if g.alarm:
            g.alarm -= 1

    def hurt(self, e, dmg, pierce, src):
        if not e.alive:
            return
        if not pierce and e.armor:
            dmg -= e.armor
            if dmg < 1:
                dmg = 1
        e.hp -= dmg
        e.hit = 3
        if e.hp <= 0:
            src.kills += 1
            self.kill(e)

    def kill(self, e):
        e.alive = False
        self.cred += e.reward
        self.kills += 1
        k = e.kind
        if k == E_JAMMER:
            self.jam_kills += 1
            self.check_unlocks()
        if k == E_FLARE:
            self.flare_gone(e)
            self.burst(e.x, e.y, 30, C_ORANGE)
            self.burst(e.x, e.y, 20, C_YELLOW)
            self.rings.append([e.x, e.y, 2, 30, C_YELLOW])
            self.shake = 12
            audio.sfx(audio.BIG_KILL)
            return
        col = (C_WHITE, C_ORANGE, C_RED, C_PURPLE, C_YELLOW, C_ORANGE, C_CYAN, C_PURPLE)[k]
        self.burst(e.x, e.y, 5 if k == E_CHIRP else 8, col)
        if k == E_STORM:
            self.spawn_split(e.x, e.y, NOISE_KINDS[E_CHIRP].hp * self.hp_mult)
        audio.sfx(audio.KILL)

    def move_shots(self):
        dead = False
        for s in self.shots:
            t = s.t
            if t.alive:
                s.tx = t.x
                s.ty = t.y
            dx = s.tx - s.x
            dy = s.ty - s.y
            d2 = dx * dx + dy * dy
            if d2 <= 9.0:
                s.alive = False
                dead = True
                if t.alive:
                    self.hurt(t, s.dmg, False, s.src)
            else:
                f = 3.2 / math.sqrt(d2)
                s.x += dx * f
                s.y += dy * f
        if dead:
            self.shots = [s for s in self.shots if s.alive]

    def burst(self, x, y, n, col):
        parts = self.parts
        if len(parts) > 150:
            return
        for _ in range(n):
            a = pyxel.rndf(0, 6.283)
            v = pyxel.rndf(0.3, 1.4)
            parts.append([x, y, math.cos(a) * v, math.sin(a) * v, pyxel.rndi(8, 18), col])

    def update_fx(self):
        if self.rings:
            for r in self.rings:
                r[2] += 2
            self.rings = [r for r in self.rings if r[2] <= r[3]]
        if self.beams:
            for b in self.beams:
                b[4] -= 1
            self.beams = [b for b in self.beams if b[4] > 0]
        if self.parts:
            for p in self.parts:
                p[0] += p[2]
                p[1] += p[3]
                p[2] *= 0.9
                p[3] *= 0.9
                p[4] -= 1
            self.parts = [p for p in self.parts if p[4] > 0]
        if self.shake:
            self.shake -= 1
        if self.gate_hit:
            self.gate_hit -= 1
        if self.income_flash:
            self.income_flash -= 1
        for a in self.actors:
            if a.flash:
                a.flash -= 1

    # -----------------------------------------------------------------
    # Input
    # -----------------------------------------------------------------
    def note(self, text, col, frames=NOTE_TIME, more=""):
        # A one-line message in the strip under the HUD: text in col, then
        # more in white.
        self.msg = (text, col, more)
        self.msg_t = frames

    def update(self):
        if self.msg_t and self.mode == M_PLAY:
            self.msg_t -= 1
        if self.deny:
            self.deny -= 1
        if self.demo:
            self.demo_t += 1
            if self.mode == M_OVER or self.mode == M_WIN:
                self.demo_end += 1
            if ctl.any_key() or self.demo_t > DEMO_FRAMES or self.demo_end > 90:
                self.app.to_title()
                return
        if self.bot is not None:
            self.bot.act()
            if self.mode == M_OVER or self.mode == M_WIN:
                return
        elif self.mode == M_OVER or self.mode == M_WIN:
            self.update_end()
            return
        elif self.mode == M_PLAY:
            self.input_play()
        elif self.mode == M_BUILD:
            self.input_build()
        elif self.mode == M_GATE:
            self.input_gate()
        elif self.mode == M_HQ:
            self.input_hq()
        elif self.mode == M_ACTOR:
            self.input_actor()
        elif self.mode == M_MAP:
            self.input_map()
        elif self.mode == M_SPEC:
            self.input_spec()
        else:
            self.input_node()
        if self.mode == M_PLAY or self.bot is not None:
            for _ in range(self.speed):
                self.step()
                if self.mode != M_PLAY:
                    break
        self.ease_view()

    def move_cursor(self):
        moved = False
        if ctl.left() and self.cx > 0:
            self.cx -= 1
            moved = True
        if ctl.right() and self.cx < MAP_W - 1:
            self.cx += 1
            moved = True
        if ctl.up() and self.cy > 0:
            self.cy -= 1
            moved = True
        if ctl.down() and self.cy < MAP_H - 1:
            self.cy += 1
            moved = True
        if moved:
            self.follow()

    def input_play(self):
        self.move_cursor()
        if ctl.a():
            a = self.actor_at(self.cx, self.cy)
            n = self.node_at(self.cx, self.cy)
            if a is not None:
                self.mode = M_ACTOR
                audio.sfx(audio.SELECT)
            elif n is not None:
                self.mode = M_NODE
                self.opt = self.node_default_opt(n)
                audio.sfx(audio.SELECT)
            elif self.cx == self.gate.tx and self.cy == self.gate.ty:
                self.mode = M_GATE
                self.opt = 0
                self.confirm = False
                audio.sfx(audio.SELECT)
            elif self.level.buildable(self.cx, self.cy):
                self.mode = M_BUILD
                t = self.cy * MAP_W + self.cx
                self.power_sel = P_GRID if self.level.power[t] else P_SOLAR
                audio.sfx(audio.SELECT)
            else:
                self.denied()
        elif ctl.b():
            self.speed = 3 - self.speed
            audio.sfx(audio.SELECT)
        elif ctl.start():
            if self.can_call():
                self.start_wave(True)
            else:
                self.denied()
        elif ctl.y():
            self.mode = M_MAP
            self.map_x = int(self.cam_tx)
            self.map_y = int(self.cam_ty)
            audio.sfx(audio.SELECT)

    def node_opts(self, n):
        return OPTS_SOLAR if n.power == P_SOLAR else OPTS_GRID

    def node_default_opt(self, n):
        opts = self.node_opts(n)
        if n.built and (n.down or n.nulled or n.hp < n.maxhp):
            return opts.index(O_FIX)
        if n.power == P_SOLAR and n.built and n.flat and n.bat_lv < 2:
            return opts.index(O_BAT)
        return 0 if n.level < 2 else opts.index(O_SELL)

    def input_build(self):
        t = self.cy * MAP_W + self.cx
        if ctl.left():
            self.sel = (self.sel - 1) % len(NODE_KINDS)
            audio.sfx(audio.MOVE)
        elif ctl.right():
            self.sel = (self.sel + 1) % len(NODE_KINDS)
            audio.sfx(audio.MOVE)
        elif ctl.up() or ctl.down():
            if self.level.power[t]:
                self.power_sel = 1 - self.power_sel
                audio.sfx(audio.MOVE)
            else:
                self.denied()
        elif ctl.a():
            if self.order_build(self.sel, self.cx, self.cy, self.power_sel) is not None:
                self.mode = M_PLAY
        elif ctl.b() or ctl.start():
            self.mode = M_PLAY

    def input_node(self):
        n = self.node_at(self.cx, self.cy)
        if n is None:
            self.mode = M_PLAY
            return
        opts = self.node_opts(n)
        if self.opt >= len(opts):
            self.opt = 0
        if ctl.left() or ctl.up():
            self.opt = (self.opt - 1) % len(opts)
            audio.sfx(audio.MOVE)
        elif ctl.right() or ctl.down():
            self.opt = (self.opt + 1) % len(opts)
            audio.sfx(audio.MOVE)
        elif ctl.a():
            o = opts[self.opt]
            if o == O_UP:
                ok = self.order_upgrade(n)
            elif o == O_BAT:
                ok = self.order_battery(n)
            elif o == O_PANEL:
                ok = self.order_panel(n)
            elif o == O_FIX:
                ok = self.order_repair(n)
            elif o == O_SPEC:
                ok = False
                if n.built:
                    self.mode = M_SPEC
                    self.spec_sel = 0
                    for i in range(len(NODE_SPECS)):
                        if self.spec_open[i] and not n.spec[i]:
                            self.spec_sel = i
                            break
                    audio.sfx(audio.SELECT)
                else:
                    self.denied()
            else:
                self.sell(n)
                ok = True
            if ok:
                self.mode = M_PLAY
        elif ctl.b() or ctl.start():
            self.mode = M_PLAY

    def input_spec(self):
        n = self.node_at(self.cx, self.cy)
        if n is None:
            self.mode = M_PLAY
            return
        if ctl.up():
            self.spec_sel = (self.spec_sel - 1) % len(NODE_SPECS)
            audio.sfx(audio.MOVE)
        elif ctl.down():
            self.spec_sel = (self.spec_sel + 1) % len(NODE_SPECS)
            audio.sfx(audio.MOVE)
        elif ctl.a():
            if self.order_special(n, self.spec_sel):
                self.mode = M_PLAY
        elif ctl.b() or ctl.start():
            self.mode = M_NODE

    def input_gate(self):
        if self.confirm:
            if ctl.a():
                self.app.to_select()
            elif ctl.b() or ctl.start():
                self.confirm = False
            return
        if ctl.left() or ctl.up():
            self.opt = (self.opt - 1) % len(GATE_OPTS)
            audio.sfx(audio.MOVE)
        elif ctl.right() or ctl.down():
            self.opt = (self.opt + 1) % len(GATE_OPTS)
            audio.sfx(audio.MOVE)
        elif ctl.a():
            if self.opt == 0:
                if self.can_call():
                    self.mode = M_PLAY
                    self.start_wave(True)
                else:
                    self.denied()
            elif self.opt == 1:
                self.mode = M_HQ
                audio.sfx(audio.SELECT)
            elif self.opt == 2:
                self.speed = 3 - self.speed
                audio.sfx(audio.SELECT)
            elif self.opt == 3:
                self.app.set_music(not audio.music_on)
                audio.sfx(audio.SELECT)
            else:
                self.confirm = True
                audio.sfx(audio.SELECT)
        elif ctl.b() or ctl.start():
            self.mode = M_PLAY

    def input_hq(self):
        # The HQ tracks, then the HQ specials, HQ_SHOW rows at a time.
        rows = len(HQ) + len(HQ_SPECS)
        if ctl.up():
            self.hq_sel = (self.hq_sel - 1) % rows
            audio.sfx(audio.MOVE)
        elif ctl.down():
            self.hq_sel = (self.hq_sel + 1) % rows
            audio.sfx(audio.MOVE)
        elif ctl.a():
            if self.hq_sel < len(HQ):
                self.buy_hq(self.hq_sel)
            else:
                self.buy_hq_special(self.hq_sel - len(HQ))
        elif ctl.b() or ctl.start():
            self.mode = M_GATE
        if self.hq_sel < self.hq_top:
            self.hq_top = self.hq_sel
        elif self.hq_sel >= self.hq_top + HQ_SHOW:
            self.hq_top = self.hq_sel - HQ_SHOW + 1

    def input_actor(self):
        a = self.actor_at(self.cx, self.cy)
        if a is None:
            self.mode = M_PLAY
            return
        if ctl.a():
            if self.order_takedown(a):
                self.mode = M_PLAY
        elif ctl.b() or ctl.start():
            self.mode = M_PLAY

    def input_map(self):
        step = 2 * TILE
        if ctl.left():
            self.map_x -= step
        if ctl.right():
            self.map_x += step
        if ctl.up():
            self.map_y -= step
        if ctl.down():
            self.map_y += step
        self.map_x = max(0, min(WORLD_W - VIEW_W, self.map_x))
        self.map_y = max(0, min(WORLD_H - VIEW_H, self.map_y))
        if ctl.a():
            self.look_at((self.map_x + VIEW_W // 2) // TILE, (self.map_y + VIEW_H // 2) // TILE)
            self.mode = M_PLAY
            audio.sfx(audio.SELECT)
        elif ctl.y() or ctl.b() or ctl.start():
            self.mode = M_PLAY

    def update_end(self):
        if self.bot is not None:
            return
        if ctl.a():
            if self.mode == M_WIN:
                self.endless = True
                self.mode = M_PLAY
                audio.music(audio.M_GAME)
                self.note("OVERTIME:", C_ORANGE, NOTE_TIME, " HOW LONG CAN THE MESH HOLD?")
            else:
                self.app.start_game(self.mi)
        elif ctl.b():
            self.app.to_select()

    # -----------------------------------------------------------------
    # Drawing
    # -----------------------------------------------------------------
    def draw(self):
        t = self.t
        if self.mode == M_MAP:
            self.draw_overview(t)
            return
        pyxel.cls(C_BG)
        vx = int(self.cam_x)
        vy = int(self.cam_y)
        self.vx = vx
        self.vy = vy
        # The view in world pixels, 12 px wider all round: what gets drawn.
        self.bx0 = vx - 12
        self.bx1 = vx + VIEW_W + 12
        self.by0 = MAP_Y + vy - 12
        self.by1 = MAP_Y + vy + VIEW_H + 12
        sx = 0
        sy = 0
        if self.shake:
            sx = pyxel.rndi(-2, 2)
            sy = pyxel.rndi(-1, 1)
        pyxel.camera(vx + sx, vy + sy)
        pyxel.clip(0, MAP_Y, VIEW_W, VIEW_H)
        pyxel.blt(vx, MAP_Y + vy, 1, vx, vy, VIEW_W, VIEW_H)
        if self.zones_dirty:
            self.draw_zones_layer()
        if (t // 20) % 3:
            pyxel.blt(vx, MAP_Y + vy, 2, vx, vy, VIEW_W, VIEW_H, 0)
        self.draw_water(t)
        self.draw_homes(t)
        if self.mode == M_BUILD:
            self.draw_linkable(t)
        self.draw_links(t)
        self.draw_fields(t)
        self.draw_nodes(t)
        self.draw_gate(t)
        self.draw_actors(t)
        self.draw_crews(t)
        self.draw_noise(t)
        self.draw_fx()
        self.draw_clouds(t)
        if self.bot is None and self.mode != M_OVER and self.mode != M_WIN:
            self.draw_cursor(t)
        pyxel.camera(0, 0)
        pyxel.clip()
        self.draw_pointers(t)
        self.draw_hud(t)
        if self.mode == M_BUILD:
            self.draw_build_panel()
        elif self.mode == M_NODE:
            self.draw_node_panel()
        elif self.mode == M_GATE:
            self.draw_gate_panel()
        elif self.mode == M_HQ:
            self.draw_hq_panel()
        elif self.mode == M_ACTOR:
            self.draw_actor_panel()
        elif self.mode == M_SPEC:
            self.draw_spec_panel()
        if self.mode == M_OVER or self.mode == M_WIN:
            self.draw_end()

    def visible(self, x, y, m=12):
        return (self.vx - m <= x <= self.vx + VIEW_W + m and
                MAP_Y + self.vy - m <= y <= MAP_Y + self.vy + VIEW_H + m)

    def draw_water(self, t):
        water = self.level.water
        n = len(water)
        if n == 0:
            return
        for i in range(6):
            w = water[(t // 7 + i * 37) % n]
            x = (w % MAP_W) * TILE + (t + i * 3) % 7
            y = MAP_Y + (w // MAP_W) * TILE + 2 + (i % 3) * 2
            if self.visible(x, y, 0):
                pyxel.pset(x, y, C_CYAN if (t // 4 + i) % 3 else C_WHITE)

    def draw_homes(self, t):
        homes = self.level.homes
        x0, x1, y0, y1 = self.bx0, self.bx1, self.by0, self.by1
        for i in range(len(homes)):
            h = homes[i]
            x = (h % MAP_W) * TILE
            y = MAP_Y + (h // MAP_W) * TILE
            if x < x0 or x > x1 or y < y0 or y > y1:
                continue
            if self.home_on[i]:
                pyxel.pset(x + 3, y + 5, C_YELLOW)
                if (t + i * 11) % 60 < 30:
                    pyxel.pset(x + 6, y + 1, C_GREEN)
            else:
                pyxel.pset(x + 3, y + 5, C_NAVY)

    def draw_linkable(self, t):
        col = C_GREEN if (t // 8) % 2 else C_GRASS
        power = self.level.power
        for tx, ty in self.linkable_tiles():
            x = tx * TILE
            y = MAP_Y + ty * TILE
            pyxel.pset(x + 3, y + 3, col)
            pyxel.pset(x + 4, y + 4, col)
            if power[ty * MAP_W + tx]:
                pyxel.pset(x + 1, y + 6, C_YELLOW)

    def draw_links(self, t):
        # A link is drawn when either end is within 52 px of the view.
        x0 = self.bx0 - 52
        x1 = self.bx1 + 52
        y0 = self.by0 - 52
        y1 = self.by1 + 52
        for n in self.nodes:
            p = n.parent
            if p is None:
                continue
            if ((n.x < x0 or n.x > x1 or n.y < y0 or n.y > y1) and
                    (p.x < x0 or p.x > x1 or p.y < y0 or p.y > y1)):
                continue
            x0 = n.x
            y0 = n.y - 3
            x1 = p.x
            y1 = p.y - (7 if p.kind == N_GATE else 3)
            pyxel.line(x0, y0, x1, y1, C_GRASS)
            f = ((t + n.phase) % 30) / 30.0
            pyxel.pset(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, C_GREEN)

    def draw_fields(self, t):
        for n in self.nodes:
            if n.kind == N_SLOW and n.online and self.visible(n.x, n.y, n.rng):
                r = n.rng - (t // 3) % 8
                pyxel.circb(n.x, n.y, r, C_NAVY)
                pyxel.circb(n.x, n.y, n.rng, C_NAVY)
        for e in self.enemies:
            if e.jam and e.alive and e.seen and self.visible(e.x, e.y, e.jam):
                r = e.jam - (t // 2) % 4
                pyxel.dither(0.5)
                pyxel.circb(e.x, e.y, r, C_PURPLE)
                pyxel.dither(1.0)
        for r in self.rings:
            pyxel.circb(r[0], r[1], r[2], r[4])

    def draw_nodes(self, t):
        blink = (t // 6) % 2
        blt = pyxel.blt
        x0, x1, y0, y1 = self.bx0, self.bx1, self.by0, self.by1
        for n in self.nodes:
            if n.x < x0 or n.x > x1 or n.y < y0 or n.y > y1:
                continue
            x = n.x - 4
            y = n.y - 4
            if not n.built:
                # A site: scaffold corners and a faint sprite.
                pyxel.dither(0.4)
                blt(x, y, 0, n.kind * 8, 0, 8, 8, 0)
                pyxel.dither(1.0)
                col = C_ORANGE if blink else C_YELLOW
                pyxel.line(x - 1, y - 1, x + 1, y - 1, col)
                pyxel.line(x + 6, y + 8, x + 8, y + 8, col)
                self.draw_job(n, x, y, t)
                continue
            blt(x, y, 0, n.kind * 8, gfx.NODE_V + n.level * 8, 8, 8, 0)
            if n.hurt_t and blink:
                pyxel.rectb(x - 1, y - 1, 10, 10, C_WHITE)
            if n.down:
                pyxel.dither(0.5)
                pyxel.rect(x, y, 8, 8, C_BG)
                pyxel.dither(1.0)
                col = C_RED if blink else C_ORANGE
                pyxel.line(x + 1, y + 1, x + 6, y + 6, col)
                pyxel.line(x + 6, y + 1, x + 1, y + 6, col)
            elif n.nulled:
                pyxel.dither(0.5)
                pyxel.rect(x, y, 8, 8, C_PURPLE)
                pyxel.dither(1.0)
                pyxel.pset(x + (t * 3) % 8, y + (t * 5) % 8, C_WHITE)
                pyxel.rectb(x - 1, y - 1, 10, 10, C_PURPLE if blink else C_BG)
            elif not n.online:
                pyxel.dither(0.5)
                pyxel.rect(x, y, 8, 8, C_BG)
                pyxel.dither(1.0)
                if n.jammed:
                    pyxel.pset(x + (t * 3) % 8, y + (t * 5) % 8, C_WHITE)
                    pyxel.pset(x + (t * 7 + 3) % 8, y + (t * 2 + 5) % 8, C_PURPLE)
                elif n.flat:
                    pyxel.rect(x + 2, y + 2, 4, 3, C_RED if blink else C_BG)
                pyxel.pset(x + 7, y, C_RED if blink else C_BG)
                if n.lost and blink:
                    pyxel.rectb(x - 1, y - 1, 10, 10, C_RED)
            elif n.fx:
                pyxel.pset(n.x + 1, n.y - 4, C_WHITE)
            elif n.kind == N_RELAY and (t + n.phase) % 40 < 5:
                pyxel.pset(x + 3, y, C_WHITE)
            yy = y + 8
            if n.hp < n.maxhp:
                w = int(8 * n.hp / n.maxhp)
                pyxel.rect(x, yy, 8, 1, C_NAVY)
                if w > 0:
                    pyxel.rect(x, yy, w, 1, C_GREEN if n.hp * 2 > n.maxhp else C_RED)
                yy += 1
            if n.power == P_SOLAR:
                w = int(8 * n.bat / n.cap)
                pyxel.rect(x, yy, 8, 1, C_NAVY)
                if w > 0:
                    pyxel.rect(x, yy, w, 1, C_ORANGE if n.shaded else C_YELLOW)
            if n.job is not None:
                self.draw_job(n, x, y, t)

    def draw_job(self, n, x, y, t):
        c = n.job.crew
        if c is not None and c.state == C_WORK:
            w = int(8 * c.work / c.work_total)
            pyxel.rect(x, y - 3, 8, 1, C_NAVY)
            pyxel.rect(x, y - 3, w, 1, C_RED if c.paused else C_CYAN)
            if c.paused:
                if (t // 6) % 2:
                    pyxel.rect(x + 3, y - 9, 2, 4, C_RED)
                    pyxel.pset(x + 3, y - 4, C_RED)
            elif (t // 3) % 2:
                pyxel.pset(x + pyxel.rndi(0, 7), y + pyxel.rndi(3, 7), C_YELLOW)
        elif (t // 10) % 2:
            pyxel.pset(x + 3, y - 3, C_YELLOW)
            pyxel.pset(x + 4, y - 3, C_YELLOW)

    def draw_gate(self, t):
        g = self.gate
        if not self.visible(g.x, g.y, 16):
            return
        if (self.gate_hit or g.down) and (t // 2) % 2:
            pyxel.pal(C_NAVY, C_RED)
        pyxel.blt(g.x - 8, g.y - 12, 0, 48, 0, 16, 16, 0)
        pyxel.pal()
        if g.down:
            col = C_RED if (t // 6) % 2 else C_ORANGE
            pyxel.line(g.x - 5, g.y - 5, g.x + 4, g.y + 3, col)
            pyxel.line(g.x + 4, g.y - 5, g.x - 5, g.y + 3, col)
        elif g.online:
            ph = t % 60
            if ph < 20:
                pyxel.circb(g.x, g.y - 12, ph // 2 + 1, C_CYAN if ph < 10 else C_WATER)
        if g.hp < g.maxhp:
            w = int(14 * g.hp / g.maxhp)
            pyxel.rect(g.x - 7, g.y + 4, 14, 1, C_NAVY)
            if w > 0:
                pyxel.rect(g.x - 7, g.y + 4, w, 1, C_GREEN if g.hp * 2 > g.maxhp else C_RED)
        if g.job is not None:
            self.draw_job(g, g.x - 4, g.y - 8, t)

    def draw_actors(self, t):
        for a in self.actors:
            if not a.found or a.gone or not self.visible(a.x, a.y):
                continue
            pyxel.blt(a.x - 4, a.y - 4, 0, gfx.SPR_ACTOR[0], gfx.SPR_ACTOR[1], 8, 8, 0)
            if (t // 8) % 2:
                pyxel.circb(a.x, a.y, 6 + (t // 2) % 4, C_RED)
            if a.job is not None:
                c = a.job.crew
                if c is not None and c.state == C_WORK:
                    w = int(8 * c.work / c.work_total)
                    pyxel.rect(a.x - 4, a.y - 7, 8, 1, C_NAVY)
                    pyxel.rect(a.x - 4, a.y - 7, w, 1, C_CYAN)

    def draw_crews(self, t):
        for c in self.crews:
            if c.state == C_IDLE or c.state == C_WORK:
                continue
            if self.visible(c.x, c.y):
                pyxel.blt(c.x - 3, c.y - 3, 0, gfx.SPR_TRUCK[0], gfx.SPR_TRUCK[1],
                          -7 if c.flip else 7, 5, 0)

    def draw_noise(self, t):
        x0, x1, y0, y1 = self.bx0, self.bx1, self.by0, self.by1
        for e in self.enemies:
            if not e.alive or not e.seen or e.x < x0 or e.x > x1 or e.y < y0 or e.y > y1:
                continue
            s = e.size
            half = s // 2
            x = int(e.x) - half
            y = int(e.y) - half - 1
            v = e.v + (s if ((t + e.anim) // 8) % 2 else 0)
            pyxel.blt(x, y, 0, e.u, v, -s if e.flip else s, s, 0)
            if e.hit:
                pyxel.pset(x + pyxel.rndi(0, s - 1), y + pyxel.rndi(0, s - 1), C_WHITE)
            if e.slowt:
                pyxel.pset(x + half, y - 1, C_CYAN)
            if e.attack and (t // 4) % 2:
                pyxel.pset(x + half, y + s, C_ORANGE)
            if e.hp < e.maxhp:
                w = int(s * e.hp / e.maxhp) + 1
                pyxel.rect(x, y - 3, s, 1, C_NAVY)
                pyxel.rect(x, y - 3, w, 1, C_GREEN if e.hp * 2 > e.maxhp else C_RED)

    def draw_fx(self):
        for s in self.shots:
            pyxel.rect(s.x - 1, s.y - 1, 2, 2, C_CYAN)
        for b in self.beams:
            col = b[5]
            if b[4] <= 3:
                col = C_YELLOW if col == C_WHITE else C_WHITE
            pyxel.line(b[0], b[1], b[2], b[3], col)
        for p in self.parts:
            pyxel.pset(p[0], p[1], p[5])

    def draw_clouds(self, t):
        for c in self.clouds:
            x, y, rx, ry = c
            if not self.visible(x, y, rx):
                continue
            pyxel.dither(0.3)
            pyxel.elli(x - rx, y - ry, rx * 2, ry * 2, C_BG)
            pyxel.dither(0.25)
            pyxel.elli(x - rx + 3, y - ry - 6, rx * 2 - 6, ry * 2 - 2, C_WHITE)
            pyxel.dither(1.0)

    def draw_cursor(self, t):
        x = self.cx * TILE
        y = MAP_Y + self.cy * TILE
        col = C_WHITE if (t // 8) % 2 or self.mode != M_PLAY else C_YELLOW
        if self.deny:
            col = C_RED
        n = self.node_at(self.cx, self.cy)
        if self.mode == M_BUILD:
            self.draw_preview(t)
        elif n is not None:
            pyxel.circb(n.x, n.y, n.radio, C_GRASS)
            if n.rng:
                pyxel.circb(n.x, n.y, n.rng, C_WHITE if self.mode == M_NODE else C_GRAY)
        elif self.cx == self.gate.tx and self.cy == self.gate.ty:
            pyxel.circb(self.gate.x, self.gate.y, self.gate.radio, C_GRASS)
        for (ox, oy, dx, dy) in ((-1, -1, 1, 1), (8, -1, -1, 1), (-1, 8, 1, -1), (8, 8, -1, -1)):
            pyxel.pset(x + ox, y + oy, col)
            pyxel.pset(x + ox + dx, y + oy, col)
            pyxel.pset(x + ox, y + oy + dy, col)

    def draw_preview(self, t):
        tx = self.cx
        ty = self.cy
        k = NODE_KINDS[self.sel]
        te = self.level.terrain_at(ty * MAP_W + tx)
        x, y = tile_center(tx, ty)
        radio = int(k.radio[0] * te.radio + 0.5)
        rng = int(k.rng[0] * te.rng + 0.5) if k.rng[0] else 0
        p, hops = self.cursor_link()
        if p is not None:
            y1 = p.y - (7 if p.kind == N_GATE else 3)
            pyxel.line(x, y - 3, p.x, y1, C_GREEN if (t // 4) % 2 else C_GRASS)
        pyxel.circb(x, y, radio, C_GREEN if p is not None else C_RED)
        if rng:
            pyxel.circb(x, y, rng, C_GRAY)
        pyxel.dither(0.6)
        pyxel.blt(x - 4, y - 4, 0, self.sel * 8, 0, 8, 8, 0)
        pyxel.dither(1.0)

    def draw_pointers(self, t):
        # Arrows on the view's edge toward what is out of sight: red for
        # noise, orange for a node under fire, red for a downed node or the
        # gateway, purple for a nullified node or a new search zone, yellow
        # for a pinned bad actor; before a wave, the sides it comes from.
        # Events blink; noise and wave-side arrows stay lit.
        marks = {}
        g = self.gate
        if g.down or g.alarm:
            self.mark(marks, g.x, g.y, C_RED, 6)
        for a in self.actors:
            if a.gone:
                continue
            if a.found:
                if a.job is None:
                    self.mark(marks, a.x, a.y, C_YELLOW, 5)
            elif a.flash and a.hits:
                h = a.hits[-1]
                hx, hy = tile_center(h % MAP_W, h // MAP_W)
                self.mark(marks, hx, hy, C_PURPLE, 4)
        for n in self.nodes:
            if not n.built:
                continue
            if (n.down or n.nulled) and n.job is None:
                self.mark(marks, n.x, n.y, C_PURPLE if n.nulled else C_RED, 4)
            elif n.alarm:
                self.mark(marks, n.x, n.y, C_ORANGE, 3)
        for e in self.enemies:
            if e.alive and e.seen:
                self.mark(marks, e.x, e.y, C_RED, 1)
        if self.phase == PREP and self.mode != M_OVER and self.mode != M_WIN:
            sides = self.next_sides
            if "A" in sides:
                sides = SIDES
            cx = self.vx + VIEW_W // 2
            cy = MAP_Y + self.vy + VIEW_H // 2
            for s in sides:
                if s == "N":
                    self.mark(marks, cx, cy - 4096, C_RED, 2)
                elif s == "S":
                    self.mark(marks, cx, cy + 4096, C_RED, 2)
                elif s == "W":
                    self.mark(marks, cx - 4096, cy, C_RED, 2)
                else:
                    self.mark(marks, cx + 4096, cy, C_RED, 2)
        lit = (t // 6) % 2
        # Keep clear of the message strip while it shows.
        top = MAP_Y + 18 if self.msg_t and self.mode == M_PLAY else 0
        for key in marks:
            pri, col, sx, sy, ux, uy, n = marks[key]
            if pri > 2 and not lit:
                continue
            if sy < top:
                sy = top
            s = 5 if n >= 3 or pri >= 5 else 4
            # A triangle pointing out of the view along (ux, uy).
            tx = sx + ux * s
            ty = sy + uy * s
            bx = sx - ux * 2
            by = sy - uy * 2
            px = -uy * 3
            py = ux * 3
            pyxel.tri(tx, ty, bx + px, by + py, bx - px, by - py, col)
            pyxel.trib(tx, ty, bx + px, by + py, bx - px, by - py, C_BG)

    def mark(self, marks, x, y, col, pri):
        # Add an arrow toward world point (x, y) if it is out of sight; one
        # arrow per stretch of the edge, the most urgent one winning.
        vx = self.vx
        vy = MAP_Y + self.vy
        if vx - 2 <= x <= vx + VIEW_W + 2 and vy - 2 <= y <= vy + VIEW_H + 2:
            return
        dx = x - (vx + VIEW_W // 2)
        dy = y - (vy + VIEW_H // 2)
        fx = (VIEW_W // 2 - 6) / abs(dx) if dx else 1e9
        fy = (VIEW_H // 2 - 6) / abs(dy) if dy else 1e9
        f = fx if fx < fy else fy
        sx = VIEW_W // 2 + dx * f
        sy = MAP_Y + VIEW_H // 2 + dy * f
        key = (int(sx) // 10) * 64 + int(sy) // 10
        d = math.sqrt(dx * dx + dy * dy)
        m = marks.get(key)
        if m is None:
            marks[key] = [pri, col, sx, sy, dx / d, dy / d, 1]
        else:
            m[6] += 1
            if pri > m[0]:
                m[0] = pri
                m[1] = col
                m[2] = sx
                m[3] = sy
                m[4] = dx / d
                m[5] = dy / d

    # -----------------------------------------------------------------
    # Overview
    # -----------------------------------------------------------------
    def ov(self, x, y):
        return OV_X + int(x * OV_S), OV_Y + int((y - MAP_Y) * OV_S)

    def draw_overview(self, t):
        pyxel.cls(C_BG)
        half = (WORLD_W - WORLD_W * OV_S) / 2
        pyxel.blt(OV_X - half, OV_Y - half, 1, 0, 0, WORLD_W, WORLD_H, None, None, OV_S)
        pyxel.rectb(OV_X - 1, OV_Y - 1, int(WORLD_W * OV_S) + 2, int(WORLD_H * OV_S) + 2,
                    C_GRAY)
        pyxel.clip(OV_X, OV_Y, int(WORLD_W * OV_S), int(WORLD_H * OV_S))
        if (t // 10) % 2:
            for a in self.actors:
                if a.found or a.gone or a.zone is None:
                    continue
                for z in a.zone:
                    pyxel.pset(OV_X + (z % MAP_W) * 3 + 1, OV_Y + (z // MAP_W) * 3 + 1,
                               C_PURPLE)
        pyxel.dither(0.4)
        for c in self.clouds:
            x, y = self.ov(c[0], c[1])
            pyxel.elli(x - c[2] * OV_S, y - c[3] * OV_S, c[2] * 2 * OV_S, c[3] * 2 * OV_S,
                       C_WHITE)
        pyxel.dither(1.0)
        for n in self.nodes:
            x, y = self.ov(n.x, n.y)
            if not n.built:
                col = C_YELLOW
            elif n.down:
                col = C_RED
            elif n.nulled:
                col = C_PURPLE
            elif n.online:
                col = C_GREEN
            else:
                col = C_GRAY
            pyxel.rect(x - 1, y - 1, 2, 2, col)
        x, y = self.ov(self.gate.x, self.gate.y)
        pyxel.rect(x - 1, y - 1, 3, 3, C_RED if self.gate.down else C_CYAN)
        for e in self.enemies:
            if e.alive and e.seen:
                x, y = self.ov(e.x, e.y)
                pyxel.pset(x, y, C_ORANGE if e.kind == E_FLARE else C_RED)
        for a in self.actors:
            if a.found and not a.gone:
                x, y = self.ov(a.x, a.y)
                pyxel.line(x - 2, y - 2, x + 2, y + 2, C_RED)
                pyxel.line(x + 2, y - 2, x - 2, y + 2, C_RED)
        bx = OV_X + int(self.map_x * OV_S)
        by = OV_Y + int(self.map_y * OV_S)
        pyxel.rectb(bx, by, int(VIEW_W * OV_S), int(VIEW_H * OV_S),
                    C_WHITE if (t // 8) % 2 else C_YELLOW)
        pyxel.clip()
        lx = 106
        pyxel.text(lx, 14, self.mapdef.name, C_CYAN)
        rows = ((C_GREEN, "ONLINE"), (C_GRAY, "DARK"), (C_RED, "DOWN"), (C_PURPLE, "NULLED"),
                (C_YELLOW, "BUILDING"))
        for i, (col, s) in enumerate(rows):
            pyxel.rect(lx, 25 + i * 8, 3, 3, col)
            pyxel.text(lx + 6, 24 + i * 8, s, C_WHITE)
        pyxel.text(lx, 68, "PURPLE DOTS:", C_PURPLE)
        pyxel.text(lx, 75, "BAD ACTOR", C_WHITE)
        pyxel.text(lx, 82, "SEARCH ZONE", C_WHITE)
        pyxel.text(lx, 98, "A:GO THERE", C_YELLOW)
        pyxel.text(lx, 106, "Y:BACK", C_YELLOW)
        self.draw_hud(t)

    # -----------------------------------------------------------------
    # HUD and panels
    # -----------------------------------------------------------------
    def draw_hud(self, t):
        pyxel.rect(0, 0, 160, MAP_Y, C_NAVY)
        if self.endless and self.wave > LAST_WAVE:
            pyxel.text(2, 1, "OT%d" % self.wave, C_ORANGE)
        else:
            pyxel.text(2, 1, "W%d" % self.wave, C_WHITE)
        g = self.gate
        if g.down:
            if (t // 8) % 2:
                pyxel.text(18, 1, "DOWN%d" % ((self.gate_down + 29) // 30), C_RED)
        else:
            pyxel.blt(18, 1, 0, gfx.SPR_GW[0], gfx.SPR_GW[1], 7, 6, 0)
            pc = int(100 * g.hp / g.maxhp)
            pyxel.text(26, 1, "%d" % pc, C_RED if pc < 40 else C_WHITE)
        pyxel.text(46, 1, "$%d" % self.money, C_WHITE if self.income_flash else C_YELLOW)
        gfx.star(68, 1)
        pyxel.text(74, 1, "%d" % self.cred, C_CYAN)
        pyxel.blt(95, 1, 0, gfx.SPR_HOUSE[0], gfx.SPR_HOUSE[1], 7, 6, 0)
        pyxel.text(103, 1, "%d" % self.users, C_GREEN)
        free = self.free_crews()
        pyxel.blt(112, 2, 0, gfx.SPR_TRUCK[0], gfx.SPR_TRUCK[1], 7, 5, 0)
        pyxel.text(120, 1, "%d" % free, C_WHITE if free else C_GRAY)
        live = 0
        for a in self.actors:
            if not a.gone and (a.found or a.hits):
                live += 1
        if live:
            pyxel.blt(125, 1, 0, gfx.SPR_ACTOR_ICON[0], gfx.SPR_ACTOR_ICON[1], 5, 6, 0)
            pyxel.text(131, 1, "%d" % live, C_RED)
        if self.mode == M_OVER or self.mode == M_WIN:
            pass
        elif self.phase == PREP:
            on = (t // 15) % 2 or self.mode != M_PLAY
            pyxel.text(137, 1, "T%d" % ((self.countdown + 29) // 30),
                       C_GREEN if on else C_GRASS)
        else:
            left = len(self.enemies) + len(self.spawn_q) - self.spawn_i
            pyxel.text(137, 1, "N%d" % left, C_GRAY)
            if self.speed == 2:
                pyxel.text(153, 1, ">>", C_CYAN)
        y = MAP_Y + 2
        if self.msg_t and self.mode == M_PLAY:
            self.draw_strip()
            y = MAP_Y + 10
        b = self.boss
        if b is not None and b.alive and self.mode != M_OVER and self.mode != M_WIN:
            pyxel.rect(30, y, 100, 5, C_BG)
            pyxel.rectb(30, y, 100, 5, C_ORANGE)
            w = int(98 * b.hp / b.maxhp)
            if w > 0:
                pyxel.rect(31, y + 1, w, 3, C_RED if b.hp * 3 < b.maxhp else C_ORANGE)
            pyxel.text(8, y, "FLARE", C_ORANGE)
        self.draw_bar(t)

    def draw_bar(self, t):
        # Bottom bar: what is under the cursor, or what a menu option does.
        pyxel.rect(0, BAR_Y, 160, 8, C_NAVY)
        y = BAR_Y + 1
        if self.demo:
            pyxel.text(2, y, "DEMO: " + self.mapdef.name, C_CYAN)
            if (t // 15) % 2:
                pyxel.text(112, y, "PRESS A", C_YELLOW)
            return
        if self.mode == M_MAP:
            pyxel.text(2, y, "WHOLE MAP: D-PAD MOVES THE BOX", C_GRAY)
            return
        if self.mode == M_NODE:
            n = self.node_at(self.cx, self.cy)
            if n is not None:
                self.draw_opt_help(n, y)
            return
        if self.mode == M_SPEC:
            pyxel.text(2, y, "A: A CREW FITS IT   B: BACK", C_GRAY)
            return
        if self.mode != M_PLAY:
            return
        n = self.node_at(self.cx, self.cy)
        t0 = self.cy * MAP_W + self.cx
        lv = self.level
        te = lv.terrain_at(t0)
        a = self.actor_at(self.cx, self.cy)
        if a is not None:
            pyxel.text(2, y, "BAD ACTOR: A TO TAKE IT DOWN", C_RED)
        elif n is not None:
            k = NODE_KINDS[n.kind]
            st, col = self.node_status(n)
            pyxel.text(2, y, "%s L%d" % (k.name, n.level + 1), C_WHITE)
            pyxel.text(38, y, st, col)
            pyxel.text(124, y, "A:MENU", C_GRAY)
        elif t0 == lv.gate:
            pyxel.text(2, y, "GATEWAY: KEEP IT UP", C_CYAN)
            pyxel.text(124, y, "A:MENU", C_GRAY)
        elif lv.tiles[t0] == T_HOME:
            i = lv.homes.index(t0)
            if self.home_on[i]:
                pyxel.text(2, y, "HOMES: CONNECTED, EARNING CRED", C_GREEN)
            else:
                pyxel.text(2, y, "HOMES: NO SIGNAL YET", C_ORANGE)
        elif lv.buildable(self.cx, self.cy):
            p, hops = self.cursor_link()
            pyxel.text(2, y, te.name, C_YELLOW if te.install > 1 else C_GRAY)
            if p is not None:
                pyxel.text(42, y, "HOP %d" % hops, C_GREEN)
            else:
                pyxel.text(42, y, "NO LINK", C_RED)
            pyxel.text(74, y, "GRID" if lv.power[t0] else "SOLAR",
                       C_YELLOW if lv.power[t0] else C_ORANGE)
            pyxel.text(124, y, "A:BUILD", C_GRAY)
        else:
            pyxel.text(2, y, "%s: %s" % (te.name, te.desc), C_GRAY)

    def draw_opt_help(self, n, y):
        o = self.node_opts(n)[self.opt % len(self.node_opts(n))]
        if o == O_UP:
            c = self.upgrade_cost(n)
            s = "UPGRADE TO L%d  $%d" % (n.level + 2, c) if c else "TOP LEVEL ALREADY"
        elif o == O_BAT:
            c = self.battery_cost(n)
            s = ("BIGGER BATTERY: %dS  $%d" % (BAT_CAP[n.bat_lv + 1] // 30, c) if c
                 else "BIGGEST BATTERY ALREADY")
        elif o == O_PANEL:
            c = self.panel_cost(n)
            s = ("BIGGER PANEL: FASTER CHARGE  $%d" % c if c else "BIGGEST PANEL ALREADY")
        elif o == O_FIX:
            c = self.repair_cost(n)
            if n.nulled:
                s = "RESET THE NULLIFIED NODE: " + ("$%d" % c if c else "FREE")
            elif c:
                s = "REPAIR  $%d" % c
            else:
                s = "NOTHING TO FIX"
        elif o == O_SPEC:
            s = "SPECIALS: %d OF %d UNLOCKED" % (
                sum(1 for x in self.spec_open if x), len(NODE_SPECS))
        else:
            s = "SELL FOR $%d" % self.sell_value(n)
        pyxel.text(2, y, s, C_YELLOW)

    def spec_ready(self, n):
        # A node special is unlocked and not fitted to n yet.
        for i in range(len(NODE_SPECS)):
            if self.spec_open[i] and not n.spec[i]:
                return True
        return False

    def node_status(self, n):
        job = n.job
        if job is not None:
            c = job.crew
            if c is None:
                return "QUEUED: " + JOB_NAMES[job.kind], C_YELLOW
            if c.state == C_WORK:
                if c.paused:
                    return "CREW WAITS: NOISE", C_RED
                return "%s %d%%" % (JOB_NAMES[job.kind], 100 * c.work // c.work_total), C_CYAN
            return "CREW EN ROUTE", C_CYAN
        if n.down:
            return "DOWN: NEEDS REPAIR", C_RED
        if n.nulled:
            return "NULLIFIED: RESET IT", C_PURPLE
        if n.flat:
            return "BATTERY FLAT", C_ORANGE
        if n.online:
            return "ONLINE HOP %d" % n.hops, C_GREEN
        if n.jammed:
            return "JAMMED", C_PURPLE
        return "NO LINK", C_RED

    def panel_y(self, h):
        # Top unless it would cover the cursor's row: the on-screen pad of
        # touch boards covers the bottom corners more than the top.
        sy = MAP_Y + self.cy * TILE - int(self.cam_y)
        if sy >= MAP_Y + h:
            return MAP_Y
        return BAR_Y - h

    def draw_build_panel(self):
        h = 37
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h)
        for i in range(len(NODE_KINDS)):
            k = NODE_KINDS[i]
            x = 4 + i * 31
            cost = k.cost[0]
            if i == self.sel:
                pyxel.rect(x - 2, py + 2, 30, 12, C_BG)
                pyxel.rectb(x - 2, py + 2, 30, 12, C_RED if self.deny else C_YELLOW)
            gfx.node_sprite(i, x, py + 4)
            pyxel.text(x + 10, py + 5, "$%d" % cost, C_WHITE if self.money >= cost else C_RED)
        k = NODE_KINDS[self.sel]
        t0 = self.cy * MAP_W + self.cx
        te = self.level.terrain_at(t0)
        pyxel.text(3, py + 16, k.name, C_CYAN)
        pyxel.text(3 + len(k.name) * 4 + 4, py + 16, k.desc[0], C_WHITE)
        work = int(k.install * te.install * TOOL_TIME[self.hq[HQ_TOOLS]])
        eta = (self.travel_frames(t0) + work + 29) // 30
        s = "ETA %dS  %s" % (eta, te.name)
        if te.install > 1:
            s += " X%.1f" % te.install
        if not self.free_crews():
            s += "  QUEUED"
        pyxel.text(3, py + 23, s, C_YELLOW if te.install > 1 else C_GRAY)
        if self.level.power[t0]:
            s = "POWER: %s  (UP/DN SWITCH)" % POWER_NAMES[self.power_sel]
        else:
            s = "POWER: SOLAR ONLY (NO GRID HERE)"
        pyxel.text(3, py + 30, s, C_YELLOW if self.power_sel == P_GRID else C_ORANGE)

    def draw_node_panel(self):
        n = self.node_at(self.cx, self.cy)
        if n is None:
            return
        k = NODE_KINDS[n.kind]
        h = 43
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h)
        gfx.node_sprite(n.kind, 3, py + 3)
        pyxel.text(14, py + 4, "%s L%d" % (k.name, n.level + 1), C_CYAN)
        st, col = self.node_status(n)
        pyxel.text(50, py + 4, st, col)
        te = n.terrain
        if n.kind == N_RELAY:
            info = "RADIO %d" % n.radio + (" HARD" if n.hard else "")
            where = te.name
        else:
            if n.kind == N_SLOW:
                info = "SLOW %d%% RNG %d" % (int(n.slow * 100 + 0.5), n.rng)
            else:
                info = "DMG %d RNG %d %.1f/S" % (n.dmg, n.rng, 30.0 / n.cdmax)
            where = "%s  RADIO %d" % (te.name, n.radio)
            pyxel.text(96, py + 18, "KILLS %d" % n.kills, C_ORANGE)
        pyxel.text(14, py + 11, info, C_WHITE)
        pyxel.text(96, py + 11, "HP%d/%d" % (max(0, int(n.hp)), n.maxhp),
                   C_GREEN if n.hp * 2 > n.maxhp else C_RED)
        pyxel.text(14, py + 18, where, C_YELLOW if te.install > 1 else C_GRAY)
        if n.power == P_SOLAR:
            pyxel.text(14, py + 25, "SOLAR B%d P%d" % (n.bat_lv + 1, n.pv_lv + 1), C_ORANGE)
            w = int(40 * n.bat / n.cap)
            pyxel.rect(70, py + 26, 40, 3, C_BG)
            if w > 0:
                pyxel.rect(70, py + 26, w, 3, C_ORANGE if n.shaded else C_YELLOW)
            pyxel.text(114, py + 25, "SHADE" if n.shaded else "SUN",
                       C_GRAY if n.shaded else C_YELLOW)
        else:
            pyxel.text(14, py + 25, "GRID POWER", C_YELLOW)
        opts = self.node_opts(n)
        busy = n.job is not None
        x = 6
        for i in range(len(opts)):
            o = opts[i]
            label = OPT_LABELS[o]
            if o == O_FIX and n.nulled:
                label = "RESET"
            if o == O_UP:
                c = self.upgrade_cost(n)
            elif o == O_BAT:
                c = self.battery_cost(n)
            elif o == O_PANEL:
                c = self.panel_cost(n)
            elif o == O_FIX:
                c = self.repair_cost(n)
            else:
                c = -1
            if o == O_FIX:
                ok = self.can_fix(n) and not busy and self.money >= c
            elif o == O_SPEC:
                ok = n.built and not busy and self.spec_ready(n)
            else:
                ok = c < 0 or (c and not busy and n.built and self.money >= c)
            w = len(label) * 4
            if i == self.opt:
                pyxel.rect(x - 3, py + 32, w + 5, 9, C_BG)
                pyxel.rectb(x - 3, py + 32, w + 5, 9, C_RED if self.deny else C_YELLOW)
            pyxel.text(x, py + 34, label, C_WHITE if ok else C_GRAY)
            x += w + 12

    def draw_gate_panel(self):
        h = 30
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h)
        pyxel.blt(3, py + 3, 0, 48, 0, 16, 16, 0)
        g = self.gate
        pyxel.text(24, py + 4, "GATEWAY", C_CYAN)
        pyxel.text(60, py + 4, "HP %d/%d" % (max(0, int(g.hp)), g.maxhp),
                   C_RED if g.down else C_WHITE)
        pyxel.text(24, py + 11, "CREWS %d/%d  JOBS %d  USERS %d" % (
            self.free_crews(), len(self.crews), len(self.jobs), self.users), C_GRAY)
        if self.confirm:
            pyxel.text(24, py + 21, "QUIT THIS MAP?  A:YES  B:NO", C_RED)
            return
        labels = ("SEND WAVE", "HQ", "SPEED X%d" % self.speed,
                  "MUSIC ON" if audio.music_on else "MUSIC OFF", "QUIT")
        x = 5
        for i in range(len(labels)):
            col = C_WHITE
            if i == 0 and not self.can_call():
                col = C_GRAY
            w = len(labels[i]) * 4
            if i == self.opt:
                pyxel.rect(x - 2, py + 19, w + 3, 9, C_BG)
                pyxel.rectb(x - 2, py + 19, w + 3, 9, C_RED if self.deny else C_YELLOW)
            pyxel.text(x, py + 21, labels[i], col)
            x += w + 6

    def draw_funds(self, y):
        # Money and cred, right-aligned on a panel's title line.
        m = "$%d" % self.money
        c = "%d" % self.cred
        x = 157 - len(m) * 4 - 9 - len(c) * 4
        pyxel.text(x, y, m, C_YELLOW)
        x += len(m) * 4 + 3
        gfx.star(x, y)
        pyxel.text(x + 6, y, c, C_CYAN)

    def draw_price(self, x, y, cost, cred):
        # Dollars (when cost is not 0), then the cred star and cred; a part
        # that cannot be paid is red.
        if cost:
            s = "$%d" % cost
            pyxel.text(x, y, s, C_WHITE if self.money >= cost else C_RED)
            x += len(s) * 4 + 3
        gfx.star(x, y)
        pyxel.text(x + 6, y, "%d" % cred, C_WHITE if self.cred >= cred else C_RED)

    def draw_hq_panel(self):
        # The HQ tracks, then the HQ specials, HQ_SHOW rows at a time.
        h = 58
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h)
        pyxel.text(4, py + 3, "HQ UPGRADES", C_CYAN)
        self.draw_funds(py + 3)
        rows = len(HQ) + len(HQ_SPECS)
        for j in range(HQ_SHOW):
            i = self.hq_top + j
            y = py + 11 + j * 8
            if i == self.hq_sel:
                pyxel.rect(2, y - 1, 156, 8, C_BG)
                pyxel.rectb(2, y - 1, 156, 8, C_RED if self.deny else C_YELLOW)
            if i < len(HQ):
                name, desc, costs = HQ[i]
                lv = self.hq[i]
                if i == HQ_CREWS:
                    now = "%d CREWS" % len(self.crews)
                elif i == HQ_DONATE:
                    now = "$%d EACH" % DONATE_AMOUNT[lv]
                elif i == HQ_FREQ:
                    now = "EVERY %dS" % (DONATE_EVERY[lv] // 30)
                else:
                    now = "L%d" % (lv + 1)
                pyxel.text(6, y + 1, name, C_WHITE)
                pyxel.text(48, y + 1, now, C_GREEN)
                if lv < len(costs):
                    self.draw_price(108, y + 1, 0, costs[lv])
                else:
                    pyxel.text(108, y + 1, "MAX", C_GRAY)
            else:
                k = i - len(HQ)
                name, desc, cost, cred, rank, homes = HQ_SPECS[k]
                if self.hqs_own[k]:
                    pyxel.text(6, y + 1, name, C_YELLOW)
                    pyxel.text(108, y + 1, "OWNED", C_GREEN)
                elif self.hqs_open[k]:
                    pyxel.text(6, y + 1, name, C_YELLOW)
                    self.draw_price(108, y + 1, cost, cred)
                else:
                    pyxel.text(6, y + 1, name, C_GRAY)
                    pyxel.text(108, y + 1, "LOCKED", C_GRAY)
        # Arrows on the first and last rows while more rows are above or below.
        if self.hq_top > 0:
            y = py + 11
            pyxel.tri(154, y, 152, y + 3, 156, y + 3, C_GRAY)
        if self.hq_top + HQ_SHOW < rows:
            y = py + 11 + (HQ_SHOW - 1) * 8
            pyxel.tri(152, y + 1, 156, y + 1, 154, y + 4, C_GRAY)
        i = self.hq_sel
        if i < len(HQ):
            pyxel.text(4, py + h - 7, HQ[i][1], C_GRAY)
        elif self.hqs_open[i - len(HQ)]:
            pyxel.text(4, py + h - 7, HQ_SPECS[i - len(HQ)][1], C_GRAY)
        else:
            pyxel.text(4, py + h - 7, self.hqs_lock(i - len(HQ)), C_ORANGE)

    def draw_spec_panel(self):
        # A node's specials: OWNED, being fitted, the price, or LOCKED.
        n = self.node_at(self.cx, self.cy)
        if n is None:
            return
        h = 50
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h)
        pyxel.text(4, py + 3, "%s L%d SPECIALS" % (NODE_KINDS[n.kind].name, n.level + 1),
                   C_CYAN)
        self.draw_funds(py + 3)
        job = n.job
        for i in range(len(NODE_SPECS)):
            name, desc, cost, cred, need, how = NODE_SPECS[i]
            y = py + 11 + i * 8
            if i == self.spec_sel:
                pyxel.rect(2, y - 1, 156, 8, C_BG)
                pyxel.rectb(2, y - 1, 156, 8, C_RED if self.deny else C_YELLOW)
            if n.spec[i]:
                pyxel.text(6, y + 1, name, C_WHITE)
                pyxel.text(108, y + 1, "OWNED", C_GREEN)
            elif job is not None and job.kind == J_SPECIAL and job.spec == i:
                pyxel.text(6, y + 1, name, C_WHITE)
                pyxel.text(108, y + 1, "FITTING", C_CYAN)
            elif self.spec_open[i]:
                pyxel.text(6, y + 1, name, C_WHITE)
                self.draw_price(108, y + 1, cost, cred)
            else:
                pyxel.text(6, y + 1, name, C_GRAY)
                pyxel.text(108, y + 1, "LOCKED", C_GRAY)
        i = self.spec_sel
        if self.spec_open[i]:
            pyxel.text(4, py + h - 7, NODE_SPECS[i][1], C_GRAY)
        else:
            pyxel.text(4, py + h - 7, self.spec_lock(i), C_ORANGE)

    def draw_actor_panel(self):
        a = self.actor_at(self.cx, self.cy)
        if a is None:
            return
        h = 30
        py = self.panel_y(h)
        gfx.panel(0, py, 160, h, C_RED)
        pyxel.blt(4, py + 4, 0, gfx.SPR_ACTOR[0], gfx.SPR_ACTOR[1], 8, 8, 0)
        pyxel.text(16, py + 4, "BAD ACTOR", C_RED)
        job = a.job
        if job is None:
            st = "PINNED DOWN"
        elif job.crew is None:
            st = "QUEUED"
        elif job.crew.state == C_WORK:
            st = "TAKING DOWN %d%%" % (100 * job.crew.work // job.crew.work_total)
        else:
            st = "CREW EN ROUTE"
        pyxel.text(60, py + 4, st, C_YELLOW)
        pyxel.text(16, py + 11, "TAKE IT DOWN FOR", C_GRAY)
        gfx.star(82, py + 11)
        pyxel.text(88, py + 11, "%d" % bad_cred(self.wave), C_CYAN)
        if job is None:
            pyxel.rect(13, py + 19, 47, 9, C_BG)
            pyxel.rectb(13, py + 19, 47, 9, C_RED if self.deny else C_YELLOW)
            pyxel.text(16, py + 21, "TAKE DOWN", C_WHITE)
        pyxel.text(100, py + 21, "B:BACK", C_GRAY)

    def draw_strip(self):
        # The message strip under the HUD, over the view's top row.
        text, col, more = self.msg
        pyxel.rect(0, MAP_Y, 160, 8, C_BG)
        pyxel.line(0, MAP_Y + 7, 159, MAP_Y + 7, C_NAVY)
        pyxel.text(2, MAP_Y + 1, text, col)
        if more:
            pyxel.text(2 + len(text) * 4, MAP_Y + 1, more, C_WHITE)

    def draw_end(self):
        pyxel.dither(0.5)
        pyxel.rect(0, MAP_Y, 160, VIEW_H, C_BG)
        pyxel.dither(1.0)
        win = self.mode == M_WIN
        if win:
            t = pyxel.frame_count
            for i in range(6):
                a = (t * 7 + i * 53) % 160
                b = MAP_Y + (t * 3 + i * 29) % VIEW_H
                if b < 22 or b > 96 or a < 18 or a > 142:
                    pyxel.pset(a, b, (C_YELLOW, C_CYAN, C_GREEN)[i % 3])
        gfx.panel(16, 20, 128, 78, C_GREEN if win else C_RED)
        gfx.text_c(26, "MESH HOLDS!" if win else "GATEWAY LOST", C_GREEN if win else C_RED)
        gfx.text_c(35, "%s - %s" % (self.mapdef.name, MODES[self.mode_i][0]), C_CYAN)
        waves = self.wave if win else self.wave - 1
        gfx.text_c(44, "WAVES SURVIVED %d" % waves, C_WHITE)
        gfx.text_c(51, "NOISE SILENCED %d" % self.kills, C_WHITE)
        gfx.text_c(58, "USERS SERVED %d/%d" % (self.best_users, len(self.level.homes)), C_WHITE)
        taken = 0
        for a in self.actors:
            if a.gone:
                taken += 1
        gfx.text_c(65, "BAD ACTORS DOWN %d/%d" % (taken, len(self.actors)), C_WHITE)
        if win:
            gfx.text_c(74, "GATEWAY NEVER FELL!" if not self.fell else "THE GATEWAY FELL ONCE",
                       C_YELLOW if not self.fell else C_GRAY)
        gfx.text_c(86, "A:OVERTIME  B:MENU" if win else "A:RETRY  B:MENU", C_YELLOW)
