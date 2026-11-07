# copy for Pyxel games: copy() and deepcopy() of the builtin containers and of
# instances (their attributes), with __copy__/__deepcopy__ hooks honoured.


class Error(Exception):
    pass


error = Error

_ATOMIC = (type(None), int, float, bool, complex, str, bytes, type, range, type(len), type(lambda: 0))


def _new_like(x):
    return object.__new__(type(x))


def copy(x):
    cls = type(x)
    hook = getattr(x, "__copy__", None)
    if hook is not None:
        return hook()
    if cls in _ATOMIC or cls is tuple or cls is frozenset:
        return x
    if cls is list:
        return x[:]
    if cls is dict:
        return dict(x)
    if cls is set:
        return set(x)
    if cls is bytearray:
        return bytearray(x)
    if isinstance(x, tuple):
        return x
    if isinstance(x, (list, dict, set)):
        return cls(x)
    y = _new_like(x)
    for k, v in x.__dict__.items():
        setattr(y, k, v)
    return y


def deepcopy(x, memo=None):
    if memo is None:
        memo = {}
    key = id(x)
    if key in memo:
        return memo[key]
    cls = type(x)
    if cls in _ATOMIC:
        return x
    hook = getattr(x, "__deepcopy__", None)
    if hook is not None:
        y = hook(memo)
    elif cls is list:
        y = []
        memo[key] = y
        for v in x:
            y.append(deepcopy(v, memo))
    elif cls is dict:
        y = {}
        memo[key] = y
        for k, v in x.items():
            y[deepcopy(k, memo)] = deepcopy(v, memo)
    elif cls is tuple:
        items = [deepcopy(v, memo) for v in x]
        y = x if all(a is b for a, b in zip(items, x)) else tuple(items)
    elif cls is set:
        y = set(deepcopy(v, memo) for v in x)
    elif cls is frozenset:
        y = frozenset(deepcopy(v, memo) for v in x)
    elif cls is bytearray:
        y = bytearray(x)
    elif isinstance(x, tuple):
        y = cls(*[deepcopy(v, memo) for v in x])
    elif isinstance(x, list):
        y = cls([deepcopy(v, memo) for v in x])
    elif isinstance(x, dict):
        y = cls((deepcopy(k, memo), deepcopy(v, memo)) for k, v in x.items())
    else:
        y = _new_like(x)
        memo[key] = y
        for k, v in x.__dict__.items():
            setattr(y, k, deepcopy(v, memo))
    memo[key] = y
    return y
