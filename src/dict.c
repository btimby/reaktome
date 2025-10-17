/* src/dict.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"
#include <string.h>

/* ---------- Saved original slot/method pointers ---------- */
/* mapping slot */
static int (*orig_mp_ass_subscript)(PyObject *, PyObject *, PyObject *) = NULL;

/* ORIGINAL METHOD OBJECTS (descriptors) - saved from PyDict_Type.tp_dict */
static PyObject *orig_update = NULL;    /* descriptor object for update */
static PyObject *orig_clear = NULL;     /* descriptor object for clear */
static PyObject *orig_pop = NULL;       /* descriptor object for pop */
static PyObject *orig_popitem = NULL;   /* descriptor object for popitem */
static PyObject *orig_setdefault = NULL;/* descriptor object for setdefault */

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

/* Build a new argument tuple with `self` prefixed to `args` */
static PyObject *
build_args_with_self(PyObject *self, PyObject *args)
{
    if (!args || !PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError, "expected arg tuple");
        return NULL;
    }
    Py_ssize_t n = PyTuple_Size(args);
    PyObject *newt = PyTuple_New(n + 1);
    if (!newt) return NULL;
    Py_INCREF(self);
    PyTuple_SET_ITEM(newt, 0, self); /* steals ref */
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PyTuple_GetItem(args, i); /* borrowed */
        Py_INCREF(it);
        PyTuple_SET_ITEM(newt, i + 1, it); /* steals ref */
    }
    return newt;
}

/* forward declarations for wrapper methoddefs (used when creating descriptors) */
static PyObject *patched_dict_update(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *patched_dict_clear(PyObject *self, PyObject *Py_UNUSED(ignored));
static PyObject *patched_dict_pop(PyObject *self, PyObject *arg);
static PyObject *patched_dict_popitem(PyObject *self, PyObject *Py_UNUSED(ignored));
static PyObject *patched_dict_setdefault(PyObject *self, PyObject *args);

/* methoddef templates for descriptors we'll create in type dict */
static PyMethodDef update_def = {"update", (PyCFunction)patched_dict_update, METH_VARARGS | METH_KEYWORDS, "update (trampoline)"};
static PyMethodDef clear_def  = {"clear",  (PyCFunction)patched_dict_clear,  METH_NOARGS,  "clear (trampoline)"};
static PyMethodDef pop_def    = {"pop", (PyCFunction)patched_dict_pop, METH_VARARGS, "pop (trampoline)"};
static PyMethodDef popitem_def= {"popitem",(PyCFunction)patched_dict_popitem,METH_NOARGS,  "popitem (trampoline)"};
static PyMethodDef setdefault_def = {"setdefault", (PyCFunction)patched_dict_setdefault, METH_VARARGS, "setdefault (trampoline)"};

/* update(self, ...) wrapper: best-effort — attempt to call setitem hook for input items */
static PyObject *
patched_dict_update(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *res = NULL;
    /* Save a reference to the first arg if present so we can inspect it */
    PyObject *arg0 = NULL;
    if (PyTuple_Size(args) >= 1) {
        arg0 = PyTuple_GetItem(args, 0); /* borrowed */
        Py_XINCREF(arg0);
    }

    /* call original: use saved descriptor + self-prefixed args to avoid calling our wrapper */
    if (orig_update) {
        PyObject *call_args = build_args_with_self(self, args);
        if (!call_args) { Py_XDECREF(arg0); return NULL; }
        res = PyObject_Call(orig_update, call_args, kwargs);
        Py_DECREF(call_args);
    } else {
        /* fallback to calling Python-level attribute from the type */
        PyObject *tp = (PyObject *)Py_TYPE(self);
        PyObject *callable = PyObject_GetAttrString(tp, "update");
        if (!callable) {
            Py_XDECREF(arg0);
            return NULL;
        }
        res = PyObject_Call(callable, args, kwargs);
        Py_DECREF(callable);
    }

    if (!res) {
        Py_XDECREF(arg0);
        return NULL;
    }
    Py_DECREF(res);

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

    /* Also handle keyword arguments: call hook for each key=value pair */
    if (kwargs && PyDict_Check(kwargs)) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            call_hook_advisory_dict(self, "__reaktome_setitem__", key, NULL, value);
        }
    }

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
            PyObject *empty = PyTuple_New(0);
            if (!empty) { Py_XDECREF(items); return NULL; }
            PyObject *call_args = build_args_with_self(self, empty);
            Py_DECREF(empty);
            if (!call_args) { Py_XDECREF(items); return NULL; }
            res = PyObject_Call(orig_clear, call_args, NULL);
            Py_DECREF(call_args);
            if (!res) { Py_XDECREF(items); return NULL; }
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
        PyObject *empty = PyTuple_New(0);
        if (!empty) { inprogress = 0; Py_XDECREF(items); return NULL; }
        PyObject *call_args = build_args_with_self(self, empty);
        Py_DECREF(empty);
        if (!call_args) { inprogress = 0; Py_XDECREF(items); return NULL; }
        res = PyObject_Call(orig_clear, call_args, NULL);
        Py_DECREF(call_args);
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

/* patched_dict_pop: accept (key[, default]) */
static PyObject *
patched_dict_pop(PyObject *self, PyObject *args)
{
    PyObject *key;
    PyObject *default_value = NULL;

    /* Accept 1 or 2 positional args */
    if (!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &default_value)) {
        return NULL;
    }

    /* Check whether the key existed before calling the original (so we know whether to fire hooks) */
    int had_key = PyDict_Contains(self, key);
    if (had_key < 0) {
        return NULL; /* error from Contains */
    }

    PyObject *res = NULL;

    /* Call the saved original descriptor (orig_pop) with self-prefixed args to avoid recursion */
    if (orig_pop) {
        PyObject *call_args = build_args_with_self(self, args);
        if (!call_args) return NULL;
        res = PyObject_Call(orig_pop, call_args, NULL);
        Py_DECREF(call_args);
    } else {
        /* Fallback: call the type-level method */
        PyObject *tp = (PyObject *)Py_TYPE(self);
        PyObject *callable = PyObject_GetAttrString(tp, "pop");
        if (!callable) return NULL;
        res = PyObject_Call(callable, args, NULL);
        Py_DECREF(callable);
    }

    if (!res) {
        return NULL; /* propagate exception (KeyError when no default and missing key, etc.) */
    }

    /* Only fire del hook if the key existed before (i.e., a real deletion occurred) */
    if (had_key == 1) {
        call_hook_advisory_dict(self, "__reaktome_delitem__", key, res, NULL);
    }

    return res; /* newref from original pop */
}

/* popitem(self) wrapper: call original; if returns (k,v), fire del hook */
static PyObject *
patched_dict_popitem(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *res = NULL;

    if (orig_popitem) {
        PyObject *empty = PyTuple_New(0);
        if (!empty) return NULL;
        PyObject *call_args = build_args_with_self(self, empty);
        Py_DECREF(empty);
        if (!call_args) return NULL;
        res = PyObject_Call(orig_popitem, call_args, NULL);
        Py_DECREF(call_args);
    } else {
        /* fallback */
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "popitem");
        if (!callable) return NULL;
        PyObject *empty = PyTuple_New(0);
        if (!empty) { Py_DECREF(callable); return NULL; }
        res = PyObject_Call(callable, empty, NULL);
        Py_DECREF(empty);
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

/* setdefault(self, ...) wrapper: handle binding and call hook when key absent */
static PyObject *
patched_dict_setdefault(PyObject *self, PyObject *args) {
    PyObject *key = NULL;
    PyObject *default_value = Py_None;

    /* peek at args */
    if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &default_value)) {
        return NULL;
    }

    int had_key = PyDict_Contains(self, key);
    if (had_key < 0) return NULL; /* error */

    if (inprogress) {
        if (orig_setdefault) {
            PyObject *call_args = build_args_with_self(self, args);
            if (!call_args) return NULL;
            PyObject *res = PyObject_Call(orig_setdefault, call_args, NULL);
            Py_DECREF(call_args);
            return res;
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
        PyObject *call_args = build_args_with_self(self, args);
        if (!call_args) { inprogress = 0; return NULL; }
        res = PyObject_Call(orig_setdefault, call_args, NULL);
        Py_DECREF(call_args);
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
        /* Only call hook if the key was absent before */
        call_hook_advisory_dict(self, "__reaktome_setitem__", key, NULL, res);
    }

    inprogress = 0;
    return res;
}

/* ---------- install wrappers into PyDict_Type.tp_dict (shadowing in dict) ---------- */

static int
install_method_wrappers_for_dict(void)
{
    PyObject *dict = PyType_GetDict(&PyDict_Type); /* borrowed */
    if (!dict) return -1;

    PyObject *orig;
    PyObject *func;

    /* update */
    orig = PyDict_GetItemString(dict, "update"); /* borrowed */
    if (orig) { Py_XINCREF(orig); orig_update = orig; }
    func = (PyObject *)PyDescr_NewMethod(&PyDict_Type, &update_def);
    if (!func) return -1;
    if (PyDict_SetItemString(dict, "update", func) < 0) { Py_DECREF(func); return -1; }
    Py_DECREF(func);

    /* clear */
    orig = PyDict_GetItemString(dict, "clear");
    if (orig) { Py_XINCREF(orig); orig_clear = orig; }
    func = (PyObject *)PyDescr_NewMethod(&PyDict_Type, &clear_def);
    if (!func) return -1;
    if (PyDict_SetItemString(dict, "clear", func) < 0) { Py_DECREF(func); return -1; }
    Py_DECREF(func);

    /* pop */
    orig = PyDict_GetItemString(dict, "pop");
    if (orig) { Py_XINCREF(orig); orig_pop = orig; }
    func = (PyObject *)PyDescr_NewMethod(&PyDict_Type, &pop_def);
    if (!func) return -1;
    if (PyDict_SetItemString(dict, "pop", func) < 0) { Py_DECREF(func); return -1; }
    Py_DECREF(func);

    /* popitem */
    orig = PyDict_GetItemString(dict, "popitem");
    if (orig) { Py_XINCREF(orig); orig_popitem = orig; }
    func = (PyObject *)PyDescr_NewMethod(&PyDict_Type, &popitem_def);
    if (!func) return -1;
    if (PyDict_SetItemString(dict, "popitem", func) < 0) { Py_DECREF(func); return -1; }
    Py_DECREF(func);

    /* setdefault */
    orig = PyDict_GetItemString(dict, "setdefault");
    if (orig) { Py_XINCREF(orig); orig_setdefault = orig; }
    func = (PyObject *)PyDescr_NewMethod(&PyDict_Type, &setdefault_def);
    if (!func) return -1;
    if (PyDict_SetItemString(dict, "setdefault", func) < 0) { Py_DECREF(func); return -1; }
    Py_DECREF(func);

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
