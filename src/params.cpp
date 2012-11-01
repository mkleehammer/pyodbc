

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
#include <datetime.h>


inline Connection* GetConnection(Cursor* cursor)
{
    return (Connection*)cursor->cnxn;
}

static bool GetParamType(Cursor* cur, Py_ssize_t iParam, SQLSMALLINT& type);

static void FreeInfos(ParamInfo* a, Py_ssize_t count)
{
    for (Py_ssize_t i = 0; i < count; i++)
    {
        if (a[i].allocated)
            pyodbc_free(a[i].ParameterValuePtr);
        Py_XDECREF(a[i].pParam);
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


static bool GetBytesInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // In Python 2, a bytes object (ANSI string) is passed as varchar.  In Python 3, it is passed as binary.

    Py_ssize_t len = PyBytes_GET_SIZE(param);

#if PY_MAJOR_VERSION >= 3
    info.ValueType = SQL_C_BINARY;
    info.ColumnSize = (SQLUINTEGER)max(len, 1);

    if (len <= cur->cnxn->binary_maxlength)
    {
        info.ParameterType     = SQL_VARBINARY;
        info.StrLen_or_Ind     = len;
        info.ParameterValuePtr = PyBytes_AS_STRING(param);
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = SQL_LONGVARBINARY;
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        info.ParameterValuePtr = param;
    }

#else
    info.ValueType = SQL_C_CHAR;
    info.ColumnSize = (SQLUINTEGER)max(len, 1);

    if (len <= cur->cnxn->varchar_maxlength)
    {
        info.ParameterType     = SQL_VARCHAR;
        info.StrLen_or_Ind     = len;
        info.ParameterValuePtr = PyBytes_AS_STRING(param);
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = SQL_LONGVARCHAR;
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        info.ParameterValuePtr = param;
    }
#endif

    return true;
}

static bool GetUnicodeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    Py_UNICODE* pch = PyUnicode_AsUnicode(param);
    Py_ssize_t  len = PyUnicode_GET_SIZE(param);

    info.ValueType  = SQL_C_WCHAR;
    info.ColumnSize = (SQLUINTEGER)max(len, 1);

    if (len <= cur->cnxn->wvarchar_maxlength)
    {
        if (SQLWCHAR_SIZE == Py_UNICODE_SIZE)
        {
            info.ParameterValuePtr = pch;
        }
        else
        {
            // SQLWCHAR and Py_UNICODE are not the same size, so we need to allocate and copy a buffer.
            if (len > 0)
            {
                info.ParameterValuePtr = SQLWCHAR_FromUnicode(pch, len);
                if (info.ParameterValuePtr == 0)
                    return false;
                info.allocated = true;
            }
            else
            {
                info.ParameterValuePtr = pch;
            }
        }

        info.ParameterType = SQL_WVARCHAR;
        info.StrLen_or_Ind = (SQLINTEGER)(len * sizeof(SQLWCHAR));
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.

        info.ParameterType     = SQL_WLONGVARCHAR;
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len * sizeof(SQLWCHAR)) : SQL_DATA_AT_EXEC;
        info.ParameterValuePtr = param;
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
    // TODO: Overflow?
    info.Data.i64 = (INT64)PyLong_AsLongLong(param);

    info.ValueType         = SQL_C_SBIGINT;
    info.ParameterType     = SQL_BIGINT;
    info.ParameterValuePtr = &info.Data.i64;
    return true;
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

static bool GetDecimalInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
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

    if (cb != -1 && cb <= cur->cnxn->binary_maxlength)
    {
        // There is one segment, so we can bind directly into the buffer object.

        info.ParameterType     = SQL_VARBINARY;
        info.ParameterValuePtr = (SQLPOINTER)pb;
        info.BufferLength      = cb;
        info.ColumnSize        = (SQLUINTEGER)max(cb, 1);
        info.StrLen_or_Ind     = cb;
    }
    else
    {
        // There are multiple segments, so we'll provide the data at execution time.  Pass the PyObject pointer as
        // the parameter value which will be pased back to us when the data is needed.  (If we release threads, we
        // need to up the refcount!)

        info.ParameterType     = SQL_LONGVARBINARY;
        info.ParameterValuePtr = param;
        info.ColumnSize        = (SQLUINTEGER)PyBuffer_Size(param);
        info.BufferLength      = sizeof(PyObject*); // How big is ParameterValuePtr; ODBC copies it and gives it back in SQLParamData
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)PyBuffer_Size(param)) : SQL_DATA_AT_EXEC;
    }

    return true;
}
#endif

#if PY_VERSION_HEX >= 0x02060000
static bool GetByteArrayInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    info.ValueType = SQL_C_BINARY;

    Py_ssize_t cb = PyByteArray_Size(param);
    if (cb <= cur->cnxn->binary_maxlength)
    {
        info.ParameterType     = SQL_VARBINARY;
        info.ParameterValuePtr = (SQLPOINTER)PyByteArray_AsString(param);
        info.BufferLength      = cb;
        info.ColumnSize        = (SQLUINTEGER)max(cb, 1);
        info.StrLen_or_Ind     = cb;
    }
    else
    {
        info.ParameterType     = SQL_LONGVARBINARY;
        info.ParameterValuePtr = param;
        info.ColumnSize        = (SQLUINTEGER)cb;
        info.BufferLength      = sizeof(PyObject*); // How big is ParameterValuePtr; ODBC copies it and gives it back in SQLParamData
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)cb) : SQL_DATA_AT_EXEC;
    }
    return true;
}
#endif

static bool GetParameterInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // Determines the type of SQL parameter that will be used for this parameter based on the Python data type.
    //
    // Populates `info`.

    // Hold a reference to param until info is freed, because info will often be holding data borrowed from param.
    info.pParam = param;

    if (param == Py_None)
        return GetNullInfo(cur, index, info);

    if (param == null_binary)
        return GetNullBinaryInfo(cur, index, info);

    if (PyBytes_Check(param))
        return GetBytesInfo(cur, index, param, info);

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

    if (PyDecimal_Check(param))
        return GetDecimalInfo(cur, index, param, info);

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

    RaiseErrorV("HY105", ProgrammingError, "Invalid parameter type.  param-index=%zd param-type=%s", index, Py_TYPE(param)->tp_name);
    return false;
}

bool BindParameter(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    TRACE("BIND: param=%d ValueType=%d (%s) ParameterType=%d (%s) ColumnSize=%d DecimalDigits=%d BufferLength=%d *pcb=%d\n",
          (index+1), info.ValueType, CTypeName(info.ValueType), info.ParameterType, SqlTypeName(info.ParameterType), info.ColumnSize,
          info.DecimalDigits, info.BufferLength, info.StrLen_or_Ind);

    SQLRETURN ret = -1;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(index + 1), SQL_PARAM_INPUT, info.ValueType, info.ParameterType, info.ColumnSize, info.DecimalDigits, info.ParameterValuePtr, info.BufferLength, &info.StrLen_or_Ind);
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

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* original_params, bool skip_first)
{
#if PY_MAJOR_VERSION >= 3
    if (!PyUnicode_Check(pSql))
    {
        PyErr_SetString(PyExc_TypeError, "SQL must be a Unicode string");
        return false;
    }
#endif

    //
    // Normalize the parameter variables.
    //

    // Since we may replace parameters (we replace objects with Py_True/Py_False when writing to a bit/bool column),
    // allocate an array and use it instead of the original sequence

    int        params_offset = skip_first ? 1 : 0;
    Py_ssize_t cParams       = original_params == 0 ? 0 : PySequence_Length(original_params) - params_offset;

    //
    // Prepare the SQL if necessary.
    //

    if (pSql != cur->pPreparedSQL)
    {
        FreeParameterInfo(cur);

        SQLRETURN ret = 0;
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
        // PySequence_GetItem returns a *new* reference, which GetParameterInfo will take ownership of.  It is stored
        // in paramInfos and will be released in FreeInfos (which is always eventually called).

        PyObject* param = PySequence_GetItem(original_params, i + params_offset);
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

