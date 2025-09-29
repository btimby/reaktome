#include "reaktome.h"

/* Module initialization */

static struct PyModuleDef reaktomemodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "reaktome",
    .m_doc = "Advisory setattr/item hooks",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit_reaktome(void)
{
    PyObject *m = PyModule_Create(&reaktomemodule);
    if (m == NULL)
        return NULL;

    if (reaktome_patch_list() < 0) return NULL;
    if (reaktome_patch_dict() < 0) return NULL;
    if (reaktome_patch_set() < 0) return NULL;

    return m;
}
