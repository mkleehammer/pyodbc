// The SQL Server bulk copy (BCP) fast path for Cursor.executemany.
//
// When cursor.fast_executemany and cursor.use_bcp_fast are set, an INSERT of a single
// parameterized VALUES row is carried out with the BCP API instead of an array of parameter
// sets.  The path is an optimisation only: it must store exactly what the plain fast path would,
// so anything it cannot reproduce faithfully is detected before bcp_init and left to that path.
// Once rows are streaming, a failure rolls back everything this call sent and raises.  Values
// the path converts itself, where the driver's conversion would differ from the server's, are
// shaped in bcp_convert.cpp.

#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "cursor.h"
#include "errors.h"
#include "bcp_support.h"
#include "bcp_convert.h"
#include <datetime.h>
#include <cmath>

void BcpExecuteMany_init()
{
    PyDateTime_IMPORT;
    BcpConvert_init();
}

/**
 * The setinputsizes entry for a parameter, by the SQL type the plain path then sends each value
 * as, for the server to convert to the column's type.  ENTRY_NONE is a missing or None entry,
 * which leaves the parameter typed as its column.
 */
enum Entry
{
    ENTRY_NONE,
    ENTRY_TEXT,
    ENTRY_BIT,
    ENTRY_BIGINT,
    ENTRY_DOUBLE,
    ENTRY_NUMERIC,
    ENTRY_TIMESTAMP,
    ENTRY_DATE,
};

/**
 * One bound column: where it goes, what the rows hold for it, and its capacity in characters
 * (text) or bytes (binary), or -1 where no limit applies.  `entry` is its parameter's
 * setinputsizes entry, with that entry's size (the nvarchar length or numeric precision) and
 * digits (the numeric scale).  `exactChars` is the bitmap map_exact_chars makes, or null, and
 * `asciiText` says the column takes only ASCII text (see takes_only_ascii).
 */
struct ColumnPlan
{
    const BcpTargetColumn* target;
    int ordinal;
    int destination;
    long capacity;
    unsigned classes;
    PyTypeObject* lastType;
    unsigned lastClass;
    int entry;
    long entrySize;
    long entryDigits;
    uint32_t* exactChars;
    bool asciiText;
};

/**
 * How the load runs inside a transaction: in autocommit mode autocommit is suspended so the
 * load is its own transaction; in manual-commit mode a savepoint marks where it started.
 */
struct LoadTransaction
{
    bool autocommitSuspended;
    bool savepoint;
};

/**
 * Everything a load allocates, freed when it goes out of scope.
 *
 * `cols` holds the ncols columns the rows' values go to, then nfill filler columns: the table
 * columns after the last of those, which BCP requires bound and which are always NULL so the
 * server fills them in.  keepNulls says whether BCPKEEPNULLS is set.
 *
 * The load holds its own reference to the connection, as Python code run while a row is filled
 * (a Decimal's normalize, say) can close the cursor, which drops the cursor's reference to it.
 */
struct BcpLoad
{
    Object cnxnRef;
    Cursor* cursor;
    Connection* cnxn;
    BcpInsert insert;
    bool insertParsed;
    BcpTarget target;
    ColumnPlan* plans;
    BcpCol* cols;
    int ncols;
    int nfill;
    bool keepNulls;
    Object rows;
    Object decimalType;
    Object decimalFormat;
    Object normalizeName;

    explicit BcpLoad(Cursor* cur)
        : cnxnRef(Py_NewRef((PyObject*)cur->cnxn)), cursor(cur), cnxn(cur->cnxn), insertParsed(false),
          plans(0), cols(0), ncols(0), nfill(0), keepNulls(true)
    {
        target.ncols = 0;
        target.cols = 0;
    }

    BcpLoad(const BcpLoad&) = delete;
    BcpLoad& operator=(const BcpLoad&) = delete;

    ~BcpLoad()
    {
        if (insertParsed)
            BcpFreeInsert(&insert);
        BcpFreeTarget(&target);
        for (int i = 0; plans && i < ncols; ++i)
            PyMem_Free(plans[i].exactChars);
        PyMem_Free(plans);
        BcpFreeColumns(cols, ncols + nfill);
    }
};

/**
 * The category of a destination column, or DEST_UNSUPPORTED for one the path leaves to the
 * plain path.
 */
static int destination_of(const BcpTargetColumn& col)
{
    switch (col.systemType) {
    case 35: case 99: case 167: case 175: case 231: case 239:
        return DEST_CHAR;
    case 241:
        return DEST_XML;
    case 34: case 165: case 173:
        return DEST_BINARY;
    case 48: case 52: case 56: case 127:
        return DEST_INTEGER;
    case 104:
        return DEST_BIT;
    case 59: case 62:
        return DEST_FLOAT;
    case 106: case 108:
        return DEST_DECIMAL;
    case 60: case 122:
        return DEST_MONEY;
    case 40:
        return DEST_DATE;
    case 41:
        return DEST_TIME;
    case 42:
        return DEST_DATETIME2;
    case 43:
        return DEST_DATETIMEOFFSET;
    case 61:
        return DEST_DATETIME;
    case 58:
        return DEST_SMALLDATETIME;
    case 36:
        return DEST_GUID;
    }
    return DEST_UNSUPPORTED;
}

/**
 * The length limit the plain path enforces before sending, in UTF-16 units for char, varchar,
 * nchar and nvarchar and in bytes for binary and varbinary; -1 for (max), xml and the legacy
 * text, ntext and image types, which have none that matters.
 */
static long capacity_of(const BcpTargetColumn& col)
{
    if (col.maxLength == -1)
        return -1;
    switch (col.systemType) {
    case 167: case 175: case 165: case 173:
        return col.maxLength;
    case 231: case 239:
        return col.maxLength / 2;
    }
    return -1;
}

/** Whether a column stores text in its collation's code page: char, varchar and text. */
static bool is_code_page_text(const BcpTargetColumn& col)
{
    return col.systemType == 35 || col.systemType == 167 || col.systemType == 175;
}

/**
 * Whether the driver can send only ASCII text to a column: one with a UTF-8 collation, before
 * Driver 17.4, the first to convert text to UTF-8 for the server.  Older drivers send other
 * text as bytes the server rejects.
 */
static bool takes_only_ascii(const BcpLoad& load, const BcpTargetColumn& col)
{
    return is_code_page_text(col) && col.codePage == 65001 && load.cnxn->bcp->driverVersion < 1704;
}

static bool names_equal(const SQLWCHAR* a, const SQLWCHAR* b, bool foldAsciiCase)
{
    for (; *a && *b; ++a, ++b) {
        SQLWCHAR ca = *a, cb = *b;
        if (foldAsciiCase) {
            if (ca >= 'A' && ca <= 'Z') ca = (SQLWCHAR)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (SQLWCHAR)(cb + 32);
        }
        if (ca != cb)
            return false;
    }
    return *a == 0 && *b == 0;
}

/**
 * Finds the target column a listed name refers to: an exact match, else, in a database that
 * compares identifiers ignoring case, the only match that differs in ASCII case.  A name that
 * matches only by folding other letters is left unmatched, so the plain path resolves it.
 * Returns -1 if there is none, -2 on allocation failure.
 */
static int find_target_column(const BcpTarget& target, const char* name)
{
    SQLWCHAR* wname = BcpUtf8ToSqlWChar(name);
    if (!wname)
        return -2;
    int exact = -1, folded = -1, nfolded = 0;
    for (int i = 0; i < target.ncols && exact < 0; ++i) {
        if (names_equal(target.cols[i].name, wname, false))
            exact = i;
        else if (target.caseInsensitive && names_equal(target.cols[i].name, wname, true)) {
            folded = i;
            ++nfolded;
        }
    }
    PyMem_Free(wname);
    if (exact >= 0)
        return exact;
    return nfolded == 1 ? folded : -1;
}

static bool is_bindable(const BcpTargetColumn& col)
{
    return !col.identity && !col.computed && destination_of(col) != DEST_UNSUPPORTED;
}

/**
 * Maps the INSERT's values to target columns as the INSERT itself would.
 *
 * A column list names its columns; the unlisted ones are left for the server to fill in
 * (identity, computed, DEFAULT or NULL) exactly as for the INSERT.  Without a list the values
 * go to every column except identity and computed ones, which is how T-SQL reads a positional
 * INSERT.  Anything the INSERT would reject is left to the plain path to report.
 *
 * Unbound columns before the last bound one need nothing, but BCP requires the columns after it
 * bound, so those become filler columns bound to NULL, which the server fills in the same way
 * (see choose_keep_nulls for columns with a DEFAULT).
 *
 * @return 1 if mapped, 0 to fall back, -1 on allocation failure (Python exception set).
 */
static int map_columns(BcpLoad& load)
{
    const BcpInsert& ins = load.insert;
    const BcpTarget& target = load.target;
    load.plans = (ColumnPlan*)PyMem_Calloc((size_t)ins.nmarkers, sizeof(ColumnPlan));
    if (!load.plans) {
        PyErr_NoMemory();
        return -1;
    }

    int n = 0;
    if (ins.nnames) {
        if (ins.nnames != ins.nmarkers)
            return 0;
        for (int i = 0; i < ins.nnames; ++i) {
            int t = find_target_column(target, ins.names[i]);
            if (t == -2)
                return -1;
            if (t < 0 || !is_bindable(target.cols[t]))
                return 0;
            for (int j = 0; j < n; ++j)
                if (load.plans[j].ordinal == t + 1)
                    return 0;
            load.plans[n].target = &target.cols[t];
            load.plans[n++].ordinal = t + 1;
        }
    } else {
        for (int t = 0; t < target.ncols; ++t) {
            const BcpTargetColumn& col = target.cols[t];
            if (col.identity || col.computed)
                continue;
            if (n == ins.nmarkers || destination_of(col) == DEST_UNSUPPORTED)
                return 0;
            load.plans[n].target = &col;
            load.plans[n++].ordinal = t + 1;
        }
        if (n != ins.nmarkers)
            return 0;
    }
    int last = 0;
    for (int i = 0; i < n; ++i) {
        load.plans[i].destination = destination_of(*load.plans[i].target);
        load.plans[i].capacity = capacity_of(*load.plans[i].target);
        load.plans[i].asciiText = takes_only_ascii(load, *load.plans[i].target);
        if (load.plans[i].ordinal > last)
            last = load.plans[i].ordinal;
    }
    load.ncols = n;
    load.nfill = target.ncols - last;
    return 1;
}

/**
 * Decides whether to set BCPKEEPNULLS.  With it, a NULL bound to a column with a DEFAULT is
 * stored as NULL, as an INSERT stores an explicit NULL; without it BCP stores the default, as
 * the INSERT does for a column it leaves out.  It only affects columns with a default, so it is
 * cleared when a filler column has one, which is only safe if no value column with a default
 * receives a NULL.
 *
 * @return false to fall back.
 */
static bool choose_keep_nulls(BcpLoad& load)
{
    bool fillerDefault = false;
    for (int t = load.target.ncols - load.nfill; t < load.target.ncols; ++t)
        fillerDefault = fillerDefault || load.target.cols[t].hasDefault;
    if (!fillerDefault)
        return true;
    for (int i = 0; i < load.ncols; ++i)
        if (load.plans[i].target->hasDefault && (load.plans[i].classes & VAL_NONE))
            return false;
    load.keepNulls = false;
    return true;
}

static bool read_entry_field(PyObject* entry, Py_ssize_t i, long* value)
{
    PyObject* item = PyTuple_GET_ITEM(entry, i);
    if (!PyLong_Check(item) || PyBool_Check(item))
        return false;
    int overflow = 0;
    *value = PyLong_AsLongAndOverflow(item, &overflow);
    return !overflow;
}

/**
 * The kind of a (type, size, digits) entry, or -1 for one the path does not reproduce.  The
 * driver ignores the size and digits of bit, bigint, double and date entries, and the digits of
 * an nvarchar entry, whose size must be what the plain path can bind (0 for max).  A timestamp
 * entry is taken only as datetime2(7), which leaves a datetime's microseconds intact.
 */
static int entry_kind(long type, long size, long digits)
{
    switch (type) {
    case SQL_WVARCHAR:
        return size >= 0 && size <= 4000 ? ENTRY_TEXT : -1;
    case SQL_BIT:
        return ENTRY_BIT;
    case SQL_BIGINT:
        return ENTRY_BIGINT;
    case SQL_DOUBLE:
        return ENTRY_DOUBLE;
    case SQL_NUMERIC:
    case SQL_DECIMAL:
        return size >= 1 && size <= 38 && digits >= 0 && digits <= size ? ENTRY_NUMERIC : -1;
    case SQL_TYPE_TIMESTAMP:
        return size == 27 && digits == 7 ? ENTRY_TIMESTAMP : -1;
    case SQL_TYPE_DATE:
        return ENTRY_DATE;
    }
    return -1;
}

/** The multi-byte code pages of SQL Server collations: UTF-8 and the four double-byte ones. */
static bool is_multibyte_code_page(int codePage)
{
    return codePage == 65001 || codePage == 932 || codePage == 936 || codePage == 949 || codePage == 950;
}

/**
 * Records the characters a single-byte code page holds exactly, those its 256 bytes decode to,
 * as a bitmap over the BMP for encodes_exactly to look up.  A multi-byte code page, or one
 * without a Python codec, gets none, and its values are encoded one by one instead.
 */
static void map_exact_chars(ColumnPlan& plan)
{
    const int codePage = plan.target->codePage;
    if (is_multibyte_code_page(codePage))
        return;
    char codec[16];
    snprintf(codec, sizeof(codec), "cp%d", codePage);
    char bytes[256];
    for (int b = 0; b < 256; ++b)
        bytes[b] = (char)b;
    Object chars(PyUnicode_Decode(bytes, 256, codec, "replace"));
    plan.exactChars = chars.IsValid() ? (uint32_t*)PyMem_Calloc(65536 / 32, sizeof(uint32_t)) : 0;
    if (!plan.exactChars) {
        PyErr_Clear();
        return;
    }
    for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(chars.Get()); ++i) {
        const Py_UCS4 ch = PyUnicode_READ_CHAR(chars.Get(), i);
        if (ch != 0xFFFD && ch <= 0xFFFF)
            plan.exactChars[ch / 32] |= 1u << (ch % 32);
    }
}

/**
 * Records each parameter's setinputsizes entry on its column.  An entry may be None, or a
 * 3-tuple of ints the path reproduces (see entry_kind); a parameter beyond the list has none.
 *
 * @return false to fall back, for any other entry or entries that are not a tuple or list.
 */
static bool read_entries(BcpLoad& load, PyObject* sizes)
{
    if (!sizes)
        return true;
    if (!PyTuple_Check(sizes) && !PyList_Check(sizes))
        return false;
    const Py_ssize_t nsizes = PySequence_Fast_GET_SIZE(sizes);
    for (int i = 0; i < load.ncols && i < nsizes; ++i) {
        PyObject* entry = PySequence_Fast_GET_ITEM(sizes, i);
        if (entry == Py_None)
            continue;
        long type, size, digits;
        if (!PyTuple_Check(entry) || PyTuple_GET_SIZE(entry) != 3 || !read_entry_field(entry, 0, &type) ||
            !read_entry_field(entry, 1, &size) || !read_entry_field(entry, 2, &digits))
            return false;
        ColumnPlan& plan = load.plans[i];
        plan.entry = entry_kind(type, size, digits);
        if (plan.entry < 0)
            return false;
        plan.entrySize = size;
        plan.entryDigits = digits;
        if (plan.entry == ENTRY_TEXT && is_code_page_text(*plan.target))
            map_exact_chars(plan);
    }
    return true;
}

static unsigned class_of(PyObject* cell)
{
    if (cell == Py_None)
        return VAL_NONE;
    if (PyBool_Check(cell))
        return VAL_BOOL;
    if (PyLong_Check(cell)) {
        int overflow = 0;
        PyLong_AsLongLongAndOverflow(cell, &overflow);
        return overflow ? 0 : VAL_INT;
    }
    if (PyFloat_Check(cell))
        return VAL_FLOAT;
    if (PyUnicode_Check(cell))
        return VAL_STR;
    if (PyBytes_Check(cell) || PyByteArray_Check(cell))
        return VAL_BYTES;
    if (PyDateTime_Check(cell))
        return VAL_DATETIME;
    if (PyDate_Check(cell))
        return VAL_DATE;
    if (PyTime_Check(cell))
        return VAL_TIME;
    PyObject* cls = 0;
    if (IsInstanceForThread(cell, "decimal", "Decimal", &cls) && cls) {
        Py_DECREF(cls);
        return VAL_DECIMAL;
    }
    PyErr_Clear();
    if (IsInstanceForThread(cell, "uuid", "UUID", &cls) && cls) {
        Py_DECREF(cls);
        return VAL_UUID;
    }
    PyErr_Clear();
    return 0;
}

/** Classifies a cell, reusing the column's last class when the type repeats (ints are range-checked each time). */
static unsigned classify(ColumnPlan& plan, PyObject* cell)
{
    if (Py_TYPE(cell) == plan.lastType && plan.lastClass != VAL_INT)
        return plan.lastClass;
    plan.lastType = Py_TYPE(cell);
    plan.lastClass = class_of(cell);
    return plan.lastClass;
}

static bool is_row(PyObject* row, int ncols)
{
    return (PyTuple_Check(row) || PyList_Check(row)) && PySequence_Fast_GET_SIZE(row) == ncols;
}

/** The length of a str in UTF-16 code units, as the plain path measures it. */
static Py_ssize_t utf16_length(PyObject* s)
{
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    if (PyUnicode_KIND(s) != PyUnicode_4BYTE_KIND)
        return n;
    const Py_UCS4* data = PyUnicode_4BYTE_DATA(s);
    Py_ssize_t units = n;
    for (Py_ssize_t i = 0; i < n; ++i)
        units += data[i] > 0xFFFF;
    return units;
}

/** The number of characters an int (already known to fit 64 bits) takes as decimal text. */
static long int_text_length(PyObject* cell)
{
    long long v = PyLong_AsLongLong(cell);
    long n = v < 0 ? 2 : 1;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    while (u >= 10) {
        u /= 10;
        ++n;
    }
    return n;
}

/**
 * The most bytes a str can take in a multi-byte code page: exactly its UTF-8 length for a UTF-8
 * collation, and for the double-byte ones two bytes for each UTF-16 unit outside ASCII.  -1 for
 * a single-byte code page, which takes at most a byte for each UTF-16 unit.
 */
static Py_ssize_t max_encoded_length(PyObject* s, int codePage)
{
    if (!is_multibyte_code_page(codePage))
        return -1;
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    Py_ssize_t bytes = 0;
    for (Py_ssize_t i = 0; i < n; ++i) {
        const Py_UCS4 ch = PyUnicode_READ_CHAR(s, i);
        if (ch < 0x80)
            bytes += 1;
        else if (codePage != 65001)
            bytes += ch > 0xFFFF ? 4 : 2;
        else
            bytes += ch < 0x800 ? 2 : (ch > 0xFFFF ? 4 : 3);
    }
    return bytes;
}

/**
 * Whether a str fits a char or varchar column in bytes.  In a multi-byte code page a value can
 * have no more characters than the column yet more bytes, which the server rejects but the
 * driver quietly cuts short.  ASCII takes a byte a character in every code page.
 */
static bool fits_column_bytes(const ColumnPlan& plan, PyObject* s)
{
    if (!is_code_page_text(*plan.target) || PyUnicode_IS_ASCII(s))
        return true;
    return max_encoded_length(s, plan.target->codePage) <= plan.capacity;
}

/**
 * Returns false where a value's length means the plain path would not store what BCP would:
 * text or bytes longer than the column, which the plain path rejects but the driver quietly
 * trims of trailing spaces or zero bytes (or, in a multi-byte code page, cuts short, see
 * fits_column_bytes), and a number wider as text than its text column, which the plain path
 * truncates but BCP rejects.  A double can take 24 characters as text.
 */
static bool fits_capacity(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    switch (cls) {
    case VAL_STR:
        return plan.destination != DEST_CHAR ||
               (utf16_length(cell) <= plan.capacity && fits_column_bytes(plan, cell));
    case VAL_BYTES:
        return plan.destination != DEST_BINARY ||
               (PyBytes_Check(cell) ? PyBytes_GET_SIZE(cell) : PyByteArray_GET_SIZE(cell)) <= plan.capacity;
    case VAL_INT:
        return plan.destination != DEST_CHAR || int_text_length(cell) <= plan.capacity;
    case VAL_FLOAT:
        return plan.destination != DEST_CHAR || plan.capacity >= 24;
    }
    return true;
}

static int hex_digit(Py_UCS1 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * Parses a str in the one form the plain path stores in a uniqueidentifier column,
 * xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx in either case, into the column's byte order (that of
 * uuid.UUID.bytes_le).  The plain path rejects every other form (braces, no hyphens, padding),
 * so those are left to it.  `guid` may be null to only check the form.
 */
static bool parse_guid_text(PyObject* s, unsigned char* guid)
{
    static const int byteOrder[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    if (PyUnicode_KIND(s) != PyUnicode_1BYTE_KIND || PyUnicode_GET_LENGTH(s) != 36)
        return false;
    const Py_UCS1* text = PyUnicode_1BYTE_DATA(s);
    unsigned char bytes[16];
    int n = 0;
    for (int i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i++] != '-')
                return false;
            continue;
        }
        const int high = hex_digit(text[i]), low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[n++] = (unsigned char)(high << 4 | low);
        i += 2;
    }
    if (guid)
        for (int k = 0; k < 16; ++k)
            guid[k] = bytes[byteOrder[k]];
    return true;
}

static bool is_two_digits_below(const Py_UCS1* text, int limit)
{
    return text[0] >= '0' && text[0] <= '9' && text[1] >= '0' && text[1] <= '9' &&
           (text[0] - '0') * 10 + (text[1] - '0') < limit;
}

/**
 * Whether a str is time text the plain path stores exactly in a time column of the given scale
 * and the driver's BCP conversion accepts as is: HH:MM:SS with an optional fraction of no more
 * digits than the scale, the form str(datetime.time) takes and SQLAlchemy binds.  The plain path
 * rejects a longer fraction, which does not fit the column's size, and out-of-range fields.
 */
static bool is_time_text(PyObject* s, int scale)
{
    if (PyUnicode_KIND(s) != PyUnicode_1BYTE_KIND)
        return false;
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
    const Py_UCS1* text = PyUnicode_1BYTE_DATA(s);
    if (n < 8 || text[2] != ':' || text[5] != ':' || !is_two_digits_below(text, 24) ||
        !is_two_digits_below(text + 3, 60) || !is_two_digits_below(text + 6, 60))
        return false;
    if (n == 8)
        return true;
    if (text[8] != '.' || n == 9 || n - 9 > scale)
        return false;
    for (Py_ssize_t i = 9; i < n; ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

/**
 * Whether the plain path stores a datetime in a date column: when its hours, minutes and seconds
 * are zero, ignoring the microseconds and tzinfo.  It rejects any other time of day, and SQLAlchemy
 * binds a date for such a column as a datetime at midnight.
 */
static bool is_storable_as_date(PyObject* cell)
{
    return PyDateTime_DATE_GET_HOUR(cell) == 0 && PyDateTime_DATE_GET_MINUTE(cell) == 0 &&
           PyDateTime_DATE_GET_SECOND(cell) == 0;
}

/**
 * Returns false for a value the plain path rejects in its column, or would store differently,
 * because of its form rather than its type: a str in a uniqueidentifier column that is not a
 * canonical GUID, a str in a time column that is not time text (see is_time_text), or a datetime
 * with a time of day in a date column, which the server truncates to its date only when it
 * arrives as a timestamp entry.
 */
static bool has_plain_path_form(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    if (plan.destination == DEST_GUID && cls == VAL_STR)
        return parse_guid_text(cell, 0);
    if (plan.destination == DEST_TIME && cls == VAL_STR)
        return is_time_text(cell, plan.target->scale);
    if (plan.destination == DEST_DATE && cls == VAL_DATETIME)
        return plan.entry == ENTRY_TIMESTAMP || is_storable_as_date(cell);
    return true;
}

/**
 * Whether the code page of a char, varchar or text column holds every character of a str
 * exactly.  Under an nvarchar entry the server converts text to that code page itself, and its
 * substitutes for characters the code page lacks differ from the driver's best-fit mapping
 * (U+2009 becomes '?' rather than a space in code page 1252).  ASCII is held exactly by every
 * code page; other characters are looked up in the column's exactChars, or for a multi-byte
 * code page found by encoding the value.
 */
static bool encodes_exactly(const ColumnPlan& plan, PyObject* s)
{
    if (!is_code_page_text(*plan.target) || PyUnicode_IS_ASCII(s))
        return true;
    if (plan.exactChars) {
        const unsigned kind = PyUnicode_KIND(s);
        const void* data = PyUnicode_DATA(s);
        const Py_ssize_t n = PyUnicode_GET_LENGTH(s);
        for (Py_ssize_t i = 0; i < n; ++i) {
            const Py_UCS4 ch = PyUnicode_READ(kind, data, i);
            if (ch > 0xFFFF || !((plan.exactChars[ch / 32] >> (ch % 32)) & 1))
                return false;
        }
        return true;
    }
    char codec[16];
    if (plan.target->codePage == 65001)
        snprintf(codec, sizeof(codec), "utf-8");
    else
        snprintf(codec, sizeof(codec), "cp%d", plan.target->codePage);
    Object encoded(PyUnicode_AsEncodedString(s, codec, "strict"));
    if (!encoded.IsValid())
        PyErr_Clear();
    return encoded.IsValid();
}

/**
 * Whether a str fits its nvarchar entry, which the plain path binds with room for only that
 * many UTF-16 units and rejects anything longer (an entry of 0 is nvarchar(max)), and is text
 * the server and the driver both convert exactly (see encodes_exactly).
 */
static bool fits_entry(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    if (plan.entry != ENTRY_TEXT || cls != VAL_STR)
        return true;
    return (plan.entrySize == 0 || utf16_length(cell) <= plan.entrySize) && encodes_exactly(plan, cell);
}

/**
 * Whether a value fits its column and its entry (see fits_capacity and fits_entry), and is
 * ASCII if it is text for a column that takes only that (see takes_only_ascii).
 */
static bool fits_column(const ColumnPlan& plan, PyObject* cell, unsigned cls)
{
    return (plan.capacity < 0 || fits_capacity(plan, cell, cls)) && fits_entry(plan, cell, cls) &&
           (!plan.asciiText || cls != VAL_STR || PyUnicode_IS_ASCII(cell));
}

/**
 * Scans every row once before anything is sent, recording the classes of value in each column.
 * Each value is read afresh and held while it is checked, as the checks can run Python code (a
 * first import of decimal, say) that lets another thread change the row.
 *
 * @return false if a row is not a tuple or list of the right length, or holds a value BCP
 *         cannot send as the plain path would (an int beyond 64 bits, an unsupported type, a
 *         value that does not fit its column or its entry, see fits_column, or one whose form
 *         the plain path rejects, see has_plain_path_form).  Filling a row checks its values
 *         again, as Python code run meanwhile can change them.
 */
static bool scan_rows(BcpLoad& load)
{
    const Py_ssize_t nrows = PyTuple_GET_SIZE(load.rows.Get());
    for (Py_ssize_t r = 0; r < nrows; ++r) {
        PyObject* row = PyTuple_GET_ITEM(load.rows.Get(), r);
        for (int i = 0; i < load.ncols; ++i) {
            if (!is_row(row, load.ncols))
                return false;
            Object cell(Py_NewRef(PySequence_Fast_GET_ITEM(row, i)));
            ColumnPlan& plan = load.plans[i];
            unsigned cls = classify(plan, cell);
            if (!cls || !fits_column(plan, cell, cls) || !has_plain_path_form(plan, cell, cls))
                return false;
            plan.classes |= cls;
        }
    }
    return true;
}

/**
 * Picks the host type for a column without a setinputsizes entry from its destination and the
 * classes of value it holds.
 *
 * Only pairs whose driver conversion stores what the plain fast path stores are allowed (see
 * test_bcp_matches_plain_path); text goes as UTF-16 (SQLNCHAR) because the driver reads
 * SQLCHARACTER data in the client's ANSI code page.  SQLCHARACTER is used only for ASCII text
 * the path formats itself, Decimals and dates and times shaped to the destination's precision,
 * and for time text it has checked (see is_time_text).
 *
 * @return The host type, or 0 to fall back.
 */
static int choose_untyped_host_type(int destination, unsigned classes)
{
    const unsigned v = classes & ~VAL_NONE;
    if (!v)
        return SQLCHARACTER;
    const bool onlyIntegers = !(v & ~(VAL_INT | VAL_BOOL));

    switch (destination) {
    case DEST_CHAR:
        if (v == VAL_STR || v == (VAL_STR | VAL_INT))
            return SQLNCHAR;
        if (v == VAL_INT)
            return SQLINT8;
        if (v == VAL_BOOL)
            return SQLBIT;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        return 0;
    case DEST_XML:
        return v == VAL_STR ? SQLNCHAR : 0;
    case DEST_BINARY:
        return v == VAL_BYTES ? SQLBINARY : 0;
    case DEST_INTEGER:
        if (onlyIntegers)
            return SQLINT8;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        if (!(v & ~(VAL_DECIMAL | VAL_INT)))
            return SQLCHARACTER;
        return 0;
    case DEST_BIT:
        if (onlyIntegers)
            return SQLINT8;
        return v == VAL_FLOAT ? SQLFLT8 : 0;
    case DEST_FLOAT:
        if (onlyIntegers)
            return SQLINT8;
        return !(v & ~(VAL_FLOAT | VAL_INT | VAL_BOOL)) ? SQLFLT8 : 0;
    case DEST_DECIMAL:
        if (v == VAL_INT)
            return SQLINT8;
        if (v == VAL_FLOAT)
            return SQLFLT8;
        return !(v & ~(VAL_DECIMAL | VAL_INT)) ? SQLCHARACTER : 0;
    case DEST_MONEY:
        if (v == VAL_INT)
            return SQLINT8;
        return !(v & ~(VAL_DECIMAL | VAL_INT)) ? SQLCHARACTER : 0;
    case DEST_TIME:
        return !(v & ~(VAL_TIME | VAL_DATETIME | VAL_STR)) ? SQLCHARACTER : 0;
    case DEST_DATE:
    case DEST_DATETIME2:
    case DEST_DATETIMEOFFSET:
    case DEST_DATETIME:
    case DEST_SMALLDATETIME:
        return !(v & ~(VAL_DATE | VAL_DATETIME)) ? SQLCHARACTER : 0;
    case DEST_GUID:
        return !(v & ~(VAL_UUID | VAL_STR)) ? SQLUNIQUEID : 0;
    }
    return 0;
}

static bool is_temporal(int destination)
{
    switch (destination) {
    case DEST_DATE: case DEST_TIME: case DEST_DATETIME2: case DEST_DATETIMEOFFSET:
    case DEST_DATETIME: case DEST_SMALLDATETIME:
        return true;
    }
    return false;
}

/**
 * Picks the host type for a column whose parameter has a setinputsizes entry, from the classes
 * of value other than None it holds.  The plain path sends each value as the entry's type and
 * the server converts it to the column's, so only values of the entry's own kind are allowed,
 * and only into columns whose server conversion the path reproduces (see
 * test_bcp_matches_plain_path_with_input_sizes):
 *
 *   - text as UTF-16 into char, varchar, nchar and nvarchar, as without an entry, once the scan
 *     has checked the code page of a char or varchar column holds it exactly (see
 *     encodes_exactly);
 *   - bool into bit, and bigint into the integer types, decimal, money, float and real;
 *   - double into float and real, the integer types (truncated toward zero), decimal (rounded
 *     from its exact binary value) and bit;
 *   - numeric into decimal and money (rounded half away from zero), the integer types
 *     (truncated toward zero) and float;
 *   - timestamp into the date and time types, rounded from all seven digits;
 *   - date into the date and time types other than time.
 *
 * A number into bit stores 1 when it is not zero.
 *
 * @return The host type, or 0 to fall back.
 */
static int entry_host_type(const ColumnPlan& plan, unsigned v)
{
    const int dest = plan.destination;
    switch (plan.entry) {
    case ENTRY_TEXT:
        return v == VAL_STR && dest == DEST_CHAR ? SQLNCHAR : 0;
    case ENTRY_BIT:
        return v == VAL_BOOL && dest == DEST_BIT ? SQLBIT : 0;
    case ENTRY_BIGINT:
        if (v != VAL_INT)
            return 0;
        if (dest == DEST_BIT)
            return SQLBIT;
        return dest == DEST_INTEGER || dest == DEST_DECIMAL || dest == DEST_MONEY || dest == DEST_FLOAT ? SQLINT8 : 0;
    case ENTRY_DOUBLE:
        if (v != VAL_FLOAT)
            return 0;
        switch (dest) {
        case DEST_BIT:
            return SQLBIT;
        case DEST_INTEGER:
            return SQLINT8;
        case DEST_DECIMAL:
            return SQLCHARACTER;
        case DEST_FLOAT:
            return plan.target->systemType == 59 ? SQLFLT4 : SQLFLT8;
        }
        return 0;
    case ENTRY_NUMERIC:
        if (v != VAL_DECIMAL)
            return 0;
        switch (dest) {
        case DEST_BIT:
            return SQLBIT;
        case DEST_INTEGER:
        case DEST_DECIMAL:
        case DEST_MONEY:
            return SQLCHARACTER;
        case DEST_FLOAT:
            return plan.target->systemType == 62 ? SQLFLT8 : 0;
        }
        return 0;
    case ENTRY_TIMESTAMP:
        return !(v & ~(VAL_DATETIME | VAL_DATE)) && is_temporal(dest) ? SQLCHARACTER : 0;
    case ENTRY_DATE:
        return v == VAL_DATE && is_temporal(dest) && dest != DEST_TIME ? SQLCHARACTER : 0;
    }
    return 0;
}

/** The class of value an entry's own type is sent for. */
static unsigned entry_class(int entry)
{
    switch (entry) {
    case ENTRY_TEXT:
        return VAL_STR;
    case ENTRY_BIT:
        return VAL_BOOL;
    case ENTRY_BIGINT:
        return VAL_INT;
    case ENTRY_DOUBLE:
        return VAL_FLOAT;
    case ENTRY_NUMERIC:
        return VAL_DECIMAL;
    case ENTRY_TIMESTAMP:
        return VAL_DATETIME;
    case ENTRY_DATE:
        return VAL_DATE;
    }
    return 0;
}

/**
 * The host type for a column with an entry (see entry_host_type).  The server converts even a
 * NULL from its entry's type, and rejects one it has no implicit conversion for (nvarchar into
 * binary, or date into int), so a column of NULLs takes an nvarchar entry into anything but a
 * binary column, and any other entry only where a value of the entry's own kind may go.
 */
static int choose_entry_host_type(const ColumnPlan& plan, unsigned classes)
{
    const unsigned v = classes & ~VAL_NONE;
    if (v)
        return entry_host_type(plan, v);
    if (plan.entry == ENTRY_TEXT)
        return plan.destination != DEST_BINARY ? SQLCHARACTER : 0;
    return entry_host_type(plan, entry_class(plan.entry)) ? SQLCHARACTER : 0;
}

/** The host type for a column, or 0 to fall back. */
static int choose_host_type(const ColumnPlan& plan, unsigned classes)
{
    return plan.entry == ENTRY_NONE ? choose_untyped_host_type(plan.destination, classes)
                                    : choose_entry_host_type(plan, classes);
}

/**
 * Sets up a column per plan, then the NULL filler columns.  Variable-length columns take a
 * 4-byte length prefix, except (max) columns, which need an 8-byte prefix and, because the
 * driver reads a zero-length 8-byte prefix as 64KB, every length passed to bcp_collen as well.
 *
 * @return 1 if every column has a host type, 0 to fall back, -1 on allocation failure.
 */
static int plan_columns(BcpLoad& load)
{
    load.cols = (BcpCol*)PyMem_Calloc((size_t)(load.ncols + load.nfill), sizeof(BcpCol));
    if (!load.cols) {
        PyErr_NoMemory();
        return -1;
    }
    for (int f = 0; f < load.nfill; ++f) {
        BcpCol* c = &load.cols[load.ncols + f];
        c->ordinal = load.target.ncols - load.nfill + f + 1;
        c->hostType = SQLCHARACTER;
        c->prefix = load.target.cols[c->ordinal - 1].maxLength == -1 ? 8 : 4;
        if (!BcpAllocColumn(c))
            return -1;
        write_le(c->scratch, ~0ULL, c->prefix);
    }
    for (int i = 0; i < load.ncols; ++i) {
        const ColumnPlan& plan = load.plans[i];
        BcpCol* c = &load.cols[i];
        c->ordinal = plan.ordinal;
        c->hostType = choose_host_type(plan, plan.classes);
        if (!c->hostType)
            return 0;
        const bool fixed = c->hostType == SQLBIT || c->hostType == SQLINT8 || c->hostType == SQLFLT8 ||
                           c->hostType == SQLFLT4 || c->hostType == SQLUNIQUEID;
        const bool unlimited = plan.target->maxLength == -1;
        c->prefix = fixed ? 0 : (unlimited ? 8 : 4);
        c->setsCollen = !fixed && unlimited;
        if (!BcpAllocColumn(c))
            return -1;
    }
    return 1;
}

/*=======================================================================================*/
// Filling columns
/*=======================================================================================*/

static bool unexpected_value(PyObject* cell)
{
    PyErr_Format(PyExc_TypeError, "unexpected %.100s value in a BCP column; the row changed while it was being sent",
                 Py_TYPE(cell)->tp_name);
    return false;
}

/** ASCII text the path formats itself, for a SQLCHARACTER or SQLNCHAR column. */
static bool put_ascii(Connection* cnxn, BcpCol* c, const char* text, Py_ssize_t n)
{
    if (c->hostType == SQLNCHAR)
        return BcpPutWidened(cnxn, c, (const unsigned char*)text, n);
    return BcpPutBytes(cnxn, c, text, n);
}

static bool put_int_text(Connection* cnxn, BcpCol* c, PyObject* cell)
{
    long long v = PyLong_AsLongLong(cell);
    if (v == -1 && PyErr_Occurred())
        return false;
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    return put_ascii(cnxn, c, buf, n);
}

/**
 * Python code run while filling a row (a Decimal's normalize, a tzinfo's utcoffset) can close
 * the cursor or the connection, either of which ends the load: the plain path cannot carry on
 * either, and nothing may touch a closed connection's handle or BCP state.
 */
static bool load_still_open(const BcpLoad& load)
{
    if (!load.cursor->cnxn) {
        PyErr_SetString(ProgrammingError, "The cursor was closed while BCP was sending rows.");
        return false;
    }
    if (load.cnxn->hdbc && load.cnxn->bcp)
        return true;
    PyErr_SetString(ProgrammingError, "The connection was closed while BCP was sending rows.");
    return false;
}

static bool is_decimal(BcpLoad& load, PyObject* value)
{
    if (Py_TYPE(value) == (PyTypeObject*)load.decimalType.Get())
        return true;
    const int r = PyObject_IsInstance(value, load.decimalType);
    if (r < 0)
        PyErr_Clear();
    return r > 0;
}

/**
 * A Decimal as exact text, normalized first in the thread's decimal context as the plain path
 * normalizes it: that rounds a value with more significant digits than the context's precision
 * (28 by default) before the column's scale applies.  A normalize() that returns anything but a
 * Decimal raises, as the plain path fails to read the digits of what it returned.  `text` owns
 * the characters.
 */
static bool get_decimal_text(BcpLoad& load, PyObject* cell, Object& text, const char** chars, Py_ssize_t* n)
{
    if (!is_decimal(load, cell))
        return unexpected_value(cell);
    Object normalized(PyObject_CallMethodNoArgs(cell, load.normalizeName));
    if (!normalized.IsValid() || !load_still_open(load))
        return false;
    if (!is_decimal(load, normalized)) {
        PyErr_Format(PyExc_TypeError, "Decimal.normalize() returned %.100s, not a Decimal", Py_TYPE(normalized.Get())->tp_name);
        return false;
    }
    text = PyObject_Format(normalized, load.decimalFormat);
    if (!text.IsValid() || !load_still_open(load))
        return false;
    *chars = PyUnicode_AsUTF8AndSize(text, n);
    return *chars != 0;
}

static bool put_decimal_text(BcpLoad& load, BcpCol* c, PyObject* cell)
{
    Object text;
    const char* chars = 0;
    Py_ssize_t n = 0;
    return get_decimal_text(load, cell, text, &chars, &n) && put_ascii(load.cnxn, c, chars, n);
}

/**
 * A Decimal under a numeric entry, after the plain path's checks against that entry (see
 * BcpFitsNumericEntry), converted as the server converts it: for a decimal or money column
 * rounded half away from zero to the column's scale (4 for money), for an integer column
 * truncated toward zero, for a float column the correctly rounded double, and for a bit column 1
 * unless it is zero.  NaN and the infinities raise.
 */
static bool put_entry_decimal(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    Object text;
    const char* chars = 0;
    Py_ssize_t n = 0;
    BcpDecimalParts d;
    if (!get_decimal_text(load, cell, text, &chars, &n) || !BcpSplitDecimalText(chars, n, d) ||
        !BcpFitsNumericEntry(d, plan.entrySize, plan.entryDigits))
        return false;
    if (c->hostType == SQLBIT) {
        const unsigned char b = d.nwhole > 0 || d.nfraction > 0;
        return BcpPutFixed(load.cnxn, c, &b);
    }
    if (c->hostType == SQLFLT8) {
        double value = PyOS_string_to_double(chars, 0, 0);
        if (value == -1.0 && PyErr_Occurred())
            return false;
        if (value == 0)
            value = 0;
        return BcpPutFixed(load.cnxn, c, &value);
    }
    const int scale = plan.destination == DEST_DECIMAL ? plan.target->scale : (plan.destination == DEST_MONEY ? 4 : 0);
    char buf[96];
    return put_ascii(load.cnxn, c, buf, BcpShapeNumericText(d, scale, plan.destination != DEST_INTEGER, buf));
}

/**
 * A value for a SQLBIT column: a bool, or under a bigint, double or numeric entry a number of
 * that kind, which the server stores as 1 unless it is zero.  A non-finite double raises, as the
 * server rejects it.
 */
static bool put_bit(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    unsigned char b;
    switch (plan.entry) {
    case ENTRY_BIGINT: {
        if (!PyLong_Check(cell) || PyBool_Check(cell))
            return unexpected_value(cell);
        const long long v = PyLong_AsLongLong(cell);
        if (v == -1 && PyErr_Occurred())
            return false;
        b = v != 0;
        break;
    }
    case ENTRY_DOUBLE: {
        if (!PyFloat_Check(cell))
            return unexpected_value(cell);
        const double d = PyFloat_AS_DOUBLE(cell);
        if (!std::isfinite(d))
            return BcpRaiseOutOfRange();
        b = d != 0;
        break;
    }
    case ENTRY_NUMERIC:
        return put_entry_decimal(load, plan, c, cell);
    default:
        if (!PyBool_Check(cell))
            return unexpected_value(cell);
        b = cell == Py_True;
    }
    return BcpPutFixed(load.cnxn, c, &b);
}

/**
 * A uuid.UUID or canonical GUID str for a SQLUNIQUEID column, as the 16 bytes the plain path
 * sends: a UUID's bytes_le, which can run Python code in a subclass.  A bytes_le that is not 16
 * bytes raises, where the plain path would copy 16 bytes from whatever it was given.
 */
static bool put_guid(BcpLoad& load, BcpCol* c, PyObject* cell)
{
    unsigned char guid[16];
    if (PyUnicode_Check(cell))
        return parse_guid_text(cell, guid) ? BcpPutFixed(load.cnxn, c, guid) : unexpected_value(cell);
    Object bytesLe(PyObject_GetAttrString(cell, "bytes_le"));
    if (!bytesLe.IsValid() && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return unexpected_value(cell);
    }
    if (!bytesLe.IsValid() || !load_still_open(load))
        return false;
    if (!PyBytes_Check(bytesLe.Get()) || PyBytes_GET_SIZE(bytesLe.Get()) != 16) {
        PyErr_SetString(PyExc_TypeError, "UUID.bytes_le must be 16 bytes");
        return false;
    }
    return BcpPutFixed(load.cnxn, c, PyBytes_AS_STRING(bytesLe.Get()));
}

/** Whether a value of class `cls` would still have given the column its host type. */
static bool suits_host_type(const ColumnPlan& plan, const BcpCol* c, unsigned cls)
{
    return choose_host_type(plan, plan.classes | cls) == c->hostType;
}

/**
 * A value for a SQLCHARACTER column, as text the path formats or has checked.  A row can change
 * while it is being sent, so each value must still suit the column's host type and have a form
 * the plain path stores, or the call raises.
 */
static bool fill_text(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    if (PyBool_Check(cell))
        return unexpected_value(cell);
    if (PyLong_Check(cell))
        return suits_host_type(plan, c, VAL_INT) ? put_int_text(load.cnxn, c, cell) : unexpected_value(cell);
    if (PyFloat_Check(cell)) {
        if (!suits_host_type(plan, c, VAL_FLOAT))
            return unexpected_value(cell);
        char buf[96];
        const int n = BcpFormatDoubleAsDecimal(PyFloat_AS_DOUBLE(cell), plan.target->scale, buf);
        return n >= 0 && put_ascii(load.cnxn, c, buf, n);
    }
    if (PyUnicode_Check(cell)) {
        if (plan.destination != DEST_TIME || !suits_host_type(plan, c, VAL_STR) || !is_time_text(cell, plan.target->scale))
            return unexpected_value(cell);
        return put_ascii(load.cnxn, c, (const char*)PyUnicode_1BYTE_DATA(cell), PyUnicode_GET_LENGTH(cell));
    }
    if (PyDateTime_Check(cell) || PyDate_Check(cell) || PyTime_Check(cell)) {
        const unsigned cls = PyDateTime_Check(cell) ? VAL_DATETIME : (PyDate_Check(cell) ? VAL_DATE : VAL_TIME);
        if (!suits_host_type(plan, c, cls) || !has_plain_path_form(plan, cell, cls))
            return unexpected_value(cell);
        char buf[48];
        const int n = BcpFormatTemporal(buf, sizeof(buf), cell, cls, plan.destination, plan.target->scale,
                                        plan.entry == ENTRY_TIMESTAMP);
        return n >= 0 && load_still_open(load) && put_ascii(load.cnxn, c, buf, n);
    }
    if (!suits_host_type(plan, c, VAL_DECIMAL))
        return unexpected_value(cell);
    if (plan.entry == ENTRY_NUMERIC)
        return put_entry_decimal(load, plan, c, cell);
    return put_decimal_text(load, c, cell);
}

/**
 * A str for a SQLNCHAR column.  For a zero-length value the driver stores the previous row's
 * value in a char(n) column, which pads with spaces anyway, so an empty string goes there as one
 * space, which stores the same.
 */
static bool put_str(Connection* cnxn, const ColumnPlan& plan, BcpCol* c, PyObject* s)
{
    if (PyUnicode_GET_LENGTH(s) == 0 && plan.target->systemType == 175)
        return BcpPutWidened(cnxn, c, (const unsigned char*)" ", 1);
    return BcpPutWideStr(cnxn, c, s);
}

static bool fill_cell(BcpLoad& load, const ColumnPlan& plan, BcpCol* c, PyObject* cell)
{
    Connection* cnxn = load.cnxn;
    if (cell == Py_None)
        return BcpPutNull(cnxn, c);

    switch (c->hostType) {
    case SQLBIT:
        return put_bit(load, plan, c, cell);
    case SQLINT8: {
        long long v;
        if (plan.entry == ENTRY_DOUBLE) {
            if (!PyFloat_Check(cell))
                return unexpected_value(cell);
            if (!BcpTruncateDouble(PyFloat_AS_DOUBLE(cell), &v))
                return false;
        }
        else if (PyBool_Check(cell) && suits_host_type(plan, c, VAL_BOOL))
            v = cell == Py_True;
        else if (PyLong_Check(cell) && !PyBool_Check(cell) && suits_host_type(plan, c, VAL_INT) &&
                 fits_column(plan, cell, VAL_INT)) {
            v = PyLong_AsLongLong(cell);
            if (v == -1 && PyErr_Occurred())
                return false;
        }
        else
            return unexpected_value(cell);
        return BcpPutFixed(cnxn, c, &v);
    }
    case SQLFLT8: {
        if (plan.entry == ENTRY_NUMERIC)
            return put_entry_decimal(load, plan, c, cell);
        double d;
        if (PyFloat_Check(cell))
            d = PyFloat_AS_DOUBLE(cell);
        else if (PyBool_Check(cell) && suits_host_type(plan, c, VAL_BOOL))
            d = cell == Py_True;
        else if (PyLong_Check(cell) && !PyBool_Check(cell) && suits_host_type(plan, c, VAL_INT)) {
            d = PyLong_AsDouble(cell);
            if (d == -1.0 && PyErr_Occurred())
                return false;
        }
        else
            return unexpected_value(cell);
        return BcpPutFixed(cnxn, c, &d);
    }
    case SQLFLT4: {
        float f;
        if (!PyFloat_Check(cell))
            return unexpected_value(cell);
        return BcpNarrowDouble(PyFloat_AS_DOUBLE(cell), &f) && BcpPutFixed(cnxn, c, &f);
    }
    case SQLBINARY:
        if (!(PyBytes_Check(cell) || PyByteArray_Check(cell)) || !fits_column(plan, cell, VAL_BYTES))
            return unexpected_value(cell);
        if (PyBytes_Check(cell))
            return BcpPutBytes(cnxn, c, PyBytes_AS_STRING(cell), PyBytes_GET_SIZE(cell));
        return BcpPutBytes(cnxn, c, PyByteArray_AS_STRING(cell), PyByteArray_GET_SIZE(cell));
    case SQLNCHAR:
        if (PyUnicode_Check(cell) && fits_column(plan, cell, VAL_STR))
            return put_str(cnxn, plan, c, cell);
        if (PyLong_Check(cell) && !PyBool_Check(cell) && plan.entry == ENTRY_NONE && fits_column(plan, cell, VAL_INT))
            return put_int_text(cnxn, c, cell);
        return unexpected_value(cell);
    case SQLUNIQUEID:
        return put_guid(load, c, cell);
    }
    return fill_text(load, plan, c, cell);
}

/*=======================================================================================*/
// The transaction around a load
/*=======================================================================================*/

static bool execute_direct(HDBC hdbc, const char16_t* sql)
{
    HSTMT hstmt = SQL_NULL_HANDLE;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt)))
        return false;
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = SQLExecDirectW(hstmt, (SQLWCHAR*)sql, SQL_NTS);
    Py_END_ALLOW_THREADS
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return SQL_SUCCEEDED(rc);
}

/**
 * Starts the load's transaction.  In manual-commit mode the savepoint was already set by the
 * catalog query in prepare_load, saving a round trip.
 */
static bool begin_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_ON) {
        txn.autocommitSuspended = BcpSetAutocommit(cnxn->hdbc, false);
        return txn.autocommitSuspended;
    }
    txn.savepoint = true;
    return true;
}

/** Commits a completed batch in autocommit mode; in manual-commit mode it stays in the caller's transaction. */
static bool commit_load_batch(Connection* cnxn, const LoadTransaction& txn)
{
    return !txn.autocommitSuspended || BcpEndTransaction(cnxn->hdbc, true);
}

static bool finish_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (!txn.autocommitSuspended)
        return true;
    if (!BcpEndTransaction(cnxn->hdbc, true))
        return false;
    txn.autocommitSuspended = false;
    BcpSetAutocommit(cnxn->hdbc, true);
    return true;
}

/**
 * Undoes what the load sent and restores autocommit.  A savepoint that no longer exists (the
 * error aborted the whole transaction, as with XACT_ABORT ON) leaves nothing more to undo.
 */
static void rollback_load_transaction(Connection* cnxn, LoadTransaction& txn)
{
    if (txn.autocommitSuspended) {
        BcpEndTransaction(cnxn->hdbc, false);
        BcpSetAutocommit(cnxn->hdbc, true);
        txn.autocommitSuspended = false;
    }
    if (txn.savepoint) {
        execute_direct(cnxn->hdbc, u"IF XACT_STATE() = 1 ROLLBACK TRANSACTION pyodbc_bcp");
        txn.savepoint = false;
    }
}

/*=======================================================================================*/
// The load
/*=======================================================================================*/

/**
 * Reports a failure after rows may have been sent: raises (from the driver diagnostics unless a
 * Python exception is already set), closes the bulk copy and rolls back everything it sent.
 * The diagnostics are read before bcp_done, which clears them.
 */
static bool abandon_load(BcpLoad& load, LoadTransaction& txn, const char* function)
{
    Connection* cnxn = load.cnxn;
    if (!PyErr_Occurred())
        RaiseErrorFromHandle(cnxn, function, cnxn->hdbc, SQL_NULL_HANDLE);
    if (!cnxn->hdbc || !cnxn->bcp)
        return true;
    Py_BEGIN_ALLOW_THREADS
    cnxn->bcp->procs.bcp_done(cnxn->hdbc);
    Py_END_ALLOW_THREADS
    rollback_load_transaction(cnxn, txn);
    return true;
}

/**
 * Starts the bulk copy and binds the columns.  Nothing has been sent yet, so on failure the
 * session is closed, the transaction undone and the call falls back to the plain path.
 */
static bool start_bulk_copy(BcpLoad& load, Cursor* cur, LoadTransaction& txn)
{
    Connection* cnxn = load.cnxn;
    const BcpProcs procs = cnxn->bcp->procs;
    SQLWCHAR* name = BcpQuotedTableName(load.insert);
    if (!name)
        return false;
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = procs.bcp_initW(cnxn->hdbc, name, nullptr, nullptr, DB_IN);
    Py_END_ALLOW_THREADS
    PyMem_Free(name);
    if (rc != SUCCEED)
        return false;

    bool ok = (!load.keepNulls || procs.bcp_control(cnxn->hdbc, BCPKEEPNULLS, (void*)(uintptr_t)1) == SUCCEED)
           && procs.bcp_control(cnxn->hdbc, BCPHINTS,
                                (void*)(cur->bcp_tablock ? "CHECK_CONSTRAINTS,FIRE_TRIGGERS,TABLOCK"
                                                         : "CHECK_CONSTRAINTS,FIRE_TRIGGERS")) == SUCCEED;
    for (int i = 0; ok && i < load.ncols + load.nfill; ++i)
        ok = BcpBindColumn(cnxn, &load.cols[i]);
    if (!ok) {
        Py_BEGIN_ALLOW_THREADS
        procs.bcp_done(cnxn->hdbc);
        Py_END_ALLOW_THREADS
    }
    return ok;
}

/**
 * Sends every row, batching every cur->bcp_batch_rows rows, and finishes the bulk copy.
 *
 * @return true on success with *copied set; false with an error raised and the load undone.
 */
static bool send_rows(BcpLoad& load, Cursor* cur, LoadTransaction& txn, long long* copied)
{
    Connection* cnxn = load.cnxn;
    const BcpProcs procs = cnxn->bcp->procs;
    const Py_ssize_t nrows = PyTuple_GET_SIZE(load.rows.Get());
    const long batchRows = cur->bcp_batch_rows;
    long unbatched = 0;
    SQLRETURN rc;
    DBINT count;

    *copied = 0;
    for (Py_ssize_t r = 0; r < nrows; ++r) {
        PyObject* row = PyTuple_GET_ITEM(load.rows.Get(), r);
        for (int i = 0; i < load.ncols; ++i) {
            if (!is_row(row, load.ncols)) {
                PyErr_SetString(PyExc_TypeError, "a row changed length while it was being sent by BCP");
                return !abandon_load(load, txn, "bcp_sendrow");
            }
            Object cell(PySequence_Fast_GET_ITEM(row, i));
            Py_INCREF(cell.Get());
            if (!fill_cell(load, load.plans[i], &load.cols[i], cell))
                return !abandon_load(load, txn, "bcp_collen");
        }
        Py_BEGIN_ALLOW_THREADS
        rc = procs.bcp_sendrow(cnxn->hdbc);
        Py_END_ALLOW_THREADS
        if (rc != SUCCEED)
            return !abandon_load(load, txn, "bcp_sendrow");

        if (batchRows > 0 && ++unbatched == batchRows) {
            Py_BEGIN_ALLOW_THREADS
            count = procs.bcp_batch(cnxn->hdbc);
            Py_END_ALLOW_THREADS
            if (count < 0)
                return !abandon_load(load, txn, "bcp_batch");
            if (!commit_load_batch(cnxn, txn))
                return !abandon_load(load, txn, "SQLEndTran");
            *copied += count;
            unbatched = 0;
        }
    }

    Py_BEGIN_ALLOW_THREADS
    count = procs.bcp_done(cnxn->hdbc);
    Py_END_ALLOW_THREADS
    if (count < 0) {
        RaiseErrorFromHandle(cnxn, "bcp_done", cnxn->hdbc, SQL_NULL_HANDLE);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    *copied += count;
    if (*copied != nrows) {
        PyErr_Format(PyExc_RuntimeError, "BCP copied %lld of %zd rows", *copied, nrows);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    if (!finish_load_transaction(cnxn, txn)) {
        RaiseErrorFromHandle(cnxn, "SQLEndTran", cnxn->hdbc, SQL_NULL_HANDLE);
        rollback_load_transaction(cnxn, txn);
        return false;
    }
    return true;
}

/**
 * Prepares a load: parses the INSERT, loads the destination columns, maps the parameters and
 * their setinputsizes entries (`inputSizes`, null if none) to columns, scans the rows and picks
 * host types, all before anything is sent.  A table with an enabled INSERT trigger
 * is left to the plain path: a trigger that rolls back a bulk copy run inside a transaction
 * leaves the driver unable to use the connection again.
 *
 * @return 1 to go ahead, 0 to fall back to the plain path, -1 on error (Python exception set).
 */
static int prepare_load(BcpLoad& load, PyObject* pSql, PyObject* param_seq, PyObject* inputSizes)
{
    Connection* cnxn = load.cnxn;

    Py_ssize_t sqlLen = 0;
    const char* sql = PyUnicode_AsUTF8AndSize(pSql, &sqlLen);
    if (!sql) {
        PyErr_Clear();
        return 0;
    }
    if ((Py_ssize_t)strlen(sql) != sqlLen)
        return 0;
    int r = BcpParseInsert(sql, &load.insert);
    if (r <= 0)
        return r;
    load.insertParsed = true;

    if (!BcpEnsureLoaded(cnxn))
        return PyErr_Occurred() ? -1 : 0;
    if (cnxn->bcp->sessionOpen || !BcpIsEnabled(cnxn->hdbc))
        return 0;

    if ((r = BcpLoadTarget(cnxn, load.insert, cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF, &load.target)) <= 0)
        return r;
    if (load.target.hasInsertTrigger)
        return 0;
    if ((r = map_columns(load)) <= 0)
        return r;
    if (!read_entries(load, inputSizes))
        return 0;

    load.rows = PySequence_Tuple(param_seq);
    if (!load.rows.IsValid())
        return -1;
    if (!scan_rows(load) || !choose_keep_nulls(load))
        return 0;
    if ((r = plan_columns(load)) <= 0)
        return r;

    load.decimalType = GetClassForThread("decimal", "Decimal");
    load.decimalFormat = PyUnicode_FromString("f");
    load.normalizeName = PyUnicode_InternFromString("normalize");
    return load.decimalType.IsValid() && load.decimalFormat.IsValid() && load.normalizeName.IsValid() ? 1 : -1;
}

bool ExecuteMulti_BCP(Cursor* cur, PyObject* pSql, PyObject* param_seq)
{
    if (!cur->use_bcp_fast || !PyUnicode_Check(pSql))
        return false;

    BcpLoad load(cur);
    int prepared = prepare_load(load, pSql, param_seq, cur->inputsizes);
    if (prepared <= 0)
        return prepared < 0;

    LoadTransaction txn = {false, false};
    if (!begin_load_transaction(load.cnxn, txn))
        return false;
    if (!start_bulk_copy(load, cur, txn)) {
        rollback_load_transaction(load.cnxn, txn);
        return PyErr_Occurred() != 0;
    }

    long long copied = 0;
    if (send_rows(load, cur, txn, &copied))
        cur->rowcount = (int)copied;
    return true;
}
