# abc for Pyxel games: ABC is a plain base class and the abstract decorators
# return what they wrap, so an abstract class can be instantiated.
# ABCMeta cannot be used as metaclass= (MicroPython has no metaclasses).


def abstractmethod(funcobj):
    return funcobj


def abstractclassmethod(funcobj):
    return classmethod(funcobj)


def abstractstaticmethod(funcobj):
    return staticmethod(funcobj)


abstractproperty = property


class ABC:
    pass


class ABCMeta(type):
    pass
