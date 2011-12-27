
#include "pyodbc.h"
#include "errors.h"
#include "pyodbcmodule.h"

// Exceptions

struct SqlStateMapping
{
    char* prefix;
    size_t prefix_len;
    PyObject** pexc_class;      // Note: Double indirection (pexc_class) necessary because the pointer values are not
                                // initialized during startup
};

static const struct SqlStateMapping sql_state_mapping[] =
{
    { "0A000", 5, &NotSupportedError },
    { "40002", 5, &IntegrityError },
    { "22",    2, &DataError },
    { "23",    2, &IntegrityError },
    { "24",    2, &ProgrammingError },
    { "25",    2, &ProgrammingError },
    { "42",    2, &ProgrammingError },
    { "HYT00", 5, &OperationalError },
    { "HYT01", 5, &OperationalError },
};


static PyObject* ExceptionFromSqlState(const char* sqlstate)
{
    // Returns the appropriate Python exception class given a SQLSTATE value.

    if (sqlstate && *sqlstate)
    {
        for (size_t i = 0; i < _countof(sql_state_mapping); i++)
            if (memcmp(sqlstate, sql_state_mapping[i].prefix, sql_state_mapping[i].prefix_len) == 0)
                return *sql_state_mapping[i].pexc_class;
    }

    return Error;
}


PyObject* RaiseErrorV(const char* sqlstate, PyObject* exc_class, const char* format, ...)
{
    PyObject *pAttrs = 0, *pError = 0;

    if (!sqlstate || !*sqlstate)
        sqlstate = "HY000";

    if (!exc_class)
        exc_class = ExceptionFromSqlState(sqlstate);

    // Note: Don't use any native strprintf routines.  With Py_ssize_t, we need "%zd", but VC .NET doesn't support it.
    // PyString_FromFormatV already takes this into account.

    va_list marker;
    va_start(marker, format);
    PyObject* pMsg = PyString_FromFormatV(format, marker);
    va_end(marker);
    if (!pMsg)
    {
        PyErr_NoMemory();
        return 0;
    }

    // Create an exception with a 'sqlstate' attribute (set to None if we don't have one) whose 'args' attribute is a
    // tuple containing the message and sqlstate value.  The 'sqlstate' attribute ensures it is easy to access in
    // Python (and more understandable to the reader than ex.args[1]), but putting it in the args ensures it shows up
    // in logs because of the default repr/str.

    pAttrs = Py_BuildValue("(Os)", pMsg, sqlstate);
    if (pAttrs)
    {
        pError = PyEval_CallObject(exc_class, pAttrs);
        if (pError)
            RaiseErrorFromException(pError);
    }

    Py_DECREF(pMsg);
    Py_XDECREF(pAttrs);
    Py_XDECREF(pError);

    return 0;
}


#if PY_MAJOR_VERSION < 3
#define PyString_CompareWithASCIIString(lhs, rhs) _strcmpi(PyString_AS_STRING(lhs), rhs)
#else
#define PyString_CompareWithASCIIString PyUnicode_CompareWithASCIIString
#endif


bool HasSqlState(PyObject* ex, const char* szSqlState)
{
    // Returns true if `ex` is an exception and has the given SQLSTATE.  It is safe to pass 0 for ex.

    bool has = false;

    if (ex)
    {
        PyObject* args = PyObject_GetAttrString(ex, "args");
        if (args != 0)
        {
            PyObject* s = PySequence_GetItem(args, 1);
            if (s != 0 && PyString_Check(s))
            {
                // const char* sz = PyString_AsString(s);
                // if (sz && _strcmpi(sz, szSqlState) == 0)
                //     has = true;
                has = (PyString_CompareWithASCIIString(s, szSqlState) == 0);
            }
            Py_XDECREF(s);
            Py_DECREF(args);
        }
    }

    return has;
}


static PyObject* GetError(const char* sqlstate, PyObject* exc_class, PyObject* pMsg)
{
    // pMsg
    //   The error message.  This function takes ownership of this object, so we'll free it if we fail to create an
    //   error.

    PyObject *pSqlState=0, *pAttrs=0, *pError=0;

    if (!sqlstate || !*sqlstate)
        sqlstate = "HY000";

    if (!exc_class)
        exc_class = ExceptionFromSqlState(sqlstate);

    pAttrs = PyTuple_New(2);
    if (!pAttrs)
    {
        Py_DECREF(pMsg);
        return 0;
    }

    PyTuple_SetItem(pAttrs, 1, pMsg); // pAttrs now owns the pMsg reference; steals a reference, does not increment

    pSqlState = PyString_FromString(sqlstate);
    if (!pSqlState)
    {
        Py_DECREF(pAttrs);
        return 0;
    }

    PyTuple_SetItem(pAttrs, 0, pSqlState); // pAttrs now owns the pSqlState reference

    pError = PyEval_CallObject(exc_class, pAttrs); // pError will incref pAttrs

    Py_XDECREF(pAttrs);

    return pError;
}


static const char* DEFAULT_ERROR = "The driver did not supply an error!";

PyObject* RaiseErrorFromHandle(const char* szFunction, HDBC hdbc, HSTMT hstmt)
{
    // The exception is "set" in the interpreter.  This function returns 0 so this can be used in a return statement.

    PyObject* pError = GetErrorFromHandle(szFunction, hdbc, hstmt);

    if (pError)
    {
        RaiseErrorFromException(pError);
        Py_DECREF(pError);
    }

    return 0;
}


PyObject* GetErrorFromHandle(const char* szFunction, HDBC hdbc, HSTMT hstmt)
{
    TRACE("In RaiseError(%s)!\n", szFunction);

    // Creates and returns an exception from ODBC error information.
    //
    // ODBC can generate a chain of errors which we concatenate into one error message.  We use the SQLSTATE from the
    // first message, which seems to be the most detailed, to determine the class of exception.
    //
    // If the function fails, for example, if it runs out of memory, zero is returned.
    //
    // szFunction
    //   The name of the function that failed.  Python generates a useful stack trace, but we often don't know where in
    //   the C++ code we failed.

    SQLSMALLINT nHandleType;
    SQLHANDLE   h;

    char sqlstate[6] = "";
    SQLINTEGER nNativeError;
    SQLSMALLINT cchMsg;

    char sqlstateT[6];
    char szMsg[1024];

    PyObject* pMsg = 0;
    PyObject* pMsgPart = 0;

    if (hstmt != SQL_NULL_HANDLE)
    {
        nHandleType = SQL_HANDLE_STMT;
        h = hstmt;
    }
    else if (hdbc != SQL_NULL_HANDLE)
    {
        nHandleType = SQL_HANDLE_DBC;
        h = hdbc;
    }
    else
    {
        nHandleType = SQL_HANDLE_ENV;
        h = henv;
    }

    // unixODBC + PostgreSQL driver 07.01.0003 (Fedora 8 binaries from RPMs) crash if you call SQLGetDiagRec more
    // than once.  I hate to do this, but I'm going to only call it once for non-Windows platforms for now...

    SQLSMALLINT iRecord = 1;

    for (;;)
    {
        szMsg[0]     = 0;
        sqlstateT[0] = 0;
        nNativeError = 0;
        cchMsg       = 0;

        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetDiagRec(nHandleType, h, iRecord, (SQLCHAR*)sqlstateT, &nNativeError, (SQLCHAR*)szMsg, (short)(_countof(szMsg)-1), &cchMsg);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            break;

        // Not always NULL terminated (MS Access)
        sqlstateT[5] = 0;

        if (cchMsg != 0)
        {
            if (iRecord == 1)
            {
                // This is the first error message, so save the SQLSTATE for determining the exception class and append
                // the calling function name.

                memcpy(sqlstate, sqlstateT, sizeof(sqlstate[0]) * _countof(sqlstate));

                pMsg = PyString_FromFormat("[%s] %s (%ld) (%s)", sqlstateT, szMsg, (long)nNativeError, szFunction);
                if (pMsg == 0)
                    return 0;
            }
            else
            {
                // This is not the first error message, so append to the existing one.
                pMsgPart = PyString_FromFormat("; [%s] %s (%ld)", sqlstateT, szMsg, (long)nNativeError);
                if (pMsgPart == 0)
                {
                    Py_XDECREF(pMsg);
                    return 0;
                }
                PyString_ConcatAndDel(&pMsg, pMsgPart);
            }
        }

        iRecord++;

#ifndef _MSC_VER
        // See non-Windows comment above
        break;
#endif
    }

    if (pMsg == 0)
    {
        // This only happens using unixODBC.  (Haven't tried iODBC yet.)  Either the driver or the driver manager is
        // buggy and has signaled a fault without recording error information.
        sqlstate[0] = '\0';
        pMsg = PyString_FromString(DEFAULT_ERROR);
        if (pMsg == 0)
        {
            PyErr_NoMemory();
            return 0;
        }
    }

    return GetError(sqlstate, 0, pMsg);
}


static bool GetSqlState(HSTMT hstmt, char* szSqlState)
{
    SQLCHAR szMsg[300];
    SQLSMALLINT cbMsg = (SQLSMALLINT)(_countof(szMsg) - 1);
    SQLINTEGER nNative;
    SQLSMALLINT cchMsg;
    SQLRETURN ret;

    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, (SQLCHAR*)szSqlState, &nNative, szMsg, cbMsg, &cchMsg);
    Py_END_ALLOW_THREADS
    return SQL_SUCCEEDED(ret);
}


bool HasSqlState(HSTMT hstmt, const char* szSqlState)
{
    char szActual[6];
    if (!GetSqlState(hstmt, szActual))
        return false;
    return memcmp(szActual, szSqlState, 5) == 0;
}
