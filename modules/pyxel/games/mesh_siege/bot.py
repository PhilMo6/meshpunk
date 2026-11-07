# Mesh Siege test bot (not packaged): the autopilot plus a line per wave and
# checks that the game's bookkeeping holds. Enabled by _bot.txt next to
# main.py.
import gc
import pyxel
from data import *
from pilot import Pilot
import game as G


class Bot(Pilot):
    def __init__(self, g):
        Pilot.__init__(self, g)
        self.last_phase = g.phase
        self.maxe = 0
        self.frames = 0
        self.bad = set()
        self.idle = {}
        self.don_full = 0         # wave when both donation tracks were full
        self.hq_full = 0          # wave when every HQ track was full

    def report(self, key, msg):
        if key not in self.bad:
            self.bad.add(key)
            print("[bot] BAD %s t=%d %s" % (key, self.g.t, msg))

    def specials(self):
        # Node specials: letter and nodes fitted once unlocked, "." while
        # locked. HQ specials: letter if owned, lower case if unlocked.
        g = self.g
        out = ""
        for i in range(len(NODE_SPECS)):
            if g.spec_open[i]:
                out += "SMHC"[i] + str(sum(1 for x in g.nodes if x.spec[i]))
            else:
                out += "."
        out += "/"
        for i in range(len(HQ_SPECS)):
            c = "PNDU"[i]
            out += c if g.hqs_own[i] else (c.lower() if g.hqs_open[i] else ".")
        return out

    def check(self):
        g = self.g
        if g.cred < 0:
            self.report("cred-negative", "cred %d" % g.cred)
        for c in g.crews:
            if c.state == G.C_OUT or c.state == G.C_WORK:
                if c.job is None or c.job.crew is not c:
                    self.report("crew-job", "crew state %d without its job" % c.state)
            elif c.job is not None:
                self.report("crew-idle-job", "crew state %d holding a job" % c.state)
        held = [c.job for c in g.crews if c.job is not None]
        for j in g.jobs:
            if j.crew is not None:
                self.report("queued-crew", "queued job has a crew")
        for n in g.allnodes:
            if n.job is not None and n.job not in g.jobs and n.job not in held:
                self.report("lost-job", "node job %d neither queued nor held" % n.job.kind)
        for a in g.actors:
            if a.job is not None and a.job not in g.jobs and a.job not in held:
                self.report("lost-actor-job", "takedown neither queued nor held")
        if g.gate.down and g.gate.job is None:
            self.report("gate-no-repair", "gateway down with no repair job")
        if g.phase == G.WAVE and g.wave_t > 9000 and "long-wave" not in self.bad:
            self.report("long-wave", "wave %d still running: spawned %d/%d" % (
                g.wave, g.spawn_i, len(g.spawn_q)))
            for e in g.enemies:
                tg = e.target
                print("[bot]   %s alive=%d at %d,%d target=%s attack=%d park=%d stun=%d seen=%d" % (
                    e.k.name, e.alive, e.x, e.y,
                    None if tg is None else "%s@%d,%d down=%d" % (
                        "GW" if tg.kind == N_GATE else NODE_KINDS[tg.kind].name, tg.tx, tg.ty,
                        tg.down),
                    e.attack, e.park, e.stun, e.seen))
        live = set()
        for e in g.enemies:
            if e.alive and e.target is None and e.k.aim != AIM_WIND and not e.park:
                k = id(e)
                live.add(k)
                self.idle[k] = self.idle.get(k, 0) + 30
                if self.idle[k] > 900 and not g.gate.down:
                    self.report("noise-idle", "%s without a target for 30 s" % e.k.name)
        for k in list(self.idle):
            if k not in live:
                del self.idle[k]

    def act(self):
        g = self.g
        n = len(g.enemies)
        if n > self.maxe:
            self.maxe = n
        if g.mode == G.M_OVER or g.mode == G.M_WIN:
            if self.frames == 0:
                taken = sum(1 for a in g.actors if a.gone)
                found = sum(1 for a in g.actors if a.found)
                print("[bot] END map=%s %s wave=%d kills=%d built=%d fell=%d nulls=%d "
                      "actors=%d found=%d down=%d users=%d/%d money=%d cred=%d hq=%s spec=%s "
                      "donfull=%d hqfull=%d" % (
                          g.mapdef.name, "WIN" if g.mode == G.M_WIN else "LOSS", g.wave,
                          g.kills, g.built, 1 if g.fell else 0, g.nulls, len(g.actors), found,
                          taken, g.best_users, len(g.level.homes), g.money, g.cred, g.hq,
                          self.specials(), self.don_full, self.hq_full))
                kinds = [0] * len(NODE_KINDS)
                for x in g.nodes:
                    kinds[x.kind] += 1
                print("[bot] kinds " + " ".join(
                    "%s=%d" % (NODE_KINDS[i].name, kinds[i]) for i in range(len(kinds))))
            self.frames += 1
            if self.frames > 45:
                pyxel.quit()
            return
        if g.t % 30 == 0:
            self.check()
        if not self.don_full and g.hq[HQ_DONATE] == 3 and g.hq[HQ_FREQ] == 3:
            self.don_full = g.wave
        if not self.hq_full and sum(g.hq) == sum(len(h[2]) for h in HQ):
            self.hq_full = g.wave
        if g.phase == G.PREP and self.last_phase == G.WAVE:
            down = sum(1 for x in g.nodes if x.down)
            nulled = sum(1 for x in g.nodes if x.nulled)
            flat = sum(1 for x in g.nodes if x.flat)
            solar = sum(1 for x in g.nodes if x.power == P_SOLAR)
            gc.collect()
            print("[bot] wave %2d gw=%3d%% money=%4d cred=%4d nodes=%2d online=%2d down=%d "
                  "null=%d flat=%d/%d users=%d crews=%d hq=%s spec=%s actors=%d/%d "
                  "maxnoise=%2d t=%d heap=%dK" % (
                      g.wave, int(100 * g.gate.hp / g.gate.maxhp), g.money, g.cred,
                      len(g.nodes), g.online, down, nulled, flat, solar, g.users, len(g.crews),
                      g.hq, self.specials(), sum(1 for a in g.actors if a.gone), len(g.actors),
                      self.maxe, g.t, gc.mem_alloc() // 1024))
            self.maxe = 0
            for a in g.actors:
                if a.gone:
                    continue
                near = 1 << 29
                for x in g.allnodes:
                    if x.online:
                        d = ((x.x - a.x) ** 2 + (x.y - a.y) ** 2) ** 0.5 - x.radio
                        if d < near:
                            near = d
                print("[bot]   actor @%d,%d found=%d zone=%s hits=%d heard=%d gap=%dpx" % (
                    a.tx, a.ty, a.found, None if a.zone is None else len(a.zone), len(a.hits),
                    len(a.heard), near))
        self.last_phase = g.phase
        Pilot.act(self)
