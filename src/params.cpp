
#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "params.h"
#include "cursor.h"
#include "connection.h"
#include "buffer.h"
#include "wrapper.h"
#include "errors.h"
#include "dbspecific.h"
#include "sqlwchar.h"
#include "row.h"
#include <datetime.h>


inline Connection* GetConnection(Cursor* cursor)
{
    return (Connection*)cursor->cnxn;
}

static bool GetParamType(Cursor* cur, Py_ssize_t iParam, SQLSMALLINT& type);

static bool InitInfos(ParamInfo* a, Py_ssize_t count, Py_ssize_t paramSetSize)
{
    memset(a, 0, sizeof(ParamInfo) * count);
    for (Py_ssize_t i = 0; i < count; i++)
    {
        a[i].ParamSetSize = paramSetSize;
        if (paramSetSize > 1)
        {
            a[i].StrLen_or_IndPtr = (SQLLEN *)pyodbc_malloc(sizeof(SQLLEN) * a[i].ParamSetSize);
            if (!a[i].StrLen_or_IndPtr)
            {
                PyErr_NoMemory();
                return false;
            }
            memset(a[i].StrLen_or_IndPtr, 0, sizeof(SQLLEN) * a[i].ParamSetSize);

            a[i].ParameterObjects = (PyObject**)pyodbc_malloc(sizeof(PyObject*) * a[i].ParamSetSize);
            if (!a[i].ParameterObjects)
            {
                PyErr_NoMemory();
                return false;
            }
            memset(a[i].ParameterObjects, 0, sizeof(PyObject*) * a[i].ParamSetSize);
        }
        else
        {
            a[i].ParameterValuePtr = a[i].ParameterValueBuffer;
            a[i].StrLen_or_IndPtr = a[i].StrLen_or_IndBuffer;
            a[i].ParameterObjects = a[i].ParameterObjectBuffer;
        }
    }
    return true;
}

static void FreeInfos(ParamInfo* a, Py_ssize_t count)
{
    for (Py_ssize_t i = 0; i < count; i++)
    {
        if (a[i].ParameterObjects)
        {
            for (Py_ssize_t j = 0; j < a[i].ParamSetSize; j++)
                Py_XDECREF(a[i].ParameterObjects[j]);
            if(a[i].ParameterObjects != a[i].ParameterObjectBuffer)
                pyodbc_free(a[i].ParameterObjects);
        }
        if (a[i].StrLen_or_IndPtr && a[i].StrLen_or_IndPtr != a[i].StrLen_or_IndBuffer)
            pyodbc_free(a[i].StrLen_or_IndPtr);
        if (!a[i].IsParameterValueBorrowed && a[i].ParameterValuePtr && a[i].ParameterValuePtr != a[i].ParameterValueBuffer)
            pyodbc_free(a[i].ParameterValuePtr);
    }
    pyodbc_free(a);
}

#define _MAKESTR(n) case n: return #n
static const char* SqlTypeName(SQLSMALLINT n)
{
    switch (n)
    {
        _MAKESTR(SQL_UNKNOWN_TYPE);
        _MAKESTR(SQL_CHAR);
        _MAKESTR(SQL_VARCHAR);
        _MAKESTR(SQL_LONGVARCHAR);
        _MAKESTR(SQL_NUMERIC);
        _MAKESTR(SQL_DECIMAL);
        _MAKESTR(SQL_INTEGER);
        _MAKESTR(SQL_SMALLINT);
        _MAKESTR(SQL_FLOAT);
        _MAKESTR(SQL_REAL);
        _MAKESTR(SQL_DOUBLE);
        _MAKESTR(SQL_DATETIME);
        _MAKESTR(SQL_WCHAR);
        _MAKESTR(SQL_WVARCHAR);
        _MAKESTR(SQL_WLONGVARCHAR);
        _MAKESTR(SQL_TYPE_DATE);
        _MAKESTR(SQL_TYPE_TIME);
        _MAKESTR(SQL_TYPE_TIMESTAMP);
        _MAKESTR(SQL_SS_TIME2);
        _MAKESTR(SQL_SS_XML);
        _MAKESTR(SQL_BINARY);
        _MAKESTR(SQL_VARBINARY);
        _MAKESTR(SQL_LONGVARBINARY);
    }
    return "unknown";
}

static const char* CTypeName(SQLSMALLINT n)
{
    switch (n)
    {
        _MAKESTR(SQL_C_CHAR);
        _MAKESTR(SQL_C_WCHAR);
        _MAKESTR(SQL_C_LONG);
        _MAKESTR(SQL_C_SHORT);
        _MAKESTR(SQL_C_FLOAT);
        _MAKESTR(SQL_C_DOUBLE);
        _MAKESTR(SQL_C_NUMERIC);
        _MAKESTR(SQL_C_DEFAULT);
        _MAKESTR(SQL_C_DATE);
        _MAKESTR(SQL_C_TIME);
        _MAKESTR(SQL_C_TIMESTAMP);
        _MAKESTR(SQL_C_TYPE_DATE);
        _MAKESTR(SQL_C_TYPE_TIME);
        _MAKESTR(SQL_C_TYPE_TIMESTAMP);
        _MAKESTR(SQL_C_INTERVAL_YEAR);
        _MAKESTR(SQL_C_INTERVAL_MONTH);
        _MAKESTR(SQL_C_INTERVAL_DAY);
        _MAKESTR(SQL_C_INTERVAL_HOUR);
        _MAKESTR(SQL_C_INTERVAL_MINUTE);
        _MAKESTR(SQL_C_INTERVAL_SECOND);
        _MAKESTR(SQL_C_INTERVAL_YEAR_TO_MONTH);
        _MAKESTR(SQL_C_INTERVAL_DAY_TO_HOUR);
        _MAKESTR(SQL_C_INTERVAL_DAY_TO_MINUTE);
        _MAKESTR(SQL_C_INTERVAL_DAY_TO_SECOND);
        _MAKESTR(SQL_C_INTERVAL_HOUR_TO_MINUTE);
        _MAKESTR(SQL_C_INTERVAL_HOUR_TO_SECOND);
        _MAKESTR(SQL_C_INTERVAL_MINUTE_TO_SECOND);
        _MAKESTR(SQL_C_BINARY);
        _MAKESTR(SQL_C_BIT);
        _MAKESTR(SQL_C_SBIGINT);
        _MAKESTR(SQL_C_UBIGINT);
        _MAKESTR(SQL_C_TINYINT);
        _MAKESTR(SQL_C_SLONG);
        _MAKESTR(SQL_C_SSHORT);
        _MAKESTR(SQL_C_STINYINT);
        _MAKESTR(SQL_C_ULONG);
        _MAKESTR(SQL_C_USHORT);
        _MAKESTR(SQL_C_UTINYINT);
        _MAKESTR(SQL_C_GUID);
    }
    return "unknown";
}

static bool ExpandParameterValueBuffer(ParamInfo& info, SQLINTEGER bufferLength, SQLINTEGER maxLength)
{
    if (bufferLength <= info.BufferLength)
        return true;

    if (info.BufferLength >= maxLength)
        return false;
    SQLINTEGER newBufferLength = min(info.BufferLength * 2, maxLength);
    SQLPOINTER newBuffer = pyodbc_malloc((size_t)(newBufferLength * info.ParamSetSize));
    if (!newBuffer)
    {
        PyErr_NoMemory();
        return false;
    }

    for (Py_ssize_t i = info.ParamSetSize; i > 0; i--)
        memcpy((SQLCHAR*)newBuffer + newBufferLength * (i - 1), (SQLCHAR*)info.ParameterValuePtr + info.BufferLength * (i - 1), (size_t)info.BufferLength);

    pyodbc_free(info.ParameterValuePtr);
    info.ParameterValuePtr = newBuffer;
    info.BufferLength = newBufferLength;

    return true;
}

static bool UpdateInfo(ParamInfo& info, Py_ssize_t paramSetIndex, SQLSMALLINT valueType, SQLUINTEGER columnSize, SQLSMALLINT parameterType, SQLPOINTER buffer, SQLINTEGER bufferLength, SQLINTEGER maxLength, bool isBufferBorrowed, SQLLEN strLenOrInd)
{
    info.StrLen_or_IndPtr[paramSetIndex] = strLenOrInd;

    if (info.ParameterType == SQL_UNKNOWN_TYPE)
    {
        info.ValueType = valueType;
        info.ColumnSize = columnSize;
        info.ParameterType = parameterType;
        info.BufferLength = bufferLength;

        if (info.ParamSetSize == 1 && isBufferBorrowed)
        {
            info.ParameterValuePtr = buffer;
            info.IsParameterValueBorrowed = true;
        }
        else if (info.ParamSetSize == 1 && bufferLength <= sizeof(info.ParameterValueBuffer))
        {
            if (buffer)
                memcpy(info.ParameterValuePtr, buffer, (size_t)bufferLength);
        }
        else
        {
            switch (info.ValueType)
            {
                case SQL_C_CHAR:
                case SQL_C_WCHAR:
                case SQL_C_BINARY:
                    if (buffer)
                        info.BufferLength = max(info.BufferLength, 64);
                    break;
            }
            info.ParameterValuePtr = pyodbc_malloc((size_t)(info.BufferLength * info.ParamSetSize));
            if (!info.ParameterValuePtr)
            {
                PyErr_NoMemory();
                return false;
            }
            memset(info.ParameterValuePtr, 0, (size_t)(info.BufferLength * info.ParamSetSize));
            if (buffer)
                memcpy(info.ParameterValuePtr, buffer, (size_t)bufferLength);
        }

        return true;
    }

    if (info.ValueType == valueType && info.ParameterType == parameterType)
    {
        info.ColumnSize = max(info.ColumnSize, columnSize);
        if (!ExpandParameterValueBuffer(info, bufferLength, maxLength))
            return false;
        if (buffer)
            memcpy((SQLCHAR*)info.ParameterValuePtr + info.BufferLength * paramSetIndex, buffer, (size_t)bufferLength);
        return true;
    }

    if (!buffer)
    {
        // earlier non-null values have established the parameter information
        return true;
    }

    //	all earlier values must be null
    for (Py_ssize_t i = 0; i < paramSetIndex; i++)
    {
        if ( info.StrLen_or_IndPtr[i] != SQL_NULL_DATA )
        {
            RaiseErrorV(0, ProgrammingError, "Incompatible types in same parameter to executemany.");
            return false;
        }
    }

    // free prior allocated parameter values
    if (!info.IsParameterValueBorrowed && info.ParameterValuePtr && info.ParameterValuePtr != info.ParameterValueBuffer)
    {
        pyodbc_free(info.ParameterValuePtr);
        info.ParameterValuePtr = 0;
    }

    // change parameter info
    info.ValueType = valueType;
    info.ColumnSize = columnSize;
    info.ParameterType = parameterType;
    info.BufferLength = bufferLength;
    switch (info.ValueType)
    {
        case SQL_C_CHAR:
        case SQL_C_WCHAR:
        case SQL_C_BINARY:
            info.BufferLength = max(info.BufferLength, 64);
            break;
    }

    // allocate new parameter values
    info.ParameterValuePtr = pyodbc_malloc((size_t)(info.BufferLength * info.ParamSetSize));
    if (!info.ParameterValuePtr)
    {
        PyErr_NoMemory();
        return false;
    }
    memset(info.ParameterValuePtr, 0, (size_t)(info.BufferLength * info.ParamSetSize));
    if (buffer)
        memcpy((SQLCHAR*)info.ParameterValuePtr + info.BufferLength * paramSetIndex, buffer, (size_t)bufferLength);

    return true;
}

static bool GetNullInfo(Cursor* cur, Py_ssize_t index, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    SQLSMALLINT parameterType = info.ParameterType;
    if (parameterType == SQL_UNKNOWN_TYPE)
    {
        if (!GetParamType(cur, index, parameterType))
            return false;
    }

    if (!UpdateInfo(info, paramSetIndex, SQL_C_DEFAULT, 1, parameterType, NULL, 1, 1, false, SQL_NULL_DATA))
        return false;

    return true;
}

static bool GetNullBinaryInfo(Cursor* cur, Py_ssize_t index, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, 1, SQL_BINARY, NULL, 1, 1, false, SQL_NULL_DATA))
        return false;

    return true;
}


static bool GetBytesInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    // In Python 2, a bytes object (ANSI string) is passed as varchar.  In Python 3, it is passed as binary.

    Py_ssize_t len = PyBytes_GET_SIZE(param);

    SQLUINTEGER columnSize = (SQLUINTEGER)max(len, 1);

#if PY_MAJOR_VERSION >= 3
    if (len <= cur->cnxn->binary_maxlength)
    {
        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, columnSize, SQL_VARBINARY, PyBytes_AS_STRING(param), len, true, len))
            return false;
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, columnSize, SQL_LONGVARBINARY, &param, sizeof(PyObject*), false, SQL_LEN_DATA_AT_EXEC((SQLLEN)len)))
            return false;
    }
#else
    if (len <= cur->cnxn->varchar_maxlength)
    {
        if (!UpdateInfo(info, paramSetIndex, SQL_C_CHAR, columnSize, SQL_VARCHAR, PyBytes_AS_STRING(param), len, cur->cnxn->varchar_maxlength, true, len))
            return false;
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        if (!UpdateInfo(info, paramSetIndex, SQL_C_CHAR, columnSize, SQL_LONGVARCHAR, &param, sizeof(PyObject*), sizeof(PyObject*), false, SQL_LEN_DATA_AT_EXEC((SQLLEN)len)))
            return false;
    }
#endif

    return true;
}

static bool GetUnicodeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    Py_ssize_t  len = PyUnicode_GET_SIZE(param);

    SQLUINTEGER columnSize = (SQLUINTEGER)max(len, 1);

    if (len <= cur->cnxn->wvarchar_maxlength)
    {
        Py_UNICODE* pch = PyUnicode_AsUnicode(param);
        SQLPOINTER buffer = pch;
        bool isBufferBorrowed = true;
        if (len > 0 && SQLWCHAR_SIZE != Py_UNICODE_SIZE)
        {
            buffer = SQLWCHAR_FromUnicode(pch, len);
            isBufferBorrowed = false;
        }
        if (!UpdateInfo(info, paramSetIndex, SQL_C_WCHAR, columnSize, SQL_WVARCHAR, buffer, (SQLINTEGER)(len * sizeof(SQLWCHAR)), (SQLINTEGER)(cur->cnxn->wvarchar_maxlength * sizeof(SQLWCHAR)), isBufferBorrowed, (SQLLEN)(len * sizeof(SQLWCHAR))))
        {
            if (!isBufferBorrowed)
                pyodbc_free(buffer);
            return false;
        }
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        if (!UpdateInfo(info, paramSetIndex, SQL_C_WCHAR, columnSize, SQL_WLONGVARCHAR, &param, sizeof(PyObject*), sizeof(PyObject*), false, SQL_LEN_DATA_AT_EXEC((SQLLEN)(len * sizeof(SQLWCHAR)))))
            return false;
    }

    return true;
}

static bool GetBooleanInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    unsigned char bit = (unsigned char)(param == Py_True ? 1 : 0);

    if (!UpdateInfo(info, paramSetIndex, SQL_C_BIT, sizeof(unsigned char), SQL_BIT, &bit, sizeof(bit), sizeof(bit), false, 1))
        return false;

    return true;
}

static bool GetDateTimeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    TIMESTAMP_STRUCT timestamp;
    timestamp.year   = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
    timestamp.month  = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
    timestamp.day    = (SQLUSMALLINT)PyDateTime_GET_DAY(param);
    timestamp.hour   = (SQLUSMALLINT)PyDateTime_DATE_GET_HOUR(param);
    timestamp.minute = (SQLUSMALLINT)PyDateTime_DATE_GET_MINUTE(param);
    timestamp.second = (SQLUSMALLINT)PyDateTime_DATE_GET_SECOND(param);

    // SQL Server chokes if the fraction has more data than the database supports.  We expect other databases to be the
    // same, so we reduce the value to what the database supports.  http://support.microsoft.com/kb/263872

    int precision = ((Connection*)cur->cnxn)->datetime_precision - 20; // (20 includes a separating period)
    if (precision <= 0)
    {
        timestamp.fraction = 0;
    }
    else
    {
        timestamp.fraction = (SQLUINTEGER)(PyDateTime_DATE_GET_MICROSECOND(param) * 1000); // 1000 == micro -> nano

        // (How many leading digits do we want to keep?  With SQL Server 2005, this should be 3: 123000000)
        int keep = (int)pow(10.0, 9-min(9, precision));
        timestamp.fraction = timestamp.fraction / keep * keep;
        info.DecimalDigits = (SQLSMALLINT)precision;
    }

    if (!UpdateInfo(info, paramSetIndex, SQL_C_TIMESTAMP, (SQLUINTEGER)((Connection*)cur->cnxn)->datetime_precision, SQL_TIMESTAMP, &timestamp, sizeof(timestamp), sizeof(timestamp), false, sizeof(TIMESTAMP_STRUCT)))
        return false;

    return true;
}

static bool GetDateInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    DATE_STRUCT date;
    date.year  = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
    date.month = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
    date.day   = (SQLUSMALLINT)PyDateTime_GET_DAY(param);

    if (!UpdateInfo(info, paramSetIndex, SQL_C_TYPE_DATE, 10, SQL_TYPE_DATE, &date, sizeof(date), sizeof(date), false, sizeof(DATE_STRUCT)))
        return false;

    return true;
}

static bool GetTimeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    TIME_STRUCT time;
    time.hour   = (SQLUSMALLINT)PyDateTime_TIME_GET_HOUR(param);
    time.minute = (SQLUSMALLINT)PyDateTime_TIME_GET_MINUTE(param);
    time.second = (SQLUSMALLINT)PyDateTime_TIME_GET_SECOND(param);

    if(!UpdateInfo(info, paramSetIndex, SQL_C_TYPE_TIME, 8, SQL_TYPE_TIME, &time, sizeof(time), sizeof(time), false, sizeof(TIME_STRUCT)))
        return false;

    return true;
}

#if PY_MAJOR_VERSION < 3
static bool GetIntInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    long l = PyInt_AsLong(param);

#if LONG_BIT == 64
	if (!UpdateInfo(info, paramSetIndex, SQL_C_SBIGINT, sizeof(long), SQL_BIGINT, &l, sizeof(l), sizeof(l), false, 0))
		return false;
#elif LONG_BIT == 32
	if (!UpdateInfo(info, paramSetIndex, SQL_C_LONG, sizeof(long), SQL_INTEGER, &l, sizeof(l), sizeof(l), false, 0))
		return false;
#else
    #error Unexpected LONG_BIT value
#endif

    return true;
}
#endif

static bool GetLongInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    // TODO: Overflow?
    INT64 i64 = (INT64)PyLong_AsLongLong(param);

    if (!UpdateInfo(info, paramSetIndex, SQL_C_SBIGINT, sizeof(INT64), SQL_BIGINT, &i64, sizeof(i64), sizeof(i64), false, 0))
        return false;

    return true;
}

static bool GetFloatInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    // TODO: Overflow?
    double dbl = PyFloat_AsDouble(param);

    if (!UpdateInfo(info, paramSetIndex, SQL_C_DOUBLE, 15, SQL_DOUBLE, &dbl, sizeof(dbl), sizeof(dbl), false, 0))
        return false;

    return true;
}

static char* CreateDecimalString(long sign, PyObject* digits, long exp)
{
    long count = (long)PyTuple_GET_SIZE(digits);

    char* pch;
    int len;

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

static bool GetDecimalInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    // The NUMERIC structure never works right with SQL Server and probably a lot of other drivers.  We'll bind as a
    // string.  Unfortunately, the Decimal class doesn't seem to have a way to force it to return a string without
    // exponents, so we'll have to build it ourselves.

    Object t = PyObject_CallMethod(param, "as_tuple", 0);
    if (!t)
        return false;

    long       sign   = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 0));
    PyObject*  digits = PyTuple_GET_ITEM(t.Get(), 1);
    long       exp    = PyInt_AsLong(PyTuple_GET_ITEM(t.Get(), 2));

    Py_ssize_t count = PyTuple_GET_SIZE(digits);

    SQLUINTEGER columnSize;

    if (exp >= 0)
    {
        // (1 2 3) exp = 2 --> '12300'

        columnSize         = (SQLUINTEGER)count + exp;
        info.DecimalDigits = 0;

    }
    else if (-exp <= count)
    {
        // (1 2 3) exp = -2 --> 1.23 : prec = 3, scale = 2
        columnSize         = (SQLUINTEGER)count;
        info.DecimalDigits = (SQLSMALLINT)-exp;
    }
    else
    {
        // (1 2 3) exp = -5 --> 0.00123 : prec = 5, scale = 5
        columnSize         = (SQLUINTEGER)(count + (-exp));
        info.DecimalDigits = (SQLSMALLINT)info.ColumnSize;
    }

    I(columnSize >= (SQLULEN)info.DecimalDigits);

    char* buffer = CreateDecimalString(sign, digits, exp);
    if (!buffer)
    {
        PyErr_NoMemory();
        return false;
    }
    SQLINTEGER bufferLength = (SQLINTEGER)strlen(buffer);

    if (!UpdateInfo(info, paramSetIndex, SQL_C_CHAR, columnSize, SQL_NUMERIC, buffer, bufferLength, cur->cnxn->varchar_maxlength, false, bufferLength))
    {
        pyodbc_free(buffer);
        return false;
    }
    
    pyodbc_free(buffer);
    return true;
}

#if PY_MAJOR_VERSION < 3
static bool GetBufferInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    const char* pb;
    Py_ssize_t  cb = PyBuffer_GetMemory(param, &pb);

    if (cb != -1 && cb <= cur->cnxn->binary_maxlength)
    {
        // There is one segment, so we can bind directly into the buffer object.

        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, (SQLUINTEGER)max(cb, 1), SQL_VARBINARY, (SQLPOINTER)pb, cb, cur->cnxn->binary_maxlength, true, cb))
            return false;
    }
    else
    {
        // There are multiple segments, so we'll provide the data at execution time.  Pass the PyObject pointer as
        // the parameter value which will be pased back to us when the data is needed.  (If we release threads, we
        // need to up the refcount!)

        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, (SQLUINTEGER)PyBuffer_Size(param), SQL_LONGVARBINARY, &param, sizeof(PyObject*), sizeof(PyObject*), false, SQL_LEN_DATA_AT_EXEC(PyBuffer_Size(param))))
            return false;
    }

    return true;
}
#endif

#if PY_VERSION_HEX >= 0x02060000
static bool GetByteArrayInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetIndex)
{
    info.ValueType = SQL_C_BINARY;

    Py_ssize_t cb = PyByteArray_Size(param);
    if (cb <= cur->cnxn->binary_maxlength)
    {
        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, (SQLUINTEGER)max(cb, 1), SQL_VARBINARY, PyByteArray_AsString(param), cb, cur->cnxn->binary_maxlength, true, cb))
            return false;
    }
    else
    {
        if (!UpdateInfo(info, paramSetIndex, SQL_C_BINARY, (SQLUINTEGER)cb, SQL_LONGVARBINARY, &param, sizeof(PyObject*), sizeof(PyObject*), false, SQL_LEN_DATA_AT_EXEC(cb)))
            return false;
    }
    return true;
}
#endif

static bool GetParameterInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, Py_ssize_t paramSetSize, Py_ssize_t paramSetIndex)
{
    // Determines the type of SQL parameter that will be used for this parameter based on the Python data type.
    //
    // Populates `info`.

    // Hold a reference to param until info is freed, because info will often be holding data borrowed from param.
    info.ParameterObjects[paramSetIndex] = param;

    if (param == Py_None)
        return GetNullInfo(cur, index, info, paramSetIndex);

    if (param == null_binary)
        return GetNullBinaryInfo(cur, index, info, paramSetIndex);

    if (PyBytes_Check(param))
        return GetBytesInfo(cur, index, param, info, paramSetIndex);

    if (PyUnicode_Check(param))
        return GetUnicodeInfo(cur, index, param, info, paramSetIndex);

    if (PyBool_Check(param))
        return GetBooleanInfo(cur, index, param, info, paramSetIndex);

    if (PyDateTime_Check(param))
        return GetDateTimeInfo(cur, index, param, info, paramSetIndex);

    if (PyDate_Check(param))
        return GetDateInfo(cur, index, param, info, paramSetIndex);

    if (PyTime_Check(param))
        return GetTimeInfo(cur, index, param, info, paramSetIndex);

    if (PyLong_Check(param))
        return GetLongInfo(cur, index, param, info, paramSetIndex);

    if (PyFloat_Check(param))
        return GetFloatInfo(cur, index, param, info, paramSetIndex);

    if (PyDecimal_Check(param))
        return GetDecimalInfo(cur, index, param, info, paramSetIndex);

#if PY_VERSION_HEX >= 0x02060000
    if (PyByteArray_Check(param))
        return GetByteArrayInfo(cur, index, param, info, paramSetIndex);
#endif

#if PY_MAJOR_VERSION < 3
    if (PyInt_Check(param))
        return GetIntInfo(cur, index, param, info, paramSetIndex);

    if (PyBuffer_Check(param))
        return GetBufferInfo(cur, index, param, info, paramSetIndex);
#endif

    RaiseErrorV("HY105", ProgrammingError, "Invalid parameter type.  param-index=%zd param-type=%s", index, Py_TYPE(param)->tp_name);
    return false;
}

bool BindParameter(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    TRACE("BIND: param=%d ValueType=%d (%s) ParameterType=%d (%s) ColumnSize=%d DecimalDigits=%d BufferLength=%d *pcb=%d\n",
          (index+1), info.ValueType, CTypeName(info.ValueType), info.ParameterType, SqlTypeName(info.ParameterType), info.ColumnSize,
          info.DecimalDigits, info.BufferLength, info.StrLen_or_IndPtr[0]);

    SQLRETURN ret = -1;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(index + 1), SQL_PARAM_INPUT, info.ValueType, info.ParameterType, info.ColumnSize, info.DecimalDigits, info.ParameterValuePtr, info.BufferLength, info.StrLen_or_IndPtr);
    Py_END_ALLOW_THREADS;

    if (GetConnection(cur)->hdbc == SQL_NULL_HANDLE)
    {
        // The connection was closed by another thread in the ALLOW_THREADS block above.
        RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
        return false;
    }

    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle("SQLBindParameter", GetConnection(cur)->hdbc, cur->hstmt);
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

static bool Prepare(Cursor* cur, PyObject* pSql, Py_ssize_t cParamSetSize)
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
    SQLRETURN ret = 0;

    if (pSql != cur->pPreparedSQL)
    {
        FreeParameterInfo(cur);

        SQLSMALLINT cParamsT = 0;
        const char* szErrorFunc = "SQLPrepare";

        if (PyUnicode_Check(pSql))
        {
            SQLWChar sql(pSql);
            Py_BEGIN_ALLOW_THREADS
            ret = SQLPrepareW(cur->hstmt, sql, SQL_NTS);
            if (SQL_SUCCEEDED(ret))
            {
                szErrorFunc = "SQLNumParams";
                ret = SQLNumParams(cur->hstmt, &cParamsT);
            }
            Py_END_ALLOW_THREADS
        }
#if PY_MAJOR_VERSION < 3
        else
        {
            TRACE("SQLPrepare(%s)\n", PyString_AS_STRING(pSql));
            Py_BEGIN_ALLOW_THREADS
            ret = SQLPrepare(cur->hstmt, (SQLCHAR*)PyString_AS_STRING(pSql), SQL_NTS);
            if (SQL_SUCCEEDED(ret))
            {
                szErrorFunc = "SQLNumParams";
                ret = SQLNumParams(cur->hstmt, &cParamsT);
            }
            Py_END_ALLOW_THREADS
        }
#endif

        if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
        {
            // The connection was closed by another thread in the ALLOW_THREADS block above.
            RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
            return false;
        }

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle(szErrorFunc, GetConnection(cur)->hdbc, cur->hstmt);
            return false;
        }

        cur->paramcount = (int)cParamsT;

        cur->pPreparedSQL = pSql;
        Py_INCREF(cur->pPreparedSQL);
    }

    if (cur->cnxn->useParameterArrayBinding && cParamSetSize != cur->paramSetSize)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)cParamSetSize, SQL_IS_INTEGER);
        Py_END_ALLOW_THREADS

        if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
        {
            // The connection was closed by another thread in the ALLOW_THREADS block above.
            RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
            return false;
        }

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorV(0, ArrayBindingNotSupportedError, "Binding parameters to arrays is not supported.");
            return false;
        }

        cur->paramSetSize = cParamSetSize;
    }

    return true;
}

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* original_params, Py_ssize_t paramsOffset)
{
    if (!Prepare(cur, pSql, 1))
    {
        return false;
    }

    //
    // Normalize the parameter variables.
    //

    // Since we may replace parameters (we replace objects with Py_True/Py_False when writing to a bit/bool column),
    // allocate an array and use it instead of the original sequence

    Py_ssize_t cParams       = original_params == 0 ? 0 : PySequence_Length(original_params) - paramsOffset;

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
    if (!InitInfos(cur->paramInfos, cParams, 1))
    {
        FreeInfos(cur->paramInfos, cParams);
        cur->paramInfos = 0;
        return false;
    }

    // Since you can't call SQLDesribeParam *after* calling SQLBindParameter, we'll loop through all of the
    // GetParameterInfos first, then bind.

    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        // PySequence_GetItem returns a *new* reference, which GetParameterInfo will take ownership of.  It is stored
        // in paramInfos and will be released in FreeInfos (which is always eventually called).

        PyObject* param = PySequence_GetItem(original_params, i + paramsOffset);
        if (!GetParameterInfo(cur, i, param, cur->paramInfos[i], 1, 0))
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

bool PrepareAndBindArray(Cursor* cur, PyObject* pSql, PyObject* original_params)
{
    Py_ssize_t cItems = PySequence_Length(original_params);
    if ( cItems == 0 )
    {
        RaiseErrorV(0, ProgrammingError, "Parameters must be a non-empty sequence");
        return false;
    }

    if (!Prepare(cur, pSql, cItems))
    {
        return false;
    }

    cur->paramInfos = (ParamInfo*)pyodbc_malloc(sizeof(ParamInfo) * cur->paramcount);
    if (cur->paramInfos == 0)
    {
        PyErr_NoMemory();
        return false;
    }
    if (!InitInfos(cur->paramInfos, cur->paramcount, cItems))
    {
        FreeInfos(cur->paramInfos, cur->paramcount);
        cur->paramInfos = 0;
        return false;
    }

    // Since you can't call SQLDesribeParam *after* calling SQLBindParameter, we'll loop through all of the
    // GetParameterInfos first, then bind.

    for (Py_ssize_t paramSetIndex = 0; paramSetIndex < cItems; paramSetIndex++ )
    {
        PyObject *paramSet = PySequence_GetItem(original_params, paramSetIndex);

        // TODO: check type of param set; allow it to be a non-sequence and treat it as a sequence of one item
        if (PyList_Check(paramSet) || PyTuple_Check(paramSet) || Row_Check(paramSet))
        {
            Py_ssize_t paramCount = PySequence_Length(paramSet);
            for (Py_ssize_t paramIndex = 0; paramIndex < cur->paramcount; paramIndex++)
            {
                PyObject *param;
                if ( paramIndex <= paramCount )
                {
                    param = PySequence_GetItem(paramSet, paramIndex);
                }
                else
                {
                    param = Py_None;
                    Py_INCREF(param);
                }

                if (!GetParameterInfo(cur, paramIndex, param, cur->paramInfos[paramIndex], cItems, paramSetIndex))
                {
                    Py_XDECREF(param);
                    Py_XDECREF(paramSet);
                    FreeInfos(cur->paramInfos, cur->paramcount);
                    cur->paramInfos = 0;
                    return false;
                }
            }
        }

        Py_XDECREF(paramSet);
    }

    for (Py_ssize_t i = 0; i < cur->paramcount; i++)
    {
        if (!BindParameter(cur, i, cur->paramInfos[i]))
        {
            FreeInfos(cur->paramInfos, cur->paramcount);
            cur->paramInfos = 0;
            return false;
        }
    }

    return true;
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

