# Reaktome

**Reaktome** is a CPython extension that installs *advisory hooks* on Python’s built-in containers (`list`, `dict`, `set`).  
It allows you to observe and react to mutations without subclassing or replacing types.

---

## Features

- Patch *individual instances* of built-in containers.
- Hook into mutations (`append`, `pop`, `__setitem__`, etc.).
- Hooks are called **after** the mutation (advisory only).
- Works per-instance via an internal side-table.

---

## Example

```python
import _reaktome

def on_additem(self, key, old, new):
    print(f"{self} added {new}")

lst = []
_reaktome.patch_list(lst, {"__reaktome_additem__": on_additem})

lst.append(42)
# Output: [] added 42
