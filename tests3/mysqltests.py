#!/usr/bin/env python3

usage = """\
usage: %prog [options] connection_string

Unit tests for MySQL.  To use, pass a connection string as the parameter.
The tests will create and drop tables t1 and t2 as necessary.

These tests use the pyodbc library from the build directory, not the version installed in your
Python directories.  You must run `python setup.py build` before running these tests.

You can also put the connection string into a tmp/setup.cfg file like so:

  [mysqltests]
  connection-string=DRIVER=MySQL ODBC 8.0 ANSI Driver;charset=utf8mb4;SERVER=localhost;DATABASE=pyodbc;UID=root;PWD=rootpw

Note: Use the "ANSI" (not the "Unicode") driver and include charset=utf8mb4 in the connection string so the high-Unicode tests won't fail.
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

    c = (length + len(_TESTSTR)-1) // len(_TESTSTR)
    v = _TESTSTR * c
    return v[:length]

class MySqlTestCase(unittest.TestCase):

    INTEGERS = [ -1, 0, 1, 0x7FFFFFFF ]
    BIGINTS  = INTEGERS + [ 0xFFFFFFFF, 0x123456789 ]

    SMALL_FENCEPOST_SIZES = [ 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000 ]
    LARGE_FENCEPOST_SIZES = [ 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]

    STR_FENCEPOSTS    = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
    BLOB_FENCEPOSTS   = STR_FENCEPOSTS + [ _generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]

    def __init__(self, method_name, connection_string):
        unittest.TestCase.__init__(self, method_name)
        self.connection_string = connection_string

    def setUp(self):
        self.cnxn   = pyodbc.connect(self.connection_string)
        self.cursor = self.cnxn.cursor()

        # As of libmyodbc5w 5.3 SQLGetTypeInfo returns absurdly small sizes
        # leading to slow writes.  Override them:
        self.cnxn.maxwrite = 1024 * 1024 * 1024

        # My MySQL configuration (and I think the default) sends *everything*
        # in UTF-8.  The pyodbc default is to send Unicode as UTF-16 and to
        # decode WCHAR via UTF-16.  Change them both to UTF-8.
        self.cnxn.setdecoding(pyodbc.SQL_CHAR, encoding='utf-8')
        self.cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
        self.cnxn.setencoding(encoding='utf-8')

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
        self.assertTrue(isinstance(value, int))

    def test_getinfo_smallint(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        self.assertTrue(isinstance(value, int))

    def _test_strtype(self, sqltype, value, colsize=None):
        """
        The implementation for string and binary tests.
        """
        assert colsize is None or (value is None or colsize >= len(value))

        if colsize:
            sql = "create table t1(s %s(%s))" % (sqltype, colsize)
        else:
            sql = "create table t1(s %s)" % sqltype

        try:
            self.cursor.execute(sql)
        except:
            print('>>>>', sql)
        self.cursor.execute("insert into t1 values(?)", value)
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
    for value in STR_FENCEPOSTS:
        locals()['test_varchar_%s' % len(value)] = _maketest(value)

    def test_varchar_many(self):
        self.cursor.execute("create table t1(c1 varchar(300), c2 varchar(300), c3 varchar(300))")

        v1 = 'ABCDEFGHIJ' * 30
        v2 = '0123456789' * 30
        v3 = '9876543210' * 30

        self.cursor.execute("insert into t1(c1, c2, c3) values (?,?,?)", v1, v2, v3);
        row = self.cursor.execute("select c1, c2, c3 from t1").fetchone()

        self.assertEqual(v1, row.c1)
        self.assertEqual(v2, row.c2)
        self.assertEqual(v3, row.c3)

    def test_varchar_upperlatin(self):
        self._test_strtype('varchar', u'á', colsize=3)

    def test_utf16(self):
        self.cursor.execute("create table t1(c1 varchar(100) character set utf16, c2 varchar(100))")
        self.cursor.execute("insert into t1 values ('test', 'test')")
        value = "test"
        row = self.cursor.execute("select c1,c2 from t1").fetchone()
        for v in row:
            self.assertEqual(type(v), str)
            self.assertEqual(v, value)

    #
    # binary
    #

    def test_null_binary(self):
        self._test_strtype('varbinary', None, 100)

    def test_large_null_binary(self):
        # Bug 1575064
        self._test_strtype('varbinary', None, 4000)

    # Generate a test for each fencepost size: test_binary_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('varbinary', bytes(value, 'utf-8'), max(1, len(value)))
        return t
    for value in STR_FENCEPOSTS:
        locals()['test_binary_%s' % len(value)] = _maketest(value)

    #
    # blob
    #

    def test_blob_null(self):
        self._test_strtype('blob', None)

    # Generate a test for each fencepost size: test_blob_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('blob', bytes(value, 'utf-8'))
        return t
    for value in BLOB_FENCEPOSTS:
        locals()['test_blob_%s' % len(value)] = _maketest(value)

    def test_blob_upperlatin(self):
        self._test_strtype('blob', bytes('á', 'utf-8'))

    #
    # text
    #

    def test_null_text(self):
        self._test_strtype('text', None)

    # Generate a test for each fencepost size: test_text_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('text', value)
        return t
    for value in STR_FENCEPOSTS:
        locals()['test_text_%s' % len(value)] = _maketest(value)

    def test_text_upperlatin(self):
        self._test_strtype('text', 'á')

    #
    # unicode
    #

    def test_unicode_query(self):
        self.cursor.execute(u"select 1")

    #
    # bit
    #

    # The MySQL driver maps BIT colums to the ODBC bit data type, but they aren't behaving quite like a Boolean value
    # (which is what the ODBC bit data type really represents).  The MySQL BOOL data type is just an alias for a small
    # integer, so pyodbc can't recognize it and map it back to True/False.
    #
    # You can use both BIT and BOOL and they will act as you expect if you treat them as integers.  You can write 0 and
    # 1 to them and they will work.

    # def test_bit(self):
    #     value = True
    #     self.cursor.execute("create table t1(b bit)")
    #     self.cursor.execute("insert into t1 values (?)", value)
    #     v = self.cursor.execute("select b from t1").fetchone()[0]
    #     self.assertEqual(type(v), bool)
    #     self.assertEqual(v, value)
    #
    # def test_bit_string_true(self):
    #     self.cursor.execute("create table t1(b bit)")
    #     self.cursor.execute("insert into t1 values (?)", "xyzzy")
    #     v = self.cursor.execute("select b from t1").fetchone()[0]
    #     self.assertEqual(type(v), bool)
    #     self.assertEqual(v, True)
    #
    # def test_bit_string_false(self):
    #     self.cursor.execute("create table t1(b bit)")
    #     self.cursor.execute("insert into t1 values (?)", "")
    #     v = self.cursor.execute("select b from t1").fetchone()[0]
    #     self.assertEqual(type(v), bool)
    #     self.assertEqual(v, False)

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

    def _test_inttype(self, datatype, n):
        self.cursor.execute('create table t1(n %s)' % datatype)
        self.cursor.execute('insert into t1 values (?)', n)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, n)

    def _maketest(datatype, value):
        def t(self):
            self._test_inttype(datatype, value)
        return t

    for value in INTEGERS:
        name = str(abs(value))
        if value < 0:
            name = 'neg_' + name
        locals()['test_int_%s' % name] = _maketest('int', value)

    for value in BIGINTS:
        name = str(abs(value))
        if value < 0:
            name = 'neg_' + name
        locals()['test_bigint_%s' % name] = _maketest('bigint', value)

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

        self.cursor.execute("create table t1(dt datetime)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(value, result)

    def test_date(self):
        value = date(2001, 1, 1)

        self.cursor.execute("create table t1(dt date)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), type(value))
        self.assertEqual(result, value)

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

        pyodbc calls SQLRowCount after each execute and sets Cursor.rowcount.  Databases can return the actual rowcount
        or they can return -1 if it would help performance.  MySQL seems to always return the correct rowcount.
        """
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("select * from t1")
        self.assertEqual(self.cursor.rowcount, count)

        rows = self.cursor.fetchall()
        self.assertEqual(len(rows), count)
        self.assertEqual(self.cursor.rowcount, count)

    def test_rowcount_reset(self):
        "Ensure rowcount is reset to -1"

        # The Python DB API says that rowcount should be set to -1 and most ODBC drivers let us know there are no
        # records.  MySQL always returns 0, however.  Without parsing the SQL (which we are not going to do), I'm not
        # sure how we can tell the difference and set the value to -1.  For now, I'll have this test check for 0.

        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.assertEqual(self.cursor.rowcount, 1)

        self.cursor.execute("create table t2(i int)")
        self.assertEqual(self.cursor.rowcount, 0)

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

    def test_executemany(self):
        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [(i, str(i)) for i in range(1, 6)]

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
        driver_name = self.cnxn.getinfo(pyodbc.SQL_DRIVER_NAME)
        if driver_name.lower().endswith('a.dll') or driver_name.lower().endswith('a.so'):
            # skip this test for the ANSI driver
            #   on Windows, it crashes CPython
            #   on Linux, it simply fails
            return

        self.cursor.fast_executemany = True

        self.cursor.execute("create table t1(a int, b varchar(10))")

        params = [(i, str(i)) for i in range(1, 6)]

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


    # REVIEW: The following fails.  Research.

    # def test_executemany_failure(self):
    #     """
    #     Ensure that an exception is raised if one query in an executemany fails.
    #     """
    #     self.cursor.execute("create table t1(a int, b varchar(10))")
    #
    #     params = [ (1, 'good'),
    #                ('error', 'not an int'),
    #                (3, 'good') ]
    #
    #     self.assertRaises(pyodbc.Error, self.cursor.executemany, "insert into t1(a, b) value (?, ?)", params)


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


    def test_autocommit(self):
        self.assertEqual(self.cnxn.autocommit, False)

        othercnxn = pyodbc.connect(self.connection_string, autocommit=True)
        self.assertEqual(othercnxn.autocommit, True)

        othercnxn.autocommit = False
        self.assertEqual(othercnxn.autocommit, False)

    def test_emoticons_as_parameter(self):
        # https://github.com/mkleehammer/pyodbc/issues/423
        #
        # When sending a varchar parameter, pyodbc is supposed to set ColumnSize to the number
        # of characters.  Ensure it works even with 4-byte characters.
        #
        # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

        v = "x \U0001F31C z"

        self.cursor.execute("CREATE TABLE t1(s varchar(100)) DEFAULT CHARSET=utf8mb4")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_emoticons_as_literal(self):
        # https://github.com/mkleehammer/pyodbc/issues/630

        v = "x \U0001F31C z"

        self.cursor.execute("CREATE TABLE t1(s varchar(100)) DEFAULT CHARSET=utf8mb4")
        self.cursor.execute("insert into t1 values ('%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

def main():
    from optparse import OptionParser
    parser = OptionParser(usage=usage)
    parser.add_option("-v", "--verbose", action="count", default=0, help="Increment test verbosity (can be used multiple times)")
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

    if options.verbose:
        cnxn = pyodbc.connect(connection_string)
        print_library_info(cnxn)
        cnxn.close()

    suite = load_tests(MySqlTestCase, options.test, connection_string)

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(suite)

    return result


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.

    add_to_path()

    import pyodbc
    sys.exit(0 if main().wasSuccessful() else 1)
