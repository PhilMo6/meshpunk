# Mesh Siege autopilot: plays the title demo (and drives the test bot).
# In order it takes down pinned bad actors, resets and repairs nodes, buys HQ
# specials once unlocked and HQ levels with cred (donations first), guards
# the gateway, gives the busiest node signed firmware while bad actors are
# about, builds toward bad actor search zones, gives flat solar nodes bigger
# batteries, then builds defence and relays out to homes.
import pyxel
from data import *
from level import tile_center
import game as G

ORDER = (N_PING, N_PING, N_FLOOD, N_PING, N_SLOW, N_YAGI, N_FLOOD, N_YAGI, N_PING, N_SLOW,
         N_YAGI, N_FLOOD)
# HQ levels bought with cred, in this order.
CRED_PLAN = (HQ_DONATE, HQ_FREQ, HQ_DONATE, HQ_CREWS, HQ_FREQ, HQ_TOOLS, HQ_ARMOR, HQ_DONATE,
             HQ_FREQ, HQ_CREWS, HQ_TOOLS, HQ_TRUCKS, HQ_ARMOR, HQ_TRUCKS, HQ_TOOLS, HQ_TRUCKS)
# HQ specials, most wanted first.
HQ_SPEC_PLAN = (H_MONITOR, H_SPARE, H_DF, H_UPLINK)
SIGN_MAX = 3              # nodes given signed firmware at most
# Tile offsets tried around each online node when looking for a site.
RING = ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1),
        (3, 0), (-3, 0), (0, 3), (0, -3), (2, 2), (2, -2), (-2, 2), (-2, -2),
        (5, 0), (-5, 0), (0, 5), (0, -5), (4, 3), (-4, 3), (4, -3), (-4, -3))


class Pilot:
    def __init__(self, g):
        self.g = g
        self.tick = 0
        self.oi = 0
        self.cred_i = 0
        self.hunting = {}     # id(bad actor) -> the relay last built to hunt it

    def sites(self, limit=14):
        # Empty buildable tiles that would link, around a sample of online nodes.
        g = self.g
        lv = g.level
        hosts = [n for n in g.allnodes if n.online and n.hops < MAX_HOPS]
        out = []
        seen = set()
        for i in range(min(limit, len(hosts))):
            u = hosts[pyxel.rndi(0, len(hosts) - 1)]
            for dx, dy in RING:
                tx = u.tx + dx
                ty = u.ty + dy
                if tx < 0 or ty < 0 or tx >= MAP_W or ty >= MAP_H:
                    continue
                t = ty * MAP_W + tx
                if t in seen:
                    continue
                seen.add(t)
                if not lv.buildable(tx, ty) or g.grid[t] is not None:
                    continue
                if g.link_for(tx, ty, 0.5)[0] is not None:
                    out.append((tx, ty))
        return out

    def new_homes(self, tx, ty, radio, height):
        g = self.g
        lv = g.level
        t = ty * MAP_W + tx
        cx, cy = tile_center(tx, ty)
        hh = TERRAIN[T_HOME].elev + HOME_ANTENNA
        r2 = radio * radio
        n = 0
        for i in range(len(lv.homes)):
            if g.home_on[i]:
                continue
            h = lv.homes[i]
            x, y = tile_center(h % MAP_W, h // MAP_W)
            if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r2 and lv.los(t, height, h, hh):
                n += 1
        return n

    def guard_score(self, tx, ty, rng):
        # Mesh load a tower here would cover, the gateway counting extra.
        g = self.g
        cx, cy = tile_center(tx, ty)
        r2 = rng * rng
        s = 0
        for n in g.allnodes:
            if n.built:
                dx = n.x - cx
                dy = n.y - cy
                if dx * dx + dy * dy <= r2:
                    s += n.load + 1 + (6 if n is g.gate else 0) + (3 if n.hurt_t else 0)
        return s

    def guard_site(self, k):
        # A linkable tile whose tower would reach the gateway, covering the
        # most of the mesh.
        g = self.g
        lv = g.level
        gate = g.gate
        best = None
        bs = -1
        for ty in range(max(0, gate.ty - 4), min(MAP_H, gate.ty + 5)):
            for tx in range(max(0, gate.tx - 4), min(MAP_W, gate.tx + 5)):
                t = ty * MAP_W + tx
                if not lv.buildable(tx, ty) or g.grid[t] is not None:
                    continue
                te = lv.terrain_at(t)
                r = int(k.rng[0] * te.rng + 0.5)
                cx, cy = tile_center(tx, ty)
                if (cx - gate.x) ** 2 + (cy - gate.y) ** 2 > (r - 4) * (r - 4):
                    continue
                s = self.guard_score(tx, ty, r) * 10 // int(te.install * 10)
                if s > bs and g.link_for(tx, ty, 0.5)[0] is not None:
                    best = (tx, ty)
                    bs = s
        return best

    def hunt_site(self, a):
        # Linkable tiles round the zone's middle, by how much of the zone a
        # relay there would hear; failing those, the mesh's site nearest the
        # zone, to reach toward it.
        g = self.g
        lv = g.level
        k = NODE_KINDS[N_RELAY]
        mx = 0
        my = 0
        for z in a.zone:
            mx += z % MAP_W
            my += z // MAP_W
        mx = int(mx / len(a.zone) + 0.5)
        my = int(my / len(a.zone) + 0.5)
        best = None
        bs = 0
        for ty in range(max(0, my - 5), min(MAP_H, my + 6)):
            for tx in range(max(0, mx - 5), min(MAP_W, mx + 6)):
                t = ty * MAP_W + tx
                if not lv.buildable(tx, ty) or g.grid[t] is not None:
                    continue
                te = lv.terrain_at(t)
                r = int(k.radio[0] * te.radio + 0.5)
                cx, cy = tile_center(tx, ty)
                s = 0
                for z in a.zone:
                    zx, zy = tile_center(z % MAP_W, z // MAP_W)
                    if (zx - cx) * (zx - cx) + (zy - cy) * (zy - cy) <= r * r:
                        s += 1
                if s > bs and g.link_for(tx, ty, MASTS[N_RELAY])[0] is not None:
                    best = (tx, ty)
                    bs = s
        if best is not None:
            return best
        bd = 1 << 29
        for tx, ty in self.sites(20):
            d = (tx - mx) * (tx - mx) + (ty - my) * (ty - my)
            if d < bd:
                best = (tx, ty)
                bd = d
        return best

    def power_for(self, tx, ty):
        return P_GRID if self.g.level.power[ty * MAP_W + tx] else P_SOLAR

    def actors_about(self):
        # A bad actor is on the air, or one more may still be installed.
        g = self.g
        if g.installs < ACTOR_CAP[g.mode_i] and g.installs < len(g.mapdef.actors):
            return True
        for a in g.actors:
            if not a.gone:
                return True
        return False

    def sign(self):
        # Signed firmware for the online node carrying the most of the mesh.
        g = self.g
        name, desc, cost, cred, need, how = NODE_SPECS[S_SIGNED]
        if g.money < cost or g.cred < cred:
            return False
        signed = 0
        best = None
        for n in g.nodes:
            if n.spec[S_SIGNED] or (n.job is not None and n.job.kind == G.J_SPECIAL):
                signed += 1
            elif n.online and n.job is None and n.load > 1:
                if best is None or n.load > best.load:
                    best = n
        if signed >= SIGN_MAX or best is None:
            return False
        return g.order_special(best, S_SIGNED)

    def spend(self):
        g = self.g
        # Pinned bad actors first: a crew takes them down for free.
        for a in g.actors:
            if a.found and not a.gone and a.job is None:
                g.order_takedown(a)
                return
        # Nullified, then down nodes.
        for want in (0, 1):
            for n in g.nodes:
                if not n.built or n.job is not None:
                    continue
                if (want == 0 and n.nulled) or (want == 1 and n.down):
                    if g.money >= g.repair_cost(n):
                        g.order_repair(n)
                    return
        # HQ specials once unlocked, then HQ levels as the cred comes in.
        for k in HQ_SPEC_PLAN:
            if g.hqs_open[k] and not g.hqs_own[k]:
                name, desc, cost, cred, rank, homes = HQ_SPECS[k]
                if g.money >= cost and g.cred >= cred:
                    g.buy_hq_special(k)
                    return
        if self.cred_i < len(CRED_PLAN):
            track = CRED_PLAN[self.cred_i]
            lvl = g.hq[track]
            costs = HQ[track][2]
            if lvl >= len(costs):
                self.cred_i += 1
            elif g.cred >= costs[lvl]:
                g.buy_hq(track)
                self.cred_i += 1
                return
        # Keep the crews busy, not buried.
        if len(g.jobs) >= 2:
            return
        # Bricks and flares go for the gateway: keep towers on it.
        gate = g.gate
        guard = 0
        for n in g.nodes:
            if n.kind != N_RELAY:
                dx = n.x - gate.x
                dy = n.y - gate.y
                r = n.rng if n.rng else NODE_KINDS[n.kind].rng[0]
                if dx * dx + dy * dy <= r * r:
                    guard += 1
        if guard < 2 + g.wave // 2:
            kind = ORDER[self.oi % len(ORDER)]
            if kind == N_SLOW and guard < 2:
                kind = N_PING
            k = NODE_KINDS[kind]
            if g.money < k.cost[0]:
                return
            site = self.guard_site(k)
            if site is not None:
                g.order_build(kind, site[0], site[1], self.power_for(site[0], site[1]))
                self.oi += 1
                return
        if g.spec_open[S_SIGNED] and self.actors_about() and self.sign():
            return
        relay_cost = NODE_KINDS[N_RELAY].cost[0]
        # Hunt: a relay where its radio covers the most of a search zone, one
        # at a time per bad actor.
        for a in g.actors:
            if a.found or a.gone or not a.zone:
                continue
            n = self.hunting.get(id(a))
            if n is not None and not n.gone and not n.built:
                continue
            if g.money < relay_cost:
                return
            site = self.hunt_site(a)
            if site is not None:
                self.hunting[id(a)] = g.order_build(N_RELAY, site[0], site[1],
                                                    self.power_for(site[0], site[1]))
                return
        # Flat solar nodes get a bigger battery, then a bigger panel.
        for n in g.nodes:
            if n.built and n.flat and n.job is None:
                c = g.battery_cost(n) or g.panel_cost(n)
                if c and g.money >= c:
                    if g.battery_cost(n):
                        g.order_battery(n)
                    else:
                        g.order_panel(n)
                    return
        attack = 0
        for n in g.nodes:
            if n.kind != N_RELAY:
                attack += 1
        if attack < 2 + g.wave:
            kind = ORDER[self.oi % len(ORDER)]
            k = NODE_KINDS[kind]
            if g.money < k.cost[0]:
                return
            best = None
            bs = 0
            for tx, ty in self.sites():
                te = g.level.terrain_at(ty * MAP_W + tx)
                s = self.guard_score(tx, ty, int(k.rng[0] * te.rng + 0.5)) * 10
                s = s // int(te.install * 10)
                if s > bs:
                    best = (tx, ty)
                    bs = s
            if best is not None:
                g.order_build(kind, best[0], best[1], self.power_for(best[0], best[1]))
                self.oi += 1
            return
        # Homes pay: relay out to them while keeping some cash.
        if g.money >= relay_cost + 30:
            k = NODE_KINDS[N_RELAY]
            best = None
            bh = 0
            for tx, ty in self.sites(20):
                t = ty * MAP_W + tx
                te = g.level.terrain_at(t)
                h = self.new_homes(tx, ty, int(k.radio[0] * te.radio + 0.5),
                                   te.elev + MASTS[N_RELAY])
                if h > bh:
                    best = (tx, ty)
                    bh = h
            if best is not None:
                g.order_build(N_RELAY, best[0], best[1], self.power_for(best[0], best[1]))
                return
        best = None
        bs = -1
        for n in g.nodes:
            if n.level >= 2 or not n.built or n.job is not None:
                continue
            if n.kind == N_RELAY:
                s = 5 if g.wave >= 10 else -1
            else:
                s = self.guard_score(n.tx, n.ty, n.rng)
            if s > bs:
                best = n
                bs = s
        if best is not None and bs >= 0 and g.money >= g.upgrade_cost(best):
            g.order_upgrade(best)

    def act(self):
        g = self.g
        self.tick += 1
        if self.tick % 10 or g.mode != G.M_PLAY:
            return
        self.spend()
        # Send the wave once the crews are idle and nothing is queued.
        if g.phase == G.PREP and not g.jobs and g.free_crews() == len(g.crews):
            g.start_wave(True)
