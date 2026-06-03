#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "connection.h"
#include "bcp_support.h"


// Diagnostic logging toggle, controlled by the PYODBC_BCP_DEBUG environment
// variable. Evaluated once and cached. When enabled, driver loading emits
// progress lines to stderr to help diagnose why BCP is unavailable.
static int bcp_debug_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        const char* v = getenv("PYODBC_BCP_DEBUG");
        cached = (v && v[0] && !(v[0] == '0' && v[1] == '\0')) ? 1 : 0;
    }
    return cached;
}

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

#define BCPLOAD_DBG(...) do { if (bcp_debug_enabled()) { fprintf(stderr, "[bcp] " __VA_ARGS__); fflush(stderr); } } while (0)

#ifdef __linux__
// SQLGetInfo(SQL_DRIVER_NAME) returns a bare versioned soname (e.g.
// "libmsodbcsql-18.5.so.1.1") that dlopen() can't resolve unless its directory
// is on the loader path. The driver manager has, however, already mapped the
// real library into this process, so its absolute path is available in
// /proc/self/maps. Find the mapping whose basename matches the soname (or, as a
// looser fallback, that looks like the msodbcsql driver) and return its path.
static bool find_driver_path_in_maps(const char* soname, char* out, size_t outcap) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[8192];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char* path = strchr(line, '/');
        if (!path) continue;
        size_t len = strlen(path);
        while (len && (path[len-1] == '\n' || path[len-1] == '\r')) path[--len] = '\0';
        const char* base = strrchr(path, '/');
        base = base ? base + 1 : path;
        // Prefer an exact soname match; otherwise accept a library whose basename
        // *starts with* "libmsodbcsql" (not merely contains it, to avoid matching
        // an unrelated mapping).
        if ((soname && soname[0] && strcmp(base, soname) == 0) ||
            strncmp(base, "libmsodbcsql", 12) == 0) {
            strncpy(out, path, outcap - 1);
            out[outcap - 1] = '\0';
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}
#endif

bool BcpLoadFromDriver(HDBC hdbc, BcpProcs& out) {
    out = BcpProcs{}; // reset

    char drv[256] = {0};
    if (!get_driver_name(hdbc, drv, sizeof(drv))) {
        BCPLOAD_DBG("load: SQLGetInfo(SQL_DRIVER_NAME) failed\n");
        return false;
    }
    BCPLOAD_DBG("load: driver name = '%s'\n", drv);

#ifdef _WIN32
    HMODULE mod = GetModuleHandleA(drv);    // Driver name is typically "msodbcsql17.dll" or "msodbcsql18.dll"
    if (!mod) mod = LoadLibraryA(drv);      // Try again in case the driver name is not a full path
    if (!mod) { BCPLOAD_DBG("load: could not get/load module '%s'\n", drv); return false; }

    if (!fill_sym(mod, "bcp_initA",   "bcp_init",    out.bcp_initA))  { BCPLOAD_DBG("load: missing bcp_initA\n");   return false; }
    if (!fill_sym(mod, "bcp_bind",    nullptr,       out.bcp_bind))   { BCPLOAD_DBG("load: missing bcp_bind\n");    return false; }
    if (!fill_sym(mod, "bcp_collen",  nullptr,       out.bcp_collen)) { BCPLOAD_DBG("load: missing bcp_collen\n");  return false; }
    if (!fill_sym(mod, "bcp_colptr",  nullptr,       out.bcp_colptr)) { BCPLOAD_DBG("load: missing bcp_colptr\n");  return false; }
    if (!fill_sym(mod, "bcp_sendrow", nullptr,       out.bcp_sendrow)){ BCPLOAD_DBG("load: missing bcp_sendrow\n"); return false; }
    fill_sym(mod,     "bcp_batch",    nullptr,       out.bcp_batch);  // optional
    if (!fill_sym(mod,"bcp_done",     nullptr,       out.bcp_done))   { BCPLOAD_DBG("load: missing bcp_done\n");    return false; }
    fill_sym(mod,     "bcp_control",  nullptr,       out.bcp_control);// optional
#else
    // Helper: load the BCP symbols from a given handle (handle may be
    // RTLD_DEFAULT, which is NULL on glibc, so we track success separately).
    auto load_syms = [&](void* m) -> bool {
        out = BcpProcs{};
        if (!fill_sym(m, "bcp_initA",   "bcp_init",  out.bcp_initA))  { BCPLOAD_DBG("load:   missing bcp_initA/bcp_init\n"); return false; }
        if (!fill_sym(m, "bcp_bind",    nullptr,     out.bcp_bind))   { BCPLOAD_DBG("load:   missing bcp_bind\n");    return false; }
        if (!fill_sym(m, "bcp_collen",  nullptr,     out.bcp_collen)) { BCPLOAD_DBG("load:   missing bcp_collen\n");  return false; }
        if (!fill_sym(m, "bcp_colptr",  nullptr,     out.bcp_colptr)) { BCPLOAD_DBG("load:   missing bcp_colptr\n");  return false; }
        if (!fill_sym(m, "bcp_sendrow", nullptr,     out.bcp_sendrow)){ BCPLOAD_DBG("load:   missing bcp_sendrow\n"); return false; }
        fill_sym(m,     "bcp_batch",    nullptr,     out.bcp_batch);   // optional
        if (!fill_sym(m, "bcp_done",    nullptr,     out.bcp_done))   { BCPLOAD_DBG("load:   missing bcp_done\n");    return false; }
        fill_sym(m,     "bcp_control",  nullptr,     out.bcp_control); // optional
        return true;
    };

    bool loaded = false;

    // 1) dlopen the driver name reported by the driver manager (often a full
    //    path on unixODBC). Use RTLD_GLOBAL so symbols stay resolvable.
    void* mod = dlopen(drv, RTLD_NOW|RTLD_GLOBAL);
    if (mod) { BCPLOAD_DBG("load: dlopen('%s') ok\n", drv); loaded = load_syms(mod); }
    else     { BCPLOAD_DBG("load: dlopen('%s') failed: %s\n", drv, dlerror()); }

    // 2) The driver manager already has the driver loaded in-process; try the
    //    global symbol table (RTLD_DEFAULT) before giving up.
    if (!loaded) {
        BCPLOAD_DBG("load: trying RTLD_DEFAULT (in-process symbols)\n");
        loaded = load_syms(RTLD_DEFAULT);
    }

#ifdef __linux__
    // 3) Resolve the driver's absolute path from /proc/self/maps and dlopen it
    //    with RTLD_GLOBAL. Version- and location-agnostic.
    if (!loaded) {
        char fullpath[4096] = {0};
        if (find_driver_path_in_maps(drv, fullpath, sizeof(fullpath))) {
            BCPLOAD_DBG("load: driver mapped at '%s'\n", fullpath);
            void* h = dlopen(fullpath, RTLD_NOW|RTLD_GLOBAL);
            if (h) loaded = load_syms(h);
            else   BCPLOAD_DBG("load: dlopen('%s') failed: %s\n", fullpath, dlerror());
        } else {
            BCPLOAD_DBG("load: driver not found in /proc/self/maps\n");
        }
    }
#endif

    // 4) Last resort: probe well-known msodbcsql install locations.
    if (!loaded) {
        static const char* candidates[] = {
            "libmsodbcsql-18.so", "libmsodbcsql-17.so", "libmsodbcsql.so",
            "/opt/microsoft/msodbcsql18/lib64/libmsodbcsql-18.so",
            "/opt/microsoft/msodbcsql17/lib64/libmsodbcsql-17.so",
        };
        for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]) && !loaded; ++i) {
            void* h = dlopen(candidates[i], RTLD_NOW|RTLD_GLOBAL);
            if (h) { BCPLOAD_DBG("load: opened fallback '%s'\n", candidates[i]); loaded = load_syms(h); }
        }
    }

    if (!loaded) { BCPLOAD_DBG("load: could not resolve BCP exports\n"); return false; }
#endif

    // Require the core set:
    out.loaded = true;
    BCPLOAD_DBG("load: success\n");
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

// Case-insensitive (ASCII-folded) UTF-16 string equality. Non-ASCII code units
// must match exactly (we don't do Unicode case folding), which is correct when
// the INSERT uses the same case as the table definition.
static int ci_equal_w(const SQLWCHAR* a, const SQLWCHAR* b) {
    while (*a && *b) {
        SQLWCHAR ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (SQLWCHAR)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (SQLWCHAR)(cb + 32);
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

// Convert a UTF-8 C string to a newly PyMem-allocated, NUL-terminated UTF-16LE
// SQLWCHAR buffer (SQLWCHAR is 2 bytes on both Windows and unixODBC). Returns
// NULL on failure (no Python exception left set).
static SQLWCHAR* utf8_to_wide(const char* s) {
    PyObject* u = PyUnicode_FromString(s);
    if (!u) { PyErr_Clear(); return NULL; }
    PyObject* b = PyUnicode_AsEncodedString(u, "utf-16-le", "strict");
    Py_DECREF(u);
    if (!b) { PyErr_Clear(); return NULL; }
    Py_ssize_t nbytes = PyBytes_GET_SIZE(b);
    SQLWCHAR* w = (SQLWCHAR*)PyMem_Malloc((size_t)nbytes + sizeof(SQLWCHAR));
    if (!w) { Py_DECREF(b); return NULL; }
    memcpy(w, PyBytes_AS_STRING(b), (size_t)nbytes);
    w[nbytes / (Py_ssize_t)sizeof(SQLWCHAR)] = 0;
    Py_DECREF(b);
    return w;
}

// Append a copy of s[0..len) to a growable PyMem array. Returns 1 on success.
static int names_append(char*** names, int* count, int* cap, const char* s, int len) {
    if (*count == *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        char** np = (char**)PyMem_Realloc(*names, (size_t)ncap * sizeof(char*));
        if (!np) return 0;
        *names = np; *cap = ncap;
    }
    char* copy = (char*)PyMem_Malloc((size_t)len + 1);
    if (!copy) return 0;
    memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    (*names)[(*count)++] = copy;
    return 1;
}

void bcp_free_names(char** names, int n) {
    if (!names) return;
    for (int i = 0; i < n; ++i) PyMem_Free(names[i]);
    PyMem_Free(names);
}

int parse_insert_target(const char* sql,
                        char* tableref, int tableref_cap,
                        char* schema, int schema_cap,
                        char* table, int table_cap,
                        char*** out_names, int* out_ncols)
{
    *out_names = NULL;
    *out_ncols = 0;
    if (schema_cap > 0) schema[0] = '\0';

    const char* p = sql;
    p = skip_ws_and_comments(p);
    if (!match_kw(&p, "INSERT")) return 0;

    // optional stuff between INSERT and INTO (e.g. TOP (...) PERCENT)
    p = skip_ws_and_comments(p);
    skip_top_clause(&p);
    p = skip_ws_and_comments(p);

    if (!match_kw(&p, "INTO")) return 0;
    p = skip_ws_and_comments(p);

    // Parse up to a 3-part name ([cat.][schema.]table), recording the verbatim
    // reference (for bcp_init) and the unquoted parts (for SQLColumns).
    char parts[3][256];
    int nparts = 0;
    const char* refStart = p;
    const char* refEnd = p;
    for (;;) {
        if (nparts >= 3) return 0;
        if (parse_identifier(&p, parts[nparts], (int)sizeof(parts[0])) < 0) return 0;
        ++nparts;
        refEnd = p;
        const char* save = p;
        p = skip_ws_and_comments(p);
        if (*p == '.') { ++p; p = skip_ws_and_comments(p); continue; }
        p = save;
        break;
    }

    const char* tbl = parts[nparts - 1];
    const char* sch = (nparts >= 2) ? parts[nparts - 2] : "";

    int tn = 0; while (tbl[tn]) ++tn;
    if (tn <= 0 || tn >= table_cap) return 0;
    memcpy(table, tbl, (size_t)tn + 1);

    int sn = 0; while (sch[sn]) ++sn;
    if (sn >= schema_cap) return 0;
    memcpy(schema, sch, (size_t)sn + 1);

    int rn = (int)(refEnd - refStart);
    if (rn <= 0 || rn >= tableref_cap) return 0;
    memcpy(tableref, refStart, (size_t)rn);
    tableref[rn] = '\0';

    p = skip_ws_and_comments(p);

    // Optional explicit column list. BCP binds by ordinal, so the caller maps
    // these names to table ordinals (see bcp_resolve_ordinals).
    char** names = NULL;
    int count = 0, cap = 0;
    if (*p == '(') {
        ++p;
        for (;;) {
            p = skip_ws_and_comments(p);
            char ident[256];
            if (parse_identifier(&p, ident, (int)sizeof(ident)) < 0) { bcp_free_names(names, count); return 0; }
            int il = 0; while (ident[il]) ++il;
            if (!names_append(&names, &count, &cap, ident, il)) {
                bcp_free_names(names, count); PyErr_NoMemory(); return 0;
            }
            p = skip_ws_and_comments(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ')') { ++p; break; }
            bcp_free_names(names, count); return 0;   // malformed list
        }
        p = skip_ws_and_comments(p);
    }

    // Require VALUES: reject INSERT ... SELECT/EXEC, DEFAULT VALUES, table hints.
    if (!match_kw(&p, "VALUES")) { bcp_free_names(names, count); return 0; }

    *out_names = names;
    *out_ncols = count;
    return 1;
}

int bcp_resolve_ordinals(HDBC hdbc, const char* schema, const char* table,
                         char** names, int n, int* out_ord, int* out_total)
{
    for (int i = 0; i < n; ++i) out_ord[i] = 0;
    *out_total = 0;

    // All names go through SQLColumnsW (Unicode) so non-ASCII column names work;
    // everything is compared as UTF-16.
    int result = 0;
    SQLRETURN rc;
    HSTMT hstmt = SQL_NULL_HANDLE;
    SQLWCHAR* table_w = NULL;
    SQLWCHAR* schema_w = NULL;            // heap copy of an explicit schema
    SQLWCHAR  schema_default[256];        // from SCHEMA_NAME()
    SQLWCHAR* schema_arg = NULL;          // -> schema_w or schema_default
    SQLWCHAR** name_w = NULL;
    int names_built = 0;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) { hstmt = SQL_NULL_HANDLE; goto done; }

    name_w = (SQLWCHAR**)PyMem_Calloc((size_t)n, sizeof(SQLWCHAR*));
    if (!name_w) goto done;
    for (int i = 0; i < n; ++i) {
        name_w[i] = utf8_to_wide(names[i]);
        if (!name_w[i]) goto done;
        names_built = i + 1;
    }

    table_w = utf8_to_wide(table);
    if (!table_w) goto done;

    if (schema && schema[0]) {
        schema_w = utf8_to_wide(schema);
        if (!schema_w) goto done;
        schema_arg = schema_w;
    } else {
        // The INSERT omitted the schema: resolve the connection's default schema
        // and pass it explicitly. SQLColumns with a NULL schema returns columns
        // for EVERY table of this name in EVERY schema, which would inflate the
        // column count and could resolve an ordinal from the wrong table --
        // whereas bcp_init resolves the unqualified name against the default
        // schema. Pinning both to the same schema keeps the ordinals consistent.
        schema_default[0] = 0;
        Py_BEGIN_ALLOW_THREADS
        rc = SQLExecDirectW(hstmt, (SQLWCHAR*)u"SELECT SCHEMA_NAME()", SQL_NTS);
        Py_END_ALLOW_THREADS
        if (SQL_SUCCEEDED(rc)) {
            SQLLEN cb = 0;
            SQLBindCol(hstmt, 1, SQL_C_WCHAR, schema_default, (SQLLEN)sizeof(schema_default), &cb);
            Py_BEGIN_ALLOW_THREADS
            rc = SQLFetch(hstmt);
            Py_END_ALLOW_THREADS
            if ((rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) || cb == SQL_NULL_DATA)
                schema_default[0] = 0;
        }
        SQLFreeStmt(hstmt, SQL_CLOSE);
        SQLFreeStmt(hstmt, SQL_UNBIND);
        if (!schema_default[0]) goto done;   // couldn't resolve -> fall back
        schema_arg = schema_default;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = SQLColumnsW(hstmt, NULL, 0, schema_arg, SQL_NTS, table_w, SQL_NTS, NULL, 0);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(rc)) goto done;

    {
        // SQLColumns result columns: 4 = COLUMN_NAME, 17 = ORDINAL_POSITION.
        SQLWCHAR wcolname[256];
        SQLINTEGER ordinal = 0;
        SQLLEN cbName = 0, cbOrd = 0;
        SQLBindCol(hstmt, 4, SQL_C_WCHAR, wcolname, (SQLLEN)sizeof(wcolname), &cbName);
        SQLBindCol(hstmt, 17, SQL_C_SLONG, &ordinal, 0, &cbOrd);
        for (;;) {
            Py_BEGIN_ALLOW_THREADS
            rc = SQLFetch(hstmt);
            Py_END_ALLOW_THREADS
            if (rc == SQL_NO_DATA) break;
            if (!SQL_SUCCEEDED(rc)) goto done;
            if (cbName == SQL_NULL_DATA || cbOrd == SQL_NULL_DATA) continue;
            ++(*out_total);
            for (int i = 0; i < n; ++i)
                if (out_ord[i] == 0 && ci_equal_w(name_w[i], wcolname))
                    out_ord[i] = (int)ordinal;
        }
    }

    result = 1;
    for (int i = 0; i < n; ++i)
        if (out_ord[i] <= 0) result = 0;   // a listed column was not found
    // Reject duplicate/aliased names that resolved to the same ordinal: that
    // would bind two host columns to one server column and leave another unbound.
    for (int i = 0; i < n && result; ++i)
        for (int j = i + 1; j < n; ++j)
            if (out_ord[i] == out_ord[j]) { result = 0; break; }

done:
    if (hstmt != SQL_NULL_HANDLE) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    if (name_w) { for (int i = 0; i < names_built; ++i) PyMem_Free(name_w[i]); PyMem_Free(name_w); }
    PyMem_Free(table_w);
    PyMem_Free(schema_w);
    return result;
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
    const DBINT cbData  = c->isVarLen ? SQL_VARLEN_DATA : (DBINT)c->fixedSize;

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
        const DBINT cbData = c->isVarLen ? SQL_VARLEN_DATA : (DBINT)c->fixedSize;

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
        long long lv = PyLong_AsLongLong(cell);
        if (PyErr_Occurred()) return 0;
        if (lv < -2147483648LL || lv > 2147483647LL) {
            PyErr_SetString(PyExc_OverflowError, "INTEGER value out of range for BCP column");
            return 0;
        }
        DBINT v = (DBINT)lv;
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
