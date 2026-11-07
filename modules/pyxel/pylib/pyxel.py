# pyxel API for the Meshpunk Pyxel player (MicroPython).
# Drawing, input, resources and the frame loop are native (_pyxel, src/api.c);
# this module adds the constants, the resource classes and the bank objects.
# API reference: https://github.com/kitao/pyxel/blob/main/docs/api-reference.md
#
# Key codes match upstream below 0x80; upstream's 0x4000_00nn keys are
# 0x10000 + nn here and its 0x5000_0000 + n keys are 0x20000 + n.

import io

import _pyxel as _p

VERSION = "2.9.9"

# ---------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------
KEY_UNKNOWN = 0x0
KEY_BACKSPACE = 0x8
KEY_TAB = 0x9
KEY_RETURN = 0xD
KEY_ESCAPE = 0x1B
KEY_SPACE = 0x20
KEY_EXCLAIM = 0x21
KEY_QUOTEDBL = 0x22
KEY_HASH = 0x23
KEY_DOLLAR = 0x24
KEY_PERCENT = 0x25
KEY_AMPERSAND = 0x26
KEY_QUOTE = 0x27
KEY_LEFTPAREN = 0x28
KEY_RIGHTPAREN = 0x29
KEY_ASTERISK = 0x2A
KEY_PLUS = 0x2B
KEY_COMMA = 0x2C
KEY_MINUS = 0x2D
KEY_PERIOD = 0x2E
KEY_SLASH = 0x2F
KEY_0 = 0x30
KEY_1 = 0x31
KEY_2 = 0x32
KEY_3 = 0x33
KEY_4 = 0x34
KEY_5 = 0x35
KEY_6 = 0x36
KEY_7 = 0x37
KEY_8 = 0x38
KEY_9 = 0x39
KEY_COLON = 0x3A
KEY_SEMICOLON = 0x3B
KEY_LESS = 0x3C
KEY_EQUALS = 0x3D
KEY_GREATER = 0x3E
KEY_QUESTION = 0x3F
KEY_AT = 0x40
KEY_LEFTBRACKET = 0x5B
KEY_BACKSLASH = 0x5C
KEY_RIGHTBRACKET = 0x5D
KEY_CARET = 0x5E
KEY_UNDERSCORE = 0x5F
KEY_BACKQUOTE = 0x60
KEY_A = 0x61
KEY_B = 0x62
KEY_C = 0x63
KEY_D = 0x64
KEY_E = 0x65
KEY_F = 0x66
KEY_G = 0x67
KEY_H = 0x68
KEY_I = 0x69
KEY_J = 0x6A
KEY_K = 0x6B
KEY_L = 0x6C
KEY_M = 0x6D
KEY_N = 0x6E
KEY_O = 0x6F
KEY_P = 0x70
KEY_Q = 0x71
KEY_R = 0x72
KEY_S = 0x73
KEY_T = 0x74
KEY_U = 0x75
KEY_V = 0x76
KEY_W = 0x77
KEY_X = 0x78
KEY_Y = 0x79
KEY_Z = 0x7A
KEY_DELETE = 0x7F
KEY_CAPSLOCK = 0x10039
KEY_F1 = 0x1003A
KEY_F2 = 0x1003B
KEY_F3 = 0x1003C
KEY_F4 = 0x1003D
KEY_F5 = 0x1003E
KEY_F6 = 0x1003F
KEY_F7 = 0x10040
KEY_F8 = 0x10041
KEY_F9 = 0x10042
KEY_F10 = 0x10043
KEY_F11 = 0x10044
KEY_F12 = 0x10045
KEY_PRINTSCREEN = 0x10046
KEY_SCROLLLOCK = 0x10047
KEY_PAUSE = 0x10048
KEY_INSERT = 0x10049
KEY_HOME = 0x1004A
KEY_PAGEUP = 0x1004B
KEY_END = 0x1004D
KEY_PAGEDOWN = 0x1004E
KEY_RIGHT = 0x1004F
KEY_LEFT = 0x10050
KEY_DOWN = 0x10051
KEY_UP = 0x10052
KEY_NUMLOCKCLEAR = 0x10053
KEY_KP_DIVIDE = 0x10054
KEY_KP_MULTIPLY = 0x10055
KEY_KP_MINUS = 0x10056
KEY_KP_PLUS = 0x10057
KEY_KP_ENTER = 0x10058
KEY_KP_1 = 0x10059
KEY_KP_2 = 0x1005A
KEY_KP_3 = 0x1005B
KEY_KP_4 = 0x1005C
KEY_KP_5 = 0x1005D
KEY_KP_6 = 0x1005E
KEY_KP_7 = 0x1005F
KEY_KP_8 = 0x10060
KEY_KP_9 = 0x10061
KEY_KP_0 = 0x10062
KEY_KP_PERIOD = 0x10063
KEY_APPLICATION = 0x10065
KEY_POWER = 0x10066
KEY_KP_EQUALS = 0x10067
KEY_F13 = 0x10068
KEY_F14 = 0x10069
KEY_F15 = 0x1006A
KEY_F16 = 0x1006B
KEY_F17 = 0x1006C
KEY_F18 = 0x1006D
KEY_F19 = 0x1006E
KEY_F20 = 0x1006F
KEY_F21 = 0x10070
KEY_F22 = 0x10071
KEY_F23 = 0x10072
KEY_F24 = 0x10073
KEY_EXECUTE = 0x10074
KEY_HELP = 0x10075
KEY_MENU = 0x10076
KEY_SELECT = 0x10077
KEY_STOP = 0x10078
KEY_AGAIN = 0x10079
KEY_UNDO = 0x1007A
KEY_CUT = 0x1007B
KEY_COPY = 0x1007C
KEY_PASTE = 0x1007D
KEY_FIND = 0x1007E
KEY_MUTE = 0x1007F
KEY_VOLUMEUP = 0x10080
KEY_VOLUMEDOWN = 0x10081
KEY_KP_COMMA = 0x10085
KEY_KP_EQUALSAS400 = 0x10086
KEY_ALTERASE = 0x10099
KEY_SYSREQ = 0x1009A
KEY_CANCEL = 0x1009B
KEY_CLEAR = 0x1009C
KEY_PRIOR = 0x1009D
KEY_RETURN2 = 0x1009E
KEY_SEPARATOR = 0x1009F
KEY_OUT = 0x100A0
KEY_OPER = 0x100A1
KEY_CLEARAGAIN = 0x100A2
KEY_CRSEL = 0x100A3
KEY_EXSEL = 0x100A4
KEY_KP_00 = 0x100B0
KEY_KP_000 = 0x100B1
KEY_THOUSANDSSEPARATOR = 0x100B2
KEY_DECIMALSEPARATOR = 0x100B3
KEY_CURRENCYUNIT = 0x100B4
KEY_CURRENCYSUBUNIT = 0x100B5
KEY_KP_LEFTPAREN = 0x100B6
KEY_KP_RIGHTPAREN = 0x100B7
KEY_KP_LEFTBRACE = 0x100B8
KEY_KP_RIGHTBRACE = 0x100B9
KEY_KP_TAB = 0x100BA
KEY_KP_BACKSPACE = 0x100BB
KEY_KP_A = 0x100BC
KEY_KP_B = 0x100BD
KEY_KP_C = 0x100BE
KEY_KP_D = 0x100BF
KEY_KP_E = 0x100C0
KEY_KP_F = 0x100C1
KEY_KP_XOR = 0x100C2
KEY_KP_POWER = 0x100C3
KEY_KP_PERCENT = 0x100C4
KEY_KP_LESS = 0x100C5
KEY_KP_GREATER = 0x100C6
KEY_KP_AMPERSAND = 0x100C7
KEY_KP_DBLAMPERSAND = 0x100C8
KEY_KP_VERTICALBAR = 0x100C9
KEY_KP_DBLVERTICALBAR = 0x100CA
KEY_KP_COLON = 0x100CB
KEY_KP_HASH = 0x100CC
KEY_KP_SPACE = 0x100CD
KEY_KP_AT = 0x100CE
KEY_KP_EXCLAM = 0x100CF
KEY_KP_MEMSTORE = 0x100D0
KEY_KP_MEMRECALL = 0x100D1
KEY_KP_MEMCLEAR = 0x100D2
KEY_KP_MEMADD = 0x100D3
KEY_KP_MEMSUBTRACT = 0x100D4
KEY_KP_MEMMULTIPLY = 0x100D5
KEY_KP_MEMDIVIDE = 0x100D6
KEY_KP_PLUSMINUS = 0x100D7
KEY_KP_CLEAR = 0x100D8
KEY_KP_CLEARENTRY = 0x100D9
KEY_KP_BINARY = 0x100DA
KEY_KP_OCTAL = 0x100DB
KEY_KP_DECIMAL = 0x100DC
KEY_KP_HEXADECIMAL = 0x100DD
KEY_LCTRL = 0x100E0
KEY_LSHIFT = 0x100E1
KEY_LALT = 0x100E2
KEY_LGUI = 0x100E3
KEY_RCTRL = 0x100E4
KEY_RSHIFT = 0x100E5
KEY_RALT = 0x100E6
KEY_RGUI = 0x100E7
KEY_NONE = 0x20000
KEY_SHIFT = 0x20001
KEY_CTRL = 0x20002
KEY_ALT = 0x20003
KEY_GUI = 0x20004

MOUSE_POS_X = 0x20100
MOUSE_POS_Y = 0x20101
MOUSE_WHEEL_X = 0x20102
MOUSE_WHEEL_Y = 0x20103
MOUSE_BUTTON_LEFT = 0x20104
MOUSE_BUTTON_MIDDLE = 0x20105
MOUSE_BUTTON_RIGHT = 0x20106
MOUSE_BUTTON_X1 = 0x20107
MOUSE_BUTTON_X2 = 0x20108

# Module-level names must be plain assignments: a key stored through globals()
# is not interned, and one such key makes every lookup in this module's dict
# compare strings (py/map.c all_keys_are_qstrs).
GAMEPAD1_AXIS_LEFTX = 0x20200
GAMEPAD1_AXIS_LEFTY = 0x20201
GAMEPAD1_AXIS_RIGHTX = 0x20202
GAMEPAD1_AXIS_RIGHTY = 0x20203
GAMEPAD1_AXIS_TRIGGERLEFT = 0x20204
GAMEPAD1_AXIS_TRIGGERRIGHT = 0x20205
GAMEPAD1_BUTTON_A = 0x20206
GAMEPAD1_BUTTON_B = 0x20207
GAMEPAD1_BUTTON_X = 0x20208
GAMEPAD1_BUTTON_Y = 0x20209
GAMEPAD1_BUTTON_BACK = 0x2020A
GAMEPAD1_BUTTON_GUIDE = 0x2020B
GAMEPAD1_BUTTON_START = 0x2020C
GAMEPAD1_BUTTON_LEFTSTICK = 0x2020D
GAMEPAD1_BUTTON_RIGHTSTICK = 0x2020E
GAMEPAD1_BUTTON_LEFTSHOULDER = 0x2020F
GAMEPAD1_BUTTON_RIGHTSHOULDER = 0x20210
GAMEPAD1_BUTTON_DPAD_UP = 0x20211
GAMEPAD1_BUTTON_DPAD_DOWN = 0x20212
GAMEPAD1_BUTTON_DPAD_LEFT = 0x20213
GAMEPAD1_BUTTON_DPAD_RIGHT = 0x20214

GAMEPAD2_AXIS_LEFTX = 0x20300
GAMEPAD2_AXIS_LEFTY = 0x20301
GAMEPAD2_AXIS_RIGHTX = 0x20302
GAMEPAD2_AXIS_RIGHTY = 0x20303
GAMEPAD2_AXIS_TRIGGERLEFT = 0x20304
GAMEPAD2_AXIS_TRIGGERRIGHT = 0x20305
GAMEPAD2_BUTTON_A = 0x20306
GAMEPAD2_BUTTON_B = 0x20307
GAMEPAD2_BUTTON_X = 0x20308
GAMEPAD2_BUTTON_Y = 0x20309
GAMEPAD2_BUTTON_BACK = 0x2030A
GAMEPAD2_BUTTON_GUIDE = 0x2030B
GAMEPAD2_BUTTON_START = 0x2030C
GAMEPAD2_BUTTON_LEFTSTICK = 0x2030D
GAMEPAD2_BUTTON_RIGHTSTICK = 0x2030E
GAMEPAD2_BUTTON_LEFTSHOULDER = 0x2030F
GAMEPAD2_BUTTON_RIGHTSHOULDER = 0x20310
GAMEPAD2_BUTTON_DPAD_UP = 0x20311
GAMEPAD2_BUTTON_DPAD_DOWN = 0x20312
GAMEPAD2_BUTTON_DPAD_LEFT = 0x20313
GAMEPAD2_BUTTON_DPAD_RIGHT = 0x20314

GAMEPAD3_AXIS_LEFTX = 0x20400
GAMEPAD3_AXIS_LEFTY = 0x20401
GAMEPAD3_AXIS_RIGHTX = 0x20402
GAMEPAD3_AXIS_RIGHTY = 0x20403
GAMEPAD3_AXIS_TRIGGERLEFT = 0x20404
GAMEPAD3_AXIS_TRIGGERRIGHT = 0x20405
GAMEPAD3_BUTTON_A = 0x20406
GAMEPAD3_BUTTON_B = 0x20407
GAMEPAD3_BUTTON_X = 0x20408
GAMEPAD3_BUTTON_Y = 0x20409
GAMEPAD3_BUTTON_BACK = 0x2040A
GAMEPAD3_BUTTON_GUIDE = 0x2040B
GAMEPAD3_BUTTON_START = 0x2040C
GAMEPAD3_BUTTON_LEFTSTICK = 0x2040D
GAMEPAD3_BUTTON_RIGHTSTICK = 0x2040E
GAMEPAD3_BUTTON_LEFTSHOULDER = 0x2040F
GAMEPAD3_BUTTON_RIGHTSHOULDER = 0x20410
GAMEPAD3_BUTTON_DPAD_UP = 0x20411
GAMEPAD3_BUTTON_DPAD_DOWN = 0x20412
GAMEPAD3_BUTTON_DPAD_LEFT = 0x20413
GAMEPAD3_BUTTON_DPAD_RIGHT = 0x20414

GAMEPAD4_AXIS_LEFTX = 0x20500
GAMEPAD4_AXIS_LEFTY = 0x20501
GAMEPAD4_AXIS_RIGHTX = 0x20502
GAMEPAD4_AXIS_RIGHTY = 0x20503
GAMEPAD4_AXIS_TRIGGERLEFT = 0x20504
GAMEPAD4_AXIS_TRIGGERRIGHT = 0x20505
GAMEPAD4_BUTTON_A = 0x20506
GAMEPAD4_BUTTON_B = 0x20507
GAMEPAD4_BUTTON_X = 0x20508
GAMEPAD4_BUTTON_Y = 0x20509
GAMEPAD4_BUTTON_BACK = 0x2050A
GAMEPAD4_BUTTON_GUIDE = 0x2050B
GAMEPAD4_BUTTON_START = 0x2050C
GAMEPAD4_BUTTON_LEFTSTICK = 0x2050D
GAMEPAD4_BUTTON_RIGHTSTICK = 0x2050E
GAMEPAD4_BUTTON_LEFTSHOULDER = 0x2050F
GAMEPAD4_BUTTON_RIGHTSHOULDER = 0x20510
GAMEPAD4_BUTTON_DPAD_UP = 0x20511
GAMEPAD4_BUTTON_DPAD_DOWN = 0x20512
GAMEPAD4_BUTTON_DPAD_LEFT = 0x20513
GAMEPAD4_BUTTON_DPAD_RIGHT = 0x20514

# ---------------------------------------------------------------------------
# Graphics and audio constants
# ---------------------------------------------------------------------------
COLOR_BLACK = 0
COLOR_NAVY = 1
COLOR_PURPLE = 2
COLOR_GREEN = 3
COLOR_BROWN = 4
COLOR_DARK_BLUE = 5
COLOR_LIGHT_BLUE = 6
COLOR_WHITE = 7
COLOR_RED = 8
COLOR_ORANGE = 9
COLOR_YELLOW = 10
COLOR_LIME = 11
COLOR_CYAN = 12
COLOR_GRAY = 13
COLOR_PINK = 14
COLOR_PEACH = 15

NUM_COLORS = 16
NUM_IMAGES = 3
IMAGE_SIZE = 256
NUM_TILEMAPS = 8
TILEMAP_SIZE = 256
TILE_SIZE = 8
FONT_WIDTH = 4
FONT_HEIGHT = 6

NUM_CHANNELS = 4
NUM_TONES = 4
NUM_SOUNDS = 64
NUM_MUSICS = 8

TONE_TRIANGLE = 0
TONE_SQUARE = 1
TONE_PULSE = 2
TONE_NOISE = 3

EFFECT_NONE = 0
EFFECT_SLIDE = 1
EFFECT_VIBRATO = 2
EFFECT_FADEOUT = 3
EFFECT_HALF_FADEOUT = 4
EFFECT_QUARTER_FADEOUT = 5

DEFAULT_COLORS = (
    0x000000, 0x2B335F, 0x7E2072, 0x19959C, 0x8B4852, 0x395C98, 0xA9C1FF, 0xEEEEEE,
    0xD4186C, 0xD38441, 0xE9C35B, 0x70C6A9, 0x7696DE, 0xA3A3A3, 0xFF9798, 0xEDC7B0,
)


class _Colors(list):
    def from_list(self, lst):
        self[:] = list(lst)

    def to_list(self):
        return list(self)


colors = _Colors(DEFAULT_COLORS)

# ---------------------------------------------------------------------------
# Resource classes
# ---------------------------------------------------------------------------
class Image:
    # _h: bank handle (0-2 image banks, 3 screen, 4 cursor) or None for an
    # image created by the game, whose state lives in _st/_buf.
    def __init__(self, width, height, _h=None):
        self._h = _h
        if _h is None:
            self._st, self._buf = _p.image_alloc(width, height)

    @staticmethod
    def from_image(filename, include_colors=False):
        img = Image(0, 0, -1)
        img._h = None
        img._st, img._buf = _p.image_from_file(filename, include_colors)
        return img

    @property
    def width(self):
        return _p.i_size(self)[0]

    @property
    def height(self):
        return _p.i_size(self)[1]

    def set(self, x, y, data):
        _p.i_set(self, x, y, data)

    def load(self, x, y, filename, include_colors=False):
        _p.i_load(self, x, y, filename, include_colors)

    def save(self, filename, scale):
        raise NotImplementedError("Image.save is not supported")

    def data_ptr(self):
        raise NotImplementedError("Image.data_ptr is not supported")

    def clip(self, *args):
        _p.i_clip(self, *args)

    def camera(self, *args):
        _p.i_camera(self, *args)

    def pal(self, *args):
        _p.i_pal(self, *args)

    def dither(self, alpha):
        _p.i_dither(self, alpha)

    def cls(self, col):
        _p.i_cls(self, col)

    def pget(self, x, y):
        return _p.i_pget(self, x, y)

    def pset(self, x, y, col):
        _p.i_pset(self, x, y, col)

    def line(self, x1, y1, x2, y2, col):
        _p.i_line(self, x1, y1, x2, y2, col)

    def rect(self, x, y, w, h, col):
        _p.i_rect(self, x, y, w, h, col)

    def rectb(self, x, y, w, h, col):
        _p.i_rectb(self, x, y, w, h, col)

    def circ(self, x, y, r, col):
        _p.i_circ(self, x, y, r, col)

    def circb(self, x, y, r, col):
        _p.i_circb(self, x, y, r, col)

    def elli(self, x, y, w, h, col):
        _p.i_elli(self, x, y, w, h, col)

    def ellib(self, x, y, w, h, col):
        _p.i_ellib(self, x, y, w, h, col)

    def tri(self, x1, y1, x2, y2, x3, y3, col):
        _p.i_tri(self, x1, y1, x2, y2, x3, y3, col)

    def trib(self, x1, y1, x2, y2, x3, y3, col):
        _p.i_trib(self, x1, y1, x2, y2, x3, y3, col)

    def fill(self, x, y, col):
        _p.i_fill(self, x, y, col)

    def blt(self, x, y, img, u, v, w, h, colkey=None, rotate=None, scale=None):
        _p.i_blt(self, x, y, img, u, v, w, h, colkey, rotate, scale)

    def bltm(self, x, y, tm, u, v, w, h, colkey=None, rotate=None, scale=None):
        _p.i_bltm(self, x, y, tm, u, v, w, h, colkey, rotate, scale)

    def blt3d(self, x, y, w, h, img, pos, rot, fov=60, colkey=None):
        _p.i_blt3d(self, x, y, w, h, img, pos, rot, fov, colkey)

    def bltm3d(self, x, y, w, h, tm, pos, rot, fov=60, colkey=None):
        _p.i_bltm3d(self, x, y, w, h, tm, pos, rot, fov, colkey)

    def text(self, x, y, s, col, font=None):
        _p.i_text(self, x, y, s, col, font)


class Tilemap:
    def __init__(self, width, height, img, _h=None):
        self._h = _h
        self._imgsrc_obj = None
        if _h is None:
            self._st, self._buf = _p.tilemap_alloc(width, height, img if isinstance(img, int) else 0)
            if not isinstance(img, int):
                self.imgsrc = img

    @staticmethod
    def from_tmx(filename, layer):
        tm = Tilemap(0, 0, 0, -1)
        tm._h = None
        tm._st, tm._buf = _p.tilemap_from_tmx(filename, layer)
        return tm

    @property
    def width(self):
        return _p.t_size(self)[0]

    @property
    def height(self):
        return _p.t_size(self)[1]

    @property
    def imgsrc(self):
        n = _p.t_imgsrc(self, None)
        return n if n >= 0 else self._imgsrc_obj

    @imgsrc.setter
    def imgsrc(self, value):
        if isinstance(value, int):
            self._imgsrc_obj = None
            _p.t_imgsrc(self, value)
        elif value._h is not None and 0 <= value._h < NUM_IMAGES:
            self._imgsrc_obj = None
            _p.t_imgsrc(self, value._h)
        else:
            self._imgsrc_obj = value
            _p.t_imgsrc(self, -1)

    def set(self, x, y, data):
        _p.t_set(self, x, y, data)

    def load(self, x, y, filename, layer):
        _p.t_load(self, x, y, filename, layer)

    def data_ptr(self):
        raise NotImplementedError("Tilemap.data_ptr is not supported")

    def clip(self, *args):
        _p.t_clip(self, *args)

    def camera(self, *args):
        _p.t_camera(self, *args)

    def cls(self, tile):
        _p.t_cls(self, tile)

    def pget(self, x, y):
        return _p.t_pget(self, x, y)

    def pset(self, x, y, tile):
        _p.t_pset(self, x, y, tile)

    def line(self, x1, y1, x2, y2, tile):
        _p.t_line(self, x1, y1, x2, y2, tile)

    def rect(self, x, y, w, h, tile):
        _p.t_rect(self, x, y, w, h, tile)

    def rectb(self, x, y, w, h, tile):
        _p.t_rectb(self, x, y, w, h, tile)

    def circ(self, x, y, r, tile):
        _p.t_circ(self, x, y, r, tile)

    def circb(self, x, y, r, tile):
        _p.t_circb(self, x, y, r, tile)

    def elli(self, x, y, w, h, tile):
        _p.t_elli(self, x, y, w, h, tile)

    def ellib(self, x, y, w, h, tile):
        _p.t_ellib(self, x, y, w, h, tile)

    def tri(self, x1, y1, x2, y2, x3, y3, tile):
        _p.t_tri(self, x1, y1, x2, y2, x3, y3, tile)

    def trib(self, x1, y1, x2, y2, x3, y3, tile):
        _p.t_trib(self, x1, y1, x2, y2, x3, y3, tile)

    def fill(self, x, y, tile):
        _p.t_fill(self, x, y, tile)

    def blt(self, x, y, tm, u, v, w, h, tilekey=None, rotate=None, scale=None):
        _p.t_blt(self, x, y, tm, u, v, w, h, tilekey, rotate, scale)

    def collide(self, x, y, w, h, dx, dy, walls):
        return _p.t_collide(self, x, y, w, h, dx, dy, walls)


class Sound:
    def __init__(self):
        self.notes = []
        self.tones = []
        self.volumes = []
        self.effects = []
        self.speed = 30
        self._mml = None
        self._mml_old = False
        self._pcm = None

    def set(self, notes, tones, volumes, effects, speed):
        self.set_notes(notes)
        self.set_tones(tones)
        self.set_volumes(volumes)
        self.set_effects(effects)
        if speed <= 0:
            raise ValueError("speed must be greater than 0")
        self.speed = speed

    # The string forms are parsed in C (_pyxel.parse_*, sound.rs rules).
    def set_notes(self, notes):
        self.notes = _p.parse_notes(notes)

    def set_tones(self, tones):
        self.tones = _p.parse_tones(tones)

    def set_volumes(self, volumes):
        self.volumes = _p.parse_volumes(volumes)

    def set_effects(self, effects):
        self.effects = _p.parse_effects(effects)

    # MML mode: code is parsed now (errors raise here) and again at play time.
    def mml(self, code=None):
        self._set_mml(code, False)

    def old_mml(self, code=None):
        self._set_mml(code, True)

    def _set_mml(self, code, old):
        if code is not None:
            _p.mml_check(code, old)
            self._release_pcm()
        self._mml = code
        self._mml_old = old

    # PCM mode: a WAV file decoded to 22050 Hz mono.
    def pcm(self, filename=None):
        handle = None if filename is None else _p.pcm_load(filename)
        self._release_pcm()
        self._pcm = handle
        if handle is not None:
            self._mml = None

    def _release_pcm(self):
        if self._pcm is not None:
            _p.pcm_release(self._pcm)
            self._pcm = None

    def total_sec(self):
        if self._pcm is not None or self._mml is not None:
            return _p.sound_total_sec(self)
        return len(self.notes) * self.speed / 120.0

    def save(self, filename, sec, ffmpeg=False):
        raise NotImplementedError("Sound.save is not supported")


class Music:
    def __init__(self):
        self.seqs = []

    def set(self, *seqs):
        self.seqs = [list(seq) for seq in seqs]

    def save(self, filename, sec, ffmpeg=False):
        raise NotImplementedError("Music.save is not supported")


class Channel:
    # gain and detune apply from the next note, as upstream.
    def __init__(self, _index=None):
        self._index = _index
        self._gain = 0.125
        self._detune = 0

    def _sync(self):
        if self._index is not None:
            _p.channel_set(self._index, self._gain, self._detune)

    @property
    def gain(self):
        return self._gain

    @gain.setter
    def gain(self, value):
        self._gain = value
        self._sync()

    @property
    def detune(self):
        return self._detune

    @detune.setter
    def detune(self, value):
        self._detune = value
        self._sync()

    def play(self, snd, sec=None, loop=False, resume=False):
        play(self._index, snd, sec, loop, resume)

    def stop(self):
        _p.stop(self._index)

    def play_pos(self):
        return _p.play_pos(self._index)


class Tone:
    # mode: 0 wavetable, 1 short-period noise, 2 long-period noise
    def __init__(self, mode=0, sample_bits=4, wavetable=None, gain=1.0):
        self.mode = mode
        self.sample_bits = sample_bits
        self.wavetable = list(wavetable) if wavetable else []
        self.gain = gain


class Font:
    # BDF bitmap fonts; font_size applies to OTF/TTF only, which are not supported.
    def __init__(self, filename, font_size=10.0):
        if not filename.lower().endswith(".bdf"):
            raise NotImplementedError("only BDF fonts are supported, not '%s'" % filename)
        self._font = _p.font_load(filename)

    def text_width(self, s):
        return _p.font_text_width(self._font, s)


# ---------------------------------------------------------------------------
# Banks and runtime variables (frame_count, mouse_x, ... are updated by _pyxel)
# ---------------------------------------------------------------------------
_bank_images = [Image(IMAGE_SIZE, IMAGE_SIZE, i) for i in range(NUM_IMAGES)]
_bank_tilemaps = [Tilemap(TILEMAP_SIZE, TILEMAP_SIZE, 0, i) for i in range(NUM_TILEMAPS)]
images = list(_bank_images)
tilemaps = list(_bank_tilemaps)
screen = Image(0, 0, 3)
cursor = Image(8, 8, 4)
font = None
sounds = [Sound() for _ in range(NUM_SOUNDS)]
musics = [Music() for _ in range(NUM_MUSICS)]
channels = [Channel(i) for i in range(NUM_CHANNELS)]
tones = [
    Tone(0, 4, (8, 9, 10, 11, 12, 13, 14, 15, 15, 14, 13, 12, 11, 10, 9, 8,
                7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7), 1.0),
    Tone(0, 1, (1, 0), 0.3),
    Tone(0, 1, (1, 0, 0, 0), 0.3),
    Tone(2, 0, (), 0.6),
]

width = 0
height = 0
frame_count = 0
mouse_x = 0
mouse_y = 0
mouse_wheel = 0
input_keys = []
input_text = ""
dropped_files = []

# ---------------------------------------------------------------------------
# Native functions
# ---------------------------------------------------------------------------
init = _p.init
run = _p.run
show = _p.show
flip = _p.flip
quit = _p.quit
reset = _p.reset
title = _p.title
icon = _p.icon
fullscreen = _p.fullscreen
resize = _p.resize
screen_mode = _p.screen_mode
perf_monitor = _p.perf_monitor
integer_scale = _p.integer_scale

# load()/save(): a deprecated excl_* argument, when given, overrides its exclude_* one.
def _excludes(exclude, excl):
    return [e if x is None else x for e, x in zip(exclude, excl)]


def load(filename, exclude_images=False, exclude_tilemaps=False, exclude_sounds=False,
         exclude_musics=False, excl_images=None, excl_tilemaps=None, excl_sounds=None,
         excl_musics=None):
    ex_img, ex_tm, ex_snd, ex_mus = _excludes(
        (exclude_images, exclude_tilemaps, exclude_sounds, exclude_musics),
        (excl_images, excl_tilemaps, excl_sounds, excl_musics))
    _p.load(filename, ex_img, ex_tm, ex_snd, ex_mus)
    # The loaded data lives in the banks; as upstream, the lists name them again.
    if not ex_img:
        images[:] = _bank_images
    if not ex_tm:
        tilemaps[:] = _bank_tilemaps


def _toml_ints(values):
    return "[" + ", ".join(str(int(v)) for v in values) + "]"


# save(): the [[sounds]] / [[musics]] tables of pyxel_resource.toml. Musics
# drop trailing empty sequences (resource_data.rs MusicData::from_music).
def _sound_tables():
    return "".join(
        "\n[[sounds]]\nnotes = %s\ntones = %s\nvolumes = %s\neffects = %s\nspeed = %d\n"
        % (_toml_ints(s.notes), _toml_ints(s.tones), _toml_ints(s.volumes),
           _toml_ints(s.effects), int(s.speed))
        for s in sounds)


def _music_tables():
    out = []
    for m in musics:
        seqs = list(m.seqs)
        while seqs and not seqs[-1]:
            seqs.pop()
        out.append("\n[[musics]]\nseqs = [%s]\n" % ", ".join(_toml_ints(q) for q in seqs))
    return "".join(out)


def save(filename, exclude_images=False, exclude_tilemaps=False, exclude_sounds=False,
         exclude_musics=False, excl_images=None, excl_tilemaps=None, excl_sounds=None,
         excl_musics=None):
    ex_img, ex_tm, ex_snd, ex_mus = _excludes(
        (exclude_images, exclude_tilemaps, exclude_sounds, exclude_musics),
        (excl_images, excl_tilemaps, excl_sounds, excl_musics))
    _p.save(filename, [] if ex_img else images, [] if ex_tm else tilemaps,
            "" if ex_snd else _sound_tables(), "" if ex_mus else _music_tables())


load_pal = _p.load_pal


def save_pal(filename):
    if filename.lower().endswith(".pyxres"):
        filename = filename[:-7] + ".pyxpal"
    with open(filename, "w") as f:
        f.write("".join("%06x\n" % c for c in colors))


# Upstream: ~/.pyxel/<vendor>/<app>/, each name lowercased with spaces as "_"
# and only letters, digits, "_" and "-" kept. Here the kept characters are
# ASCII only and the directories are under /sd/pyxel/save/; they are created
# by the first file written into them.
_DIR_NAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_-"


def _dir_name(name):
    return "".join(c for c in name.lower().replace(" ", "_") if c in _DIR_NAME_CHARS)


def user_data_dir(vendor_name, app_name):
    return "/sd/pyxel/save/%s/%s/" % (_dir_name(vendor_name), _dir_name(app_name))


class _WFile(io.IOBase):
    # What open() returns for modes "w", "a" and "x". _pyxel holds the
    # contents (handle _h) and writes the whole file on flush() and close(),
    # and at exit for files left open.
    def __init__(self, h, name, mode):
        self._h = h
        self.name = name
        self.mode = mode

    @property
    def closed(self):
        return self._h is None

    def _check_open(self):
        if self._h is None:
            raise ValueError("I/O operation on closed file")

    def write(self, data):
        self._check_open()
        if isinstance(data, str) and "b" in self.mode:
            raise TypeError("a bytes-like object is required, not 'str'")
        _p.file_write(self._h, data)
        return len(data)

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def flush(self):
        self._check_open()
        _p.file_flush(self._h, self.name)

    def close(self):
        if self._h is not None:
            h = self._h
            self._h = None
            _p.file_close(h, self.name)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


screenshot = _p.screenshot
screencast = _p.screencast
reset_screencast = _p.reset_screencast

btn = _p.btn
btnp = _p.btnp
btnr = _p.btnr
btnv = _p.btnv
mouse = _p.mouse

cls = _p.cls
pget = _p.pget
pset = _p.pset
line = _p.line
rect = _p.rect
rectb = _p.rectb
circ = _p.circ
circb = _p.circb
elli = _p.elli
ellib = _p.ellib
tri = _p.tri
trib = _p.trib
fill = _p.fill
blt = _p.blt
bltm = _p.bltm
blt3d = _p.blt3d
bltm3d = _p.bltm3d
text = _p.text
clip = _p.clip
camera = _p.camera
pal = _p.pal
dither = _p.dither

def play(ch, snd, sec=None, loop=False, resume=False, tick=None):
    if tick is not None:
        sec = tick / 120.0
    if isinstance(snd, str):
        mml_sound = Sound()
        mml_sound.mml(snd)
        snd = mml_sound
    if isinstance(snd, (int, Sound)):
        snd = [snd]
    seq = [sounds[s] if isinstance(s, int) else s for s in snd]
    _p.play(ch, seq, sec, loop, resume)


def playm(msc, sec=None, loop=False, tick=None):
    if tick is not None:
        sec = tick / 120.0
    seqs = musics[msc].seqs
    for ch in range(min(len(seqs), NUM_CHANNELS)):
        if seqs[ch]:
            play(ch, seqs[ch], sec, loop)


def stop(ch=None):
    _p.stop(ch)


play_pos = _p.play_pos


def gen_bgm(preset, transp, instr, seed, play=False):
    raise NotImplementedError("gen_bgm is not supported")


ceil = _p.ceil
floor = _p.floor
clamp = _p.clamp
sgn = _p.sgn
sqrt = _p.sqrt
sin = _p.sin
cos = _p.cos
atan2 = _p.atan2
rseed = _p.rseed
rndi = _p.rndi
rndf = _p.rndf
nseed = _p.nseed
noise = _p.noise

_p._bind(globals())
