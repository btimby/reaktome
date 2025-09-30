#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

static PyObject *
py_patch_type(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyTypeObject *tp;
    PyObject *dunders = NULL;
    static char *kwlist[] = {"tp", "dunders", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!|O", kwlist, &PyType_Type, &tp, &dunders))
        return NULL;

    if (dunders == NULL) dunders = Py_None;
    if (dunders != Py_None && !PyDict_Check(dunders)) {
        PyErr_SetString(PyExc_TypeError, "patch_type: dunders must be a dict or None");
        return NULL;
    }

    if (activation_merge((PyObject *)tp, dunders) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyMethodDef obj_methods[] = {
    {"patch_type", (PyCFunction)py_patch_type, METH_VARARGS | METH_KEYWORDS,
     "Activate reaktome hooks on a type: patch_type(tp, dunders=None)"},
    {NULL, NULL, 0, NULL}
};

int
reaktome_patch_obj(PyObject *m)
{
    if (PyModule_AddFunctions(m, obj_methods) < 0)
        return -1;
    return 0;
}
