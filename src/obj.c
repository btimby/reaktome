// src/obj.c
#define PY_SSIZE_T_CLEAN
#include "reaktome.h"

/*
 * obj.c — safe attribute trampoline and patch/unpatch for types.
 *
 * Important safety: the trampoline MUST call the original tp_setattro
 * saved in the type's capsule. It must NOT fall back to PyObject_GenericSetAttr
 * when that capsule is absent, because that can re-enter the trampoline and
 * cause recursion.
 */

/* Capsule name we use to store original tp_setattro in the type's inner dict. */
#define CAPS_ORIG_SETATTRO "reaktome.orig_setattro"

/* typedef for compatibility */
typedef int (*setattrofunc_t)(PyObject *, PyObject *, PyObject *);

/* Helper that returns old attr or Py_None on attribute missing.
   It returns a *new reference* or NULL on error.
   Expects safe_getattr_as_none_obj to exist in shared helpers; if not,
   implement it here (but your code already had it).
*/
extern PyObject *safe_getattr_as_none_obj(PyObject *obj, PyObject *nameobj);

/* Helpers for saved_map (extern from common helpers) */
extern void *get_saved_pointer(PyObject *typeobj, const char *name, const char *capsule_name);
extern int store_pointer(PyObject *typeobj, const char *name, void *ptr, const char *capsule_name);
extern void remove_saved_name(PyObject *typeobj, const char *name);

/* call instance hook if present (attribute hooks) */
static int call_instance_attr_hook(PyObject *inst, const char *attrname, PyObject *args) {
    PyObject *hook = PyObject_GetAttrString(inst, attrname);
    if (!hook) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            return 0; /* no hook installed on instance */
        }
        return -1; /* other error */
    }
    if (!PyCallable_Check(hook)) {
        Py_DECREF(hook);
        PyErr_Format(PyExc_TypeError, "%s is not callable", attrname);
        return -1;
    }
    PyObject *res = PyObject_CallObject(hook, args);
    Py_DECREF(hook);
    if (!res) return -1;
    Py_DECREF(res);
    return 0;
}

/* Attribute trampoline (installed as tp_setattro).
   This function strictly uses the saved original pointer from the type dict (capsule).
   If the capsule is missing, it returns an error rather than attempting a fallback
   that could cause recursion.
*/
static int
attr_trampoline_setattro(PyObject *self, PyObject *name, PyObject *value)
{
    int rc = -1;

    /* 1) Capture old value before mutation (or Py_None if missing).
       safe_getattr_as_none_obj returns a new ref or NULL on error.
    */
    PyObject *old = safe_getattr_as_none_obj(self, name);
    if (!old) {
        return -1; /* Propagate error (not AttributeError) */
    }

    /* 2) Lookup saved original tp_setattro pointer from the type's capsule.
       We rely on patch_type() to have stored this pointer prior to installing the trampoline.
    */
    PyTypeObject *type = Py_TYPE(self);
    void *vp = get_saved_pointer((PyObject *)type, "orig_setattro", CAPS_ORIG_SETATTRO);
    if (!vp) {
        /* No saved original — this indicates a mismatch (trampoline installed but no capsule).
           Return an error rather than calling PyObject_GenericSetAttr (which could re-enter).
        */
        PyErr_SetString(PyExc_RuntimeError, "reaktome: original tp_setattro not found for patched type");
        Py_DECREF(old);
        return -1;
    }
    setattrofunc_t orig = (setattrofunc_t)vp;

    /* 3) Call original setter. We do NOT attempt any fallback to generic setter here. */
    int orig_rc = orig(self, name, value);
    if (orig_rc < 0) {
        Py_DECREF(old);
        return -1; /* original raised: propagate */
    }

    /* 4) Call instance hook advisory-only.
       - deletion: value == NULL => call "__reaktome_delattr__(inst, name, old)"
       - assignment: call "__reaktome_setattr__(inst, name, old, new)"
       Hook errors are propagated up (so tests see them).
    */
    if (value == NULL) {
        /* deletion: build args tuple (self, name, old) */
        PyObject *args = PyTuple_New(3);
        if (!args) { Py_DECREF(old); return -1; }
        Py_INCREF(self); PyTuple_SET_ITEM(args, 0, self);
        Py_INCREF(name); PyTuple_SET_ITEM(args, 1, name);
        PyTuple_SET_ITEM(args, 2, old); /* steal old */
        rc = call_instance_attr_hook(self, "__reaktome_delattr__", args);
        Py_DECREF(args);
        /* old is stolen into args (no extra DECREF) */
        if (rc < 0) return -1;
    } else {
        /* assignment: build args tuple (self, name, old, new) */
        PyObject *args = PyTuple_New(4);
        if (!args) { Py_DECREF(old); return -1; }
        Py_INCREF(self); PyTuple_SET_ITEM(args, 0, self);
        Py_INCREF(name); PyTuple_SET_ITEM(args, 1, name);
        PyTuple_SET_ITEM(args, 2, old); /* steal old */
        Py_INCREF(value); PyTuple_SET_ITEM(args, 3, value);
        rc = call_instance_attr_hook(self, "__reaktome_setattr__", args);
        Py_DECREF(args);
        if (rc < 0) return -1;
    }

    return 0;
}

/* --------- patch_type / unpatch_type (Python-exposed helpers) ---------- */

/* Exposed to Python as py_patch_type; returns True if patched, False if already patched */
PyObject *
py_patch_type(PyObject *self, PyObject *args)
{
    PyObject *typ;
    if (!PyArg_ParseTuple(args, "O:patch_type", &typ)) return NULL;
    if (!PyType_Check(typ)) {
        PyErr_SetString(PyExc_TypeError, "patch_type: expected a type object");
        return NULL;
    }
    PyTypeObject *tp = (PyTypeObject *)typ;

    /* if already patched (capsule present), return False */
    if (get_saved_pointer(typ, "orig_setattro", CAPS_ORIG_SETATTRO)) {
        Py_RETURN_FALSE;
    }

    /* store the original pointer (could be NULL; we store the raw pointer) */
    void *orig = (void *)tp->tp_setattro;
    if (store_pointer(typ, "orig_setattro", orig, CAPS_ORIG_SETATTRO) < 0) {
        return NULL;
    }

    /* install trampoline */
    tp->tp_setattro = attr_trampoline_setattro;
#if PY_VERSION_HEX >= 0x03070000
    PyType_Modified(tp);
#endif

    Py_RETURN_TRUE;
}

/* Exposed unpatch: restore saved original tp_setattro if present */
PyObject *
py_unpatch_type(PyObject *self, PyObject *args)
{
    PyObject *typ;
    if (!PyArg_ParseTuple(args, "O:unpatch_type", &typ)) return NULL;
    if (!PyType_Check(typ)) {
        PyErr_SetString(PyExc_TypeError, "unpatch_type: expected a type object");
        return NULL;
    }
    PyTypeObject *tp = (PyTypeObject *)typ;

    void *vp = get_saved_pointer(typ, "orig_setattro", CAPS_ORIG_SETATTRO);
    if (!vp) Py_RETURN_FALSE;

    setattrofunc_t orig = (setattrofunc_t)vp;
    tp->tp_setattro = orig;
#if PY_VERSION_HEX >= 0x03070000
    PyType_Modified(tp);
#endif

    remove_saved_name(typ, "orig_setattro");
    Py_RETURN_TRUE;
}

/* Note: you must add py_patch_type and py_unpatch_type to your module method table in reaktome.c */
