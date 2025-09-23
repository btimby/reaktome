import logging

from typing import Any, Callable, Union
from collections import defaultdict
from weakref import WeakMethod
from functools import wraps


LOGGER = logging.getLogger(__name__)
LOGGER.addHandler(logging.NullHandler())

NOWRAP = (int, float, str, bool, bytes, tuple, frozenset, type(None))
KLASSES = {}
ON_CHANGE = Callable[[Any, Any, Any], None]
SENTINAL = object()


def maybe_make_klass(value, attrs):
    "Create base classes to override __setitem__ and cache them"
    base = value.__class__
    name = f'_Reactome_{base.__name__.title()}'
    attrs.update({
        '_is_reaktome': True,
    })
    if base not in KLASSES:
        KLASSES[base] = type(name, (base, _ReaktomeBase), attrs)
    return KLASSES[base]


class _ReaktomeBase:
    pass


def reaktiv8(value: Any,
             on_change: ON_CHANGE,
             path: str = '',
             ) -> Any:
    """Wrap dicts/lists with reactive containers recursively"""
    # if hasattr(value, '_is_reaktome'):
    #     return value

    if isinstance(value, (ReaktomeDict, ReaktomeList, _ReaktomeBase)):
        return value

    if isinstance(value, NOWRAP):
        return value

    def _on_change(subpath, old, val):
        full_key = f"{path}{subpath}" if path and subpath else subpath or path
        on_change(full_key, old, val)

    if isinstance(value, dict):
        for k, v in value.items():
            subpath = f'{path}[{k}]'
            value[k] = reaktiv8(v, on_change=_on_change, path=subpath)
        value = ReaktomeDict(value, _on_change)

    elif isinstance(value, list):
        for i, item in enumerate(value):
            subpath = f'{path}[{i}]'
            value[i] = reaktiv8(item, on_change=_on_change, path=subpath)
        value = ReaktomeList(value, _on_change)

    elif hasattr(value, '__dict__'):
        for k, v in value.__dict__.items():
            if not k.startswith('_'):
                v = reaktiv8(v, on_change=_on_change, path=f'[{k}]')
            value.__dict__[k] = v

        value.__class__ = maybe_make_klass(value, {
            '__setattr__': __setattr__,
            'on_change': lambda self, path, old, new: _on_change(path, old, new),
        })

    return value


def __setattr__(self, name, value):
    has_changed = False
    old = getattr(self, name, None)
    if not name.startswith('_') and not callable(value):
        value = reaktiv8(value, on_change=self.on_change, path=f'.{name}')
        has_changed = True

    if ((isinstance(self, (ReaktomeDict, ReaktomeList))
         and hasattr(self, '_target'))):
        self._target.__setattr__(name, value)

    else:
        object.__setattr__(self, name, value)

    if has_changed:
        self.on_change(f'.{name}', old, value)


def __setitem__(self, key, value):
    if (isinstance(key, str) and key.startswith('_')) or callable(value):
        self._target[key] = value
        return

    value = reaktiv8(value, on_change=self.on_change, path=f'[{key}]')
    try:
        old = self[key]

    except KeyError:
        old = None

    self._target.__setitem__(key, value)
    self.on_change(f'[{key}]', old, value)


def __delitem__(self, key):
    old = self._target[key]
    self._target.__delitem__(key)
    self.on_change(f'[{key}]', old, None)


def pop(self, i=-1, default=SENTINAL):
    try:
        old = value = self._target.pop(i)

    except KeyError:
        old = None
        if default is SENTINAL:
            raise
        value = default

    if old is not None:
        self.on_change(f'[{i}]', old, None)
    return value


def remove(self, value):
    i = self._target.index(value)
    old = self._target[i]
    self._target.remove(value)
    self.on_change(f'[{i}]', old, None)


def append(self, value):
    i = len(self._target)
    value = reaktiv8(value, on_change=self.on_change, path=f'[{i}]')
    self._target.append(value)
    self.on_change(f'[{i}]', None, value)


def insert(self, i, value):
    value = reaktiv8(value, on_change=self.on_change, path=f'[{i}]')
    self._target.insert(i, value)
    self.on_change(f'[{i}]', None, value)


def extend(self, iterable):
    i = len(self._target)
    iterable = [
        reaktiv8(value, on_change=self.on_change, path=f'[{i + ii}]')
        for ii, value in enumerate(iterable)
    ]
    self._target.extend(iterable)
    self.on_change(f'[{i}:{i+len(iterable)}]', None, iterable)


def popitem(self):
    k, v = self._target.popitem()
    self.on_change(f'[{k}]', v, None)
    return k, v


def setdefault(self, key, default=None):
    old = self.get(key)
    self._target.setdefault(key, default)
    self.on_change(f'[{key}]', old, default)
    return default


def update(self, *args, **kwargs):
    keys, old, new = [], [], []
    for arg in args:
        if callable(getattr(arg, 'items', None)):
            arg = arg.items()
        for k, v in arg:
            keys.append(k)
            old.append(self.get(k))
            new.append(reaktiv8(v, on_change=self.on_change, path=f'[{k}]'))
    for k, v in kwargs.items():
        keys.append(k)
        old.append(self.get(k))
        new.append(reaktiv8(v, on_change=self.on_change, path=f'[{k}]'))
    self._target.update(zip(keys, new))
    self.on_change(f'[{",".join(keys)}]', old, new)


def __getattr__(self, *args) -> Any:
    try:
        return getattr(self.__dict__['_target'], *args)

    except KeyError:
        raise AttributeError(args[0])


def __getitem__(self, name):
    return self._target[name]


def __len__(self):
    return len(self._target)


class ReaktomeList:
    _is_reaktome = True
    __setattr__ = __setattr__
    __setitem__ = __setitem__
    __delitem__ = __delitem__
    __getattr__ = __getattr__
    __getitem__ = __getitem__
    __len__ = __len__
    pop = pop
    append = append
    remove = remove
    insert = insert
    extend = extend

    def __init__(self, value, on_change):
        self.on_change = on_change
        self._target = value
        super().__init__()


class ReaktomeDict:
    _is_reaktome = True
    __setattr__ = __setattr__
    __setitem__ = __setitem__
    __delitem__ = __delitem__
    __getattr__ = __getattr__
    __getitem__ = __getitem__
    __len__ = __len__
    pop = pop
    popitem = popitem
    setdefault = setdefault
    update = update

    def __init__(self, value, on_change):
        self.on_change = on_change
        self._target = value
        super().__init__()


class ReaktomeMeta(type):
    def __new__(mcs, name, bases, namespace):
        if '__setattr__' not in namespace:
            namespace['__setattr__'] = __setattr__
        cls = super().__new__(mcs, name, bases, namespace)
        return cls


class DeadWatcher(Exception):
    "Raised when a watcher's weakref has been collected."
    pass


class ReaktomeWatcher:
    def __init__(self, cb: ON_CHANGE) -> None:
        self.cb: Union[WeakMethod[ON_CHANGE], ON_CHANGE]
        if getattr(cb, '__self__', None) is not None:
            # NOTE: cb is a method, use a Weakref
            self.cb = WeakMethod(cb)
        else:
            self.cb = cb

    def __call__(self, name: str, old: Any, new: Any) -> Any:
        if isinstance(self.cb, WeakMethod):
            cb = self.cb()
            if cb is None:
                raise DeadWatcher()

        else:
            cb = self.cb

        return cb(name, old, new)


def ensure_watchers(f):
    @wraps(f)
    def inner(self, *args, **kwargs):
        if not hasattr(self, '_watchers'):
            self.__dict__['_watchers'] = defaultdict(set)
        return f(self, *args, **kwargs)
    return inner


class ReaktomeWatch:
    @ensure_watchers
    def on(self,
           path: str,
           cb: ON_CHANGE,
           ) -> tuple[str, ReaktomeWatcher]:
        watcher = ReaktomeWatcher(cb)
        self._watchers[path].add(watcher)  # type: ignore
        return (path, watcher)

    @ensure_watchers
    def off(self, path_cb: tuple[str, ReaktomeWatcher]) -> None:
        try:
            path, watcher = path_cb

        except TypeError:
            raise ValueError('Invalid handle: %s', path_cb)

        try:
            self._watchers[path].discard(watcher)  # type: ignore

        except ValueError:
            pass

    @ensure_watchers
    def on_change(self, path, old=None, new=None):
        """Hook to respond to all attribute/item changes"""
        LOGGER.debug(f"⚡ Change → {path}: {old} → {new}")
        dead = []

        watchers = [
            *self._watchers.get('*', []),
            *self._watchers.get(path, []),
        ]

        for watcher in watchers:
            try:
                watcher(path, old, new)

            except DeadWatcher:
                dead.append((path, watcher))

        for (path, watcher) in dead:
            self._watchers[path].discard(watcher)


class Reaktome(ReaktomeWatch, metaclass=ReaktomeMeta):
    pass
