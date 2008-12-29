
Overview
========

This project is a Python database module for ODBC that implements the Python DB API 2.0
specification.

:homepage: http://code.google.com/p/pyodbc
:source:   http://github.com/mkleehammer/pyodbc

This module requires:

* Python 2.4 or greater
* ODBC 3.0 or greater

On Windows, the easiest way to install is to use the Windows installers from:

  http://code.google.com/p/pyodbc/downloads/list

Source can be obtained at

  http://github.com/mkleehammer/pyodbc/tree

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


Convenience Methods
-------------------

* Cursors are iterable and returns Row objects.

  ::

    cursor.execute("select a,b from tmp")
    for row in cursor:
        print row


* The DB API PEP does not specify the return type for Cursor.execute, so pyodbc tries to be
  maximally convenient:

  1) If a SELECT is executed, the Cursor itself is returned to allow code like the following::

       for row in cursor.execute("select a,b from tmp"):
           print row

  2) If an UPDATE, INSERT, or DELETE statement is issued, the number of rows affected is
     returned::

       count = cursor.execute("delete from tmp where a in (1,2,3)")

  3) Otherwise (CREATE TABLE, etc.), None is returned.


* An execute method has been added to the Connection class.  It creates a Cursor and returns
  whatever Cursor.execute returns.  This allows for the following::

    for row in cnxn.execute("select a,b from tmp"):
        print row

  or

  ::

    rows = cnxn.execute("select * from tmp where a in (1,2,3)").fetchall()

  Since each call creates a new Cursor, only use this when executing a single statement.


* Both Cursor.execute and Connection.execute allow parameters to be passed as additional
  parameters following the query.

  ::

    cnxn.execute("select a,b from tmp where a=? or a=?", 1, 2)

  The specification is not entirely clear, but most other drivers require parameters to be
  passed in a sequence.  To ensure compatibility, pyodbc will also accept this format::

    cnxn.execute("select a,b from tmp where a=? or a=?", (1, 2))


* Row objects are derived from tuple to match the API specification, but they also support
  accessing columns by name.

  ::

    for row in cnxn.execute("select A,b from tmp"):
        print row.a, row.b


* The following are not supported or are ignored: nextset, setinputsizes, setoutputsizes.


* Values in Row objects can be replaced, either by name or index.  Sometimes it is convenient
  to "preprocess" values.

  ::

    row = cursor.execute("select a,b from tmp").fetchone()

    row.a  = calc(row.a)
    row[1] = calc(row.b)


Goals / Design
==============

* This module should not require any 3rd party modules other than ODBC.

* Only built-in data types should be used where possible.

  a) Reduces the number of libraries to learn.

  b) Reduces the number of modules and libraries to install.

  c) Eventually a standard is usually introduced.  For example, many previous database drivers
     used the mxDate classes.  Now that Python 2.3 has introduced built-in date/time classes,
     using those modules is more complicated than using the built-ins.

* It should adhere to the DB API specification, but be maximally convenient where possible.
  The most common usages should be optimized for convenience and speed.
