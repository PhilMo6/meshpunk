# datetime for Pyxel games: date, time, datetime, timedelta and timezone on
# the device clock (see time.py). Arithmetic is on integer microseconds.
# Not provided: strptime, zoneinfo.

import time as _time
import _pyxel as _p

MINYEAR = 1
MAXYEAR = 9999
_US_PER_DAY = 86400000000
_DAYS_IN_MONTH = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
_ORDINAL_1970 = 719163          # date(1970, 1, 1).toordinal()


def _days_in_month(y, m):
    return 29 if m == 2 and _time._is_leap(y) else _DAYS_IN_MONTH[m - 1]


def _check_date(y, m, d):
    if not MINYEAR <= y <= MAXYEAR:
        raise ValueError("year %d is out of range" % y)
    if not 1 <= m <= 12:
        raise ValueError("month must be in 1..12")
    if not 1 <= d <= _days_in_month(y, m):
        raise ValueError("day is out of range for month")


def _offset_seconds(td):
    return td.days * 86400 + td.seconds


def _check_time(hh, mm, ss, us):
    if not 0 <= hh <= 23:
        raise ValueError("hour must be in 0..23")
    if not 0 <= mm <= 59:
        raise ValueError("minute must be in 0..59")
    if not 0 <= ss <= 59:
        raise ValueError("second must be in 0..59")
    if not 0 <= us <= 999999:
        raise ValueError("microsecond must be in 0..999999")


def _iso_time(hh, mm, ss, us, off):
    s = "%02d:%02d:%02d" % (hh, mm, ss)
    if us:
        s += ".%06d" % us
    if off is not None:
        a = -off if off < 0 else off
        s += "%s%02d:%02d" % ("-" if off < 0 else "+", a // 3600, a % 3600 // 60)
    return s


class timedelta:
    def __init__(self, days=0, seconds=0, microseconds=0, milliseconds=0, minutes=0, hours=0, weeks=0):
        us = ((((weeks * 7 + days) * 24 + hours) * 60 + minutes) * 60 + seconds) * 1000000
        us = int(round(us + milliseconds * 1000 + microseconds))
        d, us = divmod(us, _US_PER_DAY)
        s, us = divmod(us, 1000000)
        self._d = d
        self._s = s
        self._us = us

    days = property(lambda self: self._d)
    seconds = property(lambda self: self._s)
    microseconds = property(lambda self: self._us)

    def _total_us(self):
        return (self._d * 86400 + self._s) * 1000000 + self._us

    def total_seconds(self):
        return self._total_us() / 1000000

    def __add__(self, other):
        if isinstance(other, timedelta):
            return timedelta(microseconds=self._total_us() + other._total_us())
        return NotImplemented

    __radd__ = __add__

    def __sub__(self, other):
        if isinstance(other, timedelta):
            return timedelta(microseconds=self._total_us() - other._total_us())
        return NotImplemented

    def __neg__(self):
        return timedelta(microseconds=-self._total_us())

    def __pos__(self):
        return self

    def __abs__(self):
        return -self if self._d < 0 else self

    def __mul__(self, k):
        if isinstance(k, (int, float)):
            return timedelta(microseconds=self._total_us() * k)
        return NotImplemented

    __rmul__ = __mul__

    def __truediv__(self, other):
        if isinstance(other, timedelta):
            return self._total_us() / other._total_us()
        return timedelta(microseconds=self._total_us() / other)

    def __floordiv__(self, other):
        if isinstance(other, timedelta):
            return self._total_us() // other._total_us()
        return timedelta(microseconds=self._total_us() // other)

    def __mod__(self, other):
        return timedelta(microseconds=self._total_us() % other._total_us())

    def __eq__(self, other):
        return isinstance(other, timedelta) and self._total_us() == other._total_us()

    def __lt__(self, other):
        return self._total_us() < other._total_us()

    def __le__(self, other):
        return self._total_us() <= other._total_us()

    def __gt__(self, other):
        return self._total_us() > other._total_us()

    def __ge__(self, other):
        return self._total_us() >= other._total_us()

    def __hash__(self):
        return hash(self._total_us())

    def __bool__(self):
        return self._total_us() != 0

    def __str__(self):
        mm, ss = divmod(self._s, 60)
        hh, mm = divmod(mm, 60)
        s = "%d:%02d:%02d" % (hh, mm, ss)
        if self._us:
            s += ".%06d" % self._us
        if self._d:
            s = "%d day%s, %s" % (self._d, "" if abs(self._d) == 1 else "s", s)
        return s

    def __repr__(self):
        parts = []
        if self._d:
            parts.append("days=%d" % self._d)
        if self._s:
            parts.append("seconds=%d" % self._s)
        if self._us:
            parts.append("microseconds=%d" % self._us)
        return "datetime.timedelta(%s)" % (", ".join(parts) if parts else "0")


timedelta.min = timedelta(-999999999)
timedelta.max = timedelta(days=999999999, hours=23, minutes=59, seconds=59, microseconds=999999)
timedelta.resolution = timedelta(microseconds=1)


class tzinfo:
    def utcoffset(self, dt):
        raise NotImplementedError("tzinfo subclass must override utcoffset()")

    def tzname(self, dt):
        raise NotImplementedError("tzinfo subclass must override tzname()")

    def dst(self, dt):
        return None


class timezone(tzinfo):
    def __init__(self, offset, name=None):
        self._offset = offset
        self._name = name

    def utcoffset(self, dt):
        return self._offset

    def dst(self, dt):
        return None

    def tzname(self, dt):
        if self._name is not None:
            return self._name
        secs = _offset_seconds(self._offset)
        if secs == 0:
            return "UTC"
        a = -secs if secs < 0 else secs
        return "UTC%s%02d:%02d" % ("-" if secs < 0 else "+", a // 3600, a % 3600 // 60)

    def __eq__(self, other):
        return isinstance(other, timezone) and self._offset == other._offset

    def __hash__(self):
        return hash(self._offset)

    def __repr__(self):
        if _offset_seconds(self._offset) == 0 and self._name is None:
            return "datetime.timezone.utc"
        return "datetime.timezone(%r)" % (self._offset,)


timezone.utc = timezone(timedelta(0))
UTC = timezone.utc


class date:
    def __init__(self, year, month, day):
        _check_date(year, month, day)
        self._y = year
        self._m = month
        self._d = day

    year = property(lambda self: self._y)
    month = property(lambda self: self._m)
    day = property(lambda self: self._d)

    @classmethod
    def today(cls):
        t = _time.localtime()
        return cls(t[0], t[1], t[2])

    @classmethod
    def fromtimestamp(cls, t):
        s = _time.localtime(t)
        return cls(s[0], s[1], s[2])

    @classmethod
    def fromordinal(cls, n):
        y, m, d = _time._civil_from_days(n - _ORDINAL_1970)
        return cls(y, m, d)

    @classmethod
    def fromisoformat(cls, s):
        return cls(int(s[0:4]), int(s[5:7]), int(s[8:10]))

    def toordinal(self):
        return _time._days_from_civil(self._y, self._m, self._d) + _ORDINAL_1970

    def weekday(self):
        return (self.toordinal() + 6) % 7

    def isoweekday(self):
        return self.weekday() + 1

    def _yday(self):
        return self.toordinal() - date(self._y, 1, 1).toordinal() + 1

    def timetuple(self):
        return _time.struct_time((self._y, self._m, self._d, 0, 0, 0, self.weekday(), self._yday(), -1))

    def strftime(self, fmt):
        return _time.strftime(fmt, self.timetuple())

    def isoformat(self):
        return "%04d-%02d-%02d" % (self._y, self._m, self._d)

    def __str__(self):
        return self.isoformat()

    def __repr__(self):
        return "datetime.date(%d, %d, %d)" % (self._y, self._m, self._d)

    def replace(self, year=None, month=None, day=None):
        return date(self._y if year is None else year, self._m if month is None else month,
                    self._d if day is None else day)

    def __add__(self, other):
        if isinstance(other, timedelta):
            return date.fromordinal(self.toordinal() + other.days)
        return NotImplemented

    __radd__ = __add__

    def __sub__(self, other):
        if isinstance(other, timedelta):
            return date.fromordinal(self.toordinal() - other.days)
        if isinstance(other, date):
            return timedelta(days=self.toordinal() - other.toordinal())
        return NotImplemented

    def _key(self):
        return (self._y, self._m, self._d)

    def __eq__(self, other):
        return type(other) is type(self) and self._key() == other._key()

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._key())


date.min = date(MINYEAR, 1, 1)
date.max = date(MAXYEAR, 12, 31)
date.resolution = timedelta(days=1)


class time:
    def __init__(self, hour=0, minute=0, second=0, microsecond=0, tzinfo=None, fold=0):
        _check_time(hour, minute, second, microsecond)
        self._hh = hour
        self._mm = minute
        self._ss = second
        self._us = microsecond
        self._tz = tzinfo

    hour = property(lambda self: self._hh)
    minute = property(lambda self: self._mm)
    second = property(lambda self: self._ss)
    microsecond = property(lambda self: self._us)
    tzinfo = property(lambda self: self._tz)
    fold = property(lambda self: 0)

    @classmethod
    def fromisoformat(cls, s):
        us = int((s[9:] + "000000")[:6]) if len(s) > 9 else 0
        return cls(int(s[0:2]), int(s[3:5]) if len(s) >= 5 else 0, int(s[6:8]) if len(s) >= 8 else 0, us)

    def utcoffset(self):
        return None if self._tz is None else self._tz.utcoffset(None)

    def tzname(self):
        return None if self._tz is None else self._tz.tzname(None)

    def isoformat(self, timespec="auto"):
        off = self.utcoffset()
        return _iso_time(self._hh, self._mm, self._ss, self._us, None if off is None else _offset_seconds(off))

    def __str__(self):
        return self.isoformat()

    def __repr__(self):
        fields = [self._hh, self._mm, self._ss, self._us]
        while len(fields) > 2 and fields[-1] == 0:
            fields.pop()
        s = "datetime.time(%s" % ", ".join("%d" % f for f in fields)
        if self._tz is not None:
            s += ", tzinfo=%r" % (self._tz,)
        return s + ")"

    def strftime(self, fmt):
        return _time.strftime(fmt, _time.struct_time((1900, 1, 1, self._hh, self._mm, self._ss, 0, 1, -1)))

    def replace(self, hour=None, minute=None, second=None, microsecond=None, tzinfo=True):
        return time(self._hh if hour is None else hour, self._mm if minute is None else minute,
                    self._ss if second is None else second, self._us if microsecond is None else microsecond,
                    self._tz if tzinfo is True else tzinfo)

    def _key(self):
        return (self._hh, self._mm, self._ss, self._us)

    def __eq__(self, other):
        return isinstance(other, time) and self._key() == other._key()

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._key())


time.min = time(0, 0)
time.max = time(23, 59, 59, 999999)
time.resolution = timedelta(microseconds=1)


class datetime(date):
    def __init__(self, year, month, day, hour=0, minute=0, second=0, microsecond=0, tzinfo=None, fold=0):
        _check_date(year, month, day)
        _check_time(hour, minute, second, microsecond)
        self._y = year
        self._m = month
        self._d = day
        self._hh = hour
        self._mm = minute
        self._ss = second
        self._us = microsecond
        self._tz = tzinfo

    hour = property(lambda self: self._hh)
    minute = property(lambda self: self._mm)
    second = property(lambda self: self._ss)
    microsecond = property(lambda self: self._us)
    tzinfo = property(lambda self: self._tz)
    fold = property(lambda self: 0)

    @classmethod
    def _from_epoch(cls, secs, us, tz, offset):
        days, rem = divmod(secs + offset, 86400)
        y, m, d = _time._civil_from_days(days)
        hh, rem = divmod(rem, 3600)
        mm, ss = divmod(rem, 60)
        return cls(y, m, d, hh, mm, ss, us, tz)

    @classmethod
    def now(cls, tz=None):
        t = _p.ticks_us()
        secs = _time._EPOCH + t // 1000000
        offset = _time._TZSEC if tz is None else _offset_seconds(tz.utcoffset(None))
        return cls._from_epoch(secs, t % 1000000, tz, offset)

    @classmethod
    def utcnow(cls):
        t = _p.ticks_us()
        return cls._from_epoch(_time._EPOCH + t // 1000000, t % 1000000, None, 0)

    @classmethod
    def today(cls):
        return cls.now()

    @classmethod
    def fromtimestamp(cls, t, tz=None):
        secs = int(t // 1)
        us = int(round((t - secs) * 1000000))
        offset = _time._TZSEC if tz is None else _offset_seconds(tz.utcoffset(None))
        return cls._from_epoch(secs, us, tz, offset)

    @classmethod
    def utcfromtimestamp(cls, t):
        secs = int(t // 1)
        return cls._from_epoch(secs, int(round((t - secs) * 1000000)), None, 0)

    @classmethod
    def fromordinal(cls, n):
        y, m, d = _time._civil_from_days(n - _ORDINAL_1970)
        return cls(y, m, d)

    @classmethod
    def combine(cls, d, t, tzinfo=True):
        return cls(d.year, d.month, d.day, t.hour, t.minute, t.second, t.microsecond,
                   t.tzinfo if tzinfo is True else tzinfo)

    @classmethod
    def fromisoformat(cls, s):
        y, mo, d = int(s[0:4]), int(s[5:7]), int(s[8:10])
        hh = mi = ss = us = 0
        tz = None
        rest = s[11:]
        if rest:
            if rest.endswith("Z"):
                tz = timezone.utc
                rest = rest[:-1]
            else:
                for i in range(len(rest) - 1, 0, -1):
                    if rest[i] in "+-":
                        off = rest[i + 1:]
                        secs = int(off[0:2]) * 3600 + int(off[3:5]) * 60
                        tz = timezone(timedelta(seconds=-secs if rest[i] == "-" else secs))
                        rest = rest[:i]
                        break
            hh = int(rest[0:2])
            mi = int(rest[3:5]) if len(rest) >= 5 else 0
            ss = int(rest[6:8]) if len(rest) >= 8 else 0
            if len(rest) > 9:
                us = int((rest[9:] + "000000")[:6])
        return cls(y, mo, d, hh, mi, ss, us, tz)

    def _offset(self):
        return None if self._tz is None else _offset_seconds(self._tz.utcoffset(self))

    def _local_us(self):
        return ((self.toordinal() * 86400 + self._hh * 3600 + self._mm * 60 + self._ss) * 1000000 + self._us)

    def _key(self):
        off = self._offset()
        return self._local_us() - (0 if off is None else off * 1000000)

    def timestamp(self):
        off = self._offset()
        secs = (self.toordinal() - _ORDINAL_1970) * 86400 + self._hh * 3600 + self._mm * 60 + self._ss
        secs -= _time._TZSEC if off is None else off
        return secs + self._us / 1000000 if self._us else secs

    def date(self):
        return date(self._y, self._m, self._d)

    def time(self):
        return time(self._hh, self._mm, self._ss, self._us)

    def timetz(self):
        return time(self._hh, self._mm, self._ss, self._us, self._tz)

    def utcoffset(self):
        return None if self._tz is None else self._tz.utcoffset(self)

    def tzname(self):
        return None if self._tz is None else self._tz.tzname(self)

    def astimezone(self, tz=None):
        secs = int(self.timestamp() // 1)
        offset = _time._TZSEC if tz is None else _offset_seconds(tz.utcoffset(None))
        return datetime._from_epoch(secs, self._us, tz, offset)

    def timetuple(self):
        return _time.struct_time((self._y, self._m, self._d, self._hh, self._mm, self._ss,
                                  self.weekday(), self._yday(), -1))

    def isoformat(self, sep="T", timespec="auto"):
        return "%04d-%02d-%02d%s" % (self._y, self._m, self._d, sep) + _iso_time(
            self._hh, self._mm, self._ss, self._us, self._offset())

    def __str__(self):
        return self.isoformat(" ")

    def __repr__(self):
        fields = [self._y, self._m, self._d, self._hh, self._mm, self._ss, self._us]
        while len(fields) > 5 and fields[-1] == 0:
            fields.pop()
        s = "datetime.datetime(%s" % ", ".join("%d" % f for f in fields)
        if self._tz is not None:
            s += ", tzinfo=%r" % (self._tz,)
        return s + ")"

    def replace(self, year=None, month=None, day=None, hour=None, minute=None, second=None,
                microsecond=None, tzinfo=True):
        return datetime(self._y if year is None else year, self._m if month is None else month,
                        self._d if day is None else day, self._hh if hour is None else hour,
                        self._mm if minute is None else minute, self._ss if second is None else second,
                        self._us if microsecond is None else microsecond,
                        self._tz if tzinfo is True else tzinfo)

    def __add__(self, other):
        if not isinstance(other, timedelta):
            return NotImplemented
        days, us = divmod(self._local_us() + other._total_us(), _US_PER_DAY)
        secs, us = divmod(us, 1000000)
        y, m, d = _time._civil_from_days(days - _ORDINAL_1970)
        hh, rem = divmod(secs, 3600)
        mm, ss = divmod(rem, 60)
        return datetime(y, m, d, hh, mm, ss, us, self._tz)

    __radd__ = __add__

    def __sub__(self, other):
        if isinstance(other, timedelta):
            return self + (-other)
        if isinstance(other, datetime):
            return timedelta(microseconds=self._key() - other._key())
        return NotImplemented

    def __eq__(self, other):
        return isinstance(other, datetime) and self._key() == other._key()

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._key())


datetime.min = datetime(MINYEAR, 1, 1)
datetime.max = datetime(MAXYEAR, 12, 31, 23, 59, 59, 999999)
datetime.resolution = timedelta(microseconds=1)
