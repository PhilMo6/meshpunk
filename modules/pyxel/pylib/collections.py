# collections for Pyxel games: MicroPython's C module (imported as
# ucollections) with a deque that, like CPython's, takes no arguments, an
# iterable, and maxlen by keyword (the C deque needs both arguments and has no
# rotate()).

from ucollections import OrderedDict, namedtuple


class deque:
    # Items are self._a[self._h:]; popleft() advances self._h and the list is
    # trimmed once the unused front outgrows the rest.
    def __init__(self, iterable=(), maxlen=None):
        if maxlen is not None and maxlen < 0:
            raise ValueError("maxlen must be non-negative")
        self._a = []
        self._h = 0
        self._maxlen = maxlen
        for x in iterable:
            self.append(x)

    maxlen = property(lambda self: self._maxlen)

    def _items(self):
        return self._a[self._h:]

    def _set(self, items):
        self._a = list(items)
        self._h = 0

    def __len__(self):
        return len(self._a) - self._h

    def __bool__(self):
        return len(self._a) > self._h

    def __iter__(self):
        return iter(self._items())

    def __reversed__(self):
        return iter(self._items()[::-1])

    def __contains__(self, x):
        return x in self._items()

    def _index(self, i):
        n = len(self)
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError("deque index out of range")
        return self._h + i

    def __getitem__(self, i):
        return self._a[self._index(i)]

    def __setitem__(self, i, x):
        self._a[self._index(i)] = x

    def __delitem__(self, i):
        del self._a[self._index(i)]

    def __eq__(self, other):
        return isinstance(other, deque) and self._items() == other._items()

    def __repr__(self):
        if self._maxlen is None:
            return "deque(%r)" % (self._items(),)
        return "deque(%r, maxlen=%d)" % (self._items(), self._maxlen)

    def __add__(self, other):
        d = deque(self._items(), self._maxlen)
        d.extend(other)
        return d

    def __iadd__(self, other):
        self.extend(other)
        return self

    def __copy__(self):
        return deque(self._items(), self._maxlen)

    def copy(self):
        return self.__copy__()

    def append(self, x):
        if self._maxlen == 0:
            return
        self._a.append(x)
        if self._maxlen is not None and len(self) > self._maxlen:
            self.popleft()

    def appendleft(self, x):
        if self._maxlen == 0:
            return
        if self._h:
            self._h -= 1
            self._a[self._h] = x
        else:
            self._a.insert(0, x)
        if self._maxlen is not None and len(self) > self._maxlen:
            self.pop()

    def pop(self):
        if not self:
            raise IndexError("pop from an empty deque")
        x = self._a.pop()
        if len(self._a) == self._h:
            self._a = []
            self._h = 0
        return x

    def popleft(self):
        if not self:
            raise IndexError("pop from an empty deque")
        x = self._a[self._h]
        self._a[self._h] = None
        self._h += 1
        if self._h > 32 and self._h * 2 > len(self._a):
            self._a = self._a[self._h:]
            self._h = 0
        return x

    def extend(self, iterable):
        for x in iterable:
            self.append(x)

    def extendleft(self, iterable):
        for x in iterable:
            self.appendleft(x)

    def clear(self):
        self._a = []
        self._h = 0

    def rotate(self, n=1):
        items = self._items()
        if items:
            n %= len(items)
            if n:
                self._set(items[-n:] + items[:-n])

    def count(self, x):
        return self._items().count(x)

    def index(self, x, start=0, stop=None):
        items = self._items()
        return items.index(x, start, len(items) if stop is None else stop)

    def insert(self, i, x):
        if self._maxlen is not None and len(self) >= self._maxlen:
            raise IndexError("deque already at its maximum size")
        items = self._items()
        items.insert(i, x)
        self._set(items)

    def remove(self, x):
        items = self._items()
        items.remove(x)
        self._set(items)

    def reverse(self):
        self._set(self._items()[::-1])
