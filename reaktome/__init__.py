import re
import logging

from fnmatch import fnmatch
from types import MethodType
from typing import Any, Optional, Callable

import _reaktome as _r


LOGGER = logging.getLogger(__name__)
LOGGER.addHandler(logging.NullHandler())

SENTINAL = object()
REVERSES = {}


def print_change(name: str, old: Any, new: Any) -> None:
    print(f'⚡ {name}: {repr(old)} → {repr(new)}')


class Change:
    def __init__(self,
                 parent: Any,
                 obj: Any,
                 name: str,
                 source: str = 'attr',
                 ) -> None:
        self.parent = parent
        self.obj = obj
        self.name = name
        self.source = source

    def make_name(self, name: str, source: str) -> str:
        if source == 'item':
            return f'{self.name}[{repr(name)}]'

        elif source == 'set':
            return f'{self.name}{{}}'

        else:  # attr
            return f'{self.name}.{name}'

    def __eq__(self, them: 'Change'):
        return (isinstance(other, Change) and
            (id(self.parent), id(self.obj), self.name, self.source) ==
            (id(them.parent), id(them.obj), them.name, them.source))

    def __hash__(self):
        return hash((id(self.parent), id(self.obj), self.name, self.source))

    def __call__(self,
                 name: str,
                 old: Any,
                 new: Any,
                 source: str = 'attr',
                 ) -> None:
        name = self.make_name(name, source)
        Changes.invoke(self, name, old, new, self.source)


class ChangeFilter:
    def __init__(self,
                 pattern: Optional[str] = None,
                 regex=False,
                 ) -> None:
        self.pattern = re.compile(pattern) if regex else pattern
        self.regex = regex

    def __call__(self, name: str) -> bool:
        if self.pattern is None:
            return True
        if self.regex:
            return bool(self.pattern.match(name))
        return fnmatch(name, self.pattern)


class Changes:
    __instances__: dict[int, 'Changes'] = {}

    def __init__(self):
        self.changes: list[Change] = []
        self.callbacks: list[tuple[Callable[Any, bool], Callable[Any, Any]]] = []

    def __bool__(self):
        return bool(self.changes)

    def __iadd__(self, change: Change) -> None:
        self.changes.append(change)

    def __isub__(self, change: Change) -> None:
        self.changes.remove(change)

    def __call__(self, name: str, old: Any, new: Any, source: str) -> None:
        for r in self.changes:
            r(name, old, new, source)
        for filter, cb in self.callbacks:
            if not filter(name):
                continue
            cb(name, old, new)

    @classmethod
    def add(cls, obj: Any, change: Change) -> Change:
        changes = cls.__instances__.setdefault(id(obj), Changes())
        changes += change
        return change

    @classmethod
    def invoke(cls, obj: Any, *args: Any, **kwargs: Any) -> None:
        changes = cls.__instances__.get(id(obj))
        if changes:
            changes(*args, **kwargs)

    @classmethod
    def remove(cls, obj: Any, change: Change) -> None:
        changes = cls.__instances__.get(id(obj))
        if not changes:
            return
        changes -= change
        if changes:
            return
        cls.__instances__.pop(id(obj), None)

    @classmethod
    def on(cls,
           obj: Any,
           cb: Callable[Any, Any],
           pattern: Optional[str] = None,
           regex: bool = False,
           ) -> None:
        try:
            changes = cls.__instances__[id(obj)]

        except KeyError:
            raise ValueError(f'object {repr(obj)} not tracked')

        changes.callbacks.append((ChangeFilter(pattern, regex=regex), cb))


def __reaktome_setattr__(self, name: str, old: Any, new: Any) -> None:
    "Used by Obj."
    if name.startswith('_'):
        return new
    reaktiv8(new, parent=self, name=name, source='attr')
    deaktiv8(old, parent=self, name=name, source='attr')
    Changes.invoke(self, name, old, new, source='attr')
    return new


def __reaktome_delattr__(self, name: str, old: Any) -> None:
    "Used by Obj."
    if name.startswith('_'):
        return
    deaktiv8(old, parent=self, name=name, source='attr')
    Changes.invoke(self, name, old, None, source='attr')


def __reaktome_setitem__(self, key: str, old: Any, new: Any) -> None:
    "Used by Dict, List."
    reaktiv8(new, parent=self, name=key, source='item')
    deaktiv8(old, parent=self, name=key, source='item')
    Changes.invoke(self, key, old, new, source='item')


def __reaktome_delitem__(self, key: str, old: Any) -> None:
    "Used by Dict, List."
    deaktiv8(old, parent=self, name=key, source='item')
    Changes.invoke(self, key, old, None, source='item')


def reaktiv8(obj: Any,
             parent: Any = None,
             name: Optional[str] = None,
             source: str = 'attr',
             ) -> None:
    """
    Inject __setattr__, __setitem__ and other instance-level hooks.
    """

    if isinstance(obj, set):
        _r.patch_set()
        Changes.add(obj, Change(parent, obj, name, source='item'))

    elif isinstance(obj, list):
        _r.patch_list()
        Changes.add(obj, Change(parent, obj, name, source='item'))

    elif isinstance(obj, dict):
        _r.patch_dict()
        Changes.add(obj, Change(parent, obj, name, source='item'))

    elif hasattr(obj, '__dict__'):
        _r.patch_type(obj.__class__)
        obj.__dict__['__reaktome_setattr__'] = __reaktome_setattr__
        obj.__dict__['__reaktome_delattr__'] = __reaktome_delattr__
        Changes.add(obj, Change(parent, obj, name, source='attr'))

    else:
        return


def deaktiv8(obj: Any,
             parent: Any = None,
             name: str = None,
             source: str = 'attr',
             ) -> None:
    if isinstance(obj, (list, dict, set)) or hasattr(obj, '__dict__'):
        Changes.remove(obj, Change(parent, obj, name, source))


class Reaktome:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        reaktiv8(self, parent=self, name=self.__class__.__name__)

    def __post_init__(self) -> None:
        reaktiv8(self, parent=self, name=self.__class__.__name__)
