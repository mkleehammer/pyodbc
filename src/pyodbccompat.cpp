#include "pyodbc.h"

bool Text_EqualsI(PyObject* lhs, const char* rhs)
{
    if (lhs == 0 || !PyUnicode_Check(lhs))
        return false;

    Py_ssize_t cchLHS = PyUnicode_GET_SIZE(lhs);
    Py_ssize_t cchRHS = (Py_ssize_t)strlen(rhs);
    if (cchLHS != cchRHS)
        return false;

    Py_UNICODE* p = PyUnicode_AS_UNICODE(lhs);
    for (Py_ssize_t i = 0; i < cchLHS; i++)
    {
        int chL = (int)Py_UNICODE_TOUPPER(p[i]);
        int chR = (int)toupper(rhs[i]);
        if (chL != chR)
            return false;
    }

    return true;
}
