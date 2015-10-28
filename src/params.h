
#ifndef PARAMS_H
#define PARAMS_H

bool Params_init();

struct Cursor;

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* params, Py_ssize_t paramsOffset);
bool PrepareAndBindArray(Cursor* cur, PyObject* pSql, PyObject* params);
void FreeParameterData(Cursor* cur);
void FreeParameterInfo(Cursor* cur);

#endif
