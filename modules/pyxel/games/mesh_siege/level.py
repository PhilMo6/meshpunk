# Mesh Siege: a map as terrain, the trucks' route map, line of sight and the
# reach of grid power. No pyxel calls, so the maps can be checked with plain
# Python.
from data import (TILE, MAP_W, MAP_H, MAP_Y, TILE_CODES, TERRAIN, T_GATE, T_WATER, T_HOME,
                  POWER_TILES, GRID_REACH)

N = MAP_W * MAP_H
INF = 1 << 29
# (dx, dy, diagonal)
NEIGH = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0),
         (1, 1, 1), (1, -1, 1), (-1, 1, 1), (-1, -1, 1))


def tile_center(tx, ty):
    return tx * TILE + 4, MAP_Y + ty * TILE + 4


def tile_of(x, y):
    tx = int(x) // TILE
    ty = (int(y) - MAP_Y) // TILE
    if tx < 0 or ty < 0 or tx >= MAP_W or ty >= MAP_H:
        return -1
    return ty * MAP_W + tx


def flow_field(cost, goals):
    # Reverse Dijkstra on a bucket queue, from the goal tiles. dist[t] is the
    # cost of driving from t to a goal, nxt[t] the tile to drive onto next.
    # cost[t] == 0 means t cannot be entered. Diagonal steps cost 1.4x and may
    # not cut the corner of a tile that cannot be entered. Edge weights stay
    # below the 64 buckets (max cost 35 * 1.4).
    dist = [INF] * N
    nxt = [-1] * N
    buckets = [[] for _ in range(64)]
    for g in goals:
        dist[g] = 0
        buckets[0].append(g)
    pending = len(goals)
    d = 0
    while pending:
        b = buckets[d & 63]
        while b:
            u = b.pop()
            pending -= 1
            if dist[u] != d:
                continue
            cu = cost[u]
            cd = cu * 14 // 10
            ux = u % MAP_W
            uy = u // MAP_W
            for dx, dy, diag in NEIGH:
                vx = ux + dx
                vy = uy + dy
                if vx < 0 or vy < 0 or vx >= MAP_W or vy >= MAP_H:
                    continue
                v = vy * MAP_W + vx
                if not cost[v]:
                    continue
                if diag:
                    if not cost[uy * MAP_W + vx] or not cost[vy * MAP_W + ux]:
                        continue
                    nd = d + cd
                else:
                    nd = d + cu
                if nd < dist[v]:
                    dist[v] = nd
                    nxt[v] = u
                    buckets[nd & 63].append(v)
                    pending += 1
        d += 1
    return dist, nxt


class Level:
    def __init__(self, mapdef):
        self.mapdef = mapdef
        tiles = []
        for row in mapdef.rows:
            for ch in row:
                tiles.append(TILE_CODES[ch])
        if len(tiles) != N:
            raise ValueError("map %s has %d tiles" % (mapdef.name, len(tiles)))
        self.tiles = tiles
        self.gate = tiles.index(T_GATE)
        self.block = [TERRAIN[t].block for t in tiles]
        self.crew = [TERRAIN[t].crew for t in tiles]
        # The trucks' route map: every tile's way home to the gateway.
        self.route = flow_field(self.crew, (self.gate,))
        dist = self.route[0]
        self.reach = [d < INF for d in dist]
        self.water = [i for i in range(N) if tiles[i] == T_WATER]
        self.homes = [i for i in range(N) if tiles[i] == T_HOME]
        # Tiles grid power reaches: within GRID_REACH of a power tile.
        power = [False] * N
        for i in range(N):
            if tiles[i] in POWER_TILES:
                x0 = i % MAP_W
                y0 = i // MAP_W
                for y in range(max(0, y0 - GRID_REACH), min(MAP_H, y0 + GRID_REACH + 1)):
                    for x in range(max(0, x0 - GRID_REACH), min(MAP_W, x0 + GRID_REACH + 1)):
                        power[y * MAP_W + x] = True
        self.power = power

    def buildable(self, tx, ty):
        t = ty * MAP_W + tx
        return TERRAIN[self.tiles[t]].build and self.reach[t]

    def terrain_at(self, t):
        return TERRAIN[self.tiles[t]]

    def chain(self, t):
        # Tiles from t home to the gateway along the route map, t first.
        out = []
        nxt = self.route[1]
        while t >= 0 and len(out) < N:
            out.append(t)
            if t == self.gate:
                break
            t = nxt[t]
        return out

    def path(self, a, b):
        # Tiles to drive from a to b, a left out: up a's chain to where b's
        # chain joins it, then down b's chain.
        ca = self.chain(a)
        cb = self.chain(b)
        pos = {}
        for i in range(len(cb)):
            pos[cb[i]] = i
        for i in range(len(ca)):
            j = pos.get(ca[i])
            if j is not None:
                down = cb[:j]
                down.reverse()
                return ca[1:i + 1] + down
        return []

    def los(self, a, ha, b, hb):
        # Line of sight between antennas at heights ha and hb on tiles a and
        # b: blocked where the ground (or trees, or a building) along the way
        # stands above the straight line between them.
        ax = a % MAP_W
        ay = a // MAP_W
        bx = b % MAP_W
        by = b // MAP_W
        n = max(abs(bx - ax), abs(by - ay))
        if n <= 1:
            return True
        block = self.block
        for i in range(1, n):
            f = i / n
            x = int(ax + (bx - ax) * f + 0.5)
            y = int(ay + (by - ay) * f + 0.5)
            if block[y * MAP_W + x] > ha + (hb - ha) * f + 0.01:
                return False
        return True
