#pragma once

// SQL Server bulk copy (BCP) support for the low-level Connection.bcp_* methods
// (connection.cpp).  Include after pyodbc.h.
//
// The BCP entry points are exported by the Microsoft ODBC driver itself rather than the driver
// manager, so they are resolved from the loaded driver at run time and no SQL Server SDK
// header is needed to build.  The constants below match msodbcsql.h and sqlncli.h.

#ifndef DB_IN
#define DB_IN 1
#endif
#ifndef SUCCEED
#define SUCCEED 1
#endif
#ifndef FAIL
#define FAIL 0
#endif

// DBINT is a 32-bit signed integer in the driver: LONG on Windows, INT on Linux.  It must be
// `int` here, not `long`, which is 8 bytes on LP64 platforms.
typedef int DBINT;

#ifndef SQL_COPT_SS_BCP
#define SQL_COPT_SS_BCP 1219
#endif
#ifndef SQL_BCP_ON
#define SQL_BCP_ON 1
#endif
#ifndef SQL_VARLEN_DATA
#define SQL_VARLEN_DATA (-10)
#endif

// Host (program variable) data types for bcp_bind.
#ifndef SQLBIT
#define SQLBIT 0x32
#endif
#ifndef SQLINT2
#define SQLINT2 0x34
#endif
#ifndef SQLINT4
#define SQLINT4 0x38
#endif
#ifndef SQLINT8
#define SQLINT8 0x7F
#endif
#ifndef SQLFLT4
#define SQLFLT4 0x3B
#endif
#ifndef SQLFLT8
#define SQLFLT8 0x3E
#endif
#ifndef SQLBINARY
#define SQLBINARY 0x2D
#endif
#ifndef SQLUNIQUEID
#define SQLUNIQUEID 0x24
#endif
#ifndef SQLCHARACTER
#define SQLCHARACTER 0x2F
#endif
#ifndef SQLNCHAR
#define SQLNCHAR 0xEF
#endif

// bcp_control options.  BCPHINTS is the ANSI variant (BCPHINTSA), taking a char* hint list.
#ifndef BCPMAXERRS
#define BCPMAXERRS 1
#endif
#ifndef BCPKEEPNULLS
#define BCPKEEPNULLS 5
#endif
#ifndef BCPHINTS
#define BCPHINTS 10
#endif

struct Connection;

/** The driver's BCP entry points. */
struct BcpProcs
{
    SQLRETURN (SQL_API *bcp_initW)(HDBC, const SQLWCHAR*, const SQLWCHAR*, const SQLWCHAR*, int);
    SQLRETURN (SQL_API *bcp_bind)(HDBC, const unsigned char*, int, DBINT, const unsigned char*, int, int, int);
    SQLRETURN (SQL_API *bcp_collen)(HDBC, DBINT, int);
    SQLRETURN (SQL_API *bcp_sendrow)(HDBC);
    DBINT     (SQL_API *bcp_batch)(HDBC);
    DBINT     (SQL_API *bcp_done)(HDBC);
    SQLRETURN (SQL_API *bcp_control)(HDBC, int, void*);
};

/**
 * Per-connection BCP state: the resolved entry points and the low-level API session.
 *
 * `loaded` is false when the driver does not export the BCP API.  A session opened by
 * Connection.bcp_init stays open until bcp_done.  While it is open the connection owns the
 * capsule of the bound context, so the buffers the driver reads from cannot be freed under it.
 * In autocommit mode the session runs in its own transaction (`ownsTransaction`), committed by
 * each batch, so rows sent since the last batch can be discarded rather than committed.
 * `rowReady` is set by a successful bcp_setrow and cleared by bcp_sendrow, so a row is never
 * sent half converted or twice by mistake.
 */
struct BcpState
{
    bool loaded;
    BcpProcs procs;
    bool sessionOpen;
    bool ownsTransaction;
    bool rowReady;
    PyObject* context;
    long batchRows;
    long unbatchedRows;
};

/**
 * One bound column: its host type and the buffer the driver reads on each bcp_sendrow.
 *
 * Variable-length values are laid out as a little-endian length prefix of `prefix` bytes
 * followed by the data; a prefix of -1 is NULL.  Fixed-width values (prefix 0) carry no
 * length, so NULL is signalled with bcp_collen and `wasNull` remembers to undo it.  An 8-byte
 * prefix is required for (max) columns, where the driver misreads a zero-length prefix, so
 * `setsCollen` also passes every value's length to bcp_collen.
 */
struct BcpCol
{
    int ordinal;
    int hostType;
    int prefix;
    bool setsCollen;
    DBINT fixedSize;
    unsigned char* scratch;
    size_t scratchCap;
    bool wasNull;
};

/** The columns bound by Connection.bcp_bind_columns, held in a capsule. */
struct BcpCtx
{
    Connection* conn;
    int ncols;
    BcpCol* cols;
};

/**
 * Resolves the driver's BCP entry points once per connection.
 *
 * @return true if the driver exports them; false if not, or on allocation failure (with a
 *         Python exception set).
 */
bool BcpEnsureLoaded(Connection* cnxn);

/** Switches the connection's autocommit mode, leaving the diagnostics on the hdbc on failure. */
bool BcpSetAutocommit(HDBC hdbc, bool on);

/** Commits or rolls back the connection's transaction, leaving the diagnostics on failure. */
bool BcpEndTransaction(HDBC hdbc, bool commit);

/**
 * Sizes and allocates a column's buffer.
 *
 * The caller sets ordinal, hostType, prefix (0 for the fixed-width host types SQLBIT, SQLINT2,
 * SQLINT4, SQLINT8, SQLFLT4, SQLFLT8 and SQLUNIQUEID; 4 or 8 otherwise) and setsCollen.
 *
 * @return false with MemoryError set on failure.
 */
bool BcpAllocColumn(BcpCol* c);

/** Frees the buffers of an array of columns and the array itself. */
void BcpFreeColumns(BcpCol* cols, int ncols);

/** Binds a column's buffer with bcp_bind.  On failure the driver diagnostics are on the hdbc. */
bool BcpBindColumn(Connection* cnxn, BcpCol* c);

/**
 * Sets a column to NULL for the next row.
 *
 * Like all the BcpPut functions it returns false either with a Python exception set or, for a
 * driver failure, with the diagnostics left on the hdbc.
 */
bool BcpPutNull(Connection* cnxn, BcpCol* c);

/** Copies a fixed-width value (fixedSize bytes) into the column for the next row. */
bool BcpPutFixed(Connection* cnxn, BcpCol* c, const void* value);

/** Copies n bytes of variable-length data into the column, growing its buffer as needed. */
bool BcpPutBytes(Connection* cnxn, BcpCol* c, const void* data, Py_ssize_t n);

/**
 * Widens 8-bit text to UTF-16LE in a SQLNCHAR column, taking each byte as the code point of
 * the same value: exact for ASCII and for the data of a 1-byte (Latin-1) str.
 */
bool BcpPutWidened(Connection* cnxn, BcpCol* c, const unsigned char* text, Py_ssize_t n);

/**
 * A low-level API value converted for its column, ready to be written without running any
 * Python code.  `holder` keeps variable-length data alive: the immutable str or bytes it points
 * into, a copy of a bytearray, or the UTF-16 encoding of a str.  `widen` marks 8-bit text (an
 * ASCII or Latin-1 str) still to be widened for a SQLNCHAR column.
 */
struct BcpLowLevelValue
{
    bool isNull;
    unsigned char fixed[16];
    PyObject* holder;
    const unsigned char* data;
    Py_ssize_t size;
    bool widen;

    BcpLowLevelValue() : isNull(true), holder(0), data(0), size(0), widen(false) {}
    BcpLowLevelValue(const BcpLowLevelValue&) = delete;
    BcpLowLevelValue& operator=(const BcpLowLevelValue&) = delete;
    ~BcpLowLevelValue() { Py_XDECREF(holder); }
};

/**
 * Converts a Python value for a low-level API column of the given host type.  This is where
 * user code can run (__float__, uuid attributes), so it touches neither the column buffer nor
 * the driver: BcpWriteLowLevelValue does that once the caller has checked the session survived.
 *
 * @return false with a Python exception set for an unsuitable value.
 */
bool BcpConvertLowLevelValue(const BcpCol* c, PyObject* value, BcpLowLevelValue* out);

/**
 * Writes a converted low-level value into its column for the next row.
 *
 * @return false with a Python exception set, or with the driver diagnostics on the hdbc.
 */
bool BcpWriteLowLevelValue(Connection* cnxn, BcpCol* c, const BcpLowLevelValue& value);

/** Writes the low `len` bytes of v to dst, least significant first. */
void write_le(unsigned char* dst, unsigned long long v, int len);
