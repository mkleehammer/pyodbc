#!/usr/bin/python

usage = """\
%(prog)s [options] connection_string

Unit tests for SQLite using the ODBC driver from http://www.ch-werner.de/sqliteodbc

To use, pass a connection string as the parameter. The tests will create and
drop tables t1 and t2 as necessary.  On Windows, use the 32-bit driver with
32-bit Python and the 64-bit driver with 64-bit Python (regardless of your
operating system bitness).

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

You can also put the connection string into a tmp/setup.cfg file like so:

  [sqlitetests]
  connection-string=Driver=SQLite3 ODBC Driver;Database=sqlite.db
"""

import sys, os, re
import pytest
import pyodbc
from decimal import Decimal
from datetime import datetime, date, time
from os.path import join, getsize, dirname, abspath
from typing import Iterator

_TESTSTR = '0123456789-abcdefghijklmnopqrstuvwxyz-'

def _generate_test_string(length):
    """
    Returns a string of `length` characters, constructed by repeating _TESTSTR as necessary.

    To enhance performance, there are 3 ways data is read, based on the length of the value, so most data types are
    tested with 3 lengths.  This function helps us generate the test data.

    We use a recognizable data set instead of a single character to make it less likely that "overlap" errors will
    be hidden and to help us manually identify where a break occurs.
    """
    if length <= len(_TESTSTR):
        return _TESTSTR[:length]

    c = (length + len(_TESTSTR)-1) // len(_TESTSTR)
    v = _TESTSTR * c
    return v[:length]

SMALL_FENCEPOST_SIZES = [ 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000 ]
LARGE_FENCEPOST_SIZES = [ 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]

STR_FENCEPOSTS = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
BYTE_FENCEPOSTS    = [ bytes(s, 'ascii') for s in STR_FENCEPOSTS ]
IMAGE_FENCEPOSTS   = BYTE_FENCEPOSTS + [ bytes(_generate_test_string(size), 'ascii') for size in LARGE_FENCEPOST_SIZES ]

@pytest.fixture
def connection_string(tmp_path):
    return os.environ.get('PYODBC_SQLITE', f'driver=SQLite3;database={tmp_path}/test.db')

@pytest.fixture()
def cnxn(connection_string):
    c = pyodbc.connect(connection_string, autocommit=False, attrs_before=None)
    yield c

    if not c.closed:
        c.close()

@pytest.fixture()
def cursor(cnxn) -> Iterator[pyodbc.Cursor]:
    cur = cnxn.cursor()

    cur.execute("drop table if exists t0")
    cur.execute("drop table if exists t1")
    cur.execute("drop table if exists t2")
    cnxn.commit()

    yield cur

def test_multiple_bindings(cursor):
    "More than one bind and select on a cursor"
    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (?)", 1)
    cursor.execute("insert into t1 values (?)", 2)
    cursor.execute("insert into t1 values (?)", 3)
    for i in range(3):
        cursor.execute("select n from t1 where n < ?", 10)
        cursor.execute("select n from t1 where n < 3")


def test_different_bindings(cursor):
    cursor.execute("create table t1(n int)")
    cursor.execute("create table t2(d datetime)")
    cursor.execute("insert into t1 values (?)", 1)
    cursor.execute("insert into t2 values (?)", datetime.now())

def test_drivers():
    p = pyodbc.drivers()
    assert isinstance(p, list)

def test_datasources():
    p = pyodbc.dataSources()
    assert isinstance(p, dict)

def test_getinfo_string(cnxn):
    value = cnxn.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
    assert isinstance(value, str)

def test_getinfo_bool(cnxn):
    value = cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
    assert isinstance(value, bool)

def test_getinfo_int(cnxn):
    value = cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
    assert isinstance(value, int)

def test_getinfo_smallint(cnxn):
    value = cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
    assert isinstance(value, int)

def _test_strtype(cursor, sqltype, value, colsize=None):
    """
    The implementation for string, Unicode, and binary tests.
    """
    assert colsize is None or (value is None or colsize >= len(value))

    if colsize:
        sql = "create table t1(s {}({}))".format(sqltype, colsize)
    else:
        sql = "create table t1(s %s)" % sqltype

    cursor.execute(sql)
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == type(value)

    if value is not None:
        assert len(v) == len(value)

    assert v == value

    # Reported by Andy Hochhaus in the pyodbc group: In 2.1.7 and earlier, a hardcoded length of 255 was used to
    # determine whether a parameter was bound as a SQL_VARCHAR or SQL_LONGVARCHAR.  Apparently SQL Server chokes if
    # we bind as a SQL_LONGVARCHAR and the target column size is 8000 or less, which is considers just SQL_VARCHAR.
    # This means binding a 256 character value would cause problems if compared with a VARCHAR column under
    # 8001. We now use SQLGetTypeInfo to determine the time to switch.
    #
    # [42000] [Microsoft][SQL Server Native Client 10.0][SQL Server]The data types varchar and text are incompatible in the equal to operator.

    cursor.execute("select * from t1 where s=?", value)


def _test_strliketype(cursor, sqltype, value, colsize=None):
    """
    The implementation for text, image, ntext, and binary.

    These types do not support comparison operators.
    """
    assert colsize is None or (value is None or colsize >= len(value))

    if colsize:
        sql = "create table t1(s {}({}))".format(sqltype, colsize)
    else:
        sql = "create table t1(s %s)" % sqltype

    cursor.execute(sql)
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == type(value)

    if value is not None:
        assert len(v) == len(value)

    assert v == value

#
# text
#

def test_text_null(cursor):
    _test_strtype(cursor, 'text', None, 100)

# Generate a test for each fencepost size: test_text_0, etc.
def _maketest(value):
    def t(cursor):
        _test_strtype(cursor, 'text', value, len(value))
    return t
for value in STR_FENCEPOSTS:
    locals()['test_text_%s' % len(value)] = _maketest(value)

def test_text_upperlatin(cursor):
    _test_strtype(cursor, 'varchar', 'á')

#
# blob
#

def test_null_blob(cursor):
    _test_strtype(cursor, 'blob', None, 100)

def test_large_null_blob(cursor):
    # Bug 1575064
    _test_strtype(cursor, 'blob', None, 4000)

# Generate a test for each fencepost size: test_unicode_0, etc.
def _maketest(value):
    def t(cursor):
        _test_strtype(cursor, 'blob', value, len(value))
    return t
for value in BYTE_FENCEPOSTS:
    locals()['test_blob_%s' % len(value)] = _maketest(value)

def test_subquery_params(cursor):
    """Ensure parameter markers work in a subquery"""
    cursor.execute("create table t1(id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, 'test')
    row = cursor.execute("""
                              select x.id
                              from (
                                select id
                                from t1
                                where s = ?
                                  and id between ? and ?
                               ) x
                               """, 'test', 1, 10).fetchone()
    assert row != None
    assert row[0] == 1

def test_close_cnxn(cursor, cnxn):
    """Make sure using a Cursor after closing its connection doesn't crash."""

    cursor.execute("create table t1(id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, 'test')
    cursor.execute("select * from t1")

    cnxn.close()

    # Now that the connection is closed, we expect an exception.  (If the code attempts to use
    # the HSTMT, we'll get an access violation instead.)
    sql = "select * from t1"
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute(sql)

def test_negative_row_index(cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "1")
    row = cursor.execute("select * from t1").fetchone()
    assert row[0] == "1"
    assert row[-1] == "1"

def test_version(cursor):
    assert 3 == len(pyodbc.version.split('.')) # 1.3.1 etc.

#
# ints and floats
#

def test_int(cursor):
    value = 1234
    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select n from t1").fetchone()[0]
    assert result == value

def test_negative_int(cursor):
    value = -1
    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select n from t1").fetchone()[0]
    assert result == value

def test_bigint(cursor):
    input = 3000000000
    cursor.execute("create table t1(d bigint)")
    cursor.execute("insert into t1 values (?)", input)
    result = cursor.execute("select d from t1").fetchone()[0]
    assert result == input

def test_negative_bigint(cursor):
    # Issue 186: BIGINT problem on 32-bit architecture
    input = -430000000
    cursor.execute("create table t1(d bigint)")
    cursor.execute("insert into t1 values (?)", input)
    result = cursor.execute("select d from t1").fetchone()[0]
    assert result == input

def test_float(cursor):
    value = 1234.567
    cursor.execute("create table t1(n float)")
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select n from t1").fetchone()[0]
    assert result == value

def test_negative_float(cursor):
    value = -200
    cursor.execute("create table t1(n float)")
    cursor.execute("insert into t1 values (?)", value)
    result  = cursor.execute("select n from t1").fetchone()[0]
    assert value == result

#
# rowcount
#

# Note: SQLRowCount does not define what the driver must return after a select statement
# and says that its value should not be relied upon.  The sqliteodbc driver is hardcoded to
# return 0 so I've deleted the test.

def test_rowcount_delete(cursor):
    assert cursor.rowcount == 0
    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    cursor.execute("delete from t1")
    assert cursor.rowcount == count

def test_rowcount_nodata(cursor):
    """
    This represents a different code path than a delete that deleted something.

    The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
    the code that errors out and drop down to the same SQLRowCount code.  On the other hand, we could hardcode a
    zero return value.
    """
    cursor.execute("create table t1(i int)")
    # This is a different code path internally.
    cursor.execute("delete from t1")
    assert cursor.rowcount == 0

# In the 2.0.x branch, Cursor.execute sometimes returned the cursor and sometimes the rowcount.  This proved very
# confusing when things went wrong and added very little value even when things went right since users could always
# use: cursor.execute("...").rowcount

def test_retcursor_delete(cursor):
    cursor.execute("create table t1(i int)")
    cursor.execute("insert into t1 values (1)")
    v = cursor.execute("delete from t1")
    assert v == cursor

def test_retcursor_nodata(cursor):
    """
    This represents a different code path than a delete that deleted something.

    The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
    the code that errors out and drop down to the same SQLRowCount code.
    """
    cursor.execute("create table t1(i int)")
    # This is a different code path internally.
    v = cursor.execute("delete from t1")
    assert v == cursor

def test_retcursor_select(cursor):
    cursor.execute("create table t1(i int)")
    cursor.execute("insert into t1 values (1)")
    v = cursor.execute("select * from t1")
    assert v == cursor

#
# misc
#

def test_lower_case(cnxn):
    "Ensure pyodbc.lowercase forces returned column names to lowercase."

    # Has to be set before creating the cursor, so we must recreate cursor.

    pyodbc.lowercase = True
    cursor = cnxn.cursor()

    cursor.execute("create table t1(Abc int, dEf int)")
    cursor.execute("select * from t1")

    names = [ t[0] for t in cursor.description ]
    names.sort()

    assert names == [ "abc", "def" ]

    # Put it back so other tests don't fail.
    pyodbc.lowercase = False

def test_row_description(cnxn):
    """
    Ensure Cursor.description is accessible as Row.cursor_description.
    """
    cursor = cnxn.cursor()
    cursor.execute("create table t1(a int, b char(3))")
    cnxn.commit()
    cursor.execute("insert into t1 values(1, 'abc')")

    row = cursor.execute("select * from t1").fetchone()

    assert cursor.description == row.cursor_description


def test_executemany(cursor):
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [ (i, str(i)) for i in range(1, 6) ]

    cursor.executemany("insert into t1(a, b) values (?,?)", params)

    count = cursor.execute("select count(*) from t1").fetchone()[0]
    assert count == len(params)

    cursor.execute("select a, b from t1 order by a")
    rows = cursor.fetchall()
    assert count == len(rows)

    for param, row in zip(params, rows):
        assert param[0] == row[0]
        assert param[1] == row[1]


def test_executemany_one(cursor):
    "Pass executemany a single sequence"
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [ (1, "test") ]

    cursor.executemany("insert into t1(a, b) values (?,?)", params)

    count = cursor.execute("select count(*) from t1").fetchone()[0]
    assert count == len(params)

    cursor.execute("select a, b from t1 order by a")
    rows = cursor.fetchall()
    assert count == len(rows)

    for param, row in zip(params, rows):
        assert param[0] == row[0]
        assert param[1] == row[1]


def test_executemany_failure(cursor):
    """
    Ensure that an exception is raised if one query in an executemany fails.
    """
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [ (1, 'good'),
               ('error', 'not an int'),
               (3, 'good') ]

    with pytest.raises(pyodbc.Error):
        cursor.executemany("insert into t1(a, b) value (?, ?)", params)


def test_row_slicing(cursor):
    cursor.execute("create table t1(a int, b int, c int, d int)");
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = row[:]
    assert result is row

    result = row[:-1]
    assert result == (1,2,3)

    result = row[0:4]
    assert result is row


def test_row_repr(cursor):
    cursor.execute("create table t1(a int, b int, c int, d int)");
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = str(row)
    assert result == "(1, 2, 3, 4)"

    result = str(row[:-1])
    assert result == "(1, 2, 3)"

    result = str(row[:1])
    assert result == "(1,)"


def test_view_select(cursor):
    # Reported in forum: Can't select from a view?  I think I do this a lot, but another test never hurts.

    # Create a table (t1) with 3 rows and a view (t2) into it.
    cursor.execute("create table t1(c1 int identity(1, 1), c2 varchar(50))")
    for i in range(3):
        cursor.execute("insert into t1(c2) values (?)", "string%s" % i)
    cursor.execute("create view t2 as select * from t1")

    # Select from the view
    cursor.execute("select * from t2")
    rows = cursor.fetchall()
    assert rows is not None
    assert len(rows) == 3

def test_autocommit(cnxn, connection_string):
    assert cnxn.autocommit == False

    othercnxn = pyodbc.connect(connection_string, autocommit=True)
    assert othercnxn.autocommit == True

    othercnxn.autocommit = False
    assert othercnxn.autocommit == False

def test_skip(cursor):
    # Insert 1, 2, and 3.  Fetch 1, skip 2, fetch 3.

    cursor.execute("create table t1(id int)");
    for i in range(1, 5):
        cursor.execute("insert into t1 values(?)", i)
    cursor.execute("select id from t1 order by id")
    assert cursor.fetchone()[0] == 1
    cursor.skip(2)
    assert cursor.fetchone()[0] == 4

def test_sets_execute(cursor):
    # Only lists and tuples are allowed.
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("create table t1 (word varchar (100))")
        words = set (['a'])
        cursor.execute("insert into t1 (word) VALUES (?)", [words])

def test_sets_executemany(cursor):
    # Only lists and tuples are allowed.
    with pytest.raises(TypeError):
        cursor.execute("create table t1 (word varchar (100))")
        words = set (['a'])
        cursor.executemany("insert into t1 (word) values (?)", [words])

def test_row_execute(cursor):
    "Ensure we can use a Row object as a parameter to execute"
    cursor.execute("create table t1(n int, s varchar(10))")
    cursor.execute("insert into t1 values (1, 'a')")
    row = cursor.execute("select n, s from t1").fetchone()
    assert row != None

    cursor.execute("create table t2(n int, s varchar(10))")
    cursor.execute("insert into t2 values (?, ?)", row)

def test_row_executemany(cursor):
    "Ensure we can use a Row object as a parameter to executemany"
    cursor.execute("create table t1(n int, s varchar(10))")

    for i in range(3):
        cursor.execute("insert into t1 values (?, ?)", i, chr(ord('a')+i))

    rows = cursor.execute("select n, s from t1").fetchall()
    assert len(rows) != 0

    cursor.execute("create table t2(n int, s varchar(10))")
    cursor.executemany("insert into t2 values (?, ?)", rows)

def test_description(cursor):
    "Ensure cursor.description is correct"

    cursor.execute("create table t1(n int, s text)")
    cursor.execute("insert into t1 values (1, 'abc')")
    cursor.execute("select * from t1")

    # (I'm not sure the precision of an int is constant across different versions, bits, so I'm hand checking the
    # items I do know.

    # int
    t = cursor.description[0]
    assert t[0] == 'n'
    assert t[1] == int
    assert t[5] == 0       # scale
    assert t[6] == True    # nullable

    # text
    t = cursor.description[1]
    assert t[0] == 's'
    assert t[1] == str
    assert t[5] == 0       # scale
    assert t[6] == True    # nullable

def test_row_equal(cursor):
    cursor.execute("create table t1(n int, s varchar(20))")
    cursor.execute("insert into t1 values (1, 'test')")
    row1 = cursor.execute("select n, s from t1").fetchone()
    row2 = cursor.execute("select n, s from t1").fetchone()
    b = (row1 == row2)
    assert b == True

def test_row_gtlt(cursor):
    cursor.execute("create table t1(n int, s varchar(20))")
    cursor.execute("insert into t1 values (1, 'test1')")
    cursor.execute("insert into t1 values (1, 'test2')")
    rows = cursor.execute("select n, s from t1 order by s").fetchall()
    assert rows[0] < rows[1]
    assert rows[0] <= rows[1]
    assert rows[1] > rows[0]
    assert rows[1] >= rows[0]
    assert rows[0] != rows[1]

    rows = list(rows)
    rows.sort() # uses <

def _test_context_manager(connection_string):
    # TODO: This is failing, but it may be due to the design of sqlite.  I've disabled it
    # for now until I can research it some more.

    # WARNING: This isn't working right now.  We've set the driver's autocommit to "off",
    # but that doesn't automatically start a transaction.  I'm not familiar enough with the
    # internals of the driver to tell what is going on, but it looks like there is support
    # for the autocommit flag.
    #
    # I thought it might be a timing issue, like it not actually starting a txn until you
    # try to do something, but that doesn't seem to work either.  I'll leave this in to
    # remind us that it isn't working yet but we need to contact the SQLite ODBC driver
    # author for some guidance.

    with pyodbc.connect(connection_string) as cnxn:
        cursor = cnxn.cursor()
        cursor.execute("begin")
        cursor.execute("create table t1(i int)")
        cursor.execute('rollback')

    # The connection should be closed now.
    with pytest.raises(pyodbc.Error):
        cnxn.execute('rollback')

def test_untyped_none(cursor):
    # From issue 129
    value = cursor.execute("select ?", None).fetchone()[0]
    assert value == None

def test_large_update_nodata(cursor):
    cursor.execute('create table t1(a blob)')
    hundredkb = 'x'*100*1024
    cursor.execute('update t1 set a=? where 1=0', (hundredkb,))

def test_no_fetch(cursor):
    # Issue 89 with FreeTDS: Multiple selects (or catalog functions that issue selects) without fetches seem to
    # confuse the driver.
    cursor.execute('select 1')
    cursor.execute('select 1')
    cursor.execute('select 1')
