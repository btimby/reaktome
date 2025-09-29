#include <Python.h>
#include "structmember.h"

// --- Core helpers ---

static PyObject *
reaktome_list_append(PyObject *self, PyObject *arg)
{
    // call original list.append(self, arg)
    PyObject *res = PyObject_CallMethod(self, "append", "O", arg);
    if (!res) return NULL;

    // notify hook: __reaktome_setitem__(self, PyLong_FromSsize_t(index), NULL, arg)
    Py_ssize_t index = PyList_GET_SIZE(self) - 1;
    PyObject *key = PyLong_FromSsize_t(index);
    if (!key) {
        Py_DECREF(res);
        return NULL;
    }

    PyObject *hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
    if (hook) {
        PyObject *dummy = PyObject_CallFunctionObjArgs(hook, self, key, Py_None, arg, NULL);
        Py_XDECREF(dummy);
        Py_DECREF(hook);
    }
    Py_DECREF(key);

    return res;
}

static PyObject *
reaktome_list_extend(PyObject *self, PyObject *arg)
{
    PyObject *res = PyObject_CallMethod(self, "extend", "O", arg);
    if (!res) return NULL;

    // Iterate items in arg
    PyObject *iter = PyObject_GetIter(arg);
    if (!iter) {
        Py_DECREF(res);
        return NULL;
    }
    Py_ssize_t start = PyList_GET_SIZE(self) - PyObject_Length(arg);

    PyObject *item;
    Py_ssize_t i = 0;
    while ((item = PyIter_Next(iter))) {
        PyObject *key = PyLong_FromSsize_t(start + i);
        if (key) {
            PyObject *hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
            if (hook) {
                PyObject *dummy = PyObject_CallFunctionObjArgs(hook, self, key, Py_None, item, NULL);
                Py_XDECREF(dummy);
                Py_DECREF(hook);
            }
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
        PyObject *hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
        if (hook) {
            PyObject *dummy = PyObject_CallFunctionObjArgs(hook, self, key, Py_None, value, NULL);
            Py_XDECREF(dummy);
            Py_DECREF(hook);
        }
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

    // call original pop
    PyObject *res = PyObject_CallMethod(self, "pop", "n", index);
    if (!res) return NULL;

    if (index < 0) {
        index = PyList_GET_SIZE(self);
    }

    PyObject *key = PyLong_FromSsize_t(index);
    if (key) {
        PyObject *hook = PyObject_GetAttrString(self, "__reaktome_delitem__");
        if (hook) {
            PyObject *dummy = PyObject_CallFunctionObjArgs(hook, self, key, res, NULL);
            Py_XDECREF(dummy);
            Py_DECREF(hook);
        }
        Py_DECREF(key);
    }

    return res;
}

// --- MethodDefs for patched list methods ---

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

// --- Patch function ---

int patch_list(void)
{
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

    return 0;
}
