
// The functions for reading a single value from the database using SQLGetData.  There is a different function for
// every data type.

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "pyodbcmodule.h"
#include "cursor.h"
#include "connection.h"
#include "errors.h"
#include "dbspecific.h"
#include <datetime.h>

// NULL terminator notes:
//
//  * pinfo->column_size, from SQLDescribeCol, does not include a NULL terminator.  For example, column_size for a
//    char(10) column would be 10.  (Also, when dealing with SQLWCHAR, it is the number of *characters*, not bytes.)
//
//  * When passing a length to PyString_FromStringAndSize and similar Unicode functions, do not add the NULL
//    terminator -- it will be added automatically.  See objects/stringobject.c
//
//  * SQLGetData does not return the NULL terminator in the length indicator.  (Therefore, you can pass this value
//    directly to the Python string functions.)
//
//  * SQLGetData will write a NULL terminator in the output buffer, so you must leave room for it.  You must also
//    include the NULL terminator in the buffer length passed to SQLGetData.
//
// ODBC generalization:
//  1) Include NULL terminators in input buffer lengths.
//  2) NULL terminators are not used in data lengths.

void GetData_init()
{
    PyDateTime_IMPORT;
}

static byte* ReallocOrFreeBuffer(byte* pb, Py_ssize_t cbNeed);

inline bool IsBinaryType(SQLSMALLINT sqltype)
{
    // Is this SQL type (e.g. SQL_VARBINARY) a binary type or not?
    switch (sqltype)
    {
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return true;
    }
    return false;
}

inline bool IsWideType(SQLSMALLINT sqltype)
{
    switch (sqltype)
    {
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_SS_XML:
    case SQL_DB2_XML:
        return true;
    }
    return false;
}

// TODO: Wont pyodbc_free crash if we didn't use pyodbc_realloc.

static bool ReadVarColumn(Cursor* cur, Py_ssize_t iCol, SQLSMALLINT ctype, bool& isNull, byte*& pbResult, Py_ssize_t& cbResult)
{
    // Called to read a variable-length column and return its data in a newly-allocated heap
    // buffer.
    //
    // Returns true if the read was successful and false if the read failed.  If the read
    // failed a Python exception will have been set.
    //
    // If a non-null and non-empty value was read, pbResult will be set to a buffer containing
    // the data and cbResult will be set to the byte length.  This length does *not* include a
    // null terminator.  In this case the data *must* be freed using pyodbc_free.
    //
    // If a null value was read, isNull is set to true and pbResult and cbResult will be set to
    // 0.
    //
    // If a zero-length value was read, isNull is set to false and pbResult and cbResult will
    // be set to 0.

    isNull   = false;
    pbResult = 0;
    cbResult = 0;

    const Py_ssize_t cbElement = (Py_ssize_t)(IsWideType(ctype) ? sizeof(ODBCCHAR) : 1);
    const Py_ssize_t cbNullTerminator = IsBinaryType(ctype) ? 0 : cbElement;

    // TODO: Make the initial allocation size configurable?
    Py_ssize_t cbAllocated = 4096;
    Py_ssize_t cbUsed = 0;
    byte* pb = (byte*)malloc((size_t)cbAllocated);
    if (!pb)
    {
        PyErr_NoMemory();
        return false;
    }

    SQLRETURN ret = SQL_SUCCESS_WITH_INFO;

    do
    {
        // Call SQLGetData in a loop as long as it keeps returning partial data (ret ==
        // SQL_SUCCESS_WITH_INFO).  Each time through, update the buffer pb, cbAllocated, and
        // cbUsed.

        Py_ssize_t cbAvailable = cbAllocated - cbUsed;
        SQLLEN cbData = 0;

        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), ctype, &pb[cbUsed], (SQLLEN)cbAvailable, &cbData);
        Py_END_ALLOW_THREADS;

        TRACE("ReadVarColumn: SQLGetData avail=%d --> ret=%d cbData=%d\n", (int)cbAvailable, (int)ret, (int)cbData);

        if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA)
        {
            RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);
            return false;
        }

        if (ret == SQL_SUCCESS && (int)cbData < 0)
        {
            // HACK: FreeTDS 0.91 on OS/X returns -4 for NULL data instead of SQL_NULL_DATA
            // (-1).  I've traced into the code and it appears to be the result of assigning -1
            // to a SQLLEN.  We are going to treat all negative values as NULL.
            ret = SQL_NULL_DATA;
            cbData = 0;
        }

        // SQLGetData behavior is incredibly quirky: It doesn't tell us the total, the total
        // we've read, or even the amount just read.  It returns the amount just read, plus any
        // remaining.  Unfortunately, the only way to pick them apart is to subtract out the
        // amount of buffer we supplied.

        if (ret == SQL_SUCCESS_WITH_INFO)
        {
            // This means we read some data, but there is more.  SQLGetData is very weird - it
            // sets cbRead to the number of bytes we read *plus* the amount remaining.

            Py_ssize_t cbRemaining = 0; // How many more bytes do we need to allocate, not including null?
            Py_ssize_t cbRead = 0; // How much did we just read, not including null?

            if (cbData == SQL_NO_TOTAL)
            {
                // This special value indicates there is more data but the driver can't tell us
                // how much more, so we'll just add whatever we want and try again.  It also
                // tells us, however, that the buffer is full, so the amount we read equals the
                // amount we offered.  Remember that if the type requires a null terminator, it
                // will be added *every* time, not just at the end, so we need to subtract it.

                cbRead = (cbAvailable - cbNullTerminator);
                cbRemaining = 1024 * 1024;
            }
            else if ((Py_ssize_t)cbData >= cbAvailable)
            {
                // We offered cbAvailable space, but there was cbData data.  The driver filled
                // the buffer with what it could.  Remember that if the type requires a null
                // terminator, the driver is going to append one on *every* read, so we need to
                // subtract them out.  At least we know the exact data amount now and we can
                // allocate a precise amount.

                cbRead = (cbAvailable - cbNullTerminator);
                cbRemaining = cbData - cbRead;
            }
            else
            {
                // I would not expect to get here - we apparently read all of the data but the
                // driver did not return SQL_SUCCESS?
                cbRead = (cbData - cbNullTerminator);
                cbRemaining = 0;
            }

            cbUsed += cbRead;

            if (cbRemaining > 0)
            {
                // This is a tiny bit complicated by the fact that the data is null terminated,
                // meaning we haven't actually used up the entire buffer (cbAllocated), only
                // cbUsed (which should be cbAllocated - cbNullTerminator).
                Py_ssize_t cbNeed = cbUsed + cbRemaining + cbNullTerminator;
                pb = ReallocOrFreeBuffer(pb, cbNeed);
                if (!pb)
                    return false;
                cbAllocated = cbNeed;
            }
        }
        else if (ret == SQL_SUCCESS)
        {
            // We read some data and this is the last batch (so we'll drop out of the
            // loop).
            //
            // If I'm reading the documentation correctly, SQLGetData is not going to
            // include the null terminator in cbRead.

            cbUsed += cbData;
        }
    }
    while (ret == SQL_SUCCESS_WITH_INFO);

    isNull = (ret == SQL_NULL_DATA);

    if (!isNull && cbUsed > 0)
    {
        pbResult = pb;
        cbResult = cbUsed;
    }
    else
    {
        pyodbc_free(pb);
    }

    return true;
}

static byte* ReallocOrFreeBuffer(byte* pb, Py_ssize_t cbNeed)
{
    // Attempts to reallocate `pb` to size `cbNeed`.  If the realloc fails, the original memory
    // is freed, a memory exception is set, and 0 is returned.  Otherwise the new pointer is
    // returned.

    byte* pbNew = (byte*)realloc(pb, (size_t)cbNeed);
    if (pbNew == 0)
    {
        pyodbc_free(pb);
        PyErr_NoMemory();
        return 0;
    }
    return pbNew;
}


static PyObject* GetText(Cursor* cur, Py_ssize_t iCol)
{
    // We are reading one of the SQL_WCHAR, SQL_WVARCHAR, etc., and will return
    // a string.
    //
    // If there is no configuration we would expect this to be UTF-16 encoded data.  (If no
    // byte-order-mark, we would expect it to be big-endian.)
    //
    // Now, just because the driver is telling us it is wide data doesn't mean it is true.
    // psqlodbc with UTF-8 will tell us it is wide data but you must ask for single-byte.
    // (Otherwise it is just UTF-8 with each character stored as 2 bytes.)  That's why we allow
    // the user to configure.

    ColumnInfo* pinfo = &cur->colinfos[iCol];
    const TextEnc& enc = IsWideType(pinfo->sql_type) ? cur->cnxn->sqlwchar_enc : cur->cnxn->sqlchar_enc;

    bool isNull = false;
    byte* pbData = 0;
    Py_ssize_t cbData = 0;
    if (!ReadVarColumn(cur, iCol, enc.ctype, isNull, pbData, cbData))
        return 0;

    if (isNull)
    {
        I(pbData == 0 && cbData == 0);
        Py_RETURN_NONE;
    }

    PyObject* result = TextBufferToObject(enc, pbData, cbData);

    pyodbc_free(pbData);

    return result;
}



static PyObject* GetBinary(Cursor* cur, Py_ssize_t iCol)
{
    // Reads SQL_BINARY.

    bool isNull = false;
    byte* pbData = 0;
    Py_ssize_t cbData = 0;
    if (!ReadVarColumn(cur, iCol, SQL_C_BINARY, isNull, pbData, cbData))
        return 0;

    if (isNull)
    {
        I(pbData == 0 && cbData == 0);
        Py_RETURN_NONE;
    }

    PyObject* obj;
#if PY_MAJOR_VERSION >= 3
    obj = PyBytes_FromStringAndSize((char*)pbData, cbData);
#else
    obj = PyByteArray_FromStringAndSize((char*)pbData, cbData);
#endif
    pyodbc_free(pbData);
    return obj;
}


static PyObject* GetDataUser(Cursor* cur, Py_ssize_t iCol, int conv)
{
    // conv
    //   The index into the connection's user-defined conversions `conv_types`.

    bool isNull = false;
    byte* pbData = 0;
    Py_ssize_t cbData = 0;
    if (!ReadVarColumn(cur, iCol, SQL_C_BINARY, isNull, pbData, cbData))
        return 0;

    if (isNull)
    {
        I(pbData == 0 && cbData == 0);
        Py_RETURN_NONE;
    }

    PyObject* value = PyBytes_FromStringAndSize((char*)pbData, cbData);
    pyodbc_free(pbData);
    if (!value)
        return 0;

    PyObject* result = PyObject_CallFunction(cur->cnxn->conv_funcs[conv], "(O)", value);
    Py_DECREF(value);
    if (!result)
        return 0;

    return result;
}


#if PY_VERSION_HEX < 0x02060000
static PyObject* GetDataBuffer(Cursor* cur, Py_ssize_t iCol)
{
    PyObject* str = GetDataString(cur, iCol);

    if (str == Py_None)
        return str;

    PyObject* buffer = 0;

    if (str)
    {
        buffer = PyBuffer_FromObject(str, 0, PyString_GET_SIZE(str));
        Py_DECREF(str);         // If no buffer, release it.  If buffer, the buffer owns it.
    }

    return buffer;
}
#endif

static PyObject* GetDataDecimal(Cursor* cur, Py_ssize_t iCol)
{
    // The SQL_NUMERIC_STRUCT support is hopeless (SQL Server ignores scale on input parameters and output columns,
    // Oracle does something else weird, and many drivers don't support it at all), so we'll rely on the Decimal's
    // string parsing.  Unfortunately, the Decimal author does not pay attention to the locale, so we have to modify
    // the string ourselves.
    //
    // Oracle inserts group separators (commas in US, periods in some countries), so leave room for that too.
    //
    // Some databases support a 'money' type which also inserts currency symbols.  Since we don't want to keep track of
    // all these, we'll ignore all characters we don't recognize.  We will look for digits, negative sign (which I hope
    // is universal), and a decimal point ('.' or ',' usually).  We'll do everything as Unicode in case currencies,
    // etc. are too far out.

    const TextEnc& enc = cur->cnxn->sqlwchar_enc;
    // I'm going to request the data as Unicode in case there is a weird currency symbol.  If
    // this is a performance problems we may want a flag on this.

    bool isNull = false;
    byte* pbData = 0;
    Py_ssize_t cbData = 0;
    if (!ReadVarColumn(cur, iCol, enc.ctype, isNull, pbData, cbData))
        return 0;

    if (isNull)
    {
        I(pbData == 0 && cbData == 0);
        Py_RETURN_NONE;
    }

    Object result(TextBufferToObject(enc, pbData, cbData));

    pyodbc_free(pbData);

    if (!result)
        return 0;

    // Remove non-digits and convert the databases decimal to a '.' (required by decimal ctor).
    //
    // We are assuming that the decimal point and digits fit within the size of ODBCCHAR.

    // If Unicode, convert to UTF-8 and copy the digits and punctuation out.  Since these are
    // all ASCII characters, we can ignore any multiple-byte characters.  Fortunately, if a
    // character is multi-byte all bytes will have the high bit set.

    char* pch;
    Py_ssize_t cch;

#if PY_MAJOR_VERSION >= 3
    if (PyUnicode_Check(result))
    {
        pch = (char*)PyUnicode_AsUTF8AndSize(result, &cch);
    }
    else
    {
        int n = PyBytes_AsStringAndSize(result, &pch, &cch);
        if (n < 0)
            pch = 0;
    }
#else
    Object encoded;
    if (PyUnicode_Check(result))
    {
        encoded = PyUnicode_AsUTF8String(result);
        if (!encoded)
            return 0;
        result = encoded.Detach();
    }
    int n = PyString_AsStringAndSize(result, &pch, &cch);
    if (n < 0)
        pch = 0;
#endif

    if (!pch)
        return 0;

    // TODO: Why is this limited to 100?  Also, can we perform a check on the original and use
    // it as-is?
    char ascii[100];
    size_t asciilen = 0;

    const char* pchMax = pch + cch;
    while (pch < pchMax)
    {
        if ((*pch & 0x80) == 0)
        {
            if (*pch == chDecimal)
            {
                // Must force it to use '.' since the Decimal class doesn't pay attention to the locale.
                ascii[asciilen++] = '.';
            }
            else if ((*pch >= '0' && *pch <= '9') || *pch == '-')
            {
                ascii[asciilen++] = (char)(*pch);
            }
        }
        pch++;
    }

    ascii[asciilen] = 0;

    Object str(PyString_FromStringAndSize(ascii, (Py_ssize_t)asciilen));
    if (!str)
        return 0;
    PyObject* decimal_type = GetClassForThread("decimal", "Decimal");
    if (!decimal_type)
        return 0;
    PyObject* decimal = PyObject_CallFunction(decimal_type, "O", str.Get());
    Py_DECREF(decimal_type);
    return decimal;
}

static PyObject* GetDataBit(Cursor* cur, Py_ssize_t iCol)
{
    SQLCHAR ch;
    SQLLEN cbFetched;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_BIT, &ch, sizeof(ch), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    if (ch == SQL_TRUE)
        Py_RETURN_TRUE;

    Py_RETURN_FALSE;
}


static PyObject* GetDataLong(Cursor* cur, Py_ssize_t iCol)
{
    ColumnInfo* pinfo = &cur->colinfos[iCol];

    SQLINTEGER value;
    SQLLEN cbFetched;
    SQLRETURN ret;

    SQLSMALLINT nCType = pinfo->is_unsigned ? SQL_C_ULONG : SQL_C_LONG;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), nCType, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    if (pinfo->is_unsigned)
        return PyInt_FromLong(*(SQLINTEGER*)&value);

    return PyInt_FromLong(value);
}


static PyObject* GetDataLongLong(Cursor* cur, Py_ssize_t iCol)
{
    ColumnInfo* pinfo = &cur->colinfos[iCol];

    SQLSMALLINT nCType = pinfo->is_unsigned ? SQL_C_UBIGINT : SQL_C_SBIGINT;
    SQLBIGINT   value;
    SQLLEN      cbFetched;
    SQLRETURN   ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), nCType, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    if (pinfo->is_unsigned)
        return PyLong_FromUnsignedLongLong((unsigned PY_LONG_LONG)(SQLUBIGINT)value);

    return PyLong_FromLongLong((PY_LONG_LONG)value);
}


static PyObject* GetDataDouble(Cursor* cur, Py_ssize_t iCol)
{
    double value;
    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_DOUBLE, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    return PyFloat_FromDouble(value);
}


static PyObject* GetSqlServerTime(Cursor* cur, Py_ssize_t iCol)
{
    SQL_SS_TIME2_STRUCT value;

    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_BINARY, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    int micros = (int)(value.fraction / 1000); // nanos --> micros
    return PyTime_FromTime(value.hour, value.minute, value.second, micros);
}

static PyObject* GetUUID(Cursor* cur, Py_ssize_t iCol)
{
    // REVIEW: Since GUID is a fixed size, do we need to pass the size or cbFetched?

    PYSQLGUID guid;
    SQLLEN cbFetched = 0;
    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_GUID, &guid, sizeof(guid), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

#if PY_MAJOR_VERSION >= 3
    const char* szFmt = "(yyy#)";
#else
    const char* szFmt = "(sss#)";
#endif
    Object args(Py_BuildValue(szFmt, NULL, NULL, &guid, (int)sizeof(guid)));
    if (!args)
        return 0;

    PyObject* uuid_type = GetClassForThread("uuid", "UUID");
    if (!uuid_type)
        return 0;
    PyObject* uuid = PyObject_CallObject(uuid_type, args.Get());
    Py_DECREF(uuid_type);
    return uuid;
}

static PyObject* GetDataTimestamp(Cursor* cur, Py_ssize_t iCol)
{
    TIMESTAMP_STRUCT value;

    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_TYPE_TIMESTAMP, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    switch (cur->colinfos[iCol].sql_type)
    {
    case SQL_TYPE_TIME:
    {
        int micros = (int)(value.fraction / 1000); // nanos --> micros
        return PyTime_FromTime(value.hour, value.minute, value.second, micros);
    }

    case SQL_TYPE_DATE:
        return PyDate_FromDate(value.year, value.month, value.day);
    }

    int micros = (int)(value.fraction / 1000); // nanos --> micros
    return PyDateTime_FromDateAndTime(value.year, value.month, value.day, value.hour, value.minute, value.second, micros);
}


int GetUserConvIndex(Cursor* cur, SQLSMALLINT sql_type)
{
    // If this sql type has a user-defined conversion, the index into the connection's `conv_funcs` array is returned.
    // Otherwise -1 is returned.

    for (int i = 0; i < cur->cnxn->conv_count; i++)
        if (cur->cnxn->conv_types[i] == sql_type)
            return i;
    return -1;
}


PyObject* PythonTypeFromSqlType(Cursor* cur, SQLSMALLINT type)
{
    // Returns a type object ('int', 'str', etc.) for the given ODBC C type.  This is used to populate
    // Cursor.description with the type of Python object that will be returned for each column.
    //
    // type
    //   The ODBC C type (SQL_C_CHAR, etc.) of the column.
    //
    // The returned object does not have its reference count incremented (is a borrowed
    // reference).
    //
    // Keep this in sync with GetData below.

    int conv_index = GetUserConvIndex(cur, type);
    if (conv_index != -1)
        return (PyObject*)&PyString_Type;

    PyObject* pytype = 0;
    bool incref = true;

    switch (type)
    {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
#if PY_MAJOR_VERSION < 3
        if (cur->cnxn->str_enc.ctype == SQL_C_CHAR)
            pytype = (PyObject*)&PyString_Type;
        else
            pytype = (PyObject*)&PyUnicode_Type;
#else
        pytype = (PyObject*)&PyUnicode_Type;
#endif
        break;

    case SQL_GUID:
        if (UseNativeUUID())
        {
            pytype = GetClassForThread("uuid", "UUID");
            incref = false;
        }
        else
        {
#if PY_MAJOR_VERSION < 3
            if (cur->cnxn->str_enc.ctype == SQL_C_CHAR)
                pytype = (PyObject*)&PyString_Type;
            else
                pytype = (PyObject*)&PyUnicode_Type;
#else
            pytype = (PyObject*)&PyUnicode_Type;
#endif
        }
        break;

    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_SS_XML:
    case SQL_DB2_XML:
        pytype = (PyObject*)&PyUnicode_Type;
        break;

    case SQL_DECIMAL:
    case SQL_NUMERIC:
        pytype = GetClassForThread("decimal", "Decimal");
        incref = false;
        break;

    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        pytype = (PyObject*)&PyFloat_Type;
        break;

    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_TINYINT:
        pytype = (PyObject*)&PyInt_Type;
        break;

    case SQL_TYPE_DATE:
        pytype = (PyObject*)PyDateTimeAPI->DateType;
        break;

    case SQL_TYPE_TIME:
    case SQL_SS_TIME2:          // SQL Server 2008+
        pytype = (PyObject*)PyDateTimeAPI->TimeType;
        break;

    case SQL_TYPE_TIMESTAMP:
        pytype = (PyObject*)PyDateTimeAPI->DateTimeType;
        break;

    case SQL_BIGINT:
        pytype = (PyObject*)&PyLong_Type;
        break;

    case SQL_BIT:
        pytype = (PyObject*)&PyBool_Type;
        break;

    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
    default:
#if PY_VERSION_HEX >= 0x02060000
        pytype = (PyObject*)&PyByteArray_Type;
#else
        pytype = (PyObject*)&PyBuffer_Type;
#endif
        break;
    }

    if (pytype && incref)
        Py_INCREF(pytype);
    return pytype;
}

PyObject* GetData(Cursor* cur, Py_ssize_t iCol)
{
    // Returns an object representing the value in the row/field.  If 0 is returned, an exception has already been set.
    //
    // The data is assumed to be the default C type for the column's SQL type.

    ColumnInfo* pinfo = &cur->colinfos[iCol];

    // First see if there is a user-defined conversion.

    int conv_index = GetUserConvIndex(cur, pinfo->sql_type);
    if (conv_index != -1)
        return GetDataUser(cur, iCol, conv_index);

    switch (pinfo->sql_type)
    {
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
        return GetText(cur, iCol);

    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_SS_XML:
    case SQL_DB2_XML:
        return GetText(cur, iCol);

    case SQL_GUID:
        if (UseNativeUUID())
            return GetUUID(cur, iCol);
        return GetText(cur, iCol);
        break;

    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return GetBinary(cur, iCol);

    case SQL_DECIMAL:
    case SQL_NUMERIC:
    case SQL_DB2_DECFLOAT:
        return GetDataDecimal(cur, iCol);

    case SQL_BIT:
        return GetDataBit(cur, iCol);

    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
        return GetDataLong(cur, iCol);

    case SQL_BIGINT:
        return GetDataLongLong(cur, iCol);

    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        return GetDataDouble(cur, iCol);


    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
        return GetDataTimestamp(cur, iCol);

    case SQL_SS_TIME2:
        return GetSqlServerTime(cur, iCol);
    }

    return RaiseErrorV("HY106", ProgrammingError, "ODBC SQL type %d is not yet supported.  column-index=%zd  type=%d",
                       (int)pinfo->sql_type, iCol, (int)pinfo->sql_type);
}
