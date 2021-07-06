#!/usr/bin/python

usage="""\
usage: %prog [options] filename

Unit tests for Microsoft Access

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

To run, pass the file EXTENSION of an Access database on the command line:

  accesstests accdb

An empty Access 2000 database (empty.mdb) or an empty Access 2007 database
(empty.accdb), are automatically created for the tests.

To run a single test, use the -t option:

  accesstests -t unicode_null accdb

If you want to report an error, it would be helpful to include the driver information
by using the verbose flag and redirecting the output to a file:

 accesstests -v accdb >& results.txt

You can pass the verbose flag twice for more verbose output:

 accesstests -vv accdb
"""

# Access SQL data types: http://msdn2.microsoft.com/en-us/library/bb208866.aspx

import sys, os, re
import unittest
from decimal import Decimal
from datetime import datetime, date, time
from os.path import abspath, dirname, join
import shutil
from testutils import *

CNXNSTRING = None

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


class AccessTestCase(unittest.TestCase):

    SMALL_FENCEPOST_SIZES = [ 0, 1, 254, 255 ] # text fields <= 255
    LARGE_FENCEPOST_SIZES = [ 256, 270, 304, 508, 510, 511, 512, 1023, 1024, 2047, 2048, 4000, 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]

    CHAR_FENCEPOSTS    = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
    IMAGE_FENCEPOSTS   = CHAR_FENCEPOSTS + [ _generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]

    def __init__(self, method_name):
        unittest.TestCase.__init__(self, method_name)

    def setUp(self):
        self.cnxn   = pyodbc.connect(CNXNSTRING)
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

    def test_closed_reflects_connection_state(self):
        self.assertFalse(self.cnxn.closed)
        self.cnxn.close()
        self.assertTrue(self.cnxn.closed)

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
        The implementation for string, Unicode, and binary tests.
        """
        assert colsize is None or (value is None or colsize >= len(value)), 'colsize=%s value=%s' % (colsize, (value is None) and 'none' or len(value))

        if colsize:
            sql = "create table t1(n1 int not null, s1 %s(%s), s2 %s(%s))" % (sqltype, colsize, sqltype, colsize)
        else:
            sql = "create table t1(n1 int not null, s1 %s, s2 %s)" % (sqltype, sqltype)

        self.cursor.execute(sql)
        self.cursor.execute("insert into t1 values(1, ?, ?)", (value, value))
        row = self.cursor.execute("select s1, s2 from t1").fetchone()

        for i in range(2):
            v = row[i]

            self.assertEqual(type(v), type(value))

            if value is not None:
                self.assertEqual(len(v), len(value))

            self.assertEqual(v, value)


    def test_varchar_null(self):
        self._test_strtype('varchar', None, 255)

    # Generate a test for each fencepost size: test_varchar_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('varchar', value, len(value))
        t.__doc__ = 'varchar %s' % len(value)
        return t
    for value in CHAR_FENCEPOSTS:
        locals()['test_varchar_%s' % len(value)] = _maketest(value)

    #
    # binary
    #

    def test_null_binary(self):
        self._test_strtype('binary', None)

    # Generate a test for each fencepost size: test_varchar_0, etc.
    def _maketest(value):
        def t(self):
            # Convert to UTF-8 to create a byte array
            self._test_strtype('varbinary', value.encode('utf-8'), len(value))
        t.__doc__ = 'binary %s' % len(value)
        return t
    for value in CHAR_FENCEPOSTS:
        locals()['test_binary_%s' % len(value)] = _maketest(value)


    # #
    # # image
    # #

    # def test_null_image(self):
    #     self._test_strtype('image', None)

    # # Generate a test for each fencepost size: test_varchar_0, etc.
    # def _maketest(value):
    #     def t(self):
    #         self._test_strtype('image', value.encode('utf-8'))
    #     t.__doc__ = 'image %s' % len(value)
    #     return t
    # for value in IMAGE_FENCEPOSTS:
    #     locals()['test_image_%s' % len(value)] = _maketest(value)

    #
    # memo
    #

    def test_null_memo(self):
        self._test_strtype('memo', None)

    # Generate a test for each fencepost size: test_varchar_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('memo', value)
        t.__doc__ = 'Unicode to memo %s' % len(value)
        return t
    for value in IMAGE_FENCEPOSTS:
        locals()['test_memo_%s' % len(value)] = _maketest(value)

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

        self.cursor.execute("create table t1(dt datetime)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
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

    def test_smallint(self):
        value = 32767
        self.cursor.execute("create table t1(n smallint)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_real(self):
        value = 1234.5
        self.cursor.execute("create table t1(n real)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_negative_real(self):
        value = -200.5
        self.cursor.execute("create table t1(n real)")
        self.cursor.execute("insert into t1 values (?)", value)
        result  = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(value, result)

    def test_float(self):
        value = 1234.567
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_negative_float(self):
        value = -200.5
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result  = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(value, result)

    def test_tinyint(self):
        self.cursor.execute("create table t1(n tinyint)")
        value = 10
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(type(result), type(value))
        self.assertEqual(value, result)

    #
    # decimal & money
    #

    def test_decimal(self):
        value = Decimal('12345.6789')
        self.cursor.execute("create table t1(n numeric(10,4))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(type(v), Decimal)
        self.assertEqual(v, value)

    def test_money(self):
        self.cursor.execute("create table t1(n money)")
        value = Decimal('1234.45')
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(type(result), type(value))
        self.assertEqual(value, result)

    def test_negative_decimal_scale(self):
        value = Decimal('-10.0010')
        self.cursor.execute("create table t1(d numeric(19,4))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), Decimal)
        self.assertEqual(v, value)

    #
    # bit
    #

    def test_bit(self):
        self.cursor.execute("create table t1(b bit)")

        value = True
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select b from t1").fetchone()[0]
        self.assertEqual(type(result), bool)
        self.assertEqual(value, result)

    def test_bit_null(self):
        self.cursor.execute("create table t1(b bit)")

        value = None
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select b from t1").fetchone()[0]
        self.assertEqual(type(result), bool)
        self.assertEqual(False, result)

    def test_guid(self):
        value = u"de2ac9c6-8676-4b0b-b8a6-217a8580cbee"
        self.cursor.execute("create table t1(g1 uniqueidentifier)")
        self.cursor.execute("insert into t1 values (?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), type(value))
        self.assertEqual(len(v), len(value))


    #
    # rowcount
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
    # Misc
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
        v2 = u'0123456789' * 25
        v3 = u'9876543210' * 25
        value = v2 + 'x' + v3

        self.cursor.execute("create table t1(c2 varchar(250), c3 varchar(250))")
        self.cursor.execute("insert into t1(c2, c3) values (?,?)", v2, v3)

        row = self.cursor.execute("select c2 + 'x' + c3 from t1").fetchone()

        self.assertEqual(row[0], value)


    def test_autocommit(self):
        self.assertEqual(self.cnxn.autocommit, False)

        othercnxn = pyodbc.connect(CNXNSTRING, autocommit=True)
        self.assertEqual(othercnxn.autocommit, True)

        othercnxn.autocommit = False
        self.assertEqual(othercnxn.autocommit, False)


def main():
    from argparse import ArgumentParser
    parser = ArgumentParser(usage=usage)
    parser.add_argument("-v", "--verbose", default=0, action="count", help="Increment test verbosity (can be used multiple times)")
    parser.add_argument("-d", "--debug", action="store_true", default=False, help="Print debugging items")
    parser.add_argument("-t", "--test", help="Run only the named test")
    parser.add_argument('type', choices=['accdb', 'mdb'], help='Which type of file to test')

    args = parser.parse_args()

    DRIVERS = {
        'accdb': 'Microsoft Access Driver (*.mdb, *.accdb)',
        'mdb': 'Microsoft Access Driver (*.mdb)'
    }

    here = dirname(abspath(__file__))
    src = join(here, 'empty.' + args.type)
    dest = join(here, 'test.' + args.type)
    shutil.copy(src, dest)

    global CNXNSTRING
    CNXNSTRING = 'DRIVER={%s};DBQ=%s;ExtendedAnsiSQL=1' % (DRIVERS[args.type], dest)
    print(CNXNSTRING)

    if args.verbose:
        cnxn = pyodbc.connect(CNXNSTRING)
        print_library_info(cnxn)
        cnxn.close()

    suite = load_tests(AccessTestCase, args.test)

    testRunner = unittest.TextTestRunner(verbosity=args.verbose)
    result = testRunner.run(suite)

    return result


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.
    add_to_path()
    import pyodbc
    sys.exit(0 if main().wasSuccessful() else 1)
