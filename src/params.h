
#ifndef PARAMS_H
#define PARAMS_H

bool Params_init();

struct Cursor;

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* params, bool skip_first);
void FreeParameterData(Cursor* cur);
void FreeParameterInfo(Cursor* cur);

#endif
