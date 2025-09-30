// src/activation.c
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/*
activation.c

Provides a per-instance registry keyed by id(obj) (PyLong_FromVoidPtr(obj))
Stores a dict of dunder -> callable for each activated instance.

API:
  int activation_merge(PyObject *obj, PyObject *dunders);
    - dunders: dict -> merge keys into registry[obj]
    - dunders: Py_None -> clear registry[obj]
    returns 0 on success, -1 on error (and sets exception)

  PyObject *activation_get_hooks(PyObject *obj);
    - returns a NEW REFERENCE to the hooks dict for obj, or NULL if none (no exception)
*/

static PyObject *activation_map = NULL; /* dict: key=id(obj) -> dict(dunder_str -> callable) */

static int
ensure_activation_map(void)
{
    if (activation_map)
        return 0;
    activation_map = PyDict_New();
    if (!activation_map) return -1;
    return 0;
}

/* Merge dunders dict into registry entry for obj, or clear if dunders is Py_None.
   Returns 0 on success, -1 on error (with Python exception). */
int
activation_merge(PyObject *obj, PyObject *dunders)
{
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "obj must not be NULL");
        return -1;
    }

    if (ensure_activation_map() < 0) return -1;

    PyObject *key = PyLong_FromVoidPtr((void *)obj);
    if (!key) return -1;

    if (dunders == Py_None) {
        /* clear entry (ignore KeyError) */
        if (PyDict_DelItem(activation_map, key) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(key);
        return 0;
    }

    if (!PyDict_Check(dunders)) {
        Py_DECREF(key);
        PyErr_SetString(PyExc_TypeError, "dunders must be a dict or None");
        return -1;
    }

    /* See if an entry exists */
    PyObject *existing = PyDict_GetItem(activation_map, key); /* borrowed */
    if (existing) {
        /* existing is a dict: update it with dunders (merge) */
        if (PyDict_Update(existing, dunders) < 0) {
            Py_DECREF(key);
            return -1;
        }
        Py_DECREF(key);
        return 0;
    } else {
        /* No existing entry: insert a shallow copy (we can just insert the dict object;
           PyDict_SetItem will INCREF the value). We want the stored value to be a dict
           that we own; to avoid callers later mutating the same dict unexpectedly,
           it's reasonable to create a shallow copy here. */
        PyObject *copy = PyDict_Copy(dunders);
        if (!copy) {
            Py_DECREF(key);
            return -1;
        }
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

/* Return a NEW reference to the hooks dict for obj, or NULL if none.
   Does NOT set a Python exception when none exists (convenient for callers). */
PyObject *
activation_get_hooks(PyObject *obj)
{
    if (!obj) return NULL;
    if (!activation_map) return NULL;

    PyObject *key = PyLong_FromVoidPtr((void *)obj);
    if (!key) return NULL;

    PyObject *existing = PyDict_GetItem(activation_map, key); /* borrowed */
    if (!existing) {
        Py_DECREF(key);
        return NULL;
    }

    Py_INCREF(existing);
    Py_DECREF(key);
    return existing;
}

/* Python wrapper: patch/activate function
   Usage from Python: _reaktome._activate(obj, dunders)
   - dunders: dict to merge; None to clear.
*/
static PyObject *
py_activation_patch(PyObject *self, PyObject *args)
{
    PyObject *obj;
    PyObject *dunders;

    if (!PyArg_ParseTuple(args, "OO:activation_patch", &obj, &dunders)) {
        return NULL;
    }

    if (activation_merge(obj, dunders) < 0) return NULL;
    Py_RETURN_NONE;
}

/* Module-level methods (optional; you can instead call activation_merge directly
   from other C modules if you link them together). */
static PyMethodDef activation_methods[] = {
    {"_activation_patch", (PyCFunction)py_activation_patch, METH_VARARGS,
     "Activate an object with a dict of dunders, or clear if dunders is None."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef activation_module = {
    PyModuleDef_HEAD_INIT,
    "_reaktome_activation",
    "Activation registry for reaktome (internal).",
    -1,
    activation_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit__reaktome_activation(void)
{
    PyObject *m = PyModule_Create(&activation_module);
    if (!m) return NULL;
    /* ensure activation_map is initialized lazily */
    return m;
}

/* Provide C-level symbols for other translation units to call.
   If linking into a single extension, these symbol names will be available. */
