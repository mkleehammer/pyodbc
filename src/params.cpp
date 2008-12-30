
#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "params.h"
#include "cursor.h"
#include "connection.h"
#include "buffer.h"
#include "wrapper.h"
#include "errors.h"
#include "dbspecific.h"

inline Connection* GetConnection(Cursor* cursor)
{
    return (Connection*)cursor->cnxn;
}

static int GetParamBufferSize(PyObject* param, Py_ssize_t iParam);
static bool BindParam(Cursor* cur, int iParam, PyObject* param, byte** ppbParam);
static SQLSMALLINT GetParamType(Cursor* cur, int iParam);

void FreeParameterData(Cursor* cur)
{
    // Unbinds the parameters and frees the parameter buffer.

    if (cur->paramdata)
    {
        Py_BEGIN_ALLOW_THREADS
        SQLFreeStmt(cur->hstmt, SQL_RESET_PARAMS);
        Py_END_ALLOW_THREADS
        free(cur->paramdata);
        cur->paramdata = 0;
    }
}

void FreeParameterInfo(Cursor* cur)
{
    // Internal function to free just the cached parameter information.  This is not used by the general cursor code
    // since this information is also freed in the less granular free_results function that clears everything.

    Py_XDECREF(cur->pPreparedSQL);
    free(cur->paramtypes);
    cur->pPreparedSQL = 0;
    cur->paramtypes   = 0;
    cur->paramcount   = 0;
}


struct ObjectArrayHolder
{
    Py_ssize_t count;
    PyObject** objs;
    ObjectArrayHolder(Py_ssize_t count, PyObject** objs)
    {
        this->count = count;
        this->objs  = objs;
    }
    ~ObjectArrayHolder()
    {
        for (Py_ssize_t i = 0; i < count; i++)
            Py_XDECREF(objs[i]);
        free(objs);
    }
};

bool PrepareAndBind(Cursor* cur, PyObject* pSql, PyObject* original_params, bool skip_first)
{
    //
    // Normalize the parameter variables.
    //

    // Since we may replace parameters (we replace objects with Py_True/Py_False when writing to a bit/bool column),
    // allocate an array and use it instead of the original sequence.  Since we don't change ownership we don't bother
    // with incref.  (That is, PySequence_GetItem will INCREF and ~ObjectArrayHolder will DECREF.)

    int        params_offset = skip_first ? 1 : 0;
    Py_ssize_t cParams       = original_params == 0 ? 0 : PySequence_Length(original_params) - params_offset;

    PyObject** params = (PyObject**)malloc(sizeof(PyObject*) * cParams);
    if (!params)
    {
        PyErr_NoMemory();
        return 0;
    }
    
    for (Py_ssize_t i = 0; i < cParams; i++)
        params[i] = PySequence_GetItem(original_params, i + params_offset);

    ObjectArrayHolder holder(cParams, params);

    //
    // Prepare the SQL if necessary.
    //

    if (pSql != cur->pPreparedSQL)
    {
        FreeParameterInfo(cur);

        SQLRETURN ret;
        SQLSMALLINT cParamsT = 0;
        const char* szErrorFunc = "SQLPrepare";
        if (PyString_Check(pSql))
        {
            Py_BEGIN_ALLOW_THREADS
            ret = SQLPrepare(cur->hstmt, (SQLCHAR*)PyString_AS_STRING(pSql), SQL_NTS);
            if (SQL_SUCCEEDED(ret))
            {
                szErrorFunc = "SQLNumParams";
                ret = SQLNumParams(cur->hstmt, &cParamsT);
            }
            Py_END_ALLOW_THREADS
        }
        else
        {
            Py_BEGIN_ALLOW_THREADS
            ret = SQLPrepareW(cur->hstmt, (SQLWCHAR*)PyUnicode_AsUnicode(pSql), SQL_NTS);
            if (SQL_SUCCEEDED(ret))
            {
                szErrorFunc = "SQLNumParams";
                ret = SQLNumParams(cur->hstmt, &cParamsT);
            }
            Py_END_ALLOW_THREADS
        }
  
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

    //
    // Convert parameters if necessary
    //

    // Calculate the amount of memory we need for param_buffer.  We can't allow it to reallocate on the fly since
    // we will bind directly into its memory.  (We only use a vector so its destructor will free the memory.)
    // We'll set aside one SQLLEN for each column to be used as the StrLen_or_IndPtr.

    int cb = 0;

    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        int cbT = GetParamBufferSize(params[i], i + 1) + sizeof(SQLLEN); // +1 to map to ODBC one-based index

        if (cbT < 0)
            return 0;

        cb += cbT;
    }

    cur->paramdata = reinterpret_cast<byte*>(malloc(cb));
    if (cur->paramdata == 0)
    {
        PyErr_NoMemory();
        return false;
    }

    // Bind each parameter.  If possible, items will be bound directly into the Python object.  Otherwise,
    // param_buffer will be used and ibNext will be updated.

    byte* pbParam = cur->paramdata;

    for (Py_ssize_t i = 0; i < cParams; i++)
    {
        if (!BindParam(cur, i + 1, params[i], &pbParam))
        {
            free(cur->paramdata);
            cur->paramdata = 0;
            return false;
        }
    }

    return true;
}

static SQLSMALLINT GetParamType(Cursor* cur, int iParam)
{
    // Returns the ODBC type of the of given parameter.
    //
    // Normally we set the parameter type based on the parameter's Python object type (e.g. str --> SQL_CHAR), so this
    // is only called when the parameter is None.  In that case, we can't guess the type and have to use
    // SQLDescribeParam.
    //
    // If the database doesn't support SQLDescribeParam, we return SQL_UNKNOWN_TYPE.

    if (!GetConnection(cur)->supports_describeparam || cur->paramcount == 0)
        return SQL_UNKNOWN_TYPE;

    if (cur->paramtypes == 0)
    {
        cur->paramtypes = reinterpret_cast<SQLSMALLINT*>(malloc(sizeof(SQLSMALLINT) * cur->paramcount));
        if (cur->paramtypes == 0)
        {
            // Out of memory.  We *could* define a new return type to indicate this, but since returning
            // SQL_UNKNOWN_TYPE is allowed, we'll do that instead.  Some other function higher up will eventually try
            // to allocate memory and fail, so we'll rely on that.
            
            return SQL_UNKNOWN_TYPE;
        }
            
        // SQL_UNKNOWN_TYPE is zero, so zero out all columns since we haven't looked any up yet.
        memset(cur->paramtypes, 0, sizeof(SQLSMALLINT) * cur->paramcount);
    }

    if (cur->paramtypes[iParam-1] == SQL_UNKNOWN_TYPE)
    {
        SQLUINTEGER ParameterSizePtr;
        SQLSMALLINT DecimalDigitsPtr;
        SQLSMALLINT NullablePtr;
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLDescribeParam(cur->hstmt, static_cast<SQLUSMALLINT>(iParam), &cur->paramtypes[iParam-1],
                               &ParameterSizePtr, &DecimalDigitsPtr, &NullablePtr);
        Py_END_ALLOW_THREADS

        // If this fails, the value will still be SQL_UNKNOWN_TYPE, so we drop out below and return it.
    }

    return cur->paramtypes[iParam-1];
}


static int GetParamBufferSize(PyObject* param, Py_ssize_t iParam)
{
    // Returns the size in bytes needed to hold the parameter in a format for binding, used to allocate the parameter
    // buffer.  (The value is not passed to ODBC.  Values passed to ODBC are in BindParam.)
    //
    // If we can bind directly into the Python object (e.g., using PyString_AsString), zero is returned since no extra
    // memory is required.  If the data will be provided at execution time (e.g. SQL_DATA_AT_EXEC), zero is returned
    // since the parameter value is not stored at all.  If the data type is not recognized, -1 is returned.

    if (param == Py_None)
        return 0;

    if (PyString_Check(param) || PyUnicode_Check(param))
        return 0;

    if (param == Py_True || param == Py_False)
        return 1;

    if (PyInt_Check(param))
        return sizeof(long int);

    if (PyLong_Check(param))
        return sizeof(INT64);

    if (PyFloat_Check(param))
        return sizeof(double);

    if (PyDecimal_Check(param))
    {
        // There isn't an efficient way of getting the precision, but it's there and it's obvious.

        Object digits = PyObject_GetAttrString(param, "_int");
        if (digits)
            return PySequence_Length(digits) + 3;  // sign, decimal, null
        
        // _int doesn't exist any more?
        return 42;
    }
    
    if (PyBuffer_Check(param))
    {
        // If the buffer has a single segment, we can bind directly to it, so we need 0 bytes.  Otherwise, we'll use
        // SQL_DATA_AT_EXEC, so we still need 0 bytes.
        return 0;
    }

    if (PyDateTime_Check(param))
        return sizeof(TIMESTAMP_STRUCT);

    if (PyDate_Check(param))
        return sizeof(DATE_STRUCT);

    if (PyTime_Check(param))
        return sizeof(TIME_STRUCT);

    RaiseErrorV("HY105", ProgrammingError, "Invalid parameter type.  param-index=%zd param-type=%s", iParam, param->ob_type->tp_name);

    return -1;
}

#ifdef TRACE_ALL
#define _MAKESTR(n) case n: return #n
static const char* SqlTypeName(SQLSMALLINT n)
{
    switch (n)
    {
        _MAKESTR(SQL_UNKNOWN_TYPE);
        _MAKESTR(SQL_CHAR);
        _MAKESTR(SQL_NUMERIC);
        _MAKESTR(SQL_DECIMAL);
        _MAKESTR(SQL_INTEGER);
        _MAKESTR(SQL_SMALLINT);
        _MAKESTR(SQL_FLOAT);
        _MAKESTR(SQL_REAL);
        _MAKESTR(SQL_DOUBLE);
        _MAKESTR(SQL_DATETIME);
        _MAKESTR(SQL_VARCHAR);
        _MAKESTR(SQL_TYPE_DATE);
        _MAKESTR(SQL_TYPE_TIME);
        _MAKESTR(SQL_TYPE_TIMESTAMP);
        _MAKESTR(SQL_SS_TIME2);
    }
    return "unknown";
}

static const char* CTypeName(SQLSMALLINT n)
{
    switch (n)
    {
        _MAKESTR(SQL_C_CHAR);
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

#endif

static bool BindParam(Cursor* cur, int iParam, PyObject* param, byte** ppbParam)
{
    // Called to bind a single parameter.
    //
    // iParam
    //   The one-based index of the parameter being bound.
    //
    // param
    //   The parameter to bind.
    //
    // ppbParam
    //   On entry, *ppbParam points to the memory available for binding the current parameter.  It should be
    //   incremented by the amount of memory used.
    //
    //   Each parameter saves room for a length-indicator.  If the Python object is not in a format that we can bind to
    //   directly, the memory immediately after the length indicator is used to copy the parameter data in a usable
    //   format.
    //
    //   The memory used is determined by the type of PyObject.  The total required is calculated, a buffer is
    //   allocated, and passed repeatedly to this function.  It is essential that the amount pre-calculated (from
    //   GetParamBufferSize) match the amount used by this function.  Any changes to either function must be
    //   coordinated.  (It might be wise to build a table.  I would do a lot more with assertions, but building a debug
    //   version of Python is a real pain.  It would be great if the Python for Windows team provided a pre-built
    //   version.)

    // When binding, ODBC requires 2 values: the column size and the buffer size.  The column size is related to the
    // destination (the SQL type of the column being written to) and the buffer size refers to the source (the size of
    // the C data being written).  If you send the wrong column size, data may be truncated.  For example, if you send
    // sizeof(TIMESTAMP_STRUCT) as the column size when writing a timestamp, it will be rounded to the nearest minute
    // because that is the precision that would fit into a string of that size.

    // Every parameter reserves space for a length-indicator.  Either set *pcbData to the actual input data length or
    // set pcbData to zero (not *pcbData) if you have a fixed-length parameter and don't need it.

    SQLLEN* pcbValue = reinterpret_cast<SQLLEN*>(*ppbParam);
    *ppbParam += sizeof(SQLLEN);

    // (I've made the parameter a pointer-to-a-pointer (ergo, the "pp") so that it is obvious at the call-site that we
    // are modifying it (&p).  Here we save a pointer into the buffer which we can compare to pbValue later to see if
    // we bound into the buffer (pbValue == pbParam) or bound directly into `param` (pbValue != pbParam).
    //
    // (This const means that the data the pointer points to is not const, you can change *pbParam, but the actual
    // pointer itself is.  We will be comparing the address to pbValue, not the contents.)

    byte* const pbParam = *ppbParam;

    SQLSMALLINT fCType        = 0;
    SQLSMALLINT fSqlType      = 0;
    SQLULEN     cbColDef      = 0;
    SQLSMALLINT decimalDigits = 0;
    SQLPOINTER  pbValue       = 0; // Set to the data to bind, either into `param` or set to pbParam.
    SQLLEN      cbValueMax    = 0;

    if (param == Py_None)
    {
        fSqlType  = GetParamType(cur, iParam); // First see if the driver can tell us the target type...
        if (fSqlType == SQL_UNKNOWN_TYPE)      // ... if it can't
            fSqlType =  SQL_VARCHAR;           // ... assume varchar (which fails on SQL Server binary fields)

        fCType    = SQL_C_DEFAULT;
        *pcbValue = SQL_NULL_DATA;
        cbColDef  = 1;
    }
    else if (PyString_Check(param))
    {
        char* pch = PyString_AS_STRING(param); 
        int   len = PyString_GET_SIZE(param);

        if (len <= MAX_VARCHAR_BUFFER)
        {
            fSqlType  = SQL_VARCHAR;
            fCType    = SQL_C_CHAR;
            pbValue  = pch;
            cbColDef  = max(len, 1);
            cbValueMax  = len + 1;
            *pcbValue = (SQLLEN)len;
        }
        else
        {
            fSqlType   = SQL_LONGVARCHAR;
            fCType     = SQL_C_CHAR;
            pbValue    = param;
            cbColDef   = max(len, 1);
            cbValueMax = sizeof(PyObject*);
            *pcbValue  = SQL_LEN_DATA_AT_EXEC((SQLLEN)len);
        }
    }
    else if (PyUnicode_Check(param))
    {
        Py_UNICODE* pch = PyUnicode_AsUnicode(param); 
        int      len = PyUnicode_GET_SIZE(param);

        if (len <= MAX_VARCHAR_BUFFER)
        {
            fSqlType   = SQL_WVARCHAR;
            fCType     = SQL_C_WCHAR;
            pbValue    = pch;
            cbColDef   = max(len, 1);
            cbValueMax = (len + 1) * Py_UNICODE_SIZE;
            *pcbValue  = (SQLLEN)(len * Py_UNICODE_SIZE);
        }
        else
        {
            fSqlType   = SQL_WLONGVARCHAR;
            fCType     = SQL_C_WCHAR;
            pbValue    = param;
            cbColDef   = max(len, 1) * sizeof(SQLWCHAR);
            cbValueMax = sizeof(PyObject*);
            *pcbValue  = SQL_LEN_DATA_AT_EXEC((SQLLEN)(len * Py_UNICODE_SIZE));
        }
    }
    else if (param == Py_True || param == Py_False)
    {
        *pbParam = (unsigned char)(param == Py_True ? 1 : 0);

        fSqlType = SQL_BIT;
        fCType   = SQL_C_BIT;
        pbValue = pbParam;
        cbValueMax = 1;
        pcbValue = 0;
    }
    else if (PyDateTime_Check(param))
    {
        TIMESTAMP_STRUCT* value = (TIMESTAMP_STRUCT*)pbParam;

        value->year   = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
        value->month  = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
        value->day    = (SQLUSMALLINT)PyDateTime_GET_DAY(param);
        value->hour   = (SQLUSMALLINT)PyDateTime_DATE_GET_HOUR(param);
        value->minute = (SQLUSMALLINT)PyDateTime_DATE_GET_MINUTE(param);
        value->second = (SQLUSMALLINT)PyDateTime_DATE_GET_SECOND(param);

        // SQL Server chokes if the fraction has more data than the database supports.  We expect other databases to be
        // the same, so we reduce the value to what the database supports.
        // http://support.microsoft.com/kb/263872

        int precision = ((Connection*)cur->cnxn)->datetime_precision - 20; // (20 includes a separating period)
        if (precision <= 0)
        {
            value->fraction = 0;
        }
        else
        {
            value->fraction = (SQLUINTEGER)(PyDateTime_DATE_GET_MICROSECOND(param) * 1000); // 1000 == micro -> nano
            
            // (How many leading digits do we want to keep?  With SQL Server 2005, this should be 3: 123000000)
            int keep = (int)pow(10.0, 9-min(9, precision));
            value->fraction = value->fraction / keep * keep;
            decimalDigits = (SQLSMALLINT)precision;
        }

        fSqlType = SQL_TIMESTAMP;
        fCType   = SQL_C_TIMESTAMP;
        pbValue = pbParam;
        cbColDef = ((Connection*)cur->cnxn)->datetime_precision;
        cbValueMax = sizeof(TIMESTAMP_STRUCT);
        pcbValue = 0;
    }
    else if (PyDate_Check(param))
    {
        DATE_STRUCT* value = (DATE_STRUCT*)pbParam;
        value->year  = (SQLSMALLINT) PyDateTime_GET_YEAR(param);
        value->month = (SQLUSMALLINT)PyDateTime_GET_MONTH(param);
        value->day   = (SQLUSMALLINT)PyDateTime_GET_DAY(param);

        fSqlType = SQL_TYPE_DATE;
        fCType   = SQL_C_TYPE_DATE;
        pbValue = pbParam;
        cbColDef = 10;           // The size of date represented as a string (yyyy-mm-dd)
        cbValueMax = sizeof(DATE_STRUCT);
        pcbValue = 0;
    }
    else if (PyTime_Check(param))
    {
        TIME_STRUCT* value = (TIME_STRUCT*)pbParam;
        value->hour   = (SQLUSMALLINT)PyDateTime_TIME_GET_HOUR(param);
        value->minute = (SQLUSMALLINT)PyDateTime_TIME_GET_MINUTE(param);
        value->second = (SQLUSMALLINT)PyDateTime_TIME_GET_SECOND(param);

        fSqlType = SQL_TYPE_TIME;
        fCType   = SQL_C_TIME;
        pbValue = pbParam;
        cbColDef = 8;
        cbValueMax = sizeof(TIME_STRUCT);
        pcbValue = 0;
    }
    else if (PyInt_Check(param))
    {
        long* value = (long*)pbParam;

        *value = PyInt_AsLong(param);

        fSqlType = SQL_INTEGER;
        fCType   = SQL_C_LONG;
        pbValue = pbParam;
        cbValueMax = sizeof(long);
        pcbValue = 0;
    }
    else if (PyLong_Check(param))
    {
        INT64* value = (INT64*)pbParam;

        *value = PyLong_AsLongLong(param);

        fSqlType = SQL_BIGINT;
        fCType   = SQL_C_SBIGINT;
        pbValue = pbParam;
        cbValueMax = sizeof(INT64);
        pcbValue = 0;
    }
    else if (PyFloat_Check(param))
    {
        double* value = (double*)pbParam;

        *value = PyFloat_AsDouble(param);

        fSqlType = SQL_DOUBLE;
        fCType   = SQL_C_DOUBLE;
        pbValue = pbParam;
        cbValueMax = sizeof(double);
        pcbValue = 0;
    }
    else if (PyDecimal_Check(param))
    {
        // Using the ODBC binary format would eliminate issues of whether to use '.' vs ',', but I've had unending
        // problems attemting to bind the decimal using the binary struct.  In particular, the scale is never honored
        // properly.  It appears that drivers have lots of bugs.  For now, we'll copy the value into a string, manually
        // change '.' to the database's decimal value.  (At this point, it appears that the decimal class *always* uses
        // '.', regardless of the user's locale.)

        // GetParamBufferSize reserved room for the string length, which may include a sign and decimal.

        Object str = PyObject_CallMethod(param, "__str__", 0);
        if (!str)
            return false;
         
        char* pch = PyString_AS_STRING(str.Get());
        int   len = PyString_GET_SIZE(str.Get());

        *pcbValue = (SQLLEN)len;
         
        // Note: SQL_DECIMAL here works for SQL Server but is not handled by MS Access.  SQL_NUMERIC seems to work for
        // both, probably because I am providing exactly NUMERIC(p,s) anyway.
        fSqlType = SQL_NUMERIC;
        fCType   = SQL_C_CHAR;
        pbValue = pbParam;
        cbColDef = len;
        memcpy(pbValue, pch, len + 1);
        cbValueMax = len + 1;

        char* pchDecimal = strchr((char*)pbValue, '.');
        if (pchDecimal)
        {
            decimalDigits = (SQLSMALLINT)(len - (pchDecimal - (char*)pbValue) - 1);

            if (chDecimal != '.')
                *pchDecimal = chDecimal; // (pointing into our own copy in pbValue)
        }
    }
    else if (PyBuffer_Check(param))
    {
        const char* pb;
        int cb = PyBuffer_GetMemory(param, &pb);

        if (cb != -1 && cb <= MAX_VARBINARY_BUFFER)
        {
            // There is one segment, so we can bind directly into the buffer object.

            fCType   = SQL_C_BINARY;
            fSqlType = SQL_VARBINARY;
    
            pbValue  = (SQLPOINTER)pb;
            cbValueMax  = cb;
            cbColDef  = max(cb, 1);
            *pcbValue = cb;
        }
        else
        {
            // There are multiple segments, so we'll provide the data at execution time.  Pass the PyObject pointer as
            // the parameter value which will be pased back to us when the data is needed.  (If we release threads, we
            // need to up the refcount!)

            fCType   = SQL_C_BINARY;
            fSqlType = SQL_LONGVARBINARY;

            pbValue  = param;
            cbColDef  = PyBuffer_Size(param);
            cbValueMax  = sizeof(PyObject*); // How big is pbValue; ODBC copies it and gives it back in SQLParamData
            *pcbValue = SQL_LEN_DATA_AT_EXEC(PyBuffer_Size(param));
        }
    }
    else
    {
        RaiseErrorV("HY097", NotSupportedError, "Python type %s not supported.  param=%d", param->ob_type->tp_name, iParam);
        return false;
    }

    #ifdef TRACE_ALL
    printf("BIND: param=%d fCType=%d (%s) fSqlType=%d (%s) cbColDef=%d DecimalDigits=%d cbValueMax=%d *pcb=%d\n", iParam,
           fCType, CTypeName(fCType), fSqlType, SqlTypeName(fSqlType), cbColDef, decimalDigits, cbValueMax, pcbValue ? *pcbValue : 0);
    #endif

    SQLRETURN ret = -1;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)iParam, SQL_PARAM_INPUT, fCType, fSqlType, cbColDef, decimalDigits, pbValue, cbValueMax, pcbValue);
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

    if (pbValue == pbParam)
    {
        // We are using the passed in buffer to bind; skip past the amount of buffer we used.
        *ppbParam += cbValueMax;
    }

    return true;
}
