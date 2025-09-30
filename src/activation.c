/* src/activation.c
   Side-table helpers for Reaktome.
   Keyed by PyLong_FromVoidPtr((void*)obj) to avoid depending on the object's
   hashability.  No module init here; this file provides pure C helpers.
*/

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"

/* activation_map: dict mapping PyLong(id(obj)) -> dict(hookname->callable)
   Keys are PyLong objects created from the pointer value. */
static PyObject *activation_map = NULL;

/* Ensure activation_map exists. Return 0 on success, -1 on error (exception set). */
static int
ensure_activation_map(void)
{
    if (activation_map)
        return 0;
    activation_map = PyDict_New();
    if (!activation_map) return -1;
    return 0;
}

/* activation_merge:
   - obj must be a non-NULL PyObject* (either an instance or a type cast to PyObject*).
   - dunders: dict to merge, or Py_None to clear the entry for obj.
*/
int
activation_merge(PyObject *obj, PyObject *dunders)
{
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "activation_merge: obj must not be NULL");
        return -1;
    }

    if (ensure_activation_map() < 0) return -1;

    PyObject *key = PyLong_FromVoidPtr((void *)obj);
    if (!key) return -1;

    if (dunders == Py_None) {
        /* Clear the entry (ignore if absent). */
        if (PyDict_DelItem(activation_map, key) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(key);
        return 0;
    }

    if (!PyDict_Check(dunders)) {
        Py_DECREF(key);
        PyErr_SetString(PyExc_TypeError, "activation_merge: dunders must be a dict or None");
        return -1;
    }

    /* If an entry already exists, update (merge) into it; otherwise insert a copy. */
    PyObject *existing = PyDict_GetItem(activation_map, key); /* borrowed */
    if (existing) {
        /* existing is a dict; update it in-place */
        if (PyDict_Update(existing, dunders) < 0) {
            Py_DECREF(key);
            return -1;
        }
        Py_DECREF(key);
        return 0;
    } else {
        PyObject *copy = PyDict_Copy(dunders); /* newref */
        if (!copy) { Py_DECREF(key); return -1; }
        if (PyDict_SetItem(activation_map, key, copy) < 0) {
            Py_DECREF(copy);
            Py_DECREF(key);
            return -1;
        }
        Py_DECREF(copy);
        Py_DECREF(key);
        return 0;
    }
}

/* Return NEW reference to hooks dict for obj (instance), or NULL if none.
   Try instance key first, then fall back to type(obj). No exception set if none. */
PyObject *
activation_get_hooks(PyObject *obj)
{
    if (!obj) return NULL;
    if (!activation_map) return NULL;

    /* instance key */
    PyObject *key = PyLong_FromVoidPtr((void *)obj);
    if (!key) return NULL;

    PyObject *existing = PyDict_GetItem(activation_map, key); /* borrowed */
    if (existing) {
        Py_INCREF(existing);
        Py_DECREF(key);
        return existing; /* newref */
    }
    Py_DECREF(key);

    /* try type-level hooks */
    PyObject *type_key = PyLong_FromVoidPtr((void *)Py_TYPE(obj));
    if (!type_key) return NULL;

    PyObject *type_existing = PyDict_GetItem(activation_map, type_key); /* borrowed */
    if (!type_existing) { Py_DECREF(type_key); return NULL; }

    Py_INCREF(type_existing);
    Py_DECREF(type_key);
    return type_existing;
}

/* Treat the type pointer as a PyObject* and forward to activation_merge. */
int
reaktome_activate_type(PyTypeObject *type_or_obj, PyObject *dunders)
{
    PyObject *obj = (PyObject *)type_or_obj;
    return activation_merge(obj, dunders);
}

/* Clear registrations for a type (simple wrapper). */
int
activation_clear_type(PyTypeObject *type)
{
    if (!type) {
        PyErr_SetString(PyExc_TypeError, "activation_clear_type: type must not be NULL");
        return -1;
    }
    return activation_merge((PyObject *)type, Py_None);
}

/* Replace/set the hooks dict for a type (non-merge semantic).
   If dunders is Py_None, clear. dunders must be dict otherwise TypeError. */
int
activation_set_type(PyTypeObject *type, PyObject *dunders)
{
    if (!type) {
        PyErr_SetString(PyExc_TypeError, "activation_set_type: type must not be NULL");
        return -1;
    }
    if (dunders == Py_None) {
        return activation_clear_type(type);
    }
    if (!PyDict_Check(dunders)) {
        PyErr_SetString(PyExc_TypeError, "activation_set_type: dunders must be a dict or None");
        return -1;
    }

    if (ensure_activation_map() < 0) return -1;

    PyObject *key = PyLong_FromVoidPtr((void *)type);
    if (!key) return -1;

    PyObject *copy = PyDict_Copy(dunders);
    if (!copy) { Py_DECREF(key); return -1; }

    if (PyDict_SetItem(activation_map, key, copy) < 0) {
        Py_DECREF(copy);
        Py_DECREF(key);
        return -1;
    }
    Py_DECREF(copy);
    Py_DECREF(key);
    return 0;
}

/* Call dunder if present for this object.
   Returns 0 if no hook present or on successful call; -1 on exception
   (Python exception is left set). */
int
reaktome_call_dunder(PyObject *self,
                     const char *name,
                     PyObject *key,
                     PyObject *old,
                     PyObject *newv)
{
    if (!self || !name) {
        PyErr_SetString(PyExc_TypeError, "reaktome_call_dunder: invalid arguments");
        return -1;
    }

    PyObject *hooks = activation_get_hooks(self); /* newref or NULL */
    if (!hooks) {
        /* no hooks -> not an error */
        return 0;
    }

    PyObject *callable = PyDict_GetItemString(hooks, name); /* borrowed */
    if (!callable) {
        Py_DECREF(hooks);
        return 0;
    }

    /* Build args: (self, key_or_None, old_or_None, new_or_None) */
    PyObject *k = key ? key : Py_None;
    PyObject *o = old ? old : Py_None;
    PyObject *n = newv ? newv : Py_None;
    Py_INCREF(k); Py_INCREF(o); Py_INCREF(n);

    PyObject *res = PyObject_CallFunctionObjArgs(callable, self, k, o, n, NULL);

    Py_DECREF(k); Py_DECREF(o); Py_DECREF(n);
    Py_DECREF(hooks);

    if (!res) {
        /* propagate exception */
        return -1;
    }
    Py_DECREF(res);
    return 0;
}
