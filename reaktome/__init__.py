import logging

from typing import Any, Callable, Union

import _reaktome as _r


LOGGER = logging.getLogger(__name__)
LOGGER.addHandler(logging.NullHandler())


class Reverse:
    def __init__(self, parent, obj, type, name):
        self.parent = parent
        self.obj = obj
        self.type = type
        self.name = name

    def make_name(self, name):
        if self.type == 'attr':
            return f'{self.name}.{name}'

        elif self.type == 'item':
            return f'{self.name}[{name}]'

    def __eq__(self, other: 'Reverse'):
        if isinstance(other, Reverse):
            return (self.parent, self.obj, self.name, self.type) == \
                (other.parent, other.obj, other.name, other.type)

    def __call__(self, name, old, new):
        name = self.make_name(name)
        if self.obj == self.parent:
            self.parent.on_change(name, old, new)
            return

        if hasattr(self.parent, '__reaktome_reverse__'):
            self.parent.__reaktome_reverse__(name, old, new)


class Reverses:
    def __init__(self):
        self.reverses = []

    def __bool__(self):
        return bool(self.reverses)

    def __iadd__(self, reverse: Reverse):
        self.reverses.append(reverse)

    def __isub__(self, reverse: Reverse):
        self.reverses.remove(reverse)

    def __call__(self, name, old, new):
        for r in self.reverses:
            r(name, old, new)


def __reaktome_setattr__(self, name, old, new):
    reaktiv8(new, parent=self, name=name, type='attr')
    deaktiv8(old, parent=self, name=name, type='attr')
    self.__reaktome_reverse__(name, old, new)
    return new


def __reaktome_delattr__(self, name, value):
    deaktiv8(value, parent=self, name=name, type='attr')


def reaktiv8(obj: Any, parent=None, name=None, type='attr'):
    """
    Inject __setattr__, __setitem__ hooks.
    """

    if isinstance(obj, list):
        pass

    elif isinstance(obj, dict):
        pass

    elif hasattr(obj, '__dict__'):
        _r.patch_type(obj.__class__)
        obj.__dict__['__reaktome_setattr__'] = __reaktome_setattr__
        obj.__dict__['__reaktome_delattr__'] = __reaktome_delattr__
        reverses = obj.__dict__.setdefault('__reaktome_reverse__', Reverses())
        reverses += Reverse(parent, obj, type, name)

    return obj


def deaktiv8(obj: Any, parent=None, name=None, type='attr'):
    if isinstance(obj, list):
        pass

    elif isinstance(obj, dict):
        pass

    elif hasattr(obj, '__dict__'):
        reverses = obj.__dict__.get('__reaktome_reverse__')
        if reverses:
            reverses -= Reverse(parent, obj, type, name)
        if not reverses:
            obj.__dict__.pop('__reaktome_reverse__')
            obj.__dict__.pop('__reaktome_setattr__')
            obj.__dict__.pop('__reaktome_delattr__')

    return obj


class Reaktome:
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        reaktiv8(self, parent=self, name=self.__class__.__name__)

    def __post_init__(self):
        Reaktome.__init__(self)

    def on_change(self, name, old, new):
        print(f'⚡ {name}: {repr(old)} → {repr(new)}')
