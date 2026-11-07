# random subset for MicroPython (xorshift32). Same function names and ranges
# as CPython's random; the number sequence differs from CPython.

_state = 2463534242


def seed(a=None):
    global _state
    if a is None:
        a = 2463534242
    _state = (hash(a) & 0xFFFFFFFF) or 2463534242


def getrandbits(k):
    global _state
    x = _state
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    _state = x
    return x >> (32 - k) if k < 32 else x


def random():
    return getrandbits(24) / 16777216.0


def _below(n):
    # uniform int in [0, n) by rejection, for n up to 2**32
    if n <= 0:
        raise ValueError("empty range")
    k = 1
    while (1 << k) < n:
        k += 1
    r = getrandbits(k)
    while r >= n:
        r = getrandbits(k)
    return r


def randrange(start, stop=None, step=1):
    if stop is None:
        start, stop = 0, start
    n = (stop - start + step - (1 if step > 0 else -1)) // step
    return start + step * _below(n)


def randint(a, b):
    return a + _below(b - a + 1)


def uniform(a, b):
    return a + (b - a) * random()


def choice(seq):
    if not seq:
        raise IndexError("Cannot choose from an empty sequence")
    return seq[_below(len(seq))]


def shuffle(x):
    for i in range(len(x) - 1, 0, -1):
        j = _below(i + 1)
        x[i], x[j] = x[j], x[i]


def sample(population, k):
    pool = list(population)
    n = len(pool)
    if k < 0 or k > n:
        raise ValueError("Sample larger than population or is negative")
    result = []
    for i in range(k):
        j = _below(n - i)
        result.append(pool[j])
        pool[j] = pool[n - i - 1]
    return result
