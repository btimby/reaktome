#include <Python.h>
#include "structmember.h"

// Forward declarations of Python hook symbols
static PyObject *__reaktome_setitem__ = NULL;
static PyObject *__reaktome_delitem__ = NULL;

// Our wrapper for dict assignment
static int
reaktome_dict_ass_sub(PyObject *self, PyObject *key, PyObject *value)
{
    PyObject *old = NULL;
    int res = -1;

    if (PyDict_CheckExact(self)) {
        old = PyDict_GetItemWithError(self, key);
        if (old) {
            Py_INCREF(old);
        } else if (PyErr_Occurred()) {
            return -1;
        }
    }

    // Normal assignment
    res = PyDict_Type.tp_as_mapping->mp_ass_subscript(self, key, value);
    if (res < 0) {
        Py_XDECREF(old);
        return res;
    }

    // Call hook if available
    if (__reaktome_setitem__) {
        PyObject *r = PyObject_CallFunctionObjArgs(
            __reaktome_setitem__, self, key,
            old ? old : Py_None,
            value ? value : Py_None,
            NULL);
        if (!r) {
            PyErr_WriteUnraisable(__reaktome_setitem__);
        } else {
            Py_DECREF(r);
        }
    }

    Py_XDECREF(old);
    return res;
}

// Our wrapper for dict deletion
static int
reaktome_dict_del_sub(PyObject *self, PyObject *key)
{
    PyObject *old = NULL;
    int res = -1;

    if (PyDict_CheckExact(self)) {
        old = PyDict_GetItemWithError(self, key);
        if (old) {
            Py_INCREF(old);
        } else if (PyErr_Occurred()) {
            return -1;
        }
    }

    res = PyDict_Type.tp_as_mapping->mp_ass_subscript(self, key, NULL);
    if (res < 0) {
        Py_XDECREF(old);
        return res;
    }

    if (__reaktome_delitem__) {
        PyObject *r = PyObject_CallFunctionObjArgs(
            __reaktome_delitem__, self, key,
            old ? old : Py_None,
            NULL);
        if (!r) {
            PyErr_WriteUnraisable(__reaktome_delitem__);
        } else {
            Py_DECREF(r);
        }
    }

    Py_XDECREF(old);
    return res;
}

// Patch function: replace dict methods
static PyMappingMethods reaktome_dict_as_mapping;

static int
patch_dict(void)
{
    static int patched = 0;
    if (patched) return 0;
    patched = 1;

    // Copy existing mapping table
    reaktome_dict_as_mapping = *PyDict_Type.tp_as_mapping;

    // Replace with our hooks
    reaktome_dict_as_mapping.mp_ass_subscript = reaktome_dict_ass_sub;
    PyDict_Type.tp_as_mapping = &reaktome_dict_as_mapping;

    return 0;
}

// Module init
static PyMethodDef dict_methods[] = {
    {"patch_dict", (PyCFunction)patch_dict, METH_NOARGS, "Patch dict hooks"},
    {NULL, NULL, 0, NULL}
};

static int
dict_exec(PyObject *m)
{
    __reaktome_setitem__ = PyObject_GetAttrString(m, "__reaktome_setitem__");
    __reaktome_delitem__ = PyObject_GetAttrString(m, "__reaktome_delitem__");
    if (!__reaktome_setitem__ || !__reaktome_delitem__)
        return -1;
    return 0;
}

static struct PyModuleDef_Slot dict_slots[] = {
    {Py_mod_exec, dict_exec},
    {0, NULL}
};

static struct PyModuleDef dict_module = {
    PyModuleDef_HEAD_INIT,
    "dict",
    NULL,
    0,
    dict_methods,
    dict_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__reaktome_dict(void)
{
    return PyModuleDef_Init(&dict_module);
}
