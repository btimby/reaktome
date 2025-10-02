/* src/obj.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/*
 * Per-instance patching for objects with __dict__.
 *
 * Strategy:
 *  - Save original __setattr__ / __delattr__ (if present) into the activation
 *    side-table by calling activation_merge(inst, origs_dict).
 *  - Install per-instance C-callable wrappers for __setattr__ and __delattr__
 *    by creating PyCFunction objects with the 'm' argument set to the instance.
 *  - Trampolines look up the saved originals via activation_get_hooks(inst)
 *    and call them (avoiding recursion). After successful mutation, call
 *    reaktome_call_dunder via call_hook_advisory_obj to dispatch hooks.
 */

/* methoddef templates for the instance-bound trampolines */
static PyMethodDef setattro_def = {"__setattr__", (PyCFunction)NULL, METH_VARARGS, "instance-level trampoline for __setattr__"};
static PyMethodDef delattro_def = {"__delattr__", (PyCFunction)NULL, METH_VARARGS, "instance-level trampoline for __delattr__"};

/* helper: call hook but swallow errors (advisory) */
static inline void
call_hook_advisory_obj(PyObject *self,
                       const char *name,
                       PyObject *key,
                       PyObject *old,
                       PyObject *newv)
{
    if (reaktome_call_dunder(self, name, key, old, newv) < 0) {
        PyErr_Clear();
    }
}

/* Helper: fetch the saved original from activation side-table (borrowed or NULL)
   Keys used in the side-table: "__orig_setattr__", "__orig_delattr__" */
static PyObject *
get_saved_original(PyObject *inst, const char *keyname)
{
    PyObject *hooks = activation_get_hooks(inst); /* newref or NULL */
    if (!hooks) return NULL;
    PyObject *orig = PyDict_GetItemString(hooks, keyname); /* borrowed */
    Py_DECREF(hooks);
    /* Do NOT INCREF here; keep borrowed semantics for caller convenience */
    return orig;
}

/* trampoline for instance __setattr__.
   Note: when created via PyCFunction_NewEx(&setattro_def, inst, NULL),
   the 'self' argument received by this C function will be the instance. */
static PyObject *
tramp_inst_setattr(PyObject *self, PyObject *args)
{
    PyObject *name = NULL;
    PyObject *value = NULL;

    if (!PyArg_UnpackTuple(args, "__setattr__", 2, 2, &name, &value)) {
        return NULL;
    }

    /* Try to fetch old value (if any) for passing to hook later */
    PyObject *old = PyObject_GetAttr(self, name); /* newref or NULL with exception */
    if (!old) {
        if (PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
            } else {
                return NULL;
            }
        }
    }

    /* Call saved original if present, otherwise perform generic set */
    PyObject *orig = get_saved_original(self, "__orig_setattr__"); /* borrowed or NULL */
    PyObject *res = NULL;
    if (orig) {
        /* orig is likely a bound method or callable that expects (self, name, value) */
        res = PyObject_CallFunctionObjArgs(orig, self, name, value, NULL);
    } else {
        /* fallback to generic set (do not call back into instance __setattr__) */
        if (PyObject_GenericSetAttr(self, name, value) < 0) {
            Py_XDECREF(old);
            return NULL;
        }
        /* mimic successful __setattr__ returning None */
        Py_INCREF(Py_None);
        res = Py_None;
    }

    if (!res) {
        Py_XDECREF(old);
        return NULL; /* propagate original exception */
    }
    /* discard any return value from original (usually None) */
    Py_DECREF(res);

    /* advisory: call hooks (name as key, old, new) */
    call_hook_advisory_obj(self, "__reaktome_setattr__", name, old, value);

    Py_XDECREF(old);
    Py_RETURN_NONE;
}

/* trampoline for instance __delattr__.
   signature: (__delattr__)(name) — args tuple has (name,) */
static PyObject *
tramp_inst_delattr(PyObject *self, PyObject *args)
{
    PyObject *name = NULL;
    if (!PyArg_UnpackTuple(args, "__delattr__", 1, 1, &name)) {
        return NULL;
    }

    /* Fetch old value if present to report to hooks */
    PyObject *old = PyObject_GetAttr(self, name); /* newref or NULL */
    if (!old) {
        if (PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
            } else {
                return NULL;
            }
        }
    }

    PyObject *orig = get_saved_original(self, "__orig_delattr__"); /* borrowed or NULL */
    PyObject *res = NULL;
    if (orig) {
        /* orig expected to be callable (bound method or function): call with (self, name) */
        res = PyObject_CallFunctionObjArgs(orig, self, name, NULL);
    } else {
        /* fallback to generic delete */
        if (PyObject_GenericSetAttr(self, name, NULL) < 0) {
            Py_XDECREF(old);
            return NULL;
        }
        Py_INCREF(Py_None);
        res = Py_None;
    }

    if (!res) {
        Py_XDECREF(old);
        return NULL;
    }
    Py_DECREF(res);

    /* advisory: call hooks */
    call_hook_advisory_obj(self, "__reaktome_delattr__", name, old, NULL);

    Py_XDECREF(old);
    Py_RETURN_NONE;
}

/* Create instance-bound trampolines and install them on the given instance.
   Returns 0 on success, -1 on error. */
static int
install_instance_trampolines(PyObject *inst)
{
    /* Create the PyCFunction objects with 'm' set to the instance so the C
       function receives the instance as the first arg automatically. */
    PyObject *set_fn = NULL;
    PyObject *del_fn = NULL;

    /* Set the C function pointers in the methoddefs (only once) */
    setattro_def.ml_meth = (PyCFunction)tramp_inst_setattr;
    delattro_def.ml_meth = (PyCFunction)tramp_inst_delattr;

    set_fn = PyCFunction_NewEx(&setattro_def, inst, NULL); /* newref */
    if (!set_fn) goto err;

    if (PyObject_SetAttrString(inst, "__setattr__", set_fn) < 0) goto err;

    del_fn = PyCFunction_NewEx(&delattro_def, inst, NULL); /* newref */
    if (!del_fn) goto err;

    if (PyObject_SetAttrString(inst, "__delattr__", del_fn) < 0) goto err;

    Py_DECREF(set_fn);
    Py_DECREF(del_fn);
    return 0;

err:
    Py_XDECREF(set_fn);
    Py_XDECREF(del_fn);
    return -1;
}

/* py_patch_obj(instance, dunders) — public Python-callable entrypoint.
   dunders may be None to clear hooks per activation_merge semantics. */
static PyObject *
py_patch_obj(PyObject *self, PyObject *args)
{
    PyObject *inst = NULL;
    PyObject *dunders = NULL;

    if (!PyArg_ParseTuple(args, "OO:patch_obj", &inst, &dunders)) {
        return NULL;
    }

    /* Ensure this object is a user object with a __dict__ */
    if (!PyObject_HasAttrString(inst, "__dict__")) {
        PyErr_SetString(PyExc_TypeError, "patch_obj: instance has no __dict__");
        return NULL;
    }

    /* Save current __setattr__ / __delattr__ into a small dict and merge into activation side-table.
       We do this before installing our trampolines. */
    PyObject *orig_dict = PyDict_New();
    if (!orig_dict) return NULL;

    PyObject *orig_set = PyObject_GetAttrString(inst, "__setattr__"); /* newref or NULL */
    if (orig_set) {
        if (PyDict_SetItemString(orig_dict, "__orig_setattr__", orig_set) < 0) {
            Py_DECREF(orig_set);
            Py_DECREF(orig_dict);
            return NULL;
        }
        Py_DECREF(orig_set);
    } else {
        /* If absent, clear the error and continue; we still store nothing. */
        if (PyErr_Occurred()) PyErr_Clear();
    }

    PyObject *orig_del = PyObject_GetAttrString(inst, "__delattr__"); /* newref or NULL */
    if (orig_del) {
        if (PyDict_SetItemString(orig_dict, "__orig_delattr__", orig_del) < 0) {
            Py_DECREF(orig_del);
            Py_DECREF(orig_dict);
            return NULL;
        }
        Py_DECREF(orig_del);
    } else {
        if (PyErr_Occurred()) PyErr_Clear();
    }

    /* Merge the ORIGINALS into activation side-table for this instance.
       This stores the saved originals under the instance key so trampolines can find them. */
    if (PyDict_Size(orig_dict) > 0) {
        if (activation_merge(inst, orig_dict) < 0) {
            Py_DECREF(orig_dict);
            return NULL;
        }
    }
    Py_DECREF(orig_dict);

    /* Install instance-bound trampolines (overwrites instance attrs __setattr__/__delattr__) */
    if (install_instance_trampolines(inst) < 0) {
        return NULL;
    }

    /* Finally merge the user-provided dunders (hooks) into the activation side-table.
       activation_merge accepts dunders (or None to clear). */
    if (activation_merge(inst, dunders) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

/* Module method table for obj */
static PyMethodDef obj_methods[] = {
    {"patch_obj", (PyCFunction)py_patch_obj, METH_VARARGS, "Activate object instance with dunders (or None to clear)"},
    {NULL, NULL, 0, NULL}
};

/* Called from reaktome.c to register patch_obj into the module */
int
reaktome_patch_obj(PyObject *m)
{
    if (!m) return -1;
    if (PyModule_AddFunctions(m, obj_methods) < 0) return -1;
    return 0;
}
