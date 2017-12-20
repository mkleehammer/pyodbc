// https://msdn.microsoft.com/en-us/library/ms711014(v=vs.85).aspx
//
// "The length of both the data buffer and the data it contains is measured in bytes, as
// opposed to characters."
//
// https://msdn.microsoft.com/en-us/library/ms711786(v=vs.85).aspx
//
// Column Size: "For character types, this is the length in characters of the data"

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "pyodbcmodule.h"
#include "params.h"
#include "cursor.h"
#include "connection.h"
#include "buffer.h"
#include "errors.h"
#include "dbspecific.h"
#include "sqlwchar.h"
#include "row.h"
#include <datetime.h>

inline Connection* GetConnection(Cursor* cursor)
{
    return (Connection*)cursor->cnxn;
}

inline bool IsStrType(SQLSMALLINT type)
{
    return type == SQL_WLONGVARCHAR || type == SQL_WVARCHAR ||
           type == SQL_WCHAR || type == SQL_LONGVARCHAR ||
           type == SQL_CHAR || type == SQL_VARCHAR;
}

int PyNone_Check(PyObject *o) { return o == Py_None; };
int PyNullBinary_Check(PyObject *o) { return o == null_binary; };

int PyDecimal_Check(PyObject *o)
{
    PyObject *cls;
    int res = IsInstanceForThread(o, "decimal", "Decimal", &cls) && cls;
    Py_XDECREF(cls);
    return res;
}

int PyUUID_Check(PyObject *o) {
    PyObject *cls;
    int res = IsInstanceForThread(o, "uuid", "UUID", &cls) && cls;
    Py_XDECREF(cls);
    return res;
}

static Py_ssize_t GetDecimalSize(PyObject *cell)
{
    // default precision 28 + 1 decimal point character + 1 sign
    if (Py_None == cell)
        return 30;

    Object t(PyObject_CallMethod(cell, "as_tuple", 0));
    if (!t)
        return 30;

    return PyTuple_GET_SIZE(PyTuple_GET_ITEM(t.Get(), 1)) + 2;
}

static PyObject* CreateDecimalString(long sign, PyObject* digits, long exp)
{
    // Allocate an ASCII string containing the given decimal.

    long count = (long)PyTuple_GET_SIZE(digits);

    char* pch;
    long len;

    if (exp >= 0)
    {
        // (1 2 3) exp = 2 --> '12300'

        len = sign + count + exp + 1; // 1: NULL
        pch = (char*)pyodbc_malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            for (long i = 0; i < count; i++)
                *p++ = (char)('0' + PyInt_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            for (long i = 0; i < exp; i++)
                *p++ = '0';
            *p = 0;
        }
    }
    else if (-exp < count)
    {
        // (1 2 3) exp = -2 --> 1.23 : prec = 3, scale = 2

        len = sign + count + 2; // 2: decimal + NULL
        pch = (char*)pyodbc_malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            int i = 0;
            for (; i < (count + exp); i++)
                *p++ = (char)('0' + PyInt_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = '.';
            for (; i < count; i++)
                *p++ = (char)('0' + PyInt_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = 0;
        }
    }
    else
    {
        // (1 2 3) exp = -5 --> 0.00123 : prec = 5, scale = 5

        len = sign + -exp + 3; // 3: leading zero + decimal + NULL

        pch = (char*)pyodbc_malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            *p++ = '0';
            *p++ = '.';

            for (int i = 0; i < -(exp + count); i++)
                *p++ = '0';

            for (int i = 0; i < count; i++)
                *p++ = (char)('0' + PyInt_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = 0;
        }
    }

    I(pch == 0 || (int)(strlen(pch) + 1) == len);
    PyObject *strobj = PyString_FromString(pch);
    free(pch);
    return strobj;
}

static int DetectSQLType(Cursor *cur, PyObject *cell, ParamInfo *pi)
{
    if (PyBool_Check(cell))
    {
        pi->ParameterType = SQL_BIT;
        pi->ColumnSize = 1;
    }
    else if (
#if PY_MAJOR_VERSION < 3    
    PyInt_Check(cell) ||
#endif   
    PyLong_Check(cell))
    {
        // Try to see if the value is INTEGER or BIGINT
        unsigned long val = PyLong_AsLong(cell);
        if(!PyErr_Occurred())
        {
            pi->ParameterType = (val > 0x7FFFFFFF)? SQL_BIGINT : SQL_INTEGER;
        }
        else
        {
            // Fallback to default
            pi->ParameterType = SQL_INTEGER;
        }

        pi->ColumnSize = 12;
    }
    else if (PyFloat_Check(cell))
    {
        pi->ParameterType = SQL_DOUBLE;
        pi->ColumnSize = 15;
    }
    else if (PyBytes_Check(cell))
    {
        // Assume the SQL type is also character (2.x) or binary (3.x).
        // In 2.x, also check the SQL type and adjust appropriately.
        // If it is a max-type (ColumnSize == 0), use DAE.
#if PY_MAJOR_VERSION < 3
        pi->ParameterType = cur->cnxn->str_enc.ctype == SQL_C_CHAR ? SQL_VARCHAR: SQL_WVARCHAR;
        Py_ssize_t cch = PyString_GET_SIZE(cell);
        pi->ColumnSize = (SQLUINTEGER)max(cch, 1);
#else
        pi->ParameterType = SQL_VARBINARY;
        Py_ssize_t cb = PyBytes_GET_SIZE(cell);
        pi->ColumnSize = (SQLUINTEGER)max(cb, 1);
#endif
    }
    else if (PyUnicode_Check(cell))
    {
        // Assume the SQL type should also be wide character.
        pi->ParameterType = cur->cnxn->unicode_enc.ctype == SQL_C_CHAR ? SQL_VARCHAR: SQL_WVARCHAR;
        pi->ColumnSize = (SQLUINTEGER)max(PyUnicode_GET_SIZE(cell), 1);
    }
    else if (PyDateTime_Check(cell))
    {
        pi->ParameterType = SQL_TIMESTAMP;
        pi->ColumnSize = (SQLUINTEGER)(cur->cnxn->datetime_precision);
        int precision = cur->cnxn->datetime_precision - 20;
        pi->DecimalDigits = precision < 0 ? 0 : precision;
    }
    else if (PyDate_Check(cell))
    {
        pi->ParameterType = SQL_TYPE_DATE;
        pi->ColumnSize = 10;
    }
    else if (PyTime_Check(cell))
    {
        pi->ParameterType = SQL_TYPE_TIME;
        pi->ColumnSize = 8;
    }
#if PY_VERSION_HEX >= 0x02060000
    else if (PyByteArray_Check(cell))
    {
        pi->ParameterType = SQL_VARBINARY;
        pi->ColumnSize = (SQLUINTEGER)max(PyByteArray_Size(cell), 1);
    }
#endif
#if PY_MAJOR_VERSION < 3
    else if (PyBuffer_Check(cell))
    {
        const char *pb;
        pi->ParameterType = SQL_VARBINARY;
        pi->ColumnSize = (SQLUINTEGER)max(PyBuffer_GetMemory(cell, &pb), 1);
    }
#endif
    else if (cell == Py_None)
    {
        pi->ParameterType = SQL_VARCHAR;
        pi->ColumnSize = 255;
    }
    else if (cell == null_binary)
    {
        pi->ParameterType = SQL_VARBINARY;
        pi->ColumnSize = 1;
    }
    else if (PyUUID_Check(cell))
    {
        // UUID
        pi->ParameterType = SQL_GUID;
        pi->ColumnSize = 16;
    }
    else if (PyDecimal_Check(cell))
    {
        pi->ParameterType = SQL_NUMERIC;
        Object t(PyObject_CallMethod(cell, "as_tuple", 0));
        if (!t)
            return false;

        PyObject*  digits = PyTuple_GET_ITEM(t.Get(), 1);
        long       exp    = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 2));

        Py_ssize_t count = PyTuple_GET_SIZE(digits);

        if (exp >= 0)
        {
            // (1 2 3) exp = 2 --> '12300'

            pi->ColumnSize    = (SQLUINTEGER)count + exp;
            pi->DecimalDigits = 0;

        }
        else if (-exp <= count)
        {
            // (1 2 3) exp = -2 --> 1.23 : prec = 3, scale = 2
            pi->ColumnSize    = (SQLUINTEGER)count;
            pi->DecimalDigits = (SQLSMALLINT)-exp;
        }
        else
        {
            // (1 2 3) exp = -5 --> 0.00123 : prec = 5, scale = 5
            pi->ColumnSize    = (SQLUINTEGER)(count + (-exp));
            pi->DecimalDigits = (SQLSMALLINT)pi->ColumnSize;
        }
    }
    else
    {
        RaiseErrorV(0, ProgrammingError, "Unknown object type %s during describe", cell->ob_type->tp_name);
        return false;
    }
    return true;
}

// Detects and sets the appropriate C type to use for binding the specified Python object.
// Assumes the SQL type has already been detected.
// Also sets the buffer length to use.
// Returns false if unsuccessful.
static int DetectCType(Cursor *cur, PyObject *cell, ParamInfo *pi)
{
    if (PyBool_Check(cell))
    {
    Type_Bool:
        pi->ValueType = SQL_C_BIT;
        pi->BufferLength = 1;
    }
#if PY_MAJOR_VERSION < 3
    else if (PyInt_Check(cell))
    {
    Type_Int:
        pi->ValueType = sizeof(long) == sizeof(SQLBIGINT) ? SQL_C_SBIGINT : SQL_C_LONG;
        pi->BufferLength = sizeof(long);
    }
#endif
    else if (PyLong_Check(cell))
    {
    Type_Long:
        if (IsStrType(pi->ParameterType))
        {
            pi->ValueType = SQL_C_CHAR;
            pi->BufferLength = pi->ColumnSize;
        }
        else if (pi->ParameterType == SQL_REAL || pi->ParameterType == SQL_FLOAT || pi->ParameterType == SQL_DOUBLE)
        {
            pi->ValueType = SQL_C_DOUBLE;
            pi->BufferLength = sizeof(double);
        }
        else if (pi->ParameterType == SQL_NUMERIC ||
            pi->ParameterType == SQL_DECIMAL)
        {
            pi->ValueType = SQL_C_NUMERIC;
            pi->BufferLength = sizeof(SQL_NUMERIC_STRUCT);
        }
        else if (pi->ParameterType == SQL_BIT || pi->ParameterType == SQL_TINYINT ||
                 pi->ParameterType == SQL_INTEGER || pi->ParameterType == SQL_SMALLINT)
        {
            pi->ValueType = SQL_C_LONG;
            pi->BufferLength = sizeof(SQLINTEGER);
        }
        else
        {
            pi->ValueType = SQL_C_SBIGINT;
            pi->BufferLength = sizeof(SQLBIGINT);
        }
    }
    else if (PyFloat_Check(cell))
    {
    Type_Float:
        pi->ValueType = SQL_C_DOUBLE;
        pi->BufferLength = sizeof(double);
    }
    else if (PyBytes_Check(cell))
    {
    Type_Bytes:
        // Assume the SQL type is also character (2.x) or binary (3.x).
        // In 2.x, also check the SQL type and adjust appropriately.
        // If it is a max-type (ColumnSize == 0), use DAE.
#if PY_MAJOR_VERSION < 3
        pi->ValueType = pi->ParameterType == SQL_LONGVARBINARY ||
                        pi->ParameterType == SQL_VARBINARY ||
                        pi->ParameterType == SQL_BINARY ? SQL_C_BINARY : SQL_C_CHAR;
#else
        pi->ValueType = SQL_C_BINARY;
#endif
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize : sizeof(DAEParam);
    }
    else if (PyUnicode_Check(cell))
    {
    Type_Unicode:
        // Assume the SQL type should also be wide character.
        // If it is a max-type (ColumnSize == 0), use DAE.
        pi->ValueType = cur->cnxn->unicode_enc.ctype; // defaults to SQL_C_WCHAR;
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize * sizeof(SQLWCHAR) : sizeof(DAEParam);
    }
    else if (PyDateTime_Check(cell))
    {
    Type_DateTime:
        if (pi->ParameterType == SQL_SS_TIMESTAMPOFFSET || pi->ParameterType == SQL_SS_TIME2)
        {
            pi->ValueType = SQL_C_BINARY;
            pi->BufferLength =
                pi->ParameterType == SQL_SS_TIMESTAMPOFFSET ? sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT)
                                                            : sizeof(SQL_SS_TIME2_STRUCT);
        }
        else
        {
            pi->ValueType = SQL_C_TYPE_TIMESTAMP;
            pi->BufferLength = sizeof(SQL_TIMESTAMP_STRUCT);
        }
    }
    else if (PyDate_Check(cell))
    {
    Type_Date:
        pi->ValueType = SQL_C_TYPE_DATE;
        pi->BufferLength = sizeof(SQL_DATE_STRUCT);
    }
    else if (PyTime_Check(cell))
    {
    Type_Time:
        if (pi->ParameterType == SQL_SS_TIME2)
        {
            pi->ValueType = SQL_C_BINARY;
            pi->BufferLength = sizeof(SQL_SS_TIME2_STRUCT);
        }
        else if (pi->ParameterType == SQL_SS_TIMESTAMPOFFSET)
        {
            pi->ValueType = SQL_C_BINARY;
            pi->BufferLength = sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT);
        }
        else
        {
            pi->ValueType = SQL_C_TYPE_TIME;
            pi->BufferLength = sizeof(SQL_TIME_STRUCT);
        }
    }
#if PY_VERSION_HEX >= 0x02060000
    else if (PyByteArray_Check(cell))
    {
    Type_ByteArray:
        pi->ValueType = SQL_C_BINARY;
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize : sizeof(DAEParam);
    }
#endif
#if PY_MAJOR_VERSION < 3
    else if (PyBuffer_Check(cell))
    {
        pi->ValueType = SQL_C_BINARY;
        pi->BufferLength = pi->ColumnSize && PyBuffer_GetMemory(cell, 0) >= 0 ? pi->ColumnSize : sizeof(DAEParam);
    }
#endif
    else if (cell == Py_None)
    {
        // Use the SQL type to guess what Nones should be inserted as here.
        switch (pi->ParameterType)
        {
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
            goto Type_Bytes;
        case SQL_WCHAR:
        case SQL_WVARCHAR:
        case SQL_WLONGVARCHAR:
            goto Type_Unicode;
        case SQL_DECIMAL:
        case SQL_NUMERIC:
            goto Type_Decimal;
        case SQL_BIGINT:
            goto Type_Long;
        case SQL_SMALLINT:
        case SQL_INTEGER:
        case SQL_TINYINT:
#if PY_MAJOR_VERSION < 3
            goto Type_Int;
#else
            goto Type_Long;
#endif
        case SQL_REAL:
        case SQL_FLOAT:
        case SQL_DOUBLE:
            goto Type_Float;
        case SQL_BIT:
            goto Type_Bool;
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
#if PY_VERSION_HEX >= 0x02060000
            goto Type_ByteArray;
#else
            goto Type_Bytes;
#endif
        case SQL_TYPE_DATE:
            goto Type_Date;
        case SQL_SS_TIME2:
        case SQL_TYPE_TIME:
            goto Type_Time;
        case SQL_TYPE_TIMESTAMP:
            goto Type_DateTime;
        case SQL_GUID:
            goto Type_UUID;
        default:
            goto Type_Bytes;
        }
    }
    else if (cell == null_binary)
    {
        pi->ValueType = SQL_C_BINARY;
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize : sizeof(DAEParam);
    }
    else if (PyUUID_Check(cell))
    {
    Type_UUID:
        // UUID
        pi->ValueType = SQL_C_GUID;
        pi->BufferLength = 16;
    }
    else if (PyDecimal_Check(cell))
    {
        bool isStrType;
    Type_Decimal:
        if ((isStrType = IsStrType(pi->ParameterType)) || cur->decimal_as_string)
        {
            pi->ValueType = SQL_C_CHAR;
            // Use the column size for the buffer length if available; otherwise, use the
            // precision of the decimal if it is not null, and fall back to the default
            // precision of the Decimal class (28).
            pi->BufferLength = isStrType ? pi->ColumnSize : GetDecimalSize(cell);
        }
        else if(pi->ParameterType == SQL_FLOAT || pi->ParameterType == SQL_REAL || pi->ParameterType == SQL_DOUBLE)
        {
            pi->ValueType = SQL_C_DOUBLE;
            pi->BufferLength = sizeof(double);
        }
        else
        {
            pi->ValueType = SQL_C_NUMERIC;
            pi->BufferLength = sizeof(SQL_NUMERIC_STRUCT);
        }
    }
    else
    {
        RaiseErrorV(0, ProgrammingError, "Unknown object type %s during describe", cell->ob_type->tp_name);
        return false;
    }
    return true;
}

#define WRITEOUT(type, ptr, val, indv) { *(type*)(*ptr) = (val); *ptr += sizeof(type); indv = sizeof(type); }
// Convert Python object into C data for binding.
// Output pointer is written to with data, indicator, and updated.
// If !outbuf || !*outbuf, binds into the ParamInfo directly.
// Returns false if object could not be converted.
static int PyToCType(Cursor *cur, unsigned char **outbuf, PyObject *cell, ParamInfo *pi)
{
    unsigned char *tmpptr = 0;
    TRACE("PyToCType %p %p %p (%s) %p\n", cur, outbuf, cell, cell->ob_type->tp_name, pi);
    if (!outbuf || !*outbuf)
    {
        tmpptr = (unsigned char*)&pi->Data;
        pi->ParameterValuePtr = tmpptr;
        outbuf = &tmpptr;
    }

    // TODO: Any way to make this a switch (O(1)) or similar instead of if-else chain?
    // TODO: Otherwise, rearrange these cases in order of frequency...
    SQLLEN ind;
    if (PyBool_Check(cell))
    {
        if (pi->ValueType != SQL_C_BIT)
            return false;
        WRITEOUT(char, outbuf, cell == Py_True, ind);
    }
#if PY_MAJOR_VERSION < 3
    else if (PyInt_Check(cell))
    {
        if (pi->ValueType != (sizeof(long) == 8 ? SQL_C_SBIGINT : SQL_C_LONG))
            return false;
        WRITEOUT(long, outbuf, PyInt_AS_LONG(cell), ind);
    }
#endif
    else if (PyLong_Check(cell))
    {
        if (pi->ValueType == SQL_C_LONG)
        {
            WRITEOUT(SQLINTEGER, outbuf, PyLong_AsLong(cell), ind);
        }
        else if (pi->ValueType == SQL_C_SBIGINT)
        {
            WRITEOUT(SQLBIGINT, outbuf, PyLong_AsLongLong(cell), ind);
        }
        else if (pi->ValueType == SQL_C_NUMERIC)
        {
            // Convert a PyLong into a SQL_NUMERIC_STRUCT, without losing precision
            // or taking an unnecessary trip through character strings.
            SQL_NUMERIC_STRUCT *pNum = (SQL_NUMERIC_STRUCT*)*outbuf;
            PyObject *absVal = PyNumber_Absolute(cell);
            if (pi->DecimalDigits)
            {
                static PyObject *scaler_table[38];
                static PyObject *tenObject;

                // Need to scale by 10**pi->DecimalDigits
                if (pi->DecimalDigits > 38)
                {
                NumericOverflow:
                    RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                    Py_XDECREF(absVal);
                    return false;
                }
                
                if (!scaler_table[pi->DecimalDigits - 1])
                {
                    if (!tenObject)
                        tenObject = PyInt_FromLong(10);
                    PyObject *scaleObj = PyInt_FromLong(pi->DecimalDigits);
                    scaler_table[pi->DecimalDigits - 1] = PyNumber_Power(tenObject, scaleObj, Py_None);
                    Py_XDECREF(scaleObj);
                }
                PyObject *scaledVal = PyNumber_Multiply(absVal, scaler_table[pi->DecimalDigits - 1]);
                Py_XDECREF(absVal);
                absVal = scaledVal;
            }
            pNum->precision = pi->ColumnSize;
            pNum->scale = pi->DecimalDigits;
            pNum->sign = _PyLong_Sign(cell) >= 0;
            if (_PyLong_AsByteArray((PyLongObject*)absVal, pNum->val, sizeof(pNum->val), 1, 0))
                goto NumericOverflow;
            Py_XDECREF(absVal);
            *outbuf += pi->BufferLength;
            ind = sizeof(SQL_NUMERIC_STRUCT);
        }
        else if (pi->ValueType == SQL_C_CHAR)
        {
            Object longstr(PyObject_Str(cell));
            char *pstr;
            Py_ssize_t len;
            PyBytes_AsStringAndSize(longstr.Get(), &pstr, &len);
            if (len > pi->ColumnSize)
            {
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                return false;
            }
            if (tmpptr)
            {
                pi->ParameterValuePtr = pstr;
                pi->pObject = longstr.Detach();
                Py_INCREF(pi->pObject);
            }
            else
            {
                memcpy(*outbuf, pstr, len);
                *outbuf += pi->BufferLength;
            }
            ind = len;
        }
        else if (pi->ValueType == SQL_C_DOUBLE)
        {
            WRITEOUT(double, outbuf, PyLong_AsDouble(cell), ind);
        }
        else
            return false;
    }
    else if (PyFloat_Check(cell))
    {
        if (pi->ValueType != SQL_C_DOUBLE)
            return false;
        WRITEOUT(double, outbuf, PyFloat_AS_DOUBLE(cell), ind);
    }
    else if (PyBytes_Check(cell))
    {
#if PY_MAJOR_VERSION < 3
        if (pi->ValueType != SQL_C_CHAR)
#else
        if (pi->ValueType != SQL_C_BINARY)
#endif
            return false;

#if PY_MAJOR_VERSION < 3
        Object encoded;
        const TextEnc& enc = cur->cnxn->str_enc;
        if (enc.optenc != OPTENC_RAW)
        {
            // Need to reencode the data using the specified encoding.
            encoded = PyCodec_Encode(cell, enc.name, "strict");
            if (!encoded)
                return false;
            if (!PyBytes_CheckExact(encoded))
            {
                // Not all encodings return bytes.
                PyErr_Format(PyExc_TypeError, "Unicode read encoding '%s' returned unexpected data type: %s",
                             enc.name, encoded.Get()->ob_type->tp_name);
                return false;
            }
            cell = encoded.Get();
        }
#endif

        Py_ssize_t len = PyBytes_GET_SIZE(cell);
        if (!pi->ColumnSize) // DAE
        {
            DAEParam *pParam = (DAEParam*)*outbuf;
            Py_INCREF(cell);
#if PY_MAJOR_VERSION < 3
            encoded.Detach();
#endif
            pParam->cell = cell;
            pParam->maxlen = cur->cnxn->GetMaxLength(pi->ValueType);
            *outbuf += sizeof(DAEParam);
            ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        }
        else
        {
            if (len > pi->BufferLength)
            {
                RaiseErrorV("22001", 0, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                return false;
            }
            if (tmpptr)
            {
                pi->ParameterValuePtr = PyBytes_AS_STRING((PyObject*)cell);
                Py_INCREF(cell);
                pi->pObject = cell;
            }
            else
            {
                memcpy(*outbuf, PyBytes_AS_STRING(cell), len);
                *outbuf += pi->BufferLength;
            }
            ind = len;
        }
    }
    else if (PyUnicode_Check(cell))
    {
        const TextEnc& enc = cur->cnxn->unicode_enc;
        Py_ssize_t len = PyUnicode_GET_SIZE(cell);
        //         Same size      Different size
        // DAE     DAE only       Convert + DAE
        // non-DAE Copy           Convert + Copy
        if (sizeof(Py_UNICODE) != sizeof(SQLWCHAR) || strcmp(enc.name, "utf-16le"))
        {
            Object encoded(PyCodec_Encode(cell, enc.name, "strict"));
            if (!encoded)
                return false;

            if (enc.optenc == OPTENC_NONE && !PyBytes_CheckExact(encoded))
            {
                PyErr_Format(PyExc_TypeError, "Unicode write encoding '%s' returned unexpected data type: %s",
                             enc.name, encoded.Get()->ob_type->tp_name);
                return false;
            }

            len = PyBytes_GET_SIZE(encoded);
            if (!pi->ColumnSize)
            {
                // DAE
                DAEParam *pParam = (DAEParam*)*outbuf;
                Py_INCREF(cell);
                pParam->cell = encoded.Detach();
                pParam->maxlen = cur->cnxn->GetMaxLength(pi->ValueType);
                *outbuf += sizeof(DAEParam);
                ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
            }
            else
            {
                if (len > pi->BufferLength)
                {
                    RaiseErrorV("22001", 0, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                    return false;
                }
                if (tmpptr)
                {
                    pi->ParameterValuePtr = PyBytes_AS_STRING(encoded.Get());
                    pi->pObject = encoded.Detach();
                    Py_INCREF(pi->pObject);
                }
                else
                {
                    memcpy(*outbuf, PyBytes_AS_STRING((PyObject*)encoded), len);
                    *outbuf += pi->BufferLength;
                }
                ind = len;
            }
        }
        else
        {
            len *= sizeof(SQLWCHAR);

            if (!pi->ColumnSize) // DAE
            {
                Py_INCREF(cell);
                DAEParam *pParam = (DAEParam*)*outbuf;
                pParam->cell = cell;
                pParam->maxlen= cur->cnxn->GetMaxLength(pi->ValueType);
                *outbuf += sizeof(DAEParam);
                ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
            }
            else
            {
                if (len > pi->BufferLength)
                {
                    RaiseErrorV("22001", 0, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                    return false;
                }
                if(tmpptr)
                {
                    pi->ParameterValuePtr = (SQLPOINTER)PyUnicode_AS_DATA(cell);
                    pi->pObject = cell;
                    Py_INCREF(cell);
                }
                else
                {
                    memcpy(*outbuf, PyUnicode_AS_DATA(cell), len);
                    *outbuf += pi->BufferLength;
                }
                ind = len;
            }
        }
    }
    else if (PyDateTime_Check(cell))
    {
        if (pi->ValueType == SQL_C_TYPE_TIMESTAMP)
        {
            static int scaler_table[] = { 1000000, 100000, 10000, 1000, 100, 10 };
            SQL_TIMESTAMP_STRUCT *pts = (SQL_TIMESTAMP_STRUCT*)*outbuf;
            pts->year = PyDateTime_GET_YEAR(cell);
            pts->month = PyDateTime_GET_MONTH(cell);
            pts->day = PyDateTime_GET_DAY(cell);
            pts->hour = PyDateTime_DATE_GET_HOUR(cell);
            pts->minute = PyDateTime_DATE_GET_MINUTE(cell);
            pts->second = PyDateTime_DATE_GET_SECOND(cell);
            pts->fraction = 1000 * (pi->DecimalDigits < 6 ? 
                PyDateTime_DATE_GET_MICROSECOND(cell) / scaler_table[pi->DecimalDigits]
                                                      * scaler_table[pi->DecimalDigits]
                : PyDateTime_DATE_GET_MICROSECOND(cell));
            *outbuf += sizeof(SQL_TIMESTAMP_STRUCT);
            ind = sizeof(SQL_TIMESTAMP_STRUCT);
        }
        else if (pi->ValueType == SQL_C_BINARY)
        {
            if (pi->ParameterType == SQL_SS_TIMESTAMPOFFSET)
            {
                SQL_SS_TIMESTAMPOFFSET_STRUCT *pts = (SQL_SS_TIMESTAMPOFFSET_STRUCT*)*outbuf;
                pts->year = PyDateTime_GET_YEAR(cell);
                pts->month = PyDateTime_GET_MONTH(cell);
                pts->day = PyDateTime_GET_DAY(cell);
                pts->hour = PyDateTime_DATE_GET_HOUR(cell);
                pts->minute = PyDateTime_DATE_GET_MINUTE(cell);
                pts->second = PyDateTime_DATE_GET_SECOND(cell);
                pts->fraction = PyDateTime_DATE_GET_MICROSECOND(cell) * 1000;
                PyObject *ptz = PyObject_CallMethod(cell, "utcoffset", 0);
                if (ptz && ptz != Py_None)
                {
                    Object tzo(PyObject_GetAttrString(ptz, "seconds"));
                    Py_ssize_t tzseconds = PyNumber_AsSsize_t(tzo.Get(), 0);
                    pts->timezone_hour = tzseconds / 3600;
                    pts->timezone_minute = tzseconds % 3600 / 60;
                }
                else
                {
                    pts->timezone_hour = 0;
                    pts->timezone_minute = 0;
                }
                Py_XDECREF(ptz);
                *outbuf += sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT);
                ind = sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT);
            }
            else // SQL_SS_TIME2
            {
                SQL_SS_TIME2_STRUCT *pts = (SQL_SS_TIME2_STRUCT*)*outbuf;
                pts->hour = PyDateTime_DATE_GET_HOUR(cell);
                pts->minute = PyDateTime_DATE_GET_MINUTE(cell);
                pts->second = PyDateTime_DATE_GET_SECOND(cell);
                pts->fraction = PyDateTime_DATE_GET_MICROSECOND(cell) * 1000;
                *outbuf += sizeof(SQL_SS_TIME2_STRUCT);
                ind = sizeof(SQL_SS_TIME2_STRUCT);
            }
        }
        else
            return false;
    }
    else if (PyDate_Check(cell))
    {
        if (pi->ValueType != SQL_C_TYPE_DATE)
            return false;
        SQL_DATE_STRUCT *pds = (SQL_DATE_STRUCT*)*outbuf;
        pds->year = PyDateTime_GET_YEAR(cell);
        pds->month = PyDateTime_GET_MONTH(cell);
        pds->day = PyDateTime_GET_DAY(cell);
        *outbuf += sizeof(SQL_DATE_STRUCT);
        ind = sizeof(SQL_DATE_STRUCT);
    }
    else if (PyTime_Check(cell))
    {
        if (pi->ParameterType == SQL_SS_TIME2)
        {
            if (pi->ValueType != SQL_C_BINARY)
                return false;
            SQL_SS_TIME2_STRUCT *pt2s = (SQL_SS_TIME2_STRUCT*)*outbuf;
            pt2s->hour = PyDateTime_TIME_GET_HOUR(cell);
            pt2s->minute = PyDateTime_TIME_GET_MINUTE(cell);
            pt2s->second = PyDateTime_TIME_GET_SECOND(cell);
            // This is in units of nanoseconds.
            pt2s->fraction = PyDateTime_TIME_GET_MICROSECOND(cell)*1000;
            *outbuf += sizeof(SQL_SS_TIME2_STRUCT);
            ind = sizeof(SQL_SS_TIME2_STRUCT);
        }
        else if (pi->ParameterType == SQL_SS_TIMESTAMPOFFSET)
        {
            if (pi->ValueType != SQL_C_BINARY)
                return false;
            SQL_SS_TIMESTAMPOFFSET_STRUCT *pts = (SQL_SS_TIMESTAMPOFFSET_STRUCT*)*outbuf;
            pts->year = 1;
            pts->month = 1;
            pts->day = 1;
            pts->hour = PyDateTime_TIME_GET_HOUR(cell);
            pts->minute = PyDateTime_TIME_GET_MINUTE(cell);
            pts->second = PyDateTime_TIME_GET_SECOND(cell);
            pts->fraction = PyDateTime_TIME_GET_MICROSECOND(cell) * 1000;
            PyObject *ptz = PyObject_CallMethod(cell, "utcoffset", 0);
            if (ptz && ptz != Py_None)
            {
                Object tzo(PyObject_GetAttrString(ptz, "seconds"));
                Py_ssize_t tzseconds = PyNumber_AsSsize_t(tzo.Get(), 0);
                pts->timezone_hour = tzseconds / 3600;
                pts->timezone_minute = tzseconds % 3600 / 60;
            }
            else
            {
                pts->timezone_hour = 0;
                pts->timezone_minute = 0;
            }
            Py_XDECREF(ptz);
            *outbuf += sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT);
            ind = sizeof(SQL_SS_TIMESTAMPOFFSET_STRUCT);
        }
        else
        {
            if (pi->ValueType != SQL_C_TYPE_TIME)
                return false;
            SQL_TIME_STRUCT *pts = (SQL_TIME_STRUCT*)*outbuf;
            pts->hour = PyDateTime_TIME_GET_HOUR(cell);
            pts->minute = PyDateTime_TIME_GET_MINUTE(cell);
            pts->second = PyDateTime_TIME_GET_SECOND(cell);
            *outbuf += sizeof(SQL_TIME_STRUCT);
            ind = sizeof(SQL_TIME_STRUCT);
        }
    }
#if PY_VERSION_HEX >= 0x02060000
    else if (PyByteArray_Check(cell))
    {
        if (pi->ValueType != SQL_C_BINARY)
            return false;
        Py_ssize_t len = PyByteArray_GET_SIZE(cell);
        if (!pi->ColumnSize) // DAE
        {
            DAEParam *pParam = (DAEParam*)*outbuf;
            Py_INCREF(cell);
            pParam->cell = cell;
            pParam->maxlen = cur->cnxn->GetMaxLength(pi->ValueType);
            *outbuf += sizeof(DAEParam);
            ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        }
        else
        {
            if (len > pi->BufferLength)
            {
                RaiseErrorV("22001", 0, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                return false;
            }
            if (tmpptr)
            {
                pi->ParameterValuePtr = PyByteArray_AS_STRING((PyObject*)cell);
                Py_INCREF(cell);
                pi->pObject = cell;
            }
            else
            {
                memcpy(*outbuf, PyByteArray_AS_STRING(cell), len);
                *outbuf += pi->BufferLength;
            }
            ind = len;
        }
    }
#endif
#if PY_MAJOR_VERSION < 3
    else if (PyBuffer_Check(cell))
    {
        if (pi->ValueType != SQL_C_BINARY)
            return false;
        const char* pb;
        Py_ssize_t  len = PyBuffer_GetMemory(cell, &pb);
        if (!pi->ColumnSize || len < 0)
        {
            // DAE
            DAEParam *pParam = (DAEParam*)*outbuf;
            len = PyBuffer_Size(cell);
            Py_INCREF(cell);
            pParam->cell = cell;
            pParam->maxlen = cur->cnxn->GetMaxLength(pi->ValueType);
            *outbuf += pi->BufferLength;
            ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        }
        else
        {
            if (len > pi->BufferLength)
            {
                RaiseErrorV("22001", 0, "String data, right truncation: row %u column %u", 0 /* TODO */, 0 /* TODO */);
                return false;
            }
            if (tmpptr)
            {
                pi->ParameterValuePtr = (SQLPOINTER)pb;
                pi->pObject = cell;
                Py_INCREF(cell);
            }
            else
            {
                memcpy(*outbuf, pb, len);
                *outbuf += pi->BufferLength;
            }
            ind = len;
        }
    }
#endif
    else if (PyUUID_Check(cell))
    {
        if (pi->ValueType != SQL_C_GUID)
            return false;
        pi->BufferLength = 16;
        // Do we need to use "bytes" on a big endian machine?
        Object b(PyObject_GetAttrString(cell, "bytes_le"));
        if (!b)
            return false;
        memcpy(*outbuf, PyBytes_AS_STRING(b.Get()), sizeof(SQLGUID));
        *outbuf += pi->BufferLength;
        ind = 16;
    }
    else if (PyDecimal_Check(cell))
    {
        if (pi->ValueType == SQL_C_NUMERIC)
        {
            // Get sign, exponent, and digits.
            PyObject *cellParts = PyObject_CallMethod(cell, "as_tuple", 0);
            if (!cellParts)
                return false;

            SQL_NUMERIC_STRUCT *pNum = (SQL_NUMERIC_STRUCT*)*outbuf;
            pNum->sign = !PyInt_AsLong(PyTuple_GET_ITEM(cellParts, 0));
            PyObject*  digits = PyTuple_GET_ITEM(cellParts, 1);
            long       exp    = PyInt_AsLong(PyTuple_GET_ITEM(cellParts, 2));
            Py_ssize_t numDigits = PyTuple_GET_SIZE(digits);

            // PyDecimal is digits * 10**exp = digits / 10**-exp
            // SQL_NUMERIC_STRUCT is val / 10**scale
            Py_ssize_t scaleDiff = pi->DecimalDigits + exp;
            if (scaleDiff < 0)
            {
                RaiseErrorV(0, ProgrammingError, "Converting decimal loses precision");
                return false;
            }
            
            // Append '0's to the end of the digits to effect the scaling.
            PyObject *newDigits = PyTuple_New(numDigits + scaleDiff);
            for (Py_ssize_t i = 0; i < numDigits; i++)
            {
                PyTuple_SET_ITEM(newDigits, i, PyInt_FromLong(PyNumber_AsSsize_t(PyTuple_GET_ITEM(digits, i), 0)));
            }
            for (Py_ssize_t i = numDigits; i < scaleDiff + numDigits; i++)
            {
                PyTuple_SET_ITEM(newDigits, i, PyInt_FromLong(0));
            }
            PyObject *args = Py_BuildValue("((iOi))", 0, newDigits, 0);
            PyObject *scaledDecimal = PyObject_CallObject((PyObject*)cell->ob_type, args);
            PyObject *digitLong = PyNumber_Long(scaledDecimal);

            Py_XDECREF(args);
            Py_XDECREF(scaledDecimal);
            Py_XDECREF(cellParts);

            pNum->precision = pi->ColumnSize;
            pNum->scale = pi->DecimalDigits;

            int ret = _PyLong_AsByteArray((PyLongObject*)digitLong, pNum->val, sizeof(pNum->val), 1, 0);

            Py_XDECREF(digitLong);
            if (ret)
            {
                PyErr_Clear();
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                return false;
            }
            *outbuf += pi->BufferLength;
            ind = sizeof(SQL_NUMERIC_STRUCT);
        }
        else if (pi->ValueType == SQL_C_DOUBLE)
        {
            Object floatobj(PyNumber_Float(cell));
            if(!floatobj)
                return false;
            WRITEOUT(double, outbuf, PyFloat_AS_DOUBLE(floatobj.Get()), ind);
        }
        else if (pi->ValueType == SQL_C_CHAR)
        {
            Object t(PyObject_CallMethod(cell, "as_tuple", 0));
            if (!t)
                return false;

            long       sign   = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 0));
            PyObject*  digits = PyTuple_GET_ITEM(t.Get(), 1);
            long       exp    = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 2));

            Object decstr(CreateDecimalString(sign, digits, exp));
            Py_ssize_t len = PyBytes_Size(decstr.Get());
            char *pb = PyBytes_AsString(decstr.Get());
            if (len > pi->BufferLength)
            {
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                len = pi->BufferLength;
            }
            if (tmpptr)
            {
                pi->ParameterValuePtr = pb;
                pi->pObject = decstr.Detach();
                Py_INCREF(pi->pObject);
            }
            else
            {
                memcpy(*outbuf, pb, len);
                *outbuf += pi->BufferLength;
            }
            ind = len;
        }
        else
            return false;
    }
    else if (cell == Py_None || cell == null_binary)
    {
        *outbuf += pi->BufferLength;
        ind = SQL_NULL_DATA;
    }
    else
    {
        RaiseErrorV(0, ProgrammingError, "Unknown object type: %s",cell->ob_type->tp_name);
        return false;
    }
    pi->StrLen_or_Ind = ind;
    if (!tmpptr)
    {
        *(SQLLEN*)(*outbuf) = ind;
        *outbuf += sizeof(SQLLEN);
    }
    return true;
}

int BindAndConvert(Cursor *cur, Py_ssize_t i, PyObject *cell, ParamInfo *ppi)
{
    // For non-fastexecutemany case.
    // Scatter-bind using the ParamInfo, and convert the data into the bound buffer.
    // Variable-length types (char, wchar, binary) are bound using an extra buffer;
    // fixed-length types are bound directly into the ParamInfo.
    if (!PyToCType(cur, 0, cell, ppi))
        return false;

    TRACE("BIND: param=%ld ValueType=%d (%s) ParameterType=%d (%s) ColumnSize=%ld DecimalDigits=%d BufferLength=%ld *pcb=%ld\n",
          (i+1), ppi->ValueType, CTypeName(ppi->ValueType), ppi->ParameterType, SqlTypeName(ppi->ParameterType), ppi->ColumnSize,
          ppi->DecimalDigits, ppi->BufferLength, ppi->StrLen_or_Ind);

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
        ppi->ValueType, ppi->ParameterType, ppi->ColumnSize,
        ppi->DecimalDigits, ppi->ParameterValuePtr, ppi->BufferLength, &ppi->StrLen_or_Ind);
    Py_END_ALLOW_THREADS;
    if (GetConnection(cur)->hdbc == SQL_NULL_HANDLE)
    {
        // The connection was closed by another thread in the ALLOW_THREADS block above.
        RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
        return false;
    }

    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cur->cnxn, "SQLBindParameter", GetConnection(cur)->hdbc, cur->hstmt);
        return false;
    }
    
    if (ppi->ValueType == SQL_C_NUMERIC)
    {
        SQLHDESC desc;
        SQLGetStmtAttr(cur->hstmt, SQL_ATTR_APP_PARAM_DESC, &desc, 0, 0);
        SQLSetDescField(desc, i + 1, SQL_DESC_TYPE, (SQLPOINTER)SQL_C_NUMERIC, 0);
        SQLSetDescField(desc, i + 1, SQL_DESC_PRECISION, (SQLPOINTER)ppi->ColumnSize, 0);
        SQLSetDescField(desc, i + 1, SQL_DESC_SCALE, (SQLPOINTER)(uintptr_t)ppi->DecimalDigits, 0);
        SQLSetDescField(desc, i + 1, SQL_DESC_DATA_PTR, ppi->ParameterValuePtr, 0);
    }

    return true;
}

static void FreeInfos(ParamInfo* a, Py_ssize_t count)
{
    for (Py_ssize_t i = 0; i < count; i++)
    {
        if (a[i].allocated)
            pyodbc_free(a[i].ParameterValuePtr);
        Py_XDECREF(a[i].pObject);
    }
    pyodbc_free(a);
}

void FreeParameterData(Cursor* cur)
{
    // Unbinds the parameters and frees the parameter buffer.

    if (cur->paramInfos)
    {
        // MS ODBC will crash if we use an HSTMT after the HDBC has been freed.
        if (cur->cnxn->hdbc != SQL_NULL_HANDLE)
        {
            Py_BEGIN_ALLOW_THREADS
            SQLFreeStmt(cur->hstmt, SQL_RESET_PARAMS);
            Py_END_ALLOW_THREADS
        }

        FreeInfos(cur->paramInfos, cur->paramcount);
        cur->paramInfos = 0;
    }
}

void FreeParameterInfo(Cursor* cur)
{
    // Internal function to free just the cached parameter information.  This is not used by the general cursor code
    // since this information is also freed in the less granular free_results function that clears everything.

    Py_XDECREF(cur->pPreparedSQL);
    pyodbc_free(cur->paramtypes);
    cur->pPreparedSQL = 0;
    cur->paramtypes   = 0;
    cur->paramcount   = 0;
}

bool Prepare(Cursor* cur, PyObject* pSql)
{
#if PY_MAJOR_VERSION >= 3
    if (!PyUnicode_Check(pSql))
    {
        PyErr_SetString(PyExc_TypeError, "SQL must be a Unicode string");
        return false;
    }
#endif

    //
    // Prepare the SQL if necessary.
    //
    if (pSql != cur->pPreparedSQL)
    {
        FreeParameterInfo(cur);

        SQLRETURN ret = 0;
        SQLSMALLINT cParamsT = 0;
        const char* szErrorFunc = "SQLPrepare";

        const TextEnc* penc;

#if PY_MAJOR_VERSION < 3
        if (PyBytes_Check(pSql))
        {
            penc = &cur->cnxn->str_enc;
        }
        else
#endif
        {
            penc = &cur->cnxn->unicode_enc;
        }

        Object query(penc->Encode(pSql));
        if (!query)
            return 0;

        bool isWide = (penc->ctype == SQL_C_WCHAR);

        const char* pch = PyBytes_AS_STRING(query.Get());
        SQLINTEGER  cch = (SQLINTEGER)(PyBytes_GET_SIZE(query.Get()) / (isWide ? sizeof(ODBCCHAR) : 1));

        TRACE("SQLPrepare(%s)\n", pch);

        Py_BEGIN_ALLOW_THREADS
        if (isWide)
            ret = SQLPrepareW(cur->hstmt, (SQLWCHAR*)pch, cch);
        else
            ret = SQLPrepare(cur->hstmt, (SQLCHAR*)pch, cch);
        if (SQL_SUCCEEDED(ret))
        {
            szErrorFunc = "SQLNumParams";
            ret = SQLNumParams(cur->hstmt, &cParamsT);
        }
        Py_END_ALLOW_THREADS

        if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
        {
            // The connection was closed by another thread in the ALLOW_THREADS block above.
            RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
            return false;
        }

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle(cur->cnxn, szErrorFunc, GetConnection(cur)->hdbc, cur->hstmt);
            return false;
        }

        cur->paramcount = (int)cParamsT;

        cur->pPreparedSQL = pSql;
        Py_INCREF(cur->pPreparedSQL);
    }
    return true;
}

static bool GetIntVal(PyObject *obj, SQLULEN *pOut)
{
    bool ret = false;
#if PY_MAJOR_VERSION < 3
    if (ret = PyInt_Check(obj))
    {
        *pOut = (SQLULEN)PyInt_AS_LONG(obj);
    }
    else
#endif
    if (ret = PyLong_Check(obj))
    {
        *pOut = (SQLULEN)PyLong_AsLong(obj);
    }
    Py_XDECREF(obj);
    return ret;
}

void SetParameterInfo(Cursor *cur, Py_ssize_t i, PyObject *value)
{
    // Sets the SQL type, length, and precision of the parameter according to and in
    // order of precedence:
    // setinputsizes()
    // SQLDescribeParam
    // C/Python type
    // hardcoded default
    ParamInfo& info = cur->paramInfos[i];
    SQLSMALLINT nullable;
    
    if (!cur->cnxn->supports_describeparam ||
        !SQL_SUCCEEDED(SQLDescribeParam(cur->hstmt, i + 1, &info.ParameterType,
        &info.ColumnSize, &info.DecimalDigits, &nullable)))
    {
        // Either no DescribeParam or DescribeParam failed, inspect C/Python type and
        // use that instead
        if (!DetectSQLType(cur, value, &info))
        {
            // Default to a medium-length varchar if all else fails
            info.ParameterType = SQL_VARCHAR;
            info.ColumnSize = 255;
            info.DecimalDigits = 0;
        }
    }
    
    // If inputsizes() is set, override SQL type and length using it
    if (cur->inputsizes && i < PySequence_Length(cur->inputsizes))
    {
        PyObject *desc = PySequence_GetItem(cur->inputsizes, i);
        if (desc)
        {
            // integer - sets colsize
            // type object - sets sqltype (not implemented yet; mapping between Python
            //               and SQL types  is not 1:1 so doesn't seem to offer much)
            // sequence of (colsize, sqltype, scale) ?
#if PY_MAJOR_VERSION < 3
            if (PyInt_Check(desc))
            {
                info.ColumnSize = (SQLULEN)PyInt_AS_LONG(desc);
            }
            else
#endif
            if (PyLong_Check(desc))
            {
                info.ColumnSize = (SQLULEN)PyLong_AsLong(desc);
            }
            else if(PySequence_Check(desc))
            {
                SQLULEN tmp;
                Py_ssize_t len = PySequence_Size(desc);
                if (len > 0 && GetIntVal(PySequence_ITEM(desc, 0), &tmp))
                {
                    info.ParameterType = (SQLSMALLINT)tmp;
                }
                if (len > 1 && GetIntVal(PySequence_ITEM(desc, 1), &tmp))
                {
                    info.ColumnSize = tmp;
                }
                
                if (len > 2 && GetIntVal(PySequence_ITEM(desc, 3), &tmp))
                {
                    info.DecimalDigits = (SQLSMALLINT)tmp;
                }
            }
            
        }
        Py_XDECREF(desc);
    }
    TRACE("SetParameterInfo %d -> %d %d %d\n", i, info.ParameterType, info.ColumnSize, info.DecimalDigits);
}

// Detects the SQL type, then sets the C type and buffer length
bool ParamSetup(Cursor *cur, PyObject *sql, PyObject* original_params, bool skip_first)
{
    if (!Prepare(cur, sql))
        return false;

    //
    // Normalize the parameter variables.
    //

    // Since we may replace parameters (we replace objects with Py_True/Py_False when writing to a bit/bool column),
    // allocate an array and use it instead of the original sequence

    int        params_offset = skip_first ? 1 : 0;
    Py_ssize_t cParams       = original_params == 0 ? 0 : PySequence_Length(original_params) - params_offset;
    
    if (cParams != cur->paramcount)
    {
        RaiseErrorV(0, ProgrammingError, "The SQL contains %d parameter markers, but %d parameters were supplied",
                    cur->paramcount, cParams);
        return false;
    }

    cur->paramInfos = (ParamInfo*)pyodbc_malloc(sizeof(ParamInfo) * cParams);
    if (cur->paramInfos == 0)
    {
        PyErr_NoMemory();
        return 0;
    }
    memset(cur->paramInfos, 0, sizeof(ParamInfo) * cParams);

    // Since you can't call SQLDescribeParam *after* calling SQLBindParameter, we'll loop through all of the
    // SetParameterInfos first, then bind.
    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        Object param(PySequence_GetItem(original_params, i + params_offset));
        SetParameterInfo(cur, i, param);
    }
    
    // Determine the C type to use, as well as the suggested length of the binding buffer
    for (Py_ssize_t i = 0; i < cParams;i++)
    {
        Object param(PySequence_GetItem(original_params, i + params_offset));
        if (!DetectCType(cur, param, &cur->paramInfos[i]))
        {
            FreeInfos(cur->paramInfos, cParams);
            cur->paramInfos = 0;
            return false;
        }
    }

    return true;
}

bool ExecuteMulti(Cursor* cur, PyObject* pSql, PyObject* paramArrayObj)
{
    bool ret = true;
    char *szLastFunction = 0;
    SQLRETURN rc = SQL_SUCCESS;

    PyObject *rowseq = PySequence_Fast(paramArrayObj, "Parameter array must be a sequence.");
    if (!rowseq)
    {
    ErrorRet1:
        if (cur->paramInfos)
            FreeInfos(cur->paramInfos, cur->paramcount);
        cur->paramInfos = 0;
        return false;
    }
    Py_ssize_t rowcount = PySequence_Fast_GET_SIZE(rowseq);
    PyObject **rowptr = PySequence_Fast_ITEMS(rowseq);

    // Describe each parameter (SQL type) in preparation for allocation of paramset array
    if (!ParamSetup(cur, pSql, *rowptr, false))
    {
        goto ErrorRet1;
    }

    Py_ssize_t r = 0;
    while ( r < rowcount )
    {
        // Scan current row to determine C types
        PyObject *currow = *rowptr++;
        if (!PyTuple_Check(currow) && !PyList_Check(currow) && !Row_Check(currow))
        {
            RaiseErrorV(0, PyExc_TypeError, "Params must be in a list, tuple, or Row");
        ErrorRet2:
            Py_XDECREF(rowseq);
            goto ErrorRet1;
        }
        PyObject *colseq = PySequence_Fast(currow, "Row must be a sequence.");
        if (!colseq)
        {
            goto ErrorRet2;
        }
        if (PySequence_Fast_GET_SIZE(colseq) != cur->paramcount)
        {
            RaiseErrorV(0, ProgrammingError, "Expected %u parameters, supplied %u", cur->paramcount, PySequence_Fast_GET_SIZE(colseq));
        ErrorRet3:
            Py_XDECREF(colseq);
            goto ErrorRet2;
        }
        PyObject **cells = PySequence_Fast_ITEMS(colseq);

        // Start at a non-zero offset to prevent null pointer detection.
        char *bindptr = (char*)16;
        Py_ssize_t i = 0;
        for (; i < cur->paramcount; i++)
        {
            if (!DetectCType(cur, cells[i], &cur->paramInfos[i]))
            {
                goto ErrorRet3;
            }

            TRACE("BIND: %d %d %d %d %d %p %d %p\n",
                i+1, cur->paramInfos[i].ValueType, cur->paramInfos[i].ParameterType, cur->paramInfos[i].ColumnSize,
                cur->paramInfos[i].DecimalDigits, bindptr, cur->paramInfos[i].BufferLength, (SQLLEN*)(bindptr + cur->paramInfos[i].BufferLength));
            if (!SQL_SUCCEEDED(SQLBindParameter(cur->hstmt, i + 1, SQL_PARAM_INPUT, cur->paramInfos[i].ValueType,
                cur->paramInfos[i].ParameterType, cur->paramInfos[i].ColumnSize, cur->paramInfos[i].DecimalDigits,
                bindptr, cur->paramInfos[i].BufferLength, (SQLLEN*)(bindptr + cur->paramInfos[i].BufferLength))))
            {
                RaiseErrorFromHandle(cur->cnxn, "SQLBindParameter", GetConnection(cur)->hdbc, cur->hstmt);
            ErrorRet4:
                SQLFreeStmt(cur->hstmt, SQL_RESET_PARAMS);
                goto ErrorRet3;
            }
            if (cur->paramInfos[i].ValueType == SQL_C_NUMERIC)
            {
                SQLHDESC desc;
                SQLGetStmtAttr(cur->hstmt, SQL_ATTR_APP_PARAM_DESC, &desc, 0, 0);
                SQLSetDescField(desc, i + 1, SQL_DESC_TYPE, (SQLPOINTER)SQL_C_NUMERIC, 0);
                SQLSetDescField(desc, i + 1, SQL_DESC_PRECISION, (SQLPOINTER)cur->paramInfos[i].ColumnSize, 0);
                SQLSetDescField(desc, i + 1, SQL_DESC_SCALE, (SQLPOINTER)(uintptr_t)cur->paramInfos[i].DecimalDigits, 0);
                SQLSetDescField(desc, i + 1, SQL_DESC_DATA_PTR, bindptr, 0);
            }
            bindptr += cur->paramInfos[i].BufferLength + sizeof(SQLLEN);
        }

        Py_ssize_t rowlen = bindptr - (char*)16;
        // Assume parameters are homogeneous between rows in the common case, to avoid
        // another rescan for determining the array height.
        // Subtract number of rows processed as an upper bound.
        if (!(cur->paramArray = (unsigned char*)pyodbc_malloc(rowlen * (rowcount - r))))
        {
            PyErr_NoMemory();
            goto ErrorRet4;
        }

        unsigned char *pParamDat = cur->paramArray;
        Py_ssize_t rows_converted = 0;
        
        ParamInfo *pi;
        for (;;)
        {
            // Column loop.
            pi = &cur->paramInfos[0];
            for (int c = 0; c < cur->paramcount; c++, pi++)
            {
                if (!PyToCType(cur, &pParamDat, *cells++, pi))
                {
                    // "schema change" or conversion error. Try again on next batch.
                    rowptr--;
                    Py_XDECREF(colseq);
                    // Finish this batch of rows and attempt to execute before starting another.
                    goto DoExecute;
                }
            }
            rows_converted++;
            Py_XDECREF(colseq);
            r++;
            if ( r >= rowcount )
            {
                break;
            }
            currow = *rowptr++;
            colseq = PySequence_Fast(currow, "Row must be a sequence.");
            if (!colseq)
            {
            ErrorRet5:
                pyodbc_free(cur->paramArray);
                cur->paramArray = 0;
                goto ErrorRet4;
            }
            if (PySequence_Fast_GET_SIZE(colseq) != cur->paramcount)
            {
                RaiseErrorV(0, ProgrammingError, "Expected %u parameters, supplied %u", cur->paramcount, PySequence_Fast_GET_SIZE(colseq));
                Py_XDECREF(colseq);
                goto ErrorRet5;
            }
            cells = PySequence_Fast_ITEMS(colseq);
        }
    DoExecute:
        if (!rows_converted)
        {
            if (!PyErr_Occurred())
                RaiseErrorV(0, ProgrammingError, "No suitable conversion for one or more parameters.");
            goto ErrorRet5;
        }

        SQLULEN bop = (SQLULEN)(cur->paramArray) - 16;
        if (!SQL_SUCCEEDED(SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_TYPE, (SQLPOINTER)rowlen, SQL_IS_UINTEGER)))
        {
            RaiseErrorFromHandle(cur->cnxn, "SQLSetStmtAttr", GetConnection(cur)->hdbc, cur->hstmt);
        ErrorRet6:
            SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_TYPE, SQL_BIND_BY_COLUMN, SQL_IS_UINTEGER);
            goto ErrorRet5;
        }
        if (!SQL_SUCCEEDED(SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)rows_converted, SQL_IS_UINTEGER)))
        {
            RaiseErrorFromHandle(cur->cnxn, "SQLSetStmtAttr", GetConnection(cur)->hdbc, cur->hstmt);
            goto ErrorRet6;
        }
        if (!SQL_SUCCEEDED(SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, (SQLPOINTER)&bop, SQL_IS_POINTER)))
        {
            RaiseErrorFromHandle(cur->cnxn, "SQLSetStmtAttr", GetConnection(cur)->hdbc, cur->hstmt);
        ErrorRet7:
            SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)1, SQL_IS_UINTEGER);
            goto ErrorRet6;
        }

        // The code below was copy-pasted from cursor.cpp's execute() for convenience.
        // TODO: REFACTOR if there is possibility to reuse (maybe not, because DAE structure is different)
        Py_BEGIN_ALLOW_THREADS
        rc = SQLExecute(cur->hstmt);
        Py_END_ALLOW_THREADS

        if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
        {
            // The connection was closed by another thread in the ALLOW_THREADS block above.
            RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
        ErrorRet8:
            FreeParameterData(cur);
            SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, 0, SQL_IS_POINTER);
            goto ErrorRet7;
        }

        if (!SQL_SUCCEEDED(rc) && rc != SQL_NEED_DATA && rc != SQL_NO_DATA)
        {
            // We could try dropping through the while and if below, but if there is an error, we need to raise it before
            // FreeParameterData calls more ODBC functions.
            RaiseErrorFromHandle(cur->cnxn, "SQLExecute", cur->cnxn->hdbc, cur->hstmt);
            goto ErrorRet8;
        }

        if (!ProcessDAEParams(rc, cur, true))
            return false;

        if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA)
            return RaiseErrorFromHandle(cur->cnxn, szLastFunction, cur->cnxn->hdbc, cur->hstmt);
        
        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)1, SQL_IS_UINTEGER);
        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, 0, SQL_IS_POINTER);
        pyodbc_free(cur->paramArray);
        cur->paramArray = 0;
    }

    Py_XDECREF(rowseq);
    FreeParameterData(cur);
    return ret;
}

struct NullParam
{
    PyObject_HEAD
};


PyTypeObject NullParamType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyodbc.NullParam",         // tp_name
    sizeof(NullParam),          // tp_basicsize
    0,                          // tp_itemsize
    0,                          // destructor tp_dealloc
    0,                          // tp_print
    0,                          // tp_getattr
    0,                          // tp_setattr
    0,                          // tp_compare
    0,                          // tp_repr
    0,                          // tp_as_number
    0,                          // tp_as_sequence
    0,                          // tp_as_mapping
    0,                          // tp_hash
    0,                          // tp_call
    0,                          // tp_str
    0,                          // tp_getattro
    0,                          // tp_setattro
    0,                          // tp_as_buffer
    Py_TPFLAGS_DEFAULT,         // tp_flags
};

PyObject* null_binary;

bool Params_init()
{
    if (PyType_Ready(&NullParamType) < 0)
        return false;

    null_binary = (PyObject*)PyObject_New(NullParam, &NullParamType);
    if (null_binary == 0)
        return false;

    PyDateTime_IMPORT;

    return true;
}
