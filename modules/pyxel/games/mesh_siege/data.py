# Mesh Siege: terrain, maps, node and noise stats, power, bad actors, crews,
# waves.

TILE = 8
MAP_W = 32
MAP_H = 32
MAP_Y = 8                 # screen y of the map view's top edge
VIEW_W = 160              # map view size in pixels
VIEW_H = 104
WORLD_W = MAP_W * TILE
WORLD_H = MAP_H * TILE
MAX_HOPS = 6
LAST_WAVE = 20

# Palette slots
C_BG = 0
C_NAVY = 1
C_WATER = 2
C_GRASS_D = 3
C_GRASS = 4
C_HILL = 5
C_ROCK = 6
C_WHITE = 7
C_RED = 8
C_ORANGE = 9
C_YELLOW = 10
C_GREEN = 11
C_CYAN = 12
C_PURPLE = 13
C_DIRT = 14
C_GRAY = 15

PALETTE = (
    0x0B0D17, 0x1A2240, 0x1E4A7A, 0x173826, 0x245A3A, 0x6B7F3A, 0x4E5566, 0xEEF2F8,
    0xFF3B5C, 0xFF8F3A, 0xFFDD4A, 0x4CFF8F, 0x3AD8FF, 0xA15CFF, 0x6E4F36, 0x98A2B8,
)

# Terrain
T_FLAT = 0
T_VALLEY = 1
T_HILL = 2
T_MOUNT = 3
T_WATER = 4
T_FOREST = 5
T_ROAD = 6
T_BRIDGE = 7
T_BUILDING = 8
T_GATE = 9
T_HOME = 10
T_SUB = 11

TILE_CODES = {".": T_FLAT, ",": T_VALLEY, "^": T_HILL, "M": T_MOUNT, "~": T_WATER,
              "t": T_FOREST, "=": T_ROAD, "b": T_BRIDGE, "#": T_BUILDING, "G": T_GATE,
              "h": T_HOME, "S": T_SUB}


class Terrain:
    def __init__(self, name, desc, elev, block, build, radio, rng, install, crew):
        self.name = name
        self.desc = desc
        self.elev = elev          # ground height a node's antenna stands on
        self.block = block        # obstacle height for line of sight
        self.build = build        # nodes can be built here
        self.radio = radio        # link radius multiplier
        self.rng = rng            # attack radius multiplier
        self.install = install    # install and repair time multiplier
        self.crew = crew          # truck cost to enter, tenths of a flat tile; 0 = no


TERRAIN = (
    Terrain("FIELD", "OPEN GROUND", 1.0, 1.0, True, 1.0, 1.0, 1.0, 10),
    Terrain("VALLEY", "LOW GROUND: SHORT RADIO", 0.0, 0.0, True, 0.8, 0.9, 1.0, 10),
    Terrain("HILL", "RADIO+ RANGE+, SLOW INSTALL", 2.0, 2.0, True, 1.3, 1.15, 1.6, 20),
    Terrain("MOUNTAIN", "BEST RADIO, SLOWEST INSTALL", 3.0, 3.0, True, 1.6, 1.3, 2.5, 35),
    Terrain("WATER", "NO BUILDING, NO TRUCKS", 0.0, 0.0, False, 1.0, 1.0, 1.0, 0),
    Terrain("FOREST", "TREES BLOCK LOW LINKS", 1.0, 1.7, True, 0.85, 0.9, 1.3, 18),
    Terrain("ROAD", "FAST FOR CREWS", 1.0, 1.0, False, 1.0, 1.0, 1.0, 5),
    Terrain("BRIDGE", "THE WAY ACROSS FOR TRUCKS", 0.5, 0.5, False, 1.0, 1.0, 1.0, 5),
    Terrain("ROOFTOP", "RADIO+, GRID POWER", 2.5, 2.5, True, 1.3, 1.15, 1.8, 25),
    Terrain("GATEWAY", "KEEP IT UP", 1.0, 1.0, False, 1.0, 1.0, 1.0, 10),
    Terrain("HOMES", "USERS: CONNECT THEM FOR CRED", 1.0, 1.4, False, 1.0, 1.0, 1.0, 10),
    Terrain("SUBSTATION", "GRID POWER FOR NODES NEARBY", 1.0, 1.5, False, 1.0, 1.0, 1.0, 10),
)

# Tiles that feed grid power to nodes within GRID_REACH tiles.
POWER_TILES = (T_BUILDING, T_HOME, T_GATE, T_SUB)
GRID_REACH = 2

# Antenna mast height above the ground, per node kind (N_GATE last).
MASTS = (1.0, 0.5, 0.5, 0.5, 0.5, 1.5)
HOME_ANTENNA = 0.5        # a user's radio sits this high on the house

# Money and cred. Money comes from a baseline income and donations; cred
# comes from homes served, the mesh's size, kills, cleared waves, early calls
# and bad actors taken down.
INCOME_EVERY = 60         # frames between baseline payments
INCOME = 1                # dollars per baseline payment
DONATE_AMOUNT = (12, 20, 32, 48)        # dollars per donation, per DONATIONS level
DONATE_EVERY = (900, 720, 540, 360)     # frames between donations, per FREQUENCY level
CRED_EVERY = 240          # frames between cred for homes served and mesh size
CRED_NODES = 5            # online nodes per point of cred
OUTAGE_EVERY = 60         # frames between cred losses while the gateway is down
OUTAGE_CRED = 2
NULL_CRED = 5             # cred lost per nullified node
EARLY_EVERY = 60          # countdown frames left per point of cred for an early call


class MapDef:
    def __init__(self, name, blurb, stars, money, hp_scale, clouds, actors, rows):
        self.name = name
        self.blurb = blurb
        self.stars = stars
        self.money = money
        self.hp_scale = hp_scale
        self.clouds = clouds      # (clouds at once, smallest, largest radius)
        self.actors = actors      # built-node counts that install the next bad actor
        self.rows = rows


MAPS = (
    MapDef("FARMLAND", "A RIVER, A VILLAGE, OPEN FIELDS", 1, 200, 1.0, (2, 14, 22),
           (8, 14, 20, 26), (
               "ttttt..........=..........ttt...",
               "t.ttt^^.=..h...=.....h...ttttt..",
               ".tth.^^^=......=........=.tttt..",
               ".ttt^^.^=......=........=tt..h..",
               "....^^^^=......=...h....=.ttt...",
               "~~~~^.^.=......=........=....^^^",
               "...~~~~~b~.....=........=......^",
               "........=~~~~~~b~~......=....^^^",
               "........=......=.~~~~~~~b~......",
               "........=.tt...=........=~~~~~~~",
               "........=t.t...=...^^...=.......",
               "....h...=tt.t..=..^^^h^.=....tt.",
               "........=ttt.t.=..^..^^.=..ht.tt",
               "........=.ht##.=.##^^^^.=..ttttt",
               ".S......=...##.=.##^^^..=..tttt.",
               "........=......=........=....t..",
               "===============G================",
               "......=........=...........=....",
               "......=.....##.=.###.......=....",
               "......=.....##.=.........t.=.h..",
               "..h...=........=.........tt=....",
               "..tt..=...h....=.....h...tt=....",
               ".tt.tt=........=...........=....",
               "..tttt=........=...........=..S.",
               ".ttttt=........=..........^=....",
               "...tt.======================....",
               ".......^^^.....=.....ttt^^^^....",
               "........^^.....=....t..t^^^.....",
               "...h....^^..ttt=...ttt.t^^^.....",
               "............ttt=...htt......h...",
               "............h.t=................",
               "...............=................",
           )),
    MapDef("VALLEY", "RIDGES ALL ROUND, LOW GROUND BETWEEN", 3, 210, 1.05, (3, 14, 24),
           (7, 13, 19, 25), (
               "MMMMMM^.=..^MMMMMMMM^....^MMMMMM",
               "MMMMMM^.=..^MMMMMMMM^.=..^MMMMMM",
               "MMMMMM^t=t.^MMMMMMMM^.=th^MMMMMM",
               "MMMMMM^t=h.^MMMMMMMM^.=t.^MMMMMM",
               "MMMMMM^.=..^MMMMMMMM^.=t.^MMMMMM",
               "MMMMMM^.=..^MMMMMMMM^.=..^MMMMMM",
               "^^^^^^^.=..^^^^^^^^^^.=..^^^^^^^",
               "^^^^^^^.=..^^^^^^^^^^.=..^^^^^^^",
               "...ttt..=.............=...ttt...",
               "...t.t..=....h........=...t.t...",
               ",,,h,,,,=,,,,,,####,,,=,,,,,,h,,",
               ",,,,,,,,=,,,,,,####,,,=,,,,,,,,,",
               "=S==============================",
               ",,,,,,,,,,=,,,,,G,,,h,,,,,=,,,,,",
               ",,h,,,,,,,=,,,,,,,,,,,,,,,=,,,h,",
               "~~~~~~~,,,=,,,~~~~~~~~~,,,=,,,,,",
               ",,,,,,~,,,=,,,~,,,,,,,~,,,=,,,,,",
               ",,,,,,~~~~b~~~~,,,,,,,~~~~b~~~~~",
               ",,,,,,,,,,=,,,,,,,,,,,,,,,=,,,,,",
               ",,,,,,,,h,=,,,,,,h,,,,,,,,=,,,,,",
               "==============================S=",
               ",,,,,=,,,,,,,,,,,,,=,,,,,,,,h,,,",
               ".....=........t....=....h....t..",
               ".....=......h.t....=........ttt.",
               "^^^^.=..^^^^^^^^^^.=..^^^^^^^^^^",
               "^^^^.=..^^^^^^^^^^.=..^^^^^^^^^^",
               "MMM^.=..^MMMMMMMM^.=..^MMMMMMMMM",
               "MMM^.=..^MMMMMMMM^.=t.^MMMMMMMMM",
               "MMM^t=h.^MMMMMMMM^.=tt^MMMMMMMMM",
               "MMM^t=..^MMMMMMMM^.=tt^MMMMMMMMM",
               "MMM^.=..^MMMMMMMM^.=..^MMMMMMMMM",
               "MMM^.=..^MMMMMMMM^..h.^MMMMMMMMM",
           )),
    MapDef("METRO", "GRID POWER EVERYWHERE, NO VIEW", 3, 210, 1.05, (2, 12, 20),
           (7, 13, 19, 25), (
               "#################~~#############",
               "####h############~~###h#########",
               "#################bb#############",
               "###.ttt.#########~~#############",
               "###..ttt###h#####~~####h########",
               "###ttttt#########~~##########h##",
               "###t.t..##S######~~#############",
               "###..tt.#########~~#############",
               "#################bb#############",
               "#################~~##.....######",
               "#h###############~~##.tt..######",
               "##########h######~~##..tt.######",
               "#################~~##.ttt.####h#",
               "#################~~##.....######",
               "##############G##bb#############",
               "###.....#########~~#############",
               "###.ttt.#########~~#####h#######",
               "###.tt..###h#####~~#############",
               "###.ttt.#########~~#########S###",
               "###.....#########~~#############",
               "#################bb#############",
               "#########.ttt.###~~##.....######",
               "####h####t.ttt###~~##.....###h##",
               "#########tt...##h~~##.....######",
               "#########t.tt.###~~##.....######",
               "#########.tt..###~~##.....######",
               "#################bb#############",
               "#################~~########.ttt.",
               "#################~~####h###ttttt",
               "#####h######h####~~########tttt.",
               "#################~~########ttttt",
               "#################~~########.ttt.",
           )),
    MapDef("HIGHLANDS", "PEAKS, PASSES AND CLOUD", 2, 220, 1.1, (4, 16, 28),
           (7, 13, 19, 25), (
               "MMM^^^MMMMMMM^.,,,.MMMMMMMMMMMMM",
               "MMM,,,MMMMMMM^,,.,,MMMMMMMM^^MMM",
               "M^,,,,,^MMMMM^,,h,,MMMMMMMM,,MMM",
               "M,,h,,.,MMMMM^,,,,t^MMMMM^,,,,,M",
               "^,.,=.,,^MMMMM..tt.MMMMMMM.h,,,,",
               "M,,,==h,.MMMMM.....^MMMMM,,,=,,,",
               "M^,,,==...MMM^.....^MMM^.t,==,,,",
               "MM^..,==...^MM.....^MMM..t==.h,^",
               "MMMM...==...M^.....^M^...==,,,^M",
               "MMMM^...==.h.M.....M^...==...^MM",
               "MMMMMM...==........M...==...MMMM",
               "MMMMMMM...==.t........==...^MMMM",
               "MMMMMMM^...==........==...^MMMMM",
               "MMMMMMMMM...==##...h==...M^^MM^^",
               "MMMMMMMMM^...==....==........,,,",
               "MMMMMMMMMMM...==..==........,,h,",
               "MMMMMMMMMMM.S..=G==============,",
               "MMMMMMMMMM^...====..........,,,,",
               "MMMMMMMMMM...h=..==~~~.......,,,",
               "MMMMMMMM^...==....=b~~tM^^M^^^M^",
               "MMMMMMM^...==......==tt.MMMMMMMM",
               "MMMMMMM...==........==...MMMMMMM",
               "MMMMM^...==...^^MM...==...MMMMMM",
               "MMMM^...==...^MMMM^...==...MMMMM",
               "MMM^,t,==...^MMMMMM^...==...^MMM",
               "MM^.,t==...MMMMMMMMM^...==,,,^MM",
               "M^,,,==h,.MMMMMMMMMMMM...==,,,^M",
               "M^,,,=.,,^MMMMMMMMMMMM^.,.h=,,,^",
               "M^,,h.,,,MMMMMMMMMMMMMM^,,,=,,,^",
               "MM^,,,,,^MMMMMMMMMMMMMMMM,,,h,,M",
               "MMMM,,MMMMMMMMMMMMMMMMMMM,,,,,^M",
               "MMMMMMMMMMMMMMMMMMMMMMMMM^,,,^MM",
           )),
    MapDef("COAST", "SEA FOG, A HARBOUR, TWO ISLANDS", 2, 220, 1.1, (4, 16, 30),
           (7, 13, 19, 25), (
               ".........~.ttt=.................",
               ".........~.t.t=..h..............",
               "..httt...~tt.t=...^^^h..........",
               "..ttttth.~.tt.=...^^..^^^^..^^..",
               "..tt.tt..~.ttt=...^^^.~~~~~~~~~~",
               "..ttt.t..~....=.......~~~~~~~~~~",
               ".S.ttt...~~~~.=.......~~~~~~~~~~",
               "............~.=.........^~~~~~~~",
               "......^^....~.=ttt......^~~~~~~~",
               ".......^^.h.~.=##t.......~~~~~~~",
               "......^^^...~.=##t.h.....~~~~~~~",
               "......h.....~.=..........~...~~~",
               "............~~b~~~~........h.^~~",
               "==================b=========.^~~",
               "..t=.t........G...~...........~~",
               ".tt=tt.....##.=##.~.....^~.^.~~~",
               ".tt=tt.....##.=##.......^~~~~~~~",
               "...=t..h......=##........~~~~~~~",
               ".h.=........^.=..........~~~~~~~",
               "...=........^^=.h.......^~~~~~~~",
               "...=.....t..^.=.........^~~~~~~~",
               "...=...tttt...==========b~~~~~~~",
               "...=...tt.tt..=.....=..~~~~~~~~~",
               "...=...tttt...=.....=..~~~~~~~~~",
               "...=.S..ttt...=.....=.^~~~~~~~~~",
               "...=......h...=......h.~~~~~~~~~",
               "...=................^^~~~~~~~~~~",
               ".tt=.............^^~~~~~~~~~~~~~",
               "...=............^.~~~~~~~~~~~~~~",
               ".tt=.............~~~~~~~~~~~~~~~",
               "...=.h...........~~~~~~~~~~~~~~~",
               "...=............~~~~~~~~~~~~~~~~",
           )),
)


# Nodes. Per level lists: [L1, L2, L3].
N_RELAY = 0
N_PING = 1
N_FLOOD = 2
N_YAGI = 3
N_SLOW = 4
N_GATE = 5


class NodeKind:
    def __init__(self, name, desc, cost, radio, rng, dmg, cd, hp, install, drain,
                 slow=(0, 0, 0)):
        self.name = name
        self.desc = desc
        self.cost = cost          # build, upgrade to L2, upgrade to L3
        self.radio = radio        # link radius in pixels
        self.rng = rng            # attack radius in pixels
        self.dmg = dmg
        self.cd = cd              # frames between shots
        self.hp = hp              # hit points before the node goes down
        self.install = install    # install frames on flat ground
        self.drain = drain        # battery use per frame, solar nodes
        self.slow = slow          # speed fraction removed (SF12 only)


NODE_KINDS = (
    NodeKind("RELAY", ("REPEATER: BIG RADIO CIRCLE", "L3 IS HARDENED: CANNOT BE JAMMED"),
             (25, 30, 45), (40, 50, 60), (0, 0, 0), (0, 0, 0), (0, 0, 0), (50, 70, 90), 120,
             0.8),
    NodeKind("PING", ("FIRES PACKETS AT ONE TARGET", "CHEAP AND QUICK TO INSTALL"),
             (30, 30, 50), (24, 24, 24), (32, 36, 40), (3, 5, 8), (18, 15, 12), (40, 55, 70),
             90, 1.0),
    NodeKind("FLOOD", ("BROADCAST PULSE HITS ALL NEARBY", "WEAK AGAINST ARMOR"),
             (60, 50, 80), (24, 24, 24), (22, 25, 28), (4, 7, 11), (48, 42, 36), (50, 70, 90),
             150, 1.2),
    NodeKind("YAGI", ("LONG RANGE BEAM, IGNORES ARMOR", "HITS THE TOUGHEST TARGET"),
             (75, 70, 110), (24, 24, 24), (60, 68, 76), (18, 32, 52), (60, 54, 46),
             (40, 55, 70), 180, 1.3),
    NodeKind("SF12", ("SPREADING FACTOR 12: SLOWS NOISE", "L3 ALSO DAMAGES"),
             (45, 40, 60), (24, 24, 24), (28, 31, 34), (0, 0, 1), (10, 10, 10), (50, 70, 90),
             120, 1.0, (0.35, 0.45, 0.55)),
)
GATE_RADIO = 48
GATE_HP = 200
SELL_RATE = 0.7
UPGRADE_WORK = 0.7        # an upgrade takes this share of the install time
REPAIR_COST = 0.25        # share of what the node cost, for a full repair

# Power. Grid nodes run forever; solar nodes run on a battery that only
# charges in sunshine.
P_GRID = 0
P_SOLAR = 1
POWER_NAMES = ("GRID", "SOLAR")
BAT_CAP = (600, 1050, 1500)   # battery per level, in frames of running
PANEL_RATE = (2.0, 3.0, 4.0)  # charge per level, times the node's own use
BAT_WAKE = 0.3                # share of a full battery that brings a node back
BAT_COST = (25, 40)           # battery upgrades to L2, L3
PANEL_COST = (25, 40)
POWER_WORK = 90               # frames to fit a battery or panel upgrade
POWER_EVERY = 8               # frames between battery updates
CLOUD_SPEED = (0.12, 0.28)

# Crews and jobs
CREWS_START = 1
CREW_SPEED = 1.0          # pixels per frame on flat ground
CREW_SAFE = 16            # no work goes on while noise is this close to the site
RESET_WORK = 120          # frames to clear a nullified node
RESET_COST = 0
GATE_REPAIR_WORK = 150    # frames to bring the gateway back up
GATE_DOWN_LIMIT = 600     # frames the gateway may stay down
TAKEDOWN_WORK = 300       # frames to take a bad actor off the air

# HQ tracks: (name, description, cred cost per level)
HQ = (
    ("CREWS", "HIRE ANOTHER INSTALL CREW", (120, 210)),
    ("TRUCKS", "4X4 TRUCKS: FASTER TRAVEL", (60, 110, 180)),
    ("TOOLS", "FASTER INSTALLS AND REPAIRS", (60, 110, 180)),
    ("ARMOR", "TOUGHER ENCLOSURES: MORE HP", (90, 165)),
    ("DONATIONS", "EACH DONATION PAYS MORE", (45, 90, 150)),
    ("FREQUENCY", "DONATIONS COME MORE OFTEN", (45, 90, 150)),
)
HQ_CREWS = 0
HQ_TRUCKS = 1
HQ_TOOLS = 2
HQ_ARMOR = 3
HQ_DONATE = 4
HQ_FREQ = 5
TRUCK_SPEED = (1.0, 1.35, 1.7, 2.0)
TOOL_TIME = (1.0, 0.75, 0.55, 0.4)
ARMOR_HP = (1.0, 1.5, 2.0)

# Specials cost money and cred, and unlock during a game.
# Node specials: (name, description, dollars, cred, count that unlocks it,
# what to count). A crew fits one to a node.
NODE_SPECS = (
    ("SIGNED FW", "SIGNED FIRMWARE: CAN'T BE NULLIFIED", 40, 25, 1, "TAKE DOWN A BAD ACTOR"),
    ("TALL MAST", "+1 ANTENNA HEIGHT AND +15% RADIO", 30, 15, 3, "BUILD ON HIGH GROUND"),
    ("HARDENED", "SHIELDED RADIO: CAN'T BE JAMMED", 35, 20, 5, "SILENCE JAMMERS"),
    ("ARMORED CASE", "TOUGH CASE: +50% HP", 30, 15, 5, "REPAIR DOWNED NODES"),
)
S_SIGNED = 0
S_MAST = 1
S_HARD = 2
S_CASE = 3
HIGH_GROUND = (T_HILL, T_MOUNT, T_BUILDING)
MAST_UP = 1.0             # TALL MAST: added antenna height
MAST_RADIO = 1.15         # TALL MAST: radio multiplier
CASE_HP = 1.5             # ARMORED CASE: HP multiplier
SPEC_WORK = 120           # frames to fit a node special

# HQ specials: (name, description, dollars, cred, HQ rank, homes). Rank is the
# number of HQ levels bought across all tracks; homes is the most homes served
# at once this game.
HQ_SPECS = (
    ("SPARE PARTS", "REPAIRS COST HALF, TAKE HALF THE TIME", 80, 40, 2, 4),
    ("NET MONITOR", "SENDS CREWS TO DOWN AND NULLED NODES", 100, 50, 4, 6),
    ("DIRECTION FINDING", "2 NODES PIN A BAD ACTOR, NOT 3", 120, 60, 6, 8),
    ("BACKUP UPLINK", "GATEWAY MAY STAY DOWN 40 S, NOT 20", 150, 80, 8, 10),
)
H_SPARE = 0
H_MONITOR = 1
H_DF = 2
H_UPLINK = 3


# Noise (enemies)
E_STATIC = 0
E_CHIRP = 1
E_BRICK = 2
E_JAMMER = 3
E_STORM = 4
E_FLARE = 5
E_ECHO = 6
E_BAD = 7                 # a bad actor's signal

# What a noise aims at.
AIM_NEAR = 0              # the nearest node
AIM_GATE = 1              # the gateway
AIM_BRANCH = 2            # the node carrying the most of the mesh
AIM_BOUNCE = 3            # node after node
AIM_WIND = 4              # nothing: it rides the wind across


class NoiseKind:
    def __init__(self, name, desc, hp, speed, armor, reward, dmg, jam, aim, u, v, size):
        self.name = name
        self.desc = desc
        self.hp = hp
        self.speed = speed        # pixels per frame
        self.armor = armor        # subtracted from each hit (beams pierce)
        self.reward = reward      # cred for silencing it
        self.dmg = dmg            # node HP per hit, or per second while smashing
        self.jam = jam            # jam radius in pixels (0 = none)
        self.aim = aim
        self.u = u                # sprite position in image bank 0 (frame A;
        self.v = v                # frame B is one sprite height below)
        self.size = size


NOISE_KINDS = (
    NoiseKind("STATIC", "HITS THE NEAREST NODE", 12, 0.25, 0, 3, 6, 0, AIM_NEAR, 0, 16, 8),
    NoiseKind("CHIRP", "FAST SWARMS OF SMALL HITS", 5, 0.4, 0, 1, 2, 0, AIM_NEAR, 8, 16, 8),
    NoiseKind("BRICK", "ARMORED: SMASHES THE GATEWAY", 36, 0.18, 2, 7, 6, 0, AIM_GATE,
              16, 16, 8),
    NoiseKind("JAMMER", "PARKS BY A BRANCH AND JAMS IT", 22, 0.22, 0, 9, 0, 20, AIM_BRANCH,
              24, 16, 8),
    NoiseKind("STORM", "RIDES THE WIND, ZAPS NODES", 26, 0.2, 0, 6, 5, 0, AIM_WIND, 32, 16, 8),
    NoiseKind("FLARE", "BOSS: JAMS ROUND THE GATEWAY", 220, 0.1, 2, 60, 5, 22, AIM_GATE,
              48, 16, 16),
    NoiseKind("ECHO", "BOUNCES FROM NODE TO NODE", 14, 0.32, 0, 4, 4, 0, AIM_BOUNCE, 40, 16, 8),
    NoiseKind("BAD", "BAD ACTOR SIGNAL: NULLIFIES", 16, 0.4, 0, 5, 0, 0, AIM_BRANCH,
              64, 16, 8),
)
HIT_REACH = 5             # a noise hits a node from this close
PARK_REACH = 8            # bricks and flares settle this close to their target
JAM_REACH = 14            # a jammer parks this close to its node
ECHO_HITS = 3
STORM_SPLIT = 3
SPLIT_STUN = 30           # frames a storm's chirps hang dazed after the split
JAM_PARK = 240            # frames a jammer jams before its battery dies
SMASH_TIME = 300          # frames a brick smashes before it crumbles
FLARE_TIME = 600          # frames a flare burns once it reaches its target
LOST_TIME = 300           # frames a noise with no node to go for lasts
ZAP_EVERY = 45            # frames between storm lightning strikes
ZAP_RANGE = 16

# Bad actors
BAD_RANGE = 64            # pixels: how far a bad actor reaches, and its clue zone
BAD_SEND = 600            # frames between its signals at wave 1
BAD_SEND_MIN = 300
BAD_RING = 6              # pixels either side of a hearing node's distance ring
BAD_HEARD = 3             # nodes that must hear it to pin it down
ACTOR_CAP = (2, 3, 4)     # bad actors per game, per mode


def bad_cred(wave):
    # Cred for taking a bad actor off the air.
    return 30 + 3 * wave


# Waves: (noise kind, count, frames between spawns, delay, side). Side is
# N, S, E, W or A (any side, picked per spawn).
WAVES = (
    ((E_STATIC, 6, 50, 0, "W"),),
    ((E_STATIC, 8, 40, 0, "E"),),
    ((E_STATIC, 6, 40, 0, "N"), (E_CHIRP, 6, 30, 150, "W")),
    ((E_STATIC, 8, 36, 0, "S"), (E_BRICK, 2, 120, 200, "W")),
    ((E_CHIRP, 10, 20, 0, "E"), (E_JAMMER, 1, 1, 200, "N"), (E_STATIC, 6, 36, 250, "W")),
    ((E_ECHO, 8, 40, 0, "S"), (E_STATIC, 8, 30, 120, "N")),
    ((E_STORM, 3, 120, 0, "N"), (E_STATIC, 10, 30, 60, "S")),
    ((E_BRICK, 5, 70, 0, "E"), (E_JAMMER, 2, 150, 150, "W")),
    ((E_CHIRP, 14, 16, 0, "N"), (E_ECHO, 6, 40, 100, "S"), (E_STORM, 2, 150, 200, "E")),
    ((E_FLARE, 1, 1, 0, "N"), (E_STATIC, 12, 30, 60, "S"), (E_CHIRP, 8, 20, 200, "W")),
    ((E_JAMMER, 4, 90, 0, "E"), (E_BRICK, 6, 60, 60, "W")),
    ((E_STORM, 6, 70, 0, "S"), (E_ECHO, 10, 30, 100, "N")),
    ((E_BRICK, 8, 50, 0, "N"), (E_JAMMER, 4, 90, 100, "S"), (E_STATIC, 12, 20, 200, "E")),
    ((E_STATIC, 16, 20, 0, "W"), (E_STATIC, 16, 20, 0, "E"), (E_STORM, 4, 90, 200, "N")),
    ((E_FLARE, 1, 1, 0, "S"), (E_JAMMER, 5, 70, 60, "W"), (E_BRICK, 6, 50, 120, "E")),
    ((E_CHIRP, 16, 12, 0, "N"), (E_CHIRP, 16, 12, 0, "S"), (E_STORM, 5, 60, 200, "W")),
    ((E_BRICK, 12, 40, 0, "W"), (E_JAMMER, 6, 60, 80, "E"), (E_ECHO, 10, 30, 150, "N")),
    ((E_STORM, 10, 45, 0, "E"), (E_STATIC, 24, 16, 100, "S"), (E_ECHO, 12, 25, 200, "W")),
    ((E_JAMMER, 8, 50, 0, "N"), (E_BRICK, 12, 40, 60, "S"), (E_CHIRP, 18, 12, 120, "E")),
    ((E_FLARE, 1, 1, 0, "N"), (E_FLARE, 1, 1, 300, "S"), (E_STORM, 8, 50, 60, "E"),
     (E_JAMMER, 6, 60, 120, "W"), (E_BRICK, 10, 40, 200, "W")),
)
FIRST_COUNTDOWN = 900     # frames before wave 1
COUNTDOWN = 600           # frames between waves


# Modes: (name, noise HP multiplier, extra starting money)
MODES = (("EASY", 0.7, 40), ("NORMAL", 1.0, 0), ("HARD", 1.35, 0))
NORMAL = 1


def wave_groups(wave):
    if wave <= len(WAVES):
        return WAVES[wave - 1]
    # Overtime: the last waves again, growing.
    base = WAVES[len(WAVES) - 4 + (wave - len(WAVES) - 1) % 4]
    extra = (wave - len(WAVES) + 3) // 4
    return tuple((k, n + n * extra // 2, max(6, gap - 2 * extra), d, s)
                 for (k, n, gap, d, s) in base)


def wave_kinds(wave):
    # The noise kinds of a wave, in order of first appearance.
    out = []
    for g in wave_groups(wave):
        if g[0] not in out:
            out.append(g[0])
    return out


def wave_sides(wave):
    out = []
    for g in wave_groups(wave):
        if g[4] not in out:
            out.append(g[4])
    return out


def wave_hp_mult(wave):
    w = wave - 1
    return 1.0 + 0.12 * w + 0.004 * w * w


def wave_cred(wave):
    # Cred for clearing a wave.
    return 10 + 2 * wave
