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
#include "row.h"
#include <datetime.h>

inline Connection* GetConnection(Cursor* cursor)
{
    return (Connection*)cursor->cnxn;
}

struct DAEParam
{
    PyObject *cell;
    SQLLEN maxlen;
};

// Detects and sets the appropriate C type to use for binding the specified Python object.
// Also sets the buffer length to use.
// Returns false if unsuccessful.
static int DetectCType(PyObject *cell, ParamInfo *pi)
{
    PyObject* cls = 0;
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
        pi->ValueType = sizeof(long) == 8 ? SQL_C_SBIGINT : SQL_C_LONG;
        pi->BufferLength = sizeof(long);
    }
#endif
    else if (PyLong_Check(cell))
    {
    Type_Long:
        if (pi->ParameterType == SQL_NUMERIC ||
            pi->ParameterType == SQL_DECIMAL)
        {
            pi->ValueType = SQL_C_NUMERIC;
            pi->BufferLength = sizeof(SQL_NUMERIC_STRUCT);
        }
        else
        {
            pi->ValueType = SQL_C_SBIGINT;
            pi->BufferLength = sizeof(long long);
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
        // If it is a max-type (ColumnSize == 0), use DAE.
#if PY_MAJOR_VERSION < 3
        pi->ValueType = SQL_C_CHAR;
#else
        pi->ValueType = SQL_C_BINARY;
#endif
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize : sizeof(DAEParam);
    }
    else if (PyUnicode_Check(cell))
    {
    Type_Unicode:
        // Assume the SQL type is also wide character.
        // If it is a max-type (ColumnSize == 0), use DAE.
        pi->ValueType = SQL_C_WCHAR;
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize * sizeof(SQLWCHAR) : sizeof(DAEParam);
    }
    else if (PyDateTime_Check(cell))
    {
    Type_DateTime:
        pi->ValueType = SQL_C_TYPE_TIMESTAMP;
        pi->BufferLength = sizeof(SQL_TIMESTAMP_STRUCT);
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
    else if (IsInstanceForThread(cell, "uuid", "UUID", &cls) && cls)
    {
    Type_UUID:
        // UUID
        pi->ValueType = SQL_C_GUID;
        pi->BufferLength = 16;
    }
    else if (IsInstanceForThread(cell, "decimal", "Decimal", &cls) && cls)
    {
    Type_Decimal:
        pi->ValueType = SQL_C_NUMERIC;
        pi->BufferLength = sizeof(SQL_NUMERIC_STRUCT);
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
// Returns false if object could not be converted.
static int PyToCType(Cursor *cur, unsigned char **outbuf, PyObject *cell, ParamInfo *pi)
{
    PyObject *cls = 0;
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
        if (pi->ValueType == SQL_C_SBIGINT)
        {
            WRITEOUT(long long, outbuf, PyLong_AsLongLong(cell), ind);
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
            pNum->precision = (SQLCHAR)pi->ColumnSize;
            pNum->scale = (SQLCHAR)pi->DecimalDigits;
            pNum->sign = _PyLong_Sign(cell) >= 0;
            if (_PyLong_AsByteArray((PyLongObject*)absVal, pNum->val, sizeof(pNum->val), 1, 0))
                goto NumericOverflow;
            Py_XDECREF(absVal);
            *outbuf += pi->BufferLength;
            ind = sizeof(SQL_NUMERIC_STRUCT);
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
        Py_ssize_t len = PyBytes_GET_SIZE(cell);
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
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                len = pi->BufferLength;
            }
            memcpy(*outbuf, PyBytes_AS_STRING(cell), len);
            *outbuf += pi->BufferLength;
            ind = len;
        }
    }
    else if (PyUnicode_Check(cell))
    {
        if (pi->ValueType != SQL_C_WCHAR)
            return false;

        Py_ssize_t len = PyUnicode_GET_SIZE(cell);
        //         Same size      Different size
        // DAE     DAE only       Convert + DAE
        // non-DAE Copy           Convert + Copy
        if (sizeof(Py_UNICODE) != sizeof(SQLWCHAR))
        {
            const TextEnc& enc = cur->cnxn->unicode_enc;
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
                    RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                    len = pi->BufferLength;
                }
                memcpy(*outbuf, PyBytes_AS_STRING((PyObject*)encoded), len);
                *outbuf += pi->BufferLength;
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
                    RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                    len = pi->BufferLength;
                }
                memcpy(*outbuf, PyUnicode_AS_DATA(cell), len);
                *outbuf += pi->BufferLength;
                ind = len;
            }
        }
    }
    else if (PyDateTime_Check(cell))
    {
        if (pi->ValueType != SQL_C_TYPE_TIMESTAMP)
            return false;
        SQL_TIMESTAMP_STRUCT *pts = (SQL_TIMESTAMP_STRUCT*)*outbuf;
        pts->year = PyDateTime_GET_YEAR(cell);
        pts->month = PyDateTime_GET_MONTH(cell);
        pts->day = PyDateTime_GET_DAY(cell);
        pts->hour = PyDateTime_DATE_GET_HOUR(cell);
        pts->minute = PyDateTime_DATE_GET_MINUTE(cell);
        pts->second = PyDateTime_DATE_GET_SECOND(cell);
        pts->fraction = PyDateTime_DATE_GET_MICROSECOND(cell) * 1000;
        *outbuf += sizeof(SQL_TIMESTAMP_STRUCT);
        ind = sizeof(SQL_TIMESTAMP_STRUCT);
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
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %u buffer %u", len, pi->BufferLength);
                len = pi->BufferLength;
            }
            memcpy(*outbuf, PyByteArray_AS_STRING(cell), len);
            *outbuf += pi->BufferLength;
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
        if (len < 0)
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
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: row %u column %u", 0 /* TODO */, 0 /* TODO */);
                len = pi->BufferLength;
            }
            memcpy(*outbuf, pb, len);
            *outbuf += pi->BufferLength;
            ind = len;
        }
    }
#endif
    else if (IsInstanceForThread(cell, "uuid", "UUID", &cls) && cls)
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
    else if (IsInstanceForThread(cell, "decimal", "Decimal", &cls) && cls)
    {
        if (pi->ValueType != SQL_C_NUMERIC)
            return false;
        // Normalise, then get sign, exponent, and digits.
        PyObject *normCell = PyObject_CallMethod(cell, "normalize", 0);
        if (!normCell)
            return false;
        PyObject *cellParts = PyObject_CallMethod(normCell, "as_tuple", 0);
        if (!cellParts)
            return false;

        Py_XDECREF(normCell);

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

        pNum->precision = (SQLCHAR)pi->ColumnSize;
        pNum->scale = (SQLCHAR)pi->DecimalDigits;

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
    else if (cell == Py_None)
    {
        *outbuf += pi->BufferLength;
        ind = SQL_NULL_DATA;
    }
    else
    {
        RaiseErrorV(0, ProgrammingError, "Unknown object type: %s",cell->ob_type->tp_name);
        return false;
    }
    *(SQLLEN*)(*outbuf) = ind;
    *outbuf += sizeof(SQLLEN);
    return true;
}

static bool GetParamType(Cursor* cur, Py_ssize_t iParam, SQLSMALLINT& type);

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

static bool GetNullInfo(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    if (!GetParamType(cur, index, info.ParameterType))
        return false;

    info.ValueType     = SQL_C_DEFAULT;
    info.ColumnSize    = 1;
    info.StrLen_or_Ind = SQL_NULL_DATA;
    return true;
}

static bool GetNullBinaryInfo(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    info.ValueType         = SQL_C_BINARY;
    info.ParameterType     = SQL_BINARY;
    info.ColumnSize        = 1;
    info.ParameterValuePtr = 0;
    info.StrLen_or_Ind     = SQL_NULL_DATA;
    return true;
}


#if PY_MAJOR_VERSION >= 3
static bool GetBytesInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // The Python 3 version that writes bytes as binary data.
    Py_ssize_t cb = PyBytes_GET_SIZE(param);

    info.ValueType  = SQL_C_BINARY;
    info.ColumnSize = (SQLUINTEGER)max(cb, 1);

    SQLLEN maxlength = cur->cnxn->GetMaxLength(info.ValueType);

    if (maxlength == 0 || cb <= maxlength)
    {
        info.ParameterType     = SQL_VARBINARY;
        info.StrLen_or_Ind     = cb;
        info.BufferLength      = (SQLLEN)info.ColumnSize;
        info.ParameterValuePtr = PyBytes_AS_STRING(param);
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = SQL_LONGVARBINARY;
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)cb) : SQL_DATA_AT_EXEC;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.pObject           = param;
        Py_INCREF(info.pObject);
        info.maxlength = maxlength;
    }

    return true;
}
#endif

#if PY_MAJOR_VERSION < 3
static bool GetStrInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    const TextEnc& enc = cur->cnxn->str_enc;

    info.ValueType = enc.ctype;

    Py_ssize_t cch = PyString_GET_SIZE(param);

    info.ColumnSize = (SQLUINTEGER)max(cch, 1);

    Object encoded;

    if (enc.optenc == OPTENC_RAW)
    {
        // Take the text as-is.  This is not really a good idea since users will need to make
        // sure the encoding over the wire matches their system encoding, but it will be wanted
        // and it is fast when you get it to work.
        encoded = param;
    }
    else
    {
        // Encode the text with the user's encoding.
        encoded = PyCodec_Encode(param, enc.name, "strict");
        if (!encoded)
            return false;

        if (!PyBytes_CheckExact(encoded))
        {
            // Not all encodings return bytes.
            PyErr_Format(PyExc_TypeError, "Unicode read encoding '%s' returned unexpected data type: %s",
                         enc.name, encoded.Get()->ob_type->tp_name);
            return false;
        }
    }

    Py_ssize_t cb = PyBytes_GET_SIZE(encoded);
    info.pObject = encoded.Detach();

    SQLLEN maxlength = cur->cnxn->GetMaxLength(info.ValueType);
    if (maxlength == 0 || cb <= maxlength)
    {
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_VARCHAR : SQL_WVARCHAR;
        info.ParameterValuePtr = PyBytes_AS_STRING(info.pObject);
        info.StrLen_or_Ind     = (SQLINTEGER)cb;
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_LONGVARCHAR : SQL_WLONGVARCHAR;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLINTEGER)cb) : SQL_DATA_AT_EXEC;
        info.maxlength = maxlength;
    }

    return true;
}
#endif


static bool GetUnicodeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    const TextEnc& enc = cur->cnxn->unicode_enc;

    info.ValueType = enc.ctype;

    Object encoded(PyCodec_Encode(param, enc.name, "strict"));
    if (!encoded)
        return false;

    if (enc.optenc == OPTENC_NONE && !PyBytes_CheckExact(encoded))
    {
        PyErr_Format(PyExc_TypeError, "Unicode write encoding '%s' returned unexpected data type: %s",
                     enc.name, encoded.Get()->ob_type->tp_name);
        return false;
    }

    Py_ssize_t cb = PyBytes_GET_SIZE(encoded);
    
    int denom = 1;
    
    if(enc.optenc == OPTENC_UTF16)
    {
        denom = 2;
    }
    else if(enc.optenc == OPTENC_UTF32)
    {
        denom = 4;
    }
    
    info.ColumnSize = (SQLUINTEGER)max(cb / denom, 1);
    
    info.pObject = encoded.Detach();

    SQLLEN maxlength = cur->cnxn->GetMaxLength(enc.ctype);

    if (maxlength == 0 || cb <= maxlength)
    {
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_VARCHAR : SQL_WVARCHAR;
        info.ParameterValuePtr = PyBytes_AS_STRING(info.pObject);
        info.BufferLength      = (SQLINTEGER)cb;
        info.StrLen_or_Ind     = (SQLINTEGER)cb;
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_LONGVARCHAR : SQL_WLONGVARCHAR;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLINTEGER)cb) : SQL_DATA_AT_EXEC;
        info.maxlength = maxlength;
    }

    return true;
}

static bool GetBooleanInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.ValueType         = SQL_C_BIT;
    info.ParameterType     = SQL_BIT;
    info.StrLen_or_Ind     = 1;
    info.Data.ch           = (unsigned char)(param == Py_True ? 1 : 0);
    info.ParameterValuePtr = &info.Data.ch;
    return true;
}

static bool GetDateTimeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.Data.timestamp.year   = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
    info.Data.timestamp.month  = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
    info.Data.timestamp.day    = (SQLUSMALLINT)PyDateTime_GET_DAY(param);
    info.Data.timestamp.hour   = (SQLUSMALLINT)PyDateTime_DATE_GET_HOUR(param);
    info.Data.timestamp.minute = (SQLUSMALLINT)PyDateTime_DATE_GET_MINUTE(param);
    info.Data.timestamp.second = (SQLUSMALLINT)PyDateTime_DATE_GET_SECOND(param);

    // SQL Server chokes if the fraction has more data than the database supports.  We expect other databases to be the
    // same, so we reduce the value to what the database supports.  http://support.microsoft.com/kb/263872

    int precision = ((Connection*)cur->cnxn)->datetime_precision - 20; // (20 includes a separating period)
    if (precision <= 0)
    {
        info.Data.timestamp.fraction = 0;
    }
    else
    {
        info.Data.timestamp.fraction = (SQLUINTEGER)(PyDateTime_DATE_GET_MICROSECOND(param) * 1000); // 1000 == micro -> nano

        // (How many leading digits do we want to keep?  With SQL Server 2005, this should be 3: 123000000)
        int keep = (int)pow(10.0, 9-min(9, precision));
        info.Data.timestamp.fraction = info.Data.timestamp.fraction / keep * keep;
        info.DecimalDigits = (SQLSMALLINT)precision;
    }

    info.ValueType         = SQL_C_TIMESTAMP;
    info.ParameterType     = SQL_TIMESTAMP;
    info.ColumnSize        = (SQLUINTEGER)((Connection*)cur->cnxn)->datetime_precision;
    info.StrLen_or_Ind     = sizeof(TIMESTAMP_STRUCT);
    info.ParameterValuePtr = &info.Data.timestamp;
    return true;
}

static bool GetDateInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.Data.date.year  = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
    info.Data.date.month = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
    info.Data.date.day   = (SQLUSMALLINT)PyDateTime_GET_DAY(param);

    info.ValueType         = SQL_C_TYPE_DATE;
    info.ParameterType     = SQL_TYPE_DATE;
    info.ColumnSize        = 10;
    info.ParameterValuePtr = &info.Data.date;
    info.StrLen_or_Ind     = sizeof(DATE_STRUCT);
    return true;
}

static bool GetTimeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.Data.time.hour   = (SQLUSMALLINT)PyDateTime_TIME_GET_HOUR(param);
    info.Data.time.minute = (SQLUSMALLINT)PyDateTime_TIME_GET_MINUTE(param);
    info.Data.time.second = (SQLUSMALLINT)PyDateTime_TIME_GET_SECOND(param);

    info.ValueType         = SQL_C_TYPE_TIME;
    info.ParameterType     = SQL_TYPE_TIME;
    info.ColumnSize        = 8;
    info.ParameterValuePtr = &info.Data.time;
    info.StrLen_or_Ind     = sizeof(TIME_STRUCT);
    return true;
}

#if PY_MAJOR_VERSION < 3
static bool GetIntInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.Data.l = PyInt_AsLong(param);

#if LONG_BIT == 64
    info.ValueType     = SQL_C_SBIGINT;
    // info.ValueType     = SQL_C_LONG;
    info.ParameterType = SQL_BIGINT;
#elif LONG_BIT == 32
    info.ValueType     = SQL_C_LONG;
    info.ParameterType = SQL_INTEGER;
#else
    #error Unexpected LONG_BIT value
#endif

    info.ParameterValuePtr = &info.Data.l;
    return true;
}
#endif

static bool GetLongInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // Try to use integer when possible.  BIGINT is not always supported and is a "special
    // case" for some drivers.

    // REVIEW: C & C++ now have constants for max sizes, but I'm not sure if they will be
    // available on all platforms Python supports right now.  It would be more performant when
    // a lot of 64-bit values are used since we could avoid the AsLong call.

    // It's not clear what the maximum SQL_INTEGER should be.  The Microsoft documentation says
    // it is a 'long int', but some drivers run into trouble at high values.  We'll use
    // SQL_INTEGER as an optimization for smaller values and rely on BIGINT

    info.Data.l = PyLong_AsLong(param);
    if (!PyErr_Occurred() && (info.Data.l <= 0x7FFFFFFF))
    {
        info.ValueType         = SQL_C_LONG;
        info.ParameterType     = SQL_INTEGER;
        info.ParameterValuePtr = &info.Data.l;
    }
    else
    {
        PyErr_Clear();
        info.Data.i64 = (INT64)PyLong_AsLongLong(param);
        if (!PyErr_Occurred())
        {
            info.ValueType         = SQL_C_SBIGINT;
            info.ParameterType     = SQL_BIGINT;
            info.ParameterValuePtr = &info.Data.i64;
        }
    }

    return !PyErr_Occurred();
}

static bool GetFloatInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // TODO: Overflow?
    info.Data.dbl = PyFloat_AsDouble(param);

    info.ValueType         = SQL_C_DOUBLE;
    info.ParameterType     = SQL_DOUBLE;
    info.ParameterValuePtr = &info.Data.dbl;
    info.ColumnSize = 15;
    return true;
}

static char* CreateDecimalString(long sign, PyObject* digits, long exp)
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

    return pch;
}

static bool GetUUIDInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, PyObject* uuid_type)
{
    // uuid_type: This is a new reference that we are responsible for freeing.
    Object tmp(uuid_type);

    info.ValueType = SQL_C_GUID;
    info.ParameterType = SQL_GUID;
    info.ColumnSize = 16;

    info.allocated = true;
    info.ParameterValuePtr = pyodbc_malloc(sizeof(SQLGUID));
    if (!info.ParameterValuePtr)
    {
        PyErr_NoMemory();
        return false;
    }

    // Do we need to use "bytes" on a big endian machine?
    Object b(PyObject_GetAttrString(param, "bytes_le"));
    if (!b)
        return false;
    memcpy(info.ParameterValuePtr, PyBytes_AS_STRING(b.Get()), sizeof(SQLGUID));
    return true;
}


static bool GetDecimalInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, PyObject* decimal_type)
{
    // decimal_type: This is a new reference that we are responsible for freeing.
    Object tmp(decimal_type);

    // The NUMERIC structure never works right with SQL Server and probably a lot of other drivers.  We'll bind as a
    // string.  Unfortunately, the Decimal class doesn't seem to have a way to force it to return a string without
    // exponents, so we'll have to build it ourselves.

    Object t(PyObject_CallMethod(param, "as_tuple", 0));
    if (!t)
        return false;

    long       sign   = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 0));
    PyObject*  digits = PyTuple_GET_ITEM(t.Get(), 1);
    long       exp    = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 2));

    Py_ssize_t count = PyTuple_GET_SIZE(digits);

    info.ValueType     = SQL_C_CHAR;
    info.ParameterType = SQL_NUMERIC;

    if (exp >= 0)
    {
        // (1 2 3) exp = 2 --> '12300'

        info.ColumnSize    = (SQLUINTEGER)count + exp;
        info.DecimalDigits = 0;

    }
    else if (-exp <= count)
    {
        // (1 2 3) exp = -2 --> 1.23 : prec = 3, scale = 2
        info.ColumnSize    = (SQLUINTEGER)count;
        info.DecimalDigits = (SQLSMALLINT)-exp;
    }
    else
    {
        // (1 2 3) exp = -5 --> 0.00123 : prec = 5, scale = 5
        info.ColumnSize    = (SQLUINTEGER)(count + (-exp));
        info.DecimalDigits = (SQLSMALLINT)info.ColumnSize;
    }

    I(info.ColumnSize >= (SQLULEN)info.DecimalDigits);

    info.ParameterValuePtr = CreateDecimalString(sign, digits, exp);
    if (!info.ParameterValuePtr)
    {
        PyErr_NoMemory();
        return false;
    }
    info.allocated = true;

    info.StrLen_or_Ind = (SQLINTEGER)strlen((char*)info.ParameterValuePtr);

    return true;
}

#if PY_MAJOR_VERSION < 3
static bool GetBufferInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.ValueType = SQL_C_BINARY;

    const char* pb;
    Py_ssize_t  cb = PyBuffer_GetMemory(param, &pb);

    SQLLEN maxlength = cur->cnxn->GetMaxLength(info.ValueType);
    if (maxlength == 0 || cb <= maxlength)
    {
        // There is one segment, so we can bind directly into the buffer object.

        info.ParameterType     = SQL_VARBINARY;
        info.ParameterValuePtr = (SQLPOINTER)pb;
        info.BufferLength      = (SQLINTEGER)cb;
        info.ColumnSize        = (SQLUINTEGER)max(cb, 1);
        info.StrLen_or_Ind     = (SQLINTEGER)cb;
    }
    else
    {
        // There are multiple segments, so we'll provide the data at execution time.  Pass the PyObject pointer as
        // the parameter value which will be pased back to us when the data is needed.  (If we release threads, we
        // need to up the refcount!)

        info.ParameterType     = SQL_LONGVARBINARY;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.ColumnSize        = (SQLUINTEGER)PyBuffer_Size(param);
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)PyBuffer_Size(param)) : SQL_DATA_AT_EXEC;
        info.pObject = param;
        Py_INCREF(info.pObject);
        info.maxlength = maxlength;
    }

    return true;
}
#endif

#if PY_VERSION_HEX >= 0x02060000
static bool GetByteArrayInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.ValueType = SQL_C_BINARY;

    Py_ssize_t cb = PyByteArray_Size(param);

    SQLLEN maxlength = cur->cnxn->GetMaxLength(info.ValueType);
    if (maxlength == 0 || cb <= maxlength)
    {
        info.ParameterType     = SQL_VARBINARY;
        info.ParameterValuePtr = (SQLPOINTER)PyByteArray_AsString(param);
        info.BufferLength      = (SQLINTEGER)cb;
        info.ColumnSize        = (SQLUINTEGER)max(cb, 1);
        info.StrLen_or_Ind     = (SQLINTEGER)cb;
    }
    else
    {
        info.ParameterType     = SQL_LONGVARBINARY;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.ColumnSize        = (SQLUINTEGER)cb;
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)cb) : SQL_DATA_AT_EXEC;
        info.pObject = param;
        Py_INCREF(info.pObject);
        info.maxlength = maxlength;
    }
    return true;
}
#endif

static bool GetParameterInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // Determines the type of SQL parameter that will be used for this parameter based on the Python data type.
    //
    // Populates `info`.

    if (param == Py_None)
        return GetNullInfo(cur, index, info);

    if (param == null_binary)
        return GetNullBinaryInfo(cur, index, info);

#if PY_MAJOR_VERSION >= 3
    if (PyBytes_Check(param))
        return GetBytesInfo(cur, index, param, info);
#else
    if (PyBytes_Check(param))
        return GetStrInfo(cur, index, param, info);
#endif

    if (PyUnicode_Check(param))
        return GetUnicodeInfo(cur, index, param, info);

    if (PyBool_Check(param))
        return GetBooleanInfo(cur, index, param, info);

    if (PyDateTime_Check(param))
        return GetDateTimeInfo(cur, index, param, info);

    if (PyDate_Check(param))
        return GetDateInfo(cur, index, param, info);

    if (PyTime_Check(param))
        return GetTimeInfo(cur, index, param, info);

    if (PyLong_Check(param))
        return GetLongInfo(cur, index, param, info);

    if (PyFloat_Check(param))
        return GetFloatInfo(cur, index, param, info);

#if PY_VERSION_HEX >= 0x02060000
    if (PyByteArray_Check(param))
        return GetByteArrayInfo(cur, index, param, info);
#endif

#if PY_MAJOR_VERSION < 3
    if (PyInt_Check(param))
        return GetIntInfo(cur, index, param, info);

    if (PyBuffer_Check(param))
        return GetBufferInfo(cur, index, param, info);
#endif

    // Decimal

    PyObject* cls = 0;
    if (!IsInstanceForThread(param, "decimal", "Decimal", &cls))
        return false;

    if (cls != 0)
        return GetDecimalInfo(cur, index, param, info, cls);

    // UUID

    if (!IsInstanceForThread(param, "uuid", "UUID", &cls))
        return false;

    if (cls != 0)
        return GetUUIDInfo(cur, index, param, info, cls);

    RaiseErrorV("HY105", ProgrammingError, "Invalid parameter type.  param-index=%zd param-type=%s", index, Py_TYPE(param)->tp_name);
    return false;
}

static bool getObjectValue(PyObject *pObject, long& nValue)
{
	if (pObject == NULL)
		return false;

#if PY_MAJOR_VERSION < 3
	if (PyInt_Check(pObject))
	{
		nValue = PyInt_AS_LONG(pObject);
		return true;
	}

#endif
	if (PyLong_Check(pObject))
	{
		nValue = PyLong_AsLong(pObject);
		return true;
	}

	return false;
}

static long getSequenceValue(PyObject *pSequence, Py_ssize_t nIndex, long nDefault, bool &bChanged)
{
	PyObject *obj;
	long v = nDefault;

	obj = PySequence_GetItem(pSequence, nIndex);
	if (obj != NULL)
	{
		if (getObjectValue(obj, v))
			bChanged = true;
	}
	Py_CLEAR(obj);

	return v;
}

/**
 * UpdateParamInfo updates the current columnsizes with the information provided
 * by a set from the client code, to manually override values returned by SQLDescribeParam()
 * which can be wrong in case of SQL Server statements.
 *
 * sparhawk@gmx.at (Gerhard Gruber)
 */
static bool UpdateParamInfo(Cursor* pCursor, Py_ssize_t nIndex, ParamInfo *pInfo)
{
	if (pCursor->inputsizes == NULL || nIndex >= PySequence_Length(pCursor->inputsizes))
		return false;

	PyObject *desc = PySequence_GetItem(pCursor->inputsizes, nIndex);
	if (desc == NULL)
		return false;

	bool rc = false;
	long v;
	bool clearError = true;

	// If the error was already set before we entered here, it is not from us, so we leave it alone.
	if (PyErr_Occurred())
		clearError = false;

	// integer - sets colsize
	// type object - sets sqltype (mapping between Python and SQL types is not 1:1 so it may not always work)
	// Consider: sequence of (colsize, sqltype, scale)
	if (getObjectValue(desc, v))
	{
		pInfo->ColumnSize = (SQLULEN)v;
		rc = true;
	}
	else if (PySequence_Check(desc))
	{
		pInfo->ParameterType = (SQLSMALLINT)getSequenceValue(desc, 0, (long)pInfo->ParameterType, rc);
		pInfo->ColumnSize = (SQLUINTEGER)getSequenceValue(desc, 1, (long)pInfo->ColumnSize, rc);
		pInfo->DecimalDigits = (SQLSMALLINT)getSequenceValue(desc, 2, (long)pInfo->ColumnSize, rc);
	}

	Py_CLEAR(desc);

	// If the user didn't provide the full array (in case he gave us an array), the above code would
	// set an internal error on the cursor object, as we try to read three values from an array
	// which may not have as many. This is ok, because we don't really care if the array is not completly
	// specified, so we clear the error in case it comes from this. If the error was already present before that
	// we keep it, so the user can handle it.
	if (clearError)
		PyErr_Clear();

	return rc;
}

bool BindParameter(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    SQLSMALLINT sqltype = info.ParameterType;
    SQLULEN colsize = info.ColumnSize;
    SQLSMALLINT scale = info.DecimalDigits;

    if (UpdateParamInfo(cur, index, &info))
    {
		// Reload in case it has changed.
		colsize = info.ColumnSize;
		sqltype = info.ParameterType;
		scale = info.DecimalDigits;
    }

	TRACE("BIND: param=%ld ValueType=%d (%s) ParameterType=%d (%s) ColumnSize=%ld DecimalDigits=%d BufferLength=%ld *pcb=%ld\n",
          (index+1), info.ValueType, CTypeName(info.ValueType), sqltype, SqlTypeName(sqltype), colsize,
          scale, info.BufferLength, info.StrLen_or_Ind);

    SQLRETURN ret = -1;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(index + 1), SQL_PARAM_INPUT,
        info.ValueType, sqltype, colsize, scale, info.ParameterValuePtr, info.BufferLength, &info.StrLen_or_Ind);
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

    return true;
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

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* original_params, bool skip_first)
{
    //
    // Normalize the parameter variables.
    //

    // Since we may replace parameters (we replace objects with Py_True/Py_False when writing to a bit/bool column),
    // allocate an array and use it instead of the original sequence

    int        params_offset = skip_first ? 1 : 0;
    Py_ssize_t cParams       = original_params == 0 ? 0 : PySequence_Length(original_params) - params_offset;

    if (!Prepare(cur, pSql))
        return false;

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

    // Since you can't call SQLDesribeParam *after* calling SQLBindParameter, we'll loop through all of the
    // GetParameterInfos first, then bind.

    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        Object param(PySequence_GetItem(original_params, i + params_offset));
        if (!GetParameterInfo(cur, i, param, cur->paramInfos[i]))
        {
            FreeInfos(cur->paramInfos, cParams);
            cur->paramInfos = 0;
            return false;
        }
    }

    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        if (!BindParameter(cur, i, cur->paramInfos[i]))
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
    if (!Prepare(cur, pSql))
        return false;

    if (!(cur->paramInfos = (ParamInfo*)pyodbc_malloc(sizeof(ParamInfo) * cur->paramcount)))
    {
        PyErr_NoMemory();
        return 0;
    }
    memset(cur->paramInfos, 0, sizeof(ParamInfo) * cur->paramcount);

	// Describe each parameter (SQL type) in preparation for allocation of paramset array
    for (Py_ssize_t i = 0; i < cur->paramcount; i++)
    {
		SQLSMALLINT nullable;
        if(!SQL_SUCCEEDED(SQLDescribeParam(cur->hstmt, i + 1, &(cur->paramInfos[i].ParameterType),
            &cur->paramInfos[i].ColumnSize, &cur->paramInfos[i].DecimalDigits,
            &nullable)))
        {
            // Default to a medium-length varchar if describing the parameter didn't work
            cur->paramInfos[i].ParameterType = SQL_VARCHAR;
            cur->paramInfos[i].ColumnSize = 255;
            cur->paramInfos[i].DecimalDigits = 0;
        }

		// This supports overriding of input sizes via setinputsizes
		// See issue 380
		// The logic is duplicated from BindParameter
		UpdateParamInfo(cur, i, &cur->paramInfos[i]);
	}

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
            if (!DetectCType(cells[i], &cur->paramInfos[i]))
            {
                goto ErrorRet3;
            }

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

        // TODO: Refactor into ProcessDAEParams() ?
        while (rc == SQL_NEED_DATA)
        {
            // One or more parameters were too long to bind normally so we set the
            // length to SQL_LEN_DATA_AT_EXEC.  ODBC will return SQL_NEED_DATA for
            // each of the parameters we did this for.
            //
            // For each one we set a pointer to the ParamInfo as the "parameter
            // data" we can access with SQLParamData.  We've stashed everything we
            // need in there.

            szLastFunction = "SQLParamData";
            DAEParam *pInfo;
            Py_BEGIN_ALLOW_THREADS
            rc = SQLParamData(cur->hstmt, (SQLPOINTER*)&pInfo);
            Py_END_ALLOW_THREADS

            if (rc != SQL_NEED_DATA && rc != SQL_NO_DATA && !SQL_SUCCEEDED(rc))
                return RaiseErrorFromHandle(cur->cnxn, "SQLParamData", cur->cnxn->hdbc, cur->hstmt) != NULL;

            TRACE("SQLParamData() --> %d\n", rc);

            if (rc == SQL_NEED_DATA)
            {
                szLastFunction = "SQLPutData";
                if (PyBytes_Check(pInfo->cell)
    #if PY_VERSION_HEX >= 0x02060000
                 || PyByteArray_Check(pInfo->cell)
    #endif
                )
                {
                    char *(*pGetPtr)(PyObject*);
                    Py_ssize_t (*pGetLen)(PyObject*);
    #if PY_VERSION_HEX >= 0x02060000
                    if (PyByteArray_Check(pInfo->cell))
                    {
                        pGetPtr = PyByteArray_AsString;
                        pGetLen = PyByteArray_Size;
                    }
                    else
    #endif
                    {
                        pGetPtr = PyBytes_AsString;
                        pGetLen = PyBytes_Size;
                    }

                    const char* p = pGetPtr(pInfo->cell);
                    SQLLEN cb = (SQLLEN)pGetLen(pInfo->cell);
                    SQLLEN offset = 0;

                    do
                    {
                        SQLLEN remaining = min(pInfo->maxlen, cb - offset);
                        TRACE("SQLPutData [%d] (%d) %.10s\n", offset, remaining, &p[offset]);
                        Py_BEGIN_ALLOW_THREADS
                        rc = SQLPutData(cur->hstmt, (SQLPOINTER)&p[offset], remaining);
                        Py_END_ALLOW_THREADS
                        if (!SQL_SUCCEEDED(rc))
                            return RaiseErrorFromHandle(cur->cnxn, "SQLPutData", cur->cnxn->hdbc, cur->hstmt) != NULL;
                        offset += remaining;
                    }
                    while (offset < cb);
                }
    #if PY_MAJOR_VERSION < 3
                else if (PyBuffer_Check(pInfo->cell))
                {
                    // Buffers can have multiple segments, so we might need multiple writes.  Looping through buffers isn't
                    // difficult, but we've wrapped it up in an iterator object to keep this loop simple.

                    BufferSegmentIterator it(pInfo->cell);
                    byte* pb;
                    SQLLEN cb;
                    while (it.Next(pb, cb))
                    {
                        Py_BEGIN_ALLOW_THREADS
                        rc = SQLPutData(cur->hstmt, pb, cb);
                        Py_END_ALLOW_THREADS
                        if (!SQL_SUCCEEDED(rc))
                            return RaiseErrorFromHandle(cur->cnxn, "SQLPutData", cur->cnxn->hdbc, cur->hstmt) != NULL;
                    }
                }
    #endif
                Py_XDECREF(pInfo->cell);
                rc = SQL_NEED_DATA;
            }
        }

        if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA)
            return RaiseErrorFromHandle(cur->cnxn, szLastFunction, cur->cnxn->hdbc, cur->hstmt) != NULL;

        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)1, SQL_IS_UINTEGER);
        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, 0, SQL_IS_POINTER);
        pyodbc_free(cur->paramArray);
        cur->paramArray = 0;
    }

    Py_XDECREF(rowseq);
    FreeParameterData(cur);
	return ret;
}

static bool GetParamType(Cursor* cur, Py_ssize_t index, SQLSMALLINT& type)
{
    // Returns the ODBC type of the of given parameter.
    //
    // Normally we set the parameter type based on the parameter's Python object type (e.g. str --> SQL_CHAR), so this
    // is only called when the parameter is None.  In that case, we can't guess the type and have to use
    // SQLDescribeParam.
    //
    // If the database doesn't support SQLDescribeParam, we return SQL_VARCHAR since it converts to most other types.
    // However, it will not usually work if the target column is a binary column.

    if (!GetConnection(cur)->supports_describeparam || cur->paramcount == 0)
    {
        type = SQL_VARCHAR;
        return true;
    }

    if (cur->paramtypes == 0)
    {
        cur->paramtypes = reinterpret_cast<SQLSMALLINT*>(pyodbc_malloc(sizeof(SQLSMALLINT) * cur->paramcount));
        if (cur->paramtypes == 0)
        {
            PyErr_NoMemory();
            return false;
        }

        // SQL_UNKNOWN_TYPE is zero, so zero out all columns since we haven't looked any up yet.
        memset(cur->paramtypes, 0, sizeof(SQLSMALLINT) * cur->paramcount);
    }

    if (cur->paramtypes[index] == SQL_UNKNOWN_TYPE)
    {
        SQLULEN ParameterSizePtr;
        SQLSMALLINT DecimalDigitsPtr;
        SQLSMALLINT NullablePtr;
        SQLRETURN ret;

        Py_BEGIN_ALLOW_THREADS
        ret = SQLDescribeParam(cur->hstmt, (SQLUSMALLINT)(index + 1), &cur->paramtypes[index], &ParameterSizePtr, &DecimalDigitsPtr, &NullablePtr);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            // This can happen with ("select ?", None).  We'll default to VARCHAR which works with most types.
            cur->paramtypes[index] = SQL_VARCHAR;
        }
    }

    type = cur->paramtypes[index];
    return true;
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
