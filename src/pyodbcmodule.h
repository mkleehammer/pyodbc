
/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _PYPGMODULE_H
#define _PYPGMODULE_H

extern PyObject* Error;
extern PyObject* Warning;
extern PyObject* InterfaceError;
extern PyObject* DatabaseError;
extern PyObject* InternalError;
extern PyObject* OperationalError;
extern PyObject* ProgrammingError;
extern PyObject* IntegrityError;
extern PyObject* DataError;
extern PyObject* NotSupportedError;

extern PyObject* null_binary;

extern PyObject* decimal_type;

inline bool PyDecimal_Check(PyObject* p)
{
    return Py_TYPE(p) == (_typeobject*)decimal_type;
}
extern HENV henv;

extern PyTypeObject RowType;
extern PyTypeObject CursorType;
extern PyTypeObject ConnectionType;

// Thd pyodbc module.
extern PyObject* pModule;

inline bool lowercase()
{
    return PyObject_GetAttrString(pModule, "lowercase") == Py_True;
}

extern Py_UNICODE chDecimal;

#endif // _PYPGMODULE_H
