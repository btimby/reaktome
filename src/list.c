#include <Python.h>
#include "activation.h"

// --- Core helpers ---

static PyObject *
reaktome_list_append(PyObject *self, PyObject *arg)
{
    Py_ssize_t old_size = PyList_GET_SIZE(self);

    PyObject *res = PyObject_CallMethod(self, "append", "O", arg);
    if (!res) return NULL;

    PyObject *key = PyLong_FromSsize_t(old_size);
    if (key) {
        reaktome_call_dunder(self, "__reaktome_setitem__", key, Py_None, arg);
        Py_DECREF(key);
    }

    return res;
}

static PyObject *
reaktome_list_extend(PyObject *self, PyObject *arg)
{
    Py_ssize_t start = PyList_GET_SIZE(self);

    PyObject *res = PyObject_CallMethod(self, "extend", "O", arg);
    if (!res) return NULL;

    PyObject *iter = PyObject_GetIter(arg);
    if (!iter) return res;

    PyObject *item;
    Py_ssize_t i = 0;
    while ((item = PyIter_Next(iter))) {
        PyObject *key = PyLong_FromSsize_t(start + i);
        if (key) {
            reaktome_call_dunder(self, "__reaktome_setitem__", key, Py_None, item);
            Py_DECREF(key);
        }
        Py_DECREF(item);
        i++;
    }
    Py_DECREF(iter);

    return res;
}

static PyObject *
reaktome_list_insert(PyObject *self, PyObject *args)
{
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "nO", &index, &value)) {
        return NULL;
    }

    PyObject *res = PyObject_CallMethod(self, "insert", "nO", index, value);
    if (!res) return NULL;

    PyObject *key = PyLong_FromSsize_t(index);
    if (key) {
        reaktome_call_dunder(self, "__reaktome_setitem__", key, Py_None, value);
        Py_DECREF(key);
    }

    return res;
}

static PyObject *
reaktome_list_pop(PyObject *self, PyObject *args)
{
    Py_ssize_t index = -1;
    if (!PyArg_ParseTuple(args, "|n", &index)) {
        return NULL;
    }

    PyObject *res = PyObject_CallMethod(self, "pop", "n", index);
    if (!res) return NULL;

    if (index < 0)
        index = PyList_GET_SIZE(self);

    PyObject *key = PyLong_FromSsize_t(index);
    if (key) {
        reaktome_call_dunder(self, "__reaktome_delitem__", key, res, Py_None);
        Py_DECREF(key);
    }

    return res;
}

static PyObject *
reaktome_list_remove(PyObject *self, PyObject *arg)
{
    // find index first
    Py_ssize_t index = PySequence_Index(self, arg);
    if (index < 0) return NULL;

    PyObject *old = PyList_GetItem(self, index);
    if (!old) return NULL;
    Py_INCREF(old);

    PyObject *res = PyObject_CallMethod(self, "remove", "O", arg);
    if (!res) {
        Py_DECREF(old);
        return NULL;
    }

    PyObject *key = PyLong_FromSsize_t(index);
    if (key) {
        reaktome_call_dunder(self, "__reaktome_delitem__", key, old, Py_None);
        Py_DECREF(key);
    }

    Py_DECREF(old);
    return res;
}

static PyObject *
reaktome_list_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    Py_ssize_t n = PyList_GET_SIZE(self);

    // collect old items
    PyObject *items = PySequence_List(self);
    if (!items) return NULL;

    PyObject *res = PyObject_CallMethod(self, "clear", NULL);
    if (!res) {
        Py_DECREF(items);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *key = PyLong_FromSsize_t(i);
        PyObject *old = PyList_GET_ITEM(items, i);
        if (key && old) {
            Py_INCREF(old);
            reaktome_call_dunder(self, "__reaktome_delitem__", key, old, Py_None);
            Py_DECREF(old);
            Py_DECREF(key);
        }
    }
    Py_DECREF(items);

    return res;
}

// --- MethodDefs ---

static PyMethodDef reaktome_list_append_def = {
    "append", (PyCFunction)reaktome_list_append, METH_O, NULL
};

static PyMethodDef reaktome_list_extend_def = {
    "extend", (PyCFunction)reaktome_list_extend, METH_O, NULL
};

static PyMethodDef reaktome_list_insert_def = {
    "insert", (PyCFunction)reaktome_list_insert, METH_VARARGS, NULL
};

static PyMethodDef reaktome_list_pop_def = {
    "pop", (PyCFunction)reaktome_list_pop, METH_VARARGS, NULL
};

static PyMethodDef reaktome_list_remove_def = {
    "remove", (PyCFunction)reaktome_list_remove, METH_O, NULL
};

static PyMethodDef reaktome_list_clear_def = {
    "clear", (PyCFunction)reaktome_list_clear, METH_NOARGS, NULL
};

// --- Patch function ---

int patch_list(PyObject *dunders)
{
    if (reaktome_activate_type(&PyList_Type, dunders) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "append",
            PyCFunction_NewEx(&reaktome_list_append_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "extend",
            PyCFunction_NewEx(&reaktome_list_extend_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "insert",
            PyCFunction_NewEx(&reaktome_list_insert_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "pop",
            PyCFunction_NewEx(&reaktome_list_pop_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "remove",
            PyCFunction_NewEx(&reaktome_list_remove_def, NULL, NULL)) < 0)
        return -1;

    if (PyDict_SetItemString(PyList_Type.tp_dict, "clear",
            PyCFunction_NewEx(&reaktome_list_clear_def, NULL, NULL)) < 0)
        return -1;

    return 0;
}
