
#ifndef _PYODBCSQLWCHAR_H
#define _PYODBCSQLWCHAR_H

typedef unsigned short ODBCCHAR;
// I'm not sure why, but unixODBC seems to define SQLWCHAR as wchar_t even with
// the size is incorrect.  So we might get 4-byte SQLWCHAR on 64-bit Linux even
// though it requires 2-byte characters.  We have to define our own type to
// operate on.

enum {
    ODBCCHAR_SIZE = 2
};

class SQLWChar
{
    // An object designed to convert strings and Unicode objects to SQLWCHAR,
    // hold the temporary buffer, and delete it in the destructor.

private:
    ODBCCHAR* pch;
    Py_ssize_t len;
    bool owns_memory;

public:
    SQLWChar()
    {
        pch = 0;
        len = 0;
        owns_memory = false;
    }

    SQLWChar(PyObject* o);

    bool Convert(PyObject* o);

    void Free();

    ~SQLWChar()
    {
        Free();
    }

    void dump();

    // operator SQLWCHAR*() { return (SQLWCHAR*)pch; }
    // operator const SQLWCHAR*() const { return (const SQLWCHAR*)pch; }

    SQLWCHAR* get() { return (SQLWCHAR*)pch; }
    // The ODBC headers are not const clean.  Don't use this to modify data
    // since we are not actually pointing to a buffer of SQLWCHARs.

    const SQLWCHAR* get() const { return (const SQLWCHAR*)pch; }

    operator bool() const { return pch != 0; }
    Py_ssize_t size() const { return len; }

    ODBCCHAR* operator[] (Py_ssize_t i)
    {
        I(i <= len); // we'll allow access to the NULL?
        return &pch[i];
    }
        
    const ODBCCHAR* operator[] (Py_ssize_t i) const
    {
        I(i <= len); // we'll allow access to the NULL?
        return &pch[i];
    }
};

// Allocate a new Unicode object, initialized from the given SQLWCHAR string.
PyObject* PyUnicode_FromSQLWCHAR(const SQLWCHAR* sz, Py_ssize_t cch);

SQLWCHAR* SQLWCHAR_FromUnicode(const Py_UNICODE* pch, Py_ssize_t len);

#endif // _PYODBCSQLWCHAR_H
