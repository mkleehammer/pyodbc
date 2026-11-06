#pragma once

bool InitializeDecimal();
PyObject* GetDecimalPoint();
bool SetDecimalPoint(PyObject* pNew);

PyObject* DecimalFromText(const TextEnc& enc, const byte* pb, Py_ssize_t cb, bool strict);
PyObject* DecimalFromStrictText(PyObject* text);
PyObject* DecimalFromAsciiText(const char* pch, Py_ssize_t cch);
PyObject* DecimalFromNumericStruct(const SQL_NUMERIC_STRUCT& num);
