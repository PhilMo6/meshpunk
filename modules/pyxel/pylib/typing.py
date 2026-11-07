# typing for Pyxel games: every commonly imported name exists, so imports,
# annotations and type aliases work; nothing checks types. NamedTuple is the
# functional form only (MicroPython discards class-body annotations).

TYPE_CHECKING = False


class _Alias:
    def __init__(self, name):
        self._name = name

    def __getitem__(self, params):
        return self

    def __call__(self, *args, **kwargs):
        raise TypeError("typing.%s cannot be instantiated" % self._name)

    def __or__(self, other):
        return self

    def __ror__(self, other):
        return self

    def __repr__(self):
        return "typing." + self._name


Any = _Alias("Any")
Union = _Alias("Union")
Optional = _Alias("Optional")
List = _Alias("List")
Dict = _Alias("Dict")
Tuple = _Alias("Tuple")
Set = _Alias("Set")
FrozenSet = _Alias("FrozenSet")
Callable = _Alias("Callable")
Iterable = _Alias("Iterable")
Iterator = _Alias("Iterator")
Generator = _Alias("Generator")
Sequence = _Alias("Sequence")
MutableSequence = _Alias("MutableSequence")
Mapping = _Alias("Mapping")
MutableMapping = _Alias("MutableMapping")
AbstractSet = _Alias("AbstractSet")
MutableSet = _Alias("MutableSet")
Collection = _Alias("Collection")
Container = _Alias("Container")
Hashable = _Alias("Hashable")
Sized = _Alias("Sized")
Reversible = _Alias("Reversible")
Type = _Alias("Type")
Literal = _Alias("Literal")
Final = _Alias("Final")
ClassVar = _Alias("ClassVar")
Annotated = _Alias("Annotated")
Self = _Alias("Self")
NoReturn = _Alias("NoReturn")
Never = _Alias("Never")
LiteralString = _Alias("LiteralString")
TypeAlias = _Alias("TypeAlias")
TypeGuard = _Alias("TypeGuard")
Required = _Alias("Required")
NotRequired = _Alias("NotRequired")
Unpack = _Alias("Unpack")
Concatenate = _Alias("Concatenate")
Deque = _Alias("Deque")
DefaultDict = _Alias("DefaultDict")
Counter = _Alias("Counter")
OrderedDict = _Alias("OrderedDict")
ChainMap = _Alias("ChainMap")
Awaitable = _Alias("Awaitable")
Coroutine = _Alias("Coroutine")
AsyncIterator = _Alias("AsyncIterator")
AsyncGenerator = _Alias("AsyncGenerator")
SupportsInt = _Alias("SupportsInt")
SupportsFloat = _Alias("SupportsFloat")
SupportsIndex = _Alias("SupportsIndex")
SupportsAbs = _Alias("SupportsAbs")
IO = _Alias("IO")
TextIO = _Alias("TextIO")
BinaryIO = _Alias("BinaryIO")
Pattern = _Alias("Pattern")
Match = _Alias("Match")
AnyStr = _Alias("AnyStr")
Text = str


class Generic:
    def __class_getitem__(cls, params):
        return cls


class Protocol(Generic):
    pass


class TypedDict(dict):
    pass


def TypeVar(name, *constraints, bound=None, covariant=False, contravariant=False):
    return _Alias(name)


def ParamSpec(name, *args, **kwargs):
    return _Alias(name)


def TypeVarTuple(name):
    return _Alias(name)


def NewType(name, tp):
    return lambda x: x


def NamedTuple(typename, fields=None, **kwargs):
    from collections import namedtuple
    names = [f[0] for f in fields] if fields is not None else list(kwargs)
    return namedtuple(typename, names)


def cast(typ, val):
    return val


def overload(func):
    return func


def final(f):
    return f


def no_type_check(f):
    return f


def runtime_checkable(cls):
    return cls


def get_type_hints(obj, globalns=None, localns=None, include_extras=False):
    return {}


def get_origin(tp):
    return None


def get_args(tp):
    return ()


def assert_type(val, typ):
    return val


def reveal_type(obj):
    return obj


def assert_never(arg):
    raise AssertionError("Expected code to be unreachable")


def dataclass_transform(**kwargs):
    return lambda cls: cls
