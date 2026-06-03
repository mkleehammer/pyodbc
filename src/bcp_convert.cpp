// Value conversions for the SQL Server bulk copy (BCP) fast path; see bcp_convert.h.

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "connection.h"
#include "pyodbcmodule.h"
#include "errors.h"
#include "bcp_convert.h"
#include <datetime.h>
#include <cmath>

void BcpConvert_init()
{
    PyDateTime_IMPORT;
}

/*=======================================================================================*/
// Dates and times, shaped as the plain fast path stores them
/*=======================================================================================*/

/** A wall-clock date and time; any tzinfo is ignored, as the plain fast path ignores it. */
struct CivilTime
{
    int year, month, day, hour, minute, second, micro;
};

static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, CivilTime& t)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    t.day = (int)(doy - (153 * mp + 2) / 5 + 1);
    t.month = (int)(mp < 10 ? mp + 3 : mp - 9);
    t.year = (int)((long long)yoe + era * 400 + (t.month <= 2));
}

/** Adds up to a day's worth of seconds, carrying into the date. */
static void add_seconds(CivilTime& t, int seconds)
{
    int sod = t.hour * 3600 + t.minute * 60 + t.second + seconds;
    if (sod >= 86400) {
        civil_from_days(days_from_civil(t.year, (unsigned)t.month, (unsigned)t.day) + sod / 86400, t);
        sod %= 86400;
    }
    t.hour = sod / 3600;
    t.minute = (sod / 60) % 60;
    t.second = sod % 60;
}

static CivilTime civil_of(PyObject* cell, unsigned cls)
{
    CivilTime t = {1900, 1, 1, 0, 0, 0, 0};
    if (cls != VAL_TIME) {
        t.year = PyDateTime_GET_YEAR(cell);
        t.month = PyDateTime_GET_MONTH(cell);
        t.day = PyDateTime_GET_DAY(cell);
    }
    if (cls == VAL_DATETIME) {
        t.hour = PyDateTime_DATE_GET_HOUR(cell);
        t.minute = PyDateTime_DATE_GET_MINUTE(cell);
        t.second = PyDateTime_DATE_GET_SECOND(cell);
        t.micro = PyDateTime_DATE_GET_MICROSECOND(cell);
    } else if (cls == VAL_TIME) {
        t.hour = PyDateTime_TIME_GET_HOUR(cell);
        t.minute = PyDateTime_TIME_GET_MINUTE(cell);
        t.second = PyDateTime_TIME_GET_SECOND(cell);
        t.micro = PyDateTime_TIME_GET_MICROSECOND(cell);
    }
    return t;
}

/**
 * The seconds fraction in units of `digits` digits (at most 7), from the 100ns ticks of
 * datetime2(7): truncated, as the plain path sends a value without an entry, or rounded half up,
 * as the server converts a timestamp entry.  Rounding carries into the seconds, except in the
 * last second the type holds (of the day for time, of 9999-12-31 otherwise), where the server
 * truncates instead.
 */
static int fraction_units(CivilTime& t, int digits, bool rounded, bool withinDay)
{
    static const int ticksPerUnit[8] = { 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
    const int unit = ticksPerUnit[digits > 7 ? 7 : digits];
    const int ticks = t.micro * 10;
    if (!rounded)
        return ticks / unit;
    const int units = (ticks + unit / 2) / unit;
    if (units * unit < 10000000)
        return units;
    const bool lastSecond = t.hour == 23 && t.minute == 59 && t.second == 59 &&
                            (withinDay || (t.year == 9999 && t.month == 12 && t.day == 31));
    if (lastSecond)
        return ticks / unit;
    add_seconds(t, 1);
    return 0;
}

/** Appends a seconds fraction of `digits` digits, or nothing for none. */
static int format_fraction(char* buf, size_t cap, int units, int digits)
{
    return digits > 0 ? snprintf(buf, cap, ".%0*d", digits, units) : 0;
}

/**
 * Appends the UTC offset a datetimeoffset value keeps, as a user would expect: an aware value's
 * own offset, and +00:00 for a naive value or a date, which is SQL Server's rule for a value
 * without one.  (The plain fast path differs here: the driver drops the tzinfo and applies the
 * client machine's local offset for the date.)
 *
 * @return The characters written, or -1 with an exception set: ValueError for an offset SQL
 *         Server cannot hold (not whole minutes, or beyond 14 hours), TypeError for a
 *         utcoffset() that returns something other than a timedelta or None.
 */
static int format_offset(char* buf, size_t cap, PyObject* cell, unsigned cls)
{
    long minutes = 0;
    if (cls == VAL_DATETIME && PyDateTime_DATE_GET_TZINFO(cell) != Py_None) {
        Object delta(PyObject_CallMethod(cell, "utcoffset", NULL));
        if (!delta.IsValid())
            return -1;
        if (delta.Get() != Py_None) {
            if (!PyDelta_Check(delta.Get())) {
                PyErr_Format(PyExc_TypeError, "utcoffset() returned %.100s, not a timedelta", Py_TYPE(delta.Get())->tp_name);
                return -1;
            }
            const long seconds = PyDateTime_DELTA_GET_DAYS(delta.Get()) * 86400L + PyDateTime_DELTA_GET_SECONDS(delta.Get());
            if (PyDateTime_DELTA_GET_MICROSECONDS(delta.Get()) != 0 || seconds % 60 != 0 || labs(seconds) > 14 * 3600) {
                PyErr_SetString(PyExc_ValueError, "datetimeoffset offsets must be whole minutes within 14 hours");
                return -1;
            }
            minutes = seconds / 60;
        }
    }
    return snprintf(buf, cap, " %c%02ld:%02ld", minutes < 0 ? '-' : '+', labs(minutes) / 60, labs(minutes) % 60);
}

/**
 * Without an entry, datetime2, datetimeoffset and time keep the destination's digits of the
 * fraction, truncated; datetime truncates to milliseconds then rounds to its 1/300 s tick.  Under
 * a timestamp entry the server converts from all seven digits instead: datetime2, datetimeoffset
 * and time round half up (see fraction_units), and datetime rounds to the nearest tick.  Either
 * way smalldatetime rounds up from 30 seconds, and a datetime that would round past its maximum
 * stores the maximum, as the server clamps it.  The driver's own conversion rejects excess
 * digits, and does not carry datetime rounding past midnight, hence doing it here.  A time value
 * keeps its full fraction so an unrepresentable one fails as it does on that path.  A date
 * column takes a datetime's date, for the datetimes bcp_executemany.cpp sends there, and
 * datetimeoffset values carry the offset from format_offset.
 */
int BcpFormatTemporal(char* buf, size_t cap, PyObject* cell, unsigned cls, int destination, int scale, bool rounded)
{
    CivilTime t = civil_of(cell, cls);

    if (destination == DEST_DATETIMEOFFSET) {
        const int units = fraction_units(t, scale, rounded, false);
        int n = snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
        n += format_fraction(buf + n, cap - (size_t)n, units, scale);
        const int offset = format_offset(buf + n, cap - (size_t)n, cell, cls);
        return offset < 0 ? -1 : n + offset;
    }

    if (destination == DEST_TIME) {
        if (cls != VAL_TIME) {
            const int units = fraction_units(t, scale, rounded, true);
            const int n = snprintf(buf, cap, "%02d:%02d:%02d", t.hour, t.minute, t.second);
            return n + format_fraction(buf + n, cap - (size_t)n, units, scale);
        }
        int n = snprintf(buf, cap, "%02d:%02d:%02d", t.hour, t.minute, t.second);
        if (t.micro) {
            n += snprintf(buf + n, cap - (size_t)n, ".%06d", t.micro);
            while (buf[n - 1] == '0')
                --n;
        }
        return n;
    }
    if (cls == VAL_DATE || destination == DEST_DATE)
        return snprintf(buf, cap, "%04d-%02d-%02d", t.year, t.month, t.day);

    if (destination == DEST_DATETIME) {
        int ticks = rounded ? (t.micro * 3 + 5000) / 10000 : ((t.micro / 1000) * 3 + 5) / 10;
        if (ticks == 300) {
            add_seconds(t, 1);
            ticks = 0;
        }
        if (t.year > 9999)
            return snprintf(buf, cap, "9999-12-31 23:59:59.997");
        return snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                        t.year, t.month, t.day, t.hour, t.minute, t.second, (ticks * 10 + 1) / 3);
    }
    if (destination == DEST_SMALLDATETIME) {
        if (t.second >= 30)
            add_seconds(t, 60 - t.second);
        return snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:00", t.year, t.month, t.day, t.hour, t.minute);
    }
    const int units = fraction_units(t, scale, rounded, false);
    const int n = snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return n + format_fraction(buf + n, cap - (size_t)n, units, scale);
}

/*=======================================================================================*/
// Decimals, converted as the server converts a numeric entry
/*=======================================================================================*/

bool BcpRaiseOutOfRange()
{
    RaiseErrorV("22003", 0, "Numeric value out of range");
    return false;
}

bool BcpSplitDecimalText(const char* text, Py_ssize_t n, BcpDecimalParts& d)
{
    const char* end = text + n;
    const char* p = text + (n > 0 && text[0] == '-');
    const char* point = p;
    while (point < end && *point != '.')
        ++point;
    bool digits = point > p;
    for (const char* q = p; q < end && digits; ++q)
        digits = q == point || (*q >= '0' && *q <= '9');
    if (!digits) {
        RaiseErrorV("22018", 0, "Decimal('%s') cannot be stored in a SQL Server number column", text);
        return false;
    }
    d.negative = p > text;
    d.whole = p;
    d.nwhole = (int)(point - p);
    while (d.nwhole > 0 && *d.whole == '0') {
        ++d.whole;
        --d.nwhole;
    }
    d.fraction = point < end ? point + 1 : end;
    d.nfraction = (int)(end - d.fraction);
    while (d.nfraction > 0 && d.fraction[d.nfraction - 1] == '0')
        --d.nfraction;
    return true;
}

bool BcpFitsNumericEntry(const BcpDecimalParts& d, long precision, long scale)
{
    if (d.nfraction > scale) {
        RaiseErrorV(0, ProgrammingError, "Converting decimal loses precision");
        return false;
    }
    return d.nwhole <= precision - scale || BcpRaiseOutOfRange();
}

/**
 * The text has exactly the column's places, so the driver's conversion only checks the range,
 * and loses its sign if it is zero.  96 bytes holds the 38 whole digits an entry allows (see
 * BcpFitsNumericEntry), a carry and 38 places.
 */
int BcpShapeNumericText(const BcpDecimalParts& d, int scale, bool rounded, char* buf)
{
    char digits[96];
    int n = 0;
    digits[n++] = '0';
    memcpy(digits + n, d.whole, (size_t)d.nwhole);
    n += d.nwhole;
    for (int i = 0; i < scale; ++i)
        digits[n++] = i < d.nfraction ? d.fraction[i] : '0';
    if (rounded && d.nfraction > scale && d.fraction[scale] >= '5') {
        int i = n - 1;
        for (; digits[i] == '9'; --i)
            digits[i] = '0';
        ++digits[i];
    }
    const int wholeEnd = n - scale;
    int start = 0;
    while (start < wholeEnd - 1 && digits[start] == '0')
        ++start;
    bool zero = true;
    for (int i = start; i < n && zero; ++i)
        zero = digits[i] == '0';
    int len = 0;
    if (d.negative && !zero)
        buf[len++] = '-';
    memcpy(buf + len, digits + start, (size_t)(wholeEnd - start));
    len += wholeEnd - start;
    if (scale) {
        buf[len++] = '.';
        memcpy(buf + len, digits + wholeEnd, (size_t)scale);
        len += scale;
    }
    return len;
}

/*=======================================================================================*/
// Doubles, converted as the server converts a double entry
/*=======================================================================================*/

static const int WIDE_LIMBS = 10;

/** w = w * factor + addend, for an unsigned integer of WIDE_LIMBS 32-bit limbs, least significant first. */
static void wide_multiply_add(uint32_t* w, uint32_t factor, uint32_t addend)
{
    uint64_t carry = addend;
    for (int i = 0; i < WIDE_LIMBS; ++i) {
        const uint64_t v = (uint64_t)w[i] * factor + carry;
        w[i] = (uint32_t)v;
        carry = v >> 32;
    }
}

static void wide_shift_left(uint32_t* w, int bits)
{
    const int limbs = bits / 32, rest = bits % 32;
    for (int i = WIDE_LIMBS - 1; i >= 0; --i) {
        uint64_t v = i >= limbs ? (uint64_t)w[i - limbs] << rest : 0;
        if (rest && i > limbs)
            v |= w[i - limbs - 1] >> (32 - rest);
        w[i] = (uint32_t)v;
    }
}

static void wide_shift_right(uint32_t* w, int bits)
{
    const int limbs = bits / 32, rest = bits % 32;
    for (int i = 0; i < WIDE_LIMBS; ++i) {
        uint64_t v = i + limbs < WIDE_LIMBS ? w[i + limbs] >> rest : 0;
        if (rest && i + limbs + 1 < WIDE_LIMBS)
            v |= (uint64_t)w[i + limbs + 1] << (32 - rest);
        w[i] = (uint32_t)v;
    }
}

static bool wide_bit(const uint32_t* w, int bit)
{
    return bit < 32 * WIDE_LIMBS && (w[bit / 32] >> (bit % 32)) & 1;
}

/** w = w / divisor, returning the remainder. */
static uint32_t wide_divide(uint32_t* w, uint32_t divisor)
{
    uint64_t rest = 0;
    for (int i = WIDE_LIMBS - 1; i >= 0; --i) {
        const uint64_t v = rest << 32 | w[i];
        w[i] = (uint32_t)(v / divisor);
        rest = v % divisor;
    }
    return (uint32_t)rest;
}

static bool wide_is_zero(const uint32_t* w)
{
    for (int i = 0; i < WIDE_LIMBS; ++i)
        if (w[i])
            return false;
    return true;
}

/**
 * The double's exact binary value rounded half away from zero to the scale.  The double is
 * m * 2**e for a 53-bit integer m, so the digits are m * 10**scale * 2**e rounded, which below
 * 2**128 (> 10**38, more than any decimal column holds) needs at most 255 bits.  NaN, the
 * infinities and anything larger raise; the driver rejects the rest of what does not fit the
 * column.
 */
int BcpFormatDoubleAsDecimal(double value, int scale, char* buf)
{
    if (!std::isfinite(value) || fabs(value) >= ldexp(1.0, 128)) {
        BcpRaiseOutOfRange();
        return -1;
    }
    int exponent = 0;
    const uint64_t mantissa = (uint64_t)ldexp(frexp(fabs(value), &exponent), 53);
    exponent -= 53;
    uint32_t w[WIDE_LIMBS] = { (uint32_t)mantissa, (uint32_t)(mantissa >> 32) };
    for (int i = 0; i < scale; ++i)
        wide_multiply_add(w, 10, 0);
    if (exponent >= 0)
        wide_shift_left(w, exponent);
    else {
        const bool roundUp = wide_bit(w, -exponent - 1);
        if (-exponent >= 32 * WIDE_LIMBS)
            memset(w, 0, sizeof(w));
        else
            wide_shift_right(w, -exponent);
        if (roundUp)
            wide_multiply_add(w, 1, 1);
    }

    uint32_t chunks[WIDE_LIMBS];
    int nchunks = 0;
    do
        chunks[nchunks++] = wide_divide(w, 1000000000u);
    while (!wide_is_zero(w));
    char digits[96];
    int ndigits = snprintf(digits, sizeof(digits), "%u", (unsigned)chunks[nchunks - 1]);
    for (int i = nchunks - 2; i >= 0; --i)
        ndigits += snprintf(digits + ndigits, sizeof(digits) - (size_t)ndigits, "%09u", (unsigned)chunks[i]);

    int len = 0;
    if (std::signbit(value) && !(ndigits == 1 && digits[0] == '0'))
        buf[len++] = '-';
    const int zeros = ndigits > scale ? 0 : scale + 1 - ndigits;
    const int whole = ndigits + zeros - scale;
    for (int i = 0; i < zeros; ++i)
        buf[len + i] = '0';
    memcpy(buf + len + zeros, digits, (size_t)ndigits);
    if (scale) {
        memmove(buf + len + whole + 1, buf + len + whole, (size_t)scale);
        buf[len + whole] = '.';
        ++len;
    }
    return len + whole + scale;
}

/**
 * The driver then checks the column's range.  NaN, the infinities and values beyond bigint raise,
 * where the driver's own conversion would wrap 2**63 to bigint's minimum.
 */
bool BcpTruncateDouble(double d, long long* v)
{
    if (!std::isfinite(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
        return BcpRaiseOutOfRange();
    *v = (long long)d;
    return true;
}

/**
 * Subnormals included, where the driver's own conversion rejects anything below the least normal
 * real.  NaN, the infinities and values that round past the largest real (from halfway to 2**128
 * up) raise.
 */
bool BcpNarrowDouble(double d, float* f)
{
    if (!std::isfinite(d) || fabs(d) >= ldexp(33554431.0, 103))
        return BcpRaiseOutOfRange();
    *f = (float)d;
    return true;
}
