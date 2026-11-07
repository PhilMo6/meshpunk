# os for Pyxel games: paths, the working directory and the environment.
# listdir() works inside a .pyxapp only (the firmware exports no directory
# listing). mkdir()/makedirs() record the directory for this run and the first
# file written into it creates it on the SD card. Removing and renaming files
# are not supported (the firmware exports no SD-locked call for them).

import _pyxel as _p

from . import path

name = "posix"
sep = "/"
altsep = None
extsep = "."
pathsep = ":"
linesep = "\n"
curdir = "."
pardir = ".."
devnull = "/dev/null"
environ = {}

_made = set()


def getenv(key, default=None):
    return environ.get(key, default)


def getcwd():
    return _p.getcwd()


def chdir(p):
    _p.chdir(p)


def listdir(p="."):
    return _p.listdir(p)


def mkdir(p, mode=0o777):
    a = path.abspath(p)
    if path.exists(a):
        raise FileExistsError(17, "File exists: '%s'" % p)
    _made.add(a)


def makedirs(p, mode=0o777, exist_ok=False):
    a = path.abspath(p)
    if path.exists(a) and not exist_ok:
        raise FileExistsError(17, "File exists: '%s'" % p)
    while a not in ("/", ""):
        _made.add(a)
        a = path.dirname(a)


def remove(p):
    raise OSError(1, "removing files is not supported: '%s'" % p)


unlink = remove


def rmdir(p):
    raise OSError(1, "removing directories is not supported: '%s'" % p)


def rename(src, dst):
    raise OSError(1, "renaming files is not supported: '%s'" % src)


replace = rename


# No shell: the exit status a shell gives for a command it cannot find.
def system(command):
    return 127


def getpid():
    return 1


def urandom(n):
    import random
    return bytes(random.getrandbits(8) for _ in range(n))
