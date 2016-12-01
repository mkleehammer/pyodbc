
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
private:
    Object tmp;
    // If the passed in string/unicode object needed to be encoded, this holds
    // the bytes object it was encoded into.  If set, sz points into this
    // object.

    const char* sz;
    // The value of the string.  If this is zero a Python error occurred in the
    // constructor and nothing further should be one with this.

    Py_ssize_t cch;

    void init(PyObject* value, PyObject* encoding, const char* szDefaultEncoding)
    {
        sz = 0;
        cch = 0;
        const char* szEncoding = szDefaultEncoding;

        Object tmpEncoding;
        if (encoding)
        {
            tmpEncoding = PyCodec_Encode(encoding, "utf-8", "strict");
            if (tmpEncoding)
                szEncoding = PyBytes_AsString(tmpEncoding);
        }

        if (szEncoding)
        {
            tmp = PyCodec_Encode(value, szEncoding, "strict");
            if (tmp)
            {
                sz = PyBytes_AsString(tmp);
                cch = PyBytes_Size(tmp);
            }
        }
    }

public:
    SQLWChar(PyObject* value, const char* szEncoding)
    {
        init(value, 0, szEncoding);
    }
    SQLWChar(PyObject* value, PyObject* encoding, const char* szDefaultEncoding)
    {
        init(value, encoding, szDefaultEncoding);
    }

    operator bool() const { return sz != 0; }

    const char* value() const { return sz; }
    Py_ssize_t len() const { return cch; }
};

#endif // _PYODBCSQLWCHAR_H
