// src/obj.c
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* from activation.c */
extern PyObject *activation_get_hooks(PyObject *obj);

static void
_call_setattr_hook(PyObject *hooks, PyObject *self,
                   PyObject *key, PyObject *oldv, PyObject *newv)
{
    if (!hooks) return;
    PyObject *hook = PyDict_GetItemString(hooks, "__reaktome_setattr__");
    if (!hook) return;
    PyObject *res = PyObject_CallFunctionObjArgs(hook, self, key, oldv ? oldv : Py_None, newv, NULL);
    Py_XDECREF(res);
}

static void
_call_delattr_hook(PyObject *hooks, PyObject *self,
                   PyObject *key, PyObject *oldv)
{
    if (!hooks) return;
    PyObject *hook = PyDict_GetItemString(hooks, "__reaktome_delattr__");
    if (!hook) return;
    PyObject *res = PyObject_CallFunctionObjArgs(hook, self, key, oldv ? oldv : Py_None, NULL);
    Py_XDECREF(res);
}

static PyObject *
reaktome_obj_setattr(PyObject *self, PyObject *args)
{
    PyObject *name, *value;
    if (!PyArg_ParseTuple(args, "OO", &name, &value)) return NULL;

    /* fetch old value */
    PyObject *oldv = PyObject_GetAttr(self, name);
    PyErr_Clear();

    PyObject *res = PyObject_CallMethod(self, "__setattr__", "OO", name, value);
    if (!res) {
        Py_XDECREF(oldv);
        return NULL;
    }

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        _call_setattr_hook(hooks, self, name, oldv, value);
        Py_DECREF(hooks);
    }
    Py_XDECREF(oldv);
    return res;
}

static PyObject *
reaktome_obj_delattr(PyObject *self, PyObject *arg)
{
    /* fetch old value */
    PyObject *oldv = PyObject_GetAttr(self, arg);
    PyErr_Clear();

    PyObject *res = PyObject_CallMethod(self, "__delattr__", "O", arg);
    if (!res) {
        Py_XDECREF(oldv);
        return NULL;
    }

    PyObject *hooks = activation_get_hooks(self);
    if (hooks) {
        _call_delattr_hook(hooks, self, arg, oldv);
        Py_DECREF(hooks);
    }
    Py_XDECREF(oldv);
    return res;
}

/* --- defs --- */
static PyMethodDef reaktome_obj_setattr_def = {
    "__setattr__", (PyCFunction)reaktome_obj_setattr, METH_VARARGS, NULL
};
static PyMethodDef reaktome_obj_delattr_def = {
    "__delattr__", (PyCFunction)reaktome_obj_delattr, METH_O, NULL
};

static int
_install_obj_method(PyTypeObject *tp, PyMethodDef *mdef)
{
    PyObject *func = PyCFunction_NewEx(mdef, NULL, NULL);
    if (!func) return -1;
    int rv = PyDict_SetItemString(tp->tp_dict, mdef->ml_name, func);
    Py_DECREF(func);
    return rv;
}

int
patch_obj(PyTypeObject *tp)
{
    if (_install_obj_method(tp, &reaktome_obj_setattr_def) < 0) return -1;
    if (_install_obj_method(tp, &reaktome_obj_delattr_def) < 0) return -1;
    return 0;
}
