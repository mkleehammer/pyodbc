#!/usr/bin/python
# -*- coding: utf-8 -*-

from __future__ import print_function

usage = """\
usage: %prog [options] connection_string

Unit tests for SQL Server.  To use, pass a connection string as the parameter.
The tests will create and drop tables t1 and t2 as necessary.

These run using the version from the 'build' directory, not the version
installed into the Python directories.  You must run python setup.py build
before running the tests.

You can also put the connection string into a tmp/setup.cfg file like so:

  [sqlservertests]
  connection-string=DRIVER={SQL Server};SERVER=localhost;UID=uid;PWD=pwd;DATABASE=db

The connection string above will use the 2000/2005 driver, even if SQL Server 2008
is installed:

  2000: DRIVER={SQL Server}
  2005: DRIVER={SQL Server}
  2008: DRIVER={SQL Server Native Client 10.0}
  
If using FreeTDS ODBC, be sure to use version 1.00.97 or newer.
"""

import sys, os, re, uuid
import unittest
from decimal import Decimal
from datetime import datetime, date, time
from os.path import join, getsize, dirname, abspath
from warnings import warn
from testutils import *

# Some tests have fallback code for known driver issues.
# Change this value to False to bypass the fallback code, e.g., to see
#   if a newer version of the driver has fixed the underlying issue.
#
handle_known_issues = True

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

class SqlServerTestCase(unittest.TestCase):

    SMALL_FENCEPOST_SIZES = [ 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000 ]
    LARGE_FENCEPOST_SIZES = [ 4095, 4096, 4097, 10 * 1024, 20 * 1024 ]
    MAX_FENCEPOST_SIZES   = [ 5 * 1024 * 1024 ] #, 50 * 1024 * 1024 ]

    ANSI_SMALL_FENCEPOSTS    = [ _generate_test_string(size) for size in SMALL_FENCEPOST_SIZES ]
    UNICODE_SMALL_FENCEPOSTS = [ unicode(s) for s in ANSI_SMALL_FENCEPOSTS ]
    ANSI_LARGE_FENCEPOSTS    = ANSI_SMALL_FENCEPOSTS    + [ _generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]
    UNICODE_LARGE_FENCEPOSTS = UNICODE_SMALL_FENCEPOSTS + [ unicode(s) for s in [_generate_test_string(size) for size in LARGE_FENCEPOST_SIZES ]]

    ANSI_MAX_FENCEPOSTS    = ANSI_LARGE_FENCEPOSTS + [ _generate_test_string(size) for size in MAX_FENCEPOST_SIZES ]
    UNICODE_MAX_FENCEPOSTS = UNICODE_LARGE_FENCEPOSTS + [ unicode(s) for s in [_generate_test_string(size) for size in MAX_FENCEPOST_SIZES ]]


    def __init__(self, method_name, connection_string):
        unittest.TestCase.__init__(self, method_name)
        self.connection_string = connection_string

    def driver_type_is(self, type_name):
        recognized_types = {
            'msodbcsql': '(Microsoft) ODBC Driver xx for SQL Server',
            'freetds': 'FreeTDS ODBC',
        }
        if not type_name in recognized_types.keys():
            raise KeyError('"{0}" is not a recognized driver type: {1}'.format(type_name, list(recognized_types.keys())))
        driver_name = self.cnxn.getinfo(pyodbc.SQL_DRIVER_NAME).lower()
        if type_name == 'msodbcsql':
            return ('msodbcsql' in driver_name) or ('sqlncli' in driver_name) or ('sqlsrv32.dll' == driver_name)
        elif type_name == 'freetds':
            return ('tdsodbc' in driver_name)

    def handle_known_issues_for(self, type_name, print_reminder=False):
        """
        Checks driver `type_name` and "killswitch" variable `handle_known_issues` to see if
        known issue handling should be bypassed. Optionally prints a reminder message to
        help identify tests that previously had issues but may have been fixed by a newer
        version of the driver.

        Usage examples:

        # 1. print reminder at beginning of test (before any errors can occur)
        #
        def test_some_feature(self):
            self.handle_known_issues_for('freetds', print_reminder=True)
            # (continue with test code)

        # 2. conditional execution of fallback code
        #
        try:
            # (some test code)
        except pyodbc.DataError:
            if self.handle_known_issues_for('freetds'):
                # FREETDS_KNOWN_ISSUE
                #
                # (fallback code to work around exception)
            else:
                raise
        """
        if self.driver_type_is(type_name):
            if handle_known_issues:
                return True
            else:
                if print_reminder:
                    print("Known issue handling is disabled. Does this test still fail?")
        return False

    def driver_type_is(self, type_name):
        recognized_types = {
            'msodbcsql': '(Microsoft) ODBC Driver xx for SQL Server',
            'freetds': 'FreeTDS ODBC',
        }
        if not type_name in recognized_types.keys():
            raise KeyError('"{0}" is not a recognized driver type: {1}'.format(type_name, list(recognized_types.keys())))
        driver_name = self.cnxn.getinfo(pyodbc.SQL_DRIVER_NAME).lower()
        if type_name == 'msodbcsql':
            return ('msodbcsql' in driver_name) or ('sqlncli' in driver_name) or ('sqlsrv32.dll' == driver_name)
        elif type_name == 'freetds':
            return ('tdsodbc' in driver_name)

    def get_sqlserver_version(self):
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

        for i in range(3):
            try:
                self.cursor.execute("drop procedure proc%d" % i)
                self.cnxn.commit()
            except:
                pass

        try:
            self.cursor.execute('drop function func1')
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
            self.assertTrue(pyodbc.BINARY is bytearray)
        else:
            self.assertTrue(pyodbc.BINARY is buffer)

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
        self.assertTrue(isinstance(value, (int, long)))

    def test_getinfo_smallint(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        self.assertTrue(isinstance(value, int))

    def test_noscan(self):
        self.assertEqual(self.cursor.noscan, False)
        self.cursor.noscan = True
        self.assertEqual(self.cursor.noscan, True)

    def test_nonnative_uuid(self):
        # The default is False meaning we should return a string.  Note that
        # SQL Server seems to always return uppercase.
        value = uuid.uuid4()
        self.cursor.execute("create table t1(n uniqueidentifier)")
        self.cursor.execute("insert into t1 values (?)", value)

        pyodbc.native_uuid = False
        result = self.cursor.execute("select n from t1").fetchval()
        self.assertEqual(type(result), unicode)
        self.assertEqual(result, unicode(value).upper())

    def test_native_uuid(self):
        # When true, we should return a uuid.UUID object.
        value = uuid.uuid4()
        self.cursor.execute("create table t1(n uniqueidentifier)")
        self.cursor.execute("insert into t1 values (?)", value)

        pyodbc.native_uuid = True
        result = self.cursor.execute("select n from t1").fetchval()
        self.assertTrue(isinstance(result, uuid.UUID))
        self.assertEqual(value, result)

    def test_nextset(self):
        self.cursor.execute("create table t1(i int)")
        for i in range(4):
            self.cursor.execute("insert into t1(i) values(?)", i)

        self.cursor.execute("select i from t1 where i < 2 order by i; select i from t1 where i >= 2 order by i")

        for i, row in enumerate(self.cursor):
            self.assertEqual(i, row.i)

        self.assertEqual(self.cursor.nextset(), True)

        for i, row in enumerate(self.cursor):
            self.assertEqual(i + 2, row.i)

    def test_nextset_with_raiserror(self):
        self.cursor.execute("select i = 1; RAISERROR('c', 16, 1);")
        row = next(self.cursor)
        self.assertEqual(1, row.i)
        if self.driver_type_is('freetds'):
            warn('FREETDS_KNOWN_ISSUE - test_nextset_with_raiserror: test cancelled.')
            # AssertionError: ProgrammingError not raised by nextset
            # https://github.com/FreeTDS/freetds/issues/230
            return  # for now
        self.assertRaises(pyodbc.ProgrammingError, self.cursor.nextset)

    def test_fixed_unicode(self):
        value = u"t\xebsting"
        self.cursor.execute("create table t1(s nchar(7))")
        self.cursor.execute("insert into t1 values(?)", u"t\xebsting")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), unicode)
        self.assertEqual(len(v), len(value)) # If we alloc'd wrong, the test below might work because of an embedded NULL
        self.assertEqual(v, value)


    def _test_strtype(self, sqltype, value, resulttype=None, colsize=None):
        """
        The implementation for string, Unicode, and binary tests.
        """
        assert colsize in (None, 'max') or isinstance(colsize, int), colsize
        assert colsize in (None, 'max') or (value is None or colsize >= len(value))

        if colsize:
            sql = "create table t1(s %s(%s))" % (sqltype, colsize)
        else:
            sql = "create table t1(s %s)" % sqltype
        self.cursor.execute(sql)

        if resulttype is None:
            resulttype = type(value)

        sql = "insert into t1 values(?)"
        try:
            self.cursor.execute(sql, value)
        except pyodbc.DataError:
            if self.driver_type_is('freetds'):
                # FREETDS_KNOWN_ISSUE
                #
                # cnxn.getinfo(pyodbc.SQL_DESCRIBE_PARAMETER) returns False for FreeTDS, so
                # pyodbc can't call SQLDescribeParam to get the correct parameter type.
                # This can lead to errors being returned from SQL Server when sp_prepexec is called, 
                # e.g., "Implicit conversion from data type varchar to varbinary is not allowed." 
                # for test_binary_null
                #
                # So at least verify that the user can manually specify the parameter type
                if sqltype == 'varbinary':
                    sql_param_type = pyodbc.SQL_VARBINARY
                    # (add elif blocks for other cases as required)
                self.cursor.setinputsizes([(sql_param_type, colsize, 0)])
                self.cursor.execute(sql, value)
            else:
                raise
        v = self.cursor.execute("select * from t1").fetchone()[0]

        # To allow buffer --> db --> bytearray tests, always convert the input to the expected result type before
        # comparing.
        if type(value) is not resulttype:
            value = resulttype(value)

        self.assertEqual(v, value)


    def _test_strliketype(self, sqltype, value, resulttype=None, colsize=None):
        """
        The implementation for text, image, ntext, and binary.

        These types do not support comparison operators.
        """
        assert colsize is None or isinstance(colsize, int), colsize
        assert colsize is None or (value is None or colsize >= len(value))

        if colsize:
            sql = "create table t1(s %s(%s))" % (sqltype, colsize)
        else:
            sql = "create table t1(s %s)" % sqltype

        if resulttype is None:
            resulttype = type(value)

        self.cursor.execute(sql)
        self.cursor.execute("insert into t1 values(?)", value)
        result = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(result), resulttype)

        # To allow buffer --> db --> bytearray tests, always convert the input to the expected result type before
        # comparing.
        if type(value) is not resulttype:
            value = resulttype(value)

        self.assertEqual(result, value)


    #
    # varchar
    #

    def test_varchar_null(self):
        self._test_strtype('varchar', None, colsize=100)

    # Generate a test for each fencepost size: test_varchar_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('varchar', value, colsize=len(value))
        return t
    for value in UNICODE_SMALL_FENCEPOSTS:
        locals()['test_varchar_%s' % len(value)] = _maketest(value)

    # Also test varchar(max)
    def _maketest(value):
        def t(self):
            self._test_strtype('varchar', value, colsize='max')
        return t
    for value in UNICODE_MAX_FENCEPOSTS:
        locals()['test_varcharmax_%s' % len(value)] = _maketest(value)

    def test_varchar_many(self):
        self.cursor.execute("create table t1(c1 varchar(300), c2 varchar(300), c3 varchar(300))")

        v1 = 'ABCDEFGHIJ' * 30
        v2 = '0123456789' * 30
        v3 = '9876543210' * 30

        self.cursor.execute("insert into t1(c1, c2, c3) values (?,?,?)", v1, v2, v3);
        row = self.cursor.execute("select c1, c2, c3, len(c1) as l1, len(c2) as l2, len(c3) as l3 from t1").fetchone()

        self.assertEqual(v1, row.c1)
        self.assertEqual(v2, row.c2)
        self.assertEqual(v3, row.c3)

    def test_varchar_upperlatin(self):
        self._test_strtype('varchar', u'\u00e5', colsize=1)

    #
    # nvarchar
    #

    def test_nvarchar_null(self):
        self._test_strtype('nvarchar', None, colsize=100)

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strtype('nvarchar', value, colsize=len(value))
        return t
    for value in UNICODE_SMALL_FENCEPOSTS:
        locals()['test_nvarchar_%s' % len(value)] = _maketest(value)

    # Also test nvarchar(max)
    def _maketest(value):
        def t(self):
            self._test_strtype('nvarchar', value, colsize='max')
        return t
    for value in UNICODE_MAX_FENCEPOSTS:
        locals()['test_nvarcharmax_%s' % len(value)] = _maketest(value)

    def test_unicode_upperlatin(self):
        self._test_strtype('nvarchar', u'\u00e5', colsize=1)

    def test_unicode_longmax(self):
        # Issue 188:    Segfault when fetching NVARCHAR(MAX) data over 511 bytes

        ver = self.get_sqlserver_version()
        if ver < 9:            # 2005+
            return              # so pass / ignore
        self.cursor.execute("select cast(replicate(N'x', 512) as nvarchar(max))")

    def test_fast_executemany_to_local_temp_table(self):
        if self.driver_type_is('freetds'):
            warn('FREETDS_KNOWN_ISSUE - test_fast_executemany_to_local_temp_table: test cancelled.')
            return 
        v = u'Ώπα'
        self.cursor.execute("CREATE TABLE #issue295 (id INT IDENTITY PRIMARY KEY, txt NVARCHAR(50))")
        sql = "INSERT INTO #issue295 (txt) VALUES (?)"
        params = [(v,)]
        self.cursor.setinputsizes([(pyodbc.SQL_WVARCHAR, 50, 0)])
        self.cursor.fast_executemany = True
        self.cursor.executemany(sql, params)
        self.assertEqual(self.cursor.execute("SELECT txt FROM #issue295").fetchval(), v)

    def test_fast_executemany_to_datetime2(self):
        if self.handle_known_issues_for('freetds', print_reminder=True):
            warn('FREETDS_KNOWN_ISSUE - test_fast_executemany_to_datetime2: test cancelled.')
            return
        v = datetime(2019, 3, 12, 10, 0, 0, 123456)
        self.cursor.execute("CREATE TABLE ##issue540 (dt2 DATETIME2(2))")
        sql = "INSERT INTO ##issue540 (dt2) VALUES (?)"
        params = [(v,)]
        self.cursor.fast_executemany = True
        self.cursor.executemany(sql, params)
        self.assertEqual(self.cursor.execute("SELECT CAST(dt2 AS VARCHAR) FROM ##issue540").fetchval(), '2019-03-12 10:00:00.12')

    def test_fast_executemany_high_unicode(self):
        if self.handle_known_issues_for('freetds', print_reminder=True):
            warn('FREETDS_KNOWN_ISSUE - test_fast_executemany_high_unicode: test cancelled.')
            return
        v = u"🎥"
        self.cursor.fast_executemany = True
        self.cursor.execute("CREATE TABLE t1 (col1 nvarchar(max) null)")
        self.cursor.executemany("INSERT INTO t1 (col1) VALUES (?)", [[v,]])
        self.assertEqual(self.cursor.execute("SELECT * FROM t1").fetchone()[0], v)

    #
    # binary
    #

    def test_binary_null(self):
        self._test_strtype('varbinary', None, colsize=100)

    def test_large_binary_null(self):
        # Bug 1575064
        self._test_strtype('varbinary', None, colsize=4000)

    def test_binaryNull_object(self):
        self.cursor.execute("create table t1(n varbinary(10))")
        self.cursor.execute("insert into t1 values (?)", pyodbc.BinaryNull);

    # buffer

    def _maketest(value):
        def t(self):
            self._test_strtype('varbinary', buffer(value), resulttype=pyodbc.BINARY, colsize=len(value))
        return t
    for value in ANSI_SMALL_FENCEPOSTS:
        locals()['test_binary_buffer_%s' % len(value)] = _maketest(value)

    # bytearray

    if sys.hexversion >= 0x02060000:
        def _maketest(value):
            def t(self):
                self._test_strtype('varbinary', bytearray(value), colsize=len(value))
            return t
        for value in ANSI_SMALL_FENCEPOSTS:
            locals()['test_binary_bytearray_%s' % len(value)] = _maketest(value)

    # varbinary(max)
    def _maketest(value):
        def t(self):
            self._test_strtype('varbinary', buffer(value), resulttype=pyodbc.BINARY, colsize='max')
        return t
    for value in ANSI_MAX_FENCEPOSTS:
        locals()['test_binarymax_buffer_%s' % len(value)] = _maketest(value)

    # bytearray

    if sys.hexversion >= 0x02060000:
        def _maketest(value):
            def t(self):
                self._test_strtype('varbinary', bytearray(value), colsize='max')
            return t
        for value in ANSI_MAX_FENCEPOSTS:
            locals()['test_binarymax_bytearray_%s' % len(value)] = _maketest(value)

    #
    # image
    #

    def test_image_null(self):
        self._test_strliketype('image', None, type(None))

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strliketype('image', buffer(value), pyodbc.BINARY)
        return t
    for value in ANSI_LARGE_FENCEPOSTS:
        locals()['test_image_buffer_%s' % len(value)] = _maketest(value)

    if sys.hexversion >= 0x02060000:
        # Python 2.6+ supports bytearray, which pyodbc considers varbinary.

        # Generate a test for each fencepost size: test_unicode_0, etc.
        def _maketest(value):
            def t(self):
                self._test_strtype('image', bytearray(value))
            return t
        for value in ANSI_LARGE_FENCEPOSTS:
            locals()['test_image_bytearray_%s' % len(value)] = _maketest(value)

    def test_image_upperlatin(self):
        self._test_strliketype('image', buffer('á'), pyodbc.BINARY)

    #
    # text
    #

    # def test_empty_text(self):
    #     self._test_strliketype('text', bytearray(''))

    def test_null_text(self):
        self._test_strliketype('text', None, type(None))

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strliketype('text', value)
        return t
    for value in UNICODE_SMALL_FENCEPOSTS:
        locals()['test_text_buffer_%s' % len(value)] = _maketest(value)

    def test_text_upperlatin(self):
        self._test_strliketype('text', u'á')

    #
    # xml
    #

    # def test_empty_xml(self):
    #     self._test_strliketype('xml', bytearray(''))

    def test_null_xml(self):
        self._test_strliketype('xml', None, type(None))

    # Generate a test for each fencepost size: test_unicode_0, etc.
    def _maketest(value):
        def t(self):
            self._test_strliketype('xml', value)
        return t
    for value in UNICODE_SMALL_FENCEPOSTS:
        locals()['test_xml_buffer_%s' % len(value)] = _maketest(value)

    def test_xml_str(self):
        # SQL Server treats XML like *binary* data.
        # See https://msdn.microsoft.com/en-us/library/ms131375.aspx
        #
        # The real problem with this is that we *don't* know that a value is
        # XML when we write it to the database.  It is either an `str` or a
        # `unicode` object, so we're going to convert it into one of *two*
        # different formats.
        #
        # When we read it out of the database, all we know is that it is XML
        # and we don't know how it was encoded so we don't know how to decode
        # it.  Since almost everyone treats XML as Unicode nowdays, we're going
        # to decode XML as Unicode.  Force your XML to Unicode before writing
        # to the database.  (Otherwise, set a global encoder for the XMl type.)
        ascii = 'test'
        val = unicode(ascii)
        self.cursor.execute("create table t1(a xml)")
        self.cursor.execute("insert into t1 values (?)", val)
        result = self.cursor.execute("select a from t1").fetchval()
        self.assertEqual(result, val)

    def test_xml_upperlatin(self):
        val = u'á'
        self.cursor.execute("create table t1(a xml)")
        self.cursor.execute("insert into t1 values (?)", val)
        result = self.cursor.execute("select a from t1").fetchval()
        self.assertEqual(result, val)

    #
    # bit
    #

    def test_bit(self):
        value = True
        self.cursor.execute("create table t1(b bit)")
        self.cursor.execute("insert into t1 values (?)", value)
        v = self.cursor.execute("select b from t1").fetchone()[0]
        self.assertEqual(type(v), bool)
        self.assertEqual(v, value)

    #
    # decimal
    #

    def _decimal(self, precision, scale, negative):
        # From test provided by planders (thanks!) in Issue 91

        self.cursor.execute("create table t1(d decimal(%s, %s))" % (precision, scale))

        # Construct a decimal that uses the maximum precision and scale.
        decStr = '9' * (precision - scale)
        if scale:
            decStr = decStr + "." + '9' * scale
        if negative:
            decStr = "-" + decStr
        value = Decimal(decStr)

        self.cursor.execute("insert into t1 values(?)", value)

        v = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(v, value)

    def _maketest(p, s, n):
        def t(self):
            self._decimal(p, s, n)
        return t
    for (p, s, n) in [ (1,  0,  False),
                       (1,  0,  True),
                       (6,  0,  False),
                       (6,  2,  False),
                       (6,  4,  True),
                       (6,  6,  True),
                       (38, 0,  False),
                       (38, 10, False),
                       (38, 38, False),
                       (38, 0,  True),
                       (38, 10, True),
                       (38, 38, True) ]:
        locals()['test_decimal_%s_%s_%s' % (p, s, n and 'n' or 'p')] = _maketest(p, s, n)


    def test_decimal_e(self):
        """Ensure exponential notation decimals are properly handled"""
        value = Decimal((0, (1, 2, 3), 5)) # prints as 1.23E+7
        self.cursor.execute("create table t1(d decimal(10, 2))")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(result, value)

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

    def test_empty_string_encoding(self):
        self.cnxn.setdecoding(pyodbc.SQL_CHAR, encoding='shift_jis')
        value = ""
        self.cursor.execute("create table t1(s varchar(20))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(v, value)

    def test_fixed_char(self):
        value = "testing"
        self.cursor.execute("create table t1(s char(7))")
        self.cursor.execute("insert into t1 values(?)", "testing")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(v, value)

    def test_empty_unicode(self):
        self.cursor.execute("create table t1(s nvarchar(20))")
        self.cursor.execute("insert into t1 values(?)", u"")

    def test_empty_unicode_encoding(self):
        self.cnxn.setdecoding(pyodbc.SQL_CHAR, encoding='shift_jis')
        value = ""
        self.cursor.execute("create table t1(s nvarchar(20))")
        self.cursor.execute("insert into t1 values(?)", value)
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(v, value)

    def test_unicode_query(self):
        self.cursor.execute(u"select 1")

    # From issue #206
    def _maketest(value):
        def t(self):
            self._test_strtype('nvarchar', value, colsize=len(value))
        return t
    locals()['test_chinese_param'] = _maketest(u'我的')

    def test_chinese(self):
        v = u'我的'
        self.cursor.execute(u"SELECT N'我的' AS [Name]")
        row = self.cursor.fetchone()
        self.assertEqual(row[0], v)

        self.cursor.execute(u"SELECT N'我的' AS [Name]")
        rows = self.cursor.fetchall()
        self.assertEqual(rows[0][0], v)

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
        self.assertEqual(type(result), datetime)
        self.assertEqual(value, result)

    def test_datetime_fraction(self):
        # SQL Server supports milliseconds, but Python's datetime supports nanoseconds, so the most granular datetime
        # supported is xxx000.

        value = datetime(2007, 1, 15, 3, 4, 5, 123000)

        self.cursor.execute("create table t1(dt datetime)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), datetime)
        self.assertEqual(value, result)

    def test_datetime_fraction_rounded(self):
        # SQL Server supports milliseconds, but Python's datetime supports nanoseconds.  pyodbc rounds down to what the
        # database supports.

        full    = datetime(2007, 1, 15, 3, 4, 5, 123456)
        rounded = datetime(2007, 1, 15, 3, 4, 5, 123000)

        self.cursor.execute("create table t1(dt datetime)")
        self.cursor.execute("insert into t1 values (?)", full)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), datetime)
        self.assertEqual(rounded, result)

    def test_date(self):
        ver = self.get_sqlserver_version()
        if ver < 10:            # 2008 only
            return              # so pass / ignore

        value = date.today()

        self.cursor.execute("create table t1(d date)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(type(result), date)
        self.assertEqual(value, result)

    def test_time(self):
        ver = self.get_sqlserver_version()
        if ver < 10:            # 2008 only
            return              # so pass / ignore

        value = datetime.now().time()

        # We aren't yet writing values using the new extended time type so the value written to the database is only
        # down to the second.
        value = value.replace(microsecond=0)

        self.cursor.execute("create table t1(t time)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select t from t1").fetchone()[0]
        self.assertEqual(type(result), time)
        self.assertEqual(value, result)

    def test_datetime2(self):
        value = datetime(2007, 1, 15, 3, 4, 5)

        self.cursor.execute("create table t1(dt datetime2)")
        self.cursor.execute("insert into t1 values (?)", value)

        result = self.cursor.execute("select dt from t1").fetchone()[0]
        self.assertEqual(type(result), datetime)
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
        input = 3000000000
        self.cursor.execute("create table t1(d bigint)")
        self.cursor.execute("insert into t1 values (?)", input)
        result = self.cursor.execute("select d from t1").fetchone()[0]
        self.assertEqual(result, input)

    def test_overflow_int(self):
        # python allows integers of any size, bigger than an 8 byte int can contain
        input = 9999999999999999999999999999999999999
        self.cursor.execute("create table t1(d bigint)")
        self.cnxn.commit()
        self.assertRaises(OverflowError, self.cursor.execute, "insert into t1 values (?)", input)
        result = self.cursor.execute("select * from t1").fetchall()
        self.assertEqual(result, [])

    def test_float(self):
        value = 1234.567
        self.cursor.execute("create table t1(n float)")
        self.cursor.execute("insert into t1 values (?)", value)
        result = self.cursor.execute("select n from t1").fetchone()[0]
        self.assertEqual(result, value)

    def test_denorm_float(self):
        value = 0.00012345
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

    def test_non_numeric_float(self):
        self.cursor.execute("create table t1(d float)")
        self.cnxn.commit()
        for input in (float('+Infinity'), float('-Infinity'), float('NaN')):
            self.assertRaises(pyodbc.ProgrammingError, self.cursor.execute, "insert into t1 values (?)", input)
        result = self.cursor.execute("select * from t1").fetchall()
        self.assertEqual(result, [])


    #
    # stored procedures
    #

    # def test_callproc(self):
    #     "callproc with a simple input-only stored procedure"
    #     pass

    def test_sp_results(self):
        self.cursor.execute(
            """
            Create procedure proc1
            AS
              select top 10 name, id, xtype, refdate
              from sysobjects
            """)
        rows = self.cursor.execute("exec proc1").fetchall()
        self.assertEqual(type(rows), list)
        self.assertEqual(len(rows), 10) # there has to be at least 10 items in sysobjects
        self.assertEqual(type(rows[0].refdate), datetime)


    def test_sp_results_from_temp(self):

        # Note: I've used "set nocount on" so that we don't get the number of rows deleted from #tmptable.
        # If you don't do this, you'd need to call nextset() once to skip it.

        self.cursor.execute(
            """
            Create procedure proc1
            AS
              set nocount on
              select top 10 name, id, xtype, refdate
              into #tmptable
              from sysobjects

              select * from #tmptable
            """)
        self.cursor.execute("exec proc1")
        self.assertTrue(self.cursor.description is not None)
        self.assertTrue(len(self.cursor.description) == 4)

        rows = self.cursor.fetchall()
        self.assertEqual(type(rows), list)
        self.assertEqual(len(rows), 10) # there has to be at least 10 items in sysobjects
        self.assertEqual(type(rows[0].refdate), datetime)


    def test_sp_results_from_vartbl(self):
        self.cursor.execute(
            """
            Create procedure proc1
            AS
              set nocount on
              declare @tmptbl table(name varchar(100), id int, xtype varchar(4), refdate datetime)

              insert into @tmptbl
              select top 10 name, id, xtype, refdate
              from sysobjects

              select * from @tmptbl
            """)
        self.cursor.execute("exec proc1")
        rows = self.cursor.fetchall()
        self.assertEqual(type(rows), list)
        self.assertEqual(len(rows), 10) # there has to be at least 10 items in sysobjects
        self.assertEqual(type(rows[0].refdate), datetime)

    def test_sp_with_dates(self):
        # Reported in the forums that passing two datetimes to a stored procedure doesn't work.
        self.cursor.execute(
            """
            if exists (select * from dbo.sysobjects where id = object_id(N'[test_sp]') and OBJECTPROPERTY(id, N'IsProcedure') = 1)
              drop procedure [dbo].[test_sp]
            """)
        self.cursor.execute(
            """
            create procedure test_sp(@d1 datetime, @d2 datetime)
            AS
              declare @d as int
              set @d = datediff(year, @d1, @d2)
              select @d
            """)
        self.cursor.execute("exec test_sp ?, ?", datetime.now(), datetime.now())
        rows = self.cursor.fetchall()
        self.assertTrue(rows is not None)
        self.assertTrue(rows[0][0] == 0)   # 0 years apart

    def test_sp_with_none(self):
        # Reported in the forums that passing None caused an error.
        self.cursor.execute(
            """
            if exists (select * from dbo.sysobjects where id = object_id(N'[test_sp]') and OBJECTPROPERTY(id, N'IsProcedure') = 1)
              drop procedure [dbo].[test_sp]
            """)
        self.cursor.execute(
            """
            create procedure test_sp(@x varchar(20))
            AS
              declare @y varchar(20)
              set @y = @x
              select @y
            """)
        self.cursor.execute("exec test_sp ?", None)
        rows = self.cursor.fetchall()
        self.assertTrue(rows is not None)
        self.assertTrue(rows[0][0] == None)   # 0 years apart


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
        "Ensure rowcount is reset after DDL"
        
        ddl_rowcount = 0 if self.driver_type_is('freetds') else -1

        self.cursor.execute("create table t1(i int)")
        count = 4
        for i in range(count):
            self.cursor.execute("insert into t1 values (?)", i)
        self.assertEqual(self.cursor.rowcount, 1)

        self.cursor.execute("create table t2(i int)")
        self.assertEqual(self.cursor.rowcount, ddl_rowcount)

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

    def table_with_spaces(self):
        "Ensure we can select using [x z] syntax"

        try:
            self.cursor.execute("create table [test one](int n)")
            self.cursor.execute("insert into [test one] values(1)")
            self.cursor.execute("select * from [test one]")
            v = self.cursor.fetchone()[0]
            self.assertEqual(v, 1)
        finally:
            self.cnxn.rollback()

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
        self.assertEqual(type(v), unicode)
        self.assertEqual(v, "testing")

        self.cursor.execute("select s into t2 from t1")
        v = self.cursor.execute("select * from t1").fetchone()[0]
        self.assertEqual(type(v), unicode)
        self.assertEqual(v, "testing")


    def test_money(self):
        d = Decimal('123456.78')
        self.cursor.execute("create table t1(i int identity(1,1), m money)")
        self.cursor.execute("insert into t1(m) values (?)", d)
        v = self.cursor.execute("select m from t1").fetchone()[0]
        self.assertEqual(v, d)


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

    def test_executemany_dae_0(self):
        """
        DAE for 0-length value
        """
        self.cursor.execute("create table t1(a nvarchar(max))")

        self.cursor.fast_executemany = True
        self.cursor.executemany("insert into t1(a) values(?)", [['']])

        self.assertEqual(self.cursor.execute("select a from t1").fetchone()[0], '')

        self.cursor.fast_executemany = False

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

        self.cursor.execute("create table t1(c1 int identity(1, 1), c2 varchar(300), c3 varchar(300))")
        self.cursor.execute("insert into t1(c2, c3) values (?,?)", v2, v3)

        row = self.cursor.execute("select c2, c3, c2 + c3 as both from t1").fetchone()

        self.assertEqual(row.both, v2 + v3)

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
        self.assertTrue(rows is not None)
        self.assertTrue(len(rows) == 3)

    def test_autocommit(self):
        self.assertEqual(self.cnxn.autocommit, False)

        othercnxn = pyodbc.connect(self.connection_string, autocommit=True)
        self.assertEqual(othercnxn.autocommit, True)

        othercnxn.autocommit = False
        self.assertEqual(othercnxn.autocommit, False)

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


    def test_sqlserver_callproc(self):
        try:
            self.cursor.execute("drop procedure pyodbctest")
            self.cnxn.commit()
        except:
            pass

        self.cursor.execute("create table t1(s varchar(10))")
        self.cursor.execute("insert into t1 values(?)", "testing")

        self.cursor.execute("""
                            create procedure pyodbctest @var1 varchar(32)
                            as
                            begin
                              select s
                              from t1
                            return
                            end
                            """)
        self.cnxn.commit()

        # for row in self.cursor.procedureColumns('pyodbctest'):
        #     print row.procedure_name, row.column_name, row.column_type, row.type_name

        self.cursor.execute("exec pyodbctest 'hi'")

        # print self.cursor.description
        # for row in self.cursor:
        #     print row.s

    def test_skip(self):
        # Insert 1, 2, and 3.  Fetch 1, skip 2, fetch 3.

        self.cursor.execute("create table t1(id int)");
        for i in range(1, 5):
            self.cursor.execute("insert into t1 values(?)", i)
        self.cursor.execute("select id from t1 order by id")
        self.assertEqual(self.cursor.fetchone()[0], 1)
        self.cursor.skip(2)
        self.assertEqual(self.cursor.fetchone()[0], 4)

    def test_timeout(self):
        self.assertEqual(self.cnxn.timeout, 0) # defaults to zero (off)

        self.cnxn.timeout = 30
        self.assertEqual(self.cnxn.timeout, 30)

        self.cnxn.timeout = 0
        self.assertEqual(self.cnxn.timeout, 0)

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
        self.cursor.execute("insert into t1 values (1, 'abc', '1.23')")
        self.cursor.execute("select * from t1")

        # (I'm not sure the precision of an int is constant across different versions, bits, so I'm hand checking the
        # items I do know.

        # int
        t = self.cursor.description[0]
        self.assertEqual(t[0], 'n')
        self.assertEqual(t[1], int)
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

        # varchar(8)
        t = self.cursor.description[1]
        self.assertEqual(t[0], 's')
        self.assertEqual(t[1], str)
        self.assertEqual(t[4], 8)       # precision
        self.assertEqual(t[5], 0)       # scale
        self.assertEqual(t[6], True)    # nullable

        # decimal(5, 2)
        t = self.cursor.description[2]
        self.assertEqual(t[0], 'd')
        self.assertEqual(t[1], Decimal)
        self.assertEqual(t[4], 5)       # precision
        self.assertEqual(t[5], 2)       # scale
        self.assertEqual(t[6], True)    # nullable


    def test_none_param(self):
        "Ensure None can be used for params other than the first"
        # Some driver/db versions would fail if NULL was not the first parameter because SQLDescribeParam (only used
        # with NULL) could not be used after the first call to SQLBindParameter.  This means None always worked for the
        # first column, but did not work for later columns.
        #
        # If SQLDescribeParam doesn't work, pyodbc would use VARCHAR which almost always worked.  However,
        # binary/varbinary won't allow an implicit conversion.

        self.cursor.execute("create table t1(n int, blob varbinary(max))")
        self.cursor.execute("insert into t1 values (1, newid())")
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEqual(row.n, 1)
        self.assertEqual(type(row.blob), bytearray)

        sql = "update t1 set n=?, blob=?"
        try:
            self.cursor.execute(sql, 2, None)
        except pyodbc.DataError:
            if self.driver_type_is('freetds'):
                # FREETDS_KNOWN_ISSUE
                #
                # cnxn.getinfo(pyodbc.SQL_DESCRIBE_PARAMETER) returns False for FreeTDS, so
                # pyodbc can't call SQLDescribeParam to get the correct parameter type.
                # This can lead to errors being returned from SQL Server when sp_prepexec is called, 
                # e.g., "Implicit conversion from data type varchar to varbinary(max) is not allowed." 
                #
                # So at least verify that the user can manually specify the parameter type
                self.cursor.setinputsizes([(), (pyodbc.SQL_VARBINARY, None, None)])
                self.cursor.execute(sql, 2, None)
            else:
                raise
        row = self.cursor.execute("select * from t1").fetchone()
        self.assertEqual(row.n, 2)
        self.assertEqual(row.blob, None)


    def test_output_conversion(self):
        def convert(value):
            # `value` will be a string.  We'll simply add an X at the beginning at the end.
            return 'X' + value + 'X'
        self.cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert)
        self.cursor.execute("create table t1(n int, v varchar(10))")
        self.cursor.execute("insert into t1 values (1, '123.45')")
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, 'X123.45X')

        # Now clear the conversions and try again.  There should be no Xs this time.
        self.cnxn.clear_output_converters()
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, '123.45')


    def test_too_large(self):
        """Ensure error raised if insert fails due to truncation"""
        value = 'x' * 1000
        self.cursor.execute("create table t1(s varchar(800))")
        def test():
            self.cursor.execute("insert into t1 values (?)", value)
        self.assertRaises(pyodbc.DataError, test)

    def test_geometry_null_insert(self):
        def convert(value):
            return value

        self.cnxn.add_output_converter(-151, convert) # -151 is SQL Server's geometry
        self.cursor.execute("create table t1(n int, v geometry)")
        self.cursor.execute("insert into t1 values (?, ?)", 1, None)
        value = self.cursor.execute("select v from t1").fetchone()[0]
        self.assertEqual(value, None)
        self.cnxn.clear_output_converters()

    def test_login_timeout(self):
        # This can only test setting since there isn't a way to cause it to block on the server side.
        cnxns = pyodbc.connect(self.connection_string, timeout=2)

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

    def test_large_update_nodata(self):
        self.cursor.execute('create table t1(a varbinary(max))')
        hundredkb = bytearray('x'*100*1024)
        self.cursor.execute('update t1 set a=? where 1=0', (hundredkb,))

    def test_func_param(self):
        self.cursor.execute('''
                            create function func1 (@testparam varchar(4))
                            returns @rettest table (param varchar(4))
                            as
                            begin
                                insert @rettest
                                select @testparam
                                return
                            end
                            ''')
        self.cnxn.commit()
        value = self.cursor.execute("select * from func1(?)", 'test').fetchone()[0]
        self.assertEqual(value, 'test')

    def test_no_fetch(self):
        # Issue 89 with FreeTDS: Multiple selects (or catalog functions that issue selects) without fetches seem to
        # confuse the driver.
        self.cursor.execute('select 1')
        self.cursor.execute('select 1')
        self.cursor.execute('select 1')

    def test_drivers(self):
        drivers = pyodbc.drivers()
        self.assertEqual(list, type(drivers))
        self.assertTrue(len(drivers) > 0)

        m = re.search('DRIVER={?([^}]+?)}?;', self.connection_string, re.IGNORECASE)
        current = m.group(1)
        self.assertTrue(current in drivers)

    def test_prepare_cleanup(self):
        # When statement is prepared, it is kept in case the next execute uses the same statement.  This must be
        # removed when a non-execute statement is used that returns results, such as SQLTables.

        self.cursor.execute("select top 1 name from sysobjects where name = ?", "bogus")
        self.cursor.fetchone()

        self.cursor.tables("bogus")

        self.cursor.execute("select top 1 name from sysobjects where name = ?", "bogus")
        self.cursor.fetchone()

    def test_exc_integrity(self):
        "Make sure an IntegretyError is raised"
        # This is really making sure we are properly encoding and comparing the SQLSTATEs.
        self.cursor.execute("create table t1(s1 varchar(10) primary key)")
        self.cursor.execute("insert into t1 values ('one')")
        self.assertRaises(pyodbc.IntegrityError, self.cursor.execute, "insert into t1 values ('one')")

    def test_emoticons_as_parameter(self):
        # https://github.com/mkleehammer/pyodbc/issues/423
        #
        # When sending a varchar parameter, pyodbc is supposed to set ColumnSize to the number
        # of characters.  Ensure it works even with 4-byte characters.
        #
        # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

        v = "x \U0001F31C z"

        self.cursor.execute("create table t1(s varchar(100))")
        self.cursor.execute("insert into t1 values (?)", v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)

    def test_emoticons_as_literal(self):
        # similar to `test_emoticons_as_parameter`, above, except for Unicode literal
        #
        # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

        v = "x \U0001F31C z"

        self.cursor.execute("create table t1(s varchar(100))")
        self.cursor.execute("insert into t1 values (N'%s')" % v)

        result = self.cursor.execute("select s from t1").fetchone()[0]

        self.assertEqual(result, v)
        
    def test_tvp(self):
        # https://github.com/mkleehammer/pyodbc/issues/290
        #
        # pyodbc supports queries with table valued parameters in sql server
        #

        if self.handle_known_issues_for('freetds', print_reminder=True):
            warn('FREETDS_KNOWN_ISSUE - test_tvp: test cancelled.')
            return

        # (Don't use "if exists" since older SQL Servers don't support it.)
        try:
            self.cursor.execute("drop procedure SelectTVP")
        except:
            pass
        try:
            self.cursor.execute("drop type TestTVP")
        except:
            pass
        self.cursor.commit()
        
        query = "CREATE TYPE TestTVP AS TABLE("\
                "c01 VARCHAR(255),"\
                "c02 VARCHAR(MAX),"\
                "c03 VARBINARY(255),"\
                "c04 VARBINARY(MAX),"\
                "c05 BIT,"\
                "c06 DATE,"\
                "c07 TIME,"\
                "c08 DATETIME2(5),"\
                "c09 BIGINT,"\
                "c10 FLOAT,"\
                "c11 NUMERIC(38, 24),"\
                "c12 UNIQUEIDENTIFIER)"

        self.cursor.execute(query)
        self.cursor.commit()
        self.cursor.execute("CREATE PROCEDURE SelectTVP @TVP TestTVP READONLY AS SELECT * FROM @TVP;")
        self.cursor.commit()

        long_string = ''
        long_bytearray = []
        for i in range(255):
            long_string += chr((i % 95) + 32)
            long_bytearray.append(i % 255)
            
        very_long_string = ''
        very_long_bytearray = []
        for i in range(2000000):
            very_long_string += chr((i % 95) + 32)
            very_long_bytearray.append(i % 255)
            
        c01 = ['abc', '', long_string]

        c02 = ['abc', '', very_long_string]

        c03 = [bytearray([0xD1, 0xCE, 0xFA, 0xCE]),
               bytearray([0x00, 0x01, 0x02, 0x03, 0x04]),
               bytearray(long_bytearray)]
               
        c04 = [bytearray([0x0F, 0xF1, 0xCE, 0xCA, 0xFE]),
               bytearray([0x00, 0x01, 0x02, 0x03, 0x04, 0x05]),
               bytearray(very_long_bytearray)]

        c05 = [1, 0, 1]

        c06 = [date(1997, 8, 29),
               date(1, 1, 1),
               date(9999, 12, 31)]
               
        c07 = [time(9, 13, 39),
               time(0, 0, 0),
               time(23, 59, 59)]
               
        c08 = [datetime(2018, 11, 13, 13, 33, 26, 298420),
               datetime(1, 1, 1, 0, 0, 0, 0),
               datetime(9999, 12, 31, 23, 59, 59, 999990)]
               
        c09 = [1234567, -9223372036854775808, 9223372036854775807]

        c10 = [3.14, -1.79E+308, 1.79E+308]

        c11 = [Decimal('31234567890123.141243449787580175325274'),
               Decimal(             '0.000000000000000000000001'),
               Decimal('99999999999999.999999999999999999999999')]

        c12 = ['4FE34A93-E574-04CC-200A-353F0D1770B1',
               '33F7504C-2BAC-1B83-01D1-7434A7BA6A17',
               'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF']

        param_array = []       

        for i in range (3):
            param_array.append([c01[i], c02[i], c03[i], c04[i], c05[i], c06[i], c07[i], c08[i], c09[i], c10[i], c11[i], c12[i]])

        success = True

        try:
            result_array = self.cursor.execute("exec SelectTVP ?",[param_array]).fetchall()
        except Exception as ex:
            print("Failed to execute SelectTVP")
            print("Exception: [" + type(ex).__name__ + "]" , ex.args)
            
            success = False
        else:
            for r in range(len(result_array)):
                for c in range(len(result_array[r])):
                    if(result_array[r][c] != param_array[r][c]):
                        print("Mismatch at row " + str(r+1) + ", column " + str(c+1) + "; expected:", param_array[r][c] , " received:", result_array[r][c])
                        success = False

        self.assertEqual(success, True)


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
        connection_string = load_setup_connection_string('sqlservertests')

        if not connection_string:
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args[0]

    cnxn = pyodbc.connect(connection_string)
    print_library_info(cnxn)
    cnxn.close()

    suite = load_tests(SqlServerTestCase, options.test, connection_string)

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(suite)


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.

    add_to_path()

    import pyodbc
    main()
