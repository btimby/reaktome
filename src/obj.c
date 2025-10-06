/* src/obj.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/*
 obj.c — implementation that:
  - installs a single trampoline into tp_setattro for heap types,
  - stores the real original tp_setattro per-type in module-local dict
    BEFORE replacing the type slot (so we never record the trampoline itself),
  - stores per-instance originals in the activation side-table (capsules),
  - trampoline uses per-instance capsule if present, else falls back to per-type original,
  - trampoline calls original (or generic fallback) then advisory hooks via reaktome_call_dunder,
  - additionally installs method wrappers (append/extend/insert/pop/remove/clear)
    into heap types' tp_dict for list-like behaviour; saved original method objects
    are kept in `type_orig_methods`.
*/

/* module-local dict mapping type -> capsule(original tp_setattro). newref or NULL */
static PyObject *type_orig_capsules = NULL;

/* module-local dict mapping type -> dict(method_name -> original_method_obj). newref or NULL */
static PyObject *type_orig_methods = NULL;

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

/* ---------- getattr helper: get per-instance hooks dict (newref or NULL no-exc) ---------- */
/* activation_get_hooks already supplies a newref or NULL and no exception on none; reuse it. */

/* ---------- Trampoline installed into tp_setattro (handles both setattr and delattr) ---------- */
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

    /* Advisory pre-mutation hook: distinguish between setattr and delattr */
    const char *dunder_call = (value == NULL) ? "__delattr__" : "__setattr__";
    call_hook_advisory_obj(self, dunder_call, name, old, value);

    /* Find original pointer to call */
    void *orig_ptr = NULL;

    PyObject *hooks = activation_get_hooks(self); /* newref or NULL */
    if (hooks) {
        const char *inst_key = (value == NULL) ? "__orig_delattr__" : "__orig_setattr__";
        PyObject *caps = PyDict_GetItemString(hooks, inst_key); /* borrowed */
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
        PyErr_Clear();
    }

    if (!orig_ptr && type_orig_capsules) {
        PyObject *caps_type = PyDict_GetItem(type_orig_capsules, (PyObject *)Py_TYPE(self)); /* borrowed */
        if (caps_type) {
            orig_ptr = PyCapsule_GetPointer(caps_type, NULL);
            if (!orig_ptr && PyErr_Occurred()) {
                Py_XDECREF(old);
                return -1;
            }
        }
    }

    int rc;
    if (orig_ptr) {
        setattrofunc orig = (setattrofunc)orig_ptr;
        rc = orig(self, name, value);
    } else {
        /* Fallback to generic if no original found */
        rc = (value == NULL)
            ? PyObject_GenericSetAttr(self, name, NULL)   /* true delattr */
            : PyObject_GenericSetAttr(self, name, value); /* setattr */
    }

    if (rc < 0) {
        Py_XDECREF(old);
        return -1;
    }

    /* Post-mutation hook: distinguish between actual setattr vs actual delattr.
       This is where we need __reaktome_delattr__, otherwise setattr(x, None)
       and delattr(x) would look identical. */
    if (value == NULL) {
        call_hook_advisory_obj(self, "__reaktome_delattr__", name, old, NULL);
    } else {
        call_hook_advisory_obj(self, "__reaktome_setattr__", name, old, value);
    }

    Py_XDECREF(old);
    return 0;
}

/* ---------- method trampolines for heap types (append/extend/insert/pop/remove/clear) ---------- */

/* For each tramp we look up the saved original method object in type_orig_methods[type][name]
   (borrowed) and call it with self and the appropriate args. After the original succeeds,
   call advisory hooks (__reaktome_setitem__/__reaktome_delitem__) using call_hook_advisory_obj.
   Advisory errors are swallowed. */

/* Helper: get saved original method for this type and name (borrowed or NULL) */
static PyObject *
get_saved_method(PyTypeObject *tp, const char *name)
{
    if (!type_orig_methods) return NULL;
    PyObject *per = PyDict_GetItem(type_orig_methods, (PyObject *)tp); /* borrowed */
    if (!per) return NULL;
    return PyDict_GetItemString(per, name); /* borrowed or NULL */
}

/* append(self, obj) */
static PyObject *
tramp_append(PyObject *self, PyObject *arg)
{
    PyObject *orig = get_saved_method(Py_TYPE(self), "append");
    PyObject *res;

    if (orig) {
        /* call original: orig(self, arg) */
        res = PyObject_CallFunctionObjArgs(orig, self, arg, NULL);
    } else {
        /* fallback to attribute lookup and call */
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "append");
        if (!callable) return NULL;
        res = PyObject_CallFunctionObjArgs(callable, self, arg, NULL);
        Py_DECREF(callable);
    }

    if (!res) return NULL; /* propagate exception */

    /* after success, call setitem advisory with new value arg; key unknown for sequence */
    call_hook_advisory_obj(self, "__reaktome_setitem__", NULL, NULL, arg);

    Py_DECREF(res);
    Py_RETURN_NONE;
}

/* extend(self, iterable) */
static PyObject *
tramp_extend(PyObject *self, PyObject *iterable)
{
    PyObject *orig = get_saved_method(Py_TYPE(self), "extend");
    PyObject *res;

    if (orig) {
        res = PyObject_CallFunctionObjArgs(orig, self, iterable, NULL);
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "extend");
        if (!callable) return NULL;
        res = PyObject_CallFunctionObjArgs(callable, self, iterable, NULL);
        Py_DECREF(callable);
    }

    if (!res) return NULL;

    /* If iterable is iterable, call setitem advisory for each element (best-effort) */
    if (iterable && PyObject_GetIter(iterable)) {
        PyObject *it = PyObject_GetIter(iterable);
        if (it) {
            PyObject *item;
            while ((item = PyIter_Next(it))) {
                call_hook_advisory_obj(self, "__reaktome_setitem__", NULL, NULL, item);
                Py_DECREF(item);
            }
            Py_DECREF(it);
            if (PyErr_Occurred()) PyErr_Clear();
        }
    }

    Py_DECREF(res);
    Py_RETURN_NONE;
}

/* insert(self, index, value) */
static PyObject *
tramp_insert(PyObject *self, PyObject *args)
{
    Py_ssize_t idx;
    PyObject *val;
    if (!PyArg_ParseTuple(args, "nO:insert", &idx, &val)) return NULL;

    PyObject *orig = get_saved_method(Py_TYPE(self), "insert");
    PyObject *res;

    if (orig) {
        PyObject *iobj = PyLong_FromSsize_t(idx);
        if (!iobj) return NULL;
        res = PyObject_CallFunctionObjArgs(orig, self, iobj, val, NULL);
        Py_DECREF(iobj);
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "insert");
        if (!callable) return NULL;
        res = PyObject_CallFunctionObjArgs(callable, self, PyLong_FromSsize_t(idx), val, NULL);
        Py_DECREF(callable);
    }

    if (!res) return NULL;

    PyObject *key = PyLong_FromSsize_t(idx);
    if (key) {
        call_hook_advisory_obj(self, "__reaktome_setitem__", key, NULL, val);
        Py_DECREF(key);
    }

    Py_DECREF(res);
    Py_RETURN_NONE;
}

/* pop(self[, index]) -> returns popped value */
static PyObject *
tramp_pop(PyObject *self, PyObject *args)
{
    Py_ssize_t idx = -1;
    PyObject *res = NULL;
    int have_index = 0;

    if (!PyArg_ParseTuple(args, "|n:pop", &idx)) return NULL;

    if (idx != -1) have_index = 1;

    PyObject *orig = get_saved_method(Py_TYPE(self), "pop");
    if (orig) {
        if (have_index) {
            PyObject *iobj = PyLong_FromSsize_t(idx);
            if (!iobj) return NULL;
            res = PyObject_CallFunctionObjArgs(orig, self, iobj, NULL);
            Py_DECREF(iobj);
        } else {
            res = PyObject_CallFunctionObjArgs(orig, self, NULL);
        }
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "pop");
        if (!callable) return NULL;
        if (have_index) {
            PyObject *iobj = PyLong_FromSsize_t(idx);
            if (!iobj) { Py_DECREF(callable); return NULL; }
            res = PyObject_CallFunctionObjArgs(callable, self, iobj, NULL);
            Py_DECREF(iobj);
        } else {
            res = PyObject_CallFunctionObjArgs(callable, self, NULL);
        }
        Py_DECREF(callable);
    }

    if (!res) return NULL; /* exception */

    /* res is the popped value (old). Fire del hook */
    call_hook_advisory_obj(self, "__reaktome_delitem__", NULL, res, NULL);

    return res; /* newref returned to caller */
}

/* remove(self, value) */
static PyObject *
tramp_remove(PyObject *self, PyObject *arg)
{
    PyObject *orig = get_saved_method(Py_TYPE(self), "remove");
    PyObject *res;

    if (orig) {
        res = PyObject_CallFunctionObjArgs(orig, self, arg, NULL);
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "remove");
        if (!callable) return NULL;
        res = PyObject_CallFunctionObjArgs(callable, self, arg, NULL);
        Py_DECREF(callable);
    }

    if (!res) return NULL;

    /* remove triggers a delitem advisory for the removed element (we don't have index) */
    call_hook_advisory_obj(self, "__reaktome_delitem__", NULL, arg, NULL);

    Py_DECREF(res);
    Py_RETURN_NONE;
}

/* clear(self) */
static PyObject *
tramp_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    /* Snapshot items if possible and call del hooks per item */
    PyObject *items = NULL;
    if (PyObject_HasAttrString(self, "__iter__")) {
        items = PySequence_List(self); /* newref or NULL; best-effort */
        if (!items && PyErr_Occurred()) PyErr_Clear();
    }

    PyObject *orig = get_saved_method(Py_TYPE(self), "clear");
    PyObject *res;
    if (orig) {
        res = PyObject_CallFunctionObjArgs(orig, self, NULL);
    } else {
        PyObject *callable = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "clear");
        if (!callable) {
            /* fallback to generic clearing */
            if (PyObject_HasAttrString(self, "clear")) {
                PyObject *c = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "clear");
                if (c) {
                    res = PyObject_CallFunctionObjArgs(c, self, NULL);
                    Py_DECREF(c);
                } else {
                    res = NULL;
                }
            } else {
                /* try to clear via setting slice or attributes is type-specific */
                res = NULL;
            }
        } else {
            res = PyObject_CallFunctionObjArgs(callable, self, NULL);
            Py_DECREF(callable);
        }
    }

    if (!res) return NULL;

    /* Fire del hooks for each old item we managed to take a snapshot of */
    if (items && PyList_Check(items)) {
        Py_ssize_t n = PyList_Size(items);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *it = PyList_GetItem(items, i); /* borrowed */
            if (!it) continue;
            call_hook_advisory_obj(self, "__reaktome_delitem__", NULL, it, NULL);
        }
        Py_DECREF(items);
    }

    Py_DECREF(res);
    Py_RETURN_NONE;
}

/* ---------- static PyMethodDef objects for wrappers (file scope) ---------- */
static PyMethodDef append_def  = {"append",  (PyCFunction)tramp_append,  METH_O,       "append (trampoline)"};
static PyMethodDef extend_def  = {"extend",  (PyCFunction)tramp_extend,  METH_O,       "extend (trampoline)"};
static PyMethodDef insert_def  = {"insert",  (PyCFunction)tramp_insert,  METH_VARARGS, "insert (trampoline)"};
static PyMethodDef pop_def     = {"pop",     (PyCFunction)tramp_pop,     METH_VARARGS, "pop (trampoline)"};
static PyMethodDef remove_def  = {"remove",  (PyCFunction)tramp_remove,  METH_O,       "remove (trampoline)"};
static PyMethodDef clear_def   = {"clear",   (PyCFunction)tramp_clear,   METH_NOARGS,  "clear (trampoline)"};

/* ---------- Ensure trampolines installed once per heap (user-defined) type. ---------- */
static int
ensure_type_trampolines_installed(PyTypeObject *tp)
{
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "patch_obj: target type is not a heap (user-defined) type");
        return -1;
    }

    /* Guard: if already patched, nothing to do */
    PyObject *already = PyObject_GetAttrString((PyObject *)tp, "__reaktome_type_patched__");
    if (already) {
        Py_DECREF(already);
        return 0;
    } else if (PyErr_Occurred()) {
        PyErr_Clear();
    }

    /* prepare per-type modules maps */
    if (!type_orig_capsules) {
        type_orig_capsules = PyDict_New();
        if (!type_orig_capsules) return -1;
    }
    if (!type_orig_methods) {
        type_orig_methods = PyDict_New();
        if (!type_orig_methods) return -1;
    }

    /* Save per-type original tp_setattro (capsule) if not present */
    if (!PyDict_GetItem(type_orig_capsules, (PyObject *)tp)) {
        if (tp->tp_setattro) {
            PyObject *caps = PyCapsule_New((void *)tp->tp_setattro, NULL, NULL);
            if (!caps) return -1;
            if (PyDict_SetItem(type_orig_capsules, (PyObject *)tp, caps) < 0) {
                Py_DECREF(caps);
                return -1;
            }
            Py_DECREF(caps);
        }
    }

    /* Install tp_setattro trampoline */
    tp->tp_setattro = tramp_tp_setattro;

    /* Install method wrapper descriptors for list-like methods if present on the type.
       Save the original method object (descriptor) into a per-type dict first. */
    PyObject *tp_dict = PyType_GetDict(tp); /* borrowed */
    if (!tp_dict) {
        PyErr_SetString(PyExc_RuntimeError, "patch_obj: type has no tp_dict after PyType_Ready");
        return -1;
    }

    /* helper macro: save original attribute on type into type_orig_methods[type][name] */
    #define SAVE_ORIG_METHOD_IF_PRESENT(name_lit)                                       \
        do {                                                                            \
            if (PyObject_HasAttrString((PyObject *)tp, (name_lit))) {                   \
                PyObject *orig = PyObject_GetAttrString((PyObject *)tp, (name_lit));    \
                if (orig) {                                                             \
                    /* ensure per-type dict exists */                                   \
                    PyObject *per = PyDict_GetItem(type_orig_methods, (PyObject *)tp);  \
                    if (!per) {                                                         \
                        per = PyDict_New();                                             \
                        if (!per) { Py_DECREF(orig); return -1; }                       \
                        if (PyDict_SetItem(type_orig_methods, (PyObject *)tp, per) < 0) {\
                            Py_DECREF(per); Py_DECREF(orig); return -1;                 \
                        }                                                               \
                        Py_DECREF(per);                                                 \
                    }                                                                   \
                    /* store original under name_lit */                                 \
                    PyObject *perdict = PyDict_GetItem(type_orig_methods, (PyObject *)tp); /* borrowed */ \
                    if (PyDict_SetItemString(perdict, (name_lit), orig) < 0) {          \
                        Py_DECREF(orig); return -1;                                     \
                    }                                                                   \
                    Py_DECREF(orig);                                                    \
                }                                                                       \
            }                                                                           \
        } while (0)

    /* Save originals for methods we will wrap */
    SAVE_ORIG_METHOD_IF_PRESENT("append");
    SAVE_ORIG_METHOD_IF_PRESENT("extend");
    SAVE_ORIG_METHOD_IF_PRESENT("insert");
    SAVE_ORIG_METHOD_IF_PRESENT("pop");
    SAVE_ORIG_METHOD_IF_PRESENT("remove");
    SAVE_ORIG_METHOD_IF_PRESENT("clear");

    #undef SAVE_ORIG_METHOD_IF_PRESENT

    /* helper macro: install wrapper descriptor into type dict */
    #define INSTALL_DEF_IN_DICT(defptr, name_lit)                             \
        do {                                                                  \
            PyObject *func = (PyObject *)PyDescr_NewMethod(tp, (defptr));     \
            if (!func) return -1;                                             \
            if (PyDict_SetItemString(tp_dict, (name_lit), func) < 0) {        \
                Py_DECREF(func);                                              \
                return -1;                                                    \
            }                                                                 \
            Py_DECREF(func); /* dict owns the ref */                          \
        } while (0)

    /* Only install wrappers for methods that exist on the type (we saved originals above) */
    if (type_orig_methods && PyDict_GetItem(type_orig_methods, (PyObject *)tp)) {
        PyObject *per = PyDict_GetItem(type_orig_methods, (PyObject *)tp); /* borrowed */
        if (per) {
            if (PyDict_GetItemString(per, "append"))  INSTALL_DEF_IN_DICT(&append_def,  "append");
            if (PyDict_GetItemString(per, "extend"))  INSTALL_DEF_IN_DICT(&extend_def,  "extend");
            if (PyDict_GetItemString(per, "insert"))  INSTALL_DEF_IN_DICT(&insert_def,  "insert");
            if (PyDict_GetItemString(per, "pop"))     INSTALL_DEF_IN_DICT(&pop_def,     "pop");
            if (PyDict_GetItemString(per, "remove"))  INSTALL_DEF_IN_DICT(&remove_def,  "remove");
            if (PyDict_GetItemString(per, "clear"))   INSTALL_DEF_IN_DICT(&clear_def,   "clear");
        }
    }

    #undef INSTALL_DEF_IN_DICT

    /* Mark the type as patched (sentinel). We don't store originals on the type object. */
    if (PyObject_SetAttrString((PyObject *)tp, "__reaktome_type_patched__", Py_True) < 0) {
        PyErr_Clear();
    }

    PyType_Modified(tp);
    return 0;
}

/* ---------- Store the type’s slot originals into the activation side-table for inst. ---------- */
static int
store_type_slot_originals_in_side_table(PyObject *inst)
{
    PyTypeObject *tp = Py_TYPE(inst);
    PyObject *orig_dict = PyDict_New();
    if (!orig_dict) return -1;

    void *orig_ptr_for_instance = NULL;

    if (tp->tp_setattro == tramp_tp_setattro) {
        if (type_orig_capsules) {
            PyObject *caps_type = PyDict_GetItem(type_orig_capsules, (PyObject *)tp);
            if (caps_type) {
                orig_ptr_for_instance = PyCapsule_GetPointer(caps_type, NULL);
                if (!orig_ptr_for_instance && PyErr_Occurred()) {
                    Py_DECREF(orig_dict);
                    return -1;
                }
            }
        }
    } else {
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

    /* Also capture per-type original method objects (if any) and stash them in instance side-table
       under keys so per-instance lookup prefers per-instance originals (if present). */
    if (type_orig_methods) {
        PyObject *per = PyDict_GetItem(type_orig_methods, (PyObject *)tp); /* borrowed */
        if (per) {
            PyObject *percopy = PyDict_Copy(per); /* newref */
            if (!percopy) { Py_DECREF(orig_dict); return -1; }
            /* store this mapping into instance orig_dict under key "__orig_methods__" */
            if (PyDict_SetItemString(orig_dict, "__orig_methods__", percopy) < 0) {
                Py_DECREF(percopy);
                Py_DECREF(orig_dict);
                return -1;
            }
            Py_DECREF(percopy);
        }
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

/* ---------- py_patch_obj(instance, dunders) (idempotent) ---------- */
static PyObject *
py_patch_obj(PyObject *self, PyObject *args)
{
    PyObject *inst;
    PyObject *dunders;
    if (!PyArg_ParseTuple(args, "OO:patch_obj", &inst, &dunders))
        return NULL;

    if (!PyObject_HasAttrString(inst, "__dict__")) {
        PyErr_SetString(PyExc_TypeError,
                        "patch_obj: instance has no __dict__");
        return NULL;
    }

    /* --- NEW GUARD: if already activated, just merge dunders and return --- */
    PyObject *hooks = activation_get_hooks(inst); /* newref or NULL */
    if (hooks) {
        /* Already patched: skip re-installing trampolines; just merge dunders */
        Py_DECREF(hooks);
        if (activation_merge(inst, dunders) < 0)
            return NULL;
        Py_RETURN_NONE;
    }
    PyErr_Clear(); /* in case activation_get_hooks set an exception */

    /* Not yet activated: store originals and patch type */
    if (store_type_slot_originals_in_side_table(inst) < 0)
        return NULL;

    PyTypeObject *tp = Py_TYPE(inst);
    if (ensure_type_trampolines_installed(tp) < 0)
        return NULL;

    if (activation_merge(inst, dunders) < 0)
        return NULL;

    Py_RETURN_NONE;
}

/* Module-level method table */
static PyMethodDef obj_methods[] = {
    {"patch_obj", (PyCFunction)py_patch_obj, METH_VARARGS,
     "Activate object instance with dunders (or None to clear)"},
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
