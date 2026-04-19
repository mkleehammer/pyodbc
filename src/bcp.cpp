// Implementation for the Connection.bcp() method.

#include "pyodbc.h"
#include "wrapper.h"
#include "pyodbcmodule.h"
#include "textenc.h"
#include "connection.h"
#include "errors.h"
#include "bcp.h"

#ifdef _MSC_VER
    #include <psapi.h>
    #define WINAPI_OR_CDECL WINAPI
    typedef FARPROC _BCP_FUNC;
#else
    #include <dlfcn.h>
    #define WINAPI_OR_CDECL /* nothing */
    typedef void* _BCP_FUNC;
#endif

#define BCP_DEBUG 0

// ODBC BCP constants.
#define FAIL           0
#define SUCCEED        1
#define DB_IN          1
#define DB_OUT         2
#define BCPMAXERRS     1  // Sets max errors allowed
#define BCPFIRST       2  // Sets first row to be copied out
#define BCPLAST        3  // Sets number of rows to be copied out

// Signatures for BCP calls.
typedef int (WINAPI_OR_CDECL *_BCP_INIT)(HDBC, SQLWCHAR*, SQLWCHAR*, SQLWCHAR*, int);
typedef int (WINAPI_OR_CDECL *_BCP_READFMT)(HDBC, SQLWCHAR*);
typedef int (WINAPI_OR_CDECL *_BCP_EXEC)(HDBC, long*);
typedef int (WINAPI_OR_CDECL *_BCP_CONTROL)(HDBC, int, void*);

// BCP functions
static _BCP_INIT     bcp_init     = 0;
static _BCP_READFMT  bcp_readfmt  = 0;
static _BCP_EXEC     bcp_exec     = 0;
static _BCP_CONTROL  bcp_control  = 0;


#ifdef __linux__
// Load the driver's library.
static void* _bcplib;
static void* _load_bcplib(HDBC hdbc)
{
    // Ask the DM for the file name of the driver's library.
    char name[1024];
    SQLSMALLINT cch;
    SQLRETURN rc = SQLGetInfo(hdbc, SQL_DRIVER_NAME, name, sizeof name, &cch);
    if (rc != SQL_SUCCESS)
        return NULL;

    // If we're lucky, that's all we need.
    void *handle = dlopen(name, RTLD_NOLOAD | RTLD_LAZY);
    if (handle)
        return handle;

    // As we really expected, we need the full path for the library.
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return NULL;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        char *path = strchr(line, '/');
        if (path) {
            path[strcspn(path, "\r\n")] = '\0';
            char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (strcmp(base, name) == 0) {
                handle = dlopen(path, RTLD_NOLOAD | RTLD_LAZY);
                break;
            }
        }
    }
    fclose(fp);
    return handle;
}
#endif

// Find one of the bcp API functions; different strategies for each platform.
static _BCP_FUNC _find_bcp_function(char* name)
{
#ifdef _WIN32
    static size_t count;
    static HMODULE mods[256];
    static HMODULE odbclib;
    if (!count) {
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
            return 0;
        count = needed / sizeof(HMODULE);
    }
    if (odbclib)
        return GetProcAddress(odbclib, name);
    for (size_t i = 0; i < count; ++i) {
        _BCP_FUNC func = GetProcAddress(mods[i], name);
        if (func) {
            odbclib = mods[i];
            return func;
        }
    }
    return 0;
#else
#ifdef __linux__
    return _bcplib ? dlsym(_bcplib, name) : NULL;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
#endif
}

// Dynamically locate the bcp API functions we need (returns false if we can't find them).
static bool _load_bcp_functions(HDBC hdbc)
{
    if (!hdbc)  // only really needed for Linux, but this keeps the compiler happy :)
        return false;
#ifdef __linux__
    _bcplib = _load_bcplib(hdbc);
    if (!_bcplib)
        return false;
#endif
    bcp_init    = (_BCP_INIT)   _find_bcp_function("bcp_initW");
    bcp_readfmt = (_BCP_READFMT)_find_bcp_function("bcp_readfmtW");
    bcp_exec    = (_BCP_EXEC)   _find_bcp_function("bcp_exec");
    bcp_control = (_BCP_CONTROL)_find_bcp_function("bcp_control");
#ifdef __linux__
    dlclose(_bcplib);  // just releases the handle, doesn't unload
    _bcplib = NULL;
#endif
    return bcp_init && bcp_readfmt && bcp_exec && bcp_control;
}

// Apply a control option if the user provided a value, returning false on failure.
bool _apply_int_option(Connection* conn, PyObject* value, int option, const char* name)
{
    if (value == Py_None)
        return true;
    if (!PyLong_Check(value)) {
        PyErr_Format(ProgrammingError, "%s must be an integer", name);
        return false;
    }
    long intval = PyLong_AsLong(value);
    if (bcp_control(conn->hdbc, option, (void*)intval) != SUCCEED) {
        PyErr_Format(OperationalError, "failure setting %s", name);
        return false;
    }
    return true;
}

// Prepare and execute a bcp operation.
PyObject* _bcp_impl(PyObject* py_conn, const BCP_OPTS& opts)
{
    int bcp_rc = SUCCEED;
    long row_count = 0;

#if BCP_DEBUG
    // Show the arguments.
    printf("action     : %ld\n", opts.action);
    printf("table      : "); PyObject_Print(opts.table,      stdout, 0); printf("\n");
    printf("datafile   : "); PyObject_Print(opts.datafile,   stdout, 0); printf("\n");
    printf("formatfile : "); PyObject_Print(opts.formatfile, stdout, 0); printf("\n");
    printf("errorfile  : "); PyObject_Print(opts.errorfile,  stdout, 0); printf("\n");
    printf("firstrow   : "); PyObject_Print(opts.firstrow,   stdout, 0); printf("\n");
    printf("lastrow    : "); PyObject_Print(opts.lastrow,    stdout, 0); printf("\n");
    printf("maxerrors  : "); PyObject_Print(opts.maxerrors,  stdout, 0); printf("\n");
#endif

    // Make sure we have a valid connection.
    if (!py_conn || !Connection_Check(py_conn)) {
        PyErr_SetString(ProgrammingError, "first argument must be a valid connection");
        return 0;
    }
    Connection* conn = (Connection*)py_conn;
    if (conn->hdbc == SQL_NULL_HANDLE) {
        PyErr_SetString(ProgrammingError, "attempt to use a closed connection.");
        return 0;
    }

    // Verify that bcp is enabled for this connection.
    if (!conn->bcp_enabled) {
        PyErr_SetString(ProgrammingError, "bcp not supported by this driver.");
        return 0;
    }

    // Load the BCP function pointers.
    if (!_load_bcp_functions(conn->hdbc)) {
        PyErr_SetString(OperationalError, "bcp functions not provided by driver");
        return 0;
    }

    // Get the required arguments. Note that we use less generic names in public-facing API.
    if (opts.action != DB_IN && opts.action != DB_OUT) {
        PyErr_SetString(ProgrammingError, "action must be pyodbc.BCP_IN or pyodbc.BCP_OUT");
        return 0;
    }
    SQLWChar table(opts.table, ENCSTR_UTF16NE);
    if (opts.datafile == Py_None) {
        PyErr_SetString(ProgrammingError, "datafile is a required argument");
        return 0;
    }
    PyObject* datafile_path = PyOS_FSPath(opts.datafile);
    if (!datafile_path)
        return 0;  // exception already raised by PyOS_FSPath()
    if (!PyUnicode_Check(datafile_path)) {
        Py_DECREF(datafile_path);
        PyErr_SetString(PyExc_TypeError, "datafile must be a str or pathlib.Path");
        return 0;
    }
    SQLWChar datafile(datafile_path, ENCSTR_UTF16NE);

    // The error filename is optional.
    SQLWChar errorfile;
    PyObject* errorfile_path = 0;
    if (opts.errorfile != Py_None) {
        errorfile_path = PyOS_FSPath(opts.errorfile);
        if (!errorfile_path) {
            Py_DECREF(datafile_path);
            return 0;  // exception set by PyOS_FSPath()
        }
        if (!PyUnicode_Check(errorfile_path)) {
            Py_DECREF(datafile_path);
            Py_DECREF(errorfile_path);
            PyErr_SetString(PyExc_TypeError, "errorfile must be a str or pathlib.Path");
            return 0;
        }
        errorfile.set(errorfile_path, ENCSTR_UTF16NE);
    }

    // Initialize the bcp job.
    Py_BEGIN_ALLOW_THREADS
    bcp_rc = bcp_init(conn->hdbc, (SQLWCHAR*)table, (SQLWCHAR*)datafile,  (SQLWCHAR*)errorfile, opts.action);
    Py_END_ALLOW_THREADS
    Py_DECREF(datafile_path);
    Py_XDECREF(errorfile_path);
    if (conn->hdbc == SQL_NULL_HANDLE) {
        // The connection was closed by another thread.
        PyErr_SetString(ProgrammingError, "connection was closed.");
        return 0;
    }
    if (bcp_rc != SUCCEED) {
        PyErr_SetString(OperationalError, "bcp_init failure");
        return 0;
    }

    // Read the transfer format file if requested.
    if (opts.formatfile != Py_None) {
        PyObject* formatfile_path = PyOS_FSPath(opts.formatfile);
        if (!formatfile_path)
            return 0;  // exception is already set
        if (!PyUnicode_Check(formatfile_path)) {
            PyErr_SetString(ProgrammingError, "formatfile must be a str or pathlib.Path");
            return 0;
        }
        SQLWChar formatfile(formatfile_path, ENCSTR_UTF16NE);
        Py_BEGIN_ALLOW_THREADS
        bcp_rc = bcp_readfmt(conn->hdbc, (SQLWCHAR*)formatfile);
        Py_END_ALLOW_THREADS
        Py_DECREF(formatfile_path);
        if (conn->hdbc == SQL_NULL_HANDLE) {
            PyErr_SetString(ProgrammingError, "connection was closed.");
            return 0;
        }
        if (bcp_rc != SUCCEED) {
            PyErr_SetString(OperationalError, "bcp_readfmt failure");
            return 0;
        }
    }

    // Apply the rest of the options specified (no need to release the GIL for these).
    if (!_apply_int_option(conn, opts.firstrow,  BCPFIRST,   "firstrow" )) return 0;
    if (!_apply_int_option(conn, opts.lastrow,   BCPLAST,    "lastrow"  )) return 0;
    if (!_apply_int_option(conn, opts.maxerrors, BCPMAXERRS, "maxerrors")) return 0;

    // Perform the transfer.
    Py_BEGIN_ALLOW_THREADS
    bcp_rc = bcp_exec(conn->hdbc, &row_count);
    Py_END_ALLOW_THREADS
    if (bcp_rc != SUCCEED) {
        PyErr_SetString(OperationalError, "bcp_exec failure");
        return 0;
    }

    // Return the number of rows transferred.
    return PyLong_FromLong(row_count);
}
