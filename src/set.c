/* src/set.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/* ---------- saved original slot pointers ---------- */
static int (*orig_tp_ass_sub)(PyObject *, PyObject *, PyObject *) = NULL;

/* ---------- dunder dispatcher ---------- */
static int
call_dunders(PyObject *self, const char *name, PyObject *key,
             PyObject *old, PyObject *new)
{
    PyObject *hooks = activation_get_hooks(self); /* newref or NULL */
    if (!hooks) {
        return 0; /* no hooks */
    }

    PyObject *dunder = PyDict_GetItemString(hooks, name);
    if (!dunder) {
        Py_DECREF(hooks);
        return 0; /* no dunder for this op */
    }

    PyObject *args = PyTuple_Pack(4, self, key ? key : Py_None,
                                  old ? old : Py_None,
                                  new ? new : Py_None);
    if (!args) {
        Py_DECREF(hooks);
        return -1;
    }

    PyObject *res = PyObject_CallObject(dunder, args);
    Py_DECREF(args);
    Py_DECREF(hooks);

    if (!res) {
        return -1; /* propagate exception */
    }
    Py_DECREF(res);
    return 0;
}

/* ---------- hook wrappers ---------- */
static int
reaktome_ass_sub(PyObject *self, PyObject *key, PyObject *value)
{
    int rc;
    PyObject *old = NULL;

    if (value) {
        /* add or replace */
        old = PySet_Contains(self, key) == 1 ? Py_NewRef(key) : NULL;
        rc = orig_tp_ass_sub(self, key, value);
        if (rc == 0) {
            rc = call_dunders(self, "__reaktome_additem__", key, old, key);
        }
    } else {
        /* discard */
        old = PySet_Contains(self, key) == 1 ? Py_NewRef(key) : NULL;
        rc = orig_tp_ass_sub(self, key, NULL);
        if (rc == 0 && old) {
            rc = call_dunders(self, "__reaktome_discarditem__", key, old, NULL);
        }
    }

    Py_XDECREF(old);
    return rc;
}

/* ---------- public patch API ---------- */
static PyObject *
py_patch_set(PyObject *Py_UNUSED(module), PyObject *args)
{
    PyObject *self;
    PyObject *hooks;
    if (!PyArg_ParseTuple(args, "OO", &self, &hooks)) {
        return NULL;
    }

    if (hooks == Py_None) {
        activation_clear(self);
        return Py_NewRef(Py_None);
    }

    if (!orig_tp_ass_sub) {
        orig_tp_ass_sub = Py_TYPE(self)->tp_as_mapping->mp_ass_subscript;
    }

    activation_set(self, hooks);
    Py_TYPE(self)->tp_as_mapping->mp_ass_subscript = reaktome_ass_sub;

    Py_RETURN_NONE;
}

/* ---------- method table ---------- */
static PyMethodDef set_methods[] = {
    {"patch_set", (PyCFunction)py_patch_set, METH_VARARGS,
     "Patch a set instance with reaktome hooks."},
    {NULL, NULL, 0, NULL}
};

/* ---------- registration ---------- */
int
init_set_patch(PyObject *m)
{
    return PyModule_AddFunctions(m, set_methods);
}
