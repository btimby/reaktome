#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

/*
 _reaktome.c

 Hooks for tracking object attribute changes and container mutations.

 - Attribute advisory trampoline:
     * assignment → __reaktome_setattr__(self, name, old, new)
       - return None → store `new`
       - return other → store return value
       - raise ReaktomeAbort → silently cancel
     * deletion → __reaktome_delattr__(self, name, old)
       - raise ReaktomeAbort → silently cancel

 - List advisory trampoline:
     * list[index] = val / del list[index]
       calls __reaktome_setitem__(self, index, old, new)

 - Dict advisory trampoline:
     * dict[key] = val / del dict[key]
       calls __reaktome_setitem__(self, key, old, new)

 - Patching helpers:
     patch_type / unpatch_type
     patch_list / unpatch_list
     patch_dict / unpatch_dict

 - ReaktomeAbort exception:
     Raise inside hook to cancel the operation silently.

 All hooks are optional. If not defined, fallback to original behavior.
*/

typedef int (*setattrofunc)(PyObject *, PyObject *, PyObject *);
typedef int (*list_sq_ass_item_func)(PyObject *, Py_ssize_t, PyObject *);
typedef int (*dict_mp_ass_sub_func)(PyObject *, PyObject *, PyObject *);

static PyObject *saved_map = NULL; /* type → dict(name → capsule) */
static PyObject *ReaktomeAbort = NULL;

/* ---------- saved_map helpers ---------- */

static int ensure_saved_map(void) {
    if (!saved_map) {
        saved_map = PyDict_New();
        if (!saved_map) return -1;
    }
    return 0;
}

static PyObject *get_inner_newref(PyObject *typeobj, int create_if_missing) {
    if (ensure_saved_map() < 0) return NULL;
    PyObject *entry = PyDict_GetItem(saved_map, typeobj); /* borrowed */
    if (entry) { Py_INCREF(entry); return entry; }
    if (!create_if_missing) return NULL;
    entry = PyDict_New();
    if (!entry) return NULL;
    if (PyDict_SetItem(saved_map, typeobj, entry) < 0) {
        Py_DECREF(entry);
        return NULL;
    }
    return entry;
}

static int store_pointer(PyObject *typeobj, const char *name, void *ptr, const char *capsule_name) {
    PyObject *entry = get_inner_newref(typeobj, 1);
    if (!entry) return -1;
    PyObject *cap = PyCapsule_New(ptr, capsule_name, NULL);
    if (!cap) { Py_DECREF(entry); return -1; }
    int ok = (PyDict_SetItemString(entry, name, cap) == 0);
    Py_DECREF(cap);
    Py_DECREF(entry);
    return ok ? 0 : -1;
}

static void *get_saved_pointer(PyObject *typeobj, const char *name, const char *capsule_name) {
    if (!saved_map) return NULL;
    PyObject *entry = PyDict_GetItem(saved_map, typeobj); /* borrowed */
    if (!entry) return NULL;
    PyObject *cap = PyDict_GetItemString(entry, name); /* borrowed */
    if (!cap) return NULL;
    return PyCapsule_GetPointer(cap, capsule_name);
}

static void remove_saved_name(PyObject *typeobj, const char *name) {
    if (!saved_map) return;
    PyObject *entry = PyDict_GetItem(saved_map, typeobj); /* borrowed */
    if (!entry) return;
    if (PyDict_DelItemString(entry, name) < 0) PyErr_Clear();
    if (PyDict_Size(entry) == 0) {
        if (PyDict_DelItem(saved_map, typeobj) < 0) PyErr_Clear();
    }
}

/* ---------- Utility ---------- */

static PyObject *safe_getattr_as_none(PyObject *obj, PyObject *nameobj) {
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
    PyObject *hook = NULL, *old = NULL, *new_for_hook = NULL, *call_res = NULL, *final_value = NULL;

    /* deletion */
    if (value == NULL) {
        old = safe_getattr_as_none(self, name);
        if (!old) return -1;

        hook = PyObject_GetAttrString(self, "__reaktome_delattr__");
        if (hook && PyCallable_Check(hook)) {
            call_res = PyObject_CallFunctionObjArgs(hook, self, name, old, NULL);
            Py_DECREF(hook);
            if (!call_res) {
                if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) {
                    PyErr_Clear();
                    Py_DECREF(old);
                    return 0;
                }
                Py_DECREF(old);
                return -1;
            }
            Py_DECREF(call_res);
        } else Py_XDECREF(hook);

        setattrofunc orig = (setattrofunc)get_saved_pointer((PyObject *)Py_TYPE(self),
                                                            "orig_setattro",
                                                            "reaktome.orig_setattro");
        if (orig) rc = orig(self, name, NULL);
        else rc = PyObject_GenericSetAttr(self, name, NULL);

        Py_DECREF(old);
        return rc;
    }

    /* assignment */
    old = safe_getattr_as_none(self, name);
    if (!old) return -1;

    new_for_hook = value ? value : Py_None;
    Py_INCREF(new_for_hook);

    int equal = 0;
    int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
    if (cmp < 0) goto finally;
    if (cmp == 1) equal = 1;

    if (equal) {
        setattrofunc orig = (setattrofunc)get_saved_pointer((PyObject *)Py_TYPE(self),
                                                            "orig_setattro",
                                                            "reaktome.orig_setattro");
        if (orig) rc = orig(self, name, value);
        else rc = PyObject_GenericSetAttr(self, name, value);
        goto finally;
    }

    hook = PyObject_GetAttrString(self, "__reaktome_setattr__");
    if (hook && PyCallable_Check(hook)) {
        call_res = PyObject_CallFunctionObjArgs(hook, self, name, old, new_for_hook, NULL);
        Py_DECREF(hook);
        if (!call_res) {
            if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) {
                PyErr_Clear();
                rc = 0;
                goto finally;
            }
            goto finally;
        }
        if (call_res == Py_None) {
            final_value = value; Py_INCREF(final_value);
        } else {
            final_value = call_res; Py_INCREF(final_value);
        }
        Py_DECREF(call_res);
    }

    if (!final_value) { final_value = value; Py_INCREF(final_value); }

    {
        setattrofunc orig = (setattrofunc)get_saved_pointer((PyObject *)Py_TYPE(self),
                                                            "orig_setattro",
                                                            "reaktome.orig_setattro");
        if (orig) rc = orig(self, name, final_value);
        else rc = PyObject_GenericSetAttr(self, name, final_value);
    }

finally:
    Py_XDECREF(old);
    Py_XDECREF(new_for_hook);
    Py_XDECREF(call_res);
    Py_XDECREF(final_value);
    return rc;
}

/* ---------- List trampoline ---------- */

static int list_sq_ass_item_trampoline(PyObject *self, Py_ssize_t index, PyObject *value) {
    int rc = -1;
    PyObject *hook = NULL, *old = NULL, *idxobj = NULL, *new_for_hook = NULL, *call_res = NULL, *final_value = NULL;

    idxobj = PyLong_FromSsize_t(index);
    if (!idxobj) return -1;

    old = PySequence_GetItem(self, index);
    if (!old) { PyErr_Clear(); old = Py_None; Py_INCREF(old); }

    new_for_hook = value ? value : Py_None;
    Py_INCREF(new_for_hook);

    int equal = 0;
    int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
    if (cmp < 0) goto finally;
    if (cmp == 1) equal = 1;

    if (!equal) {
        hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
        if (hook && PyCallable_Check(hook)) {
            call_res = PyObject_CallFunctionObjArgs(hook, self, idxobj, old, new_for_hook, NULL);
            Py_DECREF(hook);
            if (!call_res) {
                if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) {
                    PyErr_Clear(); rc = 0; goto finally;
                }
                goto finally;
            }
            if (call_res == Py_None) { final_value = value; Py_INCREF(final_value); }
            else { final_value = call_res; Py_INCREF(final_value); }
            Py_DECREF(call_res);
        }
    }

    if (!final_value) { final_value = value; Py_INCREF(final_value); }

    list_sq_ass_item_func orig = (list_sq_ass_item_func)get_saved_pointer((PyObject *)&PyList_Type,
                                                                          "list_sq_ass_item",
                                                                          "reaktome.list_sq_ass_item");
    if (orig) rc = orig(self, index, final_value);
    else rc = (value ? PySequence_SetItem(self, index, final_value) : PySequence_DelItem(self, index));

finally:
    Py_XDECREF(old);
    Py_XDECREF(idxobj);
    Py_XDECREF(new_for_hook);
    Py_XDECREF(call_res);
    Py_XDECREF(final_value);
    return rc;
}

/* ---------- Dict trampoline ---------- */

static int dict_mp_ass_sub_trampoline(PyObject *self, PyObject *key, PyObject *value) {
    int rc = -1;
    PyObject *hook = NULL, *old = NULL, *new_for_hook = NULL, *call_res = NULL, *final_value = NULL;

    old = PyObject_GetItem(self, key);
    if (!old) { PyErr_Clear(); old = Py_None; Py_INCREF(old); }

    new_for_hook = value ? value : Py_None;
    Py_INCREF(new_for_hook);

    int equal = 0;
    int cmp = PyObject_RichCompareBool(old, new_for_hook, Py_EQ);
    if (cmp < 0) goto finally;
    if (cmp == 1) equal = 1;

    if (!equal) {
        hook = PyObject_GetAttrString(self, "__reaktome_setitem__");
        if (hook && PyCallable_Check(hook)) {
            call_res = PyObject_CallFunctionObjArgs(hook, self, key, old, new_for_hook, NULL);
            Py_DECREF(hook);
            if (!call_res) {
                if (ReaktomeAbort && PyErr_ExceptionMatches(ReaktomeAbort)) {
                    PyErr_Clear(); rc = 0; goto finally;
                }
                goto finally;
            }
            if (call_res == Py_None) { final_value = value; Py_INCREF(final_value); }
            else { final_value = call_res; Py_INCREF(final_value); }
            Py_DECREF(call_res);
        }
    }

    if (!final_value) { final_value = value; Py_INCREF(final_value); }

    dict_mp_ass_sub_func orig = (dict_mp_ass_sub_func)get_saved_pointer((PyObject *)&PyDict_Type,
                                                                        "dict_mp_ass_subscript",
                                                                        "reaktome.dict_mp_ass_sub");
    if (orig) rc = orig(self, key, final_value);
    else rc = (value ? PyObject_SetItem(self, key, final_value) : PyObject_DelItem(self, key));

finally:
    Py_XDECREF(old);
    Py_XDECREF(new_for_hook);
    Py_XDECREF(call_res);
    Py_XDECREF(final_value);
    return rc;
}

/* ---------- patch/unpatch helpers ---------- */

static PyObject *py_patch_type(PyObject *self, PyObject *args) {
    PyObject *typ;
    if (!PyArg_ParseTuple(args, "O:patch_type", &typ)) return NULL;
    if (!PyType_Check(typ)) { PyErr_SetString(PyExc_TypeError, "type expected"); return NULL; }
    PyTypeObject *tp = (PyTypeObject *)typ;
    if (get_saved_pointer(typ, "orig_setattro", "reaktome.orig_setattro")) Py_RETURN_FALSE;
    if (store_pointer(typ, "orig_setattro", (void *)tp->tp_setattro, "reaktome.orig_setattro") < 0) return NULL;
    tp->tp_setattro = attr_trampoline_setattro;
    Py_RETURN_TRUE;
}

static PyObject *py_unpatch_type(PyObject *self, PyObject *args) {
    PyObject *typ;
    if (!PyArg_ParseTuple(args, "O:unpatch_type", &typ)) return NULL;
    if (!PyType_Check(typ)) { PyErr_SetString(PyExc_TypeError, "type expected"); return NULL; }
    PyTypeObject *tp = (PyTypeObject *)typ;
    void *p = get_saved_pointer(typ, "orig_setattro", "reaktome.orig_setattro");
    if (!p) Py_RETURN_FALSE;
    tp->tp_setattro = (setattrofunc)p;
    remove_saved_name(typ, "orig_setattro");
    Py_RETURN_TRUE;
}

static PyObject *py_patch_list(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    PyTypeObject *tp = &PyList_Type;
    if (get_saved_pointer((PyObject *)tp, "list_sq_ass_item", "reaktome.list_sq_ass_item")) Py_RETURN_FALSE;
    if (store_pointer((PyObject *)tp, "list_sq_ass_item", (void *)tp->tp_as_sequence->sq_ass_item, "reaktome.list_sq_ass_item") < 0) return NULL;
    tp->tp_as_sequence->sq_ass_item = list_sq_ass_item_trampoline;
    Py_RETURN_TRUE;
}

static PyObject *py_unpatch_list(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    PyTypeObject *tp = &PyList_Type;
    void *p = get_saved_pointer((PyObject *)tp, "list_sq_ass_item", "reaktome.list_sq_ass_item");
    if (!p) Py_RETURN_FALSE;
    tp->tp_as_sequence->sq_ass_item = (list_sq_ass_item_func)p;
    remove_saved_name((PyObject *)tp, "list_sq_ass_item");
    Py_RETURN_TRUE;
}

static PyObject *py_patch_dict(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    PyTypeObject *tp = &PyDict_Type;
    if (get_saved_pointer((PyObject *)tp, "dict_mp_ass_subscript", "reaktome.dict_mp_ass_sub")) Py_RETURN_FALSE;
    if (store_pointer((PyObject *)tp, "dict_mp_ass_subscript", (void *)tp->tp_as_mapping->mp_ass_subscript, "reaktome.dict_mp_ass_sub") < 0) return NULL;
    tp->tp_as_mapping->mp_ass_subscript = dict_mp_ass_sub_trampoline;
    Py_RETURN_TRUE;
}

static PyObject *py_unpatch_dict(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    PyTypeObject *tp = &PyDict_Type;
    void *p = get_saved_pointer((PyObject *)tp, "dict_mp_ass_subscript", "reaktome.dict_mp_ass_sub");
    if (!p) Py_RETURN_FALSE;
    tp->tp_as_mapping->mp_ass_subscript = (dict_mp_ass_sub_func)p;
    remove_saved_name((PyObject *)tp, "dict_mp_ass_subscript");
    Py_RETURN_TRUE;
}

/* ---------- module cleanup ---------- */

static void restore_all_saved(void) {
    if (!saved_map) return;
    PyObject *keys = PyDict_Keys(saved_map);
    if (!keys) return;
    for (Py_ssize_t i = 0; i < PyList_Size(keys); ++i) {
        PyObject *typeobj = PyList_GetItem(keys, i);
        if (!typeobj) continue;
        PyObject *entry = PyDict_GetItem(saved_map, typeobj);
        if (!entry) continue;

        PyObject *cap = PyDict_GetItemString(entry, "orig_setattro");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.orig_setattro");
            if (p) ((PyTypeObject *)typeobj)->tp_setattro = (setattrofunc)p;
        }

        cap = PyDict_GetItemString(entry, "list_sq_ass_item");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.list_sq_ass_item");
            if (p) ((PyTypeObject *)typeobj)->tp_as_sequence->sq_ass_item = (list_sq_ass_item_func)p;
        }

        cap = PyDict_GetItemString(entry, "dict_mp_ass_subscript");
        if (cap) {
            void *p = PyCapsule_GetPointer(cap, "reaktome.dict_mp_ass_sub");
            if (p) ((PyTypeObject *)typeobj)->tp_as_mapping->mp_ass_subscript = (dict_mp_ass_sub_func)p;
        }
    }
    Py_DECREF(keys);
    PyDict_Clear(saved_map);
}

static void module_free(void *m) {
    restore_all_saved();
    Py_XDECREF(saved_map);
    saved_map = NULL;
    Py_XDECREF(ReaktomeAbort);
    ReaktomeAbort = NULL;
}

/* ---------- Module def ---------- */

static PyMethodDef _reaktome_methods[] = {
    {"patch_type", py_patch_type, METH_VARARGS, "Patch a type's setattr/delattr."},
    {"unpatch_type", py_unpatch_type, METH_VARARGS, "Unpatch a type."},
    {"patch_list", py_patch_list, METH_NOARGS, "Patch list."},
    {"unpatch_list", py_unpatch_list, METH_NOARGS, "Unpatch list."},
    {"patch_dict", py_patch_dict, METH_NOARGS, "Patch dict."},
    {"unpatch_dict", py_unpatch_dict, METH_NOARGS, "Unpatch dict."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef _reaktomemodule = {
    PyModuleDef_HEAD_INIT,
    "_reaktome",
    "Hooks for tracking mutations on attributes, lists, and dicts.",
    -1,
    _reaktome_methods,
    NULL,
    NULL,
    NULL,
    module_free
};

PyMODINIT_FUNC PyInit__reaktome(void) {
    PyObject *m = PyModule_Create(&_reaktomemodule);
    if (!m) return NULL;

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
