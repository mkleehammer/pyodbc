#!/usr/bin/python
# -*- coding: latin-1 -*-

usage = """\
usage: %prog [options] connection_string

Unit tests for SQLite using the ODBC driver from http://www.ch-werner.de/sqliteodbc

To use, pass a connection string as the parameter. The tests will create and
drop tables t1 and t2 as necessary.  On Windows, use the 32-bit driver with
32-bit Python and the 64-bit driver with 64-bit Python (regardless of your
operating system bitness).

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

You can also put the connection string into a setup.cfg file in the root of the project
(the same one setup.py would use) like so:

  [sqlitetests]
  connection-string=Driver=SQLite3 ODBC Driver;Database=sqlite.db
"""

import sys, os, re
import unittest
from decimal import Decimal
from datetime import datetime, date, time
from os.path import join, getsize, dirname, abspath
from testutils import *

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

    c = (length + len(_TESTSTR)-1) / len(_TESTSTR)
    v = _TESTSTR * c
    return v[:length]

class SqliteTestCase(unittest.TestCase):

    SMALL_FENCEPOST_SIZES = [ 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000 ]
    LARGE_FENCEPOST_SIZES = [ 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]

    ANSI_FENCEPOSTS    = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
    UNICODE_FENCEPOSTS = [ unicode(s) for s in ANSI_FENCEPOSTS ]
    IMAGE_FENCEPOSTS   = ANSI_FENCEPOSTS + [ _generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]

    def __init__(self, method_name, connection_string):
        unittest.TestCase.__init__(self, method_name)
        self.connection_string = connection_string

    def get_sqlite_version(self):
        """
        Returns the major version: 8-->2000, 9-->2005, 10-->2008
        """
        self.cursor.execute("exec master..xp_msver 'ProductVersion'")
        row = self.cursor.fetchone()
        return int(row.Character_Value.split('.', 1)[0])

    def setUp(self):
        self.cnxn   = pyodbc.connect(self.connection_string)
        self.cursor = self.cnxn.cursor()

        for i in range(3):
            try:
                self.cursor.execute("drop table t%d" % i)
                self.cnxn.commit()
            except:
                pass

        self.cnxn.rollback()

    def tearDown(self):
        try:
            self.cursor.close()
            self.cnxn.close()
        except:
            # If we've already closed the cursor or connection, exceptions are thrown.
            pass

    def test_multiple_bindings(self):
        "More than one bind and select on a cursor"
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("insert into t1 values (?)", 1)
        self.cursor.execute("insert into t1 values (?)", 2)
        self.cursor.execute("insert into t1 values (?)", 3)
        for i in range(3):
            self.cursor.execute("select n from t1 where n < ?", 10)
            self.cursor.execute("select n from t1 where n < 3")
        

    def test_different_bindings(self):
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("create table t2(d datetime)")
        self.cursor.execute("insert into t1 values (?)", 1)
        self.cursor.execute("insert into t2 values (?)", datetime.now())

    def test_datasources(self):
        p = pyodbc.dataSources()
        self.assert_(isinstance(p, dict))

    def test_getinfo_string(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
        self.assert_(isinstance(value, str))

    def test_getinfo_bool(self):
        value = self.cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
        self.assert_(isinstance(value, bool))

    def test_getinfo_int(self):
        value = self.cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        self.assert_(isinstance(value, (int, long)))

    def test_getinfo_smallint(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        self.assert_(isinstance(value, int))

    def test_fixed_unicode(self):
        value = u"t\xebsting"
        self.cursor.execute("create table t1(s nchar(7))")
        self.cursor.execute("insert into t1 values(?)", u"t\xebsting")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), unicode)
        self.assertEqual(len(v), len(value)) # If we alloc'd wrong, the test below might work because of an embedded NULL
        self.assertEqual(v, value)


    def _test_strtype(self, sqltype, value, colsize=None):
        """
        The implementation for string, Unicode, and binary tests.
        """
        assert colsize is None or (value is None or colsize >= len(value))

        if colsize:
            sql = "create table t1(s %s(%s))" % (sqltype, colsize)
        else:
            sql = "create table t1(s %s)" % sqltype

        self.cursor.execute(sql)
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), type(value))

        if value is not None:
            self.assertEqual(len(v), len(value))

        self.assertEqual(v, value)

        # Reported by Andy Hochhaus in the pyodbc group: In 2.1.7 and earlier, a hardcoded length of 255 was used to
        # determine whether a parameter was bound as a SQL_VARCHAR or SQL_LONGVARCHAR.  Apparently SQL Server chokes if
        # we bind as a SQL_LONGVARCHAR and the target column size is 8000 or less, which is considers just SQL_VARCHAR.
        # This means binding a 256 character value would cause problems if compared with a VARCHAR column under
        # 8001. We now use SQLGetTypeInfo to determine the time to switch.
        #
        # [42000] [Microsoft][SQL Server Native Client 10.0][SQL Server]The data types varchar and text are incompatible in the equal to operator.

        self.cursor.execute("select * from t1 where s=?", value)


    def _test_strliketype(self, sqltype, value, colsize=None):
        """
        The implementation for text, image, ntext, and binary.

        These types do not support comparison operators.
        """
        assert colsize is None or (value is None or colsize >= len(value))

        if colsize:
            sql = "create table t1(s %s(%s))" % (sqltype, colsize)
        else:
            sql = "create table t1(s %s)" % sqltype

        self.cursor.execute(sql)
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), type(value))

        if value is not None:
            self.assertEqual(len(v), len(value))

        self.assertEqual(v, value)

    #
    # text
    #

    def test_text_null(self):
        self._test_strtype('text', None, 100)

    # Generate a test for each fencepost size: test_text_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('text', value, len(value))
        return t
    for value in UNICODE_FENCEPOSTS:
        locals()['test_text_%s' % len(value)] = _maketest(value)

    def test_text_upperlatin(self):
        self._test_strtype('varchar', u'á')

    #
    # blob
    #

    def test_null_blob(self):
        self._test_strtype('blob', None, 100)
     
    def test_large_null_blob(self):
        # Bug 1575064
        self._test_strtype('blob', None, 4000)

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('blob', buffer(value), len(value))
        return t
    for value in ANSI_FENCEPOSTS:
        locals()['test_blob_%s' % len(value)] = _maketest(value)

    def test_subquery_params(self):
        """Ensure parameter markers work in a subquery"""
        self.cursor.execute("create table t1(id integer, s varchar(20))")
        self.cursor.execute("insert into t1 values (?,?)", 1, 'test')
        row = self.cursor.execute("""
                                  select x.id
                                  from (
                                    select id
                                    from t1
                                    where s = ?
                                      and id between ? and ?
                                   ) x
                                   """, 'test', 1, 10).fetchone()
        self.assertNotEqual(row, None)
        self.assertEqual(row[0], 1)

    def _exec(self):
        self.cursor.execute(self.sql)
        
    def test_close_cnxn(self):
        """Make sure using a Cursor after closing its connection doesn't crash."""

        self.cursor.execute("create table t1(id integer, s varchar(20))")
        self.cursor.execute("insert into t1 values (?,?)", 1, 'test')
        self.cursor.execute("select * from t1")

        self.cnxn.close()
        
        # Now that the connection is closed, we expect an exception.  (If the code attempts to use
        # the HSTMT, we'll get an access violation instead.)
        self.sql = "select * from t1"
        self.assertRaises(pyodbc.ProgrammingError, self._exec)

    def test_empty_unicode(self):
        self.cursor.execute("create table t1(s nvarchar(20))")
        self.cursor.execute("insert into t1 values(?)", u"")

    def test_unicode_query(self):
        self.cursor.execute(u"select 1")
        
    def test_negative_row_index(self):
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", "1")
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEquals(row[0], "1")
        self.assertEquals(row[-1], "1")

    def test_version(self):
        self.assertEquals(3, len(pyodbc.version.split('.'))) # 1.3.1 etc.

    #
    # ints and floats
    #

    def test_int(self):
        value = 1234
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEquals(result, value)

    def test_negative_int(self):
        value = -1
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEquals(result, value)

    def test_bigint(self):
        input = 3000000000
        self.cursor.execute("create table t1(d bigint)")
        self.cursor.execute("insert into t1 values (?)", input)
        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(result, input)

    def test_negative_bigint(self):
        # Issue 186: BIGINT problem on 32-bit architeture
        input = -430000000
        self.cursor.execute("create table t1(d bigint)")
        self.cursor.execute("insert into t1 values (?)", input)
        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(result, input)

    def test_float(self):
        value = 1234.567
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEquals(result, value)

    def test_negative_float(self):
        value = -200
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result  = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(value, result)

    #
    # rowcount
    #

    def test_rowcount_delete(self):
        self.assertEquals(self.cursor.rowcount, -1)
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("delete from t1")
        self.assertEquals(self.cursor.rowcount, count)

    def test_rowcount_nodata(self):
        """
        This represents a different code path than a delete that deleted something.

        The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
        the code that errors out and drop down to the same SQLRowCount code.  On the other hand, we could hardcode a
        zero return value.
        """
        self.cursor.execute("create table t1(i int)")
        # This is a different code path internally.
        self.cursor.execute("delete from t1")
        self.assertEquals(self.cursor.rowcount, 0)

    def test_rowcount_select(self):
        """
        Ensure Cursor.rowcount is set properly after a select statement.
        """
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("select * from t1")
        self.assertEquals(self.cursor.rowcount, count)

        rows = self.cursor.fetchall()
        self.assertEquals(len(rows), count)
        self.assertEquals(self.cursor.rowcount, count)

    # Fails.  Not terribly important so I'm going to comment out for now and report to the ODBC driver writer.
    # def test_rowcount_reset(self):
    #     "Ensure rowcount is reset to -1"
    #     self.cursor.execute("create table t1(i int)")
    #     count = 4
    #     for i in range(count):
    #         self.cursor.execute("insert into t1 values (?)", i)
    #     self.assertEquals(self.cursor.rowcount, 1)
    #  
    #     self.cursor.execute("create table t2(i int)")
    #     self.assertEquals(self.cursor.rowcount, -1)

    #
    # always return Cursor
    #

    # In the 2.0.x branch, Cursor.execute sometimes returned the cursor and sometimes the rowcount.  This proved very
    # confusing when things went wrong and added very little value even when things went right since users could always
    # use: cursor.execute("...").rowcount

    def test_retcursor_delete(self):
        self.cursor.execute("create table t1(i int)")
        self.cursor.execute("insert into t1 values (1)")
        v = self.cursor.execute("delete from t1")
        self.assertEquals(v, self.cursor)

    def test_retcursor_nodata(self):
        """
        This represents a different code path than a delete that deleted something.

        The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
        the code that errors out and drop down to the same SQLRowCount code.
        """
        self.cursor.execute("create table t1(i int)")
        # This is a different code path internally.
        v = self.cursor.execute("delete from t1")
        self.assertEquals(v, self.cursor)

    def test_retcursor_select(self):
        self.cursor.execute("create table t1(i int)")
        self.cursor.execute("insert into t1 values (1)")
        v = self.cursor.execute("select * from t1")
        self.assertEquals(v, self.cursor)

    #
    # misc
    #

    def test_lower_case(self):
        "Ensure pyodbc.lowercase forces returned column names to lowercase."

        # Has to be set before creating the cursor, so we must recreate self.cursor.

        pyodbc.lowercase = True
        self.cursor = self.cnxn.cursor()

        self.cursor.execute("create table t1(Abc int, dEf int)")
        self.cursor.execute("select * from t1")

        names = [ t[0] for t in self.cursor.description ]
        names.sort()

        self.assertEquals(names, [ "abc", "def" ])

        # Put it back so other tests don't fail.
        pyodbc.lowercase = False
        
    def test_row_description(self):
        """
        Ensure Cursor.description is accessible as Row.cursor_description.
        """
        self.cursor = self.cnxn.cursor()
        self.cursor.execute("create table t1(a int, b char(3))")
        self.cnxn.commit()
        self.cursor.execute("insert into t1 values(1, 'abc')")

        row = self.cursor.execute("select * from t1").fetchone()

        self.assertEquals(self.cursor.description, row.cursor_description)
        

    def test_executemany(self):
        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [ (i, str(i)) for i in range(1, 6) ]

        self.cursor.executemany("insert into t1(a, b) values (?,?)", params)

        count = self.cursor.execute("select count(*) from t1").fetchone()[0]
        self.assertEqual(count, len(params))

        self.cursor.execute("select a, b from t1 order by a")
        rows = self.cursor.fetchall()
        self.assertEqual(count, len(rows))

        for param, row in zip(params, rows):
            self.assertEqual(param[0], row[0])
            self.assertEqual(param[1], row[1])


    def test_executemany_one(self):
        "Pass executemany a single sequence"
        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [ (1, "test") ]

        self.cursor.executemany("insert into t1(a, b) values (?,?)", params)

        count = self.cursor.execute("select count(*) from t1").fetchone()[0]
        self.assertEqual(count, len(params))

        self.cursor.execute("select a, b from t1 order by a")
        rows = self.cursor.fetchall()
        self.assertEqual(count, len(rows))

        for param, row in zip(params, rows):
            self.assertEqual(param[0], row[0])
            self.assertEqual(param[1], row[1])
        

    def test_executemany_failure(self):
        """
        Ensure that an exception is raised if one query in an executemany fails.
        """
        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [ (1, 'good'),
                   ('error', 'not an int'),
                   (3, 'good') ]
        
        self.failUnlessRaises(pyodbc.Error, self.cursor.executemany, "insert into t1(a, b) value (?, ?)", params)

        
    def test_row_slicing(self):
        self.cursor.execute("create table t1(a int, b int, c int, d int)");
        self.cursor.execute("insert into t1 values(1,2,3,4)")

        row = self.cursor.execute("select * from t1").fetchone()

        result = row[:]
        self.failUnless(result is row)

        result = row[:-1]
        self.assertEqual(result, (1,2,3))

        result = row[0:4]
        self.failUnless(result is row)


    def test_row_repr(self):
        self.cursor.execute("create table t1(a int, b int, c int, d int)");
        self.cursor.execute("insert into t1 values(1,2,3,4)")

        row = self.cursor.execute("select * from t1").fetchone()

        result = str(row)
        self.assertEqual(result, "(1, 2, 3, 4)")

        result = str(row[:-1])
        self.assertEqual(result, "(1, 2, 3)")

        result = str(row[:1])
        self.assertEqual(result, "(1,)")


    def test_view_select(self):
        # Reported in forum: Can't select from a view?  I think I do this a lot, but another test never hurts.

        # Create a table (t1) with 3 rows and a view (t2) into it.
        self.cursor.execute("create table t1(c1 int identity(1, 1), c2 varchar(50))")
        for i in range(3):
            self.cursor.execute("insert into t1(c2) values (?)", "string%s" % i)
        self.cursor.execute("create view t2 as select * from t1")

        # Select from the view
        self.cursor.execute("select * from t2")
        rows = self.cursor.fetchall()
        self.assert_(rows is not None)
        self.assert_(len(rows) == 3)

    def test_autocommit(self):
        self.assertEqual(self.cnxn.autocommit, False)

        othercnxn = pyodbc.connect(self.connection_string, autocommit=True)
        self.assertEqual(othercnxn.autocommit, True)

        othercnxn.autocommit = False
        self.assertEqual(othercnxn.autocommit, False)

    def test_unicode_results(self):
        "Ensure unicode_results forces Unicode"
        othercnxn = pyodbc.connect(self.connection_string, unicode_results=True)
        othercursor = othercnxn.cursor()

        # ANSI data in an ANSI column ...
        othercursor.execute("create table t1(s varchar(20))")
        othercursor.execute("insert into t1 values(?)", 'test')

        # ... should be returned as Unicode
        value = othercursor.execute("select s from t1").fetchone()[0]
        self.assertEqual(value, u'test')

    def test_skip(self):
        # Insert 1, 2, and 3.  Fetch 1, skip 2, fetch 3.

        self.cursor.execute("create table t1(id int)");
        for i in range(1, 5):
            self.cursor.execute("insert into t1 values(?)", i)
        self.cursor.execute("select id from t1 order by id")
        self.assertEqual(self.cursor.fetchone()[0], 1)
        self.cursor.skip(2)
        self.assertEqual(self.cursor.fetchone()[0], 4)

    def test_sets_execute(self):
        # Only lists and tuples are allowed.
        def f():
            self.cursor.execute("create table t1 (word varchar (100))")
            words = set (['a'])
            self.cursor.execute("insert into t1 (word) VALUES (?)", [words])

        self.assertRaises(pyodbc.ProgrammingError, f)

    def test_sets_executemany(self):
        # Only lists and tuples are allowed.
        def f():
            self.cursor.execute("create table t1 (word varchar (100))")
            words = set (['a'])
            self.cursor.executemany("insert into t1 (word) values (?)", [words])
            
        self.assertRaises(TypeError, f)

    def test_row_execute(self):
        "Ensure we can use a Row object as a parameter to execute"
        self.cursor.execute("create table t1(n int, s varchar(10))")
        self.cursor.execute("insert into t1 values (1, 'a')")
        row = self.cursor.execute("select n, s from t1").fetchone()
        self.assertNotEqual(row, None)

        self.cursor.execute("create table t2(n int, s varchar(10))")
        self.cursor.execute("insert into t2 values (?, ?)", row)
        
    def test_row_executemany(self):
        "Ensure we can use a Row object as a parameter to executemany"
        self.cursor.execute("create table t1(n int, s varchar(10))")

        for i in range(3):
            self.cursor.execute("insert into t1 values (?, ?)", i, chr(ord('a')+i))

        rows = self.cursor.execute("select n, s from t1").fetchall()
        self.assertNotEqual(len(rows), 0)

        self.cursor.execute("create table t2(n int, s varchar(10))")
        self.cursor.executemany("insert into t2 values (?, ?)", rows)
        
    def test_description(self):
        "Ensure cursor.description is correct"

        self.cursor.execute("create table t1(n int, s text)")
        self.cursor.execute("insert into t1 values (1, 'abc')")
        self.cursor.execute("select * from t1")

        # (I'm not sure the precision of an int is constant across different versions, bits, so I'm hand checking the
        # items I do know.

        # int
        t = self.cursor.description[0]
        self.assertEqual(t[0], 'n')
        self.assertEqual(t[1], int)
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

        # text
        t = self.cursor.description[1]
        self.assertEqual(t[0], 's')
        self.assertEqual(t[1], unicode)
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

    def test_none_param(self):
        "Ensure None can be used for params other than the first"
        # Some driver/db versions would fail if NULL was not the first parameter because SQLDescribeParam (only used
        # with NULL) could not be used after the first call to SQLBindParameter.  This means None always worked for the
        # first column, but did not work for later columns.
        #
        # If SQLDescribeParam doesn't work, pyodbc would use VARCHAR which almost always worked.  However,
        # binary/varbinary won't allow an implicit conversion.

        value = u'\x12abc'
        self.cursor.execute("create table t1(n int, b blob)")
        self.cursor.execute("insert into t1 values (1, ?)", value)
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEqual(row.n, 1)
        self.assertEqual(type(row.b), buffer)
        self.assertEqual(row.b, value)


    def test_row_equal(self):
        self.cursor.execute("create table t1(n int, s varchar(20))")
        self.cursor.execute("insert into t1 values (1, 'test')")
        row1 = self.cursor.execute("select n, s from t1").fetchone()
        row2 = self.cursor.execute("select n, s from t1").fetchone()
        b = (row1 == row2)
        self.assertEqual(b, True)

    def test_row_gtlt(self):
        self.cursor.execute("create table t1(n int, s varchar(20))")
        self.cursor.execute("insert into t1 values (1, 'test1')")
        self.cursor.execute("insert into t1 values (1, 'test2')")
        rows = self.cursor.execute("select n, s from t1 order by s").fetchall()
        self.assert_(rows[0] < rows[1])
        self.assert_(rows[0] <= rows[1])
        self.assert_(rows[1] > rows[0])
        self.assert_(rows[1] >= rows[0])
        self.assert_(rows[0] != rows[1])

        rows = list(rows)
        rows.sort() # uses <
        
    def test_context_manager(self):
        with pyodbc.connect(self.connection_string) as cnxn:
            cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)

        # The connection should be closed now.
        def test():
            cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        self.assertRaises(pyodbc.ProgrammingError, test)

    def test_untyped_none(self):
        # From issue 129
        value = self.cursor.execute("select ?", None).fetchone()[0]
        self.assertEqual(value, None)
        
    def test_large_update_nodata(self):
        self.cursor.execute('create table t1(a blob)')
        hundredkb = buffer('x'*100*1024)
        self.cursor.execute('update t1 set a=? where 1=0', (hundredkb,))

    def test_no_fetch(self):
        # Issue 89 with FreeTDS: Multiple selects (or catalog functions that issue selects) without fetches seem to
        # confuse the driver.
        self.cursor.execute('select 1')
        self.cursor.execute('select 1')
        self.cursor.execute('select 1')

def main():
    from optparse import OptionParser
    parser = OptionParser(usage=usage)
    parser.add_option("-v", "--verbose", action="count", help="Increment test verbosity (can be used multiple times)")
    parser.add_option("-d", "--debug", action="store_true", default=False, help="Print debugging items")
    parser.add_option("-t", "--test", help="Run only the named test")

    (options, args) = parser.parse_args()

    if len(args) > 1:
        parser.error('Only one argument is allowed.  Do you need quotes around the connection string?')

    if not args:
        connection_string = load_setup_connection_string('sqlitetests')
        print 'connection_string:', connection_string

        if not connection_string:
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args[0]

    cnxn = pyodbc.connect(connection_string)
    print_library_info(cnxn)
    cnxn.close()

    suite = load_tests(SqliteTestCase, options.test, connection_string)

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(suite)


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.

    add_to_path()

    import pyodbc
    main()
