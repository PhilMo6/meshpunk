# Mesh Siege: controls. Every action answers to a gamepad button and to keys.
import pyxel

_LEFT = (pyxel.KEY_LEFT, pyxel.KEY_A, pyxel.GAMEPAD1_BUTTON_DPAD_LEFT)
_RIGHT = (pyxel.KEY_RIGHT, pyxel.KEY_D, pyxel.GAMEPAD1_BUTTON_DPAD_RIGHT)
_UP = (pyxel.KEY_UP, pyxel.KEY_W, pyxel.GAMEPAD1_BUTTON_DPAD_UP)
_DOWN = (pyxel.KEY_DOWN, pyxel.KEY_S, pyxel.GAMEPAD1_BUTTON_DPAD_DOWN)
_A = (pyxel.KEY_M, pyxel.KEY_Z, pyxel.KEY_SPACE, pyxel.GAMEPAD1_BUTTON_A)
_B = (pyxel.KEY_N, pyxel.KEY_X, pyxel.KEY_BACKSPACE, pyxel.GAMEPAD1_BUTTON_B,
      pyxel.GAMEPAD1_BUTTON_BACK)
_START = (pyxel.KEY_RETURN, pyxel.GAMEPAD1_BUTTON_START)
_Y = (pyxel.KEY_V, pyxel.GAMEPAD1_BUTTON_Y)

HOLD = 9
REPEAT = 3


def _pressed(keys, hold=0, repeat=0):
    for k in keys:
        if pyxel.btnp(k, hold, repeat):
            return True
    return False


def left():
    return _pressed(_LEFT, HOLD, REPEAT)


def right():
    return _pressed(_RIGHT, HOLD, REPEAT)


def up():
    return _pressed(_UP, HOLD, REPEAT)


def down():
    return _pressed(_DOWN, HOLD, REPEAT)


def a():
    return _pressed(_A)


def b():
    return _pressed(_B)


def start():
    return _pressed(_START)


def y():
    return _pressed(_Y)


def any_key():
    return a() or b() or start()
