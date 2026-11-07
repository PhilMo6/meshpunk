# json for Pyxel games. Parsing is MicroPython's C json (imported as ujson),
# with its ValueError raised as JSONDecodeError and object_hook applied
# afterwards. Encoding follows CPython's json.dumps: non-string dict keys
# become strings, and indent, separators, sort_keys, ensure_ascii, default and
# cls apply.

from ujson import loads as _loads


class JSONDecodeError(ValueError):
    def __init__(self, msg, doc="", pos=0):
        super().__init__(msg)
        self.msg = msg
        self.doc = doc
        self.pos = pos
        self.lineno = doc.count("\n", 0, pos) + 1
        self.colno = pos - doc.rfind("\n", 0, pos)


def _hook(obj, object_hook):
    if isinstance(obj, list):
        return [_hook(v, object_hook) for v in obj]
    if isinstance(obj, dict):
        return object_hook({k: _hook(v, object_hook) for k, v in obj.items()})
    return obj


def loads(s, object_hook=None, **kwargs):
    if isinstance(s, (bytes, bytearray)):
        s = s.decode()
    try:
        obj = _loads(s)
    except ValueError as e:
        raise JSONDecodeError(str(e), s, 0)
    return obj if object_hook is None else _hook(obj, object_hook)


def load(fp, **kwargs):
    return loads(fp.read(), **kwargs)


_ESCAPES = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t", "\b": "\\b", "\f": "\\f"}


def _encode_str(s, ensure_ascii):
    out = ['"']
    for ch in s:
        esc = _ESCAPES.get(ch)
        if esc is not None:
            out.append(esc)
            continue
        o = ord(ch)
        if o < 0x20:
            out.append("\\u%04x" % o)
        elif o > 0x7E and ensure_ascii:
            if o > 0xFFFF:
                o -= 0x10000
                out.append("\\u%04x\\u%04x" % (0xD800 | (o >> 10), 0xDC00 | (o & 0x3FF)))
            else:
                out.append("\\u%04x" % o)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


class JSONEncoder:
    item_separator = ", "
    key_separator = ": "

    def __init__(self, skipkeys=False, ensure_ascii=True, check_circular=True, allow_nan=True,
                 sort_keys=False, indent=None, separators=None, default=None):
        self.skipkeys = skipkeys
        self.ensure_ascii = ensure_ascii
        self.allow_nan = allow_nan
        self.sort_keys = sort_keys
        if isinstance(indent, int):
            indent = " " * indent
        self.indent = indent
        if separators is not None:
            self.item_separator, self.key_separator = separators
        elif indent is not None:
            self.item_separator = ","
        if default is not None:
            self.default = default

    def default(self, o):
        raise TypeError("Object of type %s is not JSON serializable" % type(o).__name__)

    def encode(self, o):
        out = []
        self._encode(o, out, 0)
        return "".join(out)

    def _float(self, f):
        if f != f:
            text = "NaN"
        elif f == float("inf"):
            text = "Infinity"
        elif f == -float("inf"):
            text = "-Infinity"
        else:
            return repr(f)
        if not self.allow_nan:
            raise ValueError("Out of range float values are not JSON compliant")
        return text

    def _key(self, k):
        if isinstance(k, str):
            return k
        if k is True:
            return "true"
        if k is False:
            return "false"
        if k is None:
            return "null"
        if isinstance(k, float):
            return self._float(k)
        if isinstance(k, int):
            return str(k)
        if self.skipkeys:
            return None
        raise TypeError("keys must be str, int, float, bool or None, not %s" % type(k).__name__)

    def _newline(self, out, level):
        if self.indent is not None:
            out.append("\n" + self.indent * level)

    def _encode(self, o, out, level):
        if o is None:
            out.append("null")
        elif o is True:
            out.append("true")
        elif o is False:
            out.append("false")
        elif isinstance(o, str):
            out.append(_encode_str(o, self.ensure_ascii))
        elif isinstance(o, float):
            out.append(self._float(o))
        elif isinstance(o, int):
            out.append(str(int(o)))
        elif isinstance(o, (list, tuple)):
            if not o:
                out.append("[]")
                return
            out.append("[")
            for i, v in enumerate(o):
                if i:
                    out.append(self.item_separator)
                self._newline(out, level + 1)
                self._encode(v, out, level + 1)
            self._newline(out, level)
            out.append("]")
        elif isinstance(o, dict):
            items = []
            for k, v in o.items():
                key = self._key(k)
                if key is not None:
                    items.append((key, v))
            if self.sort_keys:
                items.sort(key=lambda kv: kv[0])
            if not items:
                out.append("{}")
                return
            out.append("{")
            for i, (k, v) in enumerate(items):
                if i:
                    out.append(self.item_separator)
                self._newline(out, level + 1)
                out.append(_encode_str(k, self.ensure_ascii))
                out.append(self.key_separator)
                self._encode(v, out, level + 1)
            self._newline(out, level)
            out.append("}")
        else:
            self._encode(self.default(o), out, level)


def dumps(obj, skipkeys=False, ensure_ascii=True, check_circular=True, allow_nan=True, cls=None,
          indent=None, separators=None, default=None, sort_keys=False, **kwargs):
    return (cls or JSONEncoder)(skipkeys=skipkeys, ensure_ascii=ensure_ascii, check_circular=check_circular,
                                allow_nan=allow_nan, sort_keys=sort_keys, indent=indent,
                                separators=separators, default=default, **kwargs).encode(obj)


def dump(obj, fp, **kwargs):
    fp.write(dumps(obj, **kwargs))
