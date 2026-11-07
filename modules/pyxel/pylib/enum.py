# enum for Pyxel games: Enum, IntEnum, Flag, IntFlag, auto and unique.
#
# Members are instances of their class, made in definition order, with .name
# and .value. A class iterates over its members (aliases excluded), has len(),
# is indexed by member name (Cls["NAME"]) and is called with a value
# (Cls(value)). This rests on interpreter hooks: a base class with a true
# _ordered_namespace_ keeps the class body in definition order,
# __init_subclass__ runs for every new class, and __class_iter__, __class_len__
# and __class_getitem__ serve iter(cls), len(cls) and cls[key].
#
# IntEnum and IntFlag members are int instances; str() and format() give the
# number. A class that also derives from another built-in type, such as
# class C(str, Enum), has members that are instances of that type, and str()
# and repr() of those members follow Enum.

from ucollections import OrderedDict


class auto:
    pass


def _function():
    pass


def _generator():
    yield


def _make_closure():
    x = 0

    def closure():
        return x
    return closure


_NOT_MEMBER = (type(_function), type(_generator), type(_make_closure()),
               classmethod, staticmethod, property, type)
_BUILTIN = (int, float, str, bytes, tuple)


def _is_member(name, value):
    if name[:1] == "_" and (name[-1:] == "_" or name[:2] == "__"):
        return False
    return not isinstance(value, _NOT_MEMBER) and not hasattr(type(value), "__get__")


def _noop_init(self, *args, **kwds):
    pass


def _single_bit(v):
    return v > 0 and v & (v - 1) == 0


def _builtin_of(cls):
    for t in _BUILTIN:
        if issubclass(cls, t):
            return t
    return None


def _init_builtin(cls, builtin, m, args):
    # super(k, m).__init__ reaches the built-in type's constructor when k lists
    # that type among its own bases.
    k = cls
    while builtin not in k.__bases__:
        for b in k.__bases__:
            if issubclass(b, builtin):
                k = b
                break
    super(k, m).__init__(*args)


def _find(value, value_map, member_list):
    try:
        return value_map.get(value)
    except TypeError:
        for m in member_list:
            if m._value_ == value:
                return m
        return None


def _build(cls):
    ns = cls.__dict__
    ignore = ns.get("_ignore_")
    if ignore is not None:
        if isinstance(ignore, str):
            ignore = ignore.replace(",", " ").split()
        for key in list(ignore) + ["_ignore_"]:
            if key in ns:
                delattr(cls, key)
        ns = cls.__dict__
    names = [key for key in ns if _is_member(key, ns[key])]
    if names and cls._member_list_:
        raise TypeError("%s cannot extend an enumeration that has members" % cls.__name__)

    # A __new__ or __init__ written in the class body builds members; afterwards
    # cls(value) looks members up and does not run them again.
    if "__new__" in ns:
        cls._enum_new_ = cls.__new__
        delattr(cls, "__new__")
    if "__init__" in ns:
        cls._enum_init_ = ns["__init__"]
        cls.__init__ = _noop_init
    new = getattr(cls, "_enum_new_", None)
    init = getattr(cls, "_enum_init_", None)

    # A built-in base comes before Enum in the lookup order, so the Enum
    # methods it would hide are set on the class.
    builtin = _builtin_of(cls)
    if builtin is not None:
        cls.__new__ = _lookup
        if "__repr__" not in ns:
            cls.__repr__ = Enum.__repr__
        if "__str__" not in ns and not cls._value_str_:
            cls.__str__ = Enum.__str__

    member_type = builtin or cls._member_type_
    flag = cls._flag_
    gen = cls._generate_next_value_
    members = OrderedDict()
    member_list = []
    value_map = {}
    last_values = []
    auto_values = {}
    for key in names:
        value = ns[key]
        if isinstance(value, auto):
            # B = A where A = auto() names the same auto object: an alias.
            if value in auto_values:
                value = auto_values[value]
            else:
                generated = gen(key, 1, len(members), last_values[:])
                auto_values[value] = generated
                value = generated
        last_values.append(value)
        args = value if isinstance(value, tuple) else (value,)
        if new is not None:
            m = new(cls, *args)
        else:
            m = object.__new__(cls)
            if builtin is not None:
                _init_builtin(cls, builtin, m, args)
        if not hasattr(m, "_value_"):
            m._value_ = value if member_type is object else member_type(*args)
        m._name_ = key
        m.name = key
        m.value = m._value_
        if init is not None:
            init(m, *args)
            m.value = m._value_
        canonical = _find(m._value_, value_map, member_list)
        if canonical is None:
            canonical = m
            try:
                value_map[m._value_] = m
            except TypeError:
                pass
            if not flag or _single_bit(m._value_):
                member_list.append(m)
        members[key] = canonical
        setattr(cls, key, canonical)
    cls._member_list_ = member_list
    cls._member_names_ = [m._name_ for m in member_list]
    cls._member_map_ = members
    cls.__members__ = members
    cls._value2member_map_ = value_map


def _lookup(cls, value, names=None, **kwargs):
    if names is not None:
        return _functional(cls, value, names, kwargs.get("type"), kwargs.get("start", 1))
    if type(value) is cls:
        return value
    m = _find(value, cls._value2member_map_, cls._member_list_)
    if m is None:
        m = cls._missing_(value)
        if m is None:
            raise ValueError("%r is not a valid %s" % (value, cls.__name__))
    return m


def _functional(base, cls_name, names, mixin, start):
    if isinstance(names, str):
        names = names.replace(",", " ").split()
    ns = OrderedDict()
    if isinstance(names, dict):
        for key in names:
            ns[key] = names[key]
    else:
        gen = base._generate_next_value_
        last_values = []
        for i, item in enumerate(names):
            if isinstance(item, str):
                value = gen(item, start, i, last_values[:])
            else:
                item, value = item
            last_values.append(value)
            ns[item] = value
    return type(cls_name, (base,) if mixin is None else (mixin, base), ns)


class Enum:
    _ordered_namespace_ = True
    _member_type_ = object
    _value_str_ = False
    _flag_ = False
    _member_list_ = ()
    _member_names_ = ()
    _member_map_ = OrderedDict()
    __members__ = _member_map_
    _value2member_map_ = {}

    def __init_subclass__(cls):
        _build(cls)

    @classmethod
    def __class_iter__(cls):
        return iter(cls._member_list_)

    @classmethod
    def __class_len__(cls):
        return len(cls._member_list_)

    def __class_getitem__(cls, name):
        return cls._member_map_[name]

    __new__ = _lookup

    @staticmethod
    def _generate_next_value_(name, start, count, last_values):
        if not last_values:
            return start
        return max(last_values) + 1

    @classmethod
    def _missing_(cls, value):
        return None

    def __repr__(self):
        if self._name_ is None:
            return "<%s: %r>" % (type(self).__name__, self._value_)
        return "<%s.%s: %r>" % (type(self).__name__, self._name_, self._value_)

    def __str__(self):
        if self._name_ is None:
            return "%s(%r)" % (type(self).__name__, self._value_)
        return "%s.%s" % (type(self).__name__, self._name_)

    def __copy__(self):
        return self

    def __deepcopy__(self, memo):
        return self


class IntEnum(int, Enum):
    _member_type_ = int
    _value_str_ = True


def _flag_operand(self, other):
    cls = type(self)
    if isinstance(other, cls):
        return other._value_
    if cls._member_type_ is int and isinstance(other, int):
        return int(other)
    raise TypeError("unsupported operand for %s: %r" % (cls.__name__, other))


class Flag(Enum):
    _flag_ = True
    _flag_mask_ = 0

    def __init_subclass__(cls):
        _build(cls)
        mask = 0
        for m in cls._member_list_:
            mask |= m._value_
        cls._flag_mask_ = mask

    @staticmethod
    def _generate_next_value_(name, start, count, last_values):
        if not count:
            return 1 if start is None else start
        bit = 1
        top = max(last_values)
        while bit <= top:
            bit <<= 1
        return bit

    @classmethod
    def _missing_(cls, value):
        # Combinations of flags become unnamed-in-the-class members; a Flag
        # (not IntFlag) value with bits outside its members is invalid.
        if not isinstance(value, int) or value < 0:
            return None
        if cls._member_type_ is not int and value & ~cls._flag_mask_:
            return None
        m = object.__new__(cls)
        if cls._member_type_ is int:
            _init_builtin(cls, int, m, (value,))
        names = [x._name_ for x in cls._member_list_ if x._value_ & value == x._value_]
        m._value_ = value
        m._name_ = "|".join(names) if names else None
        m.name = m._name_
        m.value = value
        cls._value2member_map_[value] = m
        return m

    def __contains__(self, other):
        v = _flag_operand(self, other)
        return v & self._value_ == v

    def __iter__(self):
        for m in type(self)._member_list_:
            if m._value_ & self._value_ == m._value_:
                yield m

    def __len__(self):
        n = 0
        for m in type(self)._member_list_:
            if m._value_ & self._value_ == m._value_:
                n += 1
        return n

    def __bool__(self):
        return self._value_ != 0

    def __or__(self, other):
        return type(self)(self._value_ | _flag_operand(self, other))

    def __and__(self, other):
        return type(self)(self._value_ & _flag_operand(self, other))

    def __xor__(self, other):
        return type(self)(self._value_ ^ _flag_operand(self, other))

    def __invert__(self):
        cls = type(self)
        return cls(cls._flag_mask_ & ~self._value_)


class IntFlag(int, Flag):
    # int comes before Flag in the lookup order, so the Flag operators are
    # repeated here.
    _member_type_ = int
    _value_str_ = True
    __or__ = Flag.__or__
    __and__ = Flag.__and__
    __xor__ = Flag.__xor__
    __ror__ = Flag.__or__
    __rand__ = Flag.__and__
    __rxor__ = Flag.__xor__
    __invert__ = Flag.__invert__
    __contains__ = Flag.__contains__
    __len__ = Flag.__len__


def unique(enumeration):
    dups = ["%s -> %s" % (name, m._name_) for name, m in enumeration.__members__.items() if name != m._name_]
    if dups:
        raise ValueError("duplicate values found in %s: %s" % (enumeration.__name__, ", ".join(dups)))
    return enumeration
