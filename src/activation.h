#ifndef REAKTOME_ACTIVATION_H
#define REAKTOME_ACTIVATION_H

#include <Python.h>

// Activate a type with a given dunders dict.
// If dunders is NULL, clear activation for that type.
int reaktome_activate_type(PyTypeObject *type, PyObject *dunders);

// Call a dunder hook on an object if available.
// key, old, new can be NULL (treated as None).
int reaktome_call_dunder(PyObject *self,
                         const char *name,
                         PyObject *key,
                         PyObject *old,
                         PyObject *newv);

#endif /* REAKTOME_ACTIVATION_H */
