
#ifndef _TEXTENC_H
#define _TEXTENC_H

enum {
    BYTEORDER_LE = -1,
    BYTEORDER_NATIVE = 0,
    BYTEORDER_BE = 1,

    OPTENC_NONE    = 0,         // No optimized encoding - use the named encoding
    OPTENC_RAW     = 1,         // In Python 2, pass bytes directly to string - no decoder
    OPTENC_UTF8    = 2,
    OPTENC_UTF16   = 3,         // "Native", so check for BOM and default to BE
    OPTENC_UTF16BE = 4,
    OPTENC_UTF16LE = 5,
    OPTENC_LATIN1  = 6,

#if PY_MAJOR_VERSION < 3
    TO_UNICODE = 1,
    TO_STR     = 2
#endif
};


struct TextEnc
{
    // Holds encoding information for reading or writing text.  Since some drivers / databases
    // are not easy to configure efficiently, a separate instance of this structure is
    // configured for:
    //
    // * reading SQL_CHAR
    // * reading SQL_WCHAR
    // * writing unicode strings
    // * writing non-unicode strings (Python 2.7 only)

#if PY_MAJOR_VERSION < 3
    int to;
    // The type of object to return if reading from the database: str or unicode.
#endif

    int optenc;
    // Set to one of the OPTENC constants to indicate whether an optimized encoding is to be
    // used or a custom one.  If OPTENC_NONE, no optimized encoding is set and `name` should be
    // used.

    const char* name;
    // The name of the encoding.  This must be freed using `free`.

    SQLSMALLINT ctype;
    // The C type to use, SQL_C_CHAR or SQL_C_WCHAR.  Normally this matches the SQL type of the
    // column (SQL_C_CHAR is used for SQL_CHAR, etc.).  At least one database reports it has
    // SQL_WCHAR data even when configured for UTF-8 which is better suited for SQL_C_CHAR.

    PyObject* Encode(PyObject*) const;
    // Given a string (unicode or str for 2.7), return a bytes object encoded.  This is used
    // for encoding a Python object for passing to a function expecting SQLCHAR* or SQLWCHAR*.
};


PyObject* TextBufferToObject(const TextEnc& enc, void* p, Py_ssize_t len);
// Convert a text buffer to a Python object using the given encoding.
//
// The buffer can be a SQLCHAR array or SQLWCHAR array.  The text encoding
// should match it.

#endif // _TEXTENC_H
