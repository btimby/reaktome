import re
import logging

from fnmatch import fnmatch
from typing import Any, Optional, Callable, Union
from types import MethodType

import _reaktome as _r  # type: ignore

BaseModel: Optional[type]
try:
    from pydantic import BaseModel

except ImportError:
    BaseModel = None


BaseCollectionModel: Optional[type]
try:
    from pydantic_collections import BaseCollectionModel  # type: ignore

except ImportError:
    BaseCollectionModel = None


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

    def __repr__(self) -> str:
        return f'⚡ {self.key}: {repr(self.old)} → {repr(self.new)}'


class BackRef:
    def __init__(self,
                 parent: Any,
                 obj: Any,
                 name: Union[int, str],
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

    def __init__(self) -> None:
        self.backrefs: set[BackRef] = set()
        self.callbacks: list[
            tuple[Callable[[Any], bool], Callable[[Any], Any]]
        ] = []

    def __repr__(self):
        backrefs = ', '.join([
            f'{id(br.parent)} → {id(br.obj)}' for br in list(self.backrefs)])
        callbacks = ', '.join(cb[1].__name__ for cb in self.callbacks)
        return f'{id(self)}: [{backrefs}] [{callbacks}]'

    def _add_backref(self, backref: BackRef) -> None:
        self.backrefs.add(backref)

    def _del_backref(self, backref: BackRef) -> None:
        self.backrefs.discard(backref)

    def _invoke(self, change: Change) -> None:
        LOGGER.debug(repr(change))
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
    LOGGER.debug(
        '__reaktome_setattr__(%s, %s, %s, %s)',
        repr(self), name, repr(old), repr(new),
    )
    if name.startswith('_'):
        LOGGER.debug('Skipping private/protected attr: %s', name)
        return new
    reaktiv8(new, name, parent=self, source='attr')
    deaktiv8(old, name, parent=self, source='attr')
    Changes.invoke(Change(self, name, old, new, source='attr'))
    return new


def __reaktome_delattr__(self, name: str, old: Any, new: Any) -> None:
    "Used by Obj."
    LOGGER.debug(
        '__reaktome_delattr__(%s, %s, %s, %s)',
        repr(self), name, repr(old), repr(new),
    )
    if name.startswith('_'):
        LOGGER.debug('Skipping private/protected attr: %s', name)
        return
    deaktiv8(old, name, parent=self, source='attr')
    Changes.invoke(Change(self, name, old, None, source='attr'))


def __reaktome_setitem__(self,
                         key: Union[int, str],
                         old: Any,
                         new: Any,
                         ) -> None:
    "Used by Dict, List."
    LOGGER.debug(
        '__reaktome_setitem__(%s, %s, %s, %s)',
        repr(self), key, repr(old), repr(new),
    )
    reaktiv8(new, key, parent=self, source='item')
    deaktiv8(old, key, parent=self, source='item')
    Changes.invoke(Change(self, key, old, new, source='item'))


def __reaktome_delitem__(self,
                         key: Union[int, str],
                         old: Any,
                         new: Any,
                         ) -> None:
    "Used by Dict, List."
    LOGGER.debug(
        '__reaktome_delitem__(%s, %s, %s, %s)',
        repr(self), key, repr(old), repr(new),
    )
    deaktiv8(old, key, parent=self, source='item')
    Changes.invoke(Change(self, key, old, None, source='item'))


def __reaktome_additem__(self,
                         key: Union[int, str],
                         old: Any,
                         new: Any,
                         ) -> None:
    LOGGER.debug(
        '__reaktome_additem__(%s, %s, %s, %s)',
        repr(self), key, repr(old), repr(new),
    )
    reaktiv8(new, key, parent=self, source='set')
    Changes.invoke(Change(self, key, old, new, source='set'))


def __reaktome_discarditem__(self,
                             key: Union[int, str],
                             old: Any,
                             new: Any,
                             ) -> None:
    LOGGER.debug(
        '__reaktome_discarditem__(%s, %s, %s, %s)',
        repr(self), key, repr(old), repr(new),
    )
    deaktiv8(old, key, parent=self, source='set')
    Changes.invoke(Change(self, key, old, None, source='set'))


def __reaktome_deepcopy__(self, memo: Optional[dict] = None) -> Any:
    if callable(getattr(self, 'model_dump', None)):
        data = self.model_dump()
    else:
        data = self.dict()
    if isinstance(data, list):
        # Might be pydantic_collections subclass.
        copy = self.__class__(data)
    else:
        copy = self.__class__(**data)
    if memo is not None:
        memo[id(self)] = copy
    return copy


def reaktiv8(
    obj: Any,
    name: Optional[Union[str, int]] = None,
    parent: Any = None,
    source: str = "attr",
    seen: Optional[set] = None,
) -> None:
    """
    Activate reaktome hooks on an object instance and register it for change
    tracking.
    """
    if name is None:
        name = obj.__class__.__name__

    seen = seen if seen else set()
    if id(obj) in seen:
        LOGGER.debug('Not activating already activated object: %s', repr(obj))
        return
    seen.add(id(obj))

    if isinstance(obj, list):
        LOGGER.debug('Activating list: %s', repr(obj))
        _r.patch_list(obj, {
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))
        LOGGER.debug("Activating list's children")
        for i, child in enumerate(obj):
            reaktiv8(child, name=i, parent=obj, source='item', seen=seen)

    elif isinstance(obj, set):
        LOGGER.debug('Activating set: %s', repr(obj))
        _r.patch_set(obj, {
            "__reaktome_additem__": __reaktome_additem__,
            "__reaktome_discarditem__": __reaktome_discarditem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))
        for child in obj:
            reaktiv8(child, parent=obj, source='item', seen=seen)

    elif isinstance(obj, dict):
        LOGGER.debug('Activating dict: %s', repr(obj))
        _r.patch_dict(obj, {
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="item"))
        for key, child in obj.items():
            reaktiv8(child, name=key, parent=obj, source='item', seen=seen)

    elif hasattr(obj, "__dict__"):
        LOGGER.debug('Activating obj: %s', repr(obj))
        _r.patch_obj(obj, {
            "__reaktome_setattr__": __reaktome_setattr__,
            "__reaktome_delattr__": __reaktome_delattr__,
            "__reaktome_setitem__": __reaktome_setitem__,
            "__reaktome_delitem__": __reaktome_delitem__,
        })
        Changes.add_backref(obj, BackRef(parent, obj, name, source="attr"))
        for name, value in obj.__dict__.items():
            if not isinstance(name, int) and name.startswith('_'):
                LOGGER.debug('Skipping private/protected attr: %s', name)
                continue
            if ((BaseCollectionModel is not None and
                 isinstance(obj, BaseCollectionModel) and name == 'root')):
                continue
            reaktiv8(value, name=name, parent=obj, source='attr', seen=seen)

    else:
        LOGGER.info('Unsupported type: %s', repr(obj))

    if BaseModel is not None and isinstance(obj, BaseModel):
        obj.__dict__['__deepcopy__'] = MethodType(__reaktome_deepcopy__, obj)


def deaktiv8(
    obj: Any,
    name: Optional[Union[str, int]] = None,
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
        self.__reaktiv8__()

    def __post_init__(self) -> None:
        self.__reaktiv8__()

    def __reaktiv8__(self):
        reaktiv8(self, parent=None, name=self.__class__.__name__)


def receiver(obj: Any, pattern: str = '*', regex: bool = False) -> Callable:
    def wrapper(f):
        Changes.on(obj, f, pattern=pattern, regex=regex)
        return f
    return wrapper
