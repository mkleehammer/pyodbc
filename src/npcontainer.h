#ifndef _NPCONTAINER_H_
#define _NPCONTAINER_H_


#if PY_VERSION_HEX >= 0x03000000
int NpContainer_init();
#else
void NpContainer_init();
#endif

PyObject *Cursor_fetchdictarray(PyObject *self, PyObject *args, PyObject *kwargs);

extern char fetchdictarray_doc[];

extern Py_ssize_t iopro_text_limit;

#endif // _NPCONTAINER_H_
