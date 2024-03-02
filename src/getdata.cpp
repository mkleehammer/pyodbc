
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
#include "decimal.h"
#include <time.h>
#include <datetime.h>

// NULL terminator notes:
//
//  * pinfo->column_size, from SQLDescribeCol, does not include a NULL terminator.  For example, column_size for a
//    char(10) column would be 10.  (Also, when dealing with SQLWCHAR, it is the number of *characters*, not bytes.)
//
//  * When passing a length to PyUnicode_FromStringAndSize and similar Unicode functions, do not add the NULL
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


static bool ReadVarColumn(Cursor* cur, Py_ssize_t iCol, SQLSMALLINT ctype, bool* isNull, void** pbResult, SQLLEN* cbResult)
{
    // Called to read a variable-length column and return its data in a newly-allocated heap
    // buffer.
    //
    // Returns true if the read was successful and false if the read failed.  If the read
    // failed a Python exception will have been set.
    //
    // If a non-null and non-empty value was read, pbResult will be set to a buffer containing
    // the data and cbResult will be set to the byte length.  This length does *not* include a
    // null terminator.  In this case the data *must* be freed using PyMem_Free.
    //
    // If a null value was read, isNull is set to true and pbResult and cbResult will be set to
    // 0.
    //
    // If a zero-length value was read, isNull is set to false and pbResult and cbResult will
    // be set to 0.

    *isNull   = false;
    *pbResult = 0;
    *cbResult = 0;

    const SQLLEN cbElement = (SQLLEN)(IsWideType(ctype) ? sizeof(uint16_t) : 1);
    const SQLLEN cbNullTerminator = IsBinaryType(ctype) ? 0 : cbElement;

    // TODO: Make the initial allocation size configurable?
    SQLLEN cbAllocated = 4096;
    SQLLEN cbUsed = 0;
    byte* pb = (byte*)PyMem_Malloc((size_t)cbAllocated);
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

        SQLLEN cbAvailable = cbAllocated - cbUsed;
        SQLLEN cbData = 0;

        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), ctype, &pb[cbUsed], cbAvailable, &cbData);
        Py_END_ALLOW_THREADS;

        TRACE("ReadVarColumn: SQLGetData avail=%d --> ret=%d cbData=%d\n", (int)cbAvailable, (int)ret, (int)cbData);

        if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA)
        {
            RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);
            return false;
        }

        if (ret == SQL_SUCCESS && cbData < 0)
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

            SQLLEN cbRemaining = 0; // How many more bytes do we need to allocate, not including null?
            SQLLEN cbRead = 0; // How much did we just read, not including null?

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
            else if (cbData >= cbAvailable)
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

            TRACE("Memory Need: cbRemaining=%ld cbRead=%ld\n", (long)cbRemaining, (long)cbRead);

            if (cbRemaining > 0)
            {
                // This is a tiny bit complicated by the fact that the data is null terminated,
                // meaning we haven't actually used up the entire buffer (cbAllocated), only
                // cbUsed (which should be cbAllocated - cbNullTerminator).
                SQLLEN cbNeed = cbUsed + cbRemaining + cbNullTerminator;
                pb = ReallocOrFreeBuffer(pb, (Py_ssize_t)cbNeed);
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

    *isNull = (ret == SQL_NULL_DATA);

    if (!*isNull && cbUsed > 0)
    {
        *pbResult = pb;
        *cbResult = cbUsed;
    }
    else
    {
        PyMem_Free(pb);
    }

    return true;
}

static byte* ReallocOrFreeBuffer(byte* pb, Py_ssize_t cbNeed)
{
    // Attempts to reallocate `pb` to size `cbNeed`.  If the realloc fails, the original memory
    // is freed, a memory exception is set, and 0 is returned.  Otherwise the new pointer is
    // returned.

    byte* pbNew = (byte*)PyMem_Realloc(pb, (size_t)cbNeed);
    if (pbNew == 0)
    {
        PyMem_Free(pb);
        PyErr_NoMemory();
        return 0;
    }
    return pbNew;
}


static PyObject* GetText(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
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

    return TextBufferToObject(enc, (byte*)buffer, (Py_ssize_t)cbFetched);
}


static PyObject* GetBinary(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    // Reads SQL_BINARY.

    return PyBytes_FromStringAndSize((char*)buffer, (Py_ssize_t)cbFetched);
}


static PyObject* GetDataUser(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    PyObject* value = PyBytes_FromStringAndSize((char*)buffer, (Py_ssize_t)cbFetched);
    if (!value)
        return 0;

    PyObject* result = PyObject_CallFunction(converter, "(O)", value);
    Py_DECREF(value);
    if (!result)
        return 0;

    return result;
}


static PyObject* GetDataDecimal(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    // The SQL_NUMERIC_STRUCT support is hopeless (SQL Server ignores scale on input parameters
    // and output columns, Oracle does something else weird, and many drivers don't support it
    // at all), so we'll rely on the Decimal's string parsing.  Unfortunately, the Decimal
    // author does not pay attention to the locale, so we have to modify the string ourselves.
    //
    // Oracle inserts group separators (commas in US, periods in some countries), so leave room
    // for that too.
    //
    // Some databases support a 'money' type which also inserts currency symbols.  Since we
    // don't want to keep track of all these, we'll ignore all characters we don't recognize.
    // We will look for digits, negative sign (which I hope is universal), and a decimal point
    // ('.' or ',' usually).  We'll do everything as Unicode in case currencies, etc. are too
    // far out.
    //
    // This seems very inefficient.  We know the characters we are interested in are ASCII
    // since they are -, ., and 0-9.  There /could/ be a Unicode currency symbol, but I'm going
    // to ignore that for right now.  Therefore if we ask for the data in SQLCHAR, it should be
    // ASCII even if the encoding is UTF-8.

    // I'm going to request the data as Unicode in case there is a weird currency symbol.  If
    // this is a performance problems we may want a flag on this.

    Object result(DecimalFromText(enc, (byte*)buffer, (Py_ssize_t)cbFetched));
    return result.Detach();
}

static PyObject* GetDataBit(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    if (*(SQLCHAR*)buffer == SQL_TRUE)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}


static PyObject* GetDataULong(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    SQLINTEGER value = *(SQLINTEGER*)buffer;
    return PyLong_FromLong(*(SQLINTEGER*)&value);
}


static PyObject* GetDataLong(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    SQLINTEGER value = *(SQLINTEGER*)buffer;
    return PyLong_FromLong(value);
}


static PyObject* GetDataULongLong(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    SQLBIGINT   value = *(SQLBIGINT*)buffer;
    return PyLong_FromUnsignedLongLong((unsigned PY_LONG_LONG)(SQLUBIGINT)value);
}


static PyObject* GetDataLongLong(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    SQLBIGINT   value = *(SQLBIGINT*)buffer;
    return PyLong_FromLongLong((PY_LONG_LONG)value);
}


static PyObject* GetDataDouble(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    double value = *(double*)buffer;
    return PyFloat_FromDouble(value);
}


static PyObject* GetSqlServerTime(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    SQL_SS_TIME2_STRUCT value = *(SQL_SS_TIME2_STRUCT*)buffer;
    int micros = (int)(value.fraction / 1000); // nanos --> micros
    return PyTime_FromTime(value.hour, value.minute, value.second, micros);
}


static PyObject* GetUUID(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    // REVIEW: Since GUID is a fixed size, do we need to pass the size or cbFetched?

    PyObject* guid_bytes = PyBytes_FromStringAndSize((char*)buffer, (Py_ssize_t)sizeof(PYSQLGUID));
    PyObject* uuid_args = PyTuple_New(3);
    PyObject* uuid_type = GetClassForThread("uuid", "UUID");

    if(!guid_bytes || !uuid_args || !uuid_type) {
        Py_XDECREF(guid_bytes);
        Py_XDECREF(uuid_args);
        Py_XDECREF(uuid_type);
        return 0;
    }

    PyTuple_SET_ITEM(uuid_args, 0, Py_None);
    PyTuple_SET_ITEM(uuid_args, 1, Py_None);
    PyTuple_SET_ITEM(uuid_args, 2, guid_bytes);

    PyObject* uuid = PyObject_CallObject(uuid_type, uuid_args);
    Py_DECREF(uuid_args);
    Py_DECREF(uuid_type);
    return uuid;
}


static PyObject* GetDataDate(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    TIMESTAMP_STRUCT value = *(TIMESTAMP_STRUCT*)buffer;
    return PyDate_FromDate(value.year, value.month, value.day);
}


static PyObject* GetDataTime(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    TIMESTAMP_STRUCT value = *(TIMESTAMP_STRUCT*)buffer;
    int micros = (int)(value.fraction / 1000); // nanos --> micros
    return PyTime_FromTime(value.hour, value.minute, value.second, micros);
}


static PyObject* GetDataTimestamp(void* buffer, SQLLEN cbFetched, bool bound, PyObject* converter, TextEnc* enc)
{
    struct tm t;
    TIMESTAMP_STRUCT value = *(TIMESTAMP_STRUCT*)buffer;

    if (value.year < 1)
    {
        value.year = 1;
    }
    else if (value.year > 9999)
    {
        value.year = 9999;
    }

    int micros = (int)(value.fraction / 1000); // nanos --> micros

    if (value.hour == 24) {  // some backends support 24:00 (hh:mm) as "end of a day"
        t.tm_year = value.year - 1900;  // tm_year is 1900-based
        t.tm_mon = value.month - 1;  // tm_mon is zero-based
        t.tm_mday = value.day;
        t.tm_hour = value.hour; t.tm_min = value.minute; t.tm_sec = value.second;
        t.tm_isdst = -1; // auto-adjust for dst

        mktime(&t); // normalize values in t
        return PyDateTime_FromDateAndTime(
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, micros
        );
    }

    return PyDateTime_FromDateAndTime(value.year, value.month, value.day, value.hour, value.minute, value.second, micros);
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

    if (cur->cnxn->map_sqltype_to_converter) {
        PyObject* func = Connection_GetConverter(cur->cnxn, type);
        if (func)
            return (PyObject*)&PyUnicode_Type;
    }

    PyObject* pytype = 0;
    bool incref = true;

    switch (type)
    {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
        pytype = (PyObject*)&PyUnicode_Type;
        break;

    case SQL_GUID:
        if (UseNativeUUID())
        {
            pytype = GetClassForThread("uuid", "UUID");
            incref = false;
        }
        else
        {
          pytype = (PyObject*)&PyUnicode_Type;
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
        pytype = (PyObject*)&PyLong_Type;
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
        pytype = (PyObject*)&PyByteArray_Type;
        break;
    }

    if (pytype && incref)
        Py_INCREF(pytype);
    return pytype;
}

PyObject* GetData(Cursor* cur, Py_ssize_t iCol, Py_ssize_t iRow)
{
    ColumnInfo* pinfo = &cur->colinfos[iCol];
    void* ptr_value;
    SQLLEN len;
    SQLLEN* ptr_len;
    bool isNull = false;

    if (pinfo->is_bound || pinfo->always_alloc) {
        assert(pinfo->buf_offset > 0);
        ptr_value = (void*)((uintptr_t)cur->fetch_buffer + pinfo->buf_offset + iRow * cur->fetch_buffer_width);
        ptr_len = (SQLLEN*)((uintptr_t)cur->fetch_buffer + pinfo->buf_offset + iRow * cur->fetch_buffer_width - sizeof(SQLLEN));
    } else {
        ptr_value = 0;
        ptr_len = &len;
    }

    if (!pinfo->is_bound && pinfo->always_alloc) {
        assert(iRow == 1);
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetData(
            cur->hstmt,
            (SQLUSMALLINT)(iCol+1),
            pinfo->c_type,
            ptr_value,
            pinfo->buf_size,
            ptr_len
        );
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret)) {
            return RaiseErrorFromHandle(cur->cnxn, "SQLGetData", cur->cnxn->hdbc, cur->hstmt);
        }
    }
    if (!pinfo->is_bound && !pinfo->always_alloc) {
        assert(iRow == 1);
        if (!ReadVarColumn(cur, iCol, pinfo->c_type, &isNull, &ptr_value, ptr_len)) {
            return 0;
        }
        assert(!ptr_value == isNull);
    } else {
        isNull = *ptr_len == SQL_NULL_DATA;
    }

    PyObject* value;
    if (isNull) {
        value = Py_None;
    } else {
        value = (*pinfo->GetData)(
            ptr_value,
            *ptr_len,
            pinfo->is_bound,
            pinfo->converter,
            pinfo->enc
        );
        if (!pinfo->is_bound && !pinfo->always_alloc) {
            PyMem_Free(ptr_value);
        }
    }

    return value;
}


inline SQLULEN CharBufferSize(SQLULEN nr_chars)
{
    // This is probably overly pessimistic. It is assumed that
    // - column_size is the number of characters, not bytes
    //   (MySQL does this, SQL Server returns the number of bytes)
    // - the encoding is UTF-8, so we need 4 bytes per character
    // - the odbc driver converts each byte (c char) into a wide char

    return (nr_chars + 1) * 8; // + 1 for null terminator
}


bool FetchBufferInfo(Cursor* cur, Py_ssize_t iCol)
{
    // 0 means error, 1 means can be bound, -1 means cannot be bound
    // If false is returned, an exception has already been set.
    //
    // The data is assumed to be the default C type for the column's SQL type.
    //
    // Must be analogous to GetData.

    ColumnInfo* pinfo = &cur->colinfos[iCol];

    // We don't implement SQLBindCol for user-defined conversions.

    // First see if there is a user-defined conversion.

    if (cur->cnxn->map_sqltype_to_converter) {
        PyObject* converter = Connection_GetConverter(cur->cnxn, pinfo->sql_type);
        if (converter) {
            pinfo->converter = converter;
            pinfo->GetData = GetDataUser;
            pinfo->can_bind = false;
            pinfo->always_alloc = false;
            pinfo->c_type = SQL_C_BINARY;
            pinfo->buf_size = 0;
            return PyErr_Occurred() ? false : true;
        }
    }

    pinfo->converter = 0;
    pinfo->can_bind = true;
    pinfo->always_alloc = true;
    switch (pinfo->sql_type)
    {
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:

    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_SS_XML:
    case SQL_DB2_XML:
        pinfo->enc = &(IsWideType(pinfo->sql_type) ? cur->cnxn->sqlwchar_enc : cur->cnxn->sqlchar_enc);
        pinfo->c_type = pinfo->enc->ctype;
        pinfo->GetData = GetText;
        if (pinfo->column_size <= 0) {
            pinfo->buf_size = 0;
            pinfo->can_bind = false;
        } else {
            pinfo->buf_size = CharBufferSize(pinfo->column_size);
        }
        pinfo->always_alloc = false;
        break;

    case SQL_GUID:
        if (UseNativeUUID()) {
            pinfo->c_type = SQL_C_GUID;
            pinfo->buf_size = sizeof(PYSQLGUID);
            pinfo->GetData = GetUUID;
        } else {
            pinfo->enc = &cur->cnxn->sqlchar_enc;
            pinfo->c_type = pinfo->enc->ctype;
            // leave space for dashes every 4 characters
            pinfo->buf_size = CharBufferSize(32+7);
            pinfo->GetData = GetText;
        }
        break;

    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        pinfo->c_type = SQL_C_BINARY;
        if (pinfo->column_size == 0) {
            pinfo->buf_size = 0;
            pinfo->can_bind = false;
        } else {
            pinfo->buf_size = pinfo->column_size + 1;
        }
        pinfo->GetData = GetBinary;
        pinfo->always_alloc = false;
        break;

    case SQL_DECIMAL:
    case SQL_NUMERIC:
    case SQL_DB2_DECFLOAT:
        pinfo->enc = &cur->cnxn->sqlwchar_enc;
        pinfo->c_type = pinfo->enc->ctype;
        // need to add padding for all kinds of situations, see GetDataDecimal
        pinfo->buf_size = CharBufferSize(pinfo->column_size + 5);
        pinfo->GetData = GetDataDecimal;
        break;

    case SQL_BIT:
        pinfo->c_type = SQL_C_BIT;
        pinfo->buf_size = sizeof(SQLCHAR);
        pinfo->GetData = GetDataBit;
        break;

    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
        if (pinfo->is_unsigned) {
            pinfo->c_type = SQL_C_ULONG;
            pinfo->buf_size = sizeof(SQLINTEGER);
            pinfo->GetData = GetDataULong;
        } else {
            pinfo->c_type = SQL_C_LONG;
            pinfo->GetData = GetDataLong;
            pinfo->buf_size = sizeof(SQLINTEGER);
        }
        break;

    case SQL_BIGINT:
        if (pinfo->is_unsigned) {
            pinfo->c_type = SQL_C_UBIGINT;
            pinfo->buf_size = sizeof(SQLBIGINT);
            pinfo->GetData = GetDataULongLong;
        } else {
            pinfo->c_type = SQL_C_SBIGINT;
            pinfo->buf_size = sizeof(SQLBIGINT);
            pinfo->GetData = GetDataLongLong;
        }
        break;

    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        pinfo->c_type = SQL_C_DOUBLE;
        pinfo->buf_size = sizeof(double);
        pinfo->GetData = GetDataDouble;
        break;

    case SQL_TYPE_DATE:
        pinfo->c_type = SQL_C_TYPE_TIMESTAMP;
        pinfo->buf_size = sizeof(TIMESTAMP_STRUCT);
        pinfo->GetData = GetDataDate;
        break;

    case SQL_TYPE_TIME:
        pinfo->c_type = SQL_C_TYPE_TIMESTAMP;
        pinfo->buf_size = sizeof(TIMESTAMP_STRUCT);
        pinfo->GetData = GetDataTime;
        break;

    case SQL_TYPE_TIMESTAMP:
        pinfo->c_type = SQL_C_TYPE_TIMESTAMP;
        pinfo->buf_size = sizeof(TIMESTAMP_STRUCT);
        pinfo->GetData = GetDataTimestamp;
        break;

    case SQL_SS_TIME2:
        pinfo->c_type = SQL_C_BINARY;
        pinfo->buf_size = sizeof(SQL_SS_TIME2_STRUCT);
        pinfo->GetData = GetSqlServerTime;
        break;
    default:
        RaiseErrorV("HY106", ProgrammingError, "ODBC SQL type %d is not yet supported.  column-index=%zd  type=%d",
                    (int)pinfo->sql_type, iCol, (int)pinfo->sql_type);
        return false;
    }
    return true;
}
