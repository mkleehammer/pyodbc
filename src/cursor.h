
/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef CURSOR_H
#define CURSOR_H

struct Connection;

struct ColumnInfo
{
    SQLSMALLINT sql_type;

    // The column size from SQLDescribeCol.  For character types, this is the maximum length, not including the NULL
    // terminator.  For binary values, this is the maximum length.  For numeric and decimal values, it is the defined
    // number of digits. For example, the precision of a column defined as NUMERIC(10,3) is 10.
    //
    // This value can be SQL_NO_TOTAL in which case the driver doesn't know the maximum length, such as for LONGVARCHAR
    // fields.
    SQLULEN column_size;

    // Tells us if an integer type is signed or unsigned.  This is determined after a query using SQLColAttribute.  All
    // of the integer types are the same size whether signed and unsigned, so we can allocate memory ahead of time
    // without knowing this.  We use this during the fetch when converting to a Python integer or long.
    bool is_unsigned;
};

struct ParamInfo
{
    // Number of parameter sets for array binding. This will be set to 1 for non-array binding.
    Py_ssize_t  ParamSetSize;

    // The following correspond to the SQLBindParameter parameters.
    SQLSMALLINT ValueType;
    SQLSMALLINT ParameterType;
    SQLULEN     ColumnSize;
    SQLSMALLINT DecimalDigits;
    SQLPOINTER  ParameterValuePtr;
    SQLLEN      BufferLength;
    SQLLEN*     StrLen_or_IndPtr;

    // Python objects this parameter is bound to.
    PyObject**  ParameterObjects;

    // Default buffers. For non-array binding of many values, these buffers provide enough storage to avoid extra
    // allocations. Pointer values above are initialized to point at these buffers.
    SQLCHAR     ParameterValueBuffer[64];
    SQLLEN      StrLen_or_IndBuffer[1];
    PyObject*   ParameterObjectBuffer[1];

    // Flag to indicate that ParameterValuePtr points into data owned by the Python object.
    bool        IsParameterValueBorrowed;
};

struct Cursor
{
    PyObject_HEAD

        // The Connection object (which is a PyObject) that created this cursor.
        Connection* cnxn;

    // Set to SQL_NULL_HANDLE when the cursor is closed.
    HSTMT hstmt;

    //
    // SQL Parameters
    //

    // If non-zero, a pointer to the previously prepared SQL string, allowing us to skip the prepare and gathering of
    // parameter data.
    PyObject* pPreparedSQL;

    // The number of parameter markers in pPreparedSQL.  This will be zero when pPreparedSQL is zero but is set
    // immediately after preparing the SQL.
    int paramcount;

    // If non-zero, a pointer to an array of SQL type values allocated via malloc.  This is zero until we actually ask
    // for the type of parameter, which is only when a parameter is None (NULL).  At that point, the entire array is
    // allocated (length == paramcount) but all entries are set to SQL_UNKNOWN_TYPE.
    SQLSMALLINT* paramtypes;

    // If non-zero, a pointer to a buffer containing the actual parameters bound.  If pPreparedSQL is zero, this should
    // be freed using free and set to zero.
    //
    // Even if the same SQL statement is executed twice, the parameter bindings are redone from scratch since we try to
    // bind into the Python objects directly.
    ParamInfo* paramInfos;
    Py_ssize_t paramSetSize;

    //
    // Result Information
    //

    // An array of ColumnInfos, allocated via malloc.  This will be zero when closed or when there are no query
    // results.
    ColumnInfo* colinfos;

    // The description tuple described in the DB API 2.0 specification.  Set to None when there are no results.
    PyObject* description;

    int arraysize;

    // The Cursor.rowcount attribute from the DB API specification.
    int rowcount;

    // A dictionary that maps from column name (PyString) to index into the result columns (PyInteger).  This is
    // constructued during an execute and shared with each row (reference counted) to implement accessing results by
    // column name.
    //
    // This duplicates some ODBC functionality, but allows us to use Row objects after the statement is closed and
    // should use less memory than putting each column into the Row's __dict__.
    //
    // Since this is shared by Row objects, it cannot be reused.  New dictionaries are created for every execute.  This
    // will be zero whenever there are no results.
    PyObject* map_name_to_index;
};

void Cursor_init();

Cursor* Cursor_New(Connection* cnxn);
PyObject* Cursor_execute(PyObject* self, PyObject* args);

#endif
