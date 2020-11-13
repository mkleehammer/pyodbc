#!/usr/bin/env python3
# -*- coding: utf-8 -*-

usage = """\
usage: %prog [options] connection_string

Unit tests for DBMaker.  To use, pass a connection string as the parameter.
The tests will create and drop tables t1 and t2 as necessary.

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

You can also put the connection string into ./tmp/setup.cfg like so:

  [dbmakertests]
  connection-string=DSN=mydb;UID=test;PWD=test
"""

import sys, os, re
import unittest
from decimal import Decimal
from datetime import datetime, date, time
from os.path import join, getsize, dirname, abspath, basename
from testutils import *

_TESTSTR = '0123456789-abcdefghijklmnopqrstuvwxyz-'

def _generate_test_string(length):
    """
    Returns a string of composed of `seed` to make a string `length` characters long.

    To enhance performance, there are 3 ways data is read, based on the length of the value, so most data types are
    tested with 3 lengths.  This function helps us generate the test data.

    We use a recognizable data set instead of a single character to make it less likely that "overlap" errors will
    be hidden and to help us manually identify where a break occurs.
    """
    if length <= len(_TESTSTR):
        return _TESTSTR[:length]

    c = int((length + len(_TESTSTR)-1) / len(_TESTSTR))
    v = _TESTSTR * c
    return v[:length]

class DBMakerTestCase(unittest.TestCase):

    SMALL_FENCEPOST_SIZES = [ 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000 ]
    LARGE_FENCEPOST_SIZES = [ 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]

    ANSI_FENCEPOSTS    = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
    UNICODE_FENCEPOSTS = [ str(s) for s in ANSI_FENCEPOSTS ]
    BLOB_FENCEPOSTS   = ANSI_FENCEPOSTS + [ _generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]

    def __init__(self, method_name, connection_string):
        unittest.TestCase.__init__(self, method_name)
        self.connection_string = connection_string

    def setUp(self):
        self.cnxn   = pyodbc.connect(self.connection_string)
        self.cursor = self.cnxn.cursor()
        
        self.cnxn.setdecoding(pyodbc.SQL_CHAR, encoding = 'utf-8')

        # As of DBMaker SQLGetTypeInfo returns absurdly small sizes leading
        # to slow writes.  Override them:
        self.cnxn.maxwrite = 1024 * 1024 * 1024

        for i in range(3):
            try:
                self.cursor.execute("drop table t%d" % i)
                self.cnxn.commit()
            except:
                pass

        for i in range(3):
            try:
                self.cursor.execute("drop procedure proc%d" % i)
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

    def test_binary_type(self):
        if sys.hexversion >= 0x02060000:
            self.assertIs(pyodbc.BINARY, bytearray)
        else:
            self.assertIs(pyodbc.BINARY, buffer)

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
        self.cursor.execute("create table t2(d timestamp)")
        self.cursor.execute("insert into t1 values (?)", 1)
        self.cursor.execute("insert into t2 values (?)", datetime.now())

    def test_drivers(self):
        p = pyodbc.drivers()
        self.assertTrue(isinstance(p, list))

    def test_datasources(self):
        p = pyodbc.dataSources()
        self.assertTrue(isinstance(p, dict))

    def test_getinfo_string(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
        self.assertTrue(isinstance(value, str))

    def test_getinfo_bool(self):
        value = self.cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
        self.assertTrue(isinstance(value, bool))

    def test_getinfo_int(self):
        value = self.cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        self.assertTrue(isinstance(value, (int, int)))

    def test_getinfo_smallint(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        self.assertTrue(isinstance(value, int))

    def test_fixed_unicode(self):
        value = "t\xebsting"
        self.cursor.execute("create table t1(s nchar(7))")
        self.cursor.execute("insert into t1 values(?)", "t\xebsting")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), str)
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

        print("sql:%s"%sql)
        try:
            self.cursor.execute(sql)
        except:
            print ('>>>>', sql)
        print("sql:insert into t1 values")
        self.cursor.execute("insert into t1 values(?)", value)
        print("sql:select * from t1")
        v = self.cursor.execute("select * from t1").fetchone()[0]

        # Removing this check for now until I get the charset working properly.
        # If we use latin1, results are 'str' instead of 'unicode', which would be
        # correct.  Setting charset to ucs-2 causes a crash in SQLGetTypeInfo(SQL_DATETIME).
        # self.assertEqual(type(v), type(value))

        if value is not None:
            self.assertEqual(len(v), len(value))

        self.assertEqual(v, value)

    #
    # varchar
    #

    def test_varchar_null(self):
        self._test_strtype('varchar', None, 100)

    # Generate a test for each fencepost size: test_varchar_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('varchar', value, max(1, len(value)))
        return t
    for value in ANSI_FENCEPOSTS:
        locals()['test_varchar_%s' % len(value)] = _maketest(value)

    # Generate a test using Unicode.
    for value in UNICODE_FENCEPOSTS:
        locals()['test_wvarchar_%s' % len(value)] = _maketest(value)

    def test_varchar_many(self):
        self.cursor.execute("create table t1(c1 varchar(300), c2 varchar(300), c3 varchar(300))")

        v1 = 'ABCDEFGHIJ' * 30
        v2 = '0123456789' * 30
        v3 = '9876543210' * 30

        self.cursor.execute("insert into t1(c1, c2, c3) values (?,?,?)", v1, v2, v3);
        row = self.cursor.execute("select c1, c2, c3 from t1").fetchone()

        self.assertEqual(v1, row.C1)
        self.assertEqual(v2, row.C2)
        self.assertEqual(v3, row.C3)
    
    def test_chinese(self):
        v = '我的'
        self.cursor.execute("SELECT N'我的' AS name")
        row = self.cursor.fetchone()
        self.assertEqual(row[0], v)

        self.cursor.execute("SELECT N'我的' AS name")
        rows = self.cursor.fetchall()
        self.assertEqual(rows[0][0], v)
    
    #
    # binary
    #

    def test_null_binary(self):
        self._test_strtype('binary', None, 100)
     
    def test_large_null_binary(self):
        # Bug 1575064
        self._test_strtype('binary', None, 4000)

    def test_binaryNull_object(self):
        self.cursor.execute("create table t1(n binary(10))")
        self.cursor.execute("insert into t1 values (?)", pyodbc.BinaryNull);

    # Generate a test for each fencepost size: test_binary_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('binary', bytearray(value, 'utf-8'), max(1, len(value)))
        return t
    for value in ANSI_FENCEPOSTS:
        locals()['test_binary_%s' % len(value)] = _maketest(value)

    #
    # blob
    #

    def test_blob_null(self):
        self._test_strtype('blob', None)

    # Generate a test for each fencepost size: test_blob_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('blob', bytearray(value, 'utf-8'))
        return t
    for value in BLOB_FENCEPOSTS:
        locals()['test_blob_%s' % len(value)] = _maketest(value)

    def test_blob_upperlatin(self):
        self._test_strtype('blob', bytearray('á','utf-8'))

    #
    # clob
    #

    def test_null_clob(self):
        self._test_strtype('clob', None)

    # Generate a test for each fencepost size: test_clob_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('clob', value)
        return t
    for value in ANSI_FENCEPOSTS:
        locals()['test_clob_%s' % len(value)] = _maketest(value)


    #
    # unicode
    #

    def test_unicode_null(self):
        self._test_strtype('nvarchar', None, colsize=100)

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('nvarchar', value, colsize=len(value))
        return t
    for value in UNICODE_FENCEPOSTS:
        locals()['test_unicode_%s' % len(value)] = _maketest(value)

    #
    # bit
    #

    # def test_bit(self):
    #     value = True
    #     self.cursor.execute("create table t1(b bit)")
    #     self.cursor.execute("insert into t1 values (?)", value)
    #     v = self.cursor.execute("select b from t1").fetchone()[0]
    #     self.assertEqual(type(v), bool)
    #     self.assertEqual(v, value)
    
    #
    # decimal
    #

    def test_small_decimal(self):
        # value = Decimal('1234567890987654321')
        value = Decimal('100010')       # (I use this because the ODBC docs tell us how the bytes should look in the C struct)
        self.cursor.execute("create table t1(d numeric(19))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), Decimal)
        self.assertEqual(v, value)


    def test_small_decimal_scale(self):
        # The same as small_decimal, except with a different scale.  This value exactly matches the ODBC documentation
        # example in the C Data Types appendix.
        value = '1000.10'
        value = Decimal(value)
        self.cursor.execute("create table t1(d numeric(20,6))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), Decimal)
        self.assertEqual(v, value)


    def test_negative_decimal_scale(self):
        value = Decimal('-10.0010')
        self.cursor.execute("create table t1(d numeric(19,4))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), Decimal)
        self.assertEqual(v, value)

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

    def test_empty_string(self):
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", "")

    def test_fixed_str(self):
        value = "testing"
        self.cursor.execute("create table t1(s char(7))")
        self.cursor.execute("insert into t1 values(?)", "testing")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(v, value)

    def test_empty_unicode(self):
        self.cursor.execute("create table t1(s nvarchar(20))")
        self.cursor.execute("insert into t1 values(?)", u"")

    def test_unicode_query(self):
        self.cursor.execute(u"select 1")

    def test_negative_row_index(self):
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", "1")
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEqual(row[0], "1")
        self.assertEqual(row[-1], "1")

    def test_version(self):
        self.assertEqual(3, len(pyodbc.version.split('.'))) # 1.3.1 etc.

    #
    # date, time, datetime
    #

    def test_datetime(self):
        value = datetime(2007, 1, 15, 3, 4, 5)

        self.cursor.execute("create table t1(dt timestamp)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(value, result)

    def test_datetime_fraction(self):
        # SQL Server supports milliseconds, but Python's datetime supports nanoseconds, so the most granular datetime
        # supported is xxx000.

        value = datetime(2007, 1, 15, 3, 4, 5, 123000)
     
        self.cursor.execute("create table t1(dt timestamp)")
        self.cursor.execute("insert into t1 values (?)", value)
     
        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), datetime)
        self.assertEqual(result, value)

    def test_datetime_fraction_rounded(self):
        # SQL Server supports milliseconds, but Python's datetime supports nanoseconds.  pyodbc rounds down to what the
        # database supports.

        full    = datetime(2007, 1, 15, 3, 4, 5, 123456)
        rounded = datetime(2007, 1, 15, 3, 4, 5, 123000)
     
        self.cursor.execute("create table t1(dt timestamp)")
        self.cursor.execute("insert into t1 values (?)", full)
     
        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), datetime)
        self.assertEqual(result, rounded)

    def test_date(self):
        value = date.today()
     
        self.cursor.execute("create table t1(d date)")
        self.cursor.execute("insert into t1 values (?)", value)
     
        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(value, result)

    def test_time(self):
        value = datetime.now().time()

        # We aren't yet writing values using the new extended time type so the value written to the database is only
        # down to the second.
        value = value.replace(microsecond=0)
         
        self.cursor.execute("create table t1(t time)")
        self.cursor.execute("insert into t1 values (?)", value)
         
        result = self.cursor.execute("select t from t1").fetchone()[0]
        self.assertEqual(type(result), time)
        self.assertEqual(value, result)

    #
    # ints and floats
    #

    def test_int(self):
        value = 1234
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_negative_int(self):
        value = -1
        self.cursor.execute("create table t1(n int)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_bigint(self):

        # This fails on 64-bit Fedora with 5.1.
        # Should return 0x0123456789
        # Does return   0x0000000000
        #
        # Top 4 bytes are returned as 0x00 00 00 00.  If the input is high enough, they are returned as 0xFF FF FF FF.
        input = 0x123456789
        self.cursor.execute("create table t1(d bigint)")
        self.cursor.execute("insert into t1 values (?)", input)
        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(result, input)

    def test_float(self):
        value = 1234.5
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)    

    def test_negative_float(self):
        value = -200
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result  = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(value, result)
    
    #
    # misc
    #

    def test_rowcount_delete(self):
        self.assertEqual(self.cursor.rowcount, -1)
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("delete from t1")
        self.assertEqual(self.cursor.rowcount, count)

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
        self.assertEqual(self.cursor.rowcount, 0)

    def test_rowcount_select(self):
        """
        Ensure Cursor.rowcount is set properly after a select statement.

        pyodbc calls SQLRowCount after each execute and sets Cursor.rowcount, but SQL Server 2005 returns -1 after a
        select statement, so we'll test for that behavior.  This is valid behavior according to the DB API
        specification, but people don't seem to like it.
        """
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("select * from t1")
        self.assertEqual(self.cursor.rowcount, -1)

        rows = self.cursor.fetchall()
        self.assertEqual(len(rows), count)
        self.assertEqual(self.cursor.rowcount, -1)

    def test_rowcount_reset(self):
        "Ensure rowcount is reset to -1"

        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.assertEqual(self.cursor.rowcount, 1)

        self.cursor.execute("create table t2(i int)")
        self.assertEqual(self.cursor.rowcount, -1)

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
        self.assertEqual(v, self.cursor)

    def test_retcursor_nodata(self):
        """
        This represents a different code path than a delete that deleted something.

        The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
        the code that errors out and drop down to the same SQLRowCount code.
        """
        self.cursor.execute("create table t1(i int)")
        # This is a different code path internally.
        v = self.cursor.execute("delete from t1")
        self.assertEqual(v, self.cursor)

    def test_retcursor_select(self):
        self.cursor.execute("create table t1(i int)")
        self.cursor.execute("insert into t1 values (1)")
        v = self.cursor.execute("select * from t1")
        self.assertEqual(v, self.cursor)

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

        self.assertEqual(names, [ "abc", "def" ])

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
        self.assertEqual(self.cursor.description, row.cursor_description)
        
    def test_temp_select(self):
        # A project was failing to create temporary tables via select into.
        self.cursor.execute("create table t1(s char(7))")
        self.cursor.execute("insert into t1 values(?)", "testing")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), str)   # hj: should remove this check?
        self.assertEqual(v, "testing")

        self.cursor.execute("select s into t2 from t1")
        v = self.cursor.execute("select * from t2").fetchone()[0]
        self.assertEqual(type(v), str)
        self.assertEqual(v, "testing")


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
        
    def test_fast_executemany(self):

        self.fast_executemany = True

        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [(i, str(i)) for i in range(1, 6)]

        self.cursor.executemany("insert into t1(a, b) values (?,?)", params)

        # REVIEW: Without the cast, we get the following error: [07006] [unixODBC]Received an
        # unsupported type from Postgres.;\nERROR: table "t2" does not exist (14)

        count = self.cursor.execute("select cast(count(*) as int) from t1").fetchone()[0]
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
        self.assertRaises(pyodbc.Error, self.cursor.executemany, "insert into t1(a, b) value (?, ?)", params)

        
    def test_row_slicing(self):
        self.cursor.execute("create table t1(a int, b int, c int, d int)");
        self.cursor.execute("insert into t1 values(1,2,3,4)")

        row = self.cursor.execute("select * from t1").fetchone()

        result = row[:]
        self.assertTrue(result is row)

        result = row[:-1]
        self.assertEqual(result, (1,2,3))

        result = row[0:4]
        self.assertTrue(result is row)


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


    def test_concatenation(self):
        v2 = '0123456789' * 30
        v3 = '9876543210' * 30

        self.cursor.execute("create table t1(c1 serial(1), c2 varchar(300), c3 varchar(300))")
        self.cursor.execute("insert into t1(c2, c3) values (?,?)", v2, v3)

        row = self.cursor.execute("select c2, c3, c2 || c3 _both from t1").fetchone()

        self.assertEqual(row._BOTH, v2 + v3)


    def test_view_select(self):
        # Reported in forum: Can't select from a view?  I think I do this a lot, but another test never hurts.

        # Create a table (t1) with 3 rows and a view (t2) into it.
        self.cursor.execute("create table t1(c1 serial(1), c2 varchar(50))")
        for i in range(3):
            self.cursor.execute("insert into t1(c2) values (?)", "string%s" % i)
        self.cursor.execute("create view t2 as select * from t1")

        # Select from the view
        self.cursor.execute("select * from t2")
        rows = self.cursor.fetchall()
        self.assertTrue(rows is not None)
        self.assertTrue(len(rows) == 3)


    def test_autocommit(self):
        self.assertEqual(self.cnxn.autocommit, False)

        othercnxn = pyodbc.connect(self.connection_string, autocommit=True)
        self.assertEqual(othercnxn.autocommit, True)

        othercnxn.autocommit = False
        self.assertEqual(othercnxn.autocommit, False)	
    
    def test_exc_integrity(self):
        "Make sure an IntegretyError is raised"
        # This is really making sure we are properly encoding and comparing the SQLSTATEs.
        self.cursor.execute("create table t1(s1 varchar(10) primary key)")
        self.cursor.execute("insert into t1 values ('one')")
        self.assertRaises(pyodbc.IntegrityError, self.cursor.execute, "insert into t1 values ('one')")
    
    def test_columns(self):
        # When using aiohttp, `await cursor.primaryKeys('t1')` was raising the error
        #
        #   Error: TypeError: argument 2 must be str, not None
        #
        # I'm not sure why, but PyArg_ParseTupleAndKeywords fails if you use "|s" for an
        # optional string keyword when calling indirectly.

        self.cursor.execute("create table t1(a int, b varchar(3))")

        self.cursor.columns('t1')
        results = {row.column_name: row for row in self.cursor}
        row = results['A']
        assert row.type_name == 'INTEGER', row.type_name
        row = results['B']
        assert row.type_name == 'VARCHAR'
        assert row.column_size == 3, row.column_size
       

        # Now do the same, but specifically pass in None to one of the keywords.  Old versions
        # were parsing arguments incorrectly and would raise an error.  (This crops up when
        # calling indirectly like columns(*args, **kwargs) which aiodbc does.)

        self.cursor.columns('t1', schema=None, catalog=None)
        results = {row.column_name: row for row in self.cursor}
        row = results['A']
        assert row.type_name == 'INTEGER', row.type_name
        row = results['B']
        assert row.type_name == 'VARCHAR'
        assert row.column_size == 3
		
    def test_cancel(self):
        # I'm not sure how to reliably cause a hang to cancel, so for now we'll settle with
        # making sure SQLCancel is called correctly.
        self.cursor.execute("select 1")
        self.cursor.cancel()

    def test_emoticons_as_parameter(self):
        # https://github.com/mkleehammer/pyodbc/issues/423
        #
        # When sending a varchar parameter, pyodbc is supposed to set ColumnSize to the number
        # of characters.  Ensure it works even with 4-byte characters.
        #
        # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

        v = "x \U0001F31C z"

        self.cursor.execute("CREATE TABLE t1(s nvarchar(100))")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_emoticons_as_literal(self):
        # https://github.com/mkleehammer/pyodbc/issues/630

        v = "x \U0001F31C z"

        self.cursor.execute("CREATE TABLE t1(s nvarchar(100))")
        self.cursor.execute("insert into t1 values ('%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)
  
    def test_GBK_nvarchar_parameter(self):
       
        v = '我的'

        self.cursor.execute("CREATE TABLE t1(s nvarchar(100))")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_GBK_nvarchar_literal(self):

        v = '我的'

        self.cursor.execute("CREATE TABLE t1(s nvarchar(100))")
        self.cursor.execute("insert into t1 values ('%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_GBK_varchar_parameter(self):
       
        v = '我的'

        self.cursor.execute("CREATE TABLE t1(s varchar(100))")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_GBK_varchar_literal(self):

        v = '我的'

        self.cursor.execute("CREATE TABLE t1(s varchar(100))")
        self.cursor.execute("insert into t1 values ('%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)
   
    def test_cursorcommit(self):
        "Ensure cursor.commit works"
        othercnxn = pyodbc.connect(self.connection_string)
        othercursor = othercnxn.cursor()
        othercnxn = None

        othercursor.execute("create table t1(s varchar(20))")
        othercursor.execute("insert into t1 values(?)", 'test')
        othercursor.commit()

        value = self.cursor.execute("select s from t1").fetchone()[0]
        self.assertEqual(value, 'test')


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

        self.cursor.execute("create table t1(n int, s varchar(8), d decimal(5,2))")
        self.cursor.execute("insert into t1 values (1, 'abc', 1.23)")
        self.cursor.execute("select * from t1")

        # (I'm not sure the precision of an int is constant across different versions, bits, so I'm hand checking the
        # items I do know.

        # int
        t = self.cursor.description[0]
        self.assertEqual(t[0], 'N')
        self.assertEqual(t[1], int)
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

        # varchar(8)
        t = self.cursor.description[1]
        self.assertEqual(t[0], 'S')
        self.assertEqual(t[1], str)
        self.assertEqual(t[4], 8)       # precision
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

        # decimal(5, 2)
        t = self.cursor.description[2]
        self.assertEqual(t[0], 'D')
        self.assertEqual(t[1], Decimal)
        self.assertEqual(t[4], 5)       # precision
        self.assertEqual(t[5], 2)       # scale
        self.assertEqual(t[6], True)    # nullable

    def test_output_conversion(self):
        def convert(value):
            # `value` will be a string.  We'll simply add an X at the beginning at the end.
            return 'X' + value.decode('latin1') + 'X'
        self.cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert)
        self.cursor.execute("create table t1(n int, v varchar(10))")
        self.cursor.execute("insert into t1 values (1, '123.45')")
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        # Now clear the conversions and try again.  There should be no Xs this time.
        self.cnxn.clear_output_converters()
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')

        # Same but clear using remove_output_converter.
        self.cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        self.cnxn.remove_output_converter(pyodbc.SQL_VARCHAR)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')

        # And lastly, clear by passing None for the converter.
        self.cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        self.cnxn.add_output_converter(pyodbc.SQL_VARCHAR, None)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')

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
        self.assertTrue(rows[0] < rows[1])
        self.assertTrue(rows[0] <= rows[1])
        self.assertTrue(rows[1] > rows[0])
        self.assertTrue(rows[1] >= rows[0])
        self.assertTrue(rows[0] != rows[1])

        rows = list(rows)
        rows.sort() # uses <

    def test_context_manager_success(self):
        """
        Ensure a successful with statement causes a commit.
        """
        self.cursor.execute("create table t1(n int)")
        self.cnxn.commit()

        with pyodbc.connect(self.connection_string) as cnxn:
            cursor = cnxn.cursor()
            cursor.execute("insert into t1 values (1)")

        cnxn = None
        cursor = None

        rows = self.cursor.execute("select n from t1").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], 1)

    def test_context_manager_fail(self):
        """
        Ensure an exception in a with statement causes a rollback.
        """
        self.cursor.execute("create table t1(n int)")
        self.cnxn.commit()

        try:
            with pyodbc.connect(self.connection_string) as cnxn:
                cursor = cnxn.cursor()
                cursor.execute("insert into t1 values (1)")
                raise Exception("Testing failure")
        except Exception:
            pass

        cnxn = None
        cursor = None

        count = self.cursor.execute("select count(*) from t1").fetchone()[0]
        self.assertEqual(count, 0)

    def test_cursor_context_manager_success(self):
        """
        Ensure a successful with statement using a cursor causes a commit.
        """
        self.cursor.execute("create table t1(n int)")
        self.cnxn.commit()

        with pyodbc.connect(self.connection_string).cursor() as cursor:
            cursor.execute("insert into t1 values (1)")

        cursor = None

        rows = self.cursor.execute("select n from t1").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], 1)


    def test_cursor_context_manager_fail(self):
        """
        Ensure an exception in a with statement using a cursor causes a rollback.
        """
        self.cursor.execute("create table t1(n int)")
        self.cnxn.commit()

        try:
            with pyodbc.connect(self.connection_string).cursor() as cursor:
                cursor.execute("insert into t1 values (1)")
                raise Exception("Testing failure")
        except Exception:
            pass

        cursor = None

        count = self.cursor.execute("select count(*) from t1").fetchone()[0]
        self.assertEqual(count, 0)

    def test_untyped_none(self):
        # From issue 129
        value = self.cursor.execute("select ?", None).fetchone()[0]
        self.assertEqual(value, None)

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
        filename = basename(sys.argv[0])
        assert filename.endswith('.py')
        connection_string = load_setup_connection_string(filename[:-3])

        if not connection_string:
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args[0]

    cnxn = pyodbc.connect(connection_string)
    print_library_info(cnxn)
    cnxn.close()

    suite = load_tests(DBMakerTestCase, options.test, connection_string)

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(suite)


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.

    add_to_path()

    import pyodbc
    main()
