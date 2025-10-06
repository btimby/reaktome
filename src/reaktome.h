#ifndef REAKTOME_H
#define REAKTOME_H

#include <Python.h>

/* Each container file provides one of these */
int reaktome_patch_list(PyObject *m);
int reaktome_patch_dict(PyObject *m);
int reaktome_patch_set(PyObject *m);
int reaktome_patch_obj(PyObject *m);

#endif /* REAKTOME_H */
