#!/usr/bin/env python
# -*- coding: utf-8 -*-

usage = """\
usage: %prog [options] connection_string

Unit tests for PostgreSQL.  To use, pass a connection string as the parameter.
The tests will create and drop tables t1 and t2 as necessary.

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

You can also put the connection string into a tmp/setup.cfg file like so:

  [pgtests]
  connection-string=DSN=PostgreSQL35W

Note: Be sure to use the "Unicode" (not the "ANSI") version of the PostgreSQL ODBC driver.
"""

import sys
import uuid
import unittest
from decimal import Decimal
from testutils import *

_TESTSTR = '0123456789-abcdefghijklmnopqrstuvwxyz-'


def _generate_test_string(length):
    """
    Returns a string of composed of `seed` to make a string `length` characters long.

    To enhance performance, there are 3 ways data is read, based on the length of the value, so
    most data types are tested with 3 lengths.  This function helps us generate the test data.

    We use a recognizable data set instead of a single character to make it less likely that
    "overlap" errors will be hidden and to help us manually identify where a break occurs.
    """
    if length <= len(_TESTSTR):
        return _TESTSTR[:length]

    c = int((length + len(_TESTSTR) - 1) / len(_TESTSTR))
    v = _TESTSTR * c
    return v[:length]


class PGTestCase(unittest.TestCase):

    INTEGERS = [ -1, 0, 1, 0x7FFFFFFF ]
    BIGINTS  = INTEGERS + [ 0xFFFFFFFF, 0x123456789 ]

    SMALL_READ = 100
    LARGE_READ = 4000

    SMALL_STRING = _generate_test_string(SMALL_READ)
    LARGE_STRING = _generate_test_string(LARGE_READ)
    SMALL_BYTES  = bytes(SMALL_STRING, 'utf-8')
    LARGE_BYTES  = bytes(LARGE_STRING, 'utf-8')

    def __init__(self, connection_string, ansi, method_name):
        unittest.TestCase.__init__(self, method_name)
        self.connection_string = connection_string
        self.ansi = ansi

    def setUp(self):
        self.cnxn   = pyodbc.connect(self.connection_string, ansi=self.ansi)
        self.cursor = self.cnxn.cursor()

        # I've set my test database to use UTF-8 which seems most popular.
        self.cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
        self.cnxn.setencoding(encoding='utf-8')

        # As of psql 9.5.04 SQLGetTypeInfo returns absurdly small sizes leading
        # to slow writes.  Override them:
        self.cnxn.maxwrite = 1024 * 1024 * 1024

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

    def _simpletest(datatype, value):
        # A simple test that can be used for any data type where the Python
        # type we write is also what we expect to receive.
        def _t(self):
            self.cursor.execute('create table t1(value %s)' % datatype)
            self.cursor.execute('insert into t1 values (?)', value)
            result = self.cursor.execute("select value from t1").fetchone()[0]
            self.assertEqual(result, value)
        return _t

    def test_drivers(self):
        p = pyodbc.drivers()
        self.assertTrue(isinstance(p, list))

    def test_datasources(self):
        p = pyodbc.dataSources()
        self.assertTrue(isinstance(p, dict))

    # def test_gettypeinfo(self):
    #     self.cursor.getTypeInfo(pyodbc.SQL_VARCHAR)
    #     cols = [t[0] for t in self.cursor.description]
    #     print('cols:', cols)
    #     for row in self.cursor:
    #         for col,val in zip(cols, row):
    #             print(' ', col, val)

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


    def test_negative_float(self):
        value = -200
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result  = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(value, result)


    def _test_strtype(self, sqltype, value, colsize=None, resulttype=None):
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

        result = self.cursor.execute("select * from t1").fetchone()[0]

        if resulttype and type(value) is not resulttype:
            value = resulttype(value)

        self.assertEqual(result, value)

    def test_maxwrite(self):
        # If we write more than `maxwrite` bytes, pyodbc will switch from
        # binding the data all at once to providing it at execute time with
        # SQLPutData.  The default maxwrite is 1GB so this is rarely needed in
        # PostgreSQL but I need to test the functionality somewhere.
        self.cnxn.maxwrite = 300
        self._test_strtype('varchar', _generate_test_string(400))

    #
    # VARCHAR
    #

    def test_empty_varchar(self):
        self._test_strtype('varchar', '', self.SMALL_READ)

    def test_null_varchar(self):
        self._test_strtype('varchar', None, self.SMALL_READ)

    def test_large_null_varchar(self):
        # There should not be a difference, but why not find out?
        self._test_strtype('varchar', None, self.LARGE_READ)

    def test_small_varchar(self):
        self._test_strtype('varchar', self.SMALL_STRING, self.SMALL_READ)

    def test_large_varchar(self):
        self._test_strtype('varchar', self.LARGE_STRING, self.LARGE_READ)

    def test_varchar_many(self):
        self.cursor.execute("create table t1(c1 varchar(300), c2 varchar(300), c3 varchar(300))")

        v1 = 'ABCDEFGHIJ' * 30
        v2 = '0123456789' * 30
        v3 = '9876543210' * 30

        self.cursor.execute("insert into t1(c1, c2, c3) values (?,?,?)", v1, v2, v3)
        row = self.cursor.execute("select c1, c2, c3 from t1").fetchone()

        self.assertEqual(v1, row.c1)
        self.assertEqual(v2, row.c2)
        self.assertEqual(v3, row.c3)

    def test_chinese(self):
        v = '我的'
        self.cursor.execute("SELECT N'我的' AS name")
        row = self.cursor.fetchone()
        self.assertEqual(row[0], v)

        self.cursor.execute("SELECT N'我的' AS name")
        rows = self.cursor.fetchall()
        self.assertEqual(rows[0][0], v)

    #
    # bytea
    #

    def test_null_bytea(self):
        self._test_strtype('bytea', None)
    def test_small_bytea(self):
        self._test_strtype('bytea', self.SMALL_BYTES)
    def test_large_bytea(self):
        self._test_strtype('bytea', self.LARGE_BYTES)

    # Now test with bytearray
    def test_large_bytea_array(self):
        self._test_strtype('bytea', bytearray(self.LARGE_BYTES), resulttype=bytes)

    for value in INTEGERS:
        name = str(value).replace('.', '_').replace('-', 'neg_')
        locals()['test_int_%s' % name] = _simpletest('int', value)

    for value in BIGINTS:
        name = str(value).replace('.', '_').replace('-', 'neg_')
        locals()['test_bigint_%s' % name] = _simpletest('bigint', value)

    for value in [-1234.56, -1, 0, 1, 1234.56, 123456789.21]:
        name = str(value).replace('.', '_').replace('-', 'neg_')
        locals()['test_money_%s' % name] = _simpletest('money', value)

    for value in "-1234.56  -1  0  1  1234.56  123456789.21".split():
        name = value.replace('.', '_').replace('-', 'neg_')
        locals()['test_decimal_%s' % name] = _simpletest('decimal(20,6)', Decimal(value))

    for value in "-1234.56  -1  0  1  1234.56  123456789.21".split():
        name = value.replace('.', '_').replace('-', 'neg_')
        locals()['test_numeric_%s' % name] = _simpletest('numeric(20,6)', Decimal(value))

    def test_small_decimal(self):
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

    def test_nonnative_uuid(self):
        # The default is False meaning we should return a string.  Note that
        # SQL Server seems to always return uppercase.
        value = uuid.uuid4()
        self.cursor.execute("create table t1(n uuid)")
        self.cursor.execute("insert into t1 values (?)", value)

        pyodbc.native_uuid = False
        result = self.cursor.execute("select n from t1").fetchval()
        self.assertEqual(type(result), str)
        self.assertEqual(result, str(value).upper())

    def test_native_uuid(self):
        # When true, we should return a uuid.UUID object.
        value = uuid.uuid4()
        self.cursor.execute("create table t1(n uuid)")
        self.cursor.execute("insert into t1 values (?)", value)

        pyodbc.native_uuid = True
        result = self.cursor.execute("select n from t1").fetchval()
        self.assertIsInstance(result, uuid.UUID)
        self.assertEqual(value, result)

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
        self.assertEqual(type(v), str)
        self.assertEqual(len(v), len(value)) # If we alloc'd wrong, the test below might work because of an embedded NULL
        self.assertEqual(v, value)

    def test_fetchval(self):
        expected = "test"
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", expected)
        result = self.cursor.execute("select * from t1").fetchval()
        self.assertEqual(result, expected)

    def test_negative_row_index(self):
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", "1")
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEqual(row[0], "1")
        self.assertEqual(row[-1], "1")

    def test_version(self):
        self.assertEqual(3, len(pyodbc.version.split('.'))) # 1.3.1 etc.

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
        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.cursor.execute("select * from t1")
        self.assertEqual(self.cursor.rowcount, 4)

    # PostgreSQL driver fails here?
    # def test_rowcount_reset(self):
    #     "Ensure rowcount is reset to -1"
    #
    #     self.cursor.execute("create table t1(i int)")
    #     count = 4
    #     for i in range(count):
    #         self.cursor.execute("insert into t1 values (?)", i)
    #     self.assertEqual(self.cursor.rowcount, 1)
    #
    #     self.cursor.execute("create table t2(i int)")
    #     self.assertEqual(self.cursor.rowcount, -1)

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

        params = [ (i, str(i)) for i in range(1, 6) ]

        self.cursor.executemany("insert into t1(a, b) values (?,?)", params)

        # REVIEW: Without the cast, we get the following error:
        # [07006] [unixODBC]Received an unsupported type from Postgres.;\nERROR:  table "t2" does not exist (14)

        count = self.cursor.execute("select cast(count(*) as int) from t1").fetchone()[0]
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

    def test_cnxn_execute_error(self):
        """
        Make sure that Connection.execute (not Cursor) errors are not "eaten".

        GitHub issue #74
        """
        self.cursor.execute("create table t1(a int primary key)")
        self.cursor.execute("insert into t1 values (1)")
        self.assertRaises(pyodbc.Error, self.cnxn.execute, "insert into t1 values (1)")

    def test_row_repr(self):
        self.cursor.execute("create table t1(a int, b int, c int, d int)")
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

    def test_exc_integrity(self):
        "Make sure an IntegretyError is raised"
        # This is really making sure we are properly encoding and comparing the SQLSTATEs.
        self.cursor.execute("create table t1(s1 varchar(10) primary key)")
        self.cursor.execute("insert into t1 values ('one')")
        self.assertRaises(pyodbc.IntegrityError, self.cursor.execute, "insert into t1 values ('one')")


    def test_cnxn_set_attr_before(self):
        # I don't have a getattr right now since I don't have a table telling me what kind of
        # value to expect.  For now just make sure it doesn't crash.
        # From the unixODBC sqlext.h header file.
        SQL_ATTR_PACKET_SIZE = 112
        othercnxn = pyodbc.connect(self.connection_string, attrs_before={ SQL_ATTR_PACKET_SIZE : 1024 * 32 })

    def test_cnxn_set_attr(self):
        # I don't have a getattr right now since I don't have a table telling me what kind of
        # value to expect.  For now just make sure it doesn't crash.
        # From the unixODBC sqlext.h header file.
        SQL_ATTR_ACCESS_MODE = 101
        SQL_MODE_READ_ONLY   = 1
        self.cnxn.set_attr(SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_ONLY)

    def test_columns(self):
        # When using aiohttp, `await cursor.primaryKeys('t1')` was raising the error
        #
        #   Error: TypeError: argument 2 must be str, not None
        #
        # I'm not sure why, but PyArg_ParseTupleAndKeywords fails if you use "|s" for an
        # optional string keyword when calling indirectly.

        self.cursor.execute("create table t1(a int, b varchar(3), xΏz varchar(4))")

        self.cursor.columns('t1')
        results = {row.column_name: row for row in self.cursor}
        row = results['a']
        assert row.type_name == 'int4', row.type_name
        row = results['b']
        assert row.type_name == 'varchar'
        assert row.precision == 3, row.precision
        row = results['xΏz']
        assert row.type_name == 'varchar'
        assert row.precision == 4, row.precision

        # Now do the same, but specifically pass in None to one of the keywords.  Old versions
        # were parsing arguments incorrectly and would raise an error.  (This crops up when
        # calling indirectly like columns(*args, **kwargs) which aiodbc does.)

        self.cursor.columns('t1', schema=None, catalog=None)
        results = {row.column_name: row for row in self.cursor}
        row = results['a']
        assert row.type_name == 'int4', row.type_name
        row = results['b']
        assert row.type_name == 'varchar'
        assert row.precision == 3

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

        self.cursor.execute("CREATE TABLE t1(s varchar(100))")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_emoticons_as_literal(self):
        # https://github.com/mkleehammer/pyodbc/issues/630

        v = "x \U0001F31C z"

        self.cursor.execute("CREATE TABLE t1(s varchar(100))")
        self.cursor.execute("insert into t1 values ('%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_cursor_messages(self):
        """
        Test the Cursor.messages attribute.
        """
        # self.cursor is used in setUp, hence is not brand new at this point
        brand_new_cursor = self.cnxn.cursor()
        self.assertIsNone(brand_new_cursor.messages)

        # using INFO message level because they are always sent to the client regardless of
        # client_min_messages: https://www.postgresql.org/docs/11/runtime-config-client.html
        for msg in ('hello world', 'ABCDEFGHIJ' * 800):
            self.cursor.execute("""
                CREATE OR REPLACE PROCEDURE test_cursor_messages()
                LANGUAGE plpgsql
                AS $$
                BEGIN
                    RAISE INFO '{}' USING ERRCODE = '01000';
                END;
                $$;
            """.format(msg))
            self.cursor.execute("CALL test_cursor_messages();")
            messages = self.cursor.messages
            self.assertTrue(type(messages) is list)
            self.assertTrue(len(messages) > 0)
            self.assertTrue(all(type(m) is tuple for m in messages))
            self.assertTrue(all(len(m) == 2 for m in messages))
            self.assertTrue(all(type(m[0]) is str for m in messages))
            self.assertTrue(all(type(m[1]) is str for m in messages))
            self.assertTrue(all(m[0] == '[01000] (-1)' for m in messages))
            self.assertTrue(''.join(m[1] for m in messages).endswith(msg))

    def test_output_conversion(self):
        # Note the use of SQL_WVARCHAR, not SQL_VARCHAR.

        def convert(value):
            # The value is the raw bytes (as a bytes object) read from the
            # database.  We'll simply add an X at the beginning at the end.
            return 'X' + value.decode('latin1') + 'X'

        self.cursor.execute("create table t1(n int, v varchar(10))")
        self.cursor.execute("insert into t1 values (1, '123.45')")

        self.cnxn.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        # Clear all conversions and try again.  There should be no Xs this time.
        self.cnxn.clear_output_converters()
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')

        # Same but clear using remove_output_converter.
        self.cnxn.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        self.cnxn.remove_output_converter(pyodbc.SQL_WVARCHAR)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')

        # And lastly, clear by passing None for the converter.
        self.cnxn.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        self.cnxn.add_output_converter(pyodbc.SQL_WVARCHAR, None)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')
        
def main():
    from optparse import OptionParser
    parser = OptionParser(usage="usage: %prog [options] connection_string")
    parser.add_option("-v", "--verbose", default=0, action="count", help="Increment test verbosity (can be used multiple times)")
    parser.add_option("-d", "--debug", action="store_true", default=False, help="Print debugging items")
    parser.add_option("-t", "--test", help="Run only the named test")
    parser.add_option('-a', '--ansi', help='ANSI only', default=False, action='store_true')

    (options, args) = parser.parse_args()

    if len(args) > 1:
        parser.error('Only one argument is allowed.  Do you need quotes around the connection string?')

    if not args:
        connection_string = load_setup_connection_string('pgtests')

        if not connection_string:
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args[0]

    if options.verbose:
        cnxn = pyodbc.connect(connection_string, ansi=options.ansi)
        print_library_info(cnxn)
        cnxn.close()

    if options.test:
        # Run a single test
        if not options.test.startswith('test_'):
            options.test = 'test_%s' % (options.test)

        s = unittest.TestSuite([ PGTestCase(connection_string, options.ansi, options.test) ])
    else:
        # Run all tests in the class

        methods = [ m for m in dir(PGTestCase) if m.startswith('test_') ]
        methods.sort()
        s = unittest.TestSuite([ PGTestCase(connection_string, options.ansi, m) for m in methods ])

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(s)

    return result


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.

    add_to_path()

    import pyodbc
    sys.exit(0 if main().wasSuccessful() else 1)
