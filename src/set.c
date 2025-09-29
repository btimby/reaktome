// src/set.c
#include <Python.h>
#include "patch.h"

// These come from obj.c (or a shared hook file)
extern PyObject* __reaktome_additem__(PyObject* self, PyObject* item);
extern PyObject* __reaktome_discarditem__(PyObject* self, PyObject* item);

// --- Wrapped methods for set ---

static PyObject *
reaktome_set_add(PyObject *self, PyObject *args)
{
    PyObject *item;
    if (!PyArg_UnpackTuple(args, "add", 1, 1, &item))
        return NULL;

    int contains = PySet_Contains(self, item);
    if (contains == -1)
        return NULL;

    if (!contains) {
        if (!__reaktome_additem__(self, item))
            return NULL;
    }

    if (PySet_Add(self, item) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
reaktome_set_remove(PyObject *self, PyObject *args)
{
    PyObject *item;
    if (!PyArg_UnpackTuple(args, "remove", 1, 1, &item))
        return NULL;

    int contains = PySet_Contains(self, item);
    if (contains == -1)
        return NULL;

    if (!contains) {
        PyErr_SetObject(PyExc_KeyError, item);
        return NULL;
    }

    if (!__reaktome_discarditem__(self, item))
        return NULL;

    if (PySet_Discard(self, item) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
reaktome_set_discard(PyObject *self, PyObject *args)
{
    PyObject *item;
    if (!PyArg_UnpackTuple(args, "discard", 1, 1, &item))
        return NULL;

    int contains = PySet_Contains(self, item);
    if (contains == -1)
        return NULL;

    if (contains) {
        if (!__reaktome_discarditem__(self, item))
            return NULL;
    }

    if (PySet_Discard(self, item) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
reaktome_set_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *it = PyObject_GetIter(self);
    if (!it)
        return NULL;

    PyObject *item;
    while ((item = PyIter_Next(it))) {
        if (!__reaktome_discarditem__(self, item)) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
        Py_DECREF(item);
    }
    Py_DECREF(it);

    if (PySet_Clear(self) < 0)
        return NULL;

    Py_RETURN_NONE;
}

// --- Method patch table ---
static PyMethodDef reaktome_set_methods[] = {
    {"add",     (PyCFunction)reaktome_set_add,     METH_VARARGS, "Add an element to the set (with hook)"},
    {"remove",  (PyCFunction)reaktome_set_remove,  METH_VARARGS, "Remove an element from the set (with hook)"},
    {"discard", (PyCFunction)reaktome_set_discard, METH_VARARGS, "Discard an element from the set (with hook)"},
    {"clear",   (PyCFunction)reaktome_set_clear,   METH_NOARGS,  "Clear the set (with hook)"},
    {NULL, NULL, 0, NULL}
};

// --- Patch function exported to Python ---
int
patch_set_type(void)
{
    PyObject *set_type = (PyObject *)&PySet_Type;
    return patch_methods(set_type, reaktome_set_methods);
}
