#ifndef REAKTOME_ACTIVATION_H
#define REAKTOME_ACTIVATION_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Merge dunders dict into registry entry for obj (instance or type).
   If dunders is Py_None the entry for obj is cleared.
   Returns 0 on success, -1 on error (with Python exception set). */
int activation_merge(PyObject *obj, PyObject *dunders);

/* Return a NEW reference to the hooks dict for obj (instance or type),
   or NULL if none exists. Does NOT set a Python exception when there
   are no hooks (convenient for callers). */
PyObject *activation_get_hooks(PyObject *obj);

/* Convenience wrapper that treats the passed in pointer as a PyObject*:
   useful for type activation (calls activation_merge internally). */
int reaktome_activate_type(PyTypeObject *type_or_obj, PyObject *dunders);

/* Call the named hook for `self` (if present in side-table).
   name: C string (e.g. "__reaktome_additem__").
   key/old/newv follow the design.
   Returns 0 on success or when no hook; -1 on exception (Python exception set). */
int reaktome_call_dunder(PyObject *self,
                         const char *name,
                         PyObject *key,
                         PyObject *old,
                         PyObject *newv);

/* Optional helpers for type-level activation; simple implementations are allowed. */
int activation_clear_type(PyTypeObject *type);
int activation_set_type(PyTypeObject *type, PyObject *dunders);

// Add these so other .c files (like list.c) can use them:
PyObject *reaktome_install_hooks(PyObject *obj, PyObject *hooks);
PyObject *reaktome_get_hooks(PyObject *obj);
int reaktome_call_dunder(PyObject *self, const char *name,
                         PyObject *key, PyObject *old, PyObject *newv);

#ifdef __cplusplus
}
#endif

#endif /* REAKTOME_ACTIVATION_H */
