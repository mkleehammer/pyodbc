// The functions for reading a single value from the database using SQLGetData.  There is a different function for
// every data type.

#include "pyodbc.h"
#include "wrapper.h"
#include "pyodbcmodule.h"
#include "cursor.h"
#include "connection.h"
#include "errors.h"
#include "dbspecific.h"
#include "sqlwchar.h"
#include <datetime.h>
#include <string.h> /* strdup */
#include <stdlib.h> /* strtol, NULL */
#include <vector> /* std::vector */

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
    Py_ssize_t cbAllocated = 256;
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
        SQLLEN cbData;

        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), ctype, &pb[cbUsed], (SQLLEN)cbAvailable, &cbData);
        Py_END_ALLOW_THREADS;

        TRACE("ReadVarColumn: SQLGetData avail=%d --> ret=%d cbData=%d\n", (int)cbAvailable, (int)ret, (int)cbData);

        if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA)
        {
            RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);
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
                cbRemaining = 2048;
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

    // NB: In each branch we make a check for a zero length string and handle it specially
    // since PyUnicode_Decode may (will?) fail if we pass a zero-length string.  Issue #172
    // first pointed this out with shift_jis.  I'm not sure if it is a fault in the
    // implementation of this codec or if others will have it also.

    PyObject* str;

#if PY_MAJOR_VERSION < 3
    // The Unicode paths use the same code.
    if (enc.to == TO_UNICODE)
    {
#endif
        if (cbData == 0)
        {
            str = PyUnicode_FromStringAndSize("", 0);
        }
        else
        {
            int byteorder = 0;
            switch (enc.optenc)
            {
            case OPTENC_UTF8:
                str = PyUnicode_DecodeUTF8((char*)pbData, cbData, "strict");
                break;
            case OPTENC_UTF16:
                byteorder = BYTEORDER_NATIVE;
                str = PyUnicode_DecodeUTF16((char*)pbData, cbData, "strict", &byteorder);
                break;
            case OPTENC_UTF16LE:
                byteorder = BYTEORDER_LE;
                str = PyUnicode_DecodeUTF16((char*)pbData, cbData, "strict", &byteorder);
                break;
            case OPTENC_UTF16BE:
                byteorder = BYTEORDER_BE;
                str = PyUnicode_DecodeUTF16((char*)pbData, cbData, "strict", &byteorder);
                break;
            case OPTENC_LATIN1:
                str = PyUnicode_DecodeLatin1((char*)pbData, cbData, "strict");
                break;
            default:
                // The user set an encoding by name.
                str = PyUnicode_Decode((char*)pbData, cbData, enc.name, "strict");
                break;
            }
        }
#if PY_MAJOR_VERSION < 3
    }
    else if (cbData == 0)
    {
        str = PyString_FromStringAndSize("", 0);
    }
    else if (enc.optenc == OPTENC_RAW)
    {
        // No conversion.
        str = PyString_FromStringAndSize((char*)pbData, cbData);
    }
    else
    {
        // The user has requested a string object.  Unfortunately we don't have
        // str versions of all of the optimized functions.
        const char* encoding;
        switch (enc.optenc)
        {
        case OPTENC_UTF8:
            encoding = "utf-8";
            break;
        case OPTENC_UTF16:
            encoding = "utf-16";
            break;
        case OPTENC_UTF16LE:
            encoding = "utf-16-le";
            break;
        case OPTENC_UTF16BE:
            encoding = "utf-16-be";
            break;
        case OPTENC_LATIN1:
            encoding = "latin-1";
            break;
        default:
            encoding = enc.name;
        }

        str = PyString_Decode((char*)pbData, cbData, encoding, "strict");
    }
#endif

    pyodbc_free(pbData);

    return str;
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

    // TODO: Is Unicode a good idea for Python 2.7?  We need to know which drivers support Unicode.

    const int buffsize = 100;
    ODBCCHAR buffer[buffsize];
    SQLLEN cbFetched = 0; // Note: will not include the NULL terminator.

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_WCHAR, buffer, sizeof(buffer), &cbFetched);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA || cbFetched > (buffsize * ODBCCHAR_SIZE))
        Py_RETURN_NONE;

    // Remove non-digits and convert the databases decimal to a '.' (required by decimal ctor).
    //
    // We are assuming that the decimal point and digits fit within the size of ODBCCHAR.

    int cch = (int)(cbFetched / ODBCCHAR_SIZE);

    char ascii[buffsize];
    size_t asciilen = 0;
    for (int i = 0; i < cch; i++)
    {
        if (buffer[i] == chDecimal)
        {
            // Must force it to use '.' since the Decimal class doesn't pay attention to the locale.
            ascii[asciilen++] = '.';
        }
        else if (buffer[i] > 0xFF || ((buffer[i] < '0' || buffer[i] > '9') && buffer[i] != '-'))
        {
            // We are expecting only digits, '.', and '-'.  This could be a
            // Unicode currency symbol or group separator (',').  Ignore.
        }
        else
        {
            ascii[asciilen++] = (char)buffer[i];
        }
    }

    ascii[asciilen] = 0;

    /*
    for (int i = (cch - 1); i >= 0; i--)
    {
        if (buffer[i] == chDecimal)
        {
            // Must force it to use '.' since the Decimal class doesn't pay attention to the locale.
            buffer[i] = '.';
        }
        else if ((buffer[i] < '0' || buffer[i] > '9') && buffer[i] != '-')
        {
            memmove(&buffer[i], &buffer[i] + 1, (cch - i) * (size_t)ODBCCHAR_SIZE);
            cch--;
        }
    }

    I(buffer[cch] == 0);

    Object str(PyUnicode_FromSQLWCHAR((const SQLWCHAR*)buffer, cch));
    if (!str)
        return 0;
    */
    Object str;

#if PY_MAJOR_VERSION < 3
    str.Attach(PyString_FromStringAndSize(ascii, (Py_ssize_t)asciilen));
#else
    // This treats the string like UTF-8 which is fine for a reall ASCII string.
    str.Attach(PyString_FromStringAndSize(ascii, (Py_ssize_t)asciilen));
#endif

    return PyObject_CallFunction(decimal_type, "O", str.Get());
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
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

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
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), nCType, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

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
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

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
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    int micros = (int)(value.fraction / 1000); // nanos --> micros
    return PyTime_FromTime(value.hour, value.minute, value.second, micros);
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
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

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


static std::vector<long int> read_time_token_list(const char *deltastr)
{
    const char* delim = " :.";
    char* saveptr = NULL, *endptr = NULL;
    char* current_token = NULL;
    char* deltacopy = strdup(deltastr);
    std::vector<long int> time_numbers;

    current_token = strtok_r(deltacopy, delim, &saveptr);
    while (current_token != NULL) {
        errno = 0;
        long int num = strtol(current_token, &endptr, 10);

        /* There was an error, so return an empty vector of numbers. */
        if (errno != 0)
            return std::vector<long int>();

        time_numbers.push_back(num);
        current_token = strtok_r(NULL, delim, &saveptr);
    }

    free(deltacopy);
    return time_numbers;
}


static PyObject* GetDataInterval(Cursor* cur, Py_ssize_t iCol)
{
    char value[1024] = { 0 };

    SQLLEN cbFetched = 0;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetData(cur->hstmt, (SQLUSMALLINT)(iCol+1), SQL_C_CHAR, &value, sizeof(value), &cbFetched);
    Py_END_ALLOW_THREADS

    /* There were more than 1024 characters of date data. This is a problem. */
    if (ret == SQL_SUCCESS_WITH_INFO)
        return RaiseErrorFromHandle("SQLGetDataInterval", cur->cnxn->hdbc, cur->hstmt);

    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLGetData", cur->cnxn->hdbc, cur->hstmt);

    if (cbFetched == SQL_NULL_DATA)
        Py_RETURN_NONE;

    const unsigned int EXPECTED_NUM_TOKENS = 5;

    std::vector<long int> time_numbers = read_time_token_list(value);
    if (time_numbers.size() != EXPECTED_NUM_TOKENS)
        Py_RETURN_NONE;

    long int days    = time_numbers[0];
    long int hours   = time_numbers[1];
    long int mins    = time_numbers[2];
    long int seconds = time_numbers[3];
    long int micros  = time_numbers[4];

    long int hours_in_seconds = hours * 60 * 60;
    long int mins_in_seconds  = mins * 60;

    return PyDelta_FromDSU(days,
                           hours_in_seconds + mins_in_seconds + seconds,
                           micros);
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
    case SQL_GUID:
    case SQL_SS_XML:
        return GetText(cur, iCol);

    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return GetBinary(cur, iCol);

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

    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
        return GetDataInterval(cur, iCol);

    case SQL_SS_TIME2:
        return GetSqlServerTime(cur, iCol);
    }

    return RaiseErrorV("HY106", ProgrammingError, "ODBC SQL type %d is not yet supported.  column-index=%zd  type=%d",
                       (int)pinfo->sql_type, iCol, (int)pinfo->sql_type);
}
