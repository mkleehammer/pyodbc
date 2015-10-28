
#include "pyodbc.h"
#include "sqlwchar.h"
#include "wrapper.h"

#ifdef HAVE_WCHAR_H
static int WCHAR_T_SIZE = sizeof(wchar_t);
#endif

inline Py_UNICODE CalculateMaxSQL()
{
    if (ODBCCHAR_SIZE >= Py_UNICODE_SIZE)
        return 0;

    Py_UNICODE m = 0;
    for (unsigned int i = 0; i < ODBCCHAR_SIZE; i++)
    {
        m <<= 8;
        m |= 0xFF;
    }
    return m;
}


// If ODBCCHAR is larger than Py_UNICODE, this is the largest value that can be held in a Py_UNICODE.  Because it is
// stored in a Py_UNICODE, it is undefined when sizeof(ODBCCHAR) <= sizeof(Py_UNICODE).
static Py_UNICODE MAX_ODBCCHAR = CalculateMaxSQL();

// If ODBCCHAR is larger than Py_UNICODE, this is the largest value that can be held in a Py_UNICODE.  Because it is
// stored in a Py_UNICODE, it is undefined when sizeof(ODBCCHAR) <= sizeof(Py_UNICODE).
static const ODBCCHAR MAX_PY_UNICODE = (ODBCCHAR)PyUnicode_GetMax();

static bool odbcchar_copy(ODBCCHAR* pdest, const Py_UNICODE* psrc, Py_ssize_t len)
{
    // Copies a Python Unicode string to a SQLWCHAR buffer.  Note that this does copy the NULL terminator, but `len`
    // should not include it.  That is, it copies (len + 1) characters.

    if (Py_UNICODE_SIZE == ODBCCHAR_SIZE)
    {
        memcpy(pdest, psrc, (size_t)ODBCCHAR_SIZE * (len + 1));
    }
    else
    {
        if (ODBCCHAR_SIZE < Py_UNICODE_SIZE)
        {
            for (int i = 0; i < len; i++)
            {
                if ((Py_ssize_t)psrc[i] > MAX_ODBCCHAR)
                {
                    PyErr_Format(PyExc_ValueError, "Cannot convert from Unicode %zd to SQLWCHAR.  Value is too large.", (Py_ssize_t)psrc[i]);
                    return false;
                }
            }
        }
        
        for (int i = 0; i <= len; i++) // ('<=' to include the NULL)
            pdest[i] = (ODBCCHAR)psrc[i];
    }
    
    return true;
}

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

    if (ODBCCHAR_SIZE == Py_UNICODE_SIZE)
    {
        // The ideal case - ODBCCHAR_SIZE and Py_UNICODE are the same, so we point into the Unicode object.

        pch         = (ODBCCHAR*)pU;
        len         = lenT;
        owns_memory = false;
        return true;
    }
    else
    {
        ODBCCHAR* pchT = (ODBCCHAR*)pyodbc_malloc((size_t)ODBCCHAR_SIZE * (lenT + 1));
        if (pchT == 0)
        {
            PyErr_NoMemory();
            return false;
        }

        if (!odbcchar_copy(pchT, pU, lenT))
        {
            pyodbc_free(pchT);
            return false;
        }
    
        pch = pchT;
        len = lenT;
        owns_memory = true;
        return true;
    }
}

PyObject* PyUnicode_FromSQLWCHAR(const SQLWCHAR* sz, Py_ssize_t cch)
{
    // Create a Python Unicode object from a zero-terminated SQLWCHAR.

    if (ODBCCHAR_SIZE == Py_UNICODE_SIZE)
    {
        // The ODBC Unicode and Python Unicode types are the same size.  Cast
        // the ODBC type to the Python type and use a fast function.
        return PyUnicode_FromUnicode((const Py_UNICODE*)sz, cch);
    }
    
#ifdef HAVE_WCHAR_H
    if (WCHAR_T_SIZE == ODBCCHAR_SIZE)
    {
        // The ODBC Unicode is the same as wchar_t.  Python provides a function for that.
        return PyUnicode_FromWideChar((const wchar_t*)sz, cch);
    }
#endif

    // There is no conversion, so we will copy it ourselves with a simple cast.

    if (Py_UNICODE_SIZE < ODBCCHAR_SIZE)
    {
        // We are casting from a larger size to a smaller one, so we'll make sure they all fit.

        for (Py_ssize_t i = 0; i < cch; i++)
        {
            if (((Py_ssize_t)sz[i]) > MAX_PY_UNICODE)
            {
                PyErr_Format(PyExc_ValueError, "Cannot convert from SQLWCHAR %zd to Unicode.  Value is too large.", (Py_ssize_t)sz[i]);
                return 0;
            }
        }
        
    }
    
    Object result(PyUnicode_FromUnicode(0, cch));
    if (!result)
        return 0;

    Py_UNICODE* pch = PyUnicode_AS_UNICODE(result.Get());
    const ODBCCHAR* szT = (const ODBCCHAR*)sz;
    for (Py_ssize_t i = 0; i < cch; i++)
        pch[i] = (Py_UNICODE)szT[i];
    
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
    ODBCCHAR* p = (ODBCCHAR*)pyodbc_malloc((size_t)ODBCCHAR_SIZE * (len+1));
    if (p != 0)
    {
        if (!odbcchar_copy(p, pch, len))
        {
            pyodbc_free(p);
            p = 0;
        }
    }
    return (SQLWCHAR*)p;
}
