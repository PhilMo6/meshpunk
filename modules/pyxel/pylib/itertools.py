# itertools subset for MicroPython, written as generators.


def count(start=0, step=1):
    n = start
    while True:
        yield n
        n += step


def cycle(iterable):
    saved = []
    for x in iterable:
        yield x
        saved.append(x)
    while saved:
        for x in saved:
            yield x


def repeat(obj, times=None):
    if times is None:
        while True:
            yield obj
    for _ in range(times):
        yield obj


def _flatten(iterables):
    for it in iterables:
        for x in it:
            yield x


class chain:
    def __init__(self, *iterables):
        self._gen = _flatten(iterables)

    def __iter__(self):
        return self

    def __next__(self):
        return next(self._gen)

    @staticmethod
    def from_iterable(iterables):
        return _flatten(iterables)


def islice(iterable, *args):
    s = slice(*args)
    start = s.start or 0
    stop = s.stop
    step = s.step or 1
    nxt = start
    for i, x in enumerate(iterable):
        if stop is not None and i >= stop:
            return
        if i == nxt:
            yield x
            nxt += step


def filterfalse(predicate, iterable):
    if predicate is None:
        predicate = bool
    for x in iterable:
        if not predicate(x):
            yield x


def accumulate(iterable, func=None, initial=None):
    it = iter(iterable)
    total = initial
    if total is None:
        try:
            total = next(it)
        except StopIteration:
            return
    yield total
    for x in it:
        total = total + x if func is None else func(total, x)
        yield total


def product(*iterables, repeat=1):
    pools = [tuple(p) for p in iterables] * repeat
    result = [()]
    for pool in pools:
        result = [r + (y,) for r in result for y in pool]
    for r in result:
        yield r


def zip_longest(*iterables, fillvalue=None):
    its = [iter(it) for it in iterables]
    while True:
        row = []
        live = 0
        for it in its:
            try:
                row.append(next(it))
                live += 1
            except StopIteration:
                row.append(fillvalue)
        if not live:
            return
        yield tuple(row)
