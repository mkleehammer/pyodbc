//
// Extensions for putting the data results of queries in NumPy containers.
// Authors: Francesc Alted <francesc@continuum.io> (original author)
//          Oscar Villellas <oscar.villellas@continuum.io>
// Copyright: Continuum Analytics 2012-2014
//

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include "pyodbc.h"
#include "cursor.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "errors.h"
#include "dbspecific.h"

#include "numpy/ndarrayobject.h"
#include "numpy/npy_math.h"

#include <vector>
#include <stdio.h>

// exported variables ----------------------------------------------------------

/* controls the maximum text field width */
Py_ssize_t iopro_text_limit = 1024;

// -----------------------------------------------------------------------------

namespace {
    inline size_t
    limit_text_size(size_t sz)
    {
        if (iopro_text_limit < 0)
            return sz;

        size_t sz_limit = static_cast<size_t>(iopro_text_limit);
        return sz < sz_limit? sz : sz_limit;
    }

    class PyNoGIL
    /* a RAII class for Python GIL */
    {
    public:
        PyNoGIL()
        {
            Py_UNBLOCK_THREADS
        }
        ~PyNoGIL()
        {
            Py_BLOCK_THREADS
        }

    private:
        PyThreadState *_save;
    };

}

// The number of rows to be fetched in case the driver cannot specify it
static size_t DEFAULT_ROWS_TO_BE_FETCHED = 10000;
static size_t DEFAULT_ROWS_TO_BE_ALLOCATED = DEFAULT_ROWS_TO_BE_FETCHED;
// API version 7 is the first one that we can use DATE/TIME
// in a pretty bug-free way. This is set to true in
// the module init function if running on Numpy >= API version 7.
static bool CAN_USE_DATETIME = false;


const char *
sql_type_to_str(SQLSMALLINT type)
{
#define TYPENAME(x,y) case x: return y;
    switch (type)
    {
        TYPENAME(SQL_CHAR, "char");
        TYPENAME(SQL_VARCHAR, "varchar");
        TYPENAME(SQL_LONGVARCHAR, "longvarchar");
        TYPENAME(SQL_WCHAR, "wchar");
        TYPENAME(SQL_WVARCHAR, "wvarchar");
        TYPENAME(SQL_WLONGVARCHAR, "wlongvarchar");

        TYPENAME(SQL_DECIMAL, "decimal");
        TYPENAME(SQL_NUMERIC, "numeric");
        TYPENAME(SQL_SMALLINT, "smallint");
        TYPENAME(SQL_INTEGER, "integer");
        TYPENAME(SQL_REAL, "real");
        TYPENAME(SQL_FLOAT, "float");
        TYPENAME(SQL_DOUBLE, "double");
        TYPENAME(SQL_BIT, "bit");
        TYPENAME(SQL_TINYINT, "tiny");
        TYPENAME(SQL_BIGINT, "bigint");

        TYPENAME(SQL_BINARY, "binary");
        TYPENAME(SQL_VARBINARY, "varbinary");
        TYPENAME(SQL_LONGVARBINARY, "longvarbinary");

        TYPENAME(SQL_TYPE_DATE, "date");
        TYPENAME(SQL_TYPE_TIME, "time");
        TYPENAME(SQL_TYPE_TIMESTAMP, "timestamp");

        TYPENAME(SQL_GUID, "guid");
    default:
        return "UNKNOWN";
    }
#undef TYPENAME
}

const char *
sql_c_type_to_str(SQLSMALLINT type)
{
#define TYPENAME(x,y) case x: return y;
    switch (type)
    {
        TYPENAME(SQL_C_BIT, "bit");
        TYPENAME(SQL_C_CHAR, "char");
        TYPENAME(SQL_C_WCHAR, "wchar");
        TYPENAME(SQL_C_TINYINT, "tinyint");
        TYPENAME(SQL_C_SSHORT, "sshort");
        TYPENAME(SQL_C_SLONG, "slong");
        TYPENAME(SQL_C_SBIGINT, "sbigint");
        TYPENAME(SQL_C_FLOAT, "float");
        TYPENAME(SQL_C_DOUBLE, "double");
        TYPENAME(SQL_C_TYPE_DATE, "date struct");
        TYPENAME(SQL_C_TIMESTAMP, "timestamp struct");
        TYPENAME(SQL_C_TIME, "time struct");
    default:
        return "UNKNOWN";
    }
#undef TYPENAME
}

using namespace std;

// Days per month, regular year and leap year
int _days_per_month_table[2][12] = {
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

//
// Returns 1 if the given year is a leap year, 0 otherwise.
//
int
is_leapyear(SQLSMALLINT year)
{
    return (year & 0x3) == 0 && /* year % 4 == 0 */
           ((year % 100) != 0 ||
            (year % 400) == 0);
}

//
// Calculates the days offset from the 1970 epoch.
//
// Code strongly based on its NumPy counterpart.
//
npy_int64
get_datestruct_days(const DATE_STRUCT *dts)
{
    int i, month;
    npy_int64 year, days = 0;
    int *month_lengths;

    year = dts->year - 1970;
    days = year * 365;

    /* Adjust for leap years */
    if (days >= 0) {
        /*
         * 1968 is the closest leap year before 1970.
         * Exclude the current year, so add 1.
         */
        year += 1;
        /* Add one day for each 4 years */
        days += year / 4;
        /* 1900 is the closest previous year divisible by 100 */
        year += 68;
        /* Subtract one day for each 100 years */
        days -= year / 100;
        /* 1600 is the closest previous year divisible by 400 */
        year += 300;
        /* Add one day for each 400 years */
        days += year / 400;
    }
    else {
        /*
         * 1972 is the closest later year after 1970.
         * Include the current year, so subtract 2.
         */
        year -= 2;
        /* Subtract one day for each 4 years */
        days += year / 4;
        /* 2000 is the closest later year divisible by 100 */
        year -= 28;
        /* Add one day for each 100 years */
        days -= year / 100;
        /* 2000 is also the closest later year divisible by 400 */
        /* Subtract one day for each 400 years */
        days += year / 400;
    }

    month_lengths = _days_per_month_table[is_leapyear(dts->year)];
    month = dts->month - 1;
    /* make sure month is in range. This prevents an illegal access
       when bad input is passed to this function */
    month = month<0 || month>11 ?  0:month;

    /* Add the months */
    for (i = 0; i < month; ++i) {
        days += month_lengths[i];
    }

    /* Add the days */
    days += dts->day - 1;

    return days;
}

//
// Convert a datetime from a datetimestruct to a datetime64 based
// on some metadata. The date is assumed to be valid.
//
// This code is heavily based on NumPy 1.7 equivalent function.
// Only conversion to microseconds is supported here.
//
npy_datetime
convert_datetimestruct_to_datetime(const TIMESTAMP_STRUCT *dts)
{
    npy_datetime ret;

    // Calculate the number of days to start
    npy_int64 days = get_datestruct_days((DATE_STRUCT*)dts);
    ret = (((days * 24 +
             dts->hour) * 60 +
            dts->minute) * 60 +
           dts->second) * 1000000 +
        dts->fraction / 1000;  // fraction is in ns (billionths of a second)

    return ret;
}

//
// Convert a date from a datestruct to a datetime64 based
// on some metadata. The date is assumed to be valid.
//
npy_datetime
convert_datestruct_to_datetime(const DATE_STRUCT *dts)
{

    // Calculate the number of days to start
    npy_datetime days = get_datestruct_days(dts);

    return days;
}

//
// Convert a time from a timestruct to a timedelta64 based
// on some metadata. The time is assumed to be valid.
//
npy_timedelta
convert_timestruct_to_timedelta(const TIME_STRUCT *dts)
{
    npy_timedelta seconds = (((dts->hour * 60) + dts->minute) * 60) +
        dts->second;

    return seconds;
}


/*
 * This is a debug helper function that allows dumping a memory buffer
 * to a string for use within TRACE calls. It reuses an internal static
 * buffer so it won't be thread safe and it will reuse the same memory
 * in different calls, but it will be enough for debugging.
 * Note: the string is valid until the next call of this function, as
 *       the buffer will be reused
 */
static
const char *
raw_buffer_as_print_string(const void *ptr, size_t len)
{
    static char _work_buffer[72];
    static char *hex_digit = "0123456789abcdef";
    const size_t max_bytes_to_dump = (sizeof(_work_buffer)/sizeof(_work_buffer[0]))/3;
    size_t pre = len < max_bytes_to_dump ? len : max_bytes_to_dump - 4;
    size_t post = len < max_bytes_to_dump ? 0 : max_bytes_to_dump - pre - 1;
    char *out = _work_buffer;
    const unsigned char *in = reinterpret_cast<const unsigned char*>(ptr);
    if (len == 0)
        return "";

    for (size_t i=0; i<pre; i++)
    {
        unsigned char c = in[i];
        *out++ = hex_digit[(c>>4) & 0xf];
        *out++ = hex_digit[c&0xf];
        *out++ = ' ';
    }

    if (post) {
        *out++ = '.';
        *out++ = '.';
        *out++ = ' ';
        in += len - post;
        for (size_t i = 0; i < post; i++)
        {
            unsigned char c = in[i];
            *out++ = hex_digit[(c>>4) & 0xf];
            *out++ = hex_digit[c&0xf];
            *out++ = ' ';
        }
    }

    out[-1] = '\0'; // overwrite last space
    return _work_buffer;
}


/*
 *  Convert the SQLWCHAR array to ucs4.
 * At most count elements will be present.
 *
 * src is assumed to be in utf16 encoding. If the driver manager uses
 * utf32 (ucs4) this will not be called.
 *
 * note: in our context the number of characters is known an comes from
 * the database schema.
 */
static void
convert_ucs4_from_utf16(void *dst, const void *src, size_t count)
{
    uint32_t *ucs4_dst = reinterpret_cast<uint32_t*>(dst);
    const uint16_t *utf16_src = reinterpret_cast<const uint16_t*>(src);
    //run until we reach the maximum number of characters (count),
    //  or null-termination (*utf16_src)
    for (size_t idx=0; idx < count && *utf16_src; ++idx) {
        uint16_t ch = *utf16_src++;
        uint32_t ucs4_ch;
        if (ch >= 0xd800 && ch <= 0xdfff) {
            // surrogate pair
            uint32_t upper = 0x3ffu & ch;
            uint32_t lower = 0x3ffu & (*utf16_src++);
            ucs4_ch = (upper << 10) + lower;
        }
        else {
            ucs4_ch = ch;
        }

        ucs4_dst[idx] = ucs4_ch;
    }
}


//
// Fill NA particular values depending on the NumPy type
//
// The only cases that need to be supported are the ones that can
// actually be generated from SQL types
static void
fill_NAvalue(void *value, PyArray_Descr *dtype) {
    int nptype = dtype->type_num;
    int elsize = dtype->elsize;
    switch (nptype)
    {
    case NPY_BOOL:
        ((npy_bool*)value)[0] = 0;   // XXX False is a good default?
        break;
    case NPY_INT8:
        ((npy_int8*)value)[0] = NPY_MIN_INT8;
        break;
    case NPY_UINT8:
        // For uint8 use max, as 0 is more likely to be valid data.
        ((npy_uint8*)value)[0] = NPY_MAX_UINT8;
        break;
    case NPY_INT16:
        ((npy_int16*)value)[0] = NPY_MIN_INT16;
        break;
    case NPY_INT32:
        ((npy_int32*)value)[0] = NPY_MIN_INT32;
        break;
    case NPY_INT64:
        ((npy_int64*)value)[0] = NPY_MIN_INT64;
        break;
    case NPY_FLOAT:
        ((npy_float *)value)[0] = NPY_NANF;
        break;
    case NPY_DOUBLE:
        ((npy_double *)value)[0] = NPY_NAN;
        break;
    case NPY_STRING:
    case NPY_UNICODE:
        memset(value, 0, static_cast<size_t>(elsize));
        break;
    case NPY_DATETIME:
        ((npy_int64*)value)[0] = NPY_DATETIME_NAT;
        break;
    case NPY_TIMEDELTA:
        ((npy_int64*)value)[0] = NPY_DATETIME_NAT;
        break;
    default:
        RaiseErrorV(0, PyExc_TypeError,
                    "NumPy data type %d is not supported.", nptype);
    }
}

static int
fill_NAarray(PyArrayObject* array, PyArrayObject* array_nulls, SQLLEN* nulls,
             size_t offset, size_t nrows)
{
    // Fill array with NA info in nullarray coming from ODBC
    npy_intp elsize_array = PyArray_ITEMSIZE(array);
    char *data_array = PyArray_BYTES(array);
    SQLLEN *data_null = nulls;

    // Only the last nrows have to be updated
    data_array += offset * elsize_array;

    if (array_nulls) {
        char *data_array_nulls = PyArray_BYTES(array_nulls);
        npy_intp elsize_array_nulls = PyArray_ITEMSIZE(array_nulls);

        data_array_nulls += offset * elsize_array_nulls;

        for (size_t i = 0; i < nrows; ++i) {
            if (data_null[i] == SQL_NULL_DATA) {
                *data_array_nulls = NPY_TRUE;
                fill_NAvalue(data_array, PyArray_DESCR(array));
            } else
            {
                *data_array_nulls = NPY_FALSE;
            }
            data_array += elsize_array;
            data_array_nulls += elsize_array_nulls;
        }
    } else
    {
        for (size_t i = 0; i < nrows; ++i) {
            // If NULL are detected, don't show data in array
            if (data_null[i] == SQL_NULL_DATA)
                fill_NAvalue(data_array, PyArray_DESCR(array));
            data_array += elsize_array;
        }
    }

    return 0;
}

//
// convert from ODBC format to NumPy format for selected types
// only types that need conversion are handled.
//
static void
convert_buffer(PyArrayObject* dst_array, void* src, int sql_c_type,
               SQLLEN offset, npy_intp nrows)
{
    switch (sql_c_type)
    {
    case SQL_C_TYPE_DATE:
        {
            npy_datetime *dst = reinterpret_cast<npy_datetime*>(PyArray_DATA(dst_array)) +
                offset;
            DATE_STRUCT *dates = static_cast<DATE_STRUCT*>(src);
            for (npy_intp i = 0; i < nrows; ++i) {
                dst[i] = convert_datestruct_to_datetime(dates+i);
            }
        }
        break;

    case SQL_C_TYPE_TIMESTAMP:
        {
            npy_datetime *dst = reinterpret_cast<npy_datetime*>(PyArray_DATA(dst_array)) +
                offset;
            TIMESTAMP_STRUCT *timestamps = static_cast<TIMESTAMP_STRUCT*>(src);
            for (npy_intp i = 0; i < nrows; ++i) {
                dst[i] = convert_datetimestruct_to_datetime(timestamps+i);
            }
        }
        break;

    case SQL_C_TYPE_TIME:
        {
            npy_timedelta *dst = reinterpret_cast<npy_timedelta*>(PyArray_DATA(dst_array)) +
                offset;
            TIME_STRUCT *timestamps = static_cast<TIME_STRUCT*>(src);
            for (npy_intp i = 0; i < nrows; ++i) {
                dst[i] = convert_timestruct_to_timedelta(&timestamps[i]);
            }
        }
        break;

    case SQL_C_WCHAR:
        {
            // note that this conversion will only be called when using ucs2/utf16
            const SQLWCHAR *utf16 = reinterpret_cast<SQLWCHAR*>(src);
            size_t len = PyArray_ITEMSIZE(dst_array)/sizeof(npy_ucs4);
            npy_ucs4 *ucs4 = reinterpret_cast<npy_ucs4*>(PyArray_DATA(dst_array)) + offset*len;
            for (npy_intp i = 0; i < nrows; ++i) {
                const SQLWCHAR *src = utf16 + 2*len*i;
                npy_ucs4 *dst = ucs4 + len*i;
                TRACE_NOLOC("Converting utf-16 buffer at %p:\n'%s'\n", src,
                            raw_buffer_as_print_string(src, 2*len*sizeof(src[0])));
                convert_ucs4_from_utf16(dst, src, len);
                TRACE_NOLOC("resulting in ucs4 buffer at %p:\n'%s'\n", dst,
                            raw_buffer_as_print_string(dst, len*sizeof(dst[0])));
            }
        }
        break;

    default:
        TRACE_NOLOC("WARN: unexpected conversion in fill_dictarray.\n");
    }
}

//
// Resize an array to a new length
//
// return 0 on success 1 on failure
//          on failure the returned array is unmodified
static int
resize_array(PyArrayObject* array, npy_intp new_len) {
    int elsize = PyArray_ITEMSIZE(array);
    void *old_data = PyArray_DATA(array);
    npy_intp old_len = PyArray_DIMS(array)[0];
    void* new_data = NULL;

    // The next test is made so as to avoid a problem with resizing to 0
    // (it seems that this is solved for NumPy 1.7 series though)
    if (new_len > 0) {
        new_data = PyDataMem_RENEW(old_data, new_len * elsize);
        if (new_data == NULL) {
            return 1;
        }
    }
    else {
        free(old_data);
    }

    // this is far from ideal. We should probably be using internal buffers
    // and then creating the NumPy array using that internal buffer. This should
    // be possible and would be cleaner.
#if (NPY_API_VERSION >= 0x7)
    ((PyArrayObject_fields *)array)->data = (char*)new_data;
#else
    array->data = (char*)new_data;
#endif
    if ((old_len < new_len) && PyArray_ISSTRING(array)) {
        memset(PyArray_BYTES(array) + old_len*elsize, 0, (new_len-old_len)*elsize);
    }

    PyArray_DIMS(array)[0] = new_len;

    return 0;
}

namespace
{
    struct fetch_status
    {
        fetch_status(SQLHSTMT h, SQLULEN chunk_size);
        ~fetch_status();

        SQLLEN rows_read_;

        /* old stmtattr to restore on destruction */
        SQLHSTMT hstmt_;
        SQLULEN old_row_bind_type_;
        SQLULEN old_row_array_size_;
        SQLULEN *old_rows_fetched_ptr_;
    };

    fetch_status::fetch_status(SQLHSTMT h, SQLULEN chunk_size) : hstmt_(h)
    {
        /* keep old stmt attr */
        SQLGetStmtAttr(hstmt_, SQL_ATTR_ROW_BIND_TYPE,
                       &old_row_bind_type_, SQL_IS_UINTEGER, 0);
        SQLGetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
                       &old_row_array_size_, SQL_IS_UINTEGER, 0);
        SQLGetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR,
                       &old_rows_fetched_ptr_, SQL_IS_POINTER, 0);

        /* configure our stmt attr */
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_BIND_TYPE,
                       SQL_BIND_BY_COLUMN, 0);
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
                       (SQLPOINTER)chunk_size, 0);
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR,
                       (SQLPOINTER)&rows_read_, 0);
    }

    fetch_status::~fetch_status()
    {
        /* unbind all cols */
        SQLFreeStmt(hstmt_, SQL_UNBIND);
        /* restore stmt attr */
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_BIND_TYPE,
                       (SQLPOINTER)old_row_bind_type_, 0);
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
                       (SQLPOINTER)old_row_array_size_, 0);
        SQLSetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR,
                       (SQLPOINTER)old_rows_fetched_ptr_, 0);
        hstmt_ = 0;
    }

    ////////////////////////////////////////////////////////////////////////

    struct column_desc
    {
        column_desc();
        ~column_desc();

        // fields coming from describe col
        SQLCHAR sql_name_[300];
        SQLSMALLINT sql_type_; // type returned in SQLDescribeCol.
        SQLULEN sql_size_;
        SQLSMALLINT sql_decimal_;
        SQLSMALLINT sql_nullable_;

        // type info
        PyArray_Descr* npy_type_descr_; // type to be used in NumPy
        int sql_c_type_; // c_type to be use when binding the column.

        // buffers used
        PyArrayObject* npy_array_; // the numpy array that will hold the result
        PyArrayObject* npy_array_nulls_; // the boolean numpy array holding null information
        void* scratch_buffer_; // source buffer when it needs conversion
        SQLLEN* null_buffer_;
        SQLLEN element_buffer_size_;
    };

    column_desc::column_desc() :
        npy_type_descr_(0), npy_array_(0), npy_array_nulls_(0), scratch_buffer_(0), null_buffer_(0), element_buffer_size_(0)
    {
    }

    column_desc::~column_desc()
    {
        if (null_buffer_) {
            GUARDED_DEALLOC(null_buffer_);
        }

        if (scratch_buffer_) {
            GUARDED_DEALLOC(scratch_buffer_);
        }

        Py_XDECREF(npy_array_nulls_);
        Py_XDECREF(npy_array_);
        Py_XDECREF(npy_type_descr_);
    }


    inline PyArray_Descr*
    dtype_from_string(const char *dtype_str_spec)
    /*
      returns a dtype (PyArray_Descr) built from a string that describes it
    */
    {
        PyObject *python_str = Py_BuildValue("s", dtype_str_spec);
        if (python_str) {
            PyArray_Descr *dtype = 0;
            PyArray_DescrConverter(python_str, &dtype);
            Py_DECREF(python_str);
            return dtype;
        }
        return 0;
    }

    inline PyArray_Descr*
    string_dtype(size_t length)
    {
        PyArray_Descr* result = PyArray_DescrNewFromType(NPY_STRING);
        if (result)
            result->elsize = static_cast<int>(length+1) * sizeof(char);
        return result;
    }

    inline PyArray_Descr*
    unicode_dtype(size_t length)
    {
        PyArray_Descr* result = PyArray_DescrNewFromType(NPY_UNICODE);
        if (result)
            result->elsize = static_cast<int>(length+1) * sizeof(npy_ucs4);
        return result;
    }

    int
    map_column_desc_types(column_desc& cd, bool unicode)
    /*
      infer the NumPy dtype and the sql_c_type to use from the
      sql_type.

      return 0 on success, 1 on failure

      remember to check support for any new NumPy type added in the function
      that handles nulls (fill_NAvalue)
     */
    {
        PyArray_Descr* dtype = 0;

#define MAP_SUCCESS(DTYPE, CTYPE) do {          \
        cd.npy_type_descr_ = DTYPE;            \
        cd.sql_c_type_ = CTYPE;                \
        return 0; } while (0)


        size_t sql_size = cd.sql_size_;


        switch (cd.sql_type_)
        {
            // string types ------------------------------------------------
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
        case SQL_GUID:
        case SQL_SS_XML:
            if (!unicode) {
                dtype = string_dtype(limit_text_size(sql_size));
                if (dtype) {
                    cd.element_buffer_size_ = dtype->elsize;
                    MAP_SUCCESS(dtype, SQL_C_CHAR);
                }
                break;
            }
            // else: fallthrough

        case SQL_WCHAR:
        case SQL_WVARCHAR:
        case SQL_WLONGVARCHAR:
            {
                dtype = unicode_dtype(limit_text_size(sql_size));
                if (dtype) {
                    cd.element_buffer_size_ = dtype->elsize;
                    MAP_SUCCESS(dtype, SQL_C_WCHAR);
                }
            }
            break;

            // real types --------------------------------------------------
        case SQL_REAL:
            dtype = PyArray_DescrFromType(NPY_FLOAT);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_FLOAT);
            }
            break;

        case SQL_FLOAT:
        case SQL_DOUBLE:
            dtype = PyArray_DescrFromType(NPY_DOUBLE);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_DOUBLE);
            }
            break;

            // integer types -----------------------------------------------
        case SQL_BIT:
            dtype = PyArray_DescrFromType(NPY_BOOL);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_BIT);
            }
            break;

        case SQL_TINYINT:
            dtype = PyArray_DescrFromType(NPY_UINT8);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_TINYINT);
            }
            break;

        case SQL_SMALLINT:
            dtype = PyArray_DescrFromType(NPY_INT16);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_SSHORT);
            }
            break;

        case SQL_INTEGER:
            dtype = PyArray_DescrFromType(NPY_INT32);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_SLONG);
            }
            break;

        case SQL_BIGINT:
            dtype = PyArray_DescrFromType(NPY_INT64);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_SBIGINT);
            }
            break;

            // time related types ------------------------------------------
        case SQL_TYPE_DATE:
            if (CAN_USE_DATETIME) {
                dtype = dtype_from_string("M8[D]");
                if (dtype) {
                    MAP_SUCCESS(dtype, SQL_C_TYPE_DATE);
                }
            }
            break;

        case SQL_TYPE_TIME:
        case SQL_SS_TIME2:
            if (CAN_USE_DATETIME) {
                dtype = dtype_from_string("m8[s]");
                if (dtype) {
                    MAP_SUCCESS(dtype, SQL_C_TYPE_TIME);
                }
            }
            break;

        case SQL_TYPE_TIMESTAMP:
            if (CAN_USE_DATETIME) {
                dtype = dtype_from_string("M8[us]");
                if (dtype) {
                    MAP_SUCCESS(dtype, SQL_C_TYPE_TIMESTAMP);
                }
            }
            break;

            // decimal -----------------------------------------------------
            // Note: these are mapped as double as per a request
            //       this means precision may be lost.
        case SQL_DECIMAL:
        case SQL_NUMERIC:
            dtype = PyArray_DescrFromType(NPY_DOUBLE);
            if (dtype) {
                MAP_SUCCESS(dtype, SQL_C_DOUBLE);
            }
            break;

            // unsupported types -------------------------------------------
            // this includes:
            // blobs:
            // SQL_BINARY, SQL_VARBINARY, SQL_LONGVARBINARY
        default:
            break;

        }
#undef MAP_SUCCESS

        TRACE_NOLOC("WARN: Failed translation of SQL\n\ttype: %s(%d)\n\tsize: %d\n\tuse_unicode: %s\n",
                    sql_type_to_str(cd.sql_type_), (int)cd.sql_type_, (int)cd.sql_size_,
                    unicode ? "Yes":"No");

        return 1;
    }

    struct query_desc
    {
        SQLRETURN init_from_statement(SQLHSTMT hstmt);
        SQLRETURN bind_cols();

        void lowercase_fields();
        int  translate_types(bool use_unicode);
        int  ensure();
        void convert(size_t read);
        void advance(size_t read);

        int allocate_buffers(size_t initial_result_count, size_t chunk_size, bool keep_nulls);
        int resize(size_t new_count);
        void cleanup();

        void dump_column_mapping() const;

        query_desc(): allocated_results_count_(0), chunk_size_(0), offset_(0) {}

        std::vector<column_desc> columns_;
        size_t allocated_results_count_;
        size_t chunk_size_;
        size_t offset_;
        SQLHSTMT hstmt_;
    };

    SQLRETURN
    query_desc::init_from_statement(SQLHSTMT hstmt)
    /*
      Fill the column descriptor from the sql statement handle hstmt.

      returns SQL_SUCCESS if successful, otherwise it returns the
      SQLRESULT from the SQL command that failed.
    */
    {
        cleanup();

        hstmt_ = hstmt;

        SQLRETURN ret;
        SQLSMALLINT field_count = 0;

        ret = SQLNumResultCols(hstmt, &field_count);

        if (!SQL_SUCCEEDED(ret))
            return ret;

        columns_.resize(field_count);
        // columns are 1 base on ODBC...
        for (SQLSMALLINT field = 1; field <= field_count; field++)
        {
            column_desc& c_desc = columns_[field-1];
            ret = SQLDescribeCol(hstmt,
                                 field,
                                 &c_desc.sql_name_[0],
                                 _countof(c_desc.sql_name_),
                                 NULL,
                                 &c_desc.sql_type_,
                                 &c_desc.sql_size_,
                                 &c_desc.sql_decimal_,
                                 &c_desc.sql_nullable_);

            if (!SQL_SUCCEEDED(ret))
                return ret;
        }

        return SQL_SUCCESS;
    }

    SQLRETURN
    query_desc::bind_cols()
    {
        SQLUSMALLINT col_number = 1;

        TRACE_NOLOC("\nBinding columns:\n");
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            void *bind_ptr;
            if (it->scratch_buffer_) {
                bind_ptr = it->scratch_buffer_;
            }
            else {
                PyArrayObject* array = it->npy_array_;
                bind_ptr = static_cast<void*>(PyArray_BYTES(array) +
                                              (offset_*PyArray_ITEMSIZE(array)));
            }

            TRACE_NOLOC("\tcolumn:%-10.10s address:%p %s\n",
                        it->sql_name_, bind_ptr,
                        bind_ptr==it->scratch_buffer_?"(scratch)":"");
            SQLRETURN status = SQLBindCol(hstmt_, col_number, it->sql_c_type_,
                                          bind_ptr, it->element_buffer_size_ ,
                                          it->null_buffer_);
            if (!SQL_SUCCEEDED(status)) {
                return status;
            }

            col_number++;
        }

        return SQL_SUCCESS;
    }

    void
    query_desc::lowercase_fields()
    /*
      Converts all the field names to lowercase
    */
    {
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            _strlwr((char*)&it->sql_name_[0]);
        }
    }

    int
    query_desc::translate_types(bool use_unicode)
    /*
      Performs the mapping of types from SQL to numpy dtype and C type.
      returns a count with the number of failed translations
    */
    {
        int failed_translations = 0;
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            failed_translations += map_column_desc_types(*it, use_unicode);
        }

        return failed_translations;
    }

    int
    query_desc::allocate_buffers(size_t buffer_element_count,
                                 size_t chunk_element_count,
                                 bool keep_nulls)
    /*
      allocate buffers to execute the query.
      row_count: initial rows to preallocate for the results
      chunk_row_count: rows to allocate for "per-chunk" buffers

      returns the number of failed allocations.
     */
    {
        int alloc_errors = 0;
        npy_intp npy_array_count = static_cast<npy_intp>(buffer_element_count);

        TRACE_NOLOC("\nAllocating arrays for column data:\n");
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            // Allocate the numpy buffer for the result
            PyObject *arr = PyArray_SimpleNewFromDescr(1, &npy_array_count,
                                                       it->npy_type_descr_);
            if (!arr) {
                // failed to allocate mem_buffer
                alloc_errors++;
                continue;
            }
            PyArrayObject *array = reinterpret_cast<PyArrayObject*>(arr);

            if (PyArray_ISSTRING(array)) {
                // clear memory on strings or undefined
                memset(PyArray_BYTES(array), 0, buffer_element_count*PyArray_ITEMSIZE(array));
            }

            it->npy_array_ = array;

            if (!arr)
                alloc_errors ++;

            TRACE_NOLOC("\tcolumn: %-10.10s address: %p\n", it->sql_name_, PyArray_DATA(array));
            // SimpleNewFromDescr steals the reference for the dtype
            Py_INCREF(it->npy_type_descr_);
            // if it is a type that needs to perform conversion,
            // allocate a buffer for the data to be read in.
            //
            // TODO: make the type logic decide what size per element
            // it needs (if any).  this will make the logic about
            // conversion simpler.
            switch (it->sql_c_type_)
            {
            case SQL_C_TYPE_DATE:
                {
                    void *mem = GUARDED_ALLOC(chunk_element_count *
                                              sizeof(DATE_STRUCT));
                    it->scratch_buffer_ = mem;
                    if (!mem)
                        alloc_errors ++;
                }
                break;
            case SQL_C_TYPE_TIMESTAMP:
                {
                    void *mem = GUARDED_ALLOC(chunk_element_count *
                                              sizeof(TIMESTAMP_STRUCT));
                    it->scratch_buffer_ = mem;
                    if (!mem)
                        alloc_errors ++;
                }
                break;
            case SQL_C_TYPE_TIME:
                {
                    void *mem = GUARDED_ALLOC(chunk_element_count *
                                              sizeof(TIME_STRUCT));
                    it->scratch_buffer_ = mem;
                    if (!mem)
                        alloc_errors ++;
                }
                break;
            case SQL_C_WCHAR:
                {
                    // this case is quite special, as a scratch
                    // buffer/conversions will only be needed when the
                    // underlying ODBC manager does not use UCS4 for
                    // its unicode strings.
                    //
                    // - MS ODBC manager uses UTF-16, which may
                    //   include surrogates (thus variable length encoded).
                    //
                    // - unixODBC seems to use UCS-2, which is
                    //   compatible with UTF-16, but may not include
                    //   surrogates limiting encoding to Basic
                    //   Multilingual Plane (not sure about this, it
                    //   will be handled using the same codepath as MS
                    //   ODBC, so it will work even if it produces
                    //   surrogates).
                    //
                    // - iODBC uses UCS-4 (UTF-32), so it shouldn't
                    //   need any kind of translation.
                    //
                    // In order to check if no translation is needed, the
                    // size of SQLWCHAR is used.
                    if (sizeof(SQLWCHAR) == 2) {
                        TRACE_NOLOC("\tscratch memory for unicode conversion (sizeof(SQLWCHAR) is %d)\n", (int)sizeof(SQLWCHAR));

                        size_t item_count = PyArray_ITEMSIZE(it->npy_array_) / sizeof(npy_ucs4);
                        // 2 due to possibility of surrogate.
                        // doing the math, the final buffer could be used instead of a scratch
                        // buffer, but would require code that can do the conversion in-place.
                        void *mem = GUARDED_ALLOC(chunk_element_count * item_count *
                                                  sizeof(SQLWCHAR) * 2);
                        it->scratch_buffer_ = mem;
                        if (!mem)
                            alloc_errors ++;
                    }
                }
                break;
            default:
                break;
            }

            if (it->sql_nullable_) {
                // if the type is nullable, allocate a buffer for null
                // data (ODBC buffer, that has SQLLEN size)
                void *mem = GUARDED_ALLOC(chunk_element_count * sizeof(SQLLEN));
                it->null_buffer_ = static_cast<SQLLEN*>(mem);
                if (!mem)
                    alloc_errors ++;

                if (keep_nulls)
                {
                    // also allocate a numpy array for bools if null data is wanted
                    arr =  PyArray_SimpleNew(1, &npy_array_count, NPY_BOOL);
                    it->npy_array_nulls_ = reinterpret_cast<PyArrayObject*>(arr);
                    if (!it->npy_array_nulls_)
                        alloc_errors++;
                }
            }
        }

        if (!alloc_errors)
        {
            allocated_results_count_ = buffer_element_count;
            chunk_size_ = chunk_element_count;
        }

        return alloc_errors;
    }

    int
    query_desc::resize(size_t new_size)
    /*
      resize the numpy array elements to the new_size.
      the chunk_size and associated buffers are to be preserved.
     */
    {
        int alloc_fail = 0;
        npy_intp size = static_cast<npy_intp>(new_size);
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            void *old_data=PyArray_DATA(it->npy_array_);
            int failed = resize_array(it->npy_array_, size);
            void *new_data=PyArray_DATA(it->npy_array_);

            TRACE_NOLOC("Array for column %s moved. %p -> %p", it->sql_name_, old_data, new_data);
            // if it has an array for nulls, resize it as well
            if (it->npy_array_nulls_)
            {
                failed += resize_array(it->npy_array_nulls_, size);
            }

            if (failed)
                alloc_fail += failed;
        }

        if (!alloc_fail)
            allocated_results_count_ = new_size;

        return alloc_fail;
    }

    int
    query_desc::ensure()
    /*
      make sure there is space allocated for the next step
      return 0 if everything ok, any other value means a problem was found
      due to resizing
     */
    {
        if (allocated_results_count_ < offset_ + chunk_size_)
        {
            return resize(offset_ + chunk_size_);
        }

        return 0;
    }

    void
    query_desc::convert(size_t count)
    /*
      Converts any column that requires conversion from the type returned
      by odbc to the type expected in numpy. Right now this is only needed
      for fields related to time. Note that odbc itself may handle other
      conversions, like decimal->double with the appropriate SQLBindCol.

      The conversion also includes the handling of nulls. In the case of
      NULL a default value is inserted in the resulting column.
     */
    {
        for (std::vector<column_desc>::iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            // TODO: It should be possible to generalize this and make it
            //       more convenient to add types if a conversion function
            //       was placed in the column structure.
            //       Probably nulls could be handled by that conversion
            //       function as well.
            if (it->scratch_buffer_) { // a conversion is needed
                CHECK_ALLOC_GUARDS(it->scratch_buffer_,
                                   "scratch buffer for field %s\n",
                                   it->sql_name_);
                convert_buffer(it->npy_array_,
                               it->scratch_buffer_, it->sql_c_type_,
                               offset_, count);
            }

            if (it->null_buffer_) { // nulls are present
                CHECK_ALLOC_GUARDS(it->null_buffer_,
                                   "null buffer for field %s\n",
                                   it->sql_name_);
                fill_NAarray(it->npy_array_,
                             it->npy_array_nulls_,
                             it->null_buffer_,
                             offset_, count);
            }
        }
    }

    void
    query_desc::advance(size_t count)
    /*
      Advance the current position
     */
    {
        offset_ += count;
    }

    void
    query_desc::cleanup()
    {
        std::vector<column_desc> tmp;
        columns_.swap(tmp);
    }

    void
    query_desc::dump_column_mapping() const
    {
        const char* fmt_str_head = "%-20.20s %-15.15s %-10.10s %-8.8s %-20.20s\n";
        const char* fmt_str = "%-20.20s %-15.15s %-10u %-8.8s %-20.20s\n";
        const char* dashes = "----------------------------------------";
        TRACE_NOLOC(fmt_str_head, "name", "sql type", "size", "null?", "c type");
        TRACE_NOLOC(fmt_str_head, dashes, dashes, dashes, dashes, dashes);
        for (std::vector<column_desc>::const_iterator it = columns_.begin();
             it < columns_.end(); ++it)
        {
            TRACE_NOLOC(fmt_str, it->sql_name_, sql_type_to_str(it->sql_type_),
                        it->sql_size_,
                        it->sql_nullable_?"null":"not null",
                        sql_c_type_to_str(it->sql_c_type_));
        }
    }
}

size_t
print_error_types(query_desc& qd, size_t err_count, char *buff,
                  size_t buff_size)
{
    size_t acc = snprintf(buff, buff_size,
                          "%d fields with unsupported types found:\n",
                          (int)err_count);

    for (std::vector<column_desc>::iterator it = qd.columns_.begin();
         it < qd.columns_.end(); ++it)
    {
        if (0 == it->npy_type_descr_) {
            // if numpy type descr is empty means a failed translation.
            acc += snprintf(buff+acc, acc < buff_size? buff_size - acc : 0,
                            "\t'%s' type: %s (%d) size: %d decimal: %d\n",
                            it->sql_name_,
                            sql_type_to_str(it->sql_type_), (int)it->sql_type_,
                            (int)it->sql_size_, (int)it->sql_decimal_);
        }
    }

    return acc;
}

int
raise_unsupported_types_exception(int err_count, query_desc& qd)
{
    char error[4096];
    char *use_string = error;
    size_t count = print_error_types(qd, err_count, error, sizeof(error));

    if (count >= sizeof(error))
    {
        // did not fit, truncated
        char *error_alloc = (char*)GUARDED_ALLOC(count);
        if (error_alloc) {
            use_string = error_alloc;
            print_error_types(qd, count, error_alloc, count);
        }
    }

    RaiseErrorV(0, PyExc_TypeError, use_string);

    if (use_string != error) {
        // we had to allocate
        GUARDED_DEALLOC(use_string);
    }
    return 0;
}

/**
// Takes an ODBC cursor object and creates a Python dictionary of
// NumPy arrays. It also creates some helpers for the NULLS, and
// datetimes.
//
// This is called after the ODBC query is complete.
//
// @param cur    The ODBC cursor object.
//
// @param nrows  The number of rows that were returned by the query.
//
// @param lower If true, makes the column names in the NumPy dtype all
//              lowercase.
//
// @returns 0 on success
*/
static int
perform_array_query(query_desc& result, Cursor* cur, npy_intp nrows, bool lower, bool want_nulls)
{
    SQLRETURN rc;
    bool use_unicode = cur->cnxn->unicode_results;
    size_t outsize, chunk_size;

    if (nrows < 0) {
        // chunked, no know final size
        outsize = DEFAULT_ROWS_TO_BE_ALLOCATED;
        chunk_size = DEFAULT_ROWS_TO_BE_FETCHED;
    }
    else {
        // all in one go
        outsize = static_cast<size_t>(nrows);
        chunk_size = static_cast<size_t>(nrows);
    }

    I(cur->hstmt != SQL_NULL_HANDLE && cur->colinfos != 0);

    if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
    {
        /*
          Is this needed or just convenient?
          Won't ODBC fail gracefully (through an ODBC error code) when
          trying to use a bad handle?
         */
        return 0 == RaiseErrorV(0, ProgrammingError,
                                "The cursor's connection was closed.");
    }

    {
        PyNoGIL ctxt;
        rc = result.init_from_statement(cur->hstmt);
    }

    if (cur->cnxn->hdbc == SQL_NULL_HANDLE)
    {
        // The connection was closed by another thread in the
        // ALLOW_THREADS block above.
        return 0 == RaiseErrorV(0, ProgrammingError,
                                "The cursor's connection was closed.");
    }

    if (!SQL_SUCCEEDED(rc))
    {
        // Note: The SQL Server driver sometimes returns HY007 here if
        // multiple statements (separated by ;) were submitted.  This
        // is not documented, but I've seen it with multiple
        // successful inserts.
        return 0 == RaiseErrorFromHandle("ODBC failed to describe the resulting columns",
                                         cur->cnxn->hdbc, cur->hstmt);
    }

    if (lower)
        result.lowercase_fields();

    int unsupported_fields = result.translate_types(use_unicode);
    if (unsupported_fields)
    {
        // TODO: add better diagnosis, pointing out the fields and
        // their types in a human readable form.
        return 0 == raise_unsupported_types_exception(unsupported_fields, result);
    }

    if (pyodbc_tracing_enabled)
        result.dump_column_mapping();

    int allocation_errors = result.allocate_buffers(outsize, chunk_size, want_nulls);
    if (allocation_errors)
    {
        return 0 == RaiseErrorV(0, PyExc_MemoryError,
                                "Can't allocate result buffers",
                                outsize);
    }

    fetch_status status(cur->hstmt, result.chunk_size_);
    do {
        TRACE_NOLOC("Fetching %d rows..\n", result.chunk_size_);
        int error = result.ensure();
        if (error) {
            return 0 == RaiseErrorV(0, PyExc_MemoryError,
                                    "Can't allocate result buffers");
        }

        rc = result.bind_cols();
        if (!SQL_SUCCEEDED(rc)) {
            return 0 == RaiseErrorFromHandle("ODBC failed when binding columns",
                                             cur->cnxn->hdbc, cur->hstmt);

        }

        // Do the fetch
        {
            PyNoGIL ctxt;
            rc = SQLFetchScroll(status.hstmt_, SQL_FETCH_NEXT, 0);
        }


        // Sometimes (test_exhaust_execute_buffer), the SQLite ODBC
        // driver returns an error here, but it should not!  I'm not
        // sure that this solution is the correct one, but anyway.
        if ((rc == SQL_NO_DATA) || (rc == -1)) {  // XXX
        //if (rc == SQL_NO_DATA) {
            TRACE_NOLOC("No more data available (%d)\n", (int)rc);
            break;
        }
        else if (rc < 0) {
            PyErr_SetString(PyExc_RuntimeError, "error in SQLFetchScroll");
            return rc;
        }

        // The next check creates false positives on SQLite, as the
        // NumRowsFetched seems arbitrary (i.e. not set).  Probably
        // reveals a problem in the ODBC driver.
        if (status.rows_read_ > static_cast<SQLLEN>(result.chunk_size_)) {
            // Let's reset its value to 0 instead (the most probable value here)
            TRACE_NOLOC("WARN: rows read reported is greater than requested (Read: %d, Requested: %d)\n",
                        static_cast<int>(status.rows_read_),
                        static_cast<int>(result.chunk_size_));

            status.rows_read_ = 0;
        }

        TRACE_NOLOC("\nConverting %d row(s)\n", status.rows_read_);
        result.convert(status.rows_read_);
        result.advance(status.rows_read_);

        // This exits the loop when the amount of rows was known
        // a-priori, so it is enough with a single call
        if (nrows >= 0)
            break;

        // We assume that when the number of rows read is lower than
        // the number we asked for, this means we are done.
    } while(status.rows_read_ == static_cast<SQLLEN>(result.chunk_size_));

    // Finally, shrink size of final container, if needed
    if (result.offset_ < result.allocated_results_count_) {
        int alloc_failures = result.resize(result.offset_);
        if (alloc_failures) {
            // note that this shouldn't be happening, as a shrinking realloc
            // should always succeed!
            TRACE_NOLOC("WARN: Unexpected failure when trying to shrink arrays");
            return 0 == RaiseErrorV(0, PyExc_MemoryError,
                                    "Can't allocate result buffers");
        }
    }

    return 0;
}


static PyObject*
query_desc_to_dictarray(query_desc& qd, const char *null_suffix)
/*
  Build a dictarray (dictionary of NumPy arrays) from the query_desc

  returns the python dictionary object, or 0 if an error occurred. In case
  of an error the appropriate python exception is raised.
 */
{
    PyObject *dictarray = PyDict_New();

    if (dictarray) {
        for (std::vector<column_desc>::iterator it = qd.columns_.begin();
             it < qd.columns_.end(); ++it)
        {
            int rv;
            rv = PyDict_SetItemString(dictarray,
                                      reinterpret_cast<char*>(it->sql_name_),
                                      reinterpret_cast<PyObject*>(it->npy_array_));

            if (rv < 0) {
                /* out of mem is very likely here */
                Py_DECREF(dictarray);
                return 0;
            }

            if (it->npy_array_nulls_) {
                char column_nulls_name[350];
                snprintf(column_nulls_name, sizeof(column_nulls_name), "%s%s",it->sql_name_,null_suffix);
                rv = PyDict_SetItemString(dictarray,
                                          column_nulls_name,
                                          reinterpret_cast<PyObject*>(it->npy_array_nulls_));
                if (rv < 0) {
                    Py_DECREF(dictarray);
                    return 0;
                }
            }
        }
    }

    return dictarray;
}

//
// Create and fill a dictarray out of a query
//
// arguments:
//   cursor - cursor object to fetch the rows from
//   nrows - number of rows to fetch, -1 for all rows
//   null_suffix - suffix to add to the column name for the bool column holding the nulls. NULL means we don't want nulls.
static PyObject*
create_fill_dictarray(Cursor* cursor, npy_intp nrows, const char* null_suffix)
{
    int error;
    query_desc qd;

    error = perform_array_query(qd, cursor, nrows, lowercase(), null_suffix != 0);
    if (error) {
        TRACE_NOLOC("WARN: perform_querydesc returned %d errors\n", error);
        return 0;
    }

    TRACE_NOLOC("\nBuilding dictarray.\n");
    PyObject *dictarray = query_desc_to_dictarray(qd, null_suffix);
    if (!dictarray) {
        TRACE_NOLOC("WARN: Failed to build dictarray from the query results.\n");
        return 0;
    }

    return dictarray;
}


static PyArray_Descr*
query_desc_to_record_dtype(query_desc &qd, const char *null_suffix)
/*
  Build a record dtype from the column information in a query
  desc.

  returns the dtype (PyArray_Descr) on success with a reference
  count. 0 if something failed. On failure the appropriate python
  exception will be already raised.

  In order to create the structured dtype, PyArray_DescrConverter is
  called passing a dictionary that maps the fields.
 */
{
    PyObject* record_dict = 0;

    record_dict = PyDict_New();

    if (!record_dict)
        return 0;

    long offset = 0;
    for (std::vector<column_desc>::iterator it = qd.columns_.begin();
         it < qd.columns_.end(); ++it) {
        PyObject *field_desc = PyTuple_New(2);

        if (!field_desc) {
            Py_DECREF(record_dict);
            return 0; /* out of memory? */
        }

        // PyTuple_SET_ITEM steals the reference, we want to keep one
        // reference for us. We don't want the extra checks made by
        // PyTuple_SetItem.
        Py_INCREF(it->npy_type_descr_);
        PyTuple_SET_ITEM(field_desc, 0, (PyObject*)it->npy_type_descr_);
        PyTuple_SET_ITEM(field_desc, 1, PyInt_FromLong(offset));

        int not_inserted = PyDict_SetItemString(record_dict, (const char*) it->sql_name_,
                                                field_desc);
        Py_DECREF(field_desc);
        if (not_inserted) {
            Py_DECREF(record_dict);
            return 0; /* out of memory? */
        }

        offset += it->npy_type_descr_->elsize;

        // handle nulls...
        if (it->npy_array_nulls_) {
            field_desc = PyTuple_New(2);
            if (!field_desc)
            {
                Py_DECREF(record_dict);
                return 0;
            }

            PyArray_Descr *descr = PyArray_DESCR(it->npy_array_nulls_);
            Py_INCREF(descr);
            PyTuple_SET_ITEM(field_desc, 0, (PyObject*)descr);
            PyTuple_SET_ITEM(field_desc, 1, PyInt_FromLong(offset));
            char null_column_name[350];
            snprintf(null_column_name, sizeof(null_column_name),
                     "%s%s", it->sql_name_, null_suffix);

            not_inserted = PyDict_SetItemString(record_dict, null_column_name, field_desc);
            Py_DECREF(field_desc);
            if (not_inserted) {
                Py_DECREF(record_dict);
                return 0;
            }
            offset += descr->elsize;
        }
    }

    PyArray_Descr *dtype=0;
    int success = PyArray_DescrConverter(record_dict, &dtype);
    Py_DECREF(record_dict);
    if (!success) {
        RaiseErrorV(0, ProgrammingError,
                    "Failed conversion from dict type into a NumPy record dtype");
        return 0;
    }

    return dtype;
}

static PyArrayObject*
query_desc_to_sarray(query_desc &qd, const char *null_suffix)
/*
  Build a sarray (structured array) from a query_desc.
 */
{
    // query_desc contains "column-wise" results as NumPy arrays. In a
    // sarray we want the data row-wise (structured layout). This
    // means a whole new array will need to be allocated and memory
    // copying will be needed.

    // 1. build the record dtype.
    PyArray_Descr *dtype = query_desc_to_record_dtype(qd, null_suffix);

    if (!dtype) {
        TRACE_NOLOC("WARN: failed to create record dtype.\n");
        return 0;
    }

    // 2. build the NumPy Array. It is not needed to clear any data
    // (even string data) as everything will be overwritten when
    // copying the column arrays. The column arrays where already
    // properly initialized before fetching the data.
    npy_intp dims = (npy_intp)qd.allocated_results_count_;
    PyArrayObject* sarray = reinterpret_cast<PyArrayObject*>(PyArray_SimpleNewFromDescr(1, &dims, dtype));
    // note: dtype got its reference stolen, but it is still valid as
    // long as the array is valid. The reference is stolen even if the
    // array fails to create (according to NumPy source code).

    if (!sarray) {
        TRACE_NOLOC("WARN: failed to create structured array.\n");
        return 0;
    }

    // 3. copy the data into the structured array. Note: the offsets
    //    will be the same as in the record array by construction.
    {
        PyNoGIL no_gil;
        long offset = 0;
        for (std::vector<column_desc>::iterator it = qd.columns_.begin();
             it < qd.columns_.end(); ++it)
        {
            size_t sarray_stride = PyArray_ITEMSIZE(sarray);
            char *sarray_data = PyArray_BYTES(sarray) + offset;
            size_t carray_stride = PyArray_ITEMSIZE(it->npy_array_);
            char *carray_data = PyArray_BYTES(it->npy_array_);
            // this approach may not be the most efficient, but it is good enough for now.
            // TODO: make the transform in a way that is sequential on the write stream
            for (size_t i = 0; i < qd.allocated_results_count_; ++i)
            {
                memcpy(sarray_data, carray_data, carray_stride);
                sarray_data += sarray_stride;
                carray_data += carray_stride;
            }

            offset += carray_stride;

            if (it->npy_array_nulls_)
            {
                // TODO: refactor this code that is duplicated
                sarray_stride = PyArray_ITEMSIZE(sarray);
                sarray_data = PyArray_BYTES(sarray) + offset;
                carray_stride = PyArray_ITEMSIZE(it->npy_array_nulls_);
                carray_data = PyArray_BYTES(it->npy_array_nulls_);
                // this approach may not be the most efficient, but it is good enough for now.
                // TODO: make the transform in a way that is sequential on the write stream
                for (size_t i = 0; i < qd.allocated_results_count_; ++i)
                {
                    memcpy(sarray_data, carray_data, carray_stride);
                    sarray_data += sarray_stride;
                    carray_data += carray_stride;
                }

                offset += carray_stride;
            }
        }
    }

    return sarray;
}

static PyObject*
create_fill_sarray(Cursor* cursor, npy_intp nrows, const char* null_suffix)
{
    int error;
    query_desc qd;

    error = perform_array_query(qd, cursor, nrows, lowercase(), null_suffix != 0);
    if (error) {
        TRACE_NOLOC("perform_querydesc returned %d errors\n", error);
        return 0;
    }

    TRACE_NOLOC("\nBuilding sarray\n");
    PyObject *sarray = reinterpret_cast<PyObject*>(query_desc_to_sarray(qd, null_suffix));
    if (!sarray) {
        TRACE_NOLOC("WARN: Failed to build sarray from the query results.\n");
    }

    return sarray;
}


// -----------------------------------------------------------------------------
// Method implementation
// -----------------------------------------------------------------------------
static char *Cursor_npfetch_kwnames[] = {
    "size", // keyword to read the maximum number of rows. Defaults to all.
    "return_nulls", // keyword to make a given fetch to add boolean columns for nulls
    "null_suffix", // keyword providing the string to use as suffix
};


//
// The main cursor.fetchsarray() method
//
PyObject*
Cursor_fetchsarray(PyObject *self, PyObject *args, PyObject *kwargs)
{
    Cursor* cursor = Cursor_Validate(self, CURSOR_REQUIRE_RESULTS | CURSOR_RAISE_ERROR);
    if (!cursor)
        return 0;

    TRACE("\n\nParse tuple\n");
    ssize_t nrows = -1;
    const char *null_suffix = "_isnull";
    PyObject *return_nulls = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|nOs", Cursor_npfetch_kwnames,
                                     &nrows, &return_nulls, &null_suffix))
        return 0;

    bool preserve_nulls = return_nulls ? PyObject_IsTrue(return_nulls) : false;

    TRACE_NOLOC("\n\nCursor fetchsarray\n\tnrows:%d\n\treturn_nulls:%s\n\tnull_suffix:%s\n\thandle:%p\n\tunicode_results:%s\n",
                (int)nrows, preserve_nulls?"Yes":"No", null_suffix, (void*)cursor->hstmt,
                cursor->cnxn->unicode_results?"Yes":"No");
    npy_intp arg = nrows;
    PyObject* rv = create_fill_sarray(cursor, arg, preserve_nulls?null_suffix:0);
    TRACE_NOLOC("\nCursor fetchsarray done.\n\tsarray: %p\n\n", rv);

    return rv;
}

//
// The main cursor.fetchdict() method
//

PyObject*
Cursor_fetchdictarray(PyObject* self, PyObject* args, PyObject *kwargs)
{
    Cursor* cursor = Cursor_Validate(self, CURSOR_REQUIRE_RESULTS | CURSOR_RAISE_ERROR);
    if (!cursor)
        return 0;

    /*
        note: ssize_t is used as a type for parse tuple as it looks like
        the integer in ParseTuple that is more likely to have the same size
        as a npy_intp
    */
    TRACE("\n\nParse tuple\n");
    ssize_t nrows = -1;
    PyObject *return_nulls = NULL;
    const char *null_suffix = "_isnull";
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|nOs", Cursor_npfetch_kwnames,
                                     &nrows, &return_nulls, &null_suffix))
        return 0;

    bool preserve_nulls = return_nulls?PyObject_IsTrue(return_nulls):false;
    TRACE("Foo\n");
    TRACE_NOLOC("\n\nCursor fetchdictarray\n\tnrows:%d\n\treturn_nulls:%s\n\tnull_suffix:%s\n\thandle:%p\n\tunicode_results:%s\n",
                (int)nrows, preserve_nulls?"yes":"no", null_suffix, (void*)cursor->hstmt,
                cursor->cnxn->unicode_results?"Yes":"No");
    npy_intp arg = nrows;
    PyObject *rv = create_fill_dictarray(cursor, arg, preserve_nulls?null_suffix:0);
    TRACE_NOLOC("\nCursor fetchdictarray done.\n\tdictarray: %p\n\n", rv);
    return rv;
}

char fetchdictarray_doc[] =
    "fetchdictarray(size=-1, return_nulls=False, null_suffix='_isnull')\n" \
    "                               --> a dictionary of column arrays.\n" \
    "\n"
    "Fetch as many rows as specified by size into a dictionary of NumPy\n" \
    "ndarrays (dictarray). The dictionary will contain a key for each column,\n"\
    "with its value being a NumPy ndarray holding its value for the fetched\n" \
    "rows. Optionally, extra columns will be added to signal nulls on\n" \
    "nullable columns.\n" \
    "\n" \
    "Parameters\n" \
    "----------\n" \
    "size : int, optional\n" \
    "    The number of rows to fetch. Use -1 (the default) to fetch all\n" \
    "    remaining rows.\n" \
    "return_nulls : boolean, optional\n" \
    "    If True, information about null values will be included adding a\n" \
    "    boolean array using as key a string  built by concatenating the\n" \
    "    column name and null_suffix.\n"                              \
    "null_suffix : string, optional\n" \
    "    A string used as a suffix when building the key for null values.\n"\
    "    Only used if return_nulls is True.\n" \
    "\n" \
    "Returns\n" \
    "-------\n" \
    "out: dict\n" \
    "    A dictionary mapping column names to an ndarray holding its values\n" \
    "    for the fetched rows. The dictionary will use the column name as\n" \
    "    key for the ndarray containing values associated to that column.\n" \
    "    Optionally, null information for nullable columns will be provided\n" \
    "    by adding additional boolean columns named after the nullable column\n"\
    "    concatenated to null_suffix\n" \
    "\n" \
    "Remarks\n" \
    "-------\n" \
    "Similar to fetchmany(size), but returning a dictionary of NumPy ndarrays\n" \
    "for the results instead of a Python list of tuples of objects, reducing\n" \
    "memory footprint as well as improving performance.\n" \
    "fetchdictarray is overall more efficient that fetchsarray.\n" \
    "\n" \
    "See Also\n" \
    "--------\n" \
    "fetchmany : Fetch rows into a Python list of rows.\n" \
    "fetchall : Fetch the remaining rows into a Python lis of rows.\n" \
    "fetchsarray : Fetch rows into a NumPy structured ndarray.\n" \
    "\n";


#if PY_VERSION_HEX >= 0x03000000
int NpContainer_init()
#else
void NpContainer_init()
#endif
{
    import_array();
    // If the version of Numpy is >= API 7 (Numpy 1.7),
    // then enable datetime features. This allows datetime
    // to work even if pyodbc is built against Numpy 1.5.
    if (PyArray_GetNDArrayCFeatureVersion() >= 7) {
        CAN_USE_DATETIME = true;
    }

#if PY_VERSION_HEX >= 0x03000000
    return 0;
#else
    return;
#endif
}
