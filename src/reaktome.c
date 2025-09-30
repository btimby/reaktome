#include "reaktome.h"

/* Module definition */
static struct PyModuleDef reaktome_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_reaktome",
    .m_doc = "Reaktome C extension for per-instance advisory hooks",
    .m_size = -1,
};

/* Module init */
PyMODINIT_FUNC PyInit__reaktome(void) {
    PyObject *m = PyModule_Create(&reaktome_module);
    if (m == NULL)
        return NULL;

    if (reaktome_patch_list(m) < 0) return NULL;
    if (reaktome_patch_dict(m) < 0) return NULL;
    if (reaktome_patch_set(m) < 0) return NULL;
    if (reaktome_patch_obj(m) < 0) return NULL;

    return m;
}
