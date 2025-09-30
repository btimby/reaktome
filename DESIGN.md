# Reaktome — Canonical Design

This document is the single source of truth for the **Reaktome** C extension design.  
All implementation and future changes must follow this document exactly to avoid regressions and the "fix A break B" loop.

---

## Goals

Provide *per-instance* advisory hooks for built-in container mutations by:
- Installing C-level trampolines into the builtin type (once per type), and
- Storing per-instance hook callables in a side-table keyed by the instance.

Trampolines perform the actual mutation first and then call hooks (advisory).

---

## Components & Responsibilities

### `activation.c` / `activation.h` — side-table helpers (C-only)
- **No Python module initialization here.** This file provides C helpers used by trampoline code.
- **Public API (exact names / prototypes):**

    int activation_merge(PyObject *obj, PyObject *dunders);
    PyObject *activation_get_hooks(PyObject *obj); /* newref or NULL; no exception if none */
    int reaktome_activate_type(PyTypeObject *type_or_obj, PyObject *dunders);
    int reaktome_call_dunder(PyObject *self,
                             const char *name,
                             PyObject *key,
                             PyObject *old,
                             PyObject *newv);
    int activation_clear_type(PyTypeObject *type);   /* optional no-op allowed */
    int activation_set_type(PyTypeObject *type, PyObject *dunders); /* optional */

- **Semantics:**
  - `activation_merge(obj, dunders)` — merge hooks dict into side-table for `obj`.  
  - `activation_get_hooks(obj)` — return new ref to hooks dict or `NULL` if none.  
  - `reaktome_call_dunder(self, name, key, old, newv)` — invoke hook if present.  
  - `activation_clear_type` / `activation_set_type` — optional type-level helpers.

---

### `list.c`, `dict.c`, `set.c`, `obj.c` — trampolines and patchers
Each container module:
- Implements static **trampolines** for slots and methods that perform mutations.
- Exposes a Python-visible function `patch_<type>(instance, dunders)` via `py_patch_<type>`.
- Exposes `int reaktome_patch_<type>(PyObject *m)` which adds the Python-callable `patch_<type>` into module `m`. Called from `reaktome.c`.

#### `py_patch_<type>(instance, dunders)` semantics
- Verify `instance` is the correct type.  
- Ensure trampolines installed once per type (static guard).  
- Call `activation_merge(instance, dunders)`.

---

### `reaktome.c` — module init
- Create `_reaktome` extension module.  
- Call `reaktome_patch_list(m)`, `reaktome_patch_dict(m)`, etc.  
- Do not patch types here.  
- If needed, expose debug helpers wrapping `activation_*`.

---

## Trampoline Rules
- Install once per type.  
- Methods: wrap with `PyCFunction_NewEx` and insert into `tp_dict`.  
- Slots: save original function pointer, replace with trampoline.  
- Trampolines:
  - Call original first.  
  - Then call hook(s) via `reaktome_call_dunder`.  
  - Swallow exceptions (`PyErr_Clear`).  

---

## Testing Checklist
1. Import `_reaktome`.  
2. Call `_reaktome.patch_list(lst, {"__reaktome_additem__": hook})`.  
3. `lst.append(1)` → list updated + hook called once.  
4. `lst.extend([2,3])` → two hook calls.  
5. `lst[0] = 9` → hook called with old=1, new=9.  
6. `lst.pop()` → returned value.  
