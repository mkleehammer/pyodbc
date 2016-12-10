
# Connection

Connection objects manage connections to the database.

Each manages a single ODBC HDBC.

There is no constructor -- connections can only be created by the connect function.

## variables

### autocommit

True if the connection is in autocommit mode; False otherwise.  Changing this value updates the
ODBC autocommit setting.  The default is False.

### searchescape

The ODBC search pattern escape character, as returned by SQLGetInfo(SQL_SEARCH_PATTERN_ESCAPE),
used to escape special characters such as '%' and '_'.  These are driver specific.

### timeout

An optional integer query timeout, in seconds.  Use zero, the default, to disable.

The timeout is applied to all cursors created by the connection, so it cannot be changed for a
given connection.

If a query timeout occurs, the database should raise an OperationalError with SQLSTATE HYT00 or
HYT01.

Note: This attribute only affects queries.  To set the timeout for the actual connection
process, use the `timeout` keyword of the `connect` function.
        
## cursor

    cnxn.cursor() --> Cursor

Returns a new Cursor Object using the connection.

pyodbc supports multiple cursors per connection but your database or driver may not.  Check
your documentation.

## commit

    cnxn.commit()

Commits any pending transaction to the database.

Pending transactions are automatically rolled back when a connection is closed, so be sure to
call this.

## rollback

    cnxn.rollback()

Causes the the database to roll back to the start of any pending transaction.

You can call this even if no work has been performed on the cursor, allowing it to be used in
finally statements, etc.

## close

    cnxn.close()

Closes the connection.  Connections are automatically closed when they are deleted, but you
should call this if the connection is referenced in more than one place.

The connection will be unusable from this point forward and a ProgrammingError will be raised
if any operation is attempted with the connection.  The same applies to all cursor objects
trying to use the connection.

Note that closing a connection without committing the changes first will cause an implicit
rollback to be performed.

## getinfo

    getinfo(type) --> str | int | bool

    cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_PROCEDURES)

Returns general information about the driver and data source associated with a connection by
calling [SQLGetInfo](http://msdn.microsoft.com/en-us/library/ms711681%28VS.85%29.aspx) and
returning its results.  See Microsoft's SQLGetInfo documentation for the types of information
available.

pyodbc provides constants for the supported types.

type | result type
---- | ------------
SQL_ACCESSIBLE_PROCEDURES | bool
SQL_ACCESSIBLE_TABLES | bool
SQL_ACTIVE_ENVIRONMENTS | int
SQL_AGGREGATE_FUNCTIONS | int
SQL_ALTER_DOMAIN | int
SQL_ALTER_TABLE | int
SQL_ASYNC_MODE | int
SQL_BATCH_ROW_COUNT | int
SQL_BATCH_SUPPORT | int
SQL_BOOKMARK_PERSISTENCE | int
SQL_CATALOG_LOCATION | int
SQL_CATALOG_NAME | bool
SQL_CATALOG_NAME_SEPARATOR | str
SQL_CATALOG_TERM | str
SQL_CATALOG_USAGE | int
SQL_COLLATION_SEQ | str
SQL_COLUMN_ALIAS | bool
SQL_CONCAT_NULL_BEHAVIOR | int
SQL_CONVERT_FUNCTIONS | int
SQL_CONVERT_VARCHAR | int
SQL_CORRELATION_NAME | int
SQL_CREATE_ASSERTION | int
SQL_CREATE_CHARACTER_SET | int
SQL_CREATE_COLLATION | int
SQL_CREATE_DOMAIN | int
SQL_CREATE_SCHEMA | int
SQL_CREATE_TABLE | int
SQL_CREATE_TRANSLATION | int
SQL_CREATE_VIEW | int
SQL_CURSOR_COMMIT_BEHAVIOR | int
SQL_CURSOR_ROLLBACK_BEHAVIOR | int
SQL_DATABASE_NAME | str
SQL_DATA_SOURCE_NAME | str
SQL_DATA_SOURCE_READ_ONLY | bool
SQL_DATETIME_LITERALS | int
SQL_DBMS_NAME | str
SQL_DBMS_VER | str
SQL_DDL_INDEX | int
SQL_DEFAULT_TXN_ISOLATION | int
SQL_DESCRIBE_PARAMETER | bool
SQL_DM_VER | str
SQL_DRIVER_NAME | str
SQL_DRIVER_ODBC_VER | str
SQL_DRIVER_VER | str
SQL_DROP_ASSERTION | int
SQL_DROP_CHARACTER_SET | int
SQL_DROP_COLLATION | int
SQL_DROP_DOMAIN | int
SQL_DROP_SCHEMA | int
SQL_DROP_TABLE | int
SQL_DROP_TRANSLATION | int
SQL_DROP_VIEW | int
SQL_DYNAMIC_CURSOR_ATTRIBUTES1 | int
SQL_DYNAMIC_CURSOR_ATTRIBUTES2 | int
SQL_EXPRESSIONS_IN_ORDERBY | bool
SQL_FILE_USAGE | int
SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1 | int
SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2 | int
SQL_GETDATA_EXTENSIONS | int
SQL_GROUP_BY | int
SQL_IDENTIFIER_CASE | int
SQL_IDENTIFIER_QUOTE_CHAR | str
SQL_INDEX_KEYWORDS | int
SQL_INFO_SCHEMA_VIEWS | int
SQL_INSERT_STATEMENT | int
SQL_INTEGRITY | bool
SQL_KEYSET_CURSOR_ATTRIBUTES1 | int
SQL_KEYSET_CURSOR_ATTRIBUTES2 | int
SQL_KEYWORDS | str
SQL_LIKE_ESCAPE_CLAUSE | bool
SQL_MAX_ASYNC_CONCURRENT_STATEMENTS | int
SQL_MAX_BINARY_LITERAL_LEN | int
SQL_MAX_CATALOG_NAME_LEN | int
SQL_MAX_CHAR_LITERAL_LEN | int
SQL_MAX_COLUMNS_IN_GROUP_BY | int
SQL_MAX_COLUMNS_IN_INDEX | int
SQL_MAX_COLUMNS_IN_ORDER_BY | int
SQL_MAX_COLUMNS_IN_SELECT | int
SQL_MAX_COLUMNS_IN_TABLE | int
SQL_MAX_COLUMN_NAME_LEN | int
SQL_MAX_CONCURRENT_ACTIVITIES | int
SQL_MAX_CURSOR_NAME_LEN | int
SQL_MAX_DRIVER_CONNECTIONS | int
SQL_MAX_IDENTIFIER_LEN | int
SQL_MAX_INDEX_SIZE | int
SQL_MAX_PROCEDURE_NAME_LEN | int
SQL_MAX_ROW_SIZE | int
SQL_MAX_ROW_SIZE_INCLUDES_LONG | bool
SQL_MAX_SCHEMA_NAME_LEN | int
SQL_MAX_STATEMENT_LEN | int
SQL_MAX_TABLES_IN_SELECT | int
SQL_MAX_TABLE_NAME_LEN | int
SQL_MAX_USER_NAME_LEN | int
SQL_MULTIPLE_ACTIVE_TXN | bool
SQL_MULT_RESULT_SETS | bool
SQL_NEED_LONG_DATA_LEN | bool
SQL_NON_NULLABLE_COLUMNS | int
SQL_NULL_COLLATION | int
SQL_NUMERIC_FUNCTIONS | int
SQL_ODBC_INTERFACE_CONFORMANCE | int
SQL_ODBC_VER | str
SQL_OJ_CAPABILITIES | int
SQL_ORDER_BY_COLUMNS_IN_SELECT | bool
SQL_PARAM_ARRAY_ROW_COUNTS | int
SQL_PARAM_ARRAY_SELECTS | int
SQL_PROCEDURES | bool
SQL_PROCEDURE_TERM | str
SQL_QUOTED_IDENTIFIER_CASE | int
SQL_ROW_UPDATES | bool
SQL_SCHEMA_TERM | str
SQL_SCHEMA_USAGE | int
SQL_SCROLL_OPTIONS | int
SQL_SEARCH_PATTERN_ESCAPE | str
SQL_SERVER_NAME | str
SQL_SPECIAL_CHARACTERS | str
SQL_SQL92_DATETIME_FUNCTIONS | int
SQL_SQL92_FOREIGN_KEY_DELETE_RULE | int
SQL_SQL92_FOREIGN_KEY_UPDATE_RULE | int
SQL_SQL92_GRANT | int
SQL_SQL92_NUMERIC_VALUE_FUNCTIONS | int
SQL_SQL92_PREDICATES | int
SQL_SQL92_RELATIONAL_JOIN_OPERATORS | int
SQL_SQL92_REVOKE | int
SQL_SQL92_ROW_VALUE_CONSTRUCTOR | int
SQL_SQL92_STR_FUNCTIONS | int
SQL_SQL92_VALUE_EXPRESSIONS | int
SQL_SQL_CONFORMANCE | int
SQL_STANDARD_CLI_CONFORMANCE | int
SQL_STATIC_CURSOR_ATTRIBUTES1 | int
SQL_STATIC_CURSOR_ATTRIBUTES2 | int
SQL_STR_FUNCTIONS | int
SQL_SUBQUERIES | int
SQL_SYSTEM_FUNCTIONS | int
SQL_TABLE_TERM | str
SQL_TIMEDATE_ADD_INTERVALS | int
SQL_TIMEDATE_DIFF_INTERVALS | int
SQL_TIMEDATE_FUNCTIONS | int
SQL_TXN_CAPABLE | int
SQL_TXN_ISOLATION_OPTION | int
SQL_UNION | int
SQL_USER_NAME | str
SQL_XOPEN_CLI_YEAR | str

## execute

    execute(sql, *params) --> Cursor

This is just a convenience function for creating a new Cursor, executing the SQL using the
cursor, then discarding the cursor.  Since a new Cursor is allocated by each call, this should
not be used if more than one SQL statement needs to be executed.

## setencoding

    # Python 2
    cnxn.setencoding(type, encoding=None, ctype=None)

    # Python 3
    cnxn.setencoding(encoding=None, ctype=None)

Sets the text encoding for SQL statements and text parameters.

### type

The text type to configure.  In Python 2 there are two text types: `str` and `unicode` which
can be configured indivually.  Python 3 only has `str` (which is Unicode), so the parameter is
not needed.


### encoding

The encoding to use.  This must be a valid Python encoding that converts text to `bytes`
(Python 3) or `str` (Python 2).

### ctype

The C data type to use when passing data: `pyodbc.SQL_CHAR` or `pyodbc.SQL_WCHAR`.

If not provided, `SQL_WCHAR` is used for "utf-16", "utf-16le", and "utf-16be".  `SQL_CHAR` is
used for all other encodings.

The defaults are:

Python version | type | encoding | ctype
-------------- | ---- | -------- | -----
Python 2 | str | utf-8 | SQL_CHAR
Python 2 | unicode | utf-16le | SQL_WCHAR
Python 3 | unicode | utf-16le | SQL_WCHAR

If your database driver communicates with only UTF-8 (often MySQL and PostgreSQL), try the
following:

      # Python 2
      cnxn.setencoding(str, encoding='utf-8')
      cnxn.setencoding(unicode, encoding='utf-8')

      # Python 3
      cnxn.setencoding(encoding='utf-8')

In Python 2.7, the value "raw" can be used as special encoding for `str` objects.  This will
pass the string object's bytes as-is to the database.  This is not recommended as you need to
make sure that the internal format matches what the database expects.

## setdecoding

      # Python 2
      cnxn.setdecoding(sqltype, encoding=None, ctype=None, to=None)

      # Python 3
      cnxn.setdecoding(sqltype, encoding=None, ctype=None)

Sets the text decoding used when reading `SQL_CHAR` and `SQL_WCHAR` from the database.

### sqltype

The SQL type being configured: `pyodbc.SQL_CHAR` or `pyodbc.SQL_WCHAR`.

### encoding

The Python encoding to use when decoding the data.

### ctype

The C data type to request from SQLGetData: `pyodbc.SQL_CHAR` or `pyodbc.SQL_WCHAR`.

### to

The Python 2 text data type to be returned: `str` or `unicode`.  If not provided (recommended),
whatever type the codec returns is returned.  (This parameter is not needed in Python 3 because
the only text data type is `str`.)

The defaults are:

Python version | type | encoding | ctype
-------------- | ---- | -------- | -----
Python 2 | str | utf-8 | SQL_CHAR
Python 2 | unicode | utf-16le | SQL_WCHAR
Python 3 | unicode | utf-16le | SQL_WCHAR

In Python 2.7, the value "raw" can be used as special encoding for `SQL_CHAR` values.  This
will create a `str` object directly from the bytes from the database with no conversion.string
object's bytes as-is to the database.  This is not recommended as you need to make sure that
the internal format matches what the database sends.

## add_output_converter

    add_output_converter(sqltype, func)

Register an output converter function that will be called whenever a value with
the given SQL type is read from the database.

### sqltype

The integer SQL type value to convert, which can be one of the defined standard constants
(e.g. pyodbc.SQL_VARCHAR) or a database-specific value (e.g. -151 for the SQL Server 2008
geometry data type).

### func

The converter function which will be called with a single parameter, the value, and should
return the converted value.  If the value is NULL, the parameter will be None.  Otherwise it
will be a Python string.

## clear_output_converters

    clear_output_converters()

Remove all output converter functions added by `add_output_converter`.

## set_attr

    set_attr(attr_id, value)

Calls SQLSetConnectAttr with the given values.

### attr_id

The attribute ID (integer) to set.  These are ODBC or driver constants.  The ODBC constants can
usually be found in the sql.h or sqlext.h file.

### value

The connection attribute value to set.  At this time only integer values are supported.
