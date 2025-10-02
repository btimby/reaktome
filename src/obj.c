/* src/obj.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/*
 obj.c — stable implementation that:
  - installs a single trampoline into tp_setattro for heap types,
  - stores the real original tp_setattro per-type in module-local dict
    BEFORE replacing the type slot (so we never record the trampoline itself),
  - stores per-instance originals in the activation side-table (capsules),
  - trampoline uses per-instance capsule if present, else falls back to per-type original,
  - trampoline calls original (or generic fallback) then advisory hooks via reaktome_call_dunder.
*/

/* module-local dict mapping type -> capsule(original tp_setattro). newref or NULL */
static PyObject *type_orig_capsules = NULL;

/* Helper: call activation dunder and swallow exceptions */
static inline void
call_hook_advisory_obj(PyObject *self,
                       const char *dunder_name,
                       PyObject *key,
                       PyObject *old,
                       PyObject *newv)
{
    if (reaktome_call_dunder(self, dunder_name, key, old, newv) < 0) {
        PyErr_Clear();
    }
}

/* Trampoline installed into tp_setattro (handles both setattr and delattr) */
static int
tramp_tp_setattro(PyObject *self, PyObject *name, PyObject *value)
{
    /* Snapshot old value (if present) for hook reporting */
    PyObject *old = PyObject_GetAttr(self, name); /* newref or NULL */
    if (!old) {
        if (PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
            } else {
                return -1;
            }
        }
    }

    /* Call advisory hook first (advisory semantics). Swallow hook errors. */
    const char *dunder_call = value ? "__setattr__" : "__delattr__";
    call_hook_advisory_obj(self, dunder_call, name, old, value);

    /* Find original pointer to call:
       1) prefer per-instance capsule stored in activation side-table
       2) else fall back to per-type original from type_orig_capsules
       3) else generic fallback */
    void *orig_ptr = NULL;

    PyObject *hooks = activation_get_hooks(self); /* newref or NULL */
    if (hooks) {
        const char *inst_key = value ? "__orig_setattr__" : "__orig_delattr__";
        PyObject *caps = PyDict_GetItemString(hooks, inst_key); /* borrowed or NULL */
        if (caps) {
            orig_ptr = PyCapsule_GetPointer(caps, NULL);
            if (!orig_ptr && PyErr_Occurred()) {
                Py_DECREF(hooks);
                Py_XDECREF(old);
                return -1;
            }
        }
        Py_DECREF(hooks);
    } else {
        /* No side-table — not fatal, fall back below */
        PyErr_Clear();
    }

    if (!orig_ptr && type_orig_capsules) {
        PyObject *caps_type = PyDict_GetItem(type_orig_capsules, (PyObject *)Py_TYPE(self)); /* borrowed or NULL */
        if (caps_type) {
            orig_ptr = PyCapsule_GetPointer(caps_type, NULL);
            if (!orig_ptr && PyErr_Occurred()) {
                Py_XDECREF(old);
                return -1;
            }
        }
    }

    int rc = -1;
    if (orig_ptr) {
        setattrofunc orig = (setattrofunc)orig_ptr;
        if (orig) {
            rc = orig(self, name, value);
        } else {
            /* defensive fallback */
            if (value == NULL) rc = PyObject_GenericSetAttr(self, name, NULL);
            else rc = PyObject_GenericSetAttr(self, name, value);
        }
    } else {
        /* No saved original found — use generic behavior */
        if (value == NULL) rc = PyObject_GenericSetAttr(self, name, NULL);
        else rc = PyObject_GenericSetAttr(self, name, value);
    }

    if (rc < 0) {
        Py_XDECREF(old);
        return -1;
    }

    /* On success, call advisory hook that indicates actual mutation */
    if (value == NULL) {
        call_hook_advisory_obj(self, "__reaktome_delattr__", name, old, NULL);
    } else {
        call_hook_advisory_obj(self, "__reaktome_setattr__", name, old, value);
    }

    Py_XDECREF(old);
    return 0;
}

/* Ensure trampolines installed once per heap (user-defined) type.
   Save the real original tp_setattro in module-local dict BEFORE overwriting. */
static int
ensure_type_trampolines_installed(PyTypeObject *tp)
{
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_SetString(PyExc_RuntimeError, "patch_obj: target type is not a heap (user-defined) type");
        return -1;
    }

    /* Guard: if already patched, nothing to do */
    PyObject *already = PyObject_GetAttrString((PyObject *)tp, "__reaktome_type_patched__");
    if (already) {
        Py_DECREF(already);
        return 0;
    } else {
        if (PyErr_Occurred()) PyErr_Clear();
    }

    /* Prepare module-local dict if needed */
    if (!type_orig_capsules) {
        type_orig_capsules = PyDict_New();
        if (!type_orig_capsules) return -1;
    }

    /* If no per-type original stored yet, capture the current tp_setattro as original */
    if (!PyDict_GetItem(type_orig_capsules, (PyObject *)tp)) {
        if (tp->tp_setattro) {
            PyObject *caps = PyCapsule_New((void *)tp->tp_setattro, NULL, NULL);
            if (!caps) return -1;
            if (PyDict_SetItem(type_orig_capsules, (PyObject *)tp, caps) < 0) {
                Py_DECREF(caps);
                return -1;
            }
            Py_DECREF(caps); /* dict owns it */
        } else {
            /* If type has no tp_setattro, we won't store anything; trampoline will use generic fallback */
        }
    }

    /* Now install our trampoline into the type slot */
    tp->tp_setattro = tramp_tp_setattro;

    /* Mark the type as patched (sentinel). We don't store originals on the type object. */
    if (PyObject_SetAttrString((PyObject *)tp, "__reaktome_type_patched__", Py_True) < 0) {
        PyErr_Clear();
    }

    PyType_Modified(tp);
    return 0;
}

/* Store the current type tp_setattro pointer into the activation side-table for inst,
   but avoid capturing the trampoline itself: if the type is already patched (i.e. its
   tp_setattro == tramp_tp_setattro), use the per-type capsule saved in type_orig_capsules. */
static int
store_type_slot_originals_in_side_table(PyObject *inst)
{
    PyTypeObject *tp = Py_TYPE(inst);
    PyObject *orig_dict = PyDict_New();
    if (!orig_dict) return -1;

    void *orig_ptr_for_instance = NULL;

    /* If type's slot equals the trampoline, get original from type_orig_capsules */
    if (tp->tp_setattro == tramp_tp_setattro) {
        if (type_orig_capsules) {
            PyObject *caps_type = PyDict_GetItem(type_orig_capsules, (PyObject *)tp); /* borrowed or NULL */
            if (caps_type) {
                orig_ptr_for_instance = PyCapsule_GetPointer(caps_type, NULL);
                if (!orig_ptr_for_instance && PyErr_Occurred()) {
                    Py_DECREF(orig_dict);
                    return -1;
                }
            }
        }
    } else {
        /* Type not patched yet; capture the current tp_setattro (may be NULL) */
        if (tp->tp_setattro) {
            orig_ptr_for_instance = (void *)tp->tp_setattro;
        }
    }

    if (orig_ptr_for_instance) {
        PyObject *cset = PyCapsule_New(orig_ptr_for_instance, NULL, NULL);
        if (!cset) { Py_DECREF(orig_dict); return -1; }
        if (PyDict_SetItemString(orig_dict, "__orig_setattr__", cset) < 0) {
            Py_DECREF(cset);
            Py_DECREF(orig_dict);
            return -1;
        }
        Py_DECREF(cset);

        PyObject *cdel = PyCapsule_New(orig_ptr_for_instance, NULL, NULL);
        if (!cdel) { Py_DECREF(orig_dict); return -1; }
        if (PyDict_SetItemString(orig_dict, "__orig_delattr__", cdel) < 0) {
            Py_DECREF(cdel);
            Py_DECREF(orig_dict);
            return -1;
        }
        Py_DECREF(cdel);
    }

    if (PyDict_Size(orig_dict) > 0) {
        if (activation_merge(inst, orig_dict) < 0) {
            Py_DECREF(orig_dict);
            return -1;
        }
    }

    Py_DECREF(orig_dict);
    return 0;
}

/* py_patch_obj(instance, dunders) */
static PyObject *
py_patch_obj(PyObject *self, PyObject *args)
{
    PyObject *inst;
    PyObject *dunders;
    if (!PyArg_ParseTuple(args, "OO:patch_obj", &inst, &dunders))
        return NULL;

    if (!PyObject_HasAttrString(inst, "__dict__")) {
        PyErr_SetString(PyExc_TypeError, "patch_obj: instance has no __dict__");
        return NULL;
    }

    /* 1) store per-instance originals (safe: uses type_orig_capsules if type already patched) */
    if (store_type_slot_originals_in_side_table(inst) < 0) {
        return NULL;
    }

    /* 2) install trampoline on the instance's type (heap types only) */
    PyTypeObject *tp = Py_TYPE(inst);
    if (ensure_type_trampolines_installed(tp) < 0) {
        return NULL;
    }

    /* 3) finally merge user-supplied dunders into activation side-table */
    if (activation_merge(inst, dunders) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

/* Module method table and registration */
static PyMethodDef obj_methods[] = {
    {"patch_obj", (PyCFunction)py_patch_obj, METH_VARARGS, "Activate object instance with dunders (or None to clear)"},
    {NULL, NULL, 0, NULL}
};

int
reaktome_patch_obj(PyObject *m)
{
    if (!m) return -1;
    if (PyModule_AddFunctions(m, obj_methods) < 0) return -1;
    return 0;
}
