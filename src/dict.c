/* src/dict.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"
#include <string.h>

/* ---------- Saved original slot/method pointers ---------- */
/* mapping slot */
static int (*orig_mp_ass_subscript)(PyObject *, PyObject *, PyObject *) = NULL;

/* method table functions (C function pointers saved from PyDict_Type.tp_methods) */
static PyCFunction orig_update = NULL;    /* METH_VARARGS */
static PyCFunction orig_clear = NULL;     /* METH_NOARGS */
static PyCFunction orig_pop = NULL;       /* METH_O */
static PyCFunction orig_popitem = NULL;   /* METH_NOARGS */
static PyCFunction orig_setdefault = NULL;/* METH_VARARGS */

/* reentrancy guard to avoid wrapper->hook->wrapper loops */
static __thread int inprogress = 0;

/* ---------- helper: call hook but swallow errors (advisory) ---------- */
static inline void
call_hook_advisory_dict(PyObject *self,
                        const char *name,
                        PyObject *key,
                        PyObject *old,
                        PyObject *newv)
{
    if (reaktome_call_dunder(self, name, key, old, newv) < 0) {
        PyErr_Clear();
    }
}

/* ---------- find a methoddef in a type's tp_methods by name ---------- */
static PyMethodDef *
find_methoddef_in_type(PyTypeObject *tp, const char *name)
{
    PyMethodDef *m = tp->tp_methods;
    if (!m) return NULL;
    for (; m->ml_name != NULL; m++) {
        if (strcmp(m->ml_name, name) == 0) return m;
    }
    return NULL;
}

/* ---------- slot trampoline: mp_ass_subscript ---------- */
/* Handles d[key] = value  (value != NULL) and del d[key] (value == NULL) */
static int
tramp_mp_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
    /* Fetch old value if present (newref) for calling hooks later */
    PyObject *old = NULL;
    int got_old = 0;

    /* Try to get old value; if missing that's okay for setitem */
    old = PyObject_GetItem(self, key);  /* new ref or NULL with exception */
    if (old) {
        got_old = 1;
    } else {
        if (PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
            } else {
                return -1;
            }
        }
    }

    int rc = -1;
    /* perform the underlying operation */
    if (orig_mp_ass_subscript) {
        rc = orig_mp_ass_subscript(self, key, value);
    } else {
        if (value == NULL) rc = PyObject_DelItem(self, key);
        else rc = PyObject_SetItem(self, key, value);
    }

    if (rc < 0) {
        Py_XDECREF(old);
        return -1;
    }

    /* On success, call advisory hook: setitem or delitem */
    if (value == NULL) {
        /* delete: old must exist to signal; if old absent, do nothing */
        if (got_old) {
            call_hook_advisory_dict(self, "__reaktome_delitem__", key, old, NULL);
        }
    } else {
        /* assignment */
        call_hook_advisory_dict(self, "__reaktome_setitem__", key, old, value);
    }

    Py_XDECREF(old);
    return 0;
}

/* ---------- method wrappers for dict methods ---------- */

/* Helper: iterate mapping 'm' (newref) and call __reaktome_setitem__ for each item (k,v) */
static int
call_setitem_for_mapping(PyObject *self, PyObject *mapping)
{
    if (!mapping) return 0;
    PyObject *items = PyMapping_Items(mapping); /* newref */
    if (!items) {
        PyErr_Clear();
        return 0;
    }
    Py_ssize_t n = PyList_Size(items);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *tup = PyList_GetItem(items, i); /* borrowed */
        if (!tup) continue;
        PyObject *k = PyTuple_GetItem(tup, 0); /* borrowed */
        PyObject *v = PyTuple_GetItem(tup, 1); /* borrowed */
        /* call advisory; ignore errors */
        call_hook_advisory_dict(self, "__reaktome_setitem__", k, NULL, v);
    }
    Py_DECREF(items);
    return 0;
}

/* update(self, ...) wrapper: best-effort — attempt to call setitem hook for input items */
static PyObject *
patched_dict_update(PyObject *self, PyObject *args)
{
    PyObject *res = NULL;
    /* Save a reference to the first arg if present so we can inspect it */
    PyObject *arg0 = NULL;
    if (PyTuple_Size(args) >= 1) {
        arg0 = PyTuple_GetItem(args, 0); /* borrowed */
        Py_XINCREF(arg0);
    }

    /* call original */
    if (orig_update) {
        res = orig_update(self, args);
    } else {
        /* fallback to calling Python-level attribute if orig missing */
        PyObject *tp = (PyObject *)Py_TYPE(self);
        PyObject *callable = PyObject_GetAttrString(tp, "update");
        if (!callable) {
            Py_XDECREF(arg0);
            return NULL;
        }
        res = PyObject_Call(callable, args, NULL);
        Py_DECREF(callable);
    }

    if (!res) {
        Py_XDECREF(arg0);
        return NULL;
    }

    /* After successful update, try to call setitem hook for items in arg0 (if mapping) */
    if (arg0 && PyMapping_Check(arg0)) {
        call_setitem_for_mapping(self, arg0);
    } else if (arg0) {
        /* If arg0 is an iterable of pairs, attempt to iterate it */
        PyObject *it = PyObject_GetIter(arg0);
        if (it) {
            PyObject *item;
            while ((item = PyIter_Next(it))) {
                if (PyTuple_Check(item) && PyTuple_Size(item) == 2) {
                    PyObject *k = PyTuple_GetItem(item, 0);
                    PyObject *v = PyTuple_GetItem(item, 1);
                    call_hook_advisory_dict(self, "__reaktome_setitem__", k, NULL, v);
                }
                Py_DECREF(item);
            }
            Py_DECREF(it);
            if (PyErr_Occurred()) PyErr_Clear();
        }
    }

    Py_DECREF(res);
    Py_XDECREF(arg0);
    Py_RETURN_NONE;
}

/* clear(self) wrapper: snapshot old items, call original, then fire delitem hooks per old item */
static PyObject *
patched_dict_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    /* Snapshot current items (newref) */
    PyObject *items = PyDict_Items(self); /* newref */
    if (!items && PyErr_Occurred()) return NULL;

    PyObject *res = NULL;

    if (inprogress) {
        /* If already in-progress, just forward to original to avoid recursion */
        if (orig_clear) {
            res = orig_clear(self, NULL);
            if (!res) return NULL;
            Py_DECREF(res);
            Py_XDECREF(items);
            Py_RETURN_NONE;
        } else {
            PyDict_Clear(self);  /* void */
            Py_XDECREF(items);
            Py_RETURN_NONE;
        }
    }

    inprogress = 1;

    if (orig_clear) {
        res = orig_clear(self, NULL);
        if (!res) {
            inprogress = 0;
            Py_XDECREF(items);
            return NULL;
        }
        Py_DECREF(res);
    } else {
        /* fallback: clear the dict (void) and continue to fire delitem hooks below */
        PyDict_Clear(self);
    }

    /* Fire delitem for each old item */
    if (items) {
        Py_ssize_t n = PyList_Size(items);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *tup = PyList_GetItem(items, i); /* borrowed */
            if (!tup) continue;
            PyObject *k = PyTuple_GetItem(tup, 0); /* borrowed */
            PyObject *v = PyTuple_GetItem(tup, 1); /* borrowed */
            call_hook_advisory_dict(self, "__reaktome_delitem__", k, v, NULL);
        }
        Py_DECREF(items);
    }

    inprogress = 0;
    Py_RETURN_NONE;
}

/* pop(self, key) wrapper: call original; if popped value returned, call delitem hook */
static PyObject *
patched_dict_pop(PyObject *self, PyObject *arg)
{
    PyObject *res = NULL;

    if (orig_pop) {
        res = orig_pop(self, arg);
    } else {
        /* fallback to PyObject_CallMethod */
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "pop");
        if (!callable) return NULL;
        PyObject *targs = PyTuple_Pack(1, arg);
        if (!targs) { Py_DECREF(callable); return NULL; }
        res = PyObject_Call(callable, targs, NULL);
        Py_DECREF(targs);
        Py_DECREF(callable);
    }

    if (!res) return NULL; /* may be exception */

    /* res is the popped value (old). Fire del hook */
    call_hook_advisory_dict(self, "__reaktome_delitem__", arg, res, NULL);

    return res; /* return popped value */
}

/* popitem(self) wrapper: call original; if returns (k,v), fire del hook */
static PyObject *
patched_dict_popitem(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *res = NULL;

    if (orig_popitem) {
        res = orig_popitem(self, NULL);
    } else {
        /* fallback */
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "popitem");
        if (!callable) return NULL;
        res = PyObject_Call(callable, PyTuple_New(0), NULL);
        Py_DECREF(callable);
    }

    if (!res) return NULL;

    /* res should be a tuple (k, v) */
    if (PyTuple_Check(res) && PyTuple_Size(res) == 2) {
        PyObject *k = PyTuple_GetItem(res, 0); /* borrowed */
        PyObject *v = PyTuple_GetItem(res, 1); /* borrowed */
        call_hook_advisory_dict(self, "__reaktome_delitem__", k, v, NULL);
    }

    return res;
}

static PyObject *
patched_dict_setdefault(PyObject *self, PyObject *args) {
    PyObject *key = NULL;
    PyObject *default_value = Py_None;

    // peek at args
    if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &default_value)) {
        return NULL;
    }

    int had_key = PyDict_Contains(self, key);
    if (had_key < 0) return NULL; /* error */

    if (inprogress) {
        if (orig_setdefault) {
            // Just forward the original args tuple, don't touch it
            return orig_setdefault(self, args);
        } else {
            PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "setdefault");
            if (!callable) return NULL;
            PyObject *res = PyObject_Call(callable, args, NULL);
            Py_DECREF(callable);
            return res;
        }
    }

    inprogress = 1;

    PyObject *res;
    if (orig_setdefault) {
        res = orig_setdefault(self, args);
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "setdefault");
        if (!callable) { inprogress = 0; return NULL; }
        res = PyObject_Call(callable, args, NULL);
        Py_DECREF(callable);
    }

    if (!res) {
        inprogress = 0;
        return NULL;
    }

    if (had_key == 0) {
        // Only call hook if the key was absent before
        call_hook_advisory_dict(self, "__reaktome_setitem__", key, NULL, res);
    }

    inprogress = 0;
    return res;
}

/* ---------- install wrappers into PyDict_Type.tp_methods ---------- */

/* Find methoddefs for target names and swap ml_meth -> our wrappers.
   Save original ml_meth into orig_* function pointers. */

static int
install_method_wrappers_for_dict(void)
{
    PyMethodDef *m;

    m = find_methoddef_in_type(&PyDict_Type, "update");
    if (!m) return -1;
    orig_update = m->ml_meth;
    m->ml_meth = (PyCFunction)patched_dict_update;

    m = find_methoddef_in_type(&PyDict_Type, "clear");
    if (!m) return -1;
    orig_clear = m->ml_meth;
    m->ml_meth = (PyCFunction)patched_dict_clear;

    m = find_methoddef_in_type(&PyDict_Type, "pop");
    if (!m) return -1;
    orig_pop = m->ml_meth;
    m->ml_meth = (PyCFunction)patched_dict_pop;

    m = find_methoddef_in_type(&PyDict_Type, "popitem");
    if (!m) return -1;
    orig_popitem = m->ml_meth;
    m->ml_meth = (PyCFunction)patched_dict_popitem;

    m = find_methoddef_in_type(&PyDict_Type, "setdefault");
    if (!m) return -1;
    orig_setdefault = m->ml_meth;
    m->ml_meth = (PyCFunction)patched_dict_setdefault;

    /* inform runtime */
    PyType_Modified(&PyDict_Type);
    return 0;
}

/* ---------- Python wrapper: py_patch_dict(instance, dunders) ---------- */
static PyObject *
py_patch_dict(PyObject *self, PyObject *args)
{
    PyObject *inst;
    PyObject *dunders;
    if (!PyArg_ParseTuple(args, "OO:patch_dict", &inst, &dunders))
        return NULL;

    if (!PyDict_Check(inst)) {
        PyErr_SetString(PyExc_TypeError, "patch_dict: expected dict instance");
        return NULL;
    }

    /* Ensure dict type ready */
    if (PyType_Ready(Py_TYPE(inst)) < 0) return NULL;

    /* Install slot trampoline once */
    if (!orig_mp_ass_subscript) {
        PyMappingMethods *mp = Py_TYPE(inst)->tp_as_mapping;
        if (!mp) {
            PyErr_SetString(PyExc_RuntimeError, "patch_dict: type has no mapping methods");
            return NULL;
        }
        orig_mp_ass_subscript = mp->mp_ass_subscript;
        mp->mp_ass_subscript = tramp_mp_ass_subscript;
        /* Tell runtime the type dict changed */
        PyType_Modified(Py_TYPE(inst));
    }

    /* Install method wrappers once */
    if (!orig_update) {
        if (install_method_wrappers_for_dict() < 0) {
            PyErr_SetString(PyExc_RuntimeError, "patch_dict: failed to install method wrappers");
            return NULL;
        }
    }

    /* Merge hooks for this instance (activation side-table). dunders may be None to clear. */
    if (activation_merge(inst, dunders) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ---------- static PyMethodDef objects (file scope) ---------- */
static PyMethodDef dict_methods[] = {
    {"patch_dict", (PyCFunction)py_patch_dict, METH_VARARGS, "Activate dict instance with dunders (or None to clear)"},
    {NULL, NULL, 0, NULL}
};

/* Called from reaktome.c to register patch_dict into the module */
int
reaktome_patch_dict(PyObject *m)
{
    if (!m) return -1;
    if (PyModule_AddFunctions(m, dict_methods) < 0) return -1;
    return 0;
}
