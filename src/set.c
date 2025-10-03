/* src/set.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/*
 * Patch the built-in set methods by replacing ml_meth pointers in
 * PySet_Type.tp_methods.  We save originals and install wrappers that
 * call the original then call reaktome_call_dunder(..., key=Py_None, ...).
 *
 * This changes behavior globally (like list slot patching).
 */

/* ---------- saved original method pointers (C function pointers) ---------- */
static PyCFunction orig_add = NULL;
static PyCFunction orig_discard = NULL;
static PyCFunction orig_remove = NULL;

/* reentrancy guard (per-thread) to avoid wrapper->hook->wrapper loops */
static __thread int inprogress = 0;

/* ---------- wrappers (match PyCFunction signature) ---------- */

static PyObject *
patched_set_add(PyObject *self, PyObject *arg)
{
    PyObject *res;

    if (!orig_add) {
        PyErr_SetString(PyExc_RuntimeError, "patched_set_add: orig_add missing");
        return NULL;
    }

    /* call original (C function pointer saved earlier) */
    res = orig_add(self, arg);
    if (!res) return NULL;

    /* guarded advisory call: key = Py_None, old = Py_None, new = arg */
    if (!inprogress) {
        inprogress = 1;
        if (reaktome_call_dunder(self,
                                 "__reaktome_additem__",
                                 Py_None,   /* key */
                                 Py_None,   /* old */
                                 arg) < 0) {
            inprogress = 0;
            Py_DECREF(res);
            return NULL;
        }
        inprogress = 0;
    }

    return res;
}

static PyObject *
patched_set_discard(PyObject *self, PyObject *arg)
{
    PyObject *res;

    if (!orig_discard) {
        PyErr_SetString(PyExc_RuntimeError, "patched_set_discard: orig_discard missing");
        return NULL;
    }

    res = orig_discard(self, arg);
    if (!res) return NULL;

    if (!inprogress) {
        inprogress = 1;
        if (reaktome_call_dunder(self,
                                 "__reaktome_discarditem__",
                                 Py_None,  /* key */
                                 arg,      /* old */
                                 Py_None) < 0) {
            inprogress = 0;
            Py_DECREF(res);
            return NULL;
        }
        inprogress = 0;
    }

    return res;
}

static PyObject *
patched_set_remove(PyObject *self, PyObject *arg)
{
    PyObject *res;

    if (!orig_remove) {
        PyErr_SetString(PyExc_RuntimeError, "patched_set_remove: orig_remove missing");
        return NULL;
    }

    res = orig_remove(self, arg);
    if (!res) return NULL;

    if (!inprogress) {
        inprogress = 1;
        if (reaktome_call_dunder(self,
                                 "__reaktome_discarditem__",
                                 Py_None,  /* key */
                                 arg,      /* old */
                                 Py_None) < 0) {
            inprogress = 0;
            Py_DECREF(res);
            return NULL;
        }
        inprogress = 0;
    }

    return res;
}

/* ---------- helpers to find method entries by name in a type's tp_methods ---------- */

static PyMethodDef *
find_methoddef(PyTypeObject *tp, const char *name)
{
    PyMethodDef *m = tp->tp_methods;
    if (!m) return NULL;
    for (; m->ml_name != NULL; m++) {
        if (strcmp(m->ml_name, name) == 0) return m;
    }
    return NULL;
}

/* ---------- Python-callable: py_patch_set(target, dunders) ---------- */

static PyObject *
py_patch_set(PyObject *self, PyObject *args)
{
    PyObject *target;
    PyObject *dunders;

    if (!PyArg_ParseTuple(args, "OO:patch_set", &target, &dunders))
        return NULL;

    if (!PySet_Check(target)) {
        PyErr_SetString(PyExc_TypeError, "patch_set: expected set instance");
        return NULL;
    }

    /* Ensure the type is initialized (should be) */
    PyTypeObject *tp = Py_TYPE(target);
    if (PyType_Ready(tp) < 0) return NULL;

    /* Install wrappers once: save original ml_meth pointers and replace them */
    if (!orig_add || !orig_discard || !orig_remove) {
        PyMethodDef *m_add = find_methoddef(&PySet_Type, "add");
        PyMethodDef *m_discard = find_methoddef(&PySet_Type, "discard");
        PyMethodDef *m_remove = find_methoddef(&PySet_Type, "remove");

        if (!m_add || !m_discard || !m_remove) {
            PyErr_SetString(PyExc_RuntimeError, "patch_set: failed to locate set methods");
            return NULL;
        }

        /* save originals */
        orig_add = m_add->ml_meth;
        orig_discard = m_discard->ml_meth;
        orig_remove = m_remove->ml_meth;

        /* replace with our wrappers */
        m_add->ml_meth = (PyCFunction)patched_set_add;
        m_discard->ml_meth = (PyCFunction)patched_set_discard;
        m_remove->ml_meth = (PyCFunction)patched_set_remove;

        /* inform runtime that type dict changed (best-effort) */
        PyType_Modified(&PySet_Type);
    }

    /* Merge hooks for this instance (activation side-table). dunders may be None to clear. */
    if (activation_merge(target, dunders) < 0) return NULL;

    Py_RETURN_NONE;
}

/* ---------- method table & exporter ---------- */

static PyMethodDef set_methods[] = {
    {"patch_set", (PyCFunction)py_patch_set, METH_VARARGS, "Activate set instance with dunders (or None to clear)"},
    {NULL, NULL, 0, NULL}
};

int
reaktome_patch_set(PyObject *m)
{
    if (!m) return -1;
    if (PyModule_AddFunctions(m, set_methods) < 0) return -1;
    return 0;
}
