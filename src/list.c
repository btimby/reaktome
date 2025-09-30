/* src/list.c */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "activation.h"
#include "reaktome.h"

/* ---------- saved original slot pointers ---------- */
static int (*orig_sq_ass_item)(PyObject *, Py_ssize_t, PyObject *) = NULL;
#if PY_VERSION_HEX >= 0x03090000
static int (*orig_mp_ass_subscript)(PyObject *, PyObject *, PyObject *) = NULL;
#else
static int (*orig_sq_ass_slice)(PyObject *, Py_ssize_t, Py_ssize_t, PyObject *) = NULL;
#endif

/* ---------- helper: call hook but swallow errors (advisory) ---------- */
static inline void
call_hook_advisory(PyObject *self,
                   const char *name,
                   PyObject *key,
                   PyObject *old,
                   PyObject *newv)
{
    if (reaktome_call_dunder(self, name, key, old, newv) < 0) {
        PyErr_Clear();
    }
}

/* ---------- slot trampolines ---------- */

/* sq_ass_item trampoline: obj[i] = v  and del obj[i] */
static int
tramp_sq_ass_item(PyObject *self, Py_ssize_t i, PyObject *v)
{
    if (v == NULL) {
        PyObject *old = PySequence_GetItem(self, i); /* new ref */
        if (!old) return -1;

        int rc;
        if (orig_sq_ass_item) rc = orig_sq_ass_item(self, i, NULL);
        else rc = PySequence_DelItem(self, i);

        if (rc < 0) { Py_DECREF(old); return -1; }

        PyObject *key = PyLong_FromSsize_t(i);
        if (key) {
            call_hook_advisory(self, "__reaktome_delitem__", key, old, NULL);
            Py_DECREF(key);
        }
        Py_DECREF(old);
        return 0;
    } else {
        PyObject *old = PySequence_GetItem(self, i); /* new ref */
        if (!old) return -1;

        int rc;
        if (orig_sq_ass_item) rc = orig_sq_ass_item(self, i, v);
        else rc = PySequence_SetItem(self, i, v);

        if (rc < 0) { Py_DECREF(old); return -1; }

        PyObject *key = PyLong_FromSsize_t(i);
        if (key) {
            call_hook_advisory(self, "__reaktome_additem__", key, old, v);
            Py_DECREF(key);
        }
        Py_DECREF(old);
        return 0;
    }
}

#if PY_VERSION_HEX >= 0x03090000
static int
tramp_mp_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
    /* integer index */
    if (PyIndex_Check(key)) {
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) return -1;

        if (value == NULL) {
            PyObject *old = PySequence_GetItem(self, idx);
            if (!old) return -1;

            int rc = -1;
            if (orig_mp_ass_subscript) rc = orig_mp_ass_subscript(self, key, NULL);
            else rc = PySequence_DelItem(self, idx);

            if (rc < 0) { Py_DECREF(old); return -1; }

            call_hook_advisory(self, "__reaktome_delitem__", key, old, NULL);
            Py_DECREF(old);
            return 0;
        } else {
            PyObject *old = PySequence_GetItem(self, idx);
            if (!old) return -1;

            int rc = -1;
            if (orig_mp_ass_subscript) rc = orig_mp_ass_subscript(self, key, value);
            else rc = PySequence_SetItem(self, idx, value);

            if (rc < 0) { Py_DECREF(old); return -1; }

            call_hook_advisory(self, "__reaktome_additem__", key, old, value);
            Py_DECREF(old);
            return 0;
        }
    }

    /* slice */
    if (PySlice_Check(key)) {
        PyObject *old_slice = PyObject_GetItem(self, key); /* newref */
        if (!old_slice && PyErr_Occurred()) return -1;

        int rc = -1;
        if (orig_mp_ass_subscript) rc = orig_mp_ass_subscript(self, key, value);
        else {
            if (value == NULL) rc = PyObject_DelItem(self, key);
            else rc = PyObject_SetItem(self, key, value);
        }
        if (rc < 0) { Py_XDECREF(old_slice); return -1; }

        if (value && PySequence_Check(value)) {
            Py_ssize_t n = PySequence_Size(value);
            for (Py_ssize_t j = 0; j < n; j++) {
                PyObject *new_item = PySequence_GetItem(value, j);
                if (!new_item) { Py_XDECREF(old_slice); return -1; }
                call_hook_advisory(self, "__reaktome_additem__", NULL, NULL, new_item);
                Py_DECREF(new_item);
            }
        }

        if (old_slice && PySequence_Check(old_slice)) {
            Py_ssize_t n = PySequence_Size(old_slice);
            for (Py_ssize_t j = 0; j < n; j++) {
                PyObject *old_item = PySequence_GetItem(old_slice, j);
                if (!old_item) { Py_DECREF(old_slice); return -1; }
                call_hook_advisory(self, "__reaktome_delitem__", NULL, old_item, NULL);
                Py_DECREF(old_item);
            }
        }
        Py_XDECREF(old_slice);
        return 0;
    }

    if (value == NULL) return PyObject_DelItem(self, key);
    return PyObject_SetItem(self, key, value);
}
#else
static int
tramp_sq_ass_slice(PyObject *self, Py_ssize_t i, Py_ssize_t j, PyObject *v)
{
    PyObject *old_slice = PyList_GetSlice(self, i, j); /* newref */
    if (!old_slice && PyErr_Occurred()) return -1;

    if (PyList_SetSlice(self, i, j, v) < 0) { Py_XDECREF(old_slice); return -1; }

    if (v && PySequence_Check(v)) {
        Py_ssize_t n = PySequence_Size(v);
        for (Py_ssize_t t = 0; t < n; t++) {
            PyObject *it = PySequence_GetItem(v, t);
            if (!it) { Py_XDECREF(old_slice); return -1; }
            call_hook_advisory(self, "__reaktome_additem__", NULL, NULL, it);
            Py_DECREF(it);
        }
    }
    if (old_slice && PySequence_Check(old_slice)) {
        Py_ssize_t n = PySequence_Size(old_slice);
        for (Py_ssize_t t = 0; t < n; t++) {
            PyObject *it = PySequence_GetItem(old_slice, t);
            if (!it) { Py_DECREF(old_slice); return -1; }
            call_hook_advisory(self, "__reaktome_delitem__", NULL, it, NULL);
            Py_DECREF(it);
        }
    }
    Py_XDECREF(old_slice);
    return 0;
}
#endif

/* ---------- method trampolines using the C-API ---------- */

static PyObject *
tramp_append(PyObject *self, PyObject *arg)
{
    if (PyList_Append(self, arg) < 0) return NULL;

    if (reaktome_call_dunder(self, "__reaktome_additem__", NULL, NULL, arg) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
tramp_extend(PyObject *self, PyObject *iterable)
{
    PyObject *it = PyObject_GetIter(iterable);
    if (!it) return NULL;
    PyObject *item;
    while ((item = PyIter_Next(it))) {
        if (PyList_Append(self, item) < 0) { Py_DECREF(item); Py_DECREF(it); return NULL; }
        call_hook_advisory(self, "__reaktome_additem__", NULL, NULL, item);
        Py_DECREF(item);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
tramp_insert(PyObject *self, PyObject *args)
{
    Py_ssize_t idx;
    PyObject *val;
    if (!PyArg_ParseTuple(args, "nO:insert", &idx, &val)) return NULL;
    if (PyList_Insert(self, idx, val) < 0) return NULL;
    PyObject *key = PyLong_FromSsize_t(idx);
    if (key) {
        call_hook_advisory(self, "__reaktome_additem__", key, NULL, val);
        Py_DECREF(key);
    }
    Py_RETURN_NONE;
}

static PyObject *
tramp_pop(PyObject *self, PyObject *args)
{
    Py_ssize_t idx = -1;
    if (!PyArg_ParseTuple(args, "|n:pop", &idx)) return NULL;
    Py_ssize_t n = PyList_GET_SIZE(self);
    if (n == 0) { PyErr_SetString(PyExc_IndexError, "pop from empty list"); return NULL; }
    if (idx == -1) idx = n - 1;
    if (idx < 0) idx += n;
    if (idx < 0 || idx >= n) { PyErr_SetString(PyExc_IndexError, "pop index out of range"); return NULL; }

    PyObject *old = PySequence_GetItem(self, idx);
    if (!old) return NULL;

    int rc;
    if (orig_sq_ass_item) rc = orig_sq_ass_item(self, idx, NULL);
    else rc = PyList_SetSlice(self, idx, idx+1, NULL);

    if (rc < 0) { Py_DECREF(old); return NULL; }

    PyObject *key = PyLong_FromSsize_t(idx);
    if (key) {
        call_hook_advisory(self, "__reaktome_delitem__", key, old, NULL);
        Py_DECREF(key);
    }
    return old; /* newref */
}

static PyObject *
tramp_remove(PyObject *self, PyObject *arg)
{
    Py_ssize_t n = PyList_GET_SIZE(self);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PyList_GET_ITEM(self, i); /* borrowed */
        int eq = PyObject_RichCompareBool(it, arg, Py_EQ);
        if (eq < 0) return NULL;
        if (eq > 0) {
            PyObject *old = Py_NewRef(it);
            int rc;
            if (orig_sq_ass_item) rc = orig_sq_ass_item(self, i, NULL);
            else rc = PySequence_DelItem(self, i);
            if (rc < 0) { Py_DECREF(old); return NULL; }
            PyObject *key = PyLong_FromSsize_t(i);
            if (key) {
                call_hook_advisory(self, "__reaktome_delitem__", key, old, NULL);
                Py_DECREF(key);
            }
            Py_DECREF(old);
            Py_RETURN_NONE;
        }
    }
    PyErr_SetString(PyExc_ValueError, "list.remove(x): x not in list");
    return NULL;
}

static PyObject *
tramp_clear(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    Py_ssize_t n = PyList_GET_SIZE(self);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *old = Py_NewRef(PyList_GET_ITEM(self, i));
        PyObject *key = PyLong_FromSsize_t(i);
        if (key) {
            call_hook_advisory(self, "__reaktome_delitem__", key, old, NULL);
            Py_DECREF(key);
        }
        Py_DECREF(old);
    }
    if (PyList_SetSlice(self, 0, n, NULL) < 0) return NULL;
    Py_RETURN_NONE;
}

/* ---------- static PyMethodDef objects (file scope) ---------- */
static PyMethodDef append_def  = {"append",  (PyCFunction)tramp_append,  METH_O,       "append (trampoline)"};
static PyMethodDef extend_def  = {"extend",  (PyCFunction)tramp_extend,  METH_O,       "extend (trampoline)"};
static PyMethodDef insert_def  = {"insert",  (PyCFunction)tramp_insert,  METH_VARARGS, "insert (trampoline)"};
static PyMethodDef pop_def     = {"pop",     (PyCFunction)tramp_pop,     METH_VARARGS, "pop (trampoline)"};
static PyMethodDef remove_def  = {"remove",  (PyCFunction)tramp_remove,  METH_O,       "remove (trampoline)"};
static PyMethodDef clear_def   = {"clear",   (PyCFunction)tramp_clear,   METH_NOARGS,  "clear (trampoline)"};

/* ---------- helper: ensure trampolines installed once per type ---------- */

static int
ensure_list_type_patched(PyTypeObject *tp)
{
    static int already_patched = 0;

    if (!tp) {
        fprintf(stderr, "ensure_list_type_patched: NULL type\n");
        PyErr_SetString(PyExc_SystemError, "ensure_list_type_patched: NULL type");
        return -1;
    }

    /* Ensure the type is initialized */
    if (PyType_Ready(tp) < 0) {
        return -1;
    }

    /* If we've already patched, just ensure original slots are saved */
    if (already_patched) {
        PySequenceMethods *sq = tp->tp_as_sequence;
        if (sq && orig_sq_ass_item == NULL) orig_sq_ass_item = sq->sq_ass_item;
#if PY_VERSION_HEX < 0x03090000
        if (sq && orig_sq_ass_slice == NULL) orig_sq_ass_slice = sq->sq_ass_slice;
#endif
#if PY_VERSION_HEX >= 0x03090000
        PyMappingMethods *mp = tp->tp_as_mapping;
        if (mp && orig_mp_ass_subscript == NULL) orig_mp_ass_subscript = mp->mp_ass_subscript;
#endif
        return 0;
    }

    /* Get the type dict (borrowed) */
    PyObject *dict = PyType_GetDict(tp);  /* borrowed */
    if (dict == NULL) {
        /* Defensive: this should not happen after PyType_Ready for builtins, but check */
        fprintf(stderr, "ensure_list_type_patched: PyType_GetDict returned NULL for %p\n", (void*)tp);
        PyErr_SetString(PyExc_RuntimeError, "list type has no tp_dict after PyType_Ready");
        return -1;
    }

    /* Install method descriptors into type dict */
    #define INSTALL_DEF_IN_DICT(defptr, name_lit)                             \
        do {                                                                  \
            PyObject *func = (PyObject *)PyDescr_NewMethod(tp, (defptr));     \
            if (!func) return -1;                                             \
            if (PyDict_SetItemString(dict, (name_lit), func) < 0) {           \
                Py_DECREF(func);                                              \
                return -1;                                                    \
            }                                                                 \
            Py_DECREF(func); /* dict owns the ref */                          \
        } while (0)

    INSTALL_DEF_IN_DICT(&append_def,  "append");
    INSTALL_DEF_IN_DICT(&extend_def,  "extend");
    INSTALL_DEF_IN_DICT(&insert_def,  "insert");
    INSTALL_DEF_IN_DICT(&pop_def,     "pop");
    INSTALL_DEF_IN_DICT(&remove_def,  "remove");
    INSTALL_DEF_IN_DICT(&clear_def,   "clear");

    #undef INSTALL_DEF_IN_DICT

    /* Save original slot pointers then install our trampolines. */
    PySequenceMethods *sq = tp->tp_as_sequence;
    if (sq) {
        orig_sq_ass_item = sq->sq_ass_item;
        sq->sq_ass_item = tramp_sq_ass_item;
#if PY_VERSION_HEX < 0x03090000
        orig_sq_ass_slice = sq->sq_ass_slice;
        sq->sq_ass_slice = tramp_sq_ass_slice;
#endif
    }

#if PY_VERSION_HEX >= 0x03090000
    PyMappingMethods *mp = tp->tp_as_mapping;
    if (mp) {
        orig_mp_ass_subscript = mp->mp_ass_subscript;
        mp->mp_ass_subscript = tramp_mp_ass_subscript;
    }
#endif

    /* Tell runtime the type dict changed - PyType_Modified returns void, but call for correctness */
    PyType_Modified(tp);

    already_patched = 1;
    return 0;
}

/* ---------- Python wrapper: py_patch_list(instance, dunders) ---------- */
static PyObject *
py_patch_list(PyObject *self, PyObject *args)
{
    PyObject *inst;
    PyObject *dunders;
    if (!PyArg_ParseTuple(args, "OO:patch_list", &inst, &dunders))
        return NULL;

    if (!PyList_Check(inst)) {
        PyErr_SetString(PyExc_TypeError, "patch_list: expected list instance");
        return NULL;
    }

    /* Ensure trampolines installed for the list type */
    if (ensure_list_type_patched(Py_TYPE(inst)) < 0) {
        return NULL;
    }

    /* Merge hooks for this instance (activation side-table). dunders may be None to clear. */
    if (activation_merge(inst, dunders) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

/* Module-level method table for list patch */
static PyMethodDef list_module_methods[] = {
    {"patch_list", (PyCFunction)py_patch_list, METH_VARARGS, "Activate list instance with dunders (or None to clear)"},
    {NULL, NULL, 0, NULL}
};

/* Called from reaktome.c to register patch_list into the module */
int
reaktome_patch_list(PyObject *m)
{
    if (!m) return -1;
    if (PyModule_AddFunctions(m, list_module_methods) < 0) return -1;
    return 0;
}
