#pragma once

// --- Windows prerequisites so ODBC SAL/GUID types are present everywhere this header is used
#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#ifndef ODBCVER
    #define ODBCVER 0x0380
#endif

// --- ODBC prerequisites
#include <sql.h>
#include <sqlext.h>

// --- Provide minimal typedefs if the BCP headers are not included

/* ---- DB-Library style constants used by the BCP API ---- */
#ifndef DB_IN
#define DB_IN 1
#endif
#ifndef DB_OUT
#define DB_OUT 2
#endif
#ifndef SUCCEED
#define SUCCEED 1
#endif
#ifndef FAIL
#define FAIL 0
#endif

#ifndef DBINT
// DBINT is 32-bit signed in the SQL Server driver. Must be `int`, not `long`:
// on LP64 (Linux/macOS) `long` is 8 bytes, which makes sizeof(DBINT) wrong for
// SQLINT4 binds and mismatches the driver's bcp_bind ABI.
typedef int DBINT;
#endif

#ifndef BYTE
typedef unsigned char BYTE;
#endif
#ifndef LPCBYTE
typedef const BYTE* LPCBYTE;
#endif

// ---- Minimal constants (used only if you can't include msodbcsql.h)
#ifndef SQL_COPT_SS_BCP
#define SQL_COPT_SS_BCP 1219       // ODBC Driver 17/18
#endif
#ifndef SQL_BCP_ON
#define SQL_BCP_ON 1
#endif
#ifndef SQL_VARLEN_DATA
#define SQL_VARLEN_DATA (-10)
#endif

// Supported Host types (fallbacks; prefer exporting real ones from the module)
#ifndef SQLBIT                  // BIT (1 byte)
#define SQLBIT  0x32
#endif
#ifndef SQLINT2                 // SMALLINT (2 bytes)
#define SQLINT2 0x34
#endif
#ifndef SQLINT4                 // INTEGER (4 bytes)
#define SQLINT4 0x38               
#endif
#ifndef SQLINT8                 // BIGINT (8 bytes)
#define SQLINT8 0x7F
#endif
#ifndef SQLFLT8                 // FLOAT (8 bytes)
#define SQLFLT8 0x3E               
#endif
#ifndef SQLFLT4                 // REAL / FLOAT(24) (4 bytes)
#define SQLFLT4 0x3B
#endif
#ifndef SQLBINARY               // BINARY / VARBINARY host type
#define SQLBINARY 0x2D
#endif
#ifndef SQLUNIQUEID             // UNIQUEIDENTIFIER host type
#define SQLUNIQUEID 0x24
#endif
#ifndef SQLCHARACTER            // CHAR / VARCHAR / TEXT host type
#define SQLCHARACTER 0x2F          
#endif
#ifndef SQLTIMEN                // host type for TIME_STRUCT
#define SQLTIMEN 0x29
#endif
#ifndef SQLDATEN
#define SQLDATEN 0x28
#endif
#ifndef SQLDATETIME2N
#define SQLDATETIME2N 0x2A
#endif
#ifndef SQLDATETIMEOFFSETN
#define SQLDATETIMEOFFSETN 0x2B
#endif

// Supported bcp_control options (guarded):
#ifndef BCPBATCH
#define BCPBATCH 4              // same as msodbcsql.h
#endif
#ifndef BCPMAXERRS
#define BCPMAXERRS 1            // same as msodbcsql.h
#endif
#ifndef BCPKEEPNULLS
#define BCPKEEPNULLS 5          // same as msodbcsql.h
#endif
#ifndef BCPHINTS
#define BCPHINTS 10             // same as msodbcsql.h
#endif

struct Connection;  // forward declaration

struct BcpProcs {
    // ANSI signatures (the A-suffixed variants)
    SQLRETURN (SQL_API *bcp_initA)(HDBC, const char*, const char*, const char*, int);
    SQLRETURN (SQL_API *bcp_bind)  (HDBC, const BYTE*, int, DBINT, const BYTE*, int, int, int);
    SQLRETURN (SQL_API *bcp_collen)(HDBC, DBINT, int);
    SQLRETURN (SQL_API *bcp_colptr)(HDBC, const BYTE*, int);
    SQLRETURN (SQL_API *bcp_sendrow)(HDBC);
    DBINT     (SQL_API *bcp_batch)(HDBC);
    DBINT     (SQL_API *bcp_done)(HDBC);
    SQLRETURN (SQL_API *bcp_control)(HDBC, int, void*);
    bool loaded = false;
};

// Load once per connection
bool BcpLoadFromDriver(HDBC hdbc, BcpProcs& out);

// Convenience macro to check availability
#define HAS_BCP(p) ((p).loaded)

static inline char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

typedef struct BcpCol_
{
    int ordinal;                // 1-based
    int hostType;               // SQLINT4, SQLFLT8, SQLCHARACTER, SQLINT2, SQLINT8...
    bool isVarLen;              // 1 if varlen (SQLCHARACTER), else 0
    int ind;                    // byte length indicator for varlen; always 0 for fixed
    size_t fixedSize;           // for fixed types
    unsigned char* scratch;     // reusable per-column buffer
    DBINT scratchCap;           // bytes
    bool was_null;              // true if the previous row bound SQL_NULL_DATA for this column
} BcpCol;

typedef struct BcpCtx_ {
    Connection* conn;                   // borrowed from Connection
    int         ncols;
    BcpCol*     cols;
    DBINT       total_committed;        // running total (optional)
} BcpCtx;

/**
 * @brief Parses "INSERT INTO [[cat.]schema.]table [ (col, ...) ] VALUES" for BCP.
 *
 * Extracts the verbatim qualified table reference (for bcp_init), the unquoted
 * schema and table names (for SQLColumns), and the optional explicit column
 * list. Only a parameterized VALUES insert is accepted; INSERT ... SELECT/EXEC,
 * table hints and DEFAULT VALUES return 0 so the caller falls back.
 *
 * @param sql          The SQL INSERT statement (null-terminated).
 * @param tableref     Output buffer for the verbatim table reference.
 * @param tableref_cap Capacity of tableref.
 * @param schema       Output buffer for the unquoted schema ("" if none).
 * @param schema_cap   Capacity of schema.
 * @param table        Output buffer for the unquoted table name.
 * @param table_cap    Capacity of table.
 * @param out_names    Receives a PyMem-allocated array of column-name strings
 *                     (NULL if no explicit list); free with bcp_free_names().
 * @param out_ncols    Receives the number of column names (0 if no list).
 * @return 1 on success, 0 to fall back (PyErr may be set on allocation failure).
 */
int parse_insert_target(const char* sql,
                        char* tableref, int tableref_cap,
                        char* schema, int schema_cap,
                        char* table, int table_cap,
                        char*** out_names, int* out_ncols);

/** @brief Frees the column-name array produced by parse_insert_target(). */
void bcp_free_names(char** names, int n);

/**
 * @brief Resolves column names to 1-based table ordinals via SQLColumns.
 *
 * Used to honour an explicit INSERT column list: BCP binds by ordinal, so each
 * named column must be mapped to its position in the destination table.
 *
 * @param hdbc     Connection handle (a temporary statement is allocated on it).
 * @param schema   Unquoted schema, or "" / NULL to match the default schema.
 * @param table    Unquoted table name.
 * @param names      Column names to resolve.
 * @param n          Number of names.
 * @param out_ord    Receives the 1-based ordinal for each name.
 * @param out_total  Receives the total number of columns in the table (so the
 *                   caller can detect a partial column list, which BCP cannot
 *                   honour without overriding column DEFAULTs).
 * @return 1 if every name resolved, 0 otherwise.
 */
int bcp_resolve_ordinals(HDBC hdbc, const char* schema, const char* table,
                         char** names, int n, int* out_ord, int* out_total);

/**
 * @brief Ensures that the Bulk Copy Program (BCP) library is loaded for the given connection.
 *
 * This function checks if the BCP library required for bulk data operations is loaded
 * for the specified connection. If not, it attempts to load the library.
 *
 * @param self Pointer to the Connection object for which the BCP library should be loaded.
 * @return true if the BCP library is successfully loaded or already loaded; false otherwise.
 */
bool ensure_bcp_loaded(Connection* self);

/**
 * @brief Frees resources associated with a BCP context capsule.
 *
 * This function is intended to be used as a destructor for Python capsule objects
 * that encapsulate BCP (Bulk Copy Program) context data. It releases any memory or
 * resources held by the capsule to prevent memory leaks.
 *
 * @param cap A pointer to the Python capsule object containing the BCP context.
 */
void BcpCtx_FreeCapsule(PyObject* cap);

/**
 * @brief Sets the column pointer for a BCP column in the given context.
 *
 * This function assigns the appropriate data pointer for the specified column
 * within the Bulk Copy Program (BCP) context. It is typically used to prepare
 * the column for data transfer operations.
 *
 * @param ctx Pointer to the BCP context structure.
 * @param c Pointer to the BCP column structure whose data pointer is to be set.
 * @return Returns 0 on success, or a non-zero error code on failure.
 */
int _bcp_set_colptr(BcpCtx* ctx, BcpCol* c);

/**
 * @brief Frees the resources associated with a BcpCtx context.
 *
 * This function releases all memory and resources allocated for the specified
 * BcpCtx structure. After calling this function, the context pointer should not
 * be used unless reinitialized.
 *
 * @param ctx Pointer to the BcpCtx context to be freed.
 */
void _bcp_ctx_free(BcpCtx* ctx);

/**
 * @brief Rebinds the current column in the BCP context.
 *
 * This function updates the binding of the specified column within the given BCP context.
 * It is typically used when the column's data type or binding parameters have changed and
 * need to be reapplied for bulk copy operations. Most common cause buffer size increase.
 *
 * @param ctx Pointer to the BCP context structure.
 * @param c Pointer to the BCP column structure to be rebound.
 * @return Returns 0 on success, or a non-zero error code on failure.
 */
int _bcp_rebind_current(BcpCtx* ctx, BcpCol* c);

/**
 * @brief Bind all column buffers for bulk copy (BCP) operations.
 *
 * Iterates over all columns in the given BCP context and binds each column’s
 * scratch buffer using `bcp_bind()`. For fixed-size columns, the exact size is bound;
 * for variable-length columns, the size is set per row via `bcp_collen()`.
 *
 * Fails if a column has no scratch buffer or if any bind call fails.
 *
 * @param ctx  Pointer to an initialized BCP context with valid column metadata.
 * @return 1 on success, 0 on error (Python exception set).
 */
int _bcp_bind_all(BcpCtx* ctx);

/**
 * @brief Converts a Python value to a native buffer and binds it for BCP.
 *
 * Converts a single Python object to the column’s native type and writes it
 * into the column’s scratch buffer. Then updates the column length using
 * `bcp_collen()`. Handles `None` as SQL NULL and supports various SQL types
 * (BIT, INT2/4/8, FLOAT4/8, BINARY, CHAR, UNIQUEID, TIMEN, etc.).
 *
 * @param ctx   Active BCP context.
 * @param cell  Python object representing the cell value.
 * @param c     Target column descriptor.
 * @return 1 on success, 0 on error (Python exception set).
 */
int _bcp_fill_cell(BcpCtx* ctx, PyObject* cell, BcpCol* c);

/**
 * @brief Write an integer value to a buffer in little-endian order.
 *
 * Stores the lower @p len bytes of @p v into @p dst, least significant byte first.
 *
 * @param dst  Destination buffer.
 * @param v    Integer value to write.
 * @param len  Number of bytes to write (1–8).
 */
void write_le(unsigned char* dst, unsigned long long v, int len);

/**
 * @brief Convert a time value to SQL Server TIME(7) ticks.
 *
 * Computes the number of 100-nanosecond ticks since midnight,
 * where 1 microsecond equals 10 ticks.
 *
 * @param hh     Hours (0–23).
 * @param mm     Minutes (0–59).
 * @param ss     Seconds (0–59).
 * @param micro  Microseconds (0–999999).
 * @return Time in 10⁻⁷-second ticks since midnight.
 */
unsigned long long time_to_ticks7(int hh, int mm, int ss, int micro);

/**
 * @brief Compute days since 0001-01-01 (proleptic Gregorian).
 *
 * Converts a civil date (y, m, d) to a day count where 0001-01-01 == 0.
 * Implementation is based on Howard Hinnant’s days-from-civil algorithm.
 *
 * @param y Year (e.g., 2025). Proleptic Gregorian; no BCE/0-year handling.
 * @param m Month in [1, 12].
 * @param d Day in [1, 31] (no range validation performed).
 * @return unsigned int Days since 0001-01-01 (0 for 0001-01-01).
 *
 * @note Inputs are assumed valid; behavior is undefined for invalid dates.
 */
unsigned int days_since_0001_01_01(int y, int m, int d);

// --- Constant for conversion from days since 0001-01-01 to days since 1900-01-01
static const unsigned int DAYS_0001_TO_1900 = 693595U;
