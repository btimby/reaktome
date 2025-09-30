#include <Python.h>
#include "activation.h"

// --- Core helpers ---

static PyObject *
reaktome_set_add(PyObject *self, PyObject *arg)
{
    PyObject *res = PyObject_CallMethod(self, "add", "O", arg);
    if (!res) return NULL;

    reaktome_call_dunder(self, "__reaktome_additem__", arg, Py_None, arg);
    return res;
}

static PyObject *
reaktome_set_discard(PyObject *self, PyObject *arg)
{
    int contains = PySet_Contains(self, arg);

    PyObject *res = PyObject_CallMethod(self, "discard", "O", arg);
    if (!res) return NULL;

    if (contains == 1) {
        reaktome_call_dunder(self, "__reaktome_discarditem__", arg, arg, Py_None);
    }

    return res;
}

static PyObject *
reaktome_set_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *items = PySequence_List(self);
    if (!items) return NULL;

    PyObject *res = PyObject_CallMethod(self, "clear", NULL);
    if (!res) {
        Py_DECREF(items);
        return NULL;
    }

    Py_ssize_t n = PyList_GET_SIZE(items);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(items, i);
        Py_INCREF(item);
        reaktome_call_dunder(self, "__reaktome_discarditem__", item, item, Py_None);
        Py_DECREF(item);
    }

    Py_DECREF(items);
    return res;
}

static PyObject *
reaktome_set_update(PyObject *self, PyObject *arg)
{
    PyObject *iter = PyObject_GetIter(arg);
    if (!iter) return NULL;

    PyObject *res = PyObject_CallMethod(self, "update", "O", arg);
    if (!res) {
        Py_DECREF(iter);
        return NULL;
    }

    PyObject *item;
    while ((item = PyIter_Next(iter))) {
        reaktome_call_dunder(self, "__reaktome_additem__", item, Py_None, item);
        Py_DECREF(item);
    }
    Py_DECREF(iter);

    return res;
}

// --- MethodDefs ---

static PyMethodDef reaktome_set_add_def = {
    "add", (PyCFunction)reaktome_set_add, METH_O, NULL
};

static PyMethodDef reaktome_set_discard_def = {
    "discard", (PyCFunction)reaktome_set_discard, METH_O, NULL
};

static PyMethodDef reaktome_set_clear_def = {
    "clear", (PyCFunction)reaktome_set_clear, METH_NOARGS, NULL
};

static PyMethodDef reaktome_set_update_def = {
    "update", (PyCFunction)reaktome_set_update, METH_O, NULL
};

// --- Patch function ---

int patch_set(PyObject *dunders)
{
    if (reaktome_activate_type(&PySet_Type, dunders) < 0)
        return -1;

    if (PyDict_SetItemString(PySet_Type.tp_dict, "add",
            PyCFunction_NewEx(&reaktome_set_add_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PySet_Type.tp_dict, "discard",
            PyCFunction_NewEx(&reaktome_set_discard_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PySet_Type.tp_dict, "clear",
            PyCFunction_NewEx(&reaktome_set_clear_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PySet_Type.tp_dict, "update",
            PyCFunction_NewEx(&reaktome_set_update_def, NULL, NULL)) < 0)
        return -1;

    return 0;
}
