
#include "pyodbc.h"
#include "sqlwchar.h"
#include "wrapper.h"

// We could eliminate a lot of trouble if we had a define for the size of a SQLWCHAR.  Unfortunately, I can't think of
// a way to do this with the preprocessor.  Python's setup.cfg files aren't really making it easy either.  For now
// we'll use C code and revisit this later.

SQLWChar::SQLWChar(PyObject* o)
{
    // Converts from a Python Unicode string.

    pch = 0;
    len = 0;
    owns_memory = false;

    Convert(o);
}

void SQLWChar::Free()
{
    if (pch && owns_memory)
        pyodbc_free(pch);
    pch = 0;
    len = 0;
    owns_memory = false;
}

bool SQLWChar::Convert(PyObject* o)
{
    Free();

    if (!PyUnicode_Check(o))
    {
        PyErr_SetString(PyExc_TypeError, "Unicode required");
        return false;
    }

    Py_UNICODE* pU   = (Py_UNICODE*)PyUnicode_AS_UNICODE(o);
    Py_ssize_t  lenT = PyUnicode_GET_SIZE(o);

    if (sizeof(SQLWCHAR) == Py_UNICODE_SIZE)
    {
        // The ideal case - SQLWCHAR and Py_UNICODE are the same, so we point into the Unicode object.

        pch         = (SQLWCHAR*)pU;
        len         = lenT;
        owns_memory = false;
        return true;
    }

    SQLWCHAR* pchT = (SQLWCHAR*)pyodbc_malloc(sizeof(SQLWCHAR) * (lenT + 1));
    if (pchT == 0)
    {
        PyErr_NoMemory();
        return false;
    }

    if (!sqlwchar_copy(pchT, pU, lenT))
    {
        pyodbc_free(pchT);
        return false;
    }
    
    pch = pchT;
    len = lenT;
    owns_memory = true;
    return true;
}


bool sqlwchar_copy(SQLWCHAR* pdest, const Py_UNICODE* psrc, Py_ssize_t len)
{
    for (int i = 0; i <= len; i++) // ('<=' to include the NULL)
    {
        pdest[i] = (SQLWCHAR)psrc[i];
        if ((Py_UNICODE)pdest[i] < psrc[i])
        {
            PyErr_Format(PyExc_ValueError, "Cannot convert from Unicode %zd to SQLWCHAR.  Value is too large.", (Py_ssize_t)psrc[i]);
            return false;
        }
    }
    return true;
}


PyObject* PyUnicode_FromSQLWCHAR(const SQLWCHAR* sz, Py_ssize_t cch)
{
    if (sizeof(SQLWCHAR) == Py_UNICODE_SIZE)
        return PyUnicode_FromUnicode((const Py_UNICODE*)sz, cch);

#if HAVE_WCHAR_H
    if (sizeof(wchar_t) == sizeof(SQLWCHAR))
    {
        // Python provides a function to map from wchar_t to Unicode.  Since wchar_t and SQLWCHAR are the same size, we can
        // use it.
        return PyUnicode_FromWideChar((const wchar_t*)sz, cch);
    }
#endif

    Object result(PyUnicode_FromUnicode(0, cch));
    if (!result)
        return 0;

    Py_UNICODE* pch = PyUnicode_AS_UNICODE(result.Get());
    for (Py_ssize_t i = 0; i < cch; i++)
    {
        pch[i] = (Py_UNICODE)sz[i];
        if ((SQLWCHAR)pch[i] != sz[i])
        {
            PyErr_Format(PyExc_ValueError, "Cannot convert from SQLWCHAR %zd to Unicode.  Value is too large.", (Py_ssize_t)pch[i]);
            return 0;
        }
    }
    
    return result.Detach();
}

void SQLWChar::dump()
{
    printf("sqlwchar=%ld pch=%p len=%ld owns=%d\n", sizeof(SQLWCHAR), pch, len, (int)owns_memory);
    if (pch && len)
    {
        Py_ssize_t i = 0;
        while (i < len)
        {
            Py_ssize_t stop = min(i + 10, len);

            for (Py_ssize_t x = i; x < stop; x++)
            {
                for (int byteindex = (int)sizeof(SQLWCHAR)-1; byteindex >= 0; byteindex--)
                {
                    int byte = (pch[x] >> (byteindex * 8)) & 0xFF;
                    printf("%02x", byte);
                }
                printf(" ");
            }

            for (Py_ssize_t x = i; x < stop; x++)
                printf("%c", (char)pch[x]);
                
            printf("\n");

            i += 10;
        }

        printf("\n\n");
    }
}

SQLWCHAR* SQLWCHAR_FromUnicode(const Py_UNICODE* pch, Py_ssize_t len)
{
    SQLWCHAR* p = (SQLWCHAR*)pyodbc_malloc(sizeof(SQLWCHAR) * len);
    if (p != 0)
    {
        if (!sqlwchar_copy(p, pch, len))
        {
            pyodbc_free(p);
            p = 0;
        }
    }
    return p;
}

