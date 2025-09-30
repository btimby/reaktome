// src/dict.c
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* from activation.c */
extern PyObject *activation_get_hooks(PyObject *obj);

static void
_call_setitem_hook(PyObject *hooks, PyObject *self,
                   PyObject *key, PyObject *oldv, PyObject *newv)
{
    if (!hooks) return;
    PyObject *hook = PyDict_GetItemString(hooks, "__reaktome_setitem__");
    if (!hook) return;
    PyObject *res = PyObject_CallFunctionObjArgs(hook, self, key,
                                                 oldv ? oldv : Py_None, newv, NULL);
    Py_XDECREF(res);
}

static void
_call_delitem_hook(PyObject *hooks, PyObject *self,
                   PyObject *key, PyObject *oldv)
{
    if (!hooks) return;
    PyObject *hook = PyDict_GetItemString(hooks, "__reaktome_delitem__");
    if (!hook) return;
    PyObject *res = PyObject_CallFunctionObjArgs(hook, self, key,
                                                 oldv ? oldv : Py_None, NULL);
    Py_XDECREF(res);
}

static PyObject *
reaktome_dict_setitem(PyObject *self, PyObject *args)
{
    PyObject *key, *value;
    if (!PyArg_ParseTuple(args, "OO", &key, &value)) return NULL;

    PyObject *oldv = PyObject_GetItem(self, key);
    PyErr_Clear();

    PyObject *res = PyObject_CallMethod(self, "__setitem__", "OO", key, value);
    if (!res) {
        Py_XDECREF(oldv);
        return NULL;
    }

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        _call_setitem_hook(hooks, self, key, oldv, value);
        Py_DECREF(hooks);
    }
    Py_XDECREF(oldv);
    return res;
}

static PyObject *
reaktome_dict_delitem(PyObject *self, PyObject *arg)
{
    PyObject *oldv = PyObject_GetItem(self, arg);
    if (!oldv && PyErr_Occurred()) return NULL;
    PyErr_Clear();

    PyObject *res = PyObject_CallMethod(self, "__delitem__", "O", arg);
    if (!res) {
        Py_XDECREF(oldv);
        return NULL;
    }

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        _call_delitem_hook(hooks, self, arg, oldv);
        Py_DECREF(hooks);
    }
    Py_XDECREF(oldv);
    return res;
}

static PyObject *
reaktome_dict_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *items = PyDict_Items(self);
    if (!items) return NULL;

    PyObject *res = PyObject_CallMethod(self, "clear", NULL);
    if (!res) {
        Py_DECREF(items);
        return NULL;
    }

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        Py_ssize_t n = PyList_GET_SIZE(items);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *pair = PyList_GET_ITEM(items, i);
            PyObject *key = PyTuple_GET_ITEM(pair, 0);
            PyObject *val = PyTuple_GET_ITEM(pair, 1);
            Py_INCREF(key);
            Py_INCREF(val);
            _call_delitem_hook(hooks, self, key, val);
            Py_DECREF(key);
            Py_DECREF(val);
        }
        Py_DECREF(hooks);
    }
    Py_DECREF(items);
    return res;
}

static PyObject *
reaktome_dict_pop(PyObject *self, PyObject *args)
{
    PyObject *key, *def = NULL;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;

    PyObject *oldv = PyObject_GetItem(self, key);
    PyErr_Clear();

    PyObject *res;
    if (def)
        res = PyObject_CallMethod(self, "pop", "OO", key, def);
    else
        res = PyObject_CallMethod(self, "pop", "O", key);

    if (!res) {
        Py_XDECREF(oldv);
        return NULL;
    }

    if (oldv) {
        PyObject *hooks = activation_get_hooks(self);
        if (hooks) {
            _call_delitem_hook(hooks, self, key, oldv);
            Py_DECREF(hooks);
        }
    }
    Py_XDECREF(oldv);
    return res;
}

static PyObject *
reaktome_dict_popitem(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *res = PyObject_CallMethod(self, "popitem", NULL);
    if (!res) return NULL;

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        PyObject *key = PyTuple_GET_ITEM(res, 0);
        PyObject *val = PyTuple_GET_ITEM(res, 1);
        Py_INCREF(key);
        Py_INCREF(val);
        _call_delitem_hook(hooks, self, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
        Py_DECREF(hooks);
    }
    return res;
}

static PyObject *
reaktome_dict_update(PyObject *self, PyObject *arg)
{
    PyObject *res = PyObject_CallMethod(self, "update", "O", arg);
    if (!res) return NULL;

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        PyObject *items = PyMapping_Items(arg);
        if (items) {
            Py_ssize_t n = PyList_GET_SIZE(items);
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *pair = PyList_GET_ITEM(items, i);
                PyObject *key = PyTuple_GET_ITEM(pair, 0);
                PyObject *val = PyTuple_GET_ITEM(pair, 1);
                Py_INCREF(key);
                Py_INCREF(val);
                _call_setitem_hook(hooks, self, key, Py_None, val);
                Py_DECREF(key);
                Py_DECREF(val);
            }
            Py_DECREF(items);
        }
        Py_DECREF(hooks);
    }
    return res;
}

/* --- defs --- */
static PyMethodDef reaktome_dict_setitem_def = {
    "__setitem__", (PyCFunction)reaktome_dict_setitem, METH_VARARGS, NULL
};
static PyMethodDef reaktome_dict_delitem_def = {
    "__delitem__", (PyCFunction)reaktome_dict_delitem, METH_O, NULL
};
static PyMethodDef reaktome_dict_clear_def = {
    "clear", (PyCFunction)reaktome_dict_clear, METH_NOARGS, NULL
};
static PyMethodDef reaktome_dict_pop_def = {
    "pop", (PyCFunction)reaktome_dict_pop, METH_VARARGS, NULL
};
static PyMethodDef reaktome_dict_popitem_def = {
    "popitem", (PyCFunction)reaktome_dict_popitem, METH_NOARGS, NULL
};
static PyMethodDef reaktome_dict_update_def = {
    "update", (PyCFunction)reaktome_dict_update, METH_O, NULL
};

static int
_install_dict_method(PyMethodDef *mdef)
{
    PyObject *func = PyCFunction_NewEx(mdef, NULL, NULL);
    if (!func) return -1;
    int rv = PyDict_SetItemString(PyDict_Type.tp_dict, mdef->ml_name, func);
    Py_DECREF(func);
    return rv;
}

int
patch_dict(void)
{
    if (_install_dict_method(&reaktome_dict_setitem_def) < 0) return -1;
    if (_install_dict_method(&reaktome_dict_delitem_def) < 0) return -1;
    if (_install_dict_method(&reaktome_dict_clear_def) < 0) return -1;
    if (_install_dict_method(&reaktome_dict_pop_def) < 0) return -1;
    if (_install_dict_method(&reaktome_dict_popitem_def) < 0) return -1;
    if (_install_dict_method(&reaktome_dict_update_def) < 0) return -1;
    return 0;
}
