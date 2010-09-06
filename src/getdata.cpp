
// The functions for reading a single value from the database using SQLGetData.  There is a different function for
// every data type.

#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "cursor.h"
#include "connection.h"
#include "errors.h"
#include "dbspecific.h"
#include "sqlwchar.h"

void GetData_init()
{
    PyDateTime_IMPORT;
}

class DataBuffer
{
    // Manages memory that GetDataString uses to read data in chunks.  We use the same function (GetDataString) to read
    // variable length data for 3 different types of data: binary, ANSI, and Unicode.  This class abstracts out the
    // memory management details to keep the function simple.
    //
    // There are 3 potential data buffer types we deal with in GetDataString:
    //
    //   1) Binary, which is a simple array of 8-bit bytes.
    //   2) ANSI text, which is an array of chars with a NULL terminator.
    //   3) Unicode text, which is an array of SQLWCHARs with a NULL terminator.
    //
    // When dealing with Unicode, there are two widths we have to be aware of: (1) SQLWCHAR and (2) Py_UNICODE.  If
    // these are the same we can use a PyUnicode object so we don't have to allocate our own buffer and then the
    // Unicode object.  If they are not the same (e.g. OS/X where wchar_t-->4 Py_UNICODE-->2) then we need to maintain
    // our own buffer and pass it to the PyUnicode object later.  Many Linux distros are now using UCS4, so Py_UNICODE
    // will be larger than SQLWCHAR.
    //
    // To reduce heap fragmentation, we perform the initial read into an array on the stack since we don't know the
    // length of the data.  If the data doesn't fit, this class then allocates new memory.  If the first read gives us
    // the length, then we create a Python object of the right size and read into its memory.

private:
    SQLSMALLINT dataType;

    char* buffer;
    Py_ssize_t bufferSize;      // How big is the buffer.
    int bytesUsed;              // How many elements have been read into the buffer?

    PyObject* bufferOwner;      // If possible, we bind into a PyString or PyUnicode object.
    int element_size;           // How wide is each character: ASCII/ANSI -> 1, Unicode -> 2 or 4, binary -> 1

    bool usingStack;            // Is buffer pointing to the initial stack buffer?

public:
    int null_size;              // How much room, in bytes, to add for null terminator: binary -> 0, other -> same as a element_size

    DataBuffer(SQLSMALLINT dataType, char* stackBuffer, SQLLEN stackBufferSize)
    {
        // dataType
        //   The type of data we will be reading: SQL_C_CHAR, SQL_C_WCHAR, or SQL_C_BINARY.

        this->dataType = dataType;

        element_size = (dataType == SQL_C_WCHAR)  ? sizeof(SQLWCHAR) : sizeof(char);
        null_size    = (dataType == SQL_C_BINARY) ? 0 : element_size;

        buffer        = stackBuffer;
        bufferSize    = stackBufferSize;
        usingStack    = true;
        bufferOwner   = 0;
        bytesUsed     = 0;
    }

    ~DataBuffer()
    {
        if (!usingStack)
        {
            if (bufferOwner)
            {
                Py_DECREF(bufferOwner);
            }
            else
            {
                pyodbc_free(buffer);
            }
        }
    }

    char* GetBuffer()
    {
        if (!buffer)
            return 0;

        return buffer + bytesUsed;
    }

    SQLLEN GetRemaining()
    {
        // Returns the amount of data remaining in the buffer, ready to be passed to SQLGetData.
        return bufferSize - bytesUsed;
    }

    void AddUsed(SQLLEN cbRead)
    {
        I(cbRead <= GetRemaining());
        bytesUsed += (int)cbRead;
    }

    bool AllocateMore(SQLLEN cbAdd)
    {
        // cbAdd
        //   The number of bytes (cb --> count of bytes) to add.

        if (cbAdd == 0)
            return true;

        SQLLEN newSize = bufferSize + cbAdd;

        if (usingStack)
        {
            // This is the first call and `buffer` points to stack memory.  Allocate a new object and copy the stack
            // data into it.

            char* stackBuffer = buffer;

            if (dataType == SQL_C_CHAR || dataType == SQL_C_BINARY)
            {
                bufferOwner = PyString_FromStringAndSize(0, newSize);
                buffer      = bufferOwner ? PyString_AS_STRING(bufferOwner) : 0;
            }
            else if (sizeof(SQLWCHAR) == Py_UNICODE_SIZE)
            {
                // Allocate directly into a Unicode object.
                bufferOwner = PyUnicode_FromUnicode(0, newSize / element_size);
                buffer      = bufferOwner ? (char*)PyUnicode_AsUnicode(bufferOwner) : 0;
            }
            else
            {
                // We're Unicode, but SQLWCHAR and Py_UNICODE don't match, so maintain our own SQLWCHAR buffer.
                buffer = (char*)pyodbc_malloc(newSize);
            }

            if (buffer == 0)
                return false;

            usingStack = false;

            memcpy(buffer, stackBuffer, bufferSize);
            bufferSize = newSize;
            return true;
        }

        if (PyString_CheckExact(bufferOwner))
        {
            if (_PyString_Resize(&bufferOwner, newSize) == -1)
                return false;
            buffer = PyString_AS_STRING(bufferOwner);
        }
        else if (PyUnicode_CheckExact(bufferOwner))
        {
            if (PyUnicode_Resize(&bufferOwner, newSize / element_size) == -1)
                return false;
            buffer = (char*)PyUnicode_AsUnicode(bufferOwner);
        }
        else
        {
            char* tmp = (char*)realloc(buffer, newSize);
            if (tmp == 0)
                return false;
            buffer = tmp;
        }

        bufferSize = newSize;

        return true;
    }

    PyObject* DetachValue()
    {
        // At this point, Trim should have been called by PostRead.

        if (bytesUsed == SQL_NULL_DATA || buffer == 0)
            Py_RETURN_NONE;

        if (usingStack)
        {
            if (dataType == SQL_C_CHAR || dataType == SQL_C_BINARY)
                return PyString_FromStringAndSize(buffer, bytesUsed);

            if (sizeof(SQLWCHAR) == Py_UNICODE_SIZE)
                return PyUnicode_FromUnicode((const Py_UNICODE*)buffer, bytesUsed / element_size);

            return PyUnicode_FromSQLWCHAR((const SQLWCHAR*)buffer, bytesUsed / element_size);
        }

        if (PyString_CheckExact(bufferOwner))
        {
            if (_PyString_Resize(&bufferOwner, bytesUsed) == -1)
                return 0;
            PyObject* tmp = bufferOwner;
            bufferOwner = 0;
            buffer      = 0;
            return tmp;
        }

        if (PyUnicode_CheckExact(bufferOwner))
        {
            if (PyUnicode_Resize(&bufferOwner, bytesUsed / element_size) == -1)
                return 0;
            PyObject* tmp = bufferOwner;
            bufferOwner = 0;
            buffer      = 0;
            return tmp;
        }

        // We have allocated our own SQLWCHAR buffer and must now copy it to a Unicode object.
        PyObject* result = PyUnicode_FromSQLWCHAR((const SQLWCHAR*)buffer, bytesUsed / element_size);
        if (result == 0)
            return false;
        pyodbc_free(buffer);
        buffer = 0;
        return result;
    }
};

static PyObject*
GetDataString(Cursor* cur, Py_ssize_t iCol)
{
    // Returns a String or Unicode object for character and binary data.

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

    ColumnInfo* pinfo = &cur->colinfos[iCol];

    // Some Unix ODBC drivers do not return the correct length.
    if (pinfo->sql_type == SQL_GUID)
        pinfo->column_size = 36;

    SQLSMALLINT nTargetType;

    switch (pinfo->sql_type)
    {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_GUID:
    case SQL_SS_XML:
        if (cur->cnxn->unicode_results)
            nTargetType  = SQL_C_WCHAR;
        else
            nTargetType  = SQL_C_CHAR;
        break;

    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
        nTargetType  = SQL_C_WCHAR;
        break;

    default:
        nTargetType  = SQL_C_BINARY;
        break;
    }

    char tempBuffer[1024];
    DataBuffer buffer(nTargetType, tempBuffer, sizeof(tempBuffer));

    for (int iDbg = 0; iDbg < 10; iDbg++) // failsafe
    {
        SQLRETURN ret;
        SQLLEN cbData = 0;

        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), nTargetType, buffer.GetBuffer(), buffer.GetRemaining(), &cbData);
        Py_END_ALLOW_THREADS;

        if (cbData == SQL_NULL_DATA)
            Py_RETURN_NONE;

        if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA)
            return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

        // The SQLGetData behavior is incredibly quirky.  It doesn't tell us the total, the total we've read, or even
        // the amount just read.  It returns the amount just read, plus any remaining.  Unfortunately, the only way to
        // pick them apart is to subtract out the amount of buffer we supplied.

        SQLLEN cbBuffer = buffer.GetRemaining(); // how much we gave SQLGetData

        if (ret == SQL_SUCCESS_WITH_INFO)
        {
            // There is more data than fits in the buffer.  The amount of data equals the amount of data in the buffer
            // minus a NULL terminator.

            SQLLEN cbRead;
            SQLLEN cbMore;

            if (cbData == SQL_NO_TOTAL)
            {
                // We don't know how much more, so just guess.
                cbRead = cbBuffer - buffer.null_size;
                cbMore = 2048;
            }
            else if (cbData >= cbBuffer)
            {
                // There is more data.  We supplied cbBuffer, but there was cbData (more).  We received cbBuffer, so we
                // need to subtract that, allocate enough to read the rest (cbData-cbBuffer).

                cbRead = cbBuffer - buffer.null_size;
                cbMore = cbData - cbRead;
            }
            else
            {
                // I'm not really sure why I would be here ... I would have expected SQL_SUCCESS
                cbRead = cbData - buffer.null_size;
                cbMore = 0;
            }

            buffer.AddUsed(cbRead);
            if (!buffer.AllocateMore(cbMore))
                return PyErr_NoMemory();
        }
        else if (ret == SQL_SUCCESS)
        {
            // For some reason, the NULL terminator is used in intermediate buffers but not in this final one.
            buffer.AddUsed(cbData);
        }

        if (ret == SQL_SUCCESS || ret == SQL_NO_DATA)
            return buffer.DetachValue();
    }

    // REVIEW: Add an error message.
    return 0;
}


static PyObject*
GetDataUser(Cursor* cur, Py_ssize_t iCol, int conv)
{
    // conv
    //   The index into the connection's user-defined conversions `conv_types`.

    PyObject* value = GetDataString(cur, iCol);
    if (value == 0)
        return 0;

    PyObject* result = PyObject_CallFunction(cur->cnxn->conv_funcs[conv], "(O)", value);
    Py_DECREF(value);
    return result;
}


static PyObject*
GetDataBuffer(Cursor* cur, Py_ssize_t iCol)
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

static PyObject*
GetDataDecimal(Cursor* cur, Py_ssize_t iCol)
{
    // The SQL_NUMERIC_STRUCT support is hopeless (SQL Server ignores scale on input parameters and output columns), so
    // we'll rely on the Decimal's string parsing.  Unfortunately, the Decimal author does not pay attention to the
    // locale, so we have to modify the string ourselves.
    //
    // Oracle inserts group separators (commas in US, periods in some countries), so leave room for that too.

    ColumnInfo* pinfo = &cur->colinfos[iCol];

    SQLLEN cbNeeded = pinfo->column_size + 3 +      // sign, decimal, NULL
                      (pinfo->column_size / 3) + 2; // grouping.  I believe this covers all cases.

    SQLLEN cbFetched = 0;
    char* sz = (char*)_alloca(cbNeeded);

    if (sz == 0)
        return PyErr_NoMemory();

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), SQL_C_CHAR, sz, cbNeeded, &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    // The decimal class requires the decimal to be a period and does not allow thousands separators.  Clean it up.
    //
    // Unfortunately this code only handles single-character values, which might be good enough for decimals and
    // separators, but is certainly not good enough for currency symbols.
    //
    // Note: cbFetched does not include the NULL terminator.

    for (int i = (int)(cbFetched - 1); i >=0; i--)
    {
        if (sz[i] == chGroupSeparator || sz[i] == '$' || sz[i] == chCurrencySymbol)
        {
            memmove(&sz[i], &sz[i] + 1, cbFetched - i);
            cbFetched--;
        }
        else if (sz[i] == chDecimal)
        {
            sz[i] = '.';
        }
    }

    return PyObject_CallFunction(decimal_type, "s", sz);
}

static PyObject*
GetDataBit(Cursor* cur, Py_ssize_t iCol)
{
    SQLCHAR ch;
    SQLLEN cbFetched;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), SQL_C_BIT, &ch, sizeof(ch), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    if (ch == SQL_TRUE)
        Py_RETURN_TRUE;

    Py_RETURN_FALSE;
}

static PyObject*
GetDataLong(Cursor* cur, Py_ssize_t iCol)
{
    ColumnInfo* pinfo = &cur->colinfos[iCol];

    long value = 0;
    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    SQLSMALLINT nCType = pinfo->is_unsigned ? SQL_C_ULONG : SQL_C_LONG;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), nCType, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

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
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), nCType, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    if (pinfo->is_unsigned)
        return PyLong_FromUnsignedLongLong((PY_LONG_LONG)value);

    return PyLong_FromLongLong((PY_LONG_LONG)value);
}

static PyObject*
GetDataDouble(Cursor* cur, Py_ssize_t iCol)
{
    double value;
    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), SQL_C_DOUBLE, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    return PyFloat_FromDouble(value);
}

static PyObject*
GetSqlServerTime(Cursor* cur, Py_ssize_t iCol)
{
    SQL_SS_TIME2_STRUCT value;

    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), SQL_C_BINARY, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    int micros = value.fraction / 1000; // nanos --> micros
    return PyTime_FromTime(value.hour, value.minute, value.second, micros);
}

static PyObject*
GetDataTimestamp(Cursor* cur, Py_ssize_t iCol)
{
    TIMESTAMP_STRUCT value;

    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLSMALLINT)(iCol+1), SQL_C_TYPE_TIMESTAMP, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    switch (cur->colinfos[iCol].sql_type)
    {
    case SQL_TYPE_TIME:
    {
        int micros = value.fraction / 1000; // nanos --> micros
        return PyTime_FromTime(value.hour, value.minute, value.second, micros);
    }

    case SQL_TYPE_DATE:
        return PyDate_FromDate(value.year, value.month, value.day);
    }

    int micros = value.fraction / 1000; // nanos --> micros
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


PyObject*
GetData(Cursor* cur, Py_ssize_t iCol)
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
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_GUID:
    case SQL_SS_XML:
        return GetDataString(cur, iCol);

    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return GetDataBuffer(cur, iCol);

    case SQL_DECIMAL:
    case SQL_NUMERIC:
    {
        if (decimal_type == 0)
            break;

        return GetDataDecimal(cur, iCol);
    }

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
