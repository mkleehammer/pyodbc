# pyodbc

pyodbc is an open source Python module that makes accessing ODBC databases simple.  It
implements the [DB API 2.0](https://www.python.org/dev/peps/pep-0249) specification but is
packed with even more Pythonic convenience.

* Getting Started
* API
  * [pyodbc](api-module.md)
  * [Connection](api-connection.md)
  * [Cursor](api-cursor.md)
  * [Row](api-row.md)
  * [Errors](api-errors.md)
* [Handling Unicode](unicode.md)
* [Releases](releases.md)

## Installing

The easiest way to install is using pip.  Windows and macOS binaries can often be downloaded by
pip but other operating systems will need to compile from source.

    pip install pyodbc

If pip needs to compile from source, you'll need to make sure you have the necessary build
packages installed on your system such as "python-devel", etc.  See
this
[Building From Source](https://github.com/mkleehammer/pyodbc/wiki/Building-pyodbc-from-source)
wiki page for help.

Development is on [GitHub](https://github.com/mkleehammer/pyodbc).

## Getting Started

### Connect to A Database

Pass an [ODBC connection string](https://github.com/mkleehammer/pyodbc/wiki) to
the [connect](api-module.md) function which will return a [Connection](api-connection.md).
Once you have a connection you can ask it for a [Cursor](api-cursor.md).  Several connection
examples are provided below.

    # Connection example: Windows, without a DSN, using the Windows SQL Server driver
    cnxn = pyodbc.connect('DRIVER={SQL Server};SERVER=localhost;PORT=1433;DATABASE=testdb;UID=me;PWD=pass')

    # Connection example: Linux, without a DSN, using the FreeTDS driver
    cnxn = pyodbc.connect('DRIVER={FreeTDS};SERVER=localhost;PORT=1433;DATABASE=testdb;UID=me;PWD=pass;TDS_Version=7.0')

    # Connection example: with a DSN
    cnxn = pyodbc.connect('DSN=test;PWD=password')

    # Opening a cursor
    cursor = cnxn.cursor()

There is no standard so the keywords depend on the driver you are using.

### Configure character set encodings

The default Unicode encoding is UTF-16LE.  See the [Unicode section](unicode.md) for tips on
configuring MySQL and PostgreSQL.

    # Python 2.7
    cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
    cnxn.setencoding(str, encoding='utf-8')
    cnxn.setencoding(unicode, encoding='utf-8', ctype=pyodbc.SQL_CHAR)

    # Python 3.x
    cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
    cnxn.setencoding(encoding='utf-8')


### Select Some Data

All SQL statements are executed using `Cursor.execute`.  If the statement returns rows, such as
a select statement, you can retreive them using the Cursor fetch functions (fetchone, fetchall,
fetchval, and fetchmany).

`Cursor.fetchone` is used to return a single [Row](api-row.md).

    cursor.execute("select user_id, user_name from users")
    row = cursor.fetchone()
    if row:
        print(row)

Row objects are similar to tuples, but they also allow access to columns by name:

    cursor.execute("select user_id, user_name from users")
    row = cursor.fetchone()
    print('name:', row[1])         # access by column index
    print('name:', row.user_name)  # or access by name

`fetchone` returns `None` when all rows have been retrieved.

    while 1:
        row = cursor.fetchone()
        if not row:
            break
        print('id:', row.user_id)

`Cursor.fetchall` returns all remaining rows in a list.  If there are no rows, an empty list is
returned.  (If there are a lot of rows, this will use a lot of memory.  Unread rows are stored
by the database driver in a compact format and are often sent in batches from the database
server.  Reading in only the rows you need at one time will save a lot of memory.)

    cursor.execute("select user_id, user_name from users")
    rows = cursor.fetchall()
    for row in rows:
        print(row.user_id, row.user_name)

If you are going to process the rows one at a time, you can use the cursor itself as an
iterator:

    cursor.execute("select user_id, user_name from users")
    for row in cursor:
        print(row.user_id, row.user_name)

Since `Cursor.execute` always returns the cursor, you can simplify this even more:

    for row in cursor.execute("select user_id, user_name from users"):
        print(row.user_id, row.user_name)

A lot of SQL statements are not very readable on a single line, which is where Python's triple
quoted strings really shine:

    cursor.execute(
        """
        select user_id, user_name
        from users
        where last_logon < '2001-01-01'
          and bill_overdue = 1
        """)

### Parameters

ODBC supports query parameters using a question mark as a place holder in the SQL.  You provide
the values for the question marks by passing them after the SQL:

    cursor.execute(
        """
        select user_id, user_name
        from users
        where last_logon < ?
          and bill_overdue = ?
        """, '2001-01-01', 1)

This is safer than putting the values into the string because the parameters are passed to the
database separately, protecting against
[SQL injection attacks](http://en.wikipedia.org/wiki/SQL_injection).  It is also more efficient
if you execute the same SQL repeatedly with different parameters.  The SQL will
be [prepared](http://en.wikipedia.org/wiki/Prepared_statements#Parameterized_statements) only
once.  (pyodbc only keeps the last statement prepared, so if you switch between statements,
each will be prepared multiple times.)

The Python DB API specifies that parameters should be passed in a sequence, so this is also
supported by pyodbc:

    cursor.execute(
      """
      select user_id, user_name
      from users
      where last_logon < ?
        and bill_overdue = ?
      """, ['2001-01-01', 1])

### Insert Data

Inserting data uses the same function - pass the insert SQL to execute along with any parameters.

    cursor.execute("insert into products(id, name) values ('pyodbc', 'awesome library')")
    cnxn.commit()

    cursor.execute("insert into products(id, name) values (?, ?)", 'pyodbc', 'awesome library')
    cnxn.commit()

Note the calls to cnxn.commit().  Connections maintain a transaction that is rolled back if
commit is not called.  This makes error recovery easy (and finally is not needed).

**Warning*:* You must call commit or your changes will be lost or you must turn auto-commit on.

### Update and Delete

Updating and deleting work the same way: pass the SQL to execute.  However, you often want to
know how many records were affected when updating and deleting, in which case you can use
`Cursor.rowcount` value:

    cursor.execute("delete from products where id <> ?", 'pyodbc')
    print('Deleted {} inferior products'.format(cursor.rowcount))
    cnxn.commit()

Since `execute` returns the cursor (allowing you to chain calls or use in an iterator), you
will sometimes see code with rowcount on the end:

    deleted = cursor.execute("delete from products where id <> 'pyodbc'").rowcount
    cnxn.commit()

**Warning*:* You must call commit or your changes will be lost or you must turn auto-commit on.

## Tips and Tricks

### Quotes

Since single quotes are valid in SQL, getting into the habit of using double quotes to surround
your SQL will be convenient:

    cursor.execute("delete from products where id <> 'pyodbc'")

If you are using triple quotes, you can use either:

    cursor.execute(
        """
        delete
        from products
        where id <> 'pyodbc'
        """)

### Column names

Some databases like Microsoft SQL Server do not generate column names for calculations, in
which case you need to access the columns by index.  You can also use the 'as' keyword to name
columns (the "as user_count" in the SQL below).

    row = cursor.execute("select count(*) as user_count from users").fetchone()
    print('{} users'.format(row.user_count)

Of course you can always extract the value by index or use `fetchval`:

    count = cursor.execute("select count(*) from users").fetchval()
    print('{} users'.format(count)

If there is a default value, often you can is `ISNULL` or `coalesce`:

    maxid = cursor.execute("select coalesce(max(id), 0) from users").fetchval()

### Automatic Cleanup

Connections (by default) are always in a transaction.  If a connection is closed without being
committed, the current transaction is rolled back.  Because connections are closed immediately
when they go out of scope (due to C Python's reference counting implementation) cleanup rarely
requires `except` or `finally` clauses.

For example, if either of the execute statements fails in the example below, an exception would
be raised causing both the connection cursor to go out of scope as the exception leaves the
function and unwinds the stack.  Either both inserts take place or neither, all with no clean
up code to write.

    cnxn = pyodbc.connect(...)
    cursor = cnxn.cursor()
    cursor.execute("insert into t(col) values (1)")
    cursor.execute("insert into t(col) values (2)")
    cnxn.commit()
