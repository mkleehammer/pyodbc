#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "connection.h"
#include "pyodbcmodule.h"
#include "bcp_support.h"
#include <climits>
#include <cmath>
#include <cfloat>
#ifndef _WIN32
#include <dlfcn.h>
#endif


/**
 * Diagnostic logging toggle, controlled by the PYODBC_BCP_DEBUG environment variable and
 * cached on first use.  When enabled, driver loading reports to stderr why BCP is unavailable.
 */
static int bcp_debug_enabled(void)
{
    static int cached = -1;
    if (cached == -1) {
        const char* v = getenv("PYODBC_BCP_DEBUG");
        cached = (v && v[0] && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
    }
    return cached;
}

#define BCPLOAD_DBG(...) do { if (bcp_debug_enabled()) { fprintf(stderr, "[bcp] " __VA_ARGS__); fflush(stderr); } } while (0)

static bool get_driver_name(HDBC hdbc, char* buf, SQLSMALLINT buflen)
{
    SQLSMALLINT outlen = 0;
    if (SQLGetInfo(hdbc, SQL_DRIVER_NAME, (SQLPOINTER)buf, buflen, &outlen) != SQL_SUCCESS)
        return false;
    buf[buflen - 1] = '\0';
    return true;
}

#ifdef __linux__
/**
 * Finds the absolute path of the mapped library whose file name is exactly `soname`.
 *
 * SQLGetInfo(SQL_DRIVER_NAME) reports a bare versioned soname such as
 * "libmsodbcsql-18.5.so.1.1", which dlopen cannot resolve unless its directory is on the loader
 * path, but the driver manager has already mapped the library, so /proc/self/maps has its path.
 * Only an exact match is accepted, so a process with two driver versions loaded gets the right one.
 */
static bool find_mapped_library(const char* soname, char* out, size_t outcap)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f)
        return false;
    char line[8192];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char* path = strchr(line, '/');
        if (!path)
            continue;
        size_t len = strlen(path);
        while (len && (path[len - 1] == '\n' || path[len - 1] == '\r'))
            path[--len] = '\0';
        const char* base = strrchr(path, '/') + 1;
        if (strcmp(base, soname) == 0 && len < outcap) {
            memcpy(out, path, len + 1);
            found = true;
        }
    }
    fclose(f);
    return found;
}
#endif

/**
 * Returns a handle to the already-loaded driver library, never loading another copy.
 */
static void* find_driver_module(const char* drv)
{
#ifdef _WIN32
    return (void*)GetModuleHandleA(drv);
#else
    void* mod = dlopen(drv, RTLD_NOW | RTLD_NOLOAD);
#ifdef __linux__
    if (!mod) {
        const char* slash = strrchr(drv, '/');
        char path[4096];
        if (find_mapped_library(slash ? slash + 1 : drv, path, sizeof(path))) {
            BCPLOAD_DBG("load: driver mapped at '%s'\n", path);
            mod = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        }
    }
#endif
    return mod;
#endif
}

template <class TFn>
static bool resolve(void* mod, const char* name, TFn& out)
{
#ifdef _WIN32
    out = reinterpret_cast<TFn>(GetProcAddress((HMODULE)mod, name));
#else
    out = reinterpret_cast<TFn>(dlsym(mod, name));
#endif
    if (!out)
        BCPLOAD_DBG("load: missing %s\n", name);
    return out != nullptr;
}

/** Returns true for the Microsoft ODBC Driver for SQL Server (msodbcsql*.dll, libmsodbcsql*). */
static bool is_microsoft_driver(const char* drv)
{
    const char* base = strrchr(drv, '/');
    base = base ? base + 1 : drv;
    const char* prefix = strncmp(base, "lib", 3) == 0 ? base + 3 : base;
    char lower[16] = {0};
    for (int i = 0; i < 9 && prefix[i]; ++i)
        lower[i] = (prefix[i] >= 'A' && prefix[i] <= 'Z') ? (char)(prefix[i] + 32) : prefix[i];
    return strncmp(lower, "msodbcsql", 9) == 0;
}

/** Returns the major version in SQLGetInfo(SQL_DRIVER_VER), "##.##.####", or 0 if unknown. */
static int get_driver_major_version(HDBC hdbc)
{
    char ver[32] = {0};
    SQLSMALLINT outlen = 0;
    if (SQLGetInfo(hdbc, SQL_DRIVER_VER, (SQLPOINTER)ver, sizeof(ver), &outlen) != SQL_SUCCESS)
        return 0;
    ver[sizeof(ver) - 1] = '\0';
    return atoi(ver);
}

/**
 * Resolves the BCP entry points from the driver behind hdbc.  Only the Microsoft ODBC Driver
 * 13 or later is used.  FreeTDS exports the same functions but differs in its conversions and
 * diagnostics, which the fast path's guarantees rest on.  SQL Server Native Client and Driver
 * 11 leave a failed bcp_batch or bcp_done without diagnostics when the Windows driver manager
 * pools connections, as pyodbc has it do by default, so their errors could not be raised.
 */
static bool load_driver_procs(HDBC hdbc, BcpProcs& out)
{
    char drv[256] = {0};
    if (!get_driver_name(hdbc, drv, sizeof(drv))) {
        BCPLOAD_DBG("load: SQLGetInfo(SQL_DRIVER_NAME) failed\n");
        return false;
    }
    BCPLOAD_DBG("load: driver name = '%s'\n", drv);
    if (!is_microsoft_driver(drv)) {
        BCPLOAD_DBG("load: not the Microsoft ODBC Driver for SQL Server\n");
        return false;
    }
    int major = get_driver_major_version(hdbc);
    if (major < 13) {
        BCPLOAD_DBG("load: driver version %d is older than 13\n", major);
        return false;
    }

    void* mod = find_driver_module(drv);
    if (!mod) {
        BCPLOAD_DBG("load: driver module '%s' is not loaded\n", drv);
        return false;
    }
    return resolve(mod, "bcp_initW",   out.bcp_initW)
        && resolve(mod, "bcp_bind",    out.bcp_bind)
        && resolve(mod, "bcp_collen",  out.bcp_collen)
        && resolve(mod, "bcp_sendrow", out.bcp_sendrow)
        && resolve(mod, "bcp_batch",   out.bcp_batch)
        && resolve(mod, "bcp_done",    out.bcp_done)
        && resolve(mod, "bcp_control", out.bcp_control);
}

bool BcpEnsureLoaded(Connection* cnxn)
{
    if (cnxn->bcp)
        return cnxn->bcp->loaded;
    if (!cnxn->hdbc)
        return false;
    BcpState* state = (BcpState*)PyMem_Calloc(1, sizeof(BcpState));
    if (!state) {
        PyErr_NoMemory();
        return false;
    }
    state->loaded = load_driver_procs(cnxn->hdbc, state->procs);
    cnxn->bcp = state;
    return state->loaded;
}

bool BcpSetAutocommit(HDBC hdbc, bool on)
{
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT,
                           (SQLPOINTER)(uintptr_t)(on ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF), SQL_IS_UINTEGER);
    Py_END_ALLOW_THREADS
    return SQL_SUCCEEDED(rc);
}

bool BcpEndTransaction(HDBC hdbc, bool commit)
{
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = SQLEndTran(SQL_HANDLE_DBC, hdbc, commit ? SQL_COMMIT : SQL_ROLLBACK);
    Py_END_ALLOW_THREADS
    return SQL_SUCCEEDED(rc);
}

/*=======================================================================================*/
// Column buffers
/*=======================================================================================*/

void write_le(unsigned char* dst, unsigned long long v, int len)
{
    for (int i = 0; i < len; ++i)
        dst[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static DBINT fixed_size_of(int hostType)
{
    switch (hostType) {
    case SQLBIT:      return 1;
    case SQLINT2:     return 2;
    case SQLINT4:     return 4;
    case SQLFLT4:     return 4;
    case SQLINT8:     return 8;
    case SQLFLT8:     return 8;
    case SQLUNIQUEID: return 16;
    }
    return 0;
}

bool BcpAllocColumn(BcpCol* c)
{
    c->fixedSize = c->prefix ? 0 : fixed_size_of(c->hostType);
    c->scratchCap = c->prefix ? (size_t)c->prefix + 256 : (size_t)c->fixedSize;
    c->scratch = (unsigned char*)PyMem_Calloc(1, c->scratchCap);
    if (!c->scratch) {
        PyErr_NoMemory();
        return false;
    }
    if (c->prefix)
        write_le(c->scratch, ~0ULL, c->prefix);
    return true;
}

void BcpFreeColumns(BcpCol* cols, int ncols)
{
    if (!cols)
        return;
    for (int i = 0; i < ncols; ++i)
        PyMem_Free(cols[i].scratch);
    PyMem_Free(cols);
}

bool BcpBindColumn(Connection* cnxn, BcpCol* c)
{
    const DBINT cbData = c->prefix ? SQL_VARLEN_DATA : c->fixedSize;
    return cnxn->bcp->procs.bcp_bind(cnxn->hdbc, c->scratch, c->prefix, cbData, nullptr, 0,
                                     c->hostType, c->ordinal) == SUCCEED;
}

bool BcpPutNull(Connection* cnxn, BcpCol* c)
{
    if (c->prefix) {
        write_le(c->scratch, ~0ULL, c->prefix);
        return true;
    }
    if (cnxn->bcp->procs.bcp_collen(cnxn->hdbc, SQL_NULL_DATA, c->ordinal) == FAIL)
        return false;
    c->wasNull = true;
    return true;
}

bool BcpPutFixed(Connection* cnxn, BcpCol* c, const void* value)
{
    memcpy(c->scratch, value, (size_t)c->fixedSize);
    if (c->wasNull) {
        if (cnxn->bcp->procs.bcp_collen(cnxn->hdbc, c->fixedSize, c->ordinal) == FAIL)
            return false;
        c->wasNull = false;
    }
    return true;
}

/**
 * Makes room for n data bytes after the prefix, rebinding the column if its buffer moves, and
 * writes the prefix (and, for setsCollen columns, bcp_collen).  The buffer grows geometrically
 * so a column of steadily longer values does not rebind on every row.
 */
static bool reserve_varlen(Connection* cnxn, BcpCol* c, long long n)
{
    if (n > (long long)INT_MAX - c->prefix) {
        PyErr_SetString(PyExc_OverflowError, "value too long for a BCP column");
        return false;
    }
    const size_t need = (size_t)c->prefix + (size_t)n;
    if (need > c->scratchCap) {
        const size_t cap = c->scratchCap * 2 > need ? c->scratchCap * 2 : need;
        unsigned char* p = (unsigned char*)PyMem_Realloc(c->scratch, cap);
        if (!p) {
            PyErr_NoMemory();
            return false;
        }
        c->scratch = p;
        c->scratchCap = cap;
        if (!BcpBindColumn(cnxn, c))
            return false;
    }
    write_le(c->scratch, (unsigned long long)n, c->prefix);
    return !c->setsCollen || cnxn->bcp->procs.bcp_collen(cnxn->hdbc, (DBINT)n, c->ordinal) != FAIL;
}

bool BcpPutBytes(Connection* cnxn, BcpCol* c, const void* data, Py_ssize_t n)
{
    if (!reserve_varlen(cnxn, c, n))
        return false;
    if (n > 0)
        memcpy(c->scratch + c->prefix, data, (size_t)n);
    return true;
}

bool BcpPutWidened(Connection* cnxn, BcpCol* c, const unsigned char* text, Py_ssize_t n)
{
    if (!reserve_varlen(cnxn, c, 2LL * n))
        return false;
    unsigned char* dst = c->scratch + c->prefix;
    for (Py_ssize_t i = 0; i < n; ++i) {
        dst[2 * i]     = text[i];
        dst[2 * i + 1] = 0;
    }
    return true;
}

/*=======================================================================================*/
// Low-level API values
/*=======================================================================================*/

static bool convert_low_level_integer(const BcpCol* c, PyObject* value, BcpLowLevelValue* out)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(PyExc_TypeError, "an int is required for an integer BCP column, not %.100s", Py_TYPE(value)->tp_name);
        return false;
    }
    long long v = PyLong_AsLongLong(value);
    if (v == -1 && PyErr_Occurred())
        return false;
    if (c->hostType == SQLINT2) {
        if (v < SHRT_MIN || v > SHRT_MAX) {
            PyErr_SetString(PyExc_OverflowError, "value out of range for a SQLINT2 BCP column");
            return false;
        }
        short s = (short)v;
        memcpy(out->fixed, &s, sizeof(s));
    } else if (c->hostType == SQLINT4) {
        if (v < INT_MIN || v > INT_MAX) {
            PyErr_SetString(PyExc_OverflowError, "value out of range for a SQLINT4 BCP column");
            return false;
        }
        int i = (int)v;
        memcpy(out->fixed, &i, sizeof(i));
    } else {
        memcpy(out->fixed, &v, sizeof(v));
    }
    return true;
}

static bool convert_low_level_float(const BcpCol* c, PyObject* value, BcpLowLevelValue* out)
{
    double d = PyFloat_AsDouble(value);
    if (d == -1.0 && PyErr_Occurred())
        return false;
    if (c->hostType == SQLFLT8) {
        memcpy(out->fixed, &d, sizeof(d));
        return true;
    }
    if (std::isfinite(d) && std::fabs(d) > FLT_MAX) {
        PyErr_SetString(PyExc_OverflowError, "value out of range for a SQLFLT4 BCP column");
        return false;
    }
    float f = (float)d;
    memcpy(out->fixed, &f, sizeof(f));
    return true;
}

static bool convert_low_level_bit(PyObject* value, BcpLowLevelValue* out)
{
    if (!PyLong_Check(value)) {
        PyErr_Format(PyExc_TypeError, "a bool is required for a SQLBIT BCP column, not %.100s", Py_TYPE(value)->tp_name);
        return false;
    }
    long long v = PyLong_AsLongLong(value);
    if (v == -1 && PyErr_Occurred())
        return false;
    if (v != 0 && v != 1) {
        PyErr_SetString(PyExc_ValueError, "SQLBIT BCP columns take True, False, 0 or 1");
        return false;
    }
    out->fixed[0] = (unsigned char)v;
    return true;
}

static bool convert_low_level_uuid(PyObject* value, BcpLowLevelValue* out)
{
    if (PyBytes_Check(value) && PyBytes_GET_SIZE(value) == 16) {
        memcpy(out->fixed, PyBytes_AS_STRING(value), 16);
        return true;
    }
    PyObject* uuid_cls = 0;
    if (IsInstanceForThread(value, "uuid", "UUID", &uuid_cls) && uuid_cls) {
        Py_DECREF(uuid_cls);
        Object le(PyObject_GetAttrString(value, "bytes_le"));
        if (!le.IsValid())
            return false;
        if (PyBytes_Check(le.Get()) && PyBytes_GET_SIZE(le.Get()) == 16) {
            memcpy(out->fixed, PyBytes_AS_STRING(le.Get()), 16);
            return true;
        }
    }
    if (!PyErr_Occurred())
        PyErr_SetString(PyExc_TypeError, "a uuid.UUID or 16 bytes is required for a SQLUNIQUEID BCP column");
    return false;
}

/** Holds a reference to immutable data (bytes or an ASCII/Latin-1 str) without copying it. */
static void hold(BcpLowLevelValue* out, PyObject* owner, const void* data, Py_ssize_t size)
{
    Py_INCREF(owner);
    out->holder = owner;
    out->data = (const unsigned char*)data;
    out->size = size;
}

/** Bytes as they are; a bytearray is copied, since later Python code could resize it. */
static bool convert_low_level_binary(PyObject* value, BcpLowLevelValue* out)
{
    if (PyBytes_Check(value)) {
        hold(out, value, PyBytes_AS_STRING(value), PyBytes_GET_SIZE(value));
        return true;
    }
    PyObject* copy = PyBytes_FromStringAndSize(PyByteArray_AS_STRING(value), PyByteArray_GET_SIZE(value));
    if (!copy)
        return false;
    out->holder = copy;
    out->data = (const unsigned char*)PyBytes_AS_STRING(copy);
    out->size = PyBytes_GET_SIZE(copy);
    return true;
}

/**
 * SQLCHARACTER data is read in the client's ANSI code page, so only bytes (already encoded by
 * the caller) and ASCII str, which every code page shares, can be sent unchanged.
 */
static bool convert_low_level_character(PyObject* value, BcpLowLevelValue* out)
{
    if (PyUnicode_Check(value)) {
        if (!PyUnicode_IS_ASCII(value)) {
            PyErr_SetString(PyExc_ValueError,
                            "SQLCHARACTER BCP columns are read in the client code page, so they only take "
                            "ASCII str or encoded bytes; use SQLNCHAR for other text");
            return false;
        }
        hold(out, value, PyUnicode_1BYTE_DATA(value), PyUnicode_GET_LENGTH(value));
        return true;
    }
    if (PyBytes_Check(value) || PyByteArray_Check(value))
        return convert_low_level_binary(value, out);
    PyErr_Format(PyExc_TypeError, "a str or bytes is required for a SQLCHARACTER BCP column, not %.100s", Py_TYPE(value)->tp_name);
    return false;
}

/** A str as UTF-16LE: a 1-byte str is widened when written, a wider one encoded now. */
static bool convert_low_level_wide(PyObject* value, BcpLowLevelValue* out)
{
    if (!PyUnicode_Check(value)) {
        PyErr_Format(PyExc_TypeError, "a str is required for a SQLNCHAR BCP column, not %.100s", Py_TYPE(value)->tp_name);
        return false;
    }
    if (PyUnicode_KIND(value) == PyUnicode_1BYTE_KIND) {
        hold(out, value, PyUnicode_1BYTE_DATA(value), PyUnicode_GET_LENGTH(value));
        out->widen = true;
        return true;
    }
    PyObject* utf16 = PyUnicode_AsEncodedString(value, "utf-16-le", "strict");
    if (!utf16)
        return false;
    out->holder = utf16;
    out->data = (const unsigned char*)PyBytes_AS_STRING(utf16);
    out->size = PyBytes_GET_SIZE(utf16);
    return true;
}

bool BcpConvertLowLevelValue(const BcpCol* c, PyObject* value, BcpLowLevelValue* out)
{
    out->isNull = value == Py_None;
    if (out->isNull)
        return true;

    switch (c->hostType) {
    case SQLBIT:
        return convert_low_level_bit(value, out);
    case SQLINT2:
    case SQLINT4:
    case SQLINT8:
        return convert_low_level_integer(c, value, out);
    case SQLFLT4:
    case SQLFLT8:
        return convert_low_level_float(c, value, out);
    case SQLUNIQUEID:
        return convert_low_level_uuid(value, out);
    case SQLCHARACTER:
        return convert_low_level_character(value, out);
    case SQLNCHAR:
        return convert_low_level_wide(value, out);
    case SQLBINARY:
        if (PyBytes_Check(value) || PyByteArray_Check(value))
            return convert_low_level_binary(value, out);
        PyErr_Format(PyExc_TypeError, "bytes are required for a SQLBINARY BCP column, not %.100s", Py_TYPE(value)->tp_name);
        return false;
    }
    PyErr_SetString(PyExc_TypeError, "unsupported BCP host type");
    return false;
}

bool BcpWriteLowLevelValue(Connection* cnxn, BcpCol* c, const BcpLowLevelValue& value)
{
    if (value.isNull)
        return BcpPutNull(cnxn, c);
    if (!c->prefix)
        return BcpPutFixed(cnxn, c, value.fixed);
    if (value.widen)
        return BcpPutWidened(cnxn, c, value.data, value.size);
    return BcpPutBytes(cnxn, c, value.data, value.size);
}
