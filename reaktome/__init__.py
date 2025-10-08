import re
import logging

from fnmatch import fnmatch
from typing import Any, Optional, Callable, Union

import _reaktome as _r  # type: ignore


LOGGER = logging.getLogger(__name__)
LOGGER.addHandler(logging.NullHandler())

SENTINAL = object()


class Change:
    def __init__(self,
                 obj: Any,
                 key: Union[str, int],
                 old: Any,
                 new: Any,
                 source: str,
                 ) -> None:
        self.obj = obj
        self.key = key
        self.old = old
        self.new = new
        self.source = source


class BackRef:
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

    def make_name(self, name: Union[str, int], source: str) -> str:
        if source == 'item':
            return f'{self.name}[{repr(name)}]'

        elif source == 'set':
            return f'{self.name}{{}}'

        else:  # attr
            return f'{self.name}.{name}'

    def __eq__(self, them: Any) -> bool:
        if not isinstance(them, BackRef):
            raise NotImplementedError()
        return (id(self.parent), id(self.obj), self.name, self.source) == \
            (id(them.parent), id(them.obj), them.name, them.source)

    def __hash__(self):
        return hash((id(self.parent), id(self.obj), self.name, self.source))

    def __call__(self,
                 change: Change,
                 ) -> None:
        if self.parent is None:
            return

        name = self.make_name(change.key, change.source)
        Changes.invoke(
            Change(
                self.parent,
                name,
                change.old,
                change.new,
                source=self.source
            )
        )


class ChangeFilter:
    def __init__(self,
                 pattern: str = '*',
                 regex=False,
                 ) -> None:
        self.pattern = re.compile(pattern) if regex else pattern

    def __call__(self, change: Change) -> bool:
        if self.pattern == '*':
            return True
        if isinstance(change.key, int):
            return self.pattern == change.key
        if isinstance(self.pattern, re.Pattern):
            return bool(self.pattern.match(change.key))
        return fnmatch(change.key, self.pattern)


class Changes:
    __instances__: dict[int, 'Changes'] = {}

    def __init__(self):
        self.backrefs: set[BackRef] = set()
        self.callbacks: list[
            tuple[Callable[[Any], bool], Callable[[Any], Any]]
        ] = []

    def _add_backref(self, backref: BackRef) -> None:
        self.backrefs.add(backref)

    def _del_backref(self, backref: BackRef) -> None:
        self.backrefs.discard(backref)

    def _invoke(self, change: Change) -> None:
        LOGGER.debug(f'⚡ {change.key}: {repr(change.old)} → {repr(change.new)}')
        for r in self.backrefs:
            try:
                r(change)

            except Exception as e:
                LOGGER.error('Reverse call failed: %s', e)
                continue

        for filter, cb in self.callbacks:
            if not filter(change):
                continue

            LOGGER.debug('Invoking callback: %s', repr(cb))
            try:
                cb(change)

            except Exception as e:
                LOGGER.error('Callback failed: %s', e)
                continue

    @classmethod
    def add_backref(cls, obj: Any, backref: BackRef) -> BackRef:
        changes = cls.__instances__.setdefault(id(obj), Changes())
        changes._add_backref(backref)
        return backref

    @classmethod
    def invoke(cls, change: Change) -> None:
        changes = cls.__instances__.get(id(change.obj))
        if changes:
            changes._invoke(change)

    @classmethod
    def del_backref(cls, obj: Any, backref: BackRef) -> None:
        changes = cls.__instances__.get(id(obj))
        if not changes:
            return
        changes._del_backref(backref)
        if changes.backrefs:
            return
        cls.__instances__.pop(id(obj), None)

    @classmethod
    def on(cls,
           obj: Any,
           cb: Callable[[Any], Any],
           pattern: str = '*',
           regex: bool = False,
           ) -> None:
        try:
            changes = cls.__instances__[id(obj)]

        except KeyError:
            raise ValueError(f'object {repr(obj)} not tracked')

        changes.callbacks.append((ChangeFilter(pattern, regex=regex), cb))


def __reaktome_setattr__(self, name: str, old: Any, new: Any) -> None:
    "Used by Obj."
    LOGGER.debug('__reaktome_setattr__(%s, %s, %s)', name, repr(old), repr(new))
    if name.startswith('_'):
        LOGGER.debug('Skipping private/protected attr: %s', name)
        return new
    reaktiv8(new, name, parent=self, source='attr')
    deaktiv8(old, name, parent=self, source='attr')
    Changes.invoke(Change(self, name, old, new, source='attr'))
    return new


def __reaktome_delattr__(self, name: str, old: Any, new: Any) -> None:
    "Used by Obj."
    LOGGER.debug('__reaktome_delattr__(%s, %s, %s)', name, repr(old), repr(new))
    if name.startswith('_'):
        LOGGER.debug('Skipping private/protected attr: %s', name)
        return
    deaktiv8(old, name, parent=self, source='attr')
    Changes.invoke(Change(self, name, old, None, source='attr'))


def __reaktome_setitem__(self, key: Union[int, str], old: Any, new: Any) -> None:
    "Used by Dict, List."
    LOGGER.debug('__reaktome_setitem__(%s, %s, %s)', key, repr(old), repr(new))
    reaktiv8(new, key, parent=self, source='item')
    deaktiv8(old, key, parent=self, source='item')
    Changes.invoke(Change(self, key, old, new, source='item'))


def __reaktome_delitem__(self, key: Union[int, str], old: Any, new: Any) -> None:
    "Used by Dict, List."
    LOGGER.debug('__reaktome_delitem__(%s, %s, %s)', key, repr(old), repr(new))
    deaktiv8(old, key, parent=self, source='item')
    Changes.invoke(Change(self, key, old, None, source='item'))


def __reaktome_additem__(self, key: Union[int, str], old: Any, new: Any) -> None:
    LOGGER.debug('__reaktome_additem__(%s, %s, %s)', key, repr(old), repr(new))
    reaktiv8(new, key, parent=self, source='set')
    Changes.invoke(Change(self, key, old, new, source='set'))


def __reaktome_discarditem__(self, key: Union[int, str, None], old: Any, new: Any) -> None:
    LOGGER.debug('__reaktome_discarditem__(%s, %s, %s)', key, repr(old), repr(new))
    deaktiv8(old, key, parent=self, source='set')
    Changes.invoke(Change(self, key, old, None, source='set'))


def reaktiv8(
    obj: Any,
    name: Optional[str] = None,
    parent: Any = None,
    source: str = "attr",
) -> None:
    """
    Activate reaktome hooks on an object instance and register it for change
    tracking.
    """
    if name is None:
        name = obj.__class__.__name__

    if isinstance(obj, list):
        LOGGER.debug('Activating list: %s', name)
        _r.patch_list(obj, {
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))

    elif isinstance(obj, set):
        LOGGER.debug('Activating set: %s', name)
        _r.patch_set(obj, {
            "__reaktome_additem__": __reaktome_additem__,
            "__reaktome_discarditem__": __reaktome_discarditem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))

    elif isinstance(obj, dict):
        LOGGER.debug('Activating dict: %s', name)
        _r.patch_dict(obj, {
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))

    elif hasattr(obj, "__dict__"):
        LOGGER.debug('Activating obj: %s', name)
        _r.patch_obj(obj, {
            "__reaktome_setattr__": __reaktome_setattr__,
            "__reaktome_delattr__": __reaktome_delattr__,
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="attr"))

    else:
        LOGGER.info('Unsupported type: %s', name)
        return


def deaktiv8(
    obj: Any,
    name: Optional[str] = None,
    parent: Any = None,
    source: str = "attr",
) -> None:
    """
    Deactivate reaktome hooks on an object instance and remove it from change
    tracking.
    """
    if name is None:
        name = obj.__class__.__name__

    if isinstance(obj, list):
        LOGGER.debug('Deactivating list: %s', name)
        _r.patch_list(obj, None)
        Changes.del_backref(obj, BackRef(parent, obj, name, source))

    elif isinstance(obj, set):
        LOGGER.debug('Deactivating set: %s', name)
        _r.patch_set(obj, None)
        Changes.del_backref(obj, BackRef(parent, obj, name, source))

    elif isinstance(obj, dict):
        LOGGER.debug('Deactivating dict: %s', name)
        _r.patch_dict(obj, None)
        Changes.del_backref(obj, BackRef(parent, obj, name, source))

    elif hasattr(obj, "__dict__"):
        LOGGER.debug('Deactivating obj: %s', name)
        _r.patch_obj(obj, None)
        Changes.del_backref(obj, BackRef(parent, obj, name, source))

    else:
        LOGGER.debug('Unsupported type: %s', name)
        return


class Reaktome:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        reaktiv8(self, parent=None, name=self.__class__.__name__)

    def __post_init__(self) -> None:
        reaktiv8(self, parent=None, name=self.__class__.__name__)


def receiver(obj: Any, pattern: str = '*', regex: bool = False) -> Callable:
    def wrapper(f):
        Changes.on(obj, f, pattern=pattern, regex=regex)
        return f
    return wrapper
