# os.path for Pyxel games (POSIX rules). "~" expands to /sd/pyxel/save, the
# folder pyxel.user_data_dir() uses, since there is no home directory.

import _pyxel as _p

sep = "/"
curdir = "."
pardir = ".."
extsep = "."
HOME = "/sd/pyxel/save"


def isabs(p):
    return p.startswith("/")


def join(a, *parts):
    out = a
    for b in parts:
        if b.startswith("/"):
            out = b
        elif not out or out.endswith("/"):
            out += b
        else:
            out += "/" + b
    return out


def split(p):
    i = p.rfind("/") + 1
    head, tail = p[:i], p[i:]
    if head and head != "/" * len(head):
        head = head.rstrip("/")
    return head, tail


def dirname(p):
    return split(p)[0]


def basename(p):
    return split(p)[1]


def splitext(p):
    i = p.rfind(".")
    s = p.rfind("/")
    if i <= s + 1 or i == -1:
        return p, ""
    b = s + 1
    while b < i and p[b] == ".":
        b += 1
    if b == i:
        return p, ""
    return p[:i], p[i:]


def normpath(p):
    if not p:
        return "."
    initial = "/" if p.startswith("/") else ""
    out = []
    for part in p.split("/"):
        if part in ("", "."):
            continue
        if part == ".." and (out and out[-1] != ".." or initial):
            if out:
                out.pop()
            continue
        out.append(part)
    return (initial + "/".join(out)) or "."


def abspath(p):
    return normpath(p if isabs(p) else join(_p.getcwd(), p))


realpath = abspath


def expanduser(p):
    if p == "~" or p.startswith("~/"):
        return HOME + p[1:]
    return p


def expandvars(p):
    return p


def _made(p):
    import os
    return abspath(p) in os._made


def exists(p):
    return _p.path_stat(p) != 0 or _made(p)


def isfile(p):
    return _p.path_stat(p) == 1


def isdir(p):
    return _p.path_stat(p) == 2 or _made(p)


def getsize(p):
    with open(p, "rb") as f:
        return len(f.read())


def commonprefix(paths):
    if not paths:
        return ""
    lo, hi = min(paths), max(paths)
    for i in range(len(lo)):
        if i >= len(hi) or lo[i] != hi[i]:
            return lo[:i]
    return lo
