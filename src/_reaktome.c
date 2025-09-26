#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

/*
 _reaktome.c - advisory hooks for setattr / list / dict mutations with cleanup on module unload.

 Key points:
  - per-instance hooks: __reaktome_setattr__ (attrs) and __reaktome_setitem__ (list/dict items)
  - raise _reaktome.ReaktomeAbort() to silently abort mutation
  - equality filtering: if old == new then hook is not called (assignment still proceeds)
  - patch_list/patch_dict/patch_type and corresponding unpatch functions
  - module_free restores all patched slots recorded in patched_types_map
*/

typedef int (*setattrofunc)(PyObject *, PyObject *, PyObject *);
typedef int (*list_sq_ass_item_func)(PyObject *, Py_ssize_t, PyObject *);
typedef int (*dict_mp_ass_sub_func)(PyObject *, PyObject *, PyObject *);

static PyObject *patched_types_map = NULL; /* dict: typeobj -> dict{ name: capsule, ... } */
static PyObject *ReaktomeAbort = NULL;

/* ---------- Helpers for patched_types_map storage ---------- */

static int ensure_patched_types_map(void) {
    if (!patched_types_map) {
        patched_types_map = PyDict_New();
        if (!patched_types_map) return -1;
    }
    return 0;
}

/* Return new reference to inner dict (create if requested). */
static PyObject * get_type_entry_newref(PyObject *typeobj, int create_if_missing) {
    if (ensure_patched_types_map() < 0) return NULL;
    PyObject *entry = PyDict_GetItem(patched_types_map, typeobj); /* borrowed */
    if (entry) { Py_INCREF(entry); return entry; }
    if (!create_if_missing) return NULL;
    entry = PyDict_New();
    if (!entry) return NULL;
    if (PyDict_SetItem(patched_types_map, typeobj, entry) < 0) {
        Py_DECREF(entry);
        return NULL;
    }
    return entry; /* new ref */
}

/* Store a pointer as a capsule entry[name] under typeobj. */
static int store_pointer_for_type(PyObject *typeobj, const char *name, void *ptr, const char *capsule_name) {
    PyObject *entry = get_type_entry_newref(typeobj, 1); /* new ref */
    if (!entry) return -1;
    PyObject *cap = PyCapsule_New(ptr, capsule_name, NULL);
    if (!cap) { Py_DECREF(entry); return -1; }
    int ok = (PyDict_SetItemString(entry, name, cap) == 0);
    Py_DECREF(cap);
    Py_DECREF(entry);
    return ok ? 0 : -1;
}

/* Retrieve pointer stored under entry[name] as capsule; returns raw pointer or NULL */
static void * get_pointer_for_type(PyObject *typeobj, const char *name, const char *capsule_name) {
    if (!patched_types_map) return NULL;
    PyObject *entry = PyDict_GetItem(patched_types_map, typeobj); /* borrowed */
    if (!entry) return NULL;
    PyObject *cap = PyDict_GetItemString(entry, name); /* borrowed */
    if (!cap) return NULL;
    void *p = PyCapsule_GetPointer(cap, capsule_name);
    return p;
}

/* Remove named pointer from type entry */
static void remove_pointer_for_type(PyObject *typeobj, const char *name) {
    if (!patched_types_map) return;
    PyObject *entry = PyDict_GetItem(patched_types_map, typeobj); /* borrowed */
    if (!entry) return;
    /* Delete the key if present; ignore errors */
    if (PyDict_DelItemString(entry, name) < 0) PyErr_Clear();
    /* If entry becomes empty, remove the type entry itself */
    if (PyDict_Size(entry) == 0) {
        if (PyDict_DelItem(patched_types_map, typeobj) < 0) PyErr_Clear();
    }
}

/* ---------- Utility: safe getattr that returns Py_None new ref if missing ---------- */
static PyObject * safe_getattr_as_none(PyObject *obj, PyObject *nameobj) {
    PyObject *res = PyObject_GetAttr(obj, nameobj);
    if (res) return res;
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        Py_INCREF(Py_None);
        return Py_None;
    }
    return NULL;
}

/* ---------- Attribute trampoline ---------- */

static int attr_trampoline_setattro(PyObject *self, PyObject *name, PyObject *value) {
    int rc = -1;
    PyObject *hook = NULL;
    PyObject *old = NULL;
    PyObject *new_for_hook = NULL;

    old = safe_getattr_as_none(self, name); /* new ref or NULL on error */
    if (!old) return -1;

    if (value) { new_for_hook = value; Py_INCREF(new_for_hook); }
    else { new_for_hook = Py_None; Py_INCREF(new_for_hook); }

    int equal = 0;
    if (old != Py_None || new_for_hook != Py_None) {
        int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
        if (cmp < 0) goto finally;
        if (cmp == 1) equal = 1;
    } else {
        equal = 1;
    }

    if (equal) {
        setattrofunc orig = (setattrofunc)get_pointer_for_type((PyObject *)Py_TYPE(self), "orig_setattro", "reaktome.orig_setattro");
        if (orig) rc = orig(self, name, value);
        else rc = PyObject_GenericSetAttr(self, name, value);
        goto finally;
    }

    hook = PyObject_GetAttrString(self, "__reaktome_setattr__");
    if (!hook) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); hook = NULL; }
        else goto finally;
    }

    if (hook && PyCallable_Check(hook)) {
        PyObject *args = PyTuple_New(4);
        if (!args) goto finally;
        Py_INCREF(self); PyTuple_SET_ITEM(args, 0, self);
        Py_INCREF(name); PyTuple_SET_ITEM(args, 1, name);
        Py_INCREF(old); PyTuple_SET_ITEM(args, 2, old);
        Py_INCREF(new_for_hook); PyTuple_SET_ITEM(args, 3, new_for_hook);

        PyObject *res = PyObject_CallObject(hook, args);
        Py_DECREF(args);
        if (!res) {
            if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) {
                PyErr_Clear();
                rc = 0;  /* silent abort */
                goto finally;
            }
            goto finally;
        }
        Py_DECREF(res);
    }

    {
        setattrofunc orig = (setattrofunc)get_pointer_for_type((PyObject *)Py_TYPE(self), "orig_setattro", "reaktome.orig_setattro");
        if (orig) rc = orig(self, name, value);
        else rc = PyObject_GenericSetAttr(self, name, value);
    }

finally:
    Py_XDECREF(hook);
    Py_XDECREF(old);
    Py_XDECREF(new_for_hook);
    return rc;
}

/* ---------- LIST sq_ass_item trampoline ---------- */

static int list_sq_ass_item_trampoline(PyObject *self, Py_ssize_t index, PyObject *value) {
    int rc = -1;
    PyObject *hook = NULL, *old = NULL, *new_for_hook = NULL, *idxobj = NULL;

    idxobj = PyLong_FromSsize_t(index);
    if (!idxobj) return -1;

    old = PySequence_GetItem(self, index); /* new ref or NULL */
    if (!old) {
        if (PyErr_ExceptionMatches(PyExc_IndexError)) {
            PyErr_Clear();
            old = Py_None; Py_INCREF(old);
        } else { Py_DECREF(idxobj); return -1; }
    }

    if (value) { new_for_hook = value; Py_INCREF(new_for_hook); }
    else { new_for_hook = Py_None; Py_INCREF(new_for_hook); }

    int equal = 0;
    if (old != Py_None || new_for_hook != Py_None) {
        int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
        if (cmp < 0) goto finally;
        if (cmp == 1) equal = 1;
    } else equal = 1;

    if (equal) {
        list_sq_ass_item_func orig = (list_sq_ass_item_func)get_pointer_for_type((PyObject *)Py_TYPE(self), "list_sq_ass_item", "reaktome.list_sq_ass_item");
        if (orig) rc = orig(self, index, value);
        else {
            if (value) { if (PySequence_SetItem(self, index, value) < 0) rc = -1; else rc = 0; }
            else { if (PySequence_DelItem(self, index) < 0) rc = -1; else rc = 0; }
        }
        goto finally;
    }

    hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
    if (!hook) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); hook = NULL; }
        else goto finally;
    }

    if (hook && PyCallable_Check(hook)) {
        PyObject *args = PyTuple_New(4);
        if (!args) goto finally;
        Py_INCREF(self); PyTuple_SET_ITEM(args, 0, self);
        Py_INCREF(idxobj); PyTuple_SET_ITEM(args, 1, idxobj);
        Py_INCREF(old); PyTuple_SET_ITEM(args, 2, old);
        Py_INCREF(new_for_hook); PyTuple_SET_ITEM(args, 3, new_for_hook);

        PyObject *res = PyObject_CallObject(hook, args);
        Py_DECREF(args);
        if (!res) {
            if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) { PyErr_Clear(); rc = 0; goto finally; }
            goto finally;
        }
        Py_DECREF(res);
    }

    {
        list_sq_ass_item_func orig = (list_sq_ass_item_func)get_pointer_for_type((PyObject *)Py_TYPE(self), "list_sq_ass_item", "reaktome.list_sq_ass_item");
        if (orig) rc = orig(self, index, value);
        else {
            if (value) { if (PySequence_SetItem(self, index, value) < 0) rc = -1; else rc = 0; }
            else { if (PySequence_DelItem(self, index) < 0) rc = -1; else rc = 0; }
        }
    }

finally:
    Py_XDECREF(hook);
    Py_XDECREF(old);
    Py_XDECREF(new_for_hook);
    Py_XDECREF(idxobj);
    return rc;
}

/* ---------- DICT mp_ass_subscript trampoline ---------- */

static int dict_mp_ass_sub_trampoline(PyObject *self, PyObject *key, PyObject *value) {
    int rc = -1;
    PyObject *hook = NULL, *old = NULL, *new_for_hook = NULL;

    old = PyObject_GetItem(self, key); /* new ref or NULL */
    if (!old) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) { PyErr_Clear(); old = Py_None; Py_INCREF(old); }
        else return -1;
    }

    if (value) { new_for_hook = value; Py_INCREF(new_for_hook); }
    else { new_for_hook = Py_None; Py_INCREF(new_for_hook); }

    int equal = 0;
    if (old != Py_None || new_for_hook != Py_None) {
        int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
        if (cmp < 0) goto finally;
        if (cmp == 1) equal = 1;
    } else equal = 1;

    if (equal) {
        dict_mp_ass_sub_func orig = (dict_mp_ass_sub_func)get_pointer_for_type((PyObject *)Py_TYPE(self), "dict_mp_ass_subscript", "reaktome.dict_mp_ass_sub");
        if (orig) rc = orig(self, key, value);
        else {
            if (value) { if (PyObject_SetItem(self, key, value) < 0) rc = -1; else rc = 0; }
            else { if (PyObject_DelItem(self, key) < 0) rc = -1; else rc = 0; }
        }
        goto finally;
    }

    hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
    if (!hook) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); hook = NULL; }
        else goto finally;
    }

    if (hook && PyCallable_Check(hook)) {
        PyObject *args = PyTuple_New(4);
        if (!args) goto finally;
        Py_INCREF(self); PyTuple_SET_ITEM(args, 0, self);
        Py_INCREF(key); PyTuple_SET_ITEM(args, 1, key);
        Py_INCREF(old); PyTuple_SET_ITEM(args, 2, old);
        Py_INCREF(new_for_hook); PyTuple_SET_ITEM(args, 3, new_for_hook);

        PyObject *res = PyObject_CallObject(hook, args);
        Py_DECREF(args);
        if (!res) {
            if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) { PyErr_Clear(); rc = 0; goto finally; }
            goto finally;
        }
        Py_DECREF(res);
    }

    {
        dict_mp_ass_sub_func orig = (dict_mp_ass_sub_func)get_pointer_for_type((PyObject *)Py_TYPE(self), "dict_mp_ass_subscript", "reaktome.dict_mp_ass_sub");
        if (orig) rc = orig(self, key, value);
        else {
            if (value) { if (PyObject_SetItem(self, key, value) < 0) rc = -1; else rc = 0; }
            else { if (PyObject_DelItem(self, key) < 0) rc = -1; else rc = 0; }
        }
    }

finally:
    Py_XDECREF(hook);
    Py_XDECREF(old);
    Py_XDECREF(new_for_hook);
    return rc;
}

/* ---------- patch/unpatch helpers for list/dict ---------- */

static int patch_list_slots(void) {
    PyTypeObject *tp = &PyList_Type;
    list_sq_ass_item_func orig = NULL;
    if (tp->tp_as_sequence && tp->tp_as_sequence->sq_ass_item) orig = tp->tp_as_sequence->sq_ass_item;
    else orig = NULL;
    if (store_pointer_for_type((PyObject *)tp, "list_sq_ass_item", (void *)orig, "reaktome.list_sq_ass_item") < 0) return -1;
    if (!tp->tp_as_sequence) return -1;
    tp->tp_as_sequence->sq_ass_item = (list_sq_ass_item_func)list_sq_ass_item_trampoline;
    return 0;
}

static int unpatch_list_slots(void) {
    PyTypeObject *tp = &PyList_Type;
    void *ptr = get_pointer_for_type((PyObject *)tp, "list_sq_ass_item", "reaktome.list_sq_ass_item");
    if (ptr) {
        tp->tp_as_sequence->sq_ass_item = (list_sq_ass_item_func)ptr;
        remove_pointer_for_type((PyObject *)tp, "list_sq_ass_item");
    }
    return 0;
}

static int patch_dict_slots(void) {
    PyTypeObject *tp = &PyDict_Type;
    dict_mp_ass_sub_func orig = NULL;
    if (tp->tp_as_mapping && tp->tp_as_mapping->mp_ass_subscript) orig = tp->tp_as_mapping->mp_ass_subscript;
    else orig = NULL;
    if (store_pointer_for_type((PyObject *)tp, "dict_mp_ass_subscript", (void *)orig, "reaktome.dict_mp_ass_sub") < 0) return -1;
    if (!tp->tp_as_mapping) return -1;
    tp->tp_as_mapping->mp_ass_subscript = (dict_mp_ass_sub_func)dict_mp_ass_sub_trampoline;
    return 0;
}

static int unpatch_dict_slots(void) {
    PyTypeObject *tp = &PyDict_Type;
    void *ptr = get_pointer_for_type((PyObject *)tp, "dict_mp_ass_subscript", "reaktome.dict_mp_ass_sub");
    if (ptr) {
        tp->tp_as_mapping->mp_ass_subscript = (dict_mp_ass_sub_func)ptr;
        remove_pointer_for_type((PyObject *)tp, "dict_mp_ass_subscript");
    }
    return 0;
}

/* ---------- patch_type / unpatch_type for attribute trampoline ---------- */

static PyObject * py_patch_type(PyObject *self, PyObject *args) {
    PyObject *cls;
    if (!PyArg_ParseTuple(args, "O:patch_type", &cls)) return NULL;
    if (!PyType_Check(cls)) { PyErr_SetString(PyExc_TypeError, "patch_type expects a type object"); return NULL; }
    PyTypeObject *tp = (PyTypeObject *)cls;
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_SetString(PyExc_TypeError, "cannot patch non-heap (static/builtin) types with patch_type");
        return NULL;
    }
    if (store_pointer_for_type(cls, "orig_setattro", (void *)tp->tp_setattro, "reaktome.orig_setattro") < 0) return NULL;
    tp->tp_setattro = attr_trampoline_setattro;
    Py_RETURN_TRUE;
}

static PyObject * py_unpatch_type(PyObject *self, PyObject *args) {
    PyObject *cls;
    if (!PyArg_ParseTuple(args, "O:unpatch_type", &cls)) return NULL;
    if (!PyType_Check(cls)) { PyErr_SetString(PyExc_TypeError, "unpatch_type expects a type object"); return NULL; }
    void *ptr = get_pointer_for_type(cls, "orig_setattro", "reaktome.orig_setattro");
    PyTypeObject *tp = (PyTypeObject *)cls;
    if (ptr) {
        tp->tp_setattro = (setattrofunc)ptr;
        remove_pointer_for_type(cls, "orig_setattro");
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/* ---------- small python wrappers ---------- */

static PyObject * py_patch_list(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    if (patch_list_slots() < 0) return NULL;
    Py_RETURN_TRUE;
}
static PyObject * py_unpatch_list(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    if (unpatch_list_slots() < 0) return NULL;
    Py_RETURN_TRUE;
}
static PyObject * py_patch_dict(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    if (patch_dict_slots() < 0) return NULL;
    Py_RETURN_TRUE;
}
static PyObject * py_unpatch_dict(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    if (unpatch_dict_slots() < 0) return NULL;
    Py_RETURN_TRUE;
}

static PyObject * py_is_patched(PyObject *self, PyObject *args) {
    PyObject *cls;
    if (!PyArg_ParseTuple(args, "O:is_patched", &cls)) return NULL;
    if (!PyType_Check(cls)) { PyErr_SetString(PyExc_TypeError, "is_patched expects a type object"); return NULL; }
    if (!patched_types_map) Py_RETURN_FALSE;
    if (PyDict_Contains(patched_types_map, cls)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

/* ---------- Module cleanup: restore all stored pointers ---------- */

static void restore_all_patched_slots(void) {
    if (!patched_types_map) return;
    PyObject *keys = PyDict_Keys(patched_types_map); /* new ref */
    if (!keys) { PyErr_Clear(); return; }
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *typeobj = PyList_GetItem(keys, i); /* borrowed */
        if (!typeobj) continue;
        /* For each known slot name, attempt to restore */
        PyObject *entry = PyDict_GetItem(patched_types_map, typeobj); /* borrowed */
        if (!entry) continue;

        /* orig_setattro */
        PyObject *cap = PyDict_GetItemString(entry, "orig_setattro");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.orig_setattro");
            if (p && PyType_Check(typeobj)) {
                PyTypeObject *tp = (PyTypeObject *)typeobj;
                tp->tp_setattro = (setattrofunc)p;
            }
        }

        /* list_sq_ass_item */
        cap = PyDict_GetItemString(entry, "list_sq_ass_item");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.list_sq_ass_item");
            if (p && PyType_Check(typeobj)) {
                PyTypeObject *tp = (PyTypeObject *)typeobj;
                if (tp->tp_as_sequence) tp->tp_as_sequence->sq_ass_item = (list_sq_ass_item_func)p;
            }
        }

        /* dict_mp_ass_subscript */
        cap = PyDict_GetItemString(entry, "dict_mp_ass_subscript");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.dict_mp_ass_sub");
            if (p && PyType_Check(typeobj)) {
                PyTypeObject *tp = (PyTypeObject *)typeobj;
                if (tp->tp_as_mapping) tp->tp_as_mapping->mp_ass_subscript = (dict_mp_ass_sub_func)p;
            }
        }
        /* After restoring, we can clear the entry to avoid double-restore elsewhere */
        if (PyDict_DelItem(patched_types_map, typeobj) < 0) PyErr_Clear();
    }
    Py_DECREF(keys);
}

/* Module free function called when module object is destroyed */
static void module_free(void *m) {
    /* Restore all patches */
    restore_all_patched_slots();

    /* cleanup globals */
    if (patched_types_map) {
        Py_DECREF(patched_types_map);
        patched_types_map = NULL;
    }
    if (ReaktomeAbort) {
        Py_DECREF(ReaktomeAbort);
        ReaktomeAbort = NULL;
    }
}

/* ---------- Module method table ---------- */

static PyMethodDef _reaktome_methods[] = {
    {"patch_type", py_patch_type, METH_VARARGS, "Install attribute advisory trampoline on a heap type."},
    {"unpatch_type", py_unpatch_type, METH_VARARGS, "Restore a type's original attribute setter."},
    {"is_patched", py_is_patched, METH_VARARGS, "Return True if type has an entry in the patched-types map."},
    {"patch_list", py_patch_list, METH_NOARGS, "Patch global list indexed-assignment handling (sq_ass_item)."},
    {"unpatch_list", py_unpatch_list, METH_NOARGS, "Restore list indexed-assignment handling."},
    {"patch_dict", py_patch_dict, METH_NOARGS, "Patch global dict mapping-assignment handling (mp_ass_subscript)."},
    {"unpatch_dict", py_unpatch_dict, METH_NOARGS, "Restore dict mapping-assignment handling."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef _reaktome_module = {
    PyModuleDef_HEAD_INIT,
    "_reaktome",
    "Low-level advisory hooks for setattr / list / dict mutations.\n"
    "Hook signatures (per-instance attributes):\n"
    " - attributes: __reaktome_setattr__(self, name, old, new)\n"
    " - list/dict items: __reaktome_setitem__(self, key_or_index, old, new)\n"
    "Raise _reaktome.ReaktomeAbort() in the hook to silently abort the mutation.\n",
    -1,
    _reaktome_methods,
    NULL,   /* slots */
    NULL,   /* traverse */
    NULL,   /* clear */
    module_free /* m_free */
};

PyMODINIT_FUNC PyInit__reaktome(void) {
    PyObject *m = PyModule_Create(&_reaktome_module);
    if (!m) return NULL;

    /* patched_types_map stored on module for inspection */
    patched_types_map = PyDict_New();
    if (!patched_types_map) { Py_DECREF(m); return NULL; }
    if (PyModule_AddObject(m, "_patched_types", patched_types_map) < 0) {
        Py_DECREF(patched_types_map);
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(patched_types_map); /* keep our own ref */

    /* create ReaktomeAbort exception and export */
    ReaktomeAbort = PyErr_NewException("_reaktome.ReaktomeAbort", NULL, NULL);
    if (!ReaktomeAbort) { Py_DECREF(m); return NULL; }
    if (PyModule_AddObject(m, "ReaktomeAbort", ReaktomeAbort) < 0) {
        Py_DECREF(ReaktomeAbort);
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(ReaktomeAbort);

    return m;
}
