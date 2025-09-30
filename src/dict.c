#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

static PyObject *
py_patch_dict(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj;
    PyObject *dunders = NULL;
    static char *kwlist[] = {"obj", "dunders", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwlist, &obj, &dunders))
        return NULL;

    if (!PyDict_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "patch_dict: expected a dict instance");
        return NULL;
    }

    if (dunders == NULL) dunders = Py_None;
    if (dunders != Py_None && !PyDict_Check(dunders)) {
        PyErr_SetString(PyExc_TypeError, "patch_dict: dunders must be a dict or None");
        return NULL;
    }

    if (activation_merge(obj, dunders) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyMethodDef dict_methods[] = {
    {"patch_dict", (PyCFunction)py_patch_dict, METH_VARARGS | METH_KEYWORDS,
     "Activate reaktome hooks on a dict instance: patch_dict(obj, dunders=None)"},
    {NULL, NULL, 0, NULL}
};

int
reaktome_patch_dict(PyObject *m)
{
    if (PyModule_AddFunctions(m, dict_methods) < 0)
        return -1;
    return 0;
}
