// https://msdn.microsoft.com/en-us/library/ms711014(v=vs.85).aspx
//
// "The length of both the data buffer and the data it contains is measured in bytes, as
// opposed to characters."
//
// https://msdn.microsoft.com/en-us/library/ms711786(v=vs.85).aspx
//
// Column Size: "For character types, this is the length in characters of the data"

// NOTE: I have not ported the "fast executemany" code from 4.x to 5.x yet.  Once 5.0 is
// complete, I'll port it in 5.1.  My goal is to ensure it uses the exact same binding code
// between both code paths.  I'll probably also rename the feature to something that describes
// it more precisely like "array binding".

#include <vector>
#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "pyodbcmodule.h"
#include "cursor.h"
#include "params.h"
#include "connection.h"
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


// The largest precision a SQL_NUMERIC_STRUCT is used for (SQL Server's maximum).
static const int MAX_NUMERIC_PRECISION = 38;

// The largest exponent a Decimal can have (decimal.MAX_EMAX), well below the Py_ssize_t limit
// on 64-bit platforms.
static const Py_ssize_t MAX_DECIMAL_EXPONENT = (Py_ssize_t)(PY_SSIZE_T_MAX > 0x7FFFFFFF ? 999999999999999999LL : 0x3FFFFFFF);

static bool GetDecimalParts(PyObject* value, Object& parts, long& sign, PyObject*& digits, Py_ssize_t& exp,
                            const char** special = 0);

static void StripDecimalZeros(PyObject* digits, Py_ssize_t& numDigits, Py_ssize_t& exp)
{
    // Removes trailing zeros (1.50 -> 1.5, 100 -> 1E+2) by reducing numDigits and increasing
    // exp, so only significant digits are counted.  Any zero becomes 0E0.  The digits have been
    // checked by GetDecimalParts.
    numDigits = PyTuple_GET_SIZE(digits);
    while (numDigits > 1 && PyLong_AsLong(PyTuple_GET_ITEM(digits, numDigits - 1)) == 0)
    {
        numDigits--;
        exp++;
    }
    if (numDigits == 1 && PyLong_AsLong(PyTuple_GET_ITEM(digits, 0)) == 0)
        exp = 0;
}

static bool FitsGuessedNumeric(Py_ssize_t numDigits, Py_ssize_t exp, Py_ssize_t scale, Py_ssize_t precision)
{
    // Returns true if a Decimal with numDigits significant digits and exponent exp (from
    // StripDecimalZeros) can be bound exactly as numeric(precision, scale).
    if (scale < 0 || scale > precision || scale + exp < 0)
        return false;
    long long digits = (long long)numDigits + exp + scale;  // digits in the scaled integer
    return digits <= precision;
}

static Py_ssize_t MinimumScale(Py_ssize_t exp)
{
    return exp < 0 ? -exp : 0;
}

static int DetectCType(PyObject *cell, ParamInfo *pi)
{
    // Detects and sets the appropriate C type to use for binding the specified Python object.
    // Also sets the buffer length to use.  Returns false if unsuccessful.
    //
    // We're setting the pi ParameterType and BufferLength.  These are based on the Python
    // value if not None or binary.  For those, the *existing* ParameterType is used.  This
    // could be from a previous row or could have been initialized from SQLDescribeParam.

    PyObject* cls = 0;

    if (pi->guessScale && cell != Py_None && cell != null_binary)
    {
        // Start from the described type so that a scale guessed for a Decimal in an earlier
        // batch does not change how a str, for example, is bound.  None keeps the type of the
        // previous batch.
        pi->ParameterType = pi->describedType;
        pi->ColumnSize = pi->describedSize;
        pi->DecimalDigits = pi->describedDigits;
    }

    if (PyBool_Check(cell))
    {
    Type_Bool:
        pi->ValueType = SQL_C_BIT;
        pi->BufferLength = 1;
    }
    else if (PyLong_Check(cell))
    {
    Type_Long:
        if (pi->ParameterType == SQL_NUMERIC ||
            pi->ParameterType == SQL_DECIMAL)
        {
            pi->ValueType = SQL_C_NUMERIC;
            pi->BufferLength = sizeof(SQL_NUMERIC_STRUCT);
            if (pi->guessScale)
            {
                // An integer needs no scale.  A Decimal in a later row that needs one causes a
                // rebind.
                pi->ColumnSize = (SQLULEN)pi->guessPrecision;
                pi->DecimalDigits = 0;
            }
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
        pi->ValueType = SQL_C_BINARY;
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
    else if (PyByteArray_Check(cell))
    {
        // Type_ByteArray:
        pi->ValueType = SQL_C_BINARY;
        pi->BufferLength = pi->ColumnSize ? pi->ColumnSize : sizeof(DAEParam);
    }
    else if (cell == Py_None || cell == null_binary)
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
            goto Type_Long;
        case SQL_REAL:
        case SQL_FLOAT:
        case SQL_DOUBLE:
            goto Type_Float;
        case SQL_BIT:
            goto Type_Bool;
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
          // TODO: Shouldn't this be bytes?
            //  goto Type_ByteArray;
            goto Type_Bytes;
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

        if (pi->guessScale && cell != Py_None && cell != null_binary)
        {
            // Bind with the full precision and the smallest scale this value needs, or the
            // scale from the last batch if this value fits it too, so rows that alternate
            // between scales do not each cause a rebind.  If no scale fits, PyToCType raises.
            Object parts;
            long sign;
            PyObject* digits;
            Py_ssize_t exp;
            if (!GetDecimalParts(cell, parts, sign, digits, exp))
                return false;
            Py_ssize_t numDigits;
            StripDecimalZeros(digits, numDigits, exp);

            Py_ssize_t precision = pi->guessPrecision;
            Py_ssize_t scale = MinimumScale(exp);
            if (scale > precision)
                scale = precision;
            if (pi->guessedScale > scale && FitsGuessedNumeric(numDigits, exp, pi->guessedScale, precision))
                scale = pi->guessedScale;

            pi->guessedScale = (SQLSMALLINT)scale;
            pi->DecimalDigits = (SQLSMALLINT)scale;
            if (pi->ParameterType != SQL_DECIMAL)
                pi->ParameterType = SQL_NUMERIC;
            pi->ColumnSize = (SQLULEN)precision;
        }
    }
    else
    {
        RaiseErrorV(0, ProgrammingError, "Unknown object type %s during describe", cell->ob_type->tp_name);
        return false;
    }
    return true;
}

static bool GetDecimalParts(PyObject* value, Object& parts, long& sign, PyObject*& digits, Py_ssize_t& exp,
                            const char** special)
{
    // Calls value.as_tuple() and checks that the result is the (sign, digits, exponent) tuple
    // a Decimal returns, since a subclass can override as_tuple.
    //
    // NaN and Infinity have a str exponent.  If `special` is not null, it is set to the text to
    // send for them, "NaN", "Infinity", or "-Infinity", and to null for any other value.  If it
    // is null, they raise, since a SQL_NUMERIC_STRUCT cannot hold them.  A signaling NaN always
    // raises.

    if (!parts.Attach(PyObject_CallMethod(value, "as_tuple", 0)))
        return false;

    PyObject* t = parts.Get();
    if (!PyTuple_Check(t) || PyTuple_GET_SIZE(t) != 3 || !PyTuple_Check(PyTuple_GET_ITEM(t, 1)))
    {
        PyErr_SetString(PyExc_TypeError, "Decimal.as_tuple() did not return (sign, digits, exponent)");
        return false;
    }

    if (special)
        *special = 0;

    sign = PyLong_AsLong(PyTuple_GET_ITEM(t, 0));
    if (sign == -1 && PyErr_Occurred())
        return false;

    PyObject* e = PyTuple_GET_ITEM(t, 2);
    if (!PyLong_Check(e))
    {
        // Decimal uses 'n' for NaN, 'N' for a signaling NaN, and 'F' for Infinity.
        bool snan = PyUnicode_Check(e) && PyUnicode_CompareWithASCIIString(e, "N") == 0;
        bool inf = PyUnicode_Check(e) && PyUnicode_CompareWithASCIIString(e, "F") == 0;
        if (snan)
            RaiseErrorV(0, ProgrammingError, "Cannot bind a Decimal signaling NaN");
        else if (!special)
            RaiseErrorV(0, ProgrammingError,
                "Decimal NaN and Infinity cannot be used with fast_executemany or table-valued "
                "parameters");
        else
        {
            *special = !inf ? "NaN" : sign ? "-Infinity" : "Infinity";
            return true;
        }
        return false;
    }

    // An exponent too large for a Py_ssize_t (which a subclass can return, and which is only 32
    // bits on 32-bit platforms) saturates, so the range checks below raise the same errors
    // everywhere.
    int overflow = 0;
    long long llexp = PyLong_AsLongLongAndOverflow(e, &overflow);
    if (llexp == -1 && PyErr_Occurred())
        return false;
    if (overflow > 0 || llexp > PY_SSIZE_T_MAX)
        exp = PY_SSIZE_T_MAX;
    else if (overflow < 0 || llexp < PY_SSIZE_T_MIN)
        exp = PY_SSIZE_T_MIN;
    else
        exp = (Py_ssize_t)llexp;

    // The callers size buffers from the sign and digits, so check them too.  The scale is
    // bound as a SQLSMALLINT.
    if (sign != 0 && sign != 1)
    {
        PyErr_SetString(PyExc_TypeError, "Decimal.as_tuple() returned an invalid sign");
        return false;
    }

    digits = PyTuple_GET_ITEM(t, 1);
    if (PyTuple_GET_SIZE(digits) == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Decimal.as_tuple() returned no digits");
        return false;
    }
    for (Py_ssize_t i = 0, c = PyTuple_GET_SIZE(digits); i < c; i++)
    {
        PyObject* digit = PyTuple_GET_ITEM(digits, i);
        long n = PyLong_Check(digit) ? PyLong_AsLong(digit) : -1;
        if (n < 0 || n > 9)
        {
            PyErr_Clear();
            PyErr_SetString(PyExc_TypeError, "Decimal.as_tuple() returned an invalid digit");
            return false;
        }
    }

    if (exp > MAX_DECIMAL_EXPONENT)
    {
        // Larger than any Decimal's, so it can only come from a subclass, and would overflow
        // the arithmetic on it.
        RaiseErrorV(0, ProgrammingError, "Numeric overflow");
        return false;
    }

    if (exp < -SHRT_MAX)
    {
        RaiseErrorV(0, ProgrammingError,
                    "Cannot bind a Decimal with more than %d digits after the decimal point",
                    SHRT_MAX);
        return false;
    }

    return true;
}

static bool GetUUIDBytes(PyObject* value, Object& bytes)
{
    // Returns value.bytes_le, checking that it is 16 bytes since a subclass can override it.
    // (Do we need to use "bytes" on a big endian machine?)
    if (!bytes.Attach(PyObject_GetAttrString(value, "bytes_le")))
        return false;
    if (!PyBytes_Check(bytes.Get()) || PyBytes_GET_SIZE(bytes.Get()) != sizeof(SQLGUID))
    {
        PyErr_SetString(PyExc_TypeError, "UUID.bytes_le must be 16 bytes");
        return false;
    }
    return true;
}

static PyObject* PowerOfTen(Py_ssize_t n)
{
    // Returns a borrowed reference to 10**n, for 0 <= n <= MAX_NUMERIC_PRECISION, or null with
    // an exception set.
    static PyObject* table[MAX_NUMERIC_PRECISION + 1];
    if (n < 0 || n > MAX_NUMERIC_PRECISION)
    {
        PyErr_SetString(PyExc_ValueError, "power of ten out of range");
        return 0;
    }
    if (!table[n])
    {
        Object ten(PyLong_FromLong(10));
        Object exponent(PyLong_FromSsize_t(n));
        if (!ten || !exponent)
            return 0;
        table[n] = PyNumber_Power(ten, exponent, Py_None);
    }
    return table[n];
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

            if (pi->DecimalDigits < 0 || pi->DecimalDigits > MAX_NUMERIC_PRECISION)
            {
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                return false;
            }

            // The struct's precision tells the driver how many digits to send, and some
            // drivers drop the leading digits of a value with more, so check the digits
            // against it, not just against the 16-byte val.
            Py_ssize_t precision = MAX_NUMERIC_PRECISION;
            if (pi->guessScale && pi->ColumnSize >= 1 && pi->ColumnSize < MAX_NUMERIC_PRECISION)
                precision = (Py_ssize_t)pi->ColumnSize;

            Object absVal(PyNumber_Absolute(cell));
            if (!absVal)
                return false;
            PyObject* scaler = PowerOfTen(pi->DecimalDigits);
            PyObject* limit = PowerOfTen(precision);
            if (!scaler || !limit)
                return false;
            Object scaledVal(PyNumber_Multiply(absVal, scaler));
            if (!scaledVal)
                return false;
            int tooBig = PyObject_RichCompareBool(scaledVal, limit, Py_GE);
            if (tooBig < 0)
                return false;
            if (tooBig)
            {
                if (pi->guessScale && (pi->DecimalDigits > 0 || precision < pi->guessPrecision))
                {
                    // It fits with no scale.  Returning false without an exception executes
                    // the rows so far and rebinds starting at this row.
                    PyObject* guessLimit = PowerOfTen(pi->guessPrecision);
                    if (!guessLimit)
                        return false;
                    int fits = PyObject_RichCompareBool(absVal, guessLimit, Py_LT);
                    if (fits)
                        return false;  // a rebind, or an error if fits < 0

                }
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                return false;
            }

            pNum->precision = (SQLCHAR)(pi->guessScale ? precision : pi->ColumnSize);
            pNum->scale = (SQLCHAR)pi->DecimalDigits;
            pNum->sign = _PyLong_Sign(cell) >= 0;
#if PY_VERSION_HEX < 0x030D0000
            if (_PyLong_AsByteArray((PyLongObject*)scaledVal.Get(), pNum->val, sizeof(pNum->val), 1, 0)) {
#else
            if (_PyLong_AsByteArray((PyLongObject*)scaledVal.Get(), pNum->val, sizeof(pNum->val), 1, 0, 1)) {
#endif
                PyErr_Clear();
                RaiseErrorV(0, ProgrammingError, "Numeric overflow");
                return false;
            }
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
        if (pi->ValueType != SQL_C_BINARY)
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
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %zd buffer %zd", (Py_ssize_t)len, (Py_ssize_t)pi->BufferLength);
                return false;
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

        Py_ssize_t len = PyBytes_GET_SIZE(encoded);
        if (!pi->ColumnSize)
        {
            // DAE
            DAEParam *pParam = (DAEParam*)*outbuf;
            pParam->cell = encoded.Detach();
            pParam->maxlen = cur->cnxn->GetMaxLength(pi->ValueType);
            *outbuf += sizeof(DAEParam);
            ind = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)len) : SQL_DATA_AT_EXEC;
        }
        else
        {
            if (len > pi->BufferLength)
            {
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %zd buffer %zd", (Py_ssize_t)len, (Py_ssize_t)pi->BufferLength);
                return false;
            }
            memcpy(*outbuf, PyBytes_AS_STRING((PyObject*)encoded), len);
            *outbuf += pi->BufferLength;
            ind = len;
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

        // Truncate the fraction according to precision
        size_t digits = (pi->DecimalDigits > 0) ? min(9, pi->DecimalDigits) : 0;
        long fast_pow10[] = {1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000};
        SQLUINTEGER milliseconds = PyDateTime_DATE_GET_MICROSECOND(cell) * 1000;
        pts->fraction = milliseconds - (milliseconds % fast_pow10[9 - digits]);

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
                RaiseErrorV(0, ProgrammingError, "String data, right truncation: length %zd buffer %zd", (Py_ssize_t)len, (Py_ssize_t)pi->BufferLength);
                return false;
            }
            memcpy(*outbuf, PyByteArray_AS_STRING(cell), len);
            *outbuf += pi->BufferLength;
            ind = len;
        }
    }
    else if (IsInstanceForThread(cell, "uuid", "UUID", &cls) && cls)
    {
        if (pi->ValueType != SQL_C_GUID)
            return false;
        pi->BufferLength = 16;
        Object b;
        if (!GetUUIDBytes(cell, b))
            return false;
        memcpy(*outbuf, PyBytes_AS_STRING(b.Get()), sizeof(SQLGUID));
        *outbuf += pi->BufferLength;
        ind = 16;
    }
    else if (IsInstanceForThread(cell, "decimal", "Decimal", &cls) && cls)
    {
        if (pi->ValueType != SQL_C_NUMERIC)
            return false;
        // Get sign, exponent, and digits.  Trailing zeros are stripped from the digit tuple
        // by hand rather than by calling Decimal.normalize(): normalize() rounds the value to
        // the calling thread's decimal context precision (28 digits by default), which
        // silently corrupts values with more significant digits (up to 38 are valid).
        Object cellParts;
        long sign;
        PyObject* digits;
        Py_ssize_t exp;
        if (!GetDecimalParts(cell, cellParts, sign, digits, exp))
            return false;

        SQL_NUMERIC_STRUCT *pNum = (SQL_NUMERIC_STRUCT*)*outbuf;
        pNum->sign = !sign;

        // Strip trailing zeros so the checks below count only significant digits.
        Py_ssize_t numDigits;
        StripDecimalZeros(digits, numDigits, exp);

        // The struct's precision tells the driver how many digits to send, and some drivers
        // (psqlODBC) silently drop the leading digits of a value with more, so the digits are
        // checked against it.  When the parameter was described, the driver knows the real
        // column, so allow up to 38 and let the database reject values too large for it.
        Py_ssize_t precision = MAX_NUMERIC_PRECISION;
        if (pi->guessScale && pi->ColumnSize >= 1 && pi->ColumnSize < MAX_NUMERIC_PRECISION)
            precision = (Py_ssize_t)pi->ColumnSize;

        if (pi->DecimalDigits < 0 || pi->DecimalDigits > MAX_NUMERIC_PRECISION)
        {
            RaiseErrorV(0, ProgrammingError, "Numeric overflow");
            return false;
        }

        // PyDecimal is digits * 10**exp = digits / 10**-exp
        // SQL_NUMERIC_STRUCT is val / 10**scale
        Py_ssize_t scaleDiff = pi->DecimalDigits + exp;

        if (pi->guessScale && !FitsGuessedNumeric(numDigits, exp, pi->DecimalDigits, precision) &&
            FitsGuessedNumeric(numDigits, exp, MinimumScale(exp), pi->guessPrecision))
        {
            // The scale was guessed from earlier rows and this value does not fit it, but it
            // fits another.  Returning false without an exception executes the rows so far and
            // rebinds starting at this row, where DetectCType picks a scale that fits.
            return false;
        }
        if (scaleDiff < 0)
        {
            RaiseErrorV(0, ProgrammingError, "Converting decimal loses precision");
            return false;
        }

        // Checking here also keeps a huge exponent from sizing the tuple below.
        if ((long long)numDigits + scaleDiff > precision)
        {
            RaiseErrorV(0, ProgrammingError, "Numeric overflow");
            return false;
        }

        // Append '0's to the end of the digits to effect the scaling.
        Object newDigits(PyTuple_New(numDigits + scaleDiff));
        if (!newDigits)
            return false;
        for (Py_ssize_t i = 0; i < numDigits; i++)
        {
            PyObject* digit = PyLong_FromSsize_t(PyNumber_AsSsize_t(PyTuple_GET_ITEM(digits, i), 0));
            if (!digit)
                return false;
            PyTuple_SET_ITEM(newDigits.Get(), i, digit);
        }
        for (Py_ssize_t i = numDigits; i < scaleDiff + numDigits; i++)
        {
            PyObject* zero = PyLong_FromLong(0);
            if (!zero)
                return false;
            PyTuple_SET_ITEM(newDigits.Get(), i, zero);
        }
        Object args(Py_BuildValue("((iOi))", 0, newDigits.Get(), 0));
        if (!args)
            return false;
        Object scaledDecimal(PyObject_CallObject((PyObject*)cell->ob_type, args));
        if (!scaledDecimal)
            return false;
        Object digitLong(PyNumber_Long(scaledDecimal));
        if (!digitLong)
            return false;

        pNum->precision = (SQLCHAR)(pi->guessScale ? precision : pi->ColumnSize);
        pNum->scale = (SQLCHAR)pi->DecimalDigits;


#if PY_VERSION_HEX < 0x030D0000
        int ret = _PyLong_AsByteArray((PyLongObject*)digitLong.Get(), pNum->val, sizeof(pNum->val), 1, 0);
#else
        int ret = _PyLong_AsByteArray((PyLongObject*)digitLong.Get(), pNum->val, sizeof(pNum->val), 1, 0, 1);
#endif
        if (ret)
        {
            PyErr_Clear();
            RaiseErrorV(0, ProgrammingError, "Numeric overflow");
            return false;
        }
        *outbuf += pi->BufferLength;
        ind = sizeof(SQL_NUMERIC_STRUCT);
    }
    else if (cell == Py_None || cell == null_binary)
    {
        // REVIEW: Theoretically we could eliminate the initial call to SQLDescribeParam for
        // all columns if we had a special value for "unknown" and called SQLDescribeParam only
        // here when we hit it.  Even then, only if we don't already have previous Python
        // objects!

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
            PyMem_Free(a[i].ParameterValuePtr);
        if (a[i].ParameterType == SQL_SS_TABLE && a[i].nested)
            FreeInfos(a[i].nested, a[i].maxlength);
        Py_XDECREF(a[i].pObject);
    }
    PyMem_Free(a);
}

static bool GetNullInfo(Cursor* cur, Py_ssize_t index, ParamInfo& info, bool isTVP)
{
    // GetParamType won't work for TVP columns, so we fall back on SQL_VARCHAR.
    if (isTVP)
    {
        if (info.ParameterType == SQL_UNKNOWN_TYPE)
            info.ParameterType = SQL_VARCHAR;
    }
    else if (!GetParamType(cur, index, info.ParameterType))
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


static bool GetBytesInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, bool isTVP)
{
    // The Python 3 version that writes bytes as binary data.
    Py_ssize_t cb = PyBytes_GET_SIZE(param);

    info.ValueType  = SQL_C_BINARY;
    info.ColumnSize = isTVP ? 0 : (SQLUINTEGER)max(cb, 1);

    SQLLEN maxlength = cur->cnxn->GetMaxLength(info.ValueType);

    if (maxlength == 0 || cb <= maxlength || isTVP)
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


static bool GetUnicodeInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, bool isTVP)
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

    if (enc.optenc == OPTENC_UTF16)
    {
        denom = 2;
    }
    else if (enc.optenc == OPTENC_UTF32)
    {
        denom = 4;
    }

    info.ColumnSize = isTVP ? 0 : (SQLUINTEGER)max(cb / denom, 1);

    info.pObject = encoded.Detach();

    SQLLEN maxlength = cur->cnxn->GetMaxLength(enc.ctype);

    if (maxlength == 0 || cb <= maxlength || isTVP)
    {
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_VARCHAR : SQL_WVARCHAR;
        info.ParameterValuePtr = PyBytes_AS_STRING(info.pObject);
        info.BufferLength      = (SQLLEN)cb;
        info.StrLen_or_Ind     = (SQLLEN)cb;
    }
    else
    {
        // Too long to pass all at once, so we'll provide the data at execute.
        info.ParameterType     = (enc.ctype == SQL_C_CHAR) ? SQL_LONGVARCHAR : SQL_WLONGVARCHAR;
        info.ParameterValuePtr = &info;
        info.BufferLength      = sizeof(ParamInfo*);
        info.StrLen_or_Ind     = cur->cnxn->need_long_data_len ? SQL_LEN_DATA_AT_EXEC((SQLLEN)cb) : SQL_DATA_AT_EXEC;
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


inline bool NeedsBigInt(long long ll)
{
    // NOTE: Smallest 32-bit int should be -214748368 but the MS compiler v.1900 AMD64
    // says that (10 < -2147483648).  Perhaps I miscalculated the minimum?
    return ll < -2147483647 || ll > 2147483647;
}

static bool GetLongInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, bool isTVP)
{
    // Since some drivers like Access don't support BIGINT, we use INTEGER when possible.
    // Unfortunately this may mean that we end up with two execution plans for the same SQL.
    // We could use SQLDescribeParam but that's kind of expensive.

    long long value = PyLong_AsLongLong(param);
    if (PyErr_Occurred())
        return false;

    if (isTVP || NeedsBigInt(value))
    {
        info.Data.i64          = (INT64)value;
        info.ValueType         = SQL_C_SBIGINT;
        info.ParameterType     = SQL_BIGINT;
        info.ParameterValuePtr = &info.Data.i64;
        info.StrLen_or_Ind     = 8;
    }
    else
    {
        info.Data.i32          = (int)value;
        info.ValueType         = SQL_C_LONG;
        info.ParameterType     = SQL_INTEGER;
        info.ParameterValuePtr = &info.Data.i32;
        info.StrLen_or_Ind     = 4;
    }
    return true;
}

static bool GetFloatInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // Python floats are usually numeric values, but can also be "Infinity" or "NaN".
    // https://docs.python.org/3/library/functions.html#float
    // PyFloat_AsDouble() does not generate an error for Infinity/NaN, and it is not
    // easy to check for those values.  Typically, the database will reject them.
    double value = PyFloat_AsDouble(param);
    if (PyErr_Occurred())
        return false;

    info.Data.dbl          = value;
    info.ValueType         = SQL_C_DOUBLE;
    info.ParameterType     = SQL_DOUBLE;
    info.ParameterValuePtr = &info.Data.dbl;
    info.ColumnSize = 15;
    info.StrLen_or_Ind = sizeof(double);
    return true;
}

static char* CreateDecimalString(long sign, PyObject* digits, Py_ssize_t exp)
{
    // Allocate an ASCII string containing the given decimal.  Lengths and counters are
    // Py_ssize_t because a Decimal's exponent can be far larger than an int (or a 32-bit long).

    Py_ssize_t count = PyTuple_GET_SIZE(digits);
    Py_ssize_t ndigits = (exp >= 0) ? count + exp : max(count, -exp);
    if (ndigits > PY_SSIZE_T_MAX - 4)
    {
        PyErr_NoMemory();
        return 0;
    }

    char* pch;
    Py_ssize_t len;

    if (exp >= 0)
    {
        // (1 2 3) exp = 2 --> '12300'

        len = sign + count + exp + 1; // 1: NULL
        pch = (char*)PyMem_Malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            for (Py_ssize_t i = 0; i < count; i++)
                *p++ = (char)('0' + PyLong_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            for (Py_ssize_t i = 0; i < exp; i++)
                *p++ = '0';
            *p = 0;
        }
    }
    else if (-exp < count)
    {
        // (1 2 3) exp = -2 --> 1.23 : prec = 3, scale = 2

        len = sign + count + 2; // 2: decimal + NULL
        pch = (char*)PyMem_Malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            Py_ssize_t i = 0;
            for (; i < (count + exp); i++)
                *p++ = (char)('0' + PyLong_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = '.';
            for (; i < count; i++)
                *p++ = (char)('0' + PyLong_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = 0;
        }
    }
    else
    {
        // (1 2 3) exp = -5 --> 0.00123 : prec = 5, scale = 5

        len = sign + -exp + 3; // 3: leading zero + decimal + NULL

        pch = (char*)PyMem_Malloc((size_t)len);
        if (pch)
        {
            char* p = pch;
            if (sign)
                *p++ = '-';
            *p++ = '0';
            *p++ = '.';

            for (Py_ssize_t i = 0; i < -(exp + count); i++)
                *p++ = '0';

            for (Py_ssize_t i = 0; i < count; i++)
                *p++ = (char)('0' + PyLong_AS_LONG(PyTuple_GET_ITEM(digits, i)));
            *p++ = 0;
        }
    }

    if (!pch)
        PyErr_NoMemory();

    assert(pch == 0 || (Py_ssize_t)(strlen(pch) + 1) == len);

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
    info.ParameterValuePtr = PyMem_Malloc(sizeof(SQLGUID));
    if (!info.ParameterValuePtr)
    {
        PyErr_NoMemory();
        return false;
    }

    Object b;
    if (!GetUUIDBytes(param, b))
        return false;
    memcpy(info.ParameterValuePtr, PyBytes_AS_STRING(b.Get()), sizeof(SQLGUID));
    info.StrLen_or_Ind = sizeof(SQLGUID);
    return true;
}


static bool GetDecimalInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, PyObject* decimal_type)
{
    // decimal_type: This is a new reference that we are responsible for freeing.
    Object tmp(decimal_type);

    // The NUMERIC structure never works right with SQL Server and probably a lot of other drivers.  We'll bind as a
    // string.  Unfortunately, the Decimal class doesn't seem to have a way to force it to return a string without
    // exponents, so we'll have to build it ourselves.

    Object t;
    long sign;
    PyObject* digits;
    Py_ssize_t exp;
    const char* special;
    if (!GetDecimalParts(param, t, sign, digits, exp, &special))
        return false;

    info.ValueType     = SQL_C_CHAR;
    info.ParameterType = SQL_NUMERIC;

    if (special)
    {
        // NaN or Infinity: send the text and leave it to the database to accept it, as
        // PostgreSQL does, or raise, as SQL Server does.
        size_t cch = strlen(special);
        char* text = (char*)PyMem_Malloc(cch + 1);
        if (!text)
        {
            PyErr_NoMemory();
            return false;
        }
        memcpy(text, special, cch + 1);
        info.ColumnSize        = MAX_NUMERIC_PRECISION;
        info.DecimalDigits     = 0;
        info.ParameterValuePtr = text;
        info.allocated         = true;
        info.StrLen_or_Ind     = (SQLINTEGER)cch;
        return true;
    }

    Py_ssize_t count = PyTuple_GET_SIZE(digits);

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
        info.ColumnSize    = (SQLUINTEGER)(-exp);
        info.DecimalDigits = (SQLSMALLINT)info.ColumnSize;
    }

    assert(info.ColumnSize >= (SQLULEN)info.DecimalDigits);

    info.ParameterValuePtr = CreateDecimalString(sign, digits, exp);
    if (!info.ParameterValuePtr)
        return false;
    info.allocated = true;

    info.StrLen_or_Ind = (SQLLEN)strlen((char*)info.ParameterValuePtr);

    return true;
}


// TVP
static bool GetTableInfo(Cursor *cur, Py_ssize_t index, PyObject* param, ParamInfo& info)
{
    // This is used for SQL Server's "table valued parameters" or TVPs.
    int nskip = 0;
    Py_ssize_t nrows = PySequence_Size(param);
    if (nrows > 0)
    {
        PyObject *cell0 = PySequence_GetItem(param, 0);
        if (cell0 == NULL)
        {
            return false;
        }
        if (PyBytes_Check(cell0) || PyUnicode_Check(cell0))
        {
            nskip++;
            if (nrows > 1)
            {
                PyObject *cell1 = PySequence_GetItem(param, 1);
                nskip += (PyBytes_Check(cell1) || PyUnicode_Check(cell1));
                Py_XDECREF(cell1);
            }
        }
        Py_XDECREF(cell0);
    }
    nrows -= nskip;

    if (!nskip)
    {
        // Need to describe in order to fill in IPD with the TVP's type name, because user has
        // not provided it
        SQLSMALLINT tvptype;
        SQLDescribeParam(cur->hstmt, index + 1, &tvptype, 0, 0, 0);
    }

    info.pObject = param;
    Py_INCREF(param);
    info.ValueType = SQL_C_BINARY;
    info.ParameterType = SQL_SS_TABLE;
    info.ColumnSize = nrows;
    info.DecimalDigits = 0;
    info.ParameterValuePtr = &info;
    info.BufferLength = 0;
    info.curTvpRow = nskip;
    info.StrLen_or_Ind = SQL_DATA_AT_EXEC;
    info.allocated = false;
    return true;
}

bool GetParameterInfo(Cursor* cur, Py_ssize_t index, PyObject* param, ParamInfo& info, bool isTVP)
{
    // Determines the type of SQL parameter that will be used for this parameter based on the Python data type.
    //
    // Populates `info`.

    if (param == Py_None)
        return GetNullInfo(cur, index, info, isTVP);

    if (param == null_binary)
        return GetNullBinaryInfo(cur, index, info);

    if (PyBytes_Check(param))
        return GetBytesInfo(cur, index, param, info, isTVP);

    if (PyUnicode_Check(param))
        return GetUnicodeInfo(cur, index, param, info, isTVP);

    if (PyBool_Check(param))
        return GetBooleanInfo(cur, index, param, info);

    if (PyDateTime_Check(param))
        return GetDateTimeInfo(cur, index, param, info);

    if (PyDate_Check(param))
        return GetDateInfo(cur, index, param, info);

    if (PyTime_Check(param))
        return GetTimeInfo(cur, index, param, info);

    if (PyLong_Check(param))
        return GetLongInfo(cur, index, param, info, isTVP);

    if (PyFloat_Check(param))
        return GetFloatInfo(cur, index, param, info);

    if (PyByteArray_Check(param))
    {
        // The driver reads the data with the GIL released, when another thread could resize
        // the bytearray and free its buffer, so bind a snapshot.  info.pObject keeps it alive
        // until the parameters are freed.
        Object snapshot(PyBytes_FromObject(param));
        if (!snapshot)
            return false;
        if (!GetBytesInfo(cur, index, snapshot.Get(), info, isTVP))
            return false;
        if (!info.pObject)
            info.pObject = snapshot.Detach();
        return true;
    }

    PyObject* cls = 0;
    if (!IsInstanceForThread(param, "decimal", "Decimal", &cls))
        return false;

    if (cls != 0)
        return GetDecimalInfo(cur, index, param, info, cls);

    if (!IsInstanceForThread(param, "uuid", "UUID", &cls))
        return false;

    if (cls != 0)
        return GetUUIDInfo(cur, index, param, info, cls);

    if (PySequence_Check(param))
        return GetTableInfo(cur, index, param, info);

    RaiseErrorV("HY105", ProgrammingError, "Invalid parameter type.  param-index=%zd param-type=%s", index, Py_TYPE(param)->tp_name);
    return false;
}

static bool getObjectValue(PyObject *pObject, long& nValue)
{
  if (pObject == NULL)
    return false;

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
static bool UpdateParamInfo(Cursor* pCursor, Py_ssize_t nIndex, ParamInfo *pInfo, bool* pScaleSet = 0)
{
  // Returns true if anything was set.  If pScaleSet is not null, it is set to true if the
  // scale was.
  if (pScaleSet)
    *pScaleSet = false;

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
    bool scaleSet = false;
    pInfo->DecimalDigits = (SQLSMALLINT)getSequenceValue(desc, 2, (long)pInfo->DecimalDigits, scaleSet);
    if (scaleSet)
      rc = true;
    if (pScaleSet)
      *pScaleSet = scaleSet;
  }

  Py_CLEAR(desc);

  // If the user didn't provide the full array (in case he gave us an array), the above code would
  // set an internal error on the cursor object, as we try to read three values from an array
  // which may not have as many. This is ok, because we don't really care if the array is not completely
  // specified, so we clear the error in case it comes from this. If the error was already present before that
  // we keep it, so the user can handle it.
  if (clearError)
    PyErr_Clear();

  return rc;
}

// Peek at a Decimal value to get binding needed information without serializing it to a string.
static bool GetDecimalPrecisionAndScale(PyObject* value, SQLULEN& precision, SQLSMALLINT& scale)
{
    Object t;
    long sign;
    PyObject* digits;
    Py_ssize_t exp;
    if (!GetDecimalParts(value, t, sign, digits, exp))
        return false;
    Py_ssize_t count = PyTuple_GET_SIZE(digits);
    if (exp >= 0) {
        precision = (SQLULEN)((SQLUINTEGER)count + exp);
        scale = 0;
    }
    else if (-exp <= count) {
        precision = (SQLULEN)count;
        scale = (SQLSMALLINT)-exp;
    }
    else {
        precision = (SQLULEN)(-exp);
        scale = (SQLSMALLINT)precision;
    }
    return true;
}

static bool SetTVPName(Cursor* cur, SQLHDESC desc, Py_ssize_t index, SQLSMALLINT field, PyObject* name)
{
    // Sets the TVP's type or schema name (`field`) on the IPD.
    if (!PyUnicode_Check(name))
    {
        PyErr_Format(PyExc_TypeError, "A TVP's type and schema names must be str, not %s",
                     Py_TYPE(name)->tp_name);
        return false;
    }

    Object encoded(PyCodec_Encode(name, "UTF-16LE", "strict"));
    if (!encoded)
        return false;

    if (PyBytes_GET_SIZE(encoded.Get()) > INT_MAX)
    {
        PyErr_SetString(PyExc_ValueError, "A TVP's type or schema name is too long");
        return false;
    }

    SQLRETURN ret = SQLSetDescFieldW(desc, (SQLSMALLINT)(index + 1), field,
                                     (SQLPOINTER)PyBytes_AS_STRING(encoded.Get()),
                                     (SQLINTEGER)PyBytes_GET_SIZE(encoded.Get()));
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle(cur->cnxn, "SQLSetDescFieldW", GetConnection(cur)->hdbc, cur->hstmt);
        return false;
    }
    return true;
}

// Helper for BindParameter, which was getting unwieldy enough already.
static bool BindTVPColumns(Cursor* cur, Py_ssize_t index, ParamInfo& info)
{
    // Register the type name and schema for the TVP if provided (they'll be strings
    // instead of sequences).
    Py_ssize_t sequenceSize = PySequence_Size(info.pObject);
    if (sequenceSize < 0)
        return false;
    Py_ssize_t dataStart = 0;
    if (sequenceSize > 0) {
        Object cell0(PySequence_GetItem(info.pObject, 0));
        if (!cell0)
            return false;
        if (PyBytes_Check(cell0) || PyUnicode_Check(cell0)) {
            ++dataStart;
            SQLHDESC desc;
            SQLGetStmtAttr(cur->hstmt, SQL_ATTR_IMP_PARAM_DESC, &desc, 0, 0);
            if (!SetTVPName(cur, desc, index, SQL_CA_SS_TYPE_NAME, cell0))
                return false;

            if (sequenceSize > 1) {
                Object cell1(PySequence_GetItem(info.pObject, 1));
                if (!cell1)
                    return false;
                if (PyBytes_Check(cell1) || PyUnicode_Check(cell1)) {
                    ++dataStart;
                    if (!SetTVPName(cur, desc, index, SQL_CA_SS_SCHEMA_NAME, cell1))
                        return false;
                }
            }
        }
    }

    SQLHDESC desc;
    SQLGetStmtAttr(cur->hstmt, SQL_ATTR_APP_PARAM_DESC, &desc, 0, 0);
    SQLSetDescField(desc, index + 1, SQL_DESC_DATA_PTR, (SQLPOINTER)info.ParameterValuePtr, 0);

    // Validate the table shape and determine the column count.
    Py_ssize_t ncols = 0;
    Py_ssize_t nrows = sequenceSize - dataStart;
    for (Py_ssize_t r = 0; r < nrows; ++r) {
        Object row(PySequence_GetItem(info.pObject, r + dataStart));
        if (!row)
            return false;
        if (!PySequence_Check(row)) {
            PyErr_SetString(ProgrammingError, "A TVP's rows must be Sequence objects.");
            return false;
        }
        Py_ssize_t size = PySequence_Size(row);
        if (size < 0)
            return false;
        if (r > 0 && size != ncols) {
            RaiseErrorV(0, ProgrammingError, "A TVP's rows must all be the same size.");
            return false;
        }
        ncols = size;
    }

    // If the TVP is empty we're done.
    if (!ncols) {
        info.nested = 0;
        info.StrLen_or_Ind = SQL_DEFAULT_PARAM;
        return true;
    }

    // Store the binding informantion separately from the first row's ParamInfo array.
    // It's not enough to store the maximum precision and the maximum scale separately.
    // If a TVP column gets the Python values Decimal('1.2345') and Decimal('12.345')
    // for a DECIMAL column, the maximum precision will be 5 and the maximum scale
    // will be 4, but the second value will not fit in a SQL NUMERIC(5,4) column.
    // Therefore we must instead keep track of the maximum number of digits on each
    // side of the decimal point. We use DecimalDigits to remember the maximum number
    // of digits to the right of the decimal point (which is basically what it's already
    // doing) and we temporarily use ColumnSize to store the maximum number of digits
    // to the left of the decimal point, which for each value is max(ColumnSize -
    // DecimalDigits, 0). This calculation even works in the case where DecimalDigits
    // is negative (the example given by the ODBC spec is "12000" which could be stored
    // as "12" with a scale of -3). When we have finished scanning all the rows, we
    // assign to DecimalDigits the value max(DecimalDigits, 0), and then we assign to
    // ColumnSize the result of the calculation ColumnSize + DecimalDigits for each of
    // the Decimal columns. This approach will ensure that all the values will fit for
    // the bindings we apply for those columns (assuming they do not exceed limits
    // imposed by the database). Note that we have to check ParameterType instead of
    // ValueType, because as of this writing we're still giving DECIMAL values to the
    // driver serialized as strings, so ValueType will be the unhelpful (for this
    // purpose) SQL_C_CHAR. Note also that we don't need to pay any attention to the
    // BufferLength value: the driver ignores it at bind time when StrLen_or_IndPtr is
    // set to SQL_DATA_AT_EXEC, which it is for all of the TVP column parameters.
    struct BindInfo {
        SQLSMALLINT vtype;      // ValueType
        SQLSMALLINT ptype;      // ParameterType
        SQLULEN     colsize;    // ColumnSize
        SQLULEN     idigits;    // digits to the left of the decimal point
        SQLSMALLINT ddigits;    // digits to the right of the decimal point
        bool        confirmed;  // if true, we have what we need to bind this column.
    };

    // Scan all rows to determine binding info for each column.
    std::vector<BindInfo> bindinfo;
    try {
        bindinfo.resize(ncols);
    }
    catch (const std::bad_alloc&) {
        PyErr_NoMemory();
        return false;
    }
    info.maxlength = ncols;
    info.nested = (ParamInfo*)PyMem_Calloc(sizeof(ParamInfo), ncols);
    if (!info.nested) {
        PyErr_NoMemory();
        return false;
    }
    for (Py_ssize_t r = 0; r < nrows; r++) {

        // Check the row's value for each parameter for which we still need information.
        bool keep_scanning = false;
        Object row(PySequence_GetItem(info.pObject, dataStart + r));
        if (!row)
            return false;
        for (Py_ssize_t i = 0; i < ncols; i++) {
            if (bindinfo[i].confirmed)
                continue;
            keep_scanning = true;
            Object value(PySequence_GetItem(row, i));
            if (!value)
                return false;

            // Populate the nested array from the first row in the nested vector.
            if (r == 0) {

                // Populate nested[i] directly for the first row, even for NULLs.
                if (!GetParameterInfo(cur, i, value.Get(), info.nested[i], true))
                    return false;
                bindinfo[i].vtype = info.nested[i].ValueType;
                bindinfo[i].ptype = info.nested[i].ParameterType;
                bindinfo[i].colsize = info.nested[i].ColumnSize;
                if (info.nested[i].ParameterType == SQL_NUMERIC) {
                    bindinfo[i].idigits = max(bindinfo[i].colsize - info.nested[i].DecimalDigits, 0);
                    bindinfo[i].ddigits = max(info.nested[i].DecimalDigits, 0);
                }
                else
                    bindinfo[i].ddigits = info.nested[i].DecimalDigits;
                info.nested[i].StrLen_or_Ind = SQL_DATA_AT_EXEC;
            }

            // If we reach this block we've only seen Py_None for this column so far (so we
            // still need to find out what the column's actual type is), or the column's type
            // is Decimal (so we need to make sure we bind the column with a precision and
            // scale which can handle all the values in the TVP for the column).
            else {
                if (value.Get() == Py_None)
                    continue;

                if (info.nested[i].ParameterType == SQL_NUMERIC) {

                    // Bump up digit counts if appropriate.
                    SQLULEN precision;
                    SQLSMALLINT scale;
                    if (!GetDecimalPrecisionAndScale(value.Get(), precision, scale)) {
                        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                            return false;
                        RaiseErrorV(0, PyExc_TypeError, "Expected Decimal but got %s (TVP column %zd)",
                            Py_TYPE(value)->tp_name, (Py_ssize_t)i);
                        return false;
                    }
                    SQLULEN integerDigits = max(precision - scale, 0);
                    bindinfo[i].idigits = max(bindinfo[i].idigits, integerDigits);
                    bindinfo[i].ddigits = max(bindinfo[i].ddigits, max(scale, 0));
                }
                else {

                    // All we have so far are NULL values, so we're trying to discover the true type.
                    ParamInfo tempinfo;
                    memset(&tempinfo, 0, sizeof(tempinfo));
                    if (!GetParameterInfo(cur, i, value.Get(), tempinfo, true))
                        return false;

                    // We're not going to use the actual value yet, so make sure memory gets cleaned up.
                    Py_XDECREF(tempinfo.pObject);
                    if (tempinfo.allocated)
                        PyMem_Free(tempinfo.ParameterValuePtr);

                    // Grab the type information.
                    bindinfo[i].ptype = info.nested[i].ParameterType = tempinfo.ParameterType;
                    bindinfo[i].vtype = tempinfo.ValueType;

                    // If this is a numeric, we also want precision and scale.
                    if (tempinfo.ParameterType == SQL_NUMERIC) {
                        bindinfo[i].idigits = max(tempinfo.ColumnSize - tempinfo.DecimalDigits, 0);
                        bindinfo[i].ddigits = max(tempinfo.DecimalDigits, 0);
                    }
                    else if (tempinfo.ParameterType == SQL_TIMESTAMP)
                        bindinfo[i].ddigits = tempinfo.DecimalDigits;
                    else
                        // For any other type, we should have everything we need for binding this column.
                        bindinfo[i].confirmed = true;
                }
            }
        }

        // If the "confirmed" flag was set for all columns when we scanned this row, we don't need to
        // look at any more rows.
        if (!keep_scanning)
            break;
    }

    // Fix the DECIMAL ColumnSize values (see lengthy comment above).
    for (Py_ssize_t i = 0; i < ncols; i++) {
        if (bindinfo[i].ptype == SQL_NUMERIC) {
            bindinfo[i].colsize = bindinfo[i].idigits + bindinfo[i].ddigits;
        }
        else
            bindinfo[i].colsize = 0;
    }

    // We're finally ready to Bind each column.
    SQLRETURN ret = SQLSetStmtAttr(cur->hstmt, SQL_SOPT_SS_PARAM_FOCUS, (SQLPOINTER)(index + 1),
        SQL_IS_INTEGER);
    if (!SQL_SUCCEEDED(ret)) {
        RaiseErrorFromHandle(cur->cnxn, "SQLSetStmtAttr", GetConnection(cur)->hdbc, cur->hstmt);
        return false;
    }
    for (Py_ssize_t i = 0; i < ncols; i++) {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
            bindinfo[i].vtype, bindinfo[i].ptype, bindinfo[i].colsize, bindinfo[i].ddigits,
            info.nested + i, 0, &info.nested[i].StrLen_or_Ind);
        Py_END_ALLOW_THREADS;
        if (GetConnection(cur)->hdbc == SQL_NULL_HANDLE) {
            RaiseErrorV(0, ProgrammingError, "The cursor's connection was closed.");
            return false;
        }
        if (!SQL_SUCCEEDED(ret)) {
            RaiseErrorFromHandle(cur->cnxn, "SQLBindParameter", GetConnection(cur)->hdbc, cur->hstmt);
            return false;
        }
    }

    // Restore the context back to the statement's parameters.
    ret = SQLSetStmtAttr(cur->hstmt, SQL_SOPT_SS_PARAM_FOCUS, 0, SQL_IS_INTEGER);
    if (!SQL_SUCCEEDED(ret)) {
        RaiseErrorFromHandle(cur->cnxn, "SQLSetStmtAttr", GetConnection(cur)->hdbc, cur->hstmt);
        return false;
    }

    return true;
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
        info.ValueType, sqltype, colsize, scale, sqltype == SQL_SS_TABLE ? 0 : info.ParameterValuePtr, info.BufferLength, &info.StrLen_or_Ind);
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

    // If this is a TVP, bind its parameters, allocate descriptors for its columns (all as DAE)
    if (sqltype == SQL_SS_TABLE && !BindTVPColumns(cur, index, info))
        return false;

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
    PyMem_Free(cur->paramtypes);
    cur->pPreparedSQL = 0;
    cur->paramtypes   = 0;
    cur->paramcount   = 0;
}

bool Prepare(Cursor* cur, PyObject* pSql)
{
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
        penc = &cur->cnxn->unicode_enc;

        Object query(penc->Encode(pSql));
        if (!query)
            return 0;

        bool isWide = (penc->ctype == SQL_C_WCHAR);

        const char* pch = PyBytes_AS_STRING(query.Get());
        Py_ssize_t  len = PyBytes_GET_SIZE(query.Get()) / (isWide ? sizeof(uint16_t) : 1);
        if (len > INT_MAX)
        {
            PyErr_SetString(PyExc_ValueError, "SQL statement is too long");
            return false;
        }
        SQLINTEGER  cch = (SQLINTEGER)len;

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
        RaiseErrorV(0, ProgrammingError, "The SQL contains %d parameter markers, but %zd parameters were supplied",
                    cur->paramcount, (Py_ssize_t)cParams);
        return false;
    }

    cur->paramInfos = (ParamInfo*)PyMem_Malloc(sizeof(ParamInfo) * cParams);
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
        if (!GetParameterInfo(cur, i, param, cur->paramInfos[i], false))
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

    if (!(cur->paramInfos = (ParamInfo*)PyMem_Malloc(sizeof(ParamInfo) * cur->paramcount)))
    {
        PyErr_NoMemory();
        return false;
    }
    memset(cur->paramInfos, 0, sizeof(ParamInfo) * cur->paramcount);

    // Wouldn't hurt to free threads here?  Or is this fast enough because it is local?

  // Describe each parameter (SQL type) in preparation for allocation of paramset array
    for (Py_ssize_t i = 0; i < cur->paramcount; i++)
    {
        SQLSMALLINT nullable;
        bool described = SQL_SUCCEEDED(SQLDescribeParam(cur->hstmt, i + 1, &(cur->paramInfos[i].ParameterType),
            &cur->paramInfos[i].ColumnSize, &cur->paramInfos[i].DecimalDigits,
            &nullable));
        if (!described)
        {
            // Default to a medium-length varchar if describing the parameter didn't work
            cur->paramInfos[i].ParameterType = SQL_VARCHAR;
            cur->paramInfos[i].ColumnSize = 255;
            cur->paramInfos[i].DecimalDigits = 0;
        }

        // This supports overriding of input sizes via setinputsizes
        // See issue 380
        // The logic is duplicated from BindParameter
        bool scaleSet = false;
        bool overridden = UpdateParamInfo(cur, i, &cur->paramInfos[i], &scaleSet);

        // A driver that reports SQL_DESCRIBE_PARAMETER "N" may still answer SQLDescribeParam, but
        // not reliably: psqlODBC returns a generic numeric(28,6), or the precision and scale of
        // whatever was last bound to the statement.  A size from setinputsizes limits the
        // precision guessed; a scale stops the guessing.
        ParamInfo& info = cur->paramInfos[i];
        info.guessScale = !scaleSet && !(described && cur->cnxn->supports_describeparam);
        info.describedType = info.ParameterType;
        info.describedSize = info.ColumnSize;
        info.describedDigits = info.DecimalDigits;
        info.guessPrecision = MAX_NUMERIC_PRECISION;
        if (overridden && info.ColumnSize >= 1 && info.ColumnSize < MAX_NUMERIC_PRECISION)
            info.guessPrecision = (SQLSMALLINT)info.ColumnSize;
        info.guessedScale = -1;
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
        // REVIEW: This check is not needed - PySequence_Fast below is sufficient.
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
            RaiseErrorV(0, ProgrammingError, "Expected %d parameters, supplied %zd", cur->paramcount, PySequence_Fast_GET_SIZE(colseq));
        ErrorRet3:
            Py_XDECREF(colseq);
            goto ErrorRet2;
        }
        PyObject **cells = PySequence_Fast_ITEMS(colseq);

        // REVIEW: We need a better description of what is going on here.  Why is it OK to pass
        // a fake bindptr to SQLBindParameter.

        // Start at a non-zero offset to prevent null pointer detection.
        char *bindptr = (char*)16;
        Py_ssize_t rowlen = 0;

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
            // BufferLength comes from the driver's (or setinputsizes') column size, so make sure
            // the row length cannot overflow.
            SQLLEN cbParam = cur->paramInfos[i].BufferLength;
            if (cbParam < 0 || rowlen > PY_SSIZE_T_MAX - (Py_ssize_t)sizeof(SQLLEN) - cbParam)
            {
                RaiseErrorV(0, ProgrammingError, "Parameter %zd is too large for fast_executemany", i + 1);
                goto ErrorRet4;
            }
            rowlen += cbParam + sizeof(SQLLEN);
            bindptr += cbParam + sizeof(SQLLEN);
        }

        // Assume parameters are homogeneous between rows in the common case, to avoid
        // another rescan for determining the array height.
        // Subtract number of rows processed as an upper bound.
        if (rowlen > 0 && (rowcount - r) > PY_SSIZE_T_MAX / rowlen)
        {
            PyErr_NoMemory();
            goto ErrorRet4;
        }
        if (!(cur->paramArray = (unsigned char*)PyMem_Malloc(rowlen * (rowcount - r))))
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
                    colseq = 0;
                    // Finish this batch of rows and attempt to execute before starting another.
                    goto DoExecute;
                }
            }
            rows_converted++;
            Py_XDECREF(colseq);
            colseq = 0;
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
                PyMem_Free(cur->paramArray);
                cur->paramArray = 0;
                goto ErrorRet4;
            }
            if (PySequence_Fast_GET_SIZE(colseq) != cur->paramcount)
            {
                RaiseErrorV(0, ProgrammingError, "Expected %d parameters, supplied %zd", cur->paramcount, PySequence_Fast_GET_SIZE(colseq));
                Py_XDECREF(colseq);
                goto ErrorRet5;
            }
            cells = PySequence_Fast_ITEMS(colseq);
        }
    DoExecute:
        if (!rows_converted || PyErr_Occurred())
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

        if (rc == SQL_SUCCESS_WITH_INFO)
        {
            GetDiagRecs(cur);
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
            {
                RaiseErrorFromHandle(cur->cnxn, "SQLParamData", cur->cnxn->hdbc, cur->hstmt);
                goto ErrorRet8;
            }

            TRACE("SQLParamData() --> %d\n", rc);

            if (rc == SQL_NEED_DATA)
            {
                PyObject* objCell = pInfo->cell;

                // If the object is Unicode it needs to be converted into bytes before it can be used by SQLPutData
                if (PyUnicode_Check(objCell))
                {
                    const TextEnc& enc = cur->cnxn->sqlwchar_enc;
                    PyObject* bytes = NULL;

                    switch (enc.optenc)
                    {
                    case OPTENC_UTF8:
                        bytes = PyUnicode_AsUTF8String(objCell);
                        break;
                    case OPTENC_UTF16:
                        bytes = PyUnicode_AsUTF16String(objCell);
                        break;
                    case OPTENC_UTF16LE:
                        bytes = PyUnicode_AsEncodedString(objCell, "utf_16_le", NULL);
                        break;
                    case OPTENC_UTF16BE:
                        bytes = PyUnicode_AsEncodedString(objCell, "utf_16_be", NULL);
                        break;
                    }
                    if (bytes && PyBytes_Check(bytes))
                    {
                        objCell = bytes;
                    }
                    //TODO: Raise or clear error when bytes == NULL.
                }

                // Another thread could resize a bytearray while the GIL is released for
                // SQLPutData, freeing its buffer, so send a snapshot.
                Object snapshot;
                if (PyByteArray_Check(objCell))
                {
                    snapshot.Attach(PyBytes_FromObject(objCell));
                    if (!snapshot)
                        goto ErrorRet8;
                    objCell = snapshot.Get();
                }

                szLastFunction = "SQLPutData";
                if (PyBytes_Check(objCell) || PyByteArray_Check(objCell))
                {
                    char *(*pGetPtr)(PyObject*);
                    Py_ssize_t (*pGetLen)(PyObject*);
                    if (PyByteArray_Check(objCell))
                    {
                        pGetPtr = PyByteArray_AsString;
                        pGetLen = PyByteArray_Size;
                    }
                    else
                    {
                        pGetPtr = PyBytes_AsString;
                        pGetLen = PyBytes_Size;
                    }

                    const char* p = pGetPtr(objCell);
                    SQLLEN cb = (SQLLEN)pGetLen(objCell);
                    SQLLEN offset = 0;

                    do
                    {
                        SQLLEN remaining = min(pInfo->maxlen, cb - offset);
                        TRACE("SQLPutData [%d] (%d) %.10s\n", offset, remaining, &p[offset]);

                        Py_BEGIN_ALLOW_THREADS
                        rc = SQLPutData(cur->hstmt, (SQLPOINTER)&p[offset], remaining);
                        Py_END_ALLOW_THREADS
                        if (!SQL_SUCCEEDED(rc))
                        {
                            RaiseErrorFromHandle(cur->cnxn, "SQLPutData", cur->cnxn->hdbc, cur->hstmt);
                            // The statement is still waiting for data.  Cancel it so the
                            // attribute resets below are not rejected as a sequence error.
                            SQLCancel(cur->hstmt);
                            goto ErrorRet8;
                        }
                        offset += remaining;
                    }
                    while (offset < cb);

                    if (PyUnicode_Check(pInfo->cell) && PyBytes_Check(objCell))
                    {
                        Py_XDECREF(objCell);
                    }
                }
                Py_XDECREF(pInfo->cell);
                rc = SQL_NEED_DATA;
            }
        }

        if (!SQL_SUCCEEDED(rc) && rc != SQL_NO_DATA)
        {
            RaiseErrorFromHandle(cur->cnxn, szLastFunction, cur->cnxn->hdbc, cur->hstmt);
            goto ErrorRet8;
        }

        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)1, SQL_IS_UINTEGER);
        SQLSetStmtAttr(cur->hstmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, 0, SQL_IS_POINTER);
        PyMem_Free(cur->paramArray);
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
        cur->paramtypes = reinterpret_cast<SQLSMALLINT*>(PyMem_Malloc(sizeof(SQLSMALLINT) * cur->paramcount));
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
