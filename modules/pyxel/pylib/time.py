# time for Pyxel games: the parts of CPython's time module games use.
#
# time(), monotonic() and perf_counter() count seconds from the start of the
# game: floats in this MicroPython are 30 bits, so an epoch value (about 1.8e9
# seconds) would only resolve to 17-minute steps. time_ns() and the calendar
# functions use the device clock the launcher passes (-clock), as integers.

import _pyxel as _p

_EPOCH, _TZMIN = _p.clock()
_TZSEC = _TZMIN * 60
timezone = -_TZSEC
altzone = timezone
daylight = 0


def _offset_str(sep):
    a = _TZMIN if _TZMIN >= 0 else -_TZMIN
    return "%s%02d%s%02d" % ("+" if _TZMIN >= 0 else "-", a // 60, sep, a % 60)


tzname = ("UTC" + _offset_str(":"), "UTC" + _offset_str(":"))

_DAY_NAMES = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
_MONTH_NAMES = ("January", "February", "March", "April", "May", "June", "July", "August",
                "September", "October", "November", "December")
_DAYS_BEFORE_MONTH = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)


def time():
    return _p.ticks_us() / 1000000


def time_ns():
    return _EPOCH * 1000000000 + _p.ticks_us() * 1000


def monotonic():
    return _p.ticks_us() / 1000000


def monotonic_ns():
    return _p.ticks_us() * 1000


perf_counter = monotonic
perf_counter_ns = monotonic_ns
process_time = monotonic
process_time_ns = monotonic_ns


def sleep(secs):
    _p.sleep(secs)


def _now():
    return _EPOCH + _p.ticks_us() // 1000000


def _is_leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


# Days since 1970-01-01 <-> (year, month, day), proleptic Gregorian
# (H. Hinnant, "chrono-Compatible Low-Level Date Algorithms").
def _civil_from_days(z):
    z += 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return (yoe + era * 400 + (1 if m <= 2 else 0), m, d)


def _days_from_civil(y, m, d):
    if m <= 2:
        y -= 1
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


class struct_time:
    def __init__(self, t):
        t = tuple(t)
        if len(t) < 9:
            raise TypeError("time.struct_time() takes a 9-sequence")
        self._t = t[:9]

    tm_year = property(lambda self: self._t[0])
    tm_mon = property(lambda self: self._t[1])
    tm_mday = property(lambda self: self._t[2])
    tm_hour = property(lambda self: self._t[3])
    tm_min = property(lambda self: self._t[4])
    tm_sec = property(lambda self: self._t[5])
    tm_wday = property(lambda self: self._t[6])
    tm_yday = property(lambda self: self._t[7])
    tm_isdst = property(lambda self: self._t[8])

    def __getitem__(self, i):
        return self._t[i]

    def __len__(self):
        return 9

    def __iter__(self):
        return iter(self._t)

    def __eq__(self, other):
        return tuple(self) == tuple(other)

    def __repr__(self):
        return ("time.struct_time(tm_year=%d, tm_mon=%d, tm_mday=%d, tm_hour=%d, tm_min=%d, "
                "tm_sec=%d, tm_wday=%d, tm_yday=%d, tm_isdst=%d)" % self._t)


def _struct(secs, offset):
    t = int(secs // 1) + offset
    days, rem = divmod(t, 86400)
    y, m, d = _civil_from_days(days)
    hh, rem = divmod(rem, 3600)
    mm, ss = divmod(rem, 60)
    yday = _DAYS_BEFORE_MONTH[m - 1] + d + (1 if m > 2 and _is_leap(y) else 0)
    return struct_time((y, m, d, hh, mm, ss, (days + 3) % 7, yday, 0))


def gmtime(secs=None):
    return _struct(_now() if secs is None else secs, 0)


def localtime(secs=None):
    return _struct(_now() if secs is None else secs, _TZSEC)


# Local struct -> epoch seconds, as an int (a float could not hold it exactly).
def mktime(t):
    return _days_from_civil(t[0], t[1], t[2]) * 86400 + t[3] * 3600 + t[4] * 60 + t[5] - _TZSEC


def strftime(fmt, t=None):
    if t is None:
        t = localtime()
    y, mo, d, hh, mi, ss, wd, yd = t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]
    out = []
    i = 0
    n = len(fmt)
    while i < n:
        c = fmt[i]
        if c != "%" or i + 1 == n:
            out.append(c)
            i += 1
            continue
        k = fmt[i + 1]
        i += 2
        if k == "Y":
            out.append("%d" % y)
        elif k == "y":
            out.append("%02d" % (y % 100))
        elif k == "m":
            out.append("%02d" % mo)
        elif k == "d":
            out.append("%02d" % d)
        elif k == "e":
            out.append("%2d" % d)
        elif k == "H":
            out.append("%02d" % hh)
        elif k == "I":
            out.append("%02d" % ((hh + 11) % 12 + 1))
        elif k == "M":
            out.append("%02d" % mi)
        elif k == "S":
            out.append("%02d" % ss)
        elif k == "p":
            out.append("AM" if hh < 12 else "PM")
        elif k == "j":
            out.append("%03d" % yd)
        elif k == "a":
            out.append(_DAY_NAMES[wd][:3])
        elif k == "A":
            out.append(_DAY_NAMES[wd])
        elif k == "b" or k == "h":
            out.append(_MONTH_NAMES[mo - 1][:3])
        elif k == "B":
            out.append(_MONTH_NAMES[mo - 1])
        elif k == "w":
            out.append("%d" % ((wd + 1) % 7))
        elif k == "u":
            out.append("%d" % (wd + 1))
        elif k == "c":
            out.append(strftime("%a %b %e %H:%M:%S %Y", t))
        elif k == "x":
            out.append(strftime("%m/%d/%y", t))
        elif k == "X":
            out.append(strftime("%H:%M:%S", t))
        elif k == "D":
            out.append(strftime("%m/%d/%y", t))
        elif k == "F":
            out.append(strftime("%Y-%m-%d", t))
        elif k == "T":
            out.append(strftime("%H:%M:%S", t))
        elif k == "R":
            out.append(strftime("%H:%M", t))
        elif k == "Z":
            out.append(tzname[0])
        elif k == "z":
            out.append(_offset_str(""))
        elif k == "%":
            out.append("%")
        else:
            out.append("%" + k)
    return "".join(out)


def asctime(t=None):
    return strftime("%a %b %e %H:%M:%S %Y", localtime() if t is None else t)


def ctime(secs=None):
    return asctime(localtime(secs))
