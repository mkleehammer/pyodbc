
# Cursor

Cursors represent a database cursor (and map to ODBC HSTMTs), which is used to manage the
context of a fetch operation.  Cursors created from the same connection are not isolated, i.e.,
any changes made to the database by a cursor are immediately visible by the other cursors.

## variables

### description

This read-only attribute is a list of 7-item tuples, each containing `(name, type_code,
display_size, internal_size, precision, scale, null_ok)`.  pyodbc only provides values for
name, type_code, internal_size, and null_ok.  The other values are set to None.

This attribute will be None for operations that do not return rows or if one of the execute
methods has not been called.

The type_code member is the class type used to create the Python objects when reading rows.
For example, a varchar column's type will be `str`.

### rowcount

The number of rows modified by the previous DDL statement.

This is -1 if no SQL has been executed or if the number of rows is unknown.  Note that it is
not uncommon for databases to report -1 after a select statement for performance reasons.  (The
exact number may not be known before the first records are returned to the application.)

## execute

    cursor.execute(sql, *parameters) --> Cursor

### sql

The SQL statement to execute with optional `?` parameter markers.  Note that pyodbc *never*
modifies the SQL statement.

### parameters

Optional parameters for the markers in the SQL.  They can be passed in a single sequence as
defined by the DB API.  For convenience, however, they can also be passed individually.

    # as a sequence
    cursor.execute("select a from tbl where b=? and c=?", <b>(x, y))

    # passed individually
    cursor.execute("select a from tbl where b=? and c=?", <b>x, y)

You should use parameters when possible instead of inserting them directly into the SQL to
protect against [SQL injection attacks](http://en.wikipedia.org/wiki/SQL_injection).
Parameters are passed to the database *separately* using an ODBC parameter binding specifically
designed for this.

The return value is always the cursor itself which allows chaining.

    for row in cursor.execute("select user_id, user_name from users"):
        print()row.user_id, row.user_name)

    row  = cursor.execute("select * from tmp").fetchone()
    rows = cursor.execute("select * from tmp").fetchall()
    val  = cursor.execute("select count(*) from tmp").fetchval()

    count = cursor.execute("update users set last_logon=? where user_id=?", now, user_id).rowcount
    count = cursor.execute("delete from users where user_id=1").rowcount

The last prepared statement is kept and reused if you execute the same SQL again, making
executing the same SQL with different parameters will be more efficient.

## executemany

    cursor.executemany(sql, seq_of_parameters) --> Cursor

Executes the same SQL statement for each set of parameters.

### seq_of_parameters

a sequence of sequences

    params = [ ('A', 1), ('B', 2) ]
    executemany("insert into t(name, id) values (?, ?)", params)

This will execute the SQL statement twice, once with `('A', 1)` and once with `('B', 2)`.

## fetchval

    cursor.fetchval() --> value or None

Returns the first column value from the next row or None if no more rows are available:

    cursor.execute("select count(*) from users")
    c = cursor.fetchval()

## fetchone

    cursor.fetchone() --> Row or None

Returns the next Row or `None` if no more data is available.

A ProgrammingError exception is raised if no SQL has been executed or if it did not return a
result set (e.g. was not a SELECT statement).

    cursor.execute("select user_name from users where user_id=?", userid)
    row = cursor.fetchone()
    if row:
        print(row.user_name)

## fetchall

    cursor.fetchall() --> list of rows

Returns a list of all remaining Rows.  If there are no more rows, an empty list is returned.

Since this reads all rows into memory, it should not be used if there are a lot of rows.
Consider iterating over the rows instead.  However, it is useful for freeing up a Cursor so you
can perform a second query before processing the resulting rows.

A ProgrammingError exception is raised if no SQL has been executed or if it did not return a
result set (e.g. was not a SELECT statement).

    cursor.execute("select user_id, user_name from users where user_id < 100")
    rows = cursor.fetchall()
    for row in rows:
        print(row.user_id, row.user_name)

## fetchmany

    cursor.fetchmany([size=cursor.arraysize]) --> list of rows

Similar to fetchall but limits the returned list size to `size`.  The default for
cursor.arraysize is 1 which is no different than calling fetchone.

A ProgrammingError exception is raised if no SQL has been executed or if it did not return a
result set (e.g. was not a SELECT statement).

## commit

    cursor.commit()

A convenience function that calls commit on the Connection that created this cursor.  Since
`cursor` and `commit` are usually the only two functions called on a connection, this function
often allows you to work with cursors only.

**Warning:** This affects all cursors created by the same connection.

## rollback

    cursor.rollback()

A convenience function that calls commit on the Connection that created this cursor.

**Warning:** This affects all cursors created by the same connection.

## skip

    cursor.skip(count)

Skips the next `count` records by calling SQLFetchScroll with SQL_FETCH_NEXT.

For convenience, `skip(0)` is accepted and will do nothing.

## nextset

    cursor.nextset() --> True or None

This method will make the cursor skip to the next available set, discarding any remaining rows
from the current set.

If there are no more sets, the method returns `None`. Otherwise, it returns a true value and
subsequent calls to the fetch methods will return rows from the next result set.

This method is useful if you have stored procedures that return multiple results.  If your
database supports it, you may be able to send multiple SQL statements in a single batch and use
this method to move through each result set.

    cursor.execute("select * from users; select * from albumts")
    users = cursor.fetchall()
    cursor.nextset()
    albums = cursor.fetchall()

## close

    cursor.close()

Closes the cursor.  A ProgrammingError exception will be raised if any
operation is attempted with the cursor.

Cursors are closed automatically when they are deleted, so calling this is
not usually necessary when using C Python.

## callproc

    cursor.callproc(procname[,parameters])

This is not yet supported since there is no way for pyodbc to determine which
parameters are input, output, or both.

You will need to call stored procedures using execute().  You can use your
database's format or the ODBC escape format.

## tables

    cursor.tables(table=None, catalog=None, schema=None, tableType=None) --> Cursor

Creates a result set of tables in the database that match the given criteria.

Each row has the following columns.  See the
[SQLTables](http://msdn.microsoft.com/en-us/library/ms711831.aspx) documentation for more
information.

column | notes
------ | -----
table_cat | The catalog name.
table_schem | The schema name.
table_name | The table name.
table_type | One of TABLE, VIEW, SYSTEM TABLE, GLOBAL TEMPORARY, LOCAL TEMPORARY, ALIAS, SYNONYM, or a data source-specific type name.
remarks | A description of the table.
    
    for row in cursor.tables():
        print(row.table_name)

    # Does table 'x' exist?
    if cursor.tables(table='x').fetchone():
        print('yes it does')

The table, catalog, and schema interpret the '_' and '%' characters as wildcards.  The escape
character is driver specific, so use Connection.searchescape.

## columns

    cursor.columns(table=None, catalog=None, schema=None, column=None) --> Cursor

Creates a result set of column information in the specified tables using
the [SQLColumns](http://msdn.microsoft.com/en-us/library/ms711683%28VS.85%29.aspx) function.

column | notes
------ | -----
table_cat | 
table_schem | 
table_name | 
column_name | 
data_type | 
type_name | 
column_size | 
buffer_length | 
decimal_digits | 
num_prec_radix | 
nullable | 
remarks | 
column_def | 
sql_data_type | 
sql_datetime_sub | 
char_octet_length | 
ordinal_position | 
is_nullable | One of the pyodbc constants SQL_NULLABLE, SQL_NO_NULLS, SQL_NULLS_UNKNOWN.

    # columns in table x
    for row in cursor.columns(table='x'):
        print(row.column_name, 'is nullable?', row.is_nullable == pyodbc.SQL_NULLABLE)

## statistics

    cursor.statistics(table, catalog=None, schema=None, unique=False, quick=True) --> Cursor

Creates a result set of statistics about a single table and the indexes associated with the
table by
executing [SQLStatistics](http://msdn.microsoft.com/en-us/library/ms711022%28VS.85%29.aspx).

### unique

If True only unique indexes are returned.  If False all indexes are returned.

### quick

If True values for CARDINALITY and PAGES are returned only if they are readily available.
Otherwise `None` is returned for those columns.

Each row has the following columns:

| column
| ------
| table_cat
| table_schem
| table_name
| non_unique
| index_qualifier
| index_name
| type
| ordinal_position
| column_name
| asc_or_desc
| cardinality
| pages
| filter_condition

## rowIdColumns

    cursor.rowIdColumns(table, catalog=None, schema=None, nullable=True) --> Cursor

Executes [SQLSpecialColumns](http://msdn.microsoft.com/en-us/library/ms714602%28VS.85%29.aspx)
with `SQL_BEST_ROWID` which creates a result set of columns that uniquely identify a row.

Each row has the following columns.

column | notes
------ | -----
scope | One of SQL_SCOPE_CURROW, SQL_SCOPE_TRANSACTION, or SQL_SCOPE_SESSION
column_name |
data_type | The ODBC SQL data type constant (e.g. SQL_CHAR)
type_name |
column_size |
buffer_length |
decimal_digits |
pseudo_column | One of SQL_PC_UNKNOWN, SQL_PC_NOT_PSEUDO, SQL_PC_PSEUDO

## rowVerColumns

    cursor.rowVerColumns(table, catalog=None, schema=None, nullable=True) --> Cursor

Executes [SQLSpecialColumns](http://msdn.microsoft.com/en-us/library/ms714602%28VS.85%29.aspx)
with `SQL_ROWVER` which creates a result set of columns that are automatically updated when any
value in the row is updated.  Each row has the following columns.

keyword | converts to
------- | -----------
scope | One of SQL_SCOPE_CURROW, SQL_SCOPE_TRANSACTION, or SQL_SCOPE_SESSION
column_name |
data_type | The ODBC SQL data type constant (e.g. SQL_CHAR)
type_name |
column_size |
buffer_length |
decimal_digits |
pseudo_column | One of SQL_PC_UNKNOWN, SQL_PC_NOT_PSEUDO, SQL_PC_PSEUDO

## primaryKeys

    cursor.primaryKeys(table, catalog=None, schema=None) --> Cursor

Creates a result set of column names that make up the primary key for a table by executing
the [SQLPrimaryKeys](http://msdn.microsoft.com/en-us/library/ms711005%28VS.85%29.aspx)
function.

Each row has the following columns:

| column
| ------
| table_cat
| table_schem
| table_name
| column_name
| key_seq
| pk_name

## foreignKeys

    cursor.foreignKeys(table=None, catalog=None, schema=None, foreignTable=None,
                       foreignCatalog=None, foreignSchema=None) --> Cursor

Executes the [SQLForeignKeys](http://msdn.microsoft.com/en-us/library/ms709315%28VS.85%29.aspx)
function and creates a result set of column names that are foreign keys in the specified table
(columns in the specified table that refer to primary keys in other tables) or foreign keys in
other tables that refer to the primary key in the specified table.

Each row has the following columns:

| column
| ------
| pktable_cat
| pktable_schem
| pktable_name
| pkcolumn_name
| fktable_cat
| fktable_schem
| fktable_name
| fkcolumn_name
| key_seq
| update_rule
| delete_rule
| fk_name
| pk_name
| deferrability


## procedures

    cursor.procedures(procedure=None, catalog=None, schema=None) --> Cursor

Executes <a href="http://msdn.microsoft.com/en-us/library/ms715368%28VS.85%29.aspx">SQLProcedures
and creates a result set of information about the procedures in the data source.  Each row has
the following columns:

| column
| ------
| procedure_cat
| procedure_schem
| procedure_name
| num_input_params
| num_output_params
| num_result_sets
| remarks
| procedure_type

## getTypeInfo

    cursor.getTypeInfo(sqlType=None) --> Cursor

Executes [SQLGetTypeInfo](http://msdn.microsoft.com/en-us/library/ms714632%28VS.85%29.aspx) a
creates a result set with information about the specified data type or all data types supported
by the ODBC driver if not specified.  Each row has the following columns:

| column
| 
| type_name
| data_type
| column_size
| literal_prefix
| literal_suffix
| create_params
| nullable
| case_sensitive
| searchable
| unsigned_attribute
| fixed_prec_scale
| auto_unique_value
| local_type_name
| minimum_scale
| maximum_scale
| sql_data_type
| sql_datetime_sub
| num_prec_radix
| interval_precision
