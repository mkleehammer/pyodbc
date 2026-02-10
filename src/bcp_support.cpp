#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "connection.h"
#include "bcp_support.h"
#include <datetime.h>


static bool get_driver_name(HDBC hdbc, char* buf, SQLSMALLINT buflen) {
    SQLSMALLINT outlen = 0;
    if (SQLGetInfo(hdbc, SQL_DRIVER_NAME, (SQLPOINTER)buf, buflen, &outlen) != SQL_SUCCESS)
        return false;
    buf[buflen-1] = '\0';
    return true;
}

#ifdef _WIN32
static FARPROC sym(HMODULE m, const char* n) {
    FARPROC p = GetProcAddress(m, n);
    if (!p) {
        // Some builds decorate stdcall exports; can try common decorations as a fallback:
        // e.g., "_bcp_initA@20"
        // You can generate decorated names if needed, but "bcp_initA" works.
    }
    return p;
}
#else
static void* sym(void* m, const char* n) {
    return dlsym(m, n);
}
#endif

template <class TMod, class TFn>
static bool fill_sym(TMod mod, const char* a, const char* b, TFn& out) {
#ifdef _WIN32
    out = reinterpret_cast<TFn>(sym(mod, a));
    if (!out && b) out = reinterpret_cast<TFn>(sym(mod, b));
#else
    out = reinterpret_cast<TFn>(sym(mod, a));
    if (!out && b) out = reinterpret_cast<TFn>(sym(mod, b));
#endif
    return out != nullptr;
}

bool BcpLoadFromDriver(HDBC hdbc, BcpProcs& out) {
    out = BcpProcs{}; // reset

    char drv[256] = {0};
    if (!get_driver_name(hdbc, drv, sizeof(drv)))
        return false;

#ifdef _WIN32
    HMODULE mod = GetModuleHandleA(drv);    // Driver name is typically "msodbcsql17.dll" or "msodbcsql18.dll"
    if (!mod) mod = LoadLibraryA(drv);      // Try again in case the driver name is not a full path
    if (!mod) return false;

    if (!fill_sym(mod, "bcp_initA",   "bcp_init",    out.bcp_initA))  return false;
    if (!fill_sym(mod, "bcp_bind",    nullptr,       out.bcp_bind))   return false;
    if (!fill_sym(mod, "bcp_collen",  nullptr,       out.bcp_collen)) return false;
    if (!fill_sym(mod, "bcp_colptr",  nullptr,       out.bcp_colptr)) return false;
    if (!fill_sym(mod, "bcp_sendrow", nullptr,       out.bcp_sendrow))return false;
    fill_sym(mod,     "bcp_batch",    nullptr,       out.bcp_batch);  // optional
    if (!fill_sym(mod,"bcp_done",     nullptr,       out.bcp_done))   return false;
    fill_sym(mod,     "bcp_control",  nullptr,       out.bcp_control);// optional
#else
    // Linux/macOS: driver is typically "libmsodbcsql-18.X.so"
    void* mod = dlopen(drv, RTLD_NOW|RTLD_LOCAL);
    if (!mod) return false;

    // On non-Windows the wide variants may be the canonical ones; you can also probe both.

    if (!fill_sym(mod, "bcp_init",    "bcp_initA",   out.bcp_initA))  return false;
    if (!fill_sym(mod, "bcp_bind",    nullptr,       out.bcp_bind))   return false;
    if (!fill_sym(mod, "bcp_collen",  nullptr,       out.bcp_collen)) return false;
    if (!fill_sym(mod, "bcp_colptr",  nullptr,       out.bcp_colptr)) return false;
    if (!fill_sym(mod, "bcp_sendrow", nullptr,       out.bcp_sendrow))return false;
    fill_sym(mod,     "bcp_batch",    nullptr,       out.bcp_batch);
    if (!fill_sym(mod,"bcp_done",     nullptr,       out.bcp_done))   return false;
    fill_sym(mod,     "bcp_control",  nullptr,       out.bcp_control);
#endif

    // Require the core set:
    out.loaded = true;
    return true;
}

/*=======================================================================================*/
// Connection methods for BCP support
/*=======================================================================================*/

static const char* BCPCTX_CAPSULE = "pyodbc.BCPContext";

// ---- tiny helpers ---------------------------------------
static int is_space(char c) {
    return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f';
}
static char up(char c) { return (c>='a' && c<='z') ? (char)(c - 'a' + 'A') : c; }

// Skip whitespace and SQL-style comments
static const char* skip_ws_and_comments(const char* p) {
    for (;;) {
        // whitespace
        while (is_space(*p)) ++p;

        // line comment: --
        if (p[0]=='-' && p[1]=='-') {
            p += 2;
            while (*p && *p!='\n' && *p!='\r') ++p;
            continue;
        }
        // block comment: /* ... */
        if (p[0]=='/' && p[1]=='*') {
            p += 2;
            while (*p) {
                if (p[0]=='*' && p[1]=='/') { p += 2; break; }
                ++p;
            }
            continue;
        }
        return p;
    }
}

// Case-insensitive match of a keyword; advances *pp on success
static int match_kw(const char** pp, const char* kw) {
    const char* p = *pp;
    const char* k = kw;
    while (*k) {
        if (up(*p) != up(*k)) return 0;
        ++p; ++k;
    }
    // ensure next char isn’t a letter/underscore continuing an identifier
    char c = *p;
    if ((c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_') return 0;
    *pp = p;
    return 1;
}

// Parse bracketed identifier: starts at '[', supports ]] escape
static int parse_bracket_ident(const char** pp, char* buf, int cap) {
    const char* p = *pp; // p points to '['
    ++p; // skip '['
    int n = 0;
    while (*p) {
        if (*p == ']') {
            if (p[1] == ']') { // escaped ] -> add one ]
                if (n+1 >= cap) return -1;
                buf[n++] = ']'; p += 2; continue;
            } else {
                ++p; // end
                *pp = p;
                if (n >= cap) return -1;
                buf[n] = '\0';
                return n;
            }
        }
        if (n+1 >= cap) return -1;
        buf[n++] = *p++;
    }
    return -1; // unterminated
}

// Parse quoted identifier: starts at '"', supports "" escape
static int parse_quoted_ident(const char** pp, char* buf, int cap) {
    const char* p = *pp; // points to '"'
    ++p;
    int n = 0;
    while (*p) {
        if (*p == '"') {
            if (p[1] == '"') { // escaped "
                if (n+1 >= cap) return -1;
                buf[n++] = '"'; p += 2; continue;
            } else {
                ++p; // end
                *pp = p;
                if (n >= cap) return -1;
                buf[n] = '\0';
                return n;
            }
        }
        if (n+1 >= cap) return -1;
        buf[n++] = *p++;
    }
    return -1; // unterminated
}

// Parse bare identifier (letters, digits, _, $, #)
static int is_ident_start(char c) {
    return (c>='A'&&c<='Z') || (c>='a'&&c<='z') || c=='_' || c=='#' || c=='$';
}

// Check if the character is a valid part of the identifier 
static int is_ident_part(char c) {
    return is_ident_start(c) || (c>='0'&&c<='9');
}

// Parses a bare identifier from the input string into the buffer.
static int parse_bare_ident(const char** pp, char* buf, int cap) {
    const char* p = *pp;
    if (!is_ident_start(*p)) return -1;
    int n = 0;
    while (is_ident_part(*p)) {
        if (n+1 >= cap) return -1;
        buf[n++] = *p++;
    }
    *pp = p;
    if (n >= cap) return -1;
    buf[n] = '\0';
    return n;
}

// Parse a possibly quoted/bracketed identifier into buf
static int parse_identifier(const char** pp, char* buf, int cap) {
    const char* p = *pp;
    if (*p == '[') {
        int n = parse_bracket_ident(&p, buf, cap);
        if (n < 0) return -1;
        *pp = p; return n;
    } else if (*p == '"') {
        int n = parse_quoted_ident(&p, buf, cap);
        if (n < 0) return -1;
        *pp = p; return n;
    } else {
        int n = parse_bare_ident(&p, buf, cap);
        if (n < 0) return -1;
        *pp = p; return n;
    }
}

// Skip a TOP ( ... ) [PERCENT] clause if present
static void skip_top_clause(const char** pp) {
    const char* p = *pp;
    const char* save = p;
    if (!match_kw(&p, "TOP")) return;   // not there
    p = skip_ws_and_comments(p);
    if (*p != '(') { *pp = save; return; }
    // skip balanced parens depth 1
    int depth = 0;
    do {
        if (*p == '(') ++depth;
        else if (*p == ')') { --depth; if (depth==0) { ++p; break; } }
        if (*p == '\0') { *pp = save; return; }
        ++p;
    } while (depth > 0);
    p = skip_ws_and_comments(p);
    // optional PERCENT
    if (match_kw(&p, "PERCENT")) { /* ok */ }
    *pp = p;
}

// Returns 1 on success and writes table name into out_name (NUL-terminated),
// 0 on failure (out_name is left untouched).
int parse_insert_table(const char* sql, char* out_name, int out_cap)
{
    const char* p = sql;
    char part1[256];
    char part2[256];

    p = skip_ws_and_comments(p);

    // INSERT
    if (!match_kw(&p, "INSERT")) return 0;

    // optional stuff between INSERT and INTO (e.g. TOP (...) PERCENT)
    p = skip_ws_and_comments(p);
    skip_top_clause(&p);
    p = skip_ws_and_comments(p);

    // INTO
    if (!match_kw(&p, "INTO")) return 0;

    // schema/table
    p = skip_ws_and_comments(p);

    // first identifier
    if (parse_identifier(&p, part1, (int)sizeof(part1)) < 0) return 0;
    p = skip_ws_and_comments(p);

    // optional . second identifier
    char* table = part1;
    if (*p == '.') {
        ++p;
        p = skip_ws_and_comments(p);
        if (parse_identifier(&p, part2, (int)sizeof(part2)) < 0) return 0;
        table = part2; // last part is the table name
    }

    // (Optional) you can verify next non-space isn’t an obvious breaker,
    // but we allow WITH(...), (col list), or DEFAULT VALUES — so no hard check here.

    // Copy to output
    // compute length
    int n = 0; while (table[n] != '\0') ++n;
    if (n <= 0 || out_cap <= 0 || n >= out_cap) return 0;
    for (int i = 0; i < n; ++i) out_name[i] = table[i];
    out_name[n] = '\0';
    return 1;
}

// Dynamicaly loads BCP specific dependencies 
bool ensure_bcp_loaded(Connection* self) {
    if (self->bcp && self->bcp->loaded) return true;
    if (!self->hdbc) return false;
    if (!self->bcp) {
        self->bcp = (BcpProcs*)PyMem_Calloc(1, sizeof(BcpProcs));
        if (!self->bcp) { PyErr_NoMemory(); return false; }
    }
    return BcpLoadFromDriver(self->hdbc, *self->bcp);
}

// Frees the BCP context capsule 
void BcpCtx_FreeCapsule(PyObject* cap) 
{
    BcpCtx* ctx = (BcpCtx*)PyCapsule_GetPointer(cap, BCPCTX_CAPSULE);
    if (!ctx) return;
    if (ctx->cols) {
        for (int i = 0; i < ctx->ncols; ++i) {
            if (ctx->cols[i].scratch) PyMem_Free(ctx->cols[i].scratch);
        }
        PyMem_Free(ctx->cols);
    }
    PyMem_Free(ctx);
}

// Update the driver’s pointer to this column’s data buffer
int _bcp_set_colptr(BcpCtx* ctx, BcpCol* c)
{
    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = ctx->conn->bcp->bcp_colptr(ctx->conn->hdbc, (LPCBYTE)c->scratch, c->ordinal);
    Py_END_ALLOW_THREADS
    return (rc != FAIL);
}

// Free context buffer and clear pointer 
void _bcp_ctx_free(BcpCtx* ctx) {
    if (!ctx) return;
    if (ctx->cols) {
        for (int i = 0; i < ctx->ncols; ++i)
            if (ctx->cols[i].scratch) PyMem_Free(ctx->cols[i].scratch);
        PyMem_Free(ctx->cols);
    }
    PyMem_Free(ctx);
}

// Binds the buffer for the pased column 
int _bcp_rebind_current(BcpCtx* ctx, BcpCol* c)
{
    // When we grow a varlen buffer, tell the driver the new max length.
    const DBINT cbIndicator = 0;
    const DBINT cbData  = c->isVarLen ? 0 : (DBINT)c->fixedSize;

    SQLRETURN rc;
    Py_BEGIN_ALLOW_THREADS
    rc = ctx->conn->bcp->bcp_bind(ctx->conn->hdbc, (LPCBYTE)c->scratch, cbIndicator, cbData, NULL, 0, c->hostType, c->ordinal);
    Py_END_ALLOW_THREADS

    return (rc == SUCCEED);
}

// Binds buffers for all defined column types 
int _bcp_bind_all(BcpCtx* ctx)
{
    for (int i = 0; i < ctx->ncols; ++i)
    {
        BcpCol* c = &ctx->cols[i];

        // Ensure scratch exists and has at least 1 byte.
        if (!c->scratch || c->scratchCap == 0) 
        {
            PyErr_SetString(PyExc_RuntimeError, "Internal error: BCP column scratch buffer not allocated.");
            return 0;
        }

        // We bind a single, persistent host buffer per column.
        // For fixed types it's the exact size; for varlen we set isVarlen = 0 and set length per row via bcp_collen.
        const DBINT cbIndicator = 0;  // we keep the cbIndicator 0, this works for both static and var lengths 
        const DBINT cbData = c->isVarLen ? 0 : (DBINT)c->fixedSize;

        RETCODE rc = ctx->conn->bcp->bcp_bind(ctx->conn->hdbc,
                              (LPCBYTE)c->scratch,  // driver reads from here on each sendrow
                              cbIndicator,
                              cbData,               // 0 for varlen; sizeof(T) for fixed
                              /*pTerm*/ NULL,
                              /*cbTerm*/ 0,
                              /*eDataType*/ c->hostType,
                              /*server col*/ c->ordinal);
        if (rc != SUCCEED)
            return 0;
    }
    return 1;

}

// Convert one Python cell into a column scratch buffer + set length via bcp_collen.
// NULL -> SQL_NULL_DATA via bcp_collen for both fixed & varlen.
int _bcp_fill_cell(BcpCtx* ctx, PyObject* cell, BcpCol* c)
{
    // NULL for this column on this row
    if (cell == Py_None)
    {
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, SQL_NULL_DATA, c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }

    switch (c->hostType)
    {
    case SQLBIT: {
        int truthy = PyObject_IsTrue(cell);
        if (truthy < 0) return 0;       // error converting
        unsigned char b = truthy ? 1 : 0;
        memcpy(c->scratch, &b, 1);
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, 1, c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLINT2: {
        long v = PyLong_AsLong(cell);
        if (PyErr_Occurred()) return 0;
        if (v < SHRT_MIN || v > SHRT_MAX) {
            PyErr_SetString(PyExc_OverflowError, "SMALLINT out of range");
            return 0;
        }
        short s = (short)v;
        memcpy(c->scratch, &s, sizeof(short));
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(short), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLINT4:
    {
        // Accept ints; range-check
        long long lv = PyLong_AsLongLong(cell);
        if (PyErr_Occurred()) return 0;
        DBINT v = (DBINT)lv;  // optional: check lv in [-2147483648, 2147483647]
        memcpy(c->scratch, &v, sizeof(DBINT));

        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(DBINT), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLINT8: {
        long long v = PyLong_AsLongLong(cell);
        if (PyErr_Occurred()) return 0;
        memcpy(c->scratch, &v, sizeof(long long));
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(long long), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLFLT8:
    {
        // PyFloat_AsDouble also accepts ints (no need for PyFloat_Check here)
        double d = PyFloat_AsDouble(cell);
        if (PyErr_Occurred()) return 0;
        memcpy(c->scratch, &d, sizeof(double));

        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(double), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLFLT4: {
        double dv = PyFloat_AsDouble(cell);
        if (PyErr_Occurred()) return 0;
        float fv = (float)dv;
        memcpy(c->scratch, &fv, sizeof(float));
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(float), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLBINARY: {
        const char* p = NULL;
        Py_ssize_t n = 0;
        PyObject* bytes_obj = NULL;

        if (PyBytes_Check(cell)) {
            bytes_obj = cell; Py_INCREF(bytes_obj);
            p = PyBytes_AS_STRING(bytes_obj);
            n = PyBytes_GET_SIZE(bytes_obj);
        } else if (PyByteArray_Check(cell)) {
            p = PyByteArray_AsString(cell);
            n = PyByteArray_Size(cell);
        } else {
            PyErr_SetString(PyExc_TypeError, "Expected bytes/bytearray for VARBINARY/BINARY");
            return 0;
        }

        DBINT bytes = (DBINT)n;
        if (bytes > c->scratchCap) {
            unsigned char* np = (unsigned char*)PyMem_Realloc(c->scratch, (size_t)bytes);
            if (!np) { Py_XDECREF(bytes_obj); PyErr_NoMemory(); return 0; }
            c->scratch = np; c->scratchCap = bytes;
            if (!_bcp_rebind_current(ctx, c)) { Py_XDECREF(bytes_obj); return 0; }
        }
        if (bytes > 0) memcpy(c->scratch, p, (size_t)bytes);

        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, bytes, c->ordinal);
        Py_END_ALLOW_THREADS

        Py_XDECREF(bytes_obj);
        return (rc != FAIL);
    }
    case SQLUNIQUEID: {
        unsigned char buf[16];
        int ok = 0;
        if (PyObject_HasAttrString(cell, "bytes_le")) {
            PyObject* le = PyObject_GetAttrString(cell, "bytes_le");
            if (le) {
                if (PyBytes_Check(le) && PyBytes_GET_SIZE(le) == 16) {
                    memcpy(buf, PyBytes_AS_STRING(le), 16);
                    ok = 1;
                }
                Py_DECREF(le);
            }
        } else if (PyBytes_Check(cell) && PyBytes_GET_SIZE(cell) == 16) {
            memcpy(buf, PyBytes_AS_STRING(cell), 16);
            ok = 1;
        }
        if (!ok) {
            PyErr_SetString(PyExc_TypeError, "GUID requires uuid.UUID or 16-byte bytes");
            return 0;
        }
        memcpy(c->scratch, buf, 16);
        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, 16, c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLCHARACTER:
    {
        const char* p = NULL;
        Py_ssize_t n = 0;

        if (PyUnicode_Check(cell)) {
            p = PyUnicode_AsUTF8AndSize(cell, &n);   // <-- NO allocation
            if (!p) return 0;
        } else if (PyBytes_Check(cell)) {
            p = PyBytes_AsString(cell);
            if (!p) return 0;
            n = PyBytes_GET_SIZE(cell);
        } else if (PyByteArray_Check(cell)) {
            p = PyByteArray_AsString(cell);
            n = PyByteArray_Size(cell);
        } else {
            PyErr_SetString(PyExc_TypeError, "Expected str/bytes/bytearray for SQLCHARACTER");
            return 0;
        }

        DBINT bytes = (DBINT)n;

        if (bytes > c->scratchCap) {
            unsigned char* np = (unsigned char*)PyMem_Realloc(c->scratch, (size_t)bytes);
            if (!np) { PyErr_NoMemory(); return 0; }
            c->scratch = np;
            c->scratchCap = bytes;
            if (!_bcp_rebind_current(ctx, c)) return 0;
        }

        if (bytes > 0)
            memcpy(c->scratch, p, (size_t)bytes);

        SQLRETURN rc;
        Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, bytes, c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    case SQLTIMEN: {
        PyObject* hh = PyObject_GetAttrString(cell, "hour");
        PyObject* mm = PyObject_GetAttrString(cell, "minute");
        PyObject* ss = PyObject_GetAttrString(cell, "second");
        PyObject* us = PyObject_GetAttrString(cell, "microsecond");
        if (!hh || !mm || !ss || !us) { Py_XDECREF(hh); Py_XDECREF(mm); Py_XDECREF(ss); Py_XDECREF(us);
            PyErr_SetString(PyExc_TypeError, "TIME expects time/datetime"); return 0; }
        TIME_STRUCT ts;
        ts.hour   = (SQLUSMALLINT)PyLong_AsUnsignedLong(hh);
        ts.minute = (SQLUSMALLINT)PyLong_AsUnsignedLong(mm);
        ts.second = (SQLUSMALLINT)PyLong_AsUnsignedLong(ss);
        Py_DECREF(hh); Py_DECREF(mm); Py_DECREF(ss);
        if (PyErr_Occurred()) { Py_XDECREF(us); return 0; }

        // Store fractional seconds in TIMESTAMP_STRUCT only; TIME_STRUCT has no fraction field.
        // For TIME(p) precision, the driver will handle scale; to keep sub-second precision,
        // prefer TIMESTAMP_STRUCT + SQLTIMESTAMP (or send text). For pure TIME, send "HH:MM:SS[.fff]" as text (Option A),
        // or accept second-only here:
        Py_XDECREF(us);

        memcpy(c->scratch, &ts, sizeof(ts));
        SQLRETURN rc; Py_BEGIN_ALLOW_THREADS
        rc = ctx->conn->bcp->bcp_collen(ctx->conn->hdbc, (DBINT)sizeof(TIME_STRUCT), c->ordinal);
        Py_END_ALLOW_THREADS
        return (rc != FAIL);
    }
    default:
        PyErr_SetString(PyExc_TypeError, "Unsupported host type in types[]");
        return 0;
    }
}

// helpers (top of file)
void write_le(unsigned char* dst, unsigned long long v, int len) {
    for (int i = 0; i < len; ++i) dst[i] = (unsigned char)((v >> (8*i)) & 0xFF);
}

// time ticks for TIME(7): 10^-7s since midnight
unsigned long long time_to_ticks7(int hh, int mm, int ss, int micro) {
    unsigned long long sec = (unsigned long long)hh*3600ULL + (unsigned long long)mm*60ULL + (unsigned long long)ss;
    return sec * 10000000ULL + (unsigned long long)micro * 10ULL; // 1 micro = 10 * 1e-7
}

// days since 0001-01-01; 0001-01-01 = 0
unsigned int days_since_0001_01_01(int y, int m, int d)
{
    // Howard Hinnant’s days-from-civil (adapted), shifted so 0001-01-01 = 0
    y -= m <= 2;
    const int era = (y >= 0 ? y : y-399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);           // [0, 399]
    const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1; // [0, 365]
    const unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;     // [0, 146096]
    // days since 0000-03-01; convert to 0001-01-01 base:
    // 0001-01-01 is 306 days after 0000-03-01.
    const int days = era*146097 + (int)doe - 306;
    return (unsigned int)days; // 0 for 0001-01-01
}
