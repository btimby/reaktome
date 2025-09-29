#ifndef REAKTOME_H
#define REAKTOME_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* --- list.c --- */
int reaktome_patch_list(void);
void reaktome_unpatch_list(void);

/* --- dict.c --- */
int reaktome_patch_dict(void);
void reaktome_unpatch_dict(void);

/* --- set.c --- */
int reaktome_patch_set(void);
void reaktome_unpatch_set(void);

/* --- obj.c --- */
int reaktome_patch_type(PyTypeObject *tp);
void reaktome_unpatch_type(PyTypeObject *tp);

/* Shared trampolines */
int __reaktome_setattr__(PyObject *self, PyObject *name, PyObject *value);
int __reaktome_delattr__(PyObject *self, PyObject *name);
int __reaktome_setitem__(PyObject *self, PyObject *key, PyObject *value);
int __reaktome_delitem__(PyObject *self, PyObject *key);

#endif /* REAKTOME_H */
