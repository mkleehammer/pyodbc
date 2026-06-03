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

/**
 * Returns the major version in SQLGetInfo(SQL_DRIVER_VER), "##.##.####", times 100 plus the
 * minor version, or 0 if unknown.
 */
static int get_driver_version(HDBC hdbc)
{
    char ver[32] = {0};
    SQLSMALLINT outlen = 0;
    if (SQLGetInfo(hdbc, SQL_DRIVER_VER, (SQLPOINTER)ver, sizeof(ver), &outlen) != SQL_SUCCESS)
        return 0;
    ver[sizeof(ver) - 1] = '\0';
    const char* minor = strchr(ver, '.');
    return atoi(ver) * 100 + (minor ? atoi(minor + 1) : 0);
}

/**
 * Resolves the BCP entry points from the driver behind hdbc.  Only the Microsoft ODBC Driver
 * 13 or later is used.  FreeTDS exports the same functions but differs in its conversions and
 * diagnostics, which the fast path's guarantees rest on.  SQL Server Native Client and Driver
 * 11 leave a failed bcp_batch or bcp_done without diagnostics when the Windows driver manager
 * pools connections, as pyodbc has it do by default, so their errors could not be raised.
 */
static bool load_driver_procs(HDBC hdbc, int driverVersion, BcpProcs& out)
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
    if (driverVersion < 1300) {
        BCPLOAD_DBG("load: driver version %d is older than 13\n", driverVersion / 100);
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
    state->driverVersion = get_driver_version(cnxn->hdbc);
    state->loaded = load_driver_procs(cnxn->hdbc, state->driverVersion, state->procs);
    cnxn->bcp = state;
    return state->loaded;
}

bool BcpIsEnabled(HDBC hdbc)
{
    SQLUINTEGER value = 0;
    SQLRETURN rc = SQLGetConnectAttr(hdbc, SQL_COPT_SS_BCP, &value, SQL_IS_UINTEGER, 0);
    return SQL_SUCCEEDED(rc) && value == SQL_BCP_ON;
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
// INSERT statement parsing
/*=======================================================================================*/

static int is_space(char c)
{
    return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f' || c=='\v';
}

static char up(char c) { return (c>='a' && c<='z') ? (char)(c - 'a' + 'A') : c; }

/**
 * Skips whitespace, line comments and block comments.  T-SQL block comments nest; an
 * inner comment ends this scan early, which leaves text the parser then rejects, so nesting can
 * only make the parser fall back, never accept something T-SQL reads differently.
 */
static const char* skip_ws_and_comments(const char* p)
{
    for (;;) {
        while (is_space(*p))
            ++p;
        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p && *p != '\n' && *p != '\r')
                ++p;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/'))
                ++p;
            if (*p)
                p += 2;
            continue;
        }
        return p;
    }
}

/** Case-insensitively matches a whole keyword at *pp, advancing past it on success. */
static int match_kw(const char** pp, const char* kw)
{
    const char* p = *pp;
    for (const char* k = kw; *k; ++k, ++p)
        if (up(*p) != *k)
            return 0;
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '@' || c == '#' || c == '$' || (unsigned char)c >= 0x80)
        return 0;
    *pp = p;
    return 1;
}

/** Parses an identifier delimited by `open`/`close`, where a doubled `close` is a literal one. */
static int parse_delimited_ident(const char** pp, char close, char* buf, int cap)
{
    const char* p = *pp + 1;
    int n = 0;
    while (*p) {
        if (*p == close) {
            if (p[1] != close) {
                *pp = p + 1;
                buf[n] = '\0';
                return n;
            }
            ++p;
        }
        if (n + 1 >= cap)
            return -1;
        buf[n++] = *p++;
    }
    return -1;
}

static int is_ident_start(char c)
{
    return (c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_' || c=='#' || c=='@';
}

static int is_ident_part(char c)
{
    return is_ident_start(c) || (c>='0' && c<='9') || c=='$';
}

/** The T-SQL reserved keywords, sorted, which cannot be used as undelimited identifiers. */
static const char* const RESERVED_WORDS[] = {
    "ADD", "ALL", "ALTER", "AND", "ANY", "AS", "ASC", "AUTHORIZATION", "BACKUP", "BEGIN",
    "BETWEEN", "BREAK", "BROWSE", "BULK", "BY", "CASCADE", "CASE", "CHECK", "CHECKPOINT",
    "CLOSE", "CLUSTERED", "COALESCE", "COLLATE", "COLUMN", "COMMIT", "COMPUTE", "CONSTRAINT",
    "CONTAINS", "CONTAINSTABLE", "CONTINUE", "CONVERT", "CREATE", "CROSS", "CURRENT",
    "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP", "CURRENT_USER", "CURSOR", "DATABASE",
    "DBCC", "DEALLOCATE", "DECLARE", "DEFAULT", "DELETE", "DENY", "DESC", "DISK", "DISTINCT",
    "DISTRIBUTED", "DOUBLE", "DROP", "DUMP", "ELSE", "END", "ERRLVL", "ESCAPE", "EXCEPT", "EXEC",
    "EXECUTE", "EXISTS", "EXIT", "EXTERNAL", "FETCH", "FILE", "FILLFACTOR", "FOR", "FOREIGN",
    "FREETEXT", "FREETEXTTABLE", "FROM", "FULL", "FUNCTION", "GOTO", "GRANT", "GROUP", "HAVING",
    "HOLDLOCK", "IDENTITY", "IDENTITYCOL", "IDENTITY_INSERT", "IF", "IN", "INDEX", "INNER",
    "INSERT", "INTERSECT", "INTO", "IS", "JOIN", "KEY", "KILL", "LEFT", "LIKE", "LINENO", "LOAD",
    "MERGE", "NATIONAL", "NOCHECK", "NONCLUSTERED", "NOT", "NULL", "NULLIF", "OF", "OFF",
    "OFFSETS", "ON", "OPEN", "OPENDATASOURCE", "OPENQUERY", "OPENROWSET", "OPENXML", "OPTION",
    "OR", "ORDER", "OUTER", "OVER", "PERCENT", "PIVOT", "PLAN", "PRECISION", "PRIMARY", "PRINT",
    "PROC", "PROCEDURE", "PUBLIC", "RAISERROR", "READ", "READTEXT", "RECONFIGURE", "REFERENCES",
    "REPLICATION", "RESTORE", "RESTRICT", "RETURN", "REVERT", "REVOKE", "RIGHT", "ROLLBACK",
    "ROWCOUNT", "ROWGUIDCOL", "RULE", "SAVE", "SCHEMA", "SECURITYAUDIT", "SELECT",
    "SEMANTICKEYPHRASETABLE", "SEMANTICSIMILARITYDETAILSTABLE", "SEMANTICSIMILARITYTABLE",
    "SESSION_USER", "SET", "SETUSER", "SHUTDOWN", "SOME", "STATISTICS", "SYSTEM_USER", "TABLE",
    "TABLESAMPLE", "TEXTSIZE", "THEN", "TO", "TOP", "TRAN", "TRANSACTION", "TRIGGER", "TRUNCATE",
    "TRY_CONVERT", "TSEQUAL", "UNION", "UNIQUE", "UNPIVOT", "UPDATE", "UPDATETEXT", "USE", "USER",
    "VALUES", "VARYING", "VIEW", "WAITFOR", "WHEN", "WHERE", "WHILE", "WITH", "WITHIN",
    "WRITETEXT",
};

static int compare_reserved(const void* key, const void* item)
{
    return strcmp((const char*)key, *(const char* const*)item);
}

static bool is_reserved_word(const char* ident)
{
    char upper[40];
    size_t n = strlen(ident);
    if (n >= sizeof(upper))
        return false;
    for (size_t i = 0; i <= n; ++i)
        upper[i] = up(ident[i]);
    return bsearch(upper, RESERVED_WORDS, sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]),
                   sizeof(RESERVED_WORDS[0]), compare_reserved) != 0;
}

/**
 * Parses a regular or bracket-delimited identifier.  Regular identifiers are limited to ASCII
 * and may not be reserved words; quote-delimited ones are not accepted at all, since their
 * meaning depends on the connection's QUOTED_IDENTIFIER setting.  Statements using any of
 * these are left to the plain path.
 */
static int parse_identifier(const char** pp, char* buf, int cap)
{
    const char* p = *pp;
    if (*p == '[')
        return parse_delimited_ident(pp, ']', buf, cap);
    if (!is_ident_start(*p))
        return -1;
    int n = 0;
    while (is_ident_part(*p)) {
        if (n + 1 >= cap)
            return -1;
        buf[n++] = *p++;
    }
    if ((unsigned char)*p >= 0x80)
        return -1;
    buf[n] = '\0';
    if (is_reserved_word(buf))
        return -1;
    *pp = p;
    return n;
}

/** Parses `( ? [, ?]... )`, returning the number of markers or 0 if it is anything else. */
static int parse_markers(const char** pp)
{
    const char* p = skip_ws_and_comments(*pp);
    if (*p != '(')
        return 0;
    ++p;
    int n = 0;
    for (;;) {
        p = skip_ws_and_comments(p);
        if (*p != '?')
            return 0;
        ++p;
        ++n;
        p = skip_ws_and_comments(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ')') { ++p; break; }
        return 0;
    }
    *pp = p;
    return n;
}

static int names_append(BcpInsert* ins, int* cap, const char* s)
{
    if (ins->nnames == *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        char** np = (char**)PyMem_Realloc(ins->names, (size_t)ncap * sizeof(char*));
        if (!np)
            return 0;
        ins->names = np;
        *cap = ncap;
    }
    size_t len = strlen(s);
    char* copy = (char*)PyMem_Malloc(len + 1);
    if (!copy)
        return 0;
    memcpy(copy, s, len + 1);
    ins->names[ins->nnames++] = copy;
    return 1;
}

void BcpFreeInsert(BcpInsert* ins)
{
    for (int i = 0; i < ins->nnames; ++i)
        PyMem_Free(ins->names[i]);
    PyMem_Free(ins->names);
    ins->names = 0;
    ins->nnames = 0;
}

int BcpParseInsert(const char* sql, BcpInsert* out)
{
    memset(out, 0, sizeof(*out));

    const char* p = skip_ws_and_comments(sql);
    if (!match_kw(&p, "INSERT"))
        return 0;
    p = skip_ws_and_comments(p);
    if (match_kw(&p, "TOP"))
        return 0;
    if (match_kw(&p, "INTO"))
        p = skip_ws_and_comments(p);

    char parts[3][sizeof(out->table)];
    int nparts = 0;
    for (;;) {
        if (nparts == 3 || parse_identifier(&p, parts[nparts], (int)sizeof(parts[0])) <= 0)
            return 0;
        ++nparts;
        p = skip_ws_and_comments(p);
        if (*p != '.')
            break;
        ++p;
        p = skip_ws_and_comments(p);
        while (nparts < 3 && *p == '.') {
            parts[nparts++][0] = '\0';
            p = skip_ws_and_comments(p + 1);
        }
    }
    strcpy(out->table, parts[nparts - 1]);
    if (nparts >= 2)
        strcpy(out->schema, parts[nparts - 2]);
    if (nparts == 3)
        strcpy(out->catalog, parts[0]);
    if (out->table[0] == '@')
        return 0;
    if (out->table[0] == '#' && !out->catalog[0])
        strcpy(out->catalog, "tempdb");

    int cap = 0;
    if (*p == '(') {
        ++p;
        for (;;) {
            p = skip_ws_and_comments(p);
            char ident[sizeof(out->table)];
            if (parse_identifier(&p, ident, (int)sizeof(ident)) <= 0) {
                BcpFreeInsert(out);
                return 0;
            }
            if (!names_append(out, &cap, ident)) {
                BcpFreeInsert(out);
                PyErr_NoMemory();
                return -1;
            }
            p = skip_ws_and_comments(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ')') { ++p; break; }
            BcpFreeInsert(out);
            return 0;
        }
        p = skip_ws_and_comments(p);
    }

    if (!match_kw(&p, "VALUES") || (out->nmarkers = parse_markers(&p)) == 0) {
        BcpFreeInsert(out);
        return 0;
    }
    p = skip_ws_and_comments(p);
    if (*p == ';')
        p = skip_ws_and_comments(p + 1);
    if (*p != '\0') {
        BcpFreeInsert(out);
        return 0;
    }
    return 1;
}

SQLWCHAR* BcpUtf8ToSqlWChar(const char* s)
{
    Object text(PyUnicode_DecodeUTF8(s, (Py_ssize_t)strlen(s), "strict"));
    if (!text.IsValid())
        return 0;
    Object encoded(PyUnicode_AsEncodedString(text, "utf-16-le", "strict"));
    if (!encoded.IsValid())
        return 0;
    Py_ssize_t nbytes = PyBytes_GET_SIZE(encoded.Get());
    SQLWCHAR* w = (SQLWCHAR*)PyMem_Malloc((size_t)nbytes + sizeof(SQLWCHAR));
    if (!w) {
        PyErr_NoMemory();
        return 0;
    }
    memcpy(w, PyBytes_AS_STRING(encoded.Get()), (size_t)nbytes);
    w[nbytes / (Py_ssize_t)sizeof(SQLWCHAR)] = 0;
    return w;
}

/** Appends `[ident]`, doubling any `]`, to a NUL-terminated buffer of capacity cap. */
static bool append_quoted(char* dst, size_t cap, const char* ident)
{
    size_t n = strlen(dst);
    if (n + 1 >= cap)
        return false;
    dst[n++] = '[';
    for (const char* s = ident; *s; ++s) {
        if (n + 3 >= cap)
            return false;
        dst[n++] = *s;
        if (*s == ']')
            dst[n++] = ']';
    }
    if (n + 2 > cap)
        return false;
    dst[n++] = ']';
    dst[n] = '\0';
    return true;
}

/** Builds the quoted table name as UTF-8, e.g. "[db].[dbo].[t]" or "[tempdb]..[#t]". */
static bool quoted_table_name_utf8(const BcpInsert& ins, char* buf, size_t cap)
{
    buf[0] = '\0';
    if (ins.catalog[0] && (!append_quoted(buf, cap, ins.catalog) || strlen(buf) + 2 > cap))
        return false;
    if (ins.catalog[0])
        strcat(buf, ".");
    if (ins.schema[0] && !append_quoted(buf, cap, ins.schema))
        return false;
    if ((ins.catalog[0] || ins.schema[0]) && strlen(buf) + 2 > cap)
        return false;
    if (ins.catalog[0] || ins.schema[0])
        strcat(buf, ".");
    return append_quoted(buf, cap, ins.table);
}

SQLWCHAR* BcpQuotedTableName(const BcpInsert& ins)
{
    char name[3 * (2 * sizeof(ins.table) + 3)];
    if (!quoted_table_name_utf8(ins, name, sizeof(name))) {
        PyErr_SetString(PyExc_ValueError, "table name too long for BCP");
        return 0;
    }
    return BcpUtf8ToSqlWChar(name);
}

/*=======================================================================================*/
// Destination table metadata
/*=======================================================================================*/

void BcpFreeTarget(BcpTarget* target)
{
    PyMem_Free(target->cols);
    target->cols = 0;
    target->ncols = 0;
}

static size_t sqlwchar_len(const SQLWCHAR* s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

/**
 * Runs the catalog query on its own statement handle and collects the rows into *out.
 *
 * @return 1 for a user table with at least one visible column, 0 otherwise, -1 on allocation
 *         failure (Python exception set).
 */
static int fetch_target_columns(HDBC hdbc, SQLWCHAR* sql, SQLWCHAR* database, SQLWCHAR* name, BcpTarget* out)
{
    HSTMT hstmt = SQL_NULL_HANDLE;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt)))
        return 0;

    int result = 0;
    int cap = 0;
    SQLLEN cbDatabase = SQL_NTS, cbName = SQL_NTS;
    char type[4] = {0};
    BcpTargetColumn col;
    SQLINTEGER systemType = 0, maxLength = 0, scale = 0, identity = 0, computed = 0, defaultId = 0;
    SQLINTEGER hasTrigger = 0, ignoreCase = 0, codePage = 0;
    SQLLEN cbType = 0, cbColName = 0, cbSystemType = 0, cbMaxLength = 0, cbScale = 0, cbIdentity = 0,
           cbComputed = 0, cbDefault = 0, cbTrigger = 0, cbIgnoreCase = 0, cbCodePage = 0;
    SQLSMALLINT resultCols = 0;
    SQLRETURN rc;

    SQLULEN databaseChars = (SQLULEN)sqlwchar_len(database);
    SQLULEN nameChars = (SQLULEN)sqlwchar_len(name);
    if (!SQL_SUCCEEDED(SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
                                        databaseChars ? databaseChars : 1, 0, database, 0, &cbDatabase)) ||
        !SQL_SUCCEEDED(SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
                                        nameChars ? nameChars : 1, 0, name, 0, &cbName)))
        goto done;
    Py_BEGIN_ALLOW_THREADS
    rc = SQLExecDirectW(hstmt, sql, SQL_NTS);
    Py_END_ALLOW_THREADS
    while (SQL_SUCCEEDED(rc) && SQL_SUCCEEDED(SQLNumResultCols(hstmt, &resultCols)) && resultCols == 0) {
        Py_BEGIN_ALLOW_THREADS
        rc = SQLMoreResults(hstmt);
        Py_END_ALLOW_THREADS
    }
    if (!SQL_SUCCEEDED(rc))
        goto done;

    SQLBindCol(hstmt, 1, SQL_C_CHAR, type, sizeof(type), &cbType);
    SQLBindCol(hstmt, 2, SQL_C_WCHAR, col.name, sizeof(col.name), &cbColName);
    SQLBindCol(hstmt, 3, SQL_C_SLONG, &systemType, 0, &cbSystemType);
    SQLBindCol(hstmt, 4, SQL_C_SLONG, &maxLength, 0, &cbMaxLength);
    SQLBindCol(hstmt, 5, SQL_C_SLONG, &scale, 0, &cbScale);
    SQLBindCol(hstmt, 6, SQL_C_SLONG, &identity, 0, &cbIdentity);
    SQLBindCol(hstmt, 7, SQL_C_SLONG, &computed, 0, &cbComputed);
    SQLBindCol(hstmt, 8, SQL_C_SLONG, &defaultId, 0, &cbDefault);
    SQLBindCol(hstmt, 9, SQL_C_SLONG, &hasTrigger, 0, &cbTrigger);
    SQLBindCol(hstmt, 10, SQL_C_SLONG, &ignoreCase, 0, &cbIgnoreCase);
    SQLBindCol(hstmt, 11, SQL_C_SLONG, &codePage, 0, &cbCodePage);

    result = 1;
    for (;;) {
        Py_BEGIN_ALLOW_THREADS
        rc = SQLFetch(hstmt);
        Py_END_ALLOW_THREADS
        if (rc == SQL_NO_DATA)
            break;
        if (!SQL_SUCCEEDED(rc) || type[0] != 'U' || cbColName == SQL_NULL_DATA) {
            result = 0;
            break;
        }
        if (out->ncols == cap) {
            int ncap = cap ? cap * 2 : 16;
            BcpTargetColumn* np = (BcpTargetColumn*)PyMem_Realloc(out->cols, (size_t)ncap * sizeof(BcpTargetColumn));
            if (!np) {
                PyErr_NoMemory();
                result = -1;
                break;
            }
            out->cols = np;
            cap = ncap;
        }
        col.systemType = (int)systemType;
        col.maxLength = (int)maxLength;
        col.scale = (int)scale;
        col.codePage = cbCodePage == SQL_NULL_DATA ? 0 : (int)codePage;
        col.identity = identity != 0;
        col.computed = computed != 0;
        col.hasDefault = defaultId != 0;
        out->cols[out->ncols++] = col;
        out->hasInsertTrigger = hasTrigger != 0;
        out->caseInsensitive = cbIgnoreCase != SQL_NULL_DATA && ignoreCase != 0;
    }
    if (result == 1 && out->ncols == 0)
        result = 0;

done:
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    if (result != 1)
        BcpFreeTarget(out);
    return result;
}

int BcpLoadTarget(Connection* cnxn, const BcpInsert& ins, bool setSavepoint, BcpTarget* out)
{
    out->ncols = 0;
    out->cols = 0;
    out->hasInsertTrigger = false;
    out->caseInsensitive = false;

    char db[2 * sizeof(ins.catalog) + 4] = "";
    if (ins.catalog[0] && (!append_quoted(db, sizeof(db) - 1, ins.catalog)))
        return 0;
    if (ins.catalog[0])
        strcat(db, ".");

    char sql[4 * sizeof(db) + 900];
    snprintf(sql, sizeof(sql),
             "%sSELECT o.type, c.name, c.system_type_id, c.max_length, c.scale, c.is_identity, c.is_computed, "
             "c.default_object_id, "
             "CASE WHEN EXISTS (SELECT 1 FROM %ssys.triggers tr JOIN %ssys.trigger_events te ON te.object_id = tr.object_id "
             "WHERE tr.parent_id = o.object_id AND tr.is_disabled = 0 AND te.type = 1) THEN 1 ELSE 0 END, "
             "CONVERT(int, COLLATIONPROPERTY((SELECT d.collation_name FROM sys.databases d "
             "WHERE d.name = COALESCE(NULLIF(?, N''), DB_NAME())), 'ComparisonStyle')) & 1, "
             "CONVERT(int, COLLATIONPROPERTY(c.collation_name, 'CodePage')) "
             "FROM %ssys.objects o JOIN %ssys.columns c ON c.object_id = o.object_id "
             "WHERE o.object_id = OBJECT_ID(?) ORDER BY c.column_id",
             setSavepoint ? "SAVE TRANSACTION pyodbc_bcp; " : "", db, db, db, db);

    SQLWCHAR* wsql = BcpUtf8ToSqlWChar(sql);
    if (!wsql)
        return -1;
    SQLWCHAR* wdatabase = BcpUtf8ToSqlWChar(ins.catalog);
    if (!wdatabase) {
        PyMem_Free(wsql);
        return -1;
    }
    SQLWCHAR* wname = BcpQuotedTableName(ins);
    if (!wname) {
        PyMem_Free(wdatabase);
        PyMem_Free(wsql);
        if (PyErr_ExceptionMatches(PyExc_ValueError)) {
            PyErr_Clear();
            return 0;
        }
        return -1;
    }
    int result = fetch_target_columns(cnxn->hdbc, wsql, wdatabase, wname, out);
    PyMem_Free(wname);
    PyMem_Free(wdatabase);
    PyMem_Free(wsql);
    return result;
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

bool BcpPutWideStr(Connection* cnxn, BcpCol* c, PyObject* s)
{
    if (PyUnicode_KIND(s) == PyUnicode_1BYTE_KIND)
        return BcpPutWidened(cnxn, c, PyUnicode_1BYTE_DATA(s), PyUnicode_GET_LENGTH(s));
    Object utf16(PyUnicode_AsEncodedString(s, "utf-16-le", "strict"));
    if (!utf16.IsValid())
        return false;
    return BcpPutBytes(cnxn, c, PyBytes_AS_STRING(utf16.Get()), PyBytes_GET_SIZE(utf16.Get()));
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
