
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

    Py_ssize_t cb;
    // The length of `sz` in *bytes*.

    SQLSMALLINT ctype;
    // The target C type, either SQL_C_CHAR or SQL_C_WCHAR.

    void init(PyObject* value, SQLSMALLINT _ctype, PyObject* encoding, const char* szDefaultEncoding)
    {
        sz = 0;
        cb = 0;
        ctype = _ctype;
        I(ctype == SQL_C_CHAR || ctype == SQL_C_WCHAR);
        const char* szEncoding = szDefaultEncoding;

        if (strcmp(szEncoding, "raw") == 0)
        {
            // If `value` is not a bytes object, PyBytes_AsString below will return 0 which we
            // handle later.  (Do not use AS_STRING which does no error checking.)
            tmp = value;
            sz  = PyBytes_AsString(tmp);
            cb = PyBytes_Size(tmp);
        }
        else
        {
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
                    sz  = PyBytes_AsString(tmp);
                    cb = PyBytes_Size(tmp);
                }
            }
        }
    }

public:
    SQLWChar(PyObject* value, SQLSMALLINT ctype, const char* szEncoding)
    {
        init(value, ctype, 0, szEncoding);
    }
    SQLWChar(PyObject* value, SQLSMALLINT ctype, PyObject* encoding, const char* szDefaultEncoding)
    {
        init(value, ctype, encoding, szDefaultEncoding);
    }

    operator bool() const { return sz != 0; }

    const char* value() const { return sz; }

    Py_ssize_t bytelen() const { return cb; }
    Py_ssize_t charlen() const { return cb / (ctype == SQL_C_WCHAR ? ODBCCHAR_SIZE : 1); }
};

#endif // _PYODBCSQLWCHAR_H
