
Overview
========

This project is a Python database module for ODBC that implements the Python DB API 2.0
specification.

:homepage: http://code.google.com/p/pyodbc
:source:   http://github.com/mkleehammer/pyodbc
:source:   http://code.google.com/p/pyodbc/source/list

This module requires:

* Python 2.4 or greater
* ODBC 3.0 or greater

On Windows, the easiest way to install is to use the Windows installers from:

  http://code.google.com/p/pyodbc/downloads/list

Source can be obtained at

  http://github.com/mkleehammer/pyodbc/tree

  or

  http://code.google.com/p/pyodbc/source/list

To build from source, either check the source out of version control or download a source
extract and run::

  python setup.py build install

Module Specific Behavior
========================

General
-------

* The pyodbc.connect function accepts a single parameter: the ODBC connection string.  This
  string is not read or modified by pyodbc, so consult the ODBC documentation or your ODBC
  driver's documentation for details.  The general format is::

    cnxn = pyodbc.connect('DSN=mydsn;UID=userid;PWD=pwd')

* Connection caching in the ODBC driver manager is automatically enabled.

* Call cnxn.commit() since the DB API specification requires a rollback when a connection
  is closed that was not specifically committed.

* When a connection is closed, all cursors created from the connection are closed.


Data Types
----------

* Dates, times, and timestamps use the Python datetime module's date, time, and datetime
  classes.  These classes can be passed directly as parameters and will be returned when
  querying date/time columns.

* Binary data is passed and returned in Python buffer objects.

* Decimal and numeric columns are passed and returned using the Python 2.4 decimal class.


Convenient Additions
--------------------

* Cursors are iterable and returns Row objects.

  ::

    cursor.execute("select a,b from tmp")
    for row in cursor:
        print row


* The DB API specifies that results must be tuple-like, so columns are normally accessed by
  indexing into the sequence (e.g. row[0]) and pyodbc supports this. However, columns can also
  be accessed by name::

    cursor.execute("select album_id, photo_id from photos where user_id=1")
    row = cursor.fetchone()
    print row.album_id, row.photo_id
    print row[0], row[1] # same as above, but less readable

  This makes the code easier to maintain when modifying SQL, more readable, and allows rows to
  be used where a custom class might otherwise be used. All rows from a single execute share
  the same dictionary of column names, so using Row objects to hold a large result set may also
  use less memory than creating a object for each row.

  The SQL "as" keyword allows the name of a column in the result set to be specified. This is
  useful if a column name has spaces or if there is no name::

    cursor.execute("select count(*) as photo_count from photos where user_id < 100")
    row = cursor.fetchone()
    print row.photo_count


* The DB API specification does not specify the return value of Cursor.execute. Previous
  versions of pyodbc (2.0.x) returned different values, but the 2.1 versions always return the
  Cursor itself.

  This allows for compact code such as::

    for row in cursor.execute("select album_id, photo_id from photos where user_id=1"):
        print row.album_id, row.photo_id
     
    row  = cursor.execute("select * from tmp").fetchone()
    rows = cursor.execute("select * from tmp").fetchall()
     
    count = cursor.execute("update photos set processed=1 where user_id=1").rowcount
    count = cursor.execute("delete from photos where user_id=1").rowcount


* Though SQL is very powerful, values sometimes need to be modified before they can be
  used. Rows allow their values to be replaced, which makes them even more convenient ad-hoc
  data structures.

  ::

    # Replace the 'start_date' datetime in each row with one that has a time zone.
    rows = cursor.fetchall()
    for row in rows:
        row.start_date = row.start_date.astimezone(tz)

  Note that columns cannot be added to rows; only values for existing columns can be modified.


* As specified in the DB API, Cursor.execute accepts an optional sequence of parameters::

    cursor.execute("select a from tbl where b=? and c=?", (x, y))

  However, this seems complicated for something as simple as passing parameters, so pyodbc also
  accepts the parameters directly. Note in this example that x & y are not in a tuple::

    cursor.execute("select a from tbl where b=? and c=?", x, y)

* The DB API specifies that connections require a manual commit and pyodbc complies with
  this. However, connections also support autocommit, using the autocommit keyword of the
  connection function or the autocommit attribute of the Connection object::

    cnxn = pyodbc.connect(cstring, autocommit=True)

  or

  ::

    cnxn.autocommit = True
    cnxn.autocommit = False


Goals / Design
==============

* This module should not require any 3rd party modules other than ODBC.

* Only built-in data types should be used where possible.

  a) Reduces the number of libraries to learn.

  b) Reduces the number of modules and libraries to install.

  c) Eventually a standard is usually introduced.  For example, many previous database drivers
     used the mxDate classes.  Now that Python 2.3 has introduced built-in date/time classes,
     using those modules is more complicated than using the built-ins.

* It should adhere to the DB API specification, but be more "Pythonic" when convenient.
  The most common usages should be optimized for convenience and speed.

* All ODBC functionality should (eventually) be exposed.
