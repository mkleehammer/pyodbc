
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
//  
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pyodbc.h"
#include "connection.h"
#include "cursor.h"
#include "pyodbcmodule.h"
#include "errors.h"
#include "wrapper.h"
#include "cnxninfo.h"
#include "sqlwchar.h"

static char connection_doc[] =
    "Connection objects manage connections to the database.\n"
    "\n"
    "Each manages a single ODBC HDBC.";

static Connection* Connection_Validate(PyObject* self)
{
    Connection* cnxn;
    
    if (self == 0 || !Connection_Check(self))
    {
        PyErr_SetString(PyExc_TypeError, "Connection object required");
        return 0;
    }

    cnxn = (Connection*)self;

    if (cnxn->hdbc == SQL_NULL_HANDLE)
    {
        PyErr_SetString(ProgrammingError, "Attempt to use a closed connection.");
        return 0;
    }

    return cnxn;
}

static bool Connect(PyObject* pConnectString, HDBC hdbc, bool fAnsi, long timeout)
{
    // This should have been checked by the global connect function.
    I(PyString_Check(pConnectString) || PyUnicode_Check(pConnectString));

    const int cchMax = 600;

    if (PySequence_Length(pConnectString) >= cchMax)
    {
        PyErr_SetString(PyExc_TypeError, "connection string too long");
        return false;
    }

    // The driver manager determines if the app is a Unicode app based on whether we call SQLDriverConnectA or
    // SQLDriverConnectW.  Some drivers, notably Microsoft Access/Jet, change their behavior based on this, so we try
    // the Unicode version first.  (The Access driver only supports Unicode text, but SQLDescribeCol returns SQL_CHAR
    // instead of SQL_WCHAR if we connect with the ANSI version.  Obviously this causes lots of errors since we believe
    // what it tells us (SQL_CHAR).)

    // Python supports only UCS-2 and UCS-4, so we shouldn't need to worry about receiving surrogate pairs.  However,
    // Windows does use UCS-16, so it is possible something would be misinterpreted as one.  We may need to examine
    // this more.

    SQLRETURN ret;

    if (timeout > 0)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttr(hdbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)timeout, SQL_IS_UINTEGER);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            RaiseErrorFromHandle("SQLSetConnectAttr(SQL_ATTR_LOGIN_TIMEOUT)", hdbc, SQL_NULL_HANDLE);
    }

    if (!fAnsi)
    {
        SQLWChar connectString(pConnectString);
        Py_BEGIN_ALLOW_THREADS
        ret = SQLDriverConnectW(hdbc, 0, connectString, (SQLSMALLINT)connectString.size(), 0, 0, 0, SQL_DRIVER_NOPROMPT);
        Py_END_ALLOW_THREADS
        if (SQL_SUCCEEDED(ret))
            return true;

        // The Unicode function failed.  If the error is that the driver doesn't have a Unicode version (IM001), continue
        // to the ANSI version.
        //
        // I've commented this out since a number of common drivers are returning different errors.  The MySQL 5
        // driver, for example, returns IM002 "Data source name not found...".
        //
        // PyObject* error = GetErrorFromHandle("SQLDriverConnectW", hdbc, SQL_NULL_HANDLE);
        // if (!HasSqlState(error, "IM001"))
        // {
        //     RaiseErrorFromException(error);
        //     return false;
        // }
        // Py_XDECREF(error);
    }
        
    SQLCHAR szConnect[cchMax];
    if (PyUnicode_Check(pConnectString))
    {
        Py_UNICODE* p = PyUnicode_AS_UNICODE(pConnectString);
        for (Py_ssize_t i = 0, c = PyUnicode_GET_SIZE(pConnectString); i <= c; i++)
        {
            if (p[i] > 0xFF)
            {
                PyErr_SetString(PyExc_TypeError, "A Unicode connection string was supplied but the driver does "
                                "not have a Unicode connect function");
                return false;
            }
            szConnect[i] = (SQLCHAR)p[i];
        }
    }
    else
    {
#if PY_MAJOR_VERSION < 3
        const char* p = PyString_AS_STRING(pConnectString);
        memcpy(szConnect, p, (size_t)(PyString_GET_SIZE(pConnectString) + 1));
#else
        PyErr_SetString(PyExc_TypeError, "Connection strings must be Unicode");
        return false;
#endif
    }

    Py_BEGIN_ALLOW_THREADS
    ret = SQLDriverConnect(hdbc, 0, szConnect, SQL_NTS, 0, 0, 0, SQL_DRIVER_NOPROMPT);
    Py_END_ALLOW_THREADS
    if (SQL_SUCCEEDED(ret))
        return true;

    RaiseErrorFromHandle("SQLDriverConnect", hdbc, SQL_NULL_HANDLE);

    return false;
}


PyObject* Connection_New(PyObject* pConnectString, bool fAutoCommit, bool fAnsi, bool fUnicodeResults, long timeout, bool fReadOnly)
{
    // pConnectString
    //   A string or unicode object.  (This must be checked by the caller.)
    //
    // fAnsi
    //   If true, do not attempt a Unicode connection.
    //
    // fUnicodeResults
    //   If true, return strings in rows as unicode objects.

    //
    // Allocate HDBC and connect
    //

    HDBC hdbc = SQL_NULL_HANDLE;
    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
        return RaiseErrorFromHandle("SQLAllocHandle", SQL_NULL_HANDLE, SQL_NULL_HANDLE);

    if (!Connect(pConnectString, hdbc, fAnsi, timeout))
    {
        // Connect has already set an exception.
        Py_BEGIN_ALLOW_THREADS
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
        return 0;
    }

    //
    // Connected, so allocate the Connection object. 
    //

    // Set all variables to something valid, so we don't crash in dealloc if this function fails.

#ifdef _MSC_VER
#pragma warning(disable : 4365)
#endif
    Connection* cnxn = PyObject_NEW(Connection, &ConnectionType);
#ifdef _MSC_VER
#pragma warning(default : 4365)
#endif

    if (cnxn == 0)
    {
        Py_BEGIN_ALLOW_THREADS
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        Py_END_ALLOW_THREADS
        return 0;
    }

    cnxn->hdbc            = hdbc;
    cnxn->nAutoCommit     = fAutoCommit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    cnxn->searchescape    = 0;
    cnxn->timeout         = 0;
    cnxn->unicode_results = fUnicodeResults;
    cnxn->conv_count      = 0;
    cnxn->conv_types      = 0;
    cnxn->conv_funcs      = 0;

    //
    // Initialize autocommit mode.
    //

    // The DB API says we have to default to manual-commit, but ODBC defaults to auto-commit.  We also provide a
    // keyword parameter that allows the user to override the DB API and force us to start in auto-commit (in which
    // case we don't have to do anything).

    if (fAutoCommit == false)
    {
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)cnxn->nAutoCommit, SQL_IS_UINTEGER);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle("SQLSetConnnectAttr(SQL_ATTR_AUTOCOMMIT)", cnxn->hdbc, SQL_NULL_HANDLE);
            Py_DECREF(cnxn);
            return 0;
        }
    }
    
    if (fReadOnly)
    {
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_ACCESS_MODE, (SQLPOINTER)SQL_MODE_READ_ONLY, 0);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
        {
            RaiseErrorFromHandle("SQLSetConnnectAttr(SQL_ATTR_ACCESS_MODE)", cnxn->hdbc, SQL_NULL_HANDLE);
            Py_DECREF(cnxn);
            return 0;
        }
    }

    TRACE("cnxn.new cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

    //
    // Gather connection-level information we'll need later.
    //

    Object info(GetConnectionInfo(pConnectString, cnxn));

    if (!info.IsValid())
    {
        Py_DECREF(cnxn);
        return 0;
    }

    CnxnInfo* p = (CnxnInfo*)info.Get();
    cnxn->odbc_major             = p->odbc_major;
    cnxn->odbc_minor             = p->odbc_minor;
    cnxn->supports_describeparam = p->supports_describeparam;
    cnxn->datetime_precision     = p->datetime_precision;
    cnxn->varchar_maxlength      = p->varchar_maxlength;
    cnxn->wvarchar_maxlength     = p->wvarchar_maxlength;
    cnxn->binary_maxlength       = p->binary_maxlength;
    cnxn->need_long_data_len     = p->need_long_data_len;

    return reinterpret_cast<PyObject*>(cnxn);
}

static void _clear_conv(Connection* cnxn)
{
    if (cnxn->conv_count != 0)
    {
        pyodbc_free(cnxn->conv_types);
        cnxn->conv_types = 0;
        
        for (int i = 0; i < cnxn->conv_count; i++)
            Py_XDECREF(cnxn->conv_funcs[i]);
        pyodbc_free(cnxn->conv_funcs);
        cnxn->conv_funcs = 0;

        cnxn->conv_count = 0;
    }
}

static char conv_clear_doc[] =
    "clear_output_converters() --> None\n\n"
    "Remove all output converter functions.";

static PyObject* Connection_conv_clear(PyObject* self, PyObject* args)
{
    UNUSED(args);
    
    Connection* cnxn = (Connection*)self;
    _clear_conv(cnxn);
    Py_RETURN_NONE;
}

static int Connection_clear(PyObject* self)
{
    // Internal method for closing the connection.  (Not called close so it isn't confused with the external close
    // method.)

    Connection* cnxn = (Connection*)self;

    if (cnxn->hdbc != SQL_NULL_HANDLE)
    {
        // REVIEW: Release threads? (But make sure you zero out hdbc *first*!

        TRACE("cnxn.clear cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

        Py_BEGIN_ALLOW_THREADS
        if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF)
            SQLEndTran(SQL_HANDLE_DBC, cnxn->hdbc, SQL_ROLLBACK);

        SQLDisconnect(cnxn->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, cnxn->hdbc);
        Py_END_ALLOW_THREADS
        
        cnxn->hdbc = SQL_NULL_HANDLE;
    }

    Py_XDECREF(cnxn->searchescape);
    cnxn->searchescape = 0;
    
    _clear_conv(cnxn);

    return 0;
}

static void Connection_dealloc(PyObject* self)
{
    Connection_clear(self);
    PyObject_Del(self);
}

static char close_doc[] =
    "Close the connection now (rather than whenever __del__ is called).\n"
    "\n"
    "The connection will be unusable from this point forward and a ProgrammingError\n"
    "will be raised if any operation is attempted with the connection.  The same\n"
    "applies to all cursor objects trying to use the connection.\n"
    "\n"
    "Note that closing a connection without committing the changes first will cause\n"
    "an implicit rollback to be performed.";
    
static PyObject* Connection_close(PyObject* self, PyObject* args)
{
    UNUSED(args);
    
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    Connection_clear(self);

    Py_RETURN_NONE;
}

static PyObject* Connection_cursor(PyObject* self, PyObject* args)
{
    UNUSED(args);
    
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    return (PyObject*)Cursor_New(cnxn);
}

static PyObject* Connection_execute(PyObject* self, PyObject* args)
{
    PyObject* result = 0;

    Cursor* cursor;
    Connection* cnxn = Connection_Validate(self);

    if (!cnxn)
        return 0;

    cursor = Cursor_New(cnxn);
    if (!cursor)
        return 0;

    result = Cursor_execute((PyObject*)cursor, args);

    Py_DECREF((PyObject*)cursor);

    return result;
}

enum
{
    GI_YESNO,
    GI_STRING,
    GI_UINTEGER,
    GI_USMALLINT,
};

struct GetInfoType
{
    SQLUSMALLINT infotype;
    int datatype; // GI_XXX
};

static const GetInfoType aInfoTypes[] = {
    { SQL_ACCESSIBLE_PROCEDURES, GI_YESNO },
    { SQL_ACCESSIBLE_TABLES, GI_YESNO },
    { SQL_ACTIVE_ENVIRONMENTS, GI_USMALLINT },
    { SQL_AGGREGATE_FUNCTIONS, GI_UINTEGER },
    { SQL_ALTER_DOMAIN, GI_UINTEGER },
    { SQL_ALTER_TABLE, GI_UINTEGER },
    { SQL_ASYNC_MODE, GI_UINTEGER },
    { SQL_BATCH_ROW_COUNT, GI_UINTEGER },
    { SQL_BATCH_SUPPORT, GI_UINTEGER },
    { SQL_BOOKMARK_PERSISTENCE, GI_UINTEGER },
    { SQL_CATALOG_LOCATION, GI_USMALLINT },
    { SQL_CATALOG_NAME, GI_YESNO },
    { SQL_CATALOG_NAME_SEPARATOR, GI_STRING },
    { SQL_CATALOG_TERM, GI_STRING },
    { SQL_CATALOG_USAGE, GI_UINTEGER },
    { SQL_COLLATION_SEQ, GI_STRING },
    { SQL_COLUMN_ALIAS, GI_YESNO },
    { SQL_CONCAT_NULL_BEHAVIOR, GI_USMALLINT },
    { SQL_CONVERT_FUNCTIONS, GI_UINTEGER },
    { SQL_CONVERT_VARCHAR, GI_UINTEGER },
    { SQL_CORRELATION_NAME, GI_USMALLINT },
    { SQL_CREATE_ASSERTION, GI_UINTEGER },
    { SQL_CREATE_CHARACTER_SET, GI_UINTEGER },
    { SQL_CREATE_COLLATION, GI_UINTEGER },
    { SQL_CREATE_DOMAIN, GI_UINTEGER },
    { SQL_CREATE_SCHEMA, GI_UINTEGER },
    { SQL_CREATE_TABLE, GI_UINTEGER },
    { SQL_CREATE_TRANSLATION, GI_UINTEGER },
    { SQL_CREATE_VIEW, GI_UINTEGER },
    { SQL_CURSOR_COMMIT_BEHAVIOR, GI_USMALLINT },
    { SQL_CURSOR_ROLLBACK_BEHAVIOR, GI_USMALLINT },
    { SQL_DATABASE_NAME, GI_STRING },
    { SQL_DATA_SOURCE_NAME, GI_STRING },
    { SQL_DATA_SOURCE_READ_ONLY, GI_YESNO },
    { SQL_DATETIME_LITERALS, GI_UINTEGER },
    { SQL_DBMS_NAME, GI_STRING },
    { SQL_DBMS_VER, GI_STRING },
    { SQL_DDL_INDEX, GI_UINTEGER },
    { SQL_DEFAULT_TXN_ISOLATION, GI_UINTEGER },
    { SQL_DESCRIBE_PARAMETER, GI_YESNO },
    { SQL_DM_VER, GI_STRING },
    { SQL_DRIVER_NAME, GI_STRING },
    { SQL_DRIVER_ODBC_VER, GI_STRING },
    { SQL_DRIVER_VER, GI_STRING },
    { SQL_DROP_ASSERTION, GI_UINTEGER },
    { SQL_DROP_CHARACTER_SET, GI_UINTEGER },
    { SQL_DROP_COLLATION, GI_UINTEGER },
    { SQL_DROP_DOMAIN, GI_UINTEGER },
    { SQL_DROP_SCHEMA, GI_UINTEGER },
    { SQL_DROP_TABLE, GI_UINTEGER },
    { SQL_DROP_TRANSLATION, GI_UINTEGER },
    { SQL_DROP_VIEW, GI_UINTEGER },
    { SQL_DYNAMIC_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_DYNAMIC_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_EXPRESSIONS_IN_ORDERBY, GI_YESNO },
    { SQL_FILE_USAGE, GI_USMALLINT },
    { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_GETDATA_EXTENSIONS, GI_UINTEGER },
    { SQL_GROUP_BY, GI_USMALLINT },
    { SQL_IDENTIFIER_CASE, GI_USMALLINT },
    { SQL_IDENTIFIER_QUOTE_CHAR, GI_STRING },
    { SQL_INDEX_KEYWORDS, GI_UINTEGER },
    { SQL_INFO_SCHEMA_VIEWS, GI_UINTEGER },
    { SQL_INSERT_STATEMENT, GI_UINTEGER },
    { SQL_INTEGRITY, GI_YESNO },
    { SQL_KEYSET_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_KEYSET_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_KEYWORDS, GI_STRING },
    { SQL_LIKE_ESCAPE_CLAUSE, GI_YESNO },
    { SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, GI_UINTEGER },
    { SQL_MAX_BINARY_LITERAL_LEN, GI_UINTEGER },
    { SQL_MAX_CATALOG_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_CHAR_LITERAL_LEN, GI_UINTEGER },
    { SQL_MAX_COLUMNS_IN_GROUP_BY, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_INDEX, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_ORDER_BY, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_SELECT, GI_USMALLINT },
    { SQL_MAX_COLUMNS_IN_TABLE, GI_USMALLINT },
    { SQL_MAX_COLUMN_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_CONCURRENT_ACTIVITIES, GI_USMALLINT },
    { SQL_MAX_CURSOR_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_DRIVER_CONNECTIONS, GI_USMALLINT },
    { SQL_MAX_IDENTIFIER_LEN, GI_USMALLINT },
    { SQL_MAX_INDEX_SIZE, GI_UINTEGER },
    { SQL_MAX_PROCEDURE_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_ROW_SIZE, GI_UINTEGER },
    { SQL_MAX_ROW_SIZE_INCLUDES_LONG, GI_YESNO },
    { SQL_MAX_SCHEMA_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_STATEMENT_LEN, GI_UINTEGER },
    { SQL_MAX_TABLES_IN_SELECT, GI_USMALLINT },
    { SQL_MAX_TABLE_NAME_LEN, GI_USMALLINT },
    { SQL_MAX_USER_NAME_LEN, GI_USMALLINT },
    { SQL_MULTIPLE_ACTIVE_TXN, GI_YESNO },
    { SQL_MULT_RESULT_SETS, GI_YESNO },
    { SQL_NEED_LONG_DATA_LEN, GI_YESNO },
    { SQL_NON_NULLABLE_COLUMNS, GI_USMALLINT },
    { SQL_NULL_COLLATION, GI_USMALLINT },
    { SQL_NUMERIC_FUNCTIONS, GI_UINTEGER },
    { SQL_ODBC_INTERFACE_CONFORMANCE, GI_UINTEGER },
    { SQL_ODBC_VER, GI_STRING },
    { SQL_OJ_CAPABILITIES, GI_UINTEGER },
    { SQL_ORDER_BY_COLUMNS_IN_SELECT, GI_YESNO },
    { SQL_PARAM_ARRAY_ROW_COUNTS, GI_UINTEGER },
    { SQL_PARAM_ARRAY_SELECTS, GI_UINTEGER },
    { SQL_PROCEDURES, GI_YESNO },
    { SQL_PROCEDURE_TERM, GI_STRING },
    { SQL_QUOTED_IDENTIFIER_CASE, GI_USMALLINT },
    { SQL_ROW_UPDATES, GI_YESNO },
    { SQL_SCHEMA_TERM, GI_STRING },
    { SQL_SCHEMA_USAGE, GI_UINTEGER },
    { SQL_SCROLL_OPTIONS, GI_UINTEGER },
    { SQL_SEARCH_PATTERN_ESCAPE, GI_STRING },
    { SQL_SERVER_NAME, GI_STRING },
    { SQL_SPECIAL_CHARACTERS, GI_STRING },
    { SQL_SQL92_DATETIME_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_FOREIGN_KEY_DELETE_RULE, GI_UINTEGER },
    { SQL_SQL92_FOREIGN_KEY_UPDATE_RULE, GI_UINTEGER },
    { SQL_SQL92_GRANT, GI_UINTEGER },
    { SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_PREDICATES, GI_UINTEGER },
    { SQL_SQL92_RELATIONAL_JOIN_OPERATORS, GI_UINTEGER },
    { SQL_SQL92_REVOKE, GI_UINTEGER },
    { SQL_SQL92_ROW_VALUE_CONSTRUCTOR, GI_UINTEGER },
    { SQL_SQL92_STRING_FUNCTIONS, GI_UINTEGER },
    { SQL_SQL92_VALUE_EXPRESSIONS, GI_UINTEGER },
    { SQL_SQL_CONFORMANCE, GI_UINTEGER },
    { SQL_STANDARD_CLI_CONFORMANCE, GI_UINTEGER },
    { SQL_STATIC_CURSOR_ATTRIBUTES1, GI_UINTEGER },
    { SQL_STATIC_CURSOR_ATTRIBUTES2, GI_UINTEGER },
    { SQL_STRING_FUNCTIONS, GI_UINTEGER },
    { SQL_SUBQUERIES, GI_UINTEGER },
    { SQL_SYSTEM_FUNCTIONS, GI_UINTEGER },
    { SQL_TABLE_TERM, GI_STRING },
    { SQL_TIMEDATE_ADD_INTERVALS, GI_UINTEGER },
    { SQL_TIMEDATE_DIFF_INTERVALS, GI_UINTEGER },
    { SQL_TIMEDATE_FUNCTIONS, GI_UINTEGER },
    { SQL_TXN_CAPABLE, GI_USMALLINT },
    { SQL_TXN_ISOLATION_OPTION, GI_UINTEGER },
    { SQL_UNION, GI_UINTEGER },
    { SQL_USER_NAME, GI_STRING },
    { SQL_XOPEN_CLI_YEAR, GI_STRING },
};

static PyObject* Connection_getinfo(PyObject* self, PyObject* args)
{
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    unsigned long infotype;
    if (!PyArg_ParseTuple(args, "k", &infotype))
        return 0;

    unsigned int i = 0;
    for (; i < _countof(aInfoTypes); i++)
    {
        if (aInfoTypes[i].infotype == infotype)
            break;
    }

    if (i == _countof(aInfoTypes))
        return RaiseErrorV(0, ProgrammingError, "Invalid getinfo value: %d", infotype);

    char szBuffer[0x1000];
    SQLSMALLINT cch = 0;

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLGetInfo(cnxn->hdbc, (SQLUSMALLINT)infotype, szBuffer, sizeof(szBuffer), &cch);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle("SQLGetInfo", cnxn->hdbc, SQL_NULL_HANDLE);
        return 0;
    }

    PyObject* result = 0;

    switch (aInfoTypes[i].datatype)
    {
    case GI_YESNO:
        result = (szBuffer[0] == 'Y') ? Py_True : Py_False;
        Py_INCREF(result);
        break;
            
    case GI_STRING:
        result = PyString_FromStringAndSize(szBuffer, (Py_ssize_t)cch);
        break;
        
    case GI_UINTEGER:
    {
        SQLUINTEGER n = *(SQLUINTEGER*)szBuffer; // Does this work on PPC or do we need a union?
#if PY_MAJOR_VERSION >= 3
        result = PyLong_FromLong((long)n);
#else
        if (n <= (SQLUINTEGER)PyInt_GetMax())
            result = PyInt_FromLong((long)n);
        else
            result = PyLong_FromUnsignedLong(n);
#endif
        break;
    }
    
    case GI_USMALLINT:
        result = PyInt_FromLong(*(SQLUSMALLINT*)szBuffer);
        break;
    }

    return result;
}

PyObject* Connection_endtrans(Connection* cnxn, SQLSMALLINT type)
{
    // If called from Cursor.commit, it is possible that `cnxn` is deleted by another thread when we release them
    // below.  (The cursor has had its reference incremented by the method it is calling, but nothing has incremented
    // the connections count.  We could, but we really only need the HDBC.)
    HDBC hdbc = cnxn->hdbc;

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLEndTran(SQL_HANDLE_DBC, hdbc, type);
    Py_END_ALLOW_THREADS

    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle("SQLEndTran", hdbc, SQL_NULL_HANDLE);
        return 0;
    }

    Py_RETURN_NONE;
}

static PyObject* Connection_commit(PyObject* self, PyObject* args)
{
    UNUSED(args);
    
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;
    
    TRACE("commit: cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

    return Connection_endtrans(cnxn, SQL_COMMIT);
}

static PyObject* Connection_rollback(PyObject* self, PyObject* args)
{
    UNUSED(args);
    
    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;
    
    TRACE("rollback: cnxn=%p hdbc=%d\n", cnxn, cnxn->hdbc);

    return Connection_endtrans(cnxn, SQL_ROLLBACK);
}

static char cursor_doc[] = 
    "Return a new Cursor object using the connection.";
    
static char execute_doc[] =
    "execute(sql, [params]) --> Cursor\n"
    "\n"
    "Create a new Cursor object, call its execute method, and return it.  See\n"
    "Cursor.execute for more details.\n"
    "\n"
    "This is a convenience method that is not part of the DB API.  Since a new\n"
    "Cursor is allocated by each call, this should not be used if more than one SQL\n"
    "statement needs to be executed.";

static char commit_doc[] =
    "Commit any pending transaction to the database.";

static char rollback_doc[] =
    "Causes the the database to roll back to the start of any pending transaction.";

static char getinfo_doc[] =
    "getinfo(type) --> str | int | bool\n"
    "\n"
    "Calls SQLGetInfo, passing `type`, and returns the result formatted as a Python object.";


PyObject* Connection_getautocommit(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    PyObject* result = (cnxn->nAutoCommit == SQL_AUTOCOMMIT_ON) ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

static int Connection_setautocommit(PyObject* self, PyObject* value, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return -1;

    if (value == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the autocommit attribute.");
        return -1;
    }
    
    uintptr_t nAutoCommit = PyObject_IsTrue(value) ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)nAutoCommit, SQL_IS_UINTEGER);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle("SQLSetConnectAttr", cnxn->hdbc, SQL_NULL_HANDLE);
        return -1;
    }

    cnxn->nAutoCommit = nAutoCommit;

    return 0;
}


static PyObject* Connection_getsearchescape(PyObject* self, void* closure)
{
    UNUSED(closure);
    
    Connection* cnxn = (Connection*)self;

    if (!cnxn->searchescape)
    {
        char sz[8] = { 0 };
        SQLSMALLINT cch = 0;

        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLGetInfo(cnxn->hdbc, SQL_SEARCH_PATTERN_ESCAPE, &sz, _countof(sz), &cch);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            return RaiseErrorFromHandle("SQLGetInfo", cnxn->hdbc, SQL_NULL_HANDLE);

        cnxn->searchescape = PyString_FromStringAndSize(sz, (Py_ssize_t)cch);
    }

    Py_INCREF(cnxn->searchescape);
    return cnxn->searchescape;
}


static PyObject* Connection_gettimeout(PyObject* self, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return 0;

    return PyInt_FromLong(cnxn->timeout);
}

static int Connection_settimeout(PyObject* self, PyObject* value, void* closure)
{
    UNUSED(closure);

    Connection* cnxn = Connection_Validate(self);
    if (!cnxn)
        return -1;

    if (value == 0)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the timeout attribute.");
        return -1;
    }
    intptr_t timeout = PyInt_AsLong(value);
    if (timeout == -1 && PyErr_Occurred())
        return -1;
    if (timeout < 0)
    {
        PyErr_SetString(PyExc_ValueError, "Cannot set a negative timeout.");
        return -1;
    }

    SQLRETURN ret;
    Py_BEGIN_ALLOW_THREADS
    ret = SQLSetConnectAttr(cnxn->hdbc, SQL_ATTR_CONNECTION_TIMEOUT, (SQLPOINTER)timeout, SQL_IS_UINTEGER);
    Py_END_ALLOW_THREADS
    if (!SQL_SUCCEEDED(ret))
    {
        RaiseErrorFromHandle("SQLSetConnectAttr", cnxn->hdbc, SQL_NULL_HANDLE);
        return -1;
    }

    cnxn->timeout = timeout;

    return 0;
}

static bool _add_converter(PyObject* self, SQLSMALLINT sqltype, PyObject* func)
{
    Connection* cnxn = (Connection*)self;

    if (cnxn->conv_count)
    {
        // If the sqltype is already registered, replace the old conversion function with the new.
        for (int i = 0; i < cnxn->conv_count; i++)
        {
            if (cnxn->conv_types[i] == sqltype)
            {
                Py_XDECREF(cnxn->conv_funcs[i]);
                cnxn->conv_funcs[i] = func;
                Py_INCREF(func);
                return true;
            }
        }
    }
    
    int          oldcount = cnxn->conv_count;
    SQLSMALLINT* oldtypes = cnxn->conv_types;
    PyObject**   oldfuncs = cnxn->conv_funcs;

    int          newcount = oldcount + 1;
    SQLSMALLINT* newtypes = (SQLSMALLINT*)pyodbc_malloc(sizeof(SQLSMALLINT) * newcount);
    PyObject**   newfuncs = (PyObject**)pyodbc_malloc(sizeof(PyObject*) * newcount);

    if (newtypes == 0 || newfuncs == 0)
    {
        if (newtypes)
            pyodbc_free(newtypes);
        if (newfuncs)
            pyodbc_free(newfuncs);
        PyErr_NoMemory();
        return false;
    }

    newtypes[0] = sqltype;
    newfuncs[0] = func;
    Py_INCREF(func);

    cnxn->conv_count = newcount;
    cnxn->conv_types = newtypes;
    cnxn->conv_funcs = newfuncs;

    if (oldcount != 0)
    {
        // copy old items
        memcpy(&newtypes[1], oldtypes, sizeof(int) * oldcount);
        memcpy(&newfuncs[1], oldfuncs, sizeof(PyObject*) * oldcount);

        pyodbc_free(oldtypes);
        pyodbc_free(oldfuncs);
    }

    return true;
}

static char conv_add_doc[] =
    "add_output_converter(sqltype, func) --> None\n"
    "\n"
    "Register an output converter function that will be called whenever a value with\n"
    "the given SQL type is read from the database.\n"
    "\n"
    "sqltype\n"
    "  The integer SQL type value to convert, which can be one of the defined\n"
    "  standard constants (e.g. pyodbc.SQL_VARCHAR) or a database-specific value\n"
    "  (e.g. -151 for the SQL Server 2008 geometry data type).\n"
    "\n"
    "func\n"
    "  The converter function which will be called with a single parameter, the\n"
    "  value, and should return the converted value.  If the value is NULL, the\n"
    "  parameter will be None.  Otherwise it will be a Python string.";


static PyObject* Connection_conv_add(PyObject* self, PyObject* args)
{
    int sqltype;
    PyObject* func;
    if (!PyArg_ParseTuple(args, "iO", &sqltype, &func))
        return 0;

    if (!_add_converter(self, (SQLSMALLINT)sqltype, func))
        return 0;

    Py_RETURN_NONE;
}

static char enter_doc[] = "__enter__() -> self.";
static PyObject* Connection_enter(PyObject* self, PyObject* args)
{
    UNUSED(args);
    Py_INCREF(self);
    return self;
}

static char exit_doc[] = "__exit__(*excinfo) -> None.  Commits the connection if necessary.";
static PyObject* Connection_exit(PyObject* self, PyObject* args)
{
    Connection* cnxn = (Connection*)self;

    // If an error has occurred, `args` will be a tuple of 3 values.  Otherwise it will be a tuple of 3 `None`s.
    I(PyTuple_Check(args));

    if (cnxn->nAutoCommit == SQL_AUTOCOMMIT_OFF && PyTuple_GetItem(args, 0) == Py_None)
    {
        SQLRETURN ret;
        Py_BEGIN_ALLOW_THREADS
        ret = SQLEndTran(SQL_HANDLE_DBC, cnxn->hdbc, SQL_COMMIT);
        Py_END_ALLOW_THREADS

        if (!SQL_SUCCEEDED(ret))
            return RaiseErrorFromHandle("SQLEndTran(SQL_COMMIT)", cnxn->hdbc, SQL_NULL_HANDLE);
    }
    
    Py_RETURN_NONE;
}


static struct PyMethodDef Connection_methods[] =
{
    { "cursor",                  Connection_cursor,          METH_NOARGS,  cursor_doc     },
    { "close",                   Connection_close,           METH_NOARGS,  close_doc      },
    { "execute",                 Connection_execute,         METH_VARARGS, execute_doc    },
    { "commit",                  Connection_commit,          METH_NOARGS,  commit_doc     },
    { "rollback",                Connection_rollback,        METH_NOARGS,  rollback_doc   },
    { "getinfo",                 Connection_getinfo,         METH_VARARGS, getinfo_doc    },
    { "add_output_converter",    Connection_conv_add,        METH_VARARGS, conv_add_doc   },
    { "clear_output_converters", Connection_conv_clear,      METH_NOARGS,  conv_clear_doc },
    { "__enter__",               Connection_enter,           METH_NOARGS,  enter_doc      },
    { "__exit__",                Connection_exit,            METH_VARARGS, exit_doc       },
    
    { 0, 0, 0, 0 }
};

static PyGetSetDef Connection_getseters[] = {
    { "searchescape", (getter)Connection_getsearchescape, 0,
        "The ODBC search pattern escape character, as returned by\n"
        "SQLGetInfo(SQL_SEARCH_PATTERN_ESCAPE).  These are driver specific.", 0 },
    { "autocommit", Connection_getautocommit, Connection_setautocommit,
      "Returns True if the connection is in autocommit mode; False otherwise.", 0 },
    { "timeout", Connection_gettimeout, Connection_settimeout,
      "The timeout in seconds, zero means no timeout.", 0 },
    { 0 }
};

PyTypeObject ConnectionType =
{
    PyVarObject_HEAD_INIT(0, 0)
    "pyodbc.Connection",        // tp_name
    sizeof(Connection),         // tp_basicsize
    0,                          // tp_itemsize
    Connection_dealloc,         // destructor tp_dealloc
    0,                          // tp_print
    0,                          // tp_getattr
    0,                          // tp_setattr
    0,                          // tp_compare
    0,                          // tp_repr
    0,                          // tp_as_number
    0,                          // tp_as_sequence
    0,                          // tp_as_mapping
    0,                          // tp_hash
    0,                          // tp_call
    0,                          // tp_str
    0,                          // tp_getattro
    0,                          // tp_setattro
    0,                          // tp_as_buffer
    Py_TPFLAGS_DEFAULT,         // tp_flags
    connection_doc,             // tp_doc
    0,                          // tp_traverse
    0,                          // tp_clear
    0,                          // tp_richcompare
    0,                          // tp_weaklistoffset
    0,                          // tp_iter
    0,                          // tp_iternext
    Connection_methods,         // tp_methods
    0,                          // tp_members
    Connection_getseters,       // tp_getset
    0,                          // tp_base
    0,                          // tp_dict
    0,                          // tp_descr_get
    0,                          // tp_descr_set
    0,                          // tp_dictoffset
    0,                          // tp_init
    0,                          // tp_alloc
    0,                          // tp_new
    0,                          // tp_free
    0,                          // tp_is_gc
    0,                          // tp_bases
    0,                          // tp_mro
    0,                          // tp_cache
    0,                          // tp_subclasses
    0,                          // tp_weaklist
};
