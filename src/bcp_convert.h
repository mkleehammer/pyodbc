#pragma once

// Value conversions for the SQL Server bulk copy (BCP) fast path (bcp_executemany.cpp): the
// text and numbers BCP is given so the server stores exactly what the plain fast_executemany
// path would have stored, for the cases where the driver's own conversion of a host value would
// differ.  Include after pyodbc.h.
//
// The value classes and destination categories are shared with bcp_executemany.cpp, which
// classifies the rows and plans the columns.

/** Imports the datetime C API for the conversions; called once at module load. */
void BcpConvert_init();

/** The classes of Python value a column can hold, as bits so a column records all it has seen. */
enum : unsigned
{
    VAL_NONE     = 1u << 0,
    VAL_BOOL     = 1u << 1,
    VAL_INT      = 1u << 2,
    VAL_FLOAT    = 1u << 3,
    VAL_STR      = 1u << 4,
    VAL_BYTES    = 1u << 5,
    VAL_DECIMAL  = 1u << 6,
    VAL_DATE     = 1u << 7,
    VAL_DATETIME = 1u << 8,
    VAL_TIME     = 1u << 9,
    VAL_UUID     = 1u << 10,
};

/** Destination column categories, by how values must be prepared for them. */
enum Destination
{
    DEST_UNSUPPORTED,
    DEST_CHAR,
    DEST_XML,
    DEST_BINARY,
    DEST_INTEGER,
    DEST_BIT,
    DEST_FLOAT,
    DEST_DECIMAL,
    DEST_MONEY,
    DEST_DATE,
    DEST_TIME,
    DEST_DATETIME2,
    DEST_DATETIMEOFFSET,
    DEST_DATETIME,
    DEST_SMALLDATETIME,
    DEST_GUID,
};

/**
 * Formats a date, datetime or time value (of class `cls`) as text for a temporal destination
 * with `scale` fractional digits, rounded as the plain fast path's value is.  `rounded` says the
 * value goes as a setinputsizes timestamp entry, which the server converts from all seven
 * digits, rather than as the column's own type.
 *
 * @return The characters written, or -1 with a Python exception set.
 */
int BcpFormatTemporal(char* buf, size_t cap, PyObject* cell, unsigned cls, int destination, int scale, bool rounded);

/** Raises the out-of-range error the server reports for a value its column cannot hold. */
bool BcpRaiseOutOfRange();

/**
 * A finite Decimal's text in parts: its sign, its whole digits without leading zeros, and its
 * fraction digits without trailing zeros.
 */
struct BcpDecimalParts
{
    bool negative;
    const char* whole;
    int nwhole;
    const char* fraction;
    int nfraction;
};

/** Splits a Decimal's text into its parts, or raises for NaN or an infinity. */
bool BcpSplitDecimalText(const char* text, Py_ssize_t n, BcpDecimalParts& d);

/**
 * Checks a Decimal against a numeric entry of `precision` and `scale` as the plain path does: it
 * raises for more places than the scale before sending anything, and the server rejects more
 * whole digits than the precision leaves room for.
 */
bool BcpFitsNumericEntry(const BcpDecimalParts& d, long precision, long scale);

/**
 * Writes a Decimal with exactly `scale` places, as the server converts a numeric entry: rounded
 * half away from zero when `rounded` (decimal and money columns), otherwise truncated toward zero
 * (integer columns).  `d` must have passed BcpFitsNumericEntry, which limits it to 38 whole
 * digits, and `scale` must be at most 38, so the text fits buf's 96 bytes.
 *
 * @return The characters written.
 */
int BcpShapeNumericText(const BcpDecimalParts& d, int scale, bool rounded, char* buf);

/**
 * Writes a double with exactly `scale` places (at most 38), as the server converts a double
 * entry into a decimal column.  buf needs 96 bytes.
 *
 * @return The characters written, or -1 with an exception set.
 */
int BcpFormatDoubleAsDecimal(double value, int scale, char* buf);

/**
 * A double truncated toward zero, as the server converts a double entry into an integer column.
 * Raises, returning false, for one bigint cannot hold.
 */
bool BcpTruncateDouble(double d, long long* v);

/**
 * A double rounded to the nearest real, as the server converts a double entry into a real column.
 * Raises, returning false, for one real cannot hold.
 */
bool BcpNarrowDouble(double d, float* f);
