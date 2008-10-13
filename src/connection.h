
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef CONNECTION_H
#define CONNECTION_H

struct Cursor;

extern PyTypeObject ConnectionType;

struct Connection
{
    PyObject_HEAD

    // Set to SQL_NULL_HANDLE when the connection is closed.
	HDBC hdbc;

    // Will be SQL_AUTOCOMMIT_ON or SQL_AUTOCOMMIT_OFF.
    int nAutoCommit;

    // The ODBC version the driver supports, from SQLGetInfo(DRIVER_ODBC_VER).  This is set after connecting.
    char odbc_major;
    char odbc_minor;

    // The escape character from SQLGetInfo.  This is not initialized until requested, so this may be zero!
    PyObject* searchescape;

    // Will be true if SQLDescribeParam is supported.  If false, we'll have to guess but the user will not be able
    // to insert NULLs into binary columns.
    bool supports_describeparam;

    // The column size of datetime columns, obtained from SQLGetInfo(), used to determine the datetime precision.
    int datetime_precision;
};

#define Connection_Check(op) PyObject_TypeCheck(op, &ConnectionType)
#define Connection_CheckExact(op) ((op)->ob_type == &ConnectionType)

/*
 * Used by the module's connect function to create new connection objects.  If unable to connect to the database, an
 * exception is set and zero is returned.
 */
PyObject* Connection_New(PyObject* pConnectString, bool fAutoCommit, bool fAnsi);

#endif
