# ignore naive dates/datetimes (DTZnnn):
# ruff: noqa: DTZ001, DTZ005, DTZ011

import ctypes
import gc
import os
import random
import re
import uuid
from collections.abc import Iterator
from decimal import Decimal, localcontext
from datetime import date, time, datetime, timedelta, timezone
from functools import lru_cache

import pyodbc
import pytest


# WARNING: Wow Microsoft always manages to do the stupidest thing possible always trying to be
# smarter than everyone.  I worked with their APIs for since before "OLE" and it has always
# been a nanny state.  They won't read the UID and PWD from odbc.ini because it isn't secure.
# Really?  Less secure than what?  The next hack someone is going to use.  Do the straight
# forward thing and explain how to secure it.  it isn't their business how I deploy and secure.
#
# For every other DB we use a single default DSN but you can pass your own via an environment
# variable.  For SS, we can't just use a default DSN unless you want to go trusted.  (Which is
# more secure?  No.)   It'll be put into .bashrc most likely.  Way to go.  Now I'll go rename
# all of the others to DB specific names instead of PYODBC_CNXNSTR.  Hot garbage as usual.

CNXNSTR = os.environ.get('PYODBC_SQLSERVER', 'DSN=pyodbc-sqlserver')


def connect(autocommit=False, attrs_before=None):
    return pyodbc.connect(CNXNSTR, autocommit=autocommit, attrs_before=attrs_before)


DRIVER = connect().getinfo(pyodbc.SQL_DRIVER_NAME)
DRIVER_VERSION = tuple(int(n) for n in connect().getinfo(pyodbc.SQL_DRIVER_VER).split("."))
IS_FREETDS   = bool(re.search(r'(tsodbc|tdsodbc)', DRIVER, flags=re.IGNORECASE))
IS_MSODBCSQL = bool(re.search(r'(msodbcsql|sqlncli|sqlsrv32\.dll)', DRIVER, re.IGNORECASE))
IS_BCP_DRIVER = bool(re.search(r'msodbcsql', DRIVER, re.I)) and DRIVER_VERSION[0] >= 13


def _get_sqlserver_year():
    """
    Returns the release year of the current version of SQL Server, used to skip tests for
    features that are not supported.  If the current DB is not SQL Server, 0 is returned.
    """
    # We used to use the major version, but most documentation on the web refers to the year
    # (e.g. SQL Server 2019) so we'll use that for skipping tests that do not apply.
    if not IS_MSODBCSQL:
        return 0
    cnxn = connect()
    cursor = cnxn.cursor()
    row = cursor.execute("exec master..xp_msver 'ProductVersion'").fetchone()
    major = row.Character_Value.split('.', 1)[0]
    return {
        # https://sqlserverbuilds.blogspot.com/
        '8': 2000, '9': 2005, '10': 2008, '11': 2012, '12': 2014,
        '13': 2016, '14': 2017, '15': 2019, '16': 2022, '17': 2025
    }[major]


SQLSERVER_YEAR = _get_sqlserver_year()


@pytest.fixture
def cursor() -> Iterator[pyodbc.Cursor]:
    cnxn = connect()
    cur = cnxn.cursor()

    cur.execute("drop table if exists t1")
    cur.execute("drop table if exists t2")
    cur.execute("drop table if exists t3")
    cnxn.commit()

    yield cur

    if not cnxn.closed:
        cur.close()
        cnxn.close()


# The bulk copy (BCP) API needs a connection opened with SQL_COPT_SS_BCP.  The connection pool
# matches on the connection string, so BCP connections get their own APP name; otherwise a
# pooled connection opened without the attribute could be handed back.
BCP_CNXNSTR = CNXNSTR + ';APP=pyodbc-bcp-tests'


def bcp_connect(autocommit=True):
    return pyodbc.connect(BCP_CNXNSTR, autocommit=autocommit,
                          attrs_before={pyodbc.SQL_COPT_SS_BCP: pyodbc.SQL_BCP_ON})


@pytest.fixture
def bcp_cursor() -> Iterator[pyodbc.Cursor]:
    cnxn = bcp_connect()
    cur = cnxn.cursor()

    cur.execute("set lock_timeout 30000")
    cur.execute("drop table if exists t1")
    cur.execute("drop table if exists t2")

    yield cur

    if not cnxn.closed:
        cur.close()
        cnxn.close()


def test_text(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'text')


def test_varchar(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'varchar')


def test_nvarchar(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'nvarchar')


def test_varbinary(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'varbinary')


@pytest.mark.skipif(SQLSERVER_YEAR < 2005, reason='(max) not supported until 2005')
def test_unicode_longmax(cursor: pyodbc.Cursor):
    # Issue 188:	Segfault when fetching NVARCHAR(MAX) data over 511 bytes
    cursor.execute("select cast(replicate(N'x', 512) as nvarchar(max))")


def test_char(cursor: pyodbc.Cursor):
    value = "testing"
    cursor.execute("create table t1(s char(7))")
    cursor.execute("insert into t1 values(?)", "testing")
    v = cursor.execute("select * from t1").fetchone()[0]
    assert v == value


def test_int(cursor: pyodbc.Cursor):
    _test_scalar(cursor, 'int', [None, -1, 0, 1, 12345678])


def test_bigint(cursor: pyodbc.Cursor):
    _test_scalar(cursor, 'bigint', [None, -1, 0, 1, 0x123456789, 0x7FFFFFFF, 0xFFFFFFFF,
                                    0x123456789])


def test_overflow_int(cursor: pyodbc.Cursor):
    # python allows integers of any size, bigger than an 8 byte int can contain
    value = 9999999999999999999999999999999999999
    cursor.execute("create table t1(d bigint)")
    with pytest.raises(OverflowError):
        cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select * from t1").fetchall()
    assert result == []


def test_float(cursor: pyodbc.Cursor):
    _test_scalar(cursor, 'float', [None, -200, -1, 0, 1, 1234.5, -200, .00012345])


def test_non_numeric_float(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(d float)")
    for value in (float('+Infinity'), float('-Infinity'), float('NaN')):
        with pytest.raises(pyodbc.ProgrammingError):
            cursor.execute("insert into t1 values (?)", value)
    if IS_FREETDS:
        # Give the driver a chance to unconfuse itself. Without creating and closing
        # this second connection, this test will pass, but the next test in the queue
        # depending on the cursor-generator fixture will fail when that fixture tries
        # to commit the "DROP TABLE IF EXISTS…" statements, triggering an exception.
        # For details refer to https://github.com/FreeTDS/freetds/issues/718.
        conn2 = connect()
        conn2.close()


def test_drivers():
    p = pyodbc.drivers()
    assert isinstance(p, list)


def test_datasources():
    p = pyodbc.dataSources()
    assert isinstance(p, dict)


def test_getinfo_string():
    cnxn = connect()
    value = cnxn.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
    assert isinstance(value, str)


def test_getinfo_bool():
    cnxn = connect()
    value = cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
    assert isinstance(value, bool)


def test_getinfo_int():
    cnxn = connect()
    value = cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
    assert isinstance(value, int)


def test_getinfo_smallint():
    cnxn = connect()
    value = cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
    assert isinstance(value, int)


def test_no_fetch(cursor: pyodbc.Cursor):
    # Issue 89 with FreeTDS: Multiple selects (or catalog functions that issue selects) without
    # fetches seem to confuse the driver.
    cursor.execute('select 1')
    cursor.execute('select 1')
    cursor.execute('select 1')


def test_decode_meta(cursor: pyodbc.Cursor):
    """
    Ensure column names with non-ASCII characters are converted using the configured encodings.
    """
    # This is from GitHub issue #190
    cursor.execute("create table t1(a int)")
    cursor.execute("insert into t1 values (1)")
    cursor.execute('select a as "Tipología" from t1')
    assert cursor.description[0][0] == "Tipología"


def test_exc_integrity(cursor: pyodbc.Cursor):
    "Make sure an IntegretyError is raised"
    # This is really making sure we are properly encoding and comparing the SQLSTATEs.
    cursor.execute("create table t1(s1 varchar(10) primary key)")
    cursor.execute("insert into t1 values ('one')")
    with pytest.raises(pyodbc.IntegrityError):
        cursor.execute("insert into t1 values ('one')")


def test_multiple_bindings(cursor: pyodbc.Cursor):
    "More than one bind and select on a cursor"
    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (?)", 1)
    cursor.execute("insert into t1 values (?)", 2)
    cursor.execute("insert into t1 values (?)", 3)
    for _ in range(3):
        cursor.execute("select n from t1 where n < ?", 10)
        cursor.execute("select n from t1 where n < 3")


def test_different_bindings(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(n int)")
    cursor.execute("create table t2(d datetime)")
    cursor.execute("insert into t1 values (?)", 1)
    cursor.execute("insert into t2 values (?)", datetime.now())


SMALL_FENCEPOST_SIZES = [None, 0, 1, 255, 256, 510, 511, 512, 1023, 1024, 2047, 2048, 4000]
LARGE_FENCEPOST_SIZES = SMALL_FENCEPOST_SIZES + [4095, 4096, 4097, 10 * 1024, 20 * 1024]


def _test_vartype(cursor: pyodbc.Cursor, datatype):

    is_binary = datatype in {"blob", "varbinary"}
    encoding = "utf8" if is_binary else None

    if datatype == 'text':
        lengths = LARGE_FENCEPOST_SIZES
    else:
        lengths = SMALL_FENCEPOST_SIZES

    assert cursor.connection.readvar_initsize == 4096
    for initsize in (None, 1024 * 1024, 0):
        if initsize is not None:
            cursor.connection.readvar_initsize = initsize

        if datatype == 'text':
            cursor.execute(f"create table t1(c1 {datatype})")
        else:
            maxlen = lengths[-1]
            cursor.execute(f"create table t1(c1 {datatype}({maxlen}))")

        for length in lengths:

            # FreeTDS did not support SQLDescribeParam until version 1.5.16 (see ticket
            # https://github.com/FreeTDS/freetds/issues/104), so pyodbc had to infer the
            # SQL type from the Python value. None carries no type information, causing
            # pyodbc to fall back to SQL_VARCHAR, which SQL Server rejects for binary
            # columns.
            if length is None and IS_FREETDS and is_binary and DRIVER_VERSION < (1, 5, 16):
                continue

            cursor.execute("delete from t1")

            value = _generate_str(length, encoding=encoding)

            try:
                cursor.execute("insert into t1 values(?)", value)
            except pyodbc.Error as ex:
                if value is None:
                    msg = f"{datatype} insert of NULL failed"
                else:
                    msg = f'{datatype} insert failed: length={length} len={len(value)}'
                raise Exception(msg) from ex

            v = cursor.execute("select * from t1").fetchone()[0]
            assert v == value

        cursor.execute("drop table t1")


def _test_scalar(cursor: pyodbc.Cursor, datatype, values):
    """
    A simple test wrapper for types that are identical when written and read.
    """
    cursor.execute(f"create table t1(c1 {datatype})")
    for value in values:
        cursor.execute("delete from t1")
        cursor.execute("insert into t1 values (?)", value)
        v = cursor.execute("select c1 from t1").fetchone()[0]
        assert v == value


def test_noscan(cursor: pyodbc.Cursor):
    assert cursor.noscan is False
    cursor.noscan = True
    assert cursor.noscan is True


def test_nonnative_uuid(cursor: pyodbc.Cursor):
    # Resetting the native_uuid flag should force return of a text value.
    # Note that SQL Server seems to always return uppercase.
    value = uuid.uuid4()
    cursor.execute("create table t1(n uniqueidentifier)")
    cursor.execute("insert into t1 values (?)", value)

    saved_native_uuid = pyodbc.native_uuid
    try:
        pyodbc.native_uuid = False
        result = cursor.execute("select n from t1").fetchval()
    finally:
        pyodbc.native_uuid = saved_native_uuid
    assert isinstance(result, str)
    assert result == str(value).upper()


def test_native_uuid(cursor: pyodbc.Cursor):
    # With the native_uuid flag set we should get a uuid.UUID object.
    value = uuid.uuid4()
    cursor.execute("create table t1(n uniqueidentifier)")
    cursor.execute("insert into t1 values (?)", value)

    saved_native_uuid = pyodbc.native_uuid
    try:
        pyodbc.native_uuid = True
        result = cursor.execute("select n from t1").fetchval()
    finally:
        pyodbc.native_uuid = saved_native_uuid
    assert isinstance(result, uuid.UUID)
    assert value == result


def test_nextset(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(i int)")
    for i in range(4):
        cursor.execute("insert into t1(i) values(?)", i)

    cursor.execute(
        """
        select i from t1 where i < 2 order by i;
        select i from t1 where i >= 2 order by i
        """)

    for i, row in enumerate(cursor):
        assert i == row.i

    assert cursor.nextset()

    for i, row in enumerate(cursor):
        assert i + 2 == row.i


@pytest.mark.skipif(IS_FREETDS, reason='https://github.com/FreeTDS/freetds/issues/230')
def test_nextset_with_raiserror(cursor: pyodbc.Cursor):
    cursor.execute("select i = 1; RAISERROR('c', 16, 1);")
    row = next(cursor)
    assert row.i == 1
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.nextset()


def test_fixed_unicode(cursor: pyodbc.Cursor):
    value = "t\xebsting"
    cursor.execute("create table t1(s nchar(7))")
    cursor.execute("insert into t1 values(?)", "t\xebsting")
    v = cursor.execute("select * from t1").fetchone()[0]
    assert isinstance(v, str)
    assert len(v) == len(value)
    # If we alloc'd wrong, the test below might work because of an embedded NULL
    assert v == value


def test_chinese(cursor: pyodbc.Cursor):
    v = '我的'
    cursor.execute("SELECT N'我的' AS [Name]")
    row = cursor.fetchone()
    assert row[0] == v

    cursor.execute("SELECT N'我的' AS [Name]")
    rows = cursor.fetchall()
    assert rows[0][0] == v


def test_bit(cursor: pyodbc.Cursor):
    value = True
    cursor.execute("create table t1(b bit)")
    cursor.execute("insert into t1 values (?)", value)
    v = cursor.execute("select b from t1").fetchone()[0]
    assert isinstance(v, bool)
    assert v == value


def test_decimal(cursor: pyodbc.Cursor):
    # From test provided by planders (thanks!) in Issue 91

    for mode in (True, False):
        for (precision, scale, negative) in [
                (1, 0, False), (1, 0, True), (6, 0, False), (6, 2, False),
                (6, 4, True), (6, 6, True), (38, 0, False), (38, 10, False),
                (38, 38, False), (38, 0, True), (38, 10, True), (38, 38, True)]:

            cursor.connection.fetch_decimal_as_string = mode
            try:
                cursor.execute("drop table t1")
            except Exception:
                pass

            cursor.execute(f"create table t1(d decimal({precision}, {scale}))")

            # Construct a decimal that uses the maximum precision and scale.
            sign   = negative and '-' or ''
            before = '9' * (precision - scale)
            after  = scale and ('.' + '9' * scale) or ''
            decStr = f'{sign}{before}{after}'
            value = Decimal(decStr)

            cursor.execute("insert into t1 values(?)", value)

            v = cursor.execute("select d from t1").fetchval()
            assert v == value


def test_decimal_e(cursor: pyodbc.Cursor):
    """Ensure exponential notation decimals are properly handled"""
    value = Decimal((0, (1, 2, 3), 5))  # prints as 1.23E+7
    cursor.execute("create table t1(d decimal(10, 2))")
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select * from t1").fetchone()[0]
    assert result == value


def test_subquery_params(cursor: pyodbc.Cursor):
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
    assert row is not None
    assert row[0] == 1


def test_close_cnxn():
    """Make sure using a Cursor after closing its connection doesn't crash."""

    cnxn = connect()
    cursor = cnxn.cursor()

    cursor.execute("drop table if exists t1")
    cursor.execute("create table t1(id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, 'test')
    cursor.execute("select * from t1")

    cnxn.close()

    # Now that the connection is closed, we expect an exception.  (If the code attempts to use
    # the HSTMT, we'll get an access violation instead.)
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("select * from t1")


def test_empty_string(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "")


def test_empty_string_encoding():
    cnxn = connect()
    cnxn.setdecoding(pyodbc.SQL_CHAR, encoding='shift_jis')
    value = ""
    cursor = cnxn.cursor()
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert v == value


def test_fixed_str(cursor: pyodbc.Cursor):
    value = "testing"
    cursor.execute("create table t1(s char(7))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert isinstance(v, str)
    assert len(v) == len(value)
    # If we alloc'd wrong, the test below might work because of an embedded NULL
    assert v == value


def test_empty_unicode(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s nvarchar(20))")
    cursor.execute("insert into t1 values(?)", "")


def test_empty_unicode_encoding():
    cnxn = connect()
    cnxn.setdecoding(pyodbc.SQL_CHAR, encoding='shift_jis')
    value = ""
    cursor = cnxn.cursor()
    cursor.execute("create table t1(s nvarchar(20))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert v == value


def test_negative_row_index(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "1")
    row = cursor.execute("select * from t1").fetchone()
    assert row[0] == "1"
    assert row[-1] == "1"


def test_version():
    assert len(pyodbc.version.split('.')) == 3  # 1.3.1 etc.


@pytest.mark.skipif(IS_MSODBCSQL and SQLSERVER_YEAR < 2008,
                    reason='Date not supported until 2008?')
def test_date(cursor: pyodbc.Cursor):
    value = date.today()

    cursor.execute("create table t1(d date)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select d from t1").fetchone()[0]
    assert isinstance(result, date)
    assert value == result


@pytest.mark.skipif(IS_MSODBCSQL and SQLSERVER_YEAR < 2008,
                    reason='Time not supported until 2008?')
def test_time(cursor: pyodbc.Cursor):
    value = datetime.now().time()

    # We aren't yet writing values using the new extended time type so the value written to the
    # database is only down to the second.
    value = value.replace(microsecond=0)

    cursor.execute("create table t1(t time)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select t from t1").fetchone()[0]
    assert isinstance(result, time)
    assert value == result


def test_datetime(cursor: pyodbc.Cursor):
    value = datetime(2007, 1, 15, 3, 4, 5)

    cursor.execute("create table t1(dt datetime)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert isinstance(result, datetime)
    assert value == result


def test_datetime_fraction(cursor: pyodbc.Cursor):
    # SQL Server supports milliseconds, but Python's datetime supports nanoseconds, so the most
    # granular datetime supported is xxx000.

    value = datetime(2007, 1, 15, 3, 4, 5, 123000)

    cursor.execute("create table t1(dt datetime)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert isinstance(result, datetime)
    assert value == result


def test_datetime_fraction_rounded(cursor: pyodbc.Cursor):
    # SQL Server supports milliseconds, but Python's datetime supports nanoseconds.  pyodbc
    # rounds down to what the database supports.

    full    = datetime(2007, 1, 15, 3, 4, 5, 123456)
    rounded = datetime(2007, 1, 15, 3, 4, 5, 123000)

    cursor.execute("create table t1(dt datetime)")
    cursor.execute("insert into t1 values (?)", full)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert isinstance(result, datetime)
    assert rounded == result


def test_datetime2(cursor: pyodbc.Cursor):
    value = datetime(2007, 1, 15, 3, 4, 5)

    cursor.execute("create table t1(dt datetime2)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert isinstance(result, datetime)
    assert value == result


def test_sp_results(cursor: pyodbc.Cursor):
    cursor.execute(
        """
        Create procedure proc1
        AS
          select top 10 name, id, xtype, refdate
          from sysobjects
        """)
    rows = cursor.execute("exec proc1").fetchall()
    assert isinstance(rows, list)
    assert len(rows) == 10  # there has to be at least 10 items in sysobjects
    assert isinstance(rows[0].refdate, datetime)


def test_sp_results_from_temp(cursor: pyodbc.Cursor):

    # Note: I've used "set nocount on" so that we don't get the number of rows deleted from
    # #tmptable.  If you don't do this, you'd need to call nextset() once to skip it.

    cursor.execute(
        """
        Create procedure proc1
        AS
          set nocount on
          select top 10 name, id, xtype, refdate
          into #tmptable
          from sysobjects

          select * from #tmptable
        """)
    cursor.execute("exec proc1")
    assert cursor.description is not None
    assert len(cursor.description) == 4

    rows = cursor.fetchall()
    assert isinstance(rows, list)
    assert len(rows) == 10      # there has to be at least 10 items in sysobjects
    assert isinstance(rows[0].refdate, datetime)


def test_sp_results_from_vartbl(cursor: pyodbc.Cursor):
    cursor.execute(
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
    cursor.execute("exec proc1")
    rows = cursor.fetchall()
    assert isinstance(rows, list)
    assert len(rows) == 10  # there has to be at least 10 items in sysobjects
    assert isinstance(rows[0].refdate, datetime)


def test_sp_with_dates(cursor: pyodbc.Cursor):
    # Reported in the forums that passing two datetimes to a stored procedure doesn't work.
    cursor.execute(
        """
        if exists (select * from dbo.sysobjects where id = object_id(N'[test_sp]')
             and OBJECTPROPERTY(id, N'IsProcedure') = 1)
          drop procedure [dbo].[test_sp]
        """)
    cursor.execute(
        """
        create procedure test_sp(@d1 datetime, @d2 datetime)
        AS
          declare @d as int
          set @d = datediff(year, @d1, @d2)
          select @d
        """)
    cursor.execute("exec test_sp ?, ?", datetime.now(), datetime.now())
    rows = cursor.fetchall()
    assert rows is not None
    assert rows[0][0] == 0   # 0 years apart


def test_sp_with_none(cursor: pyodbc.Cursor):
    # Reported in the forums that passing None caused an error.
    cursor.execute(
        """
        if exists (select * from dbo.sysobjects where id = object_id(N'[test_sp]')
             and OBJECTPROPERTY(id, N'IsProcedure') = 1)
          drop procedure [dbo].[test_sp]
        """)
    cursor.execute(
        """
        create procedure test_sp(@x varchar(20))
        AS
          declare @y varchar(20)
          set @y = @x
          select @y
        """)
    cursor.execute("exec test_sp ?", None)
    rows = cursor.fetchall()
    assert rows is not None
    assert rows[0][0] is None   # 0 years apart


#
# rowcount
#


def test_rowcount_delete(cursor: pyodbc.Cursor):
    # After DDL (DROP TABLE), rowcount is driver-defined per the ODBC spec.
    # Microsoft's driver might reliably return -1 here, but that's not true
    # for FreeTDS.
    if IS_MSODBCSQL:
        assert cursor.rowcount == -1
    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    cursor.execute("delete from t1")
    assert cursor.rowcount == count


def test_rowcount_nodata(cursor: pyodbc.Cursor):
    """
    This represents a different code path than a delete that deleted something.

    The return value is SQL_NO_DATA and code after it was causing an error.  We could use
    SQL_NO_DATA to step over the code that errors out and drop down to the same SQLRowCount
    code.  On the other hand, we could hardcode a zero return value.
    """
    cursor.execute("create table t1(i int)")
    # This is a different code path internally.
    cursor.execute("delete from t1")
    assert cursor.rowcount == 0


def test_rowcount_select(cursor: pyodbc.Cursor):
    """
    Ensure Cursor.rowcount is set properly after a select statement.

    pyodbc calls SQLRowCount after each execute and sets Cursor.rowcount, but SQL Server 2005
    returns -1 after a select statement, so we'll test for that behavior.  This is valid
    behavior according to the DB API specification, but people don't seem to like it.
    """
    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    cursor.execute("select * from t1")
    assert cursor.rowcount == -1

    rows = cursor.fetchall()
    assert len(rows) == count
    assert cursor.rowcount == -1


def test_rowcount_reset(cursor: pyodbc.Cursor):
    "Ensure rowcount is reset after DDL"
    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    assert cursor.rowcount == 1

    cursor.execute("create table t2(i int)")
    ddl_rowcount = (0 if IS_FREETDS else -1)
    assert cursor.rowcount == ddl_rowcount


def test_retcursor_delete(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(i int)")
    cursor.execute("insert into t1 values (1)")
    v = cursor.execute("delete from t1")
    assert v == cursor


def test_retcursor_nodata(cursor: pyodbc.Cursor):
    """
    This represents a different code path than a delete that deleted something.

    The return value is SQL_NO_DATA and code after it was causing an error.  We could use
    SQL_NO_DATA to step over the code that errors out and drop down to the same SQLRowCount
    code.
    """
    cursor.execute("create table t1(i int)")
    # This is a different code path internally.
    v = cursor.execute("delete from t1")
    assert v == cursor


def test_retcursor_select(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(i int)")
    cursor.execute("insert into t1 values (1)")
    v = cursor.execute("select * from t1")
    assert v == cursor


def table_with_spaces(cursor: pyodbc.Cursor):
    "Ensure we can select using [x z] syntax"

    try:
        cursor.execute("create table [test one](int n)")
        cursor.execute("insert into [test one] values(1)")
        cursor.execute("select * from [test one]")
        v = cursor.fetchone()[0]
        assert v == 1
    finally:
        cursor.rollback()


def test_lower_case():
    "Ensure pyodbc.lowercase forces returned column names to lowercase."
    try:
        pyodbc.lowercase = True
        cnxn = connect()
        cursor = cnxn.cursor()

        cursor.execute("create table t1(Abc int, dEf int)")
        cursor.execute("select * from t1")

        names = [t[0] for t in cursor.description]
        names.sort()

        assert names == ["abc", "def"]
    finally:
        # Put it back so other tests don't fail.
        pyodbc.lowercase = False


def test_row_description(cursor: pyodbc.Cursor):
    """
    Ensure Cursor.description is accessible as Row.cursor_description.
    """
    cursor.execute("create table t1(a int, b char(3))")
    cursor.execute("insert into t1 values(1, 'abc')")
    row = cursor.execute("select * from t1").fetchone()
    assert cursor.description == row.cursor_description


def test_temp_select(cursor: pyodbc.Cursor):
    # A project was failing to create temporary tables via select into.
    cursor.execute("create table t1(s char(7))")
    cursor.execute("insert into t1 values(?)", "testing")
    v = cursor.execute("select * from t1").fetchone()[0]
    assert isinstance(v, str)
    assert v == "testing"

    cursor.execute("select s into t2 from t1")
    v = cursor.execute("select * from t1").fetchone()[0]
    assert isinstance(v, str)
    assert v == "testing"


def test_executemany(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [(i, str(i)) for i in range(1, 6)]

    cursor.executemany("insert into t1(a, b) values (?,?)", params)

    count = cursor.execute("select count(*) from t1").fetchone()[0]
    assert count == len(params)

    cursor.execute("select a, b from t1 order by a")
    rows = cursor.fetchall()
    assert count == len(rows)

    for param, row in zip(params, rows):
        assert param[0] == row[0]
        assert param[1] == row[1]


def test_executemany_one(cursor: pyodbc.Cursor):
    "Pass executemany a single sequence"
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [(1, "test")]

    cursor.executemany("insert into t1(a, b) values (?,?)", params)

    count = cursor.execute("select count(*) from t1").fetchone()[0]
    assert count == len(params)

    cursor.execute("select a, b from t1 order by a")
    rows = cursor.fetchall()
    assert count == len(rows)

    for param, row in zip(params, rows):
        assert param[0] == row[0]
        assert param[1] == row[1]


def test_executemany_dae_0(cursor: pyodbc.Cursor):
    """
    DAE for 0-length value
    """
    cursor.execute("create table t1(a nvarchar(max))")

    cursor.fast_executemany = True
    cursor.executemany("insert into t1(a) values(?)", [['']])

    assert cursor.execute("select a from t1").fetchone()[0] == ''

    cursor.fast_executemany = False


def test_executemany_failure(cursor: pyodbc.Cursor):
    """
    Ensure that an exception is raised if one query in an executemany fails.
    """
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [(1, 'good'),
              ('error', 'not an int'),
              (3, 'good')]

    with pytest.raises(pyodbc.Error):
        cursor.executemany("insert into t1(a, b) value (?, ?)", params)


def test_row_slicing(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b int, c int, d int)")
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = row[:]
    assert result is row

    result = row[:-1]
    assert result == (1, 2, 3)

    result = row[0:4]
    assert result is row


def test_row_repr(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b int, c int, d varchar(50))")
    cursor.execute("insert into t1 values(1,2,3,'four')")

    row = cursor.execute("select * from t1").fetchone()

    result = str(row)
    assert result == "(1, 2, 3, 'four')"

    result = str(row[:-1])
    assert result == "(1, 2, 3)"

    result = str(row[:1])
    assert result == "(1,)"


def test_concatenation(cursor: pyodbc.Cursor):
    v2 = '0123456789' * 30
    v3 = '9876543210' * 30

    cursor.execute("create table t1(c1 int identity(1, 1), c2 varchar(300), c3 varchar(300))")
    cursor.execute("insert into t1(c2, c3) values (?,?)", v2, v3)

    row = cursor.execute("select c2, c3, c2 + c3 as both from t1").fetchone()

    assert row.both == v2 + v3


def test_view_select(cursor: pyodbc.Cursor):
    # Reported in forum: Can't select from a view?  I think I do this a lot, but another test
    # never hurts.

    # Create a table (t1) with 3 rows and a view (t2) into it.
    cursor.execute("create table t1(c1 int identity(1, 1), c2 varchar(50))")
    for i in range(3):
        cursor.execute("insert into t1(c2) values (?)", f"string{i}")
    cursor.execute("create view t2 as select * from t1")

    # Select from the view
    cursor.execute("select * from t2")
    rows = cursor.fetchall()
    assert rows is not None
    assert len(rows) == 3


def test_autocommit():
    cnxn = connect()
    assert cnxn.autocommit is False
    cnxn = None

    cnxn = connect(autocommit=True)
    assert cnxn.autocommit is True
    cnxn.autocommit = False
    assert cnxn.autocommit is False


def test_sqlserver_callproc(cursor: pyodbc.Cursor):
    try:
        cursor.execute("drop procedure pyodbctest")
        cursor.commit()
    except Exception:
        pass

    cursor.execute("create table t1(s varchar(10))")
    cursor.execute("insert into t1 values(?)", "testing")

    cursor.execute("""
                    create procedure pyodbctest @var1 varchar(32)
                    as
                    begin
                      select s from t1
                    return
                    end
                    """)

    cursor.execute("exec pyodbctest 'hi'")


def test_skip(cursor: pyodbc.Cursor):
    # Insert 1, 2, and 3.  Fetch 1, skip 2, fetch 3.

    cursor.execute("create table t1(id int)")
    for i in range(1, 5):
        cursor.execute("insert into t1 values(?)", i)
    cursor.execute("select id from t1 order by id")
    assert cursor.fetchone()[0] == 1
    cursor.skip(2)
    assert cursor.fetchone()[0] == 4


def test_timeout():
    cnxn = connect()
    assert cnxn.timeout == 0    # defaults to zero (off)

    cnxn.timeout = 30
    assert cnxn.timeout == 30

    cnxn.timeout = 0
    assert cnxn.timeout == 0


def test_sets_execute(cursor: pyodbc.Cursor):
    # Only lists and tuples are allowed.
    cursor.execute("create table t1 (word varchar (100))")

    words = {'a', 'b', 'c'}

    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("insert into t1 (word) values (?)", words)

    with pytest.raises(pyodbc.ProgrammingError):
        cursor.executemany("insert into t1 (word) values (?)", words)


def test_row_execute(cursor: pyodbc.Cursor):
    "Ensure we can use a Row object as a parameter to execute"
    cursor.execute("create table t1(n int, s varchar(10))")
    cursor.execute("insert into t1 values (1, 'a')")
    row = cursor.execute("select n, s from t1").fetchone()
    assert row

    cursor.execute("create table t2(n int, s varchar(10))")
    cursor.execute("insert into t2 values (?, ?)", row)


def test_row_executemany(cursor: pyodbc.Cursor):
    "Ensure we can use a Row object as a parameter to executemany"
    cursor.execute("create table t1(n int, s varchar(10))")

    for i in range(3):
        cursor.execute("insert into t1 values (?, ?)", i, chr(ord('a') + i))

    rows = cursor.execute("select n, s from t1").fetchall()
    assert len(rows) != 0

    cursor.execute("create table t2(n int, s varchar(10))")
    cursor.executemany("insert into t2 values (?, ?)", rows)


def test_description(cursor: pyodbc.Cursor):
    "Ensure cursor.description is correct"

    cursor.execute("create table t1(n int, s varchar(8), d decimal(5,2))")
    cursor.execute("insert into t1 values (1, 'abc', '1.23')")
    cursor.execute("select * from t1")

    # (I'm not sure the precision of an int is constant across different versions, bits, so I'm
    # hand checking the items I do know.

    # int
    t = cursor.description[0]
    assert t[0] == 'n'
    assert t[1] is int
    assert t[5] == 0       # scale
    assert t[6] is True    # nullable

    # varchar(8)
    t = cursor.description[1]
    assert t[0] == 's'
    assert t[1] is str
    assert t[4] == 8       # precision
    assert t[5] == 0       # scale
    assert t[6] is True    # nullable

    # decimal(5, 2)
    t = cursor.description[2]
    assert t[0] == 'd'
    assert t[1] == Decimal
    assert t[4] == 5       # precision
    assert t[5] == 2       # scale
    assert t[6] is True    # nullable


def test_cursor_messages_with_print(cursor: pyodbc.Cursor):
    """
    Ensure the Cursor.messages attribute is handled correctly with a simple PRINT statement.
    """
    assert not cursor.messages

    # ascii / extended ascii / unicode / beyond BMP unicode
    for msg in ('hello world', 'a \xeb a', 'b \u0394 b', 'c \U0001F31C c'):
        cursor.execute(f"PRINT N'{msg}'")  # note, unicode literal
        messages = cursor.messages
        assert isinstance(messages, list)
        assert len(messages) == 1
        assert isinstance(messages[0], tuple)
        assert len(messages[0]) == 2
        assert isinstance(messages[0][0], str)
        assert isinstance(messages[0][1], str)
        assert messages[0][0] == '[01000] (0)'
        assert messages[0][1].endswith(msg)

    # maximum size message
    # SQL Server PRINT statements are never more than 8000 characters
    # https://docs.microsoft.com/en-us/sql/t-sql/language-elements/print-transact-sql#remarks
    msg = 'ABCDEFGH' * 1000
    cursor.execute(f"PRINT '{msg}'")  # note, plain ascii literal
    messages = cursor.messages
    assert len(messages) == 1
    assert messages[0][1].endswith(msg)

    # Confirm that the PRINT message is captured when DAE kicks in.
    # https://github.com/mkleehammer/pyodbc/issues/1140
    cursor.execute("PRINT 'HI!'; SELECT ?", "x" * 2001)
    assert len(cursor.messages) == 1
    assert cursor.messages[0][1].endswith("HI!")


@pytest.mark.skipif(IS_FREETDS and DRIVER_VERSION < (1, 5, 15),
                    reason="FreeTDS ignores bind offset")
def test_cursor_messages_with_fast_executemany(cursor: pyodbc.Cursor):
    """
    Ensure the Cursor.messages attribute is set with fast_executemany=True.
    """
    cursor.execute("create table t2(id1 int, id2 int)")
    cursor.commit()

    cursor.fast_executemany = True
    cursor.executemany(
        "print 'hello';insert into t2(id1, id2) values (?, ?)",
        [(10, 11), (20, 21)],
    )
    assert len(cursor.messages) == 2
    assert all(m[1].endswith('hello') for m in cursor.messages)


def test_cursor_messages_with_stored_proc(cursor: pyodbc.Cursor):
    """
    Complex scenario to test the Cursor.messages attribute.
    """
    cursor.execute("""
        create or alter procedure test_cursor_messages as
        begin
            set nocount on;
            print 'Message 1a';
            print 'Message 1b';
            select N'Field 1a' AS F UNION ALL SELECT N'Field 1b';
            select N'Field 2a' AS F UNION ALL SELECT N'Field 2b';
            print 'Message 2a';
            print 'Message 2b';
        end
    """)

    # The messages will look like:
    #
    # [Microsoft][ODBC Driver 18 for SQL Server][SQL Server]Message 1a

    # result set 1: messages, rows
    cursor.execute("exec test_cursor_messages")
    vals = [row[0] for row in cursor.fetchall()]
    assert vals == ['Field 1a', 'Field 1b']
    msgs = [
        re.search(r'Message \d[ab]$', m[1]).group(0)
        for m in cursor.messages
    ]
    assert msgs == ['Message 1a', 'Message 1b']

    # result set 2: rows, no messages
    assert cursor.nextset()
    vals = [row[0] for row in cursor.fetchall()]
    assert vals == ['Field 2a', 'Field 2b']
    assert not cursor.messages

    # result set 3: messages, no rows
    assert cursor.nextset()
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.fetchall()
    msgs = [
        re.search(r'Message \d[ab]$', m[1]).group(0)
        for m in cursor.messages
    ]
    assert msgs == ['Message 2a', 'Message 2b']

    # result set 4: no rows, no messages
    assert not cursor.nextset()
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.fetchall()
    assert not cursor.messages


def test_none_param(cursor: pyodbc.Cursor):
    "Ensure None can be used for params other than the first"
    # Some driver/db versions would fail if NULL was not the first parameter because
    # SQLDescribeParam (only used with NULL) could not be used after the first call to
    # SQLBindParameter.  This means None always worked for the first column, but did not work
    # for later columns.
    #
    # If SQLDescribeParam doesn't work, pyodbc would use VARCHAR which almost always worked.
    # However, binary/varbinary won't allow an implicit conversion.

    cursor.execute("create table t1(n int, blob varbinary(max))")
    cursor.execute("insert into t1 values (1, newid())")
    row = cursor.execute("select * from t1").fetchone()
    assert row.n == 1
    assert isinstance(row.blob, bytes)

    sql = "update t1 set n=?, blob=?"
    try:
        cursor.execute(sql, 2, None)
    except pyodbc.DataError:
        if IS_FREETDS:
            # cnxn.getinfo(pyodbc.SQL_DESCRIBE_PARAMETER) returns False for FreeTDS, so pyodbc
            # can't call SQLDescribeParam to get the correct parameter type.  This can lead to
            # errors being returned from SQL Server when sp_prepexec is called, e.g., "Implicit
            # conversion from data type varchar to varbinary(max) is not allowed."
            #
            # So at least verify that the user can manually specify the parameter type
            cursor.setinputsizes([(), (pyodbc.SQL_VARBINARY, None, None)])
            cursor.execute(sql, 2, None)
        else:
            raise
    row = cursor.execute("select * from t1").fetchone()
    assert row.n == 2
    assert row.blob is None


def test_output_conversion():
    def convert1(value):
        # The value is the raw bytes (as a bytes object) read from the
        # database.  We'll simply add an X at the beginning at the end.
        return 'X' + value.decode('latin1') + 'X'

    def convert2(value):
        # Same as above, but add a Y at the beginning at the end.
        return 'Y' + value.decode('latin1') + 'Y'

    cnxn = connect()
    cursor = cnxn.cursor()

    cursor.execute("create table t1(n int, v varchar(10))")
    cursor.execute("insert into t1 values (1, '123.45')")

    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert1)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    # Clear all conversions and try again.  There should be no Xs this time.
    cnxn.clear_output_converters()
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'

    # Same but clear using remove_output_converter.
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert1)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    cnxn.remove_output_converter(pyodbc.SQL_VARCHAR)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'

    # Clear via add_output_converter, passing None for the converter function.
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert1)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, None)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'

    # retrieve and temporarily replace converter (get_output_converter)
    #
    #   case_1: converter already registered
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert1)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'
    prev_converter = cnxn.get_output_converter(pyodbc.SQL_VARCHAR)
    assert prev_converter is not None
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert2)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'Y123.45Y'
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, prev_converter)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'
    #
    #   case_2: no converter already registered
    cnxn.clear_output_converters()
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'
    prev_converter = cnxn.get_output_converter(pyodbc.SQL_VARCHAR)
    assert prev_converter is None
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, convert2)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'Y123.45Y'
    cnxn.add_output_converter(pyodbc.SQL_VARCHAR, prev_converter)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'


def test_too_large(cursor: pyodbc.Cursor):
    """Ensure error raised if insert fails due to truncation"""
    value = 'x' * 1000
    cursor.execute("create table t1(s varchar(800))")

    with pytest.raises(pyodbc.Error):
        cursor.execute("insert into t1 values (?)", value)


def test_row_equal(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(n int, s varchar(20))")
    cursor.execute("insert into t1 values (1, 'test')")
    row1 = cursor.execute("select n, s from t1").fetchone()
    row2 = cursor.execute("select n, s from t1").fetchone()
    assert row1 == row2


def test_row_gtlt(cursor: pyodbc.Cursor):
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
    rows.sort()  # uses <


def test_context_manager_success():
    "Ensure `with` commits if an exception is not raised"
    cnxn = connect()
    cursor = cnxn.cursor()

    cursor.execute("create table t1(n int)")
    cnxn.commit()

    with cnxn:
        cursor.execute("insert into t1 values (1)")

    rows = cursor.execute("select n from t1").fetchall()
    assert len(rows) == 1
    assert rows[0][0] == 1


def test_context_manager_failure(cursor: pyodbc.Cursor):
    "Ensure `with` rolls back if an exception is raised"
    cnxn = connect()
    cursor = cnxn.cursor()

    # We'll insert a row and commit it.  Then we'll insert another row followed by an
    # exception.

    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (1)")
    cnxn.commit()

    with pytest.raises(pyodbc.Error):
        with cnxn:
            cursor.execute("insert into t1 values (2)")
            cursor.execute("delete from bogus")

    cursor.execute("select max(n) from t1")
    val = cursor.fetchval()
    assert val == 1


def test_untyped_none(cursor: pyodbc.Cursor):
    # From issue 129
    value = cursor.execute("select ?", None).fetchone()[0]
    assert value is None


def test_large_update_nodata(cursor: pyodbc.Cursor):
    cursor.execute('create table t1(a varbinary(max))')
    hundredkb = b'x' * 100 * 1024
    cursor.execute('update t1 set a=? where 1=0', (hundredkb,))


def test_func_param(cursor: pyodbc.Cursor):
    try:
        cursor.execute("drop function func1")
    except Exception:
        pass
    cursor.execute("""
                   create function func1 (@testparam varchar(4))
                   returns @rettest table (param varchar(4))
                   as
                   begin
                       insert @rettest
                       select @testparam
                       return
                   end
                   """)
    cursor.commit()
    value = cursor.execute("select * from func1(?)", 'test').fetchone()[0]
    assert value == 'test'


def test_columns(cursor: pyodbc.Cursor):
    # When using aiohttp, `await cursor.primaryKeys('t1')` was raising the error
    #
    #   Error: TypeError: argument 2 must be str, not None
    #
    # I'm not sure why, but PyArg_ParseTupleAndKeywords fails if you use "|s" for an
    # optional string keyword when calling indirectly.

    cursor.execute("create table t1(a int, b varchar(3), xΏz varchar(4))")

    cursor.columns('t1')
    results = {row.column_name: row for row in cursor}
    row = results['a']
    assert row.type_name == 'int', row.type_name
    row = results['b']
    assert row.type_name == 'varchar'
    assert row.column_size == 3

    # Now do the same, but specifically pass in None to one of the keywords.  Old versions
    # were parsing arguments incorrectly and would raise an error.  (This crops up when
    # calling indirectly like columns(*args, **kwargs) which aiodbc does.)

    cursor.columns('t1', schema=None, catalog=None)
    results = {row.column_name: row for row in cursor}
    row = results['a']
    assert row.type_name == 'int', row.type_name
    row = results['b']
    assert row.type_name == 'varchar'
    assert row.column_size == 3
    row = results['xΏz']
    assert row.type_name == 'varchar'
    assert row.column_size == 4, row.column_size

    for i in range(8, 16):
        table_name = 'pyodbc_89abcdef'[:i]

        cursor.execute(f"""
          IF OBJECT_ID (N'{table_name}', N'U') IS NOT NULL DROP TABLE {table_name};
          CREATE TABLE {table_name} (id INT PRIMARY KEY);
        """)

        col_count = len([col.column_name for col in cursor.columns(table_name)])
        assert col_count == 1

        cursor.execute(f"drop table {table_name}")


def test_table_privileges(cursor: pyodbc.Cursor):
    # Confirm exposure of SQLTablePrivileges.  We're limited in what we can test, as
    # we can't control whether we're running with permission to create users or grant
    # permissions.  We can at least verify that the method generates a results set
    # with the right columns.
    cols = ["table_cat", "table_schem", "table_name", "grantor",
            "grantee", "privilege", "is_grantable"]
    cursor.tablePrivileges()
    names = [col[0] for col in cursor.description]
    assert len(cols) == len(names), "privileges results set has the wrong shape"
    assert cols == names, "unexpected column names for privileges results set"


@pytest.mark.skipif(IS_FREETDS, reason="FreeTDS Unicode handling for catalog functions is unreliable")
def test_statistics_unicode():
    # https://github.com/mkleehammer/pyodbc/issues/1457
    # statistics() passed the table name straight to the ANSI SQLStatistics, mis-encoding a
    # non-ASCII name so the driver matched nothing and returned no rows.  The failure is
    # masked on a *reused* pooled connection -- it only shows on a fresh physical connection
    # -- which is why a plain test in the suite doesn't reliably catch it.  Force a fresh
    # connection with a unique APP= (pooling keys on the connection string), plus a unique
    # table name for good measure.
    suffix = uuid.uuid4().hex
    name = "ランドマーク_" + suffix
    cnxn = pyodbc.connect(CNXNSTR + f";APP=pyodbc_1457_{suffix}", autocommit=True)
    cur = cnxn.cursor()
    cur.execute(f"CREATE TABLE [{name}] (id INT PRIMARY KEY, foo INT)")
    cur.execute(f"CREATE INDEX ix_foo ON [{name}] (foo)")
    try:
        # index_name is column 5 of the SQLStatistics result set
        index_names = {row[5] for row in cur.statistics(name).fetchall() if row[5] is not None}
        assert "ix_foo" in index_names, \
            f"statistics() returned no index info for a Unicode table name; got {index_names}"
    finally:
        cur.execute(f"IF OBJECT_ID(N'[{name}]', N'U') IS NOT NULL DROP TABLE [{name}]")
        cnxn.close()


def test_cancel(cursor: pyodbc.Cursor):
    # I'm not sure how to reliably cause a hang to cancel, so for now we'll settle with
    # making sure SQLCancel is called correctly.
    cursor.execute("select 1")
    cursor.cancel()


def test_emoticons_as_parameter(cursor: pyodbc.Cursor):
    # https://github.com/mkleehammer/pyodbc/issues/423
    #
    # When sending a varchar parameter, pyodbc is supposed to set ColumnSize to the number
    # of characters.  Ensure it works even with 4-byte characters.
    #
    # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

    v = "x \U0001F31C z"

    cursor.execute("create table t1(s nvarchar(100))")
    cursor.execute("insert into t1 values (?)", v)

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v


def test_emoticons_as_literal(cursor: pyodbc.Cursor):
    # similar to `test_emoticons_as_parameter`, above, except for Unicode literal
    #
    # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

    # FreeTDS ODBC issue fixed in version 1.1.23
    # https://github.com/FreeTDS/freetds/issues/317

    v = "x \U0001F31C z"

    cursor.execute("create table t1(s nvarchar(100))")
    cursor.execute(f"insert into t1 values (N'{v}')")

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v


_BCP_SKIP = 'BCP requires the Microsoft ODBC Driver 13 or later for SQL Server'
_BCP_TZ = timezone(timedelta(hours=5, minutes=30))


def _executemany(cur: pyodbc.Cursor, sql: str, rows: list, bcp: bool = True) -> int:
    """
    Runs executemany with fast_executemany, with or without the BCP fast path, and returns
    cursor.rowcount: the number of rows a BCP load copied, or -1 for the plain path, which is
    also what a BCP call that declined and fell back to it reports.
    """
    cur.fast_executemany = True
    cur.use_bcp_fast = bcp
    cur.executemany(sql, rows)
    return cur.rowcount


def _fetch(cur: pyodbc.Cursor, table: str, column_type: str) -> list:
    """The v column of a test table by id; datetimeoffset as text, which pyodbc can't fetch."""
    expr = "convert(nvarchar(40), v, 121)" if column_type.startswith("datetimeoffset") else "v"
    return [row[0] for row in cur.execute(f"select {expr} from {table} order by id")]


def _load_both(cur: pyodbc.Cursor, column_type: str, values: list,
               expect_bcp: bool = True, sizes: list | None = None) -> tuple:
    """
    Loads (id, value) rows into t1 with the BCP fast path and into t2 with the plain path,
    both under the setinputsizes entries `sizes` if given, checks whether BCP carried out the
    load, and returns the values each table stored.
    """
    for table in ("t1", "t2"):
        cur.execute(f"create table {table}(id int, v {column_type})")
    rows = list(enumerate(values))
    cur.setinputsizes(sizes)
    try:
        copied = _executemany(cur, "insert into t1 values (?, ?)", rows)
        _executemany(cur, "insert into t2 values (?, ?)", rows, bcp=False)
    finally:
        cur.setinputsizes(None)
    assert copied == (len(rows) if expect_bcp else -1)
    return _fetch(cur, "t1", column_type), _fetch(cur, "t2", column_type)


_BCP_PARITY_CASES = [
    ("int", [0, -5, 2**31 - 1, True, False]),
    ("bigint", [2**40, -(2**63)]),
    ("smallint", [4.7, -2.5]),
    ("tinyint", [255]),
    ("int", [Decimal("5"), Decimal("-0"), 7]),
    ("bit", [True, False, 0, 1]),
    ("float", [0.1, 1e300, -2.5, 3, 2**60 + 1]),
    ("real", [0.1, 16777217]),
    ("decimal(10,2)", [Decimal("1.50"), Decimal("-0.01"), Decimal("1E+2"), 7]),
    ("decimal(10,2)", [1.235, 2.675, 0.1]),
    ("numeric(38,0)", [Decimal(10**37), 5]),
    ("numeric(38,10)", [Decimal("1" * 28 + ".5"), Decimal("-" + "1" * 27 + ".25"),
                        Decimal("1" * 20 + "." + "1" * 10)]),
    ("numeric(38,0)", [Decimal("1" * 28 + ".4")]),
    ("money", [Decimal("1.2345"), 3]),
    ("smallmoney", [Decimal("-1.5")]),
    ("varchar(50)", ["Zürich", "", "a\x00b"]),
    ("varchar(50) collate SQL_Latin1_General_CP1_CI_AS", ["façade ½ £", "東京"]),
    ("nvarchar(50)", ["東京 \U0001D518", 42, ""]),
    ("nvarchar(50)", [5, -7]),
    ("nvarchar(50)", [0.1, 1e20, -0.0]),
    ("nvarchar(50)", [True, False]),
    ("varchar(3)", [42, -7]),
    ("varchar(2)", ["ab", "é"]),
    ("char(5)", ["ab", ""]),
    ("nchar(3)", ["é", ""]),
    ("varbinary(20)", [b"\x00\xff", b"", bytearray(b"ab")]),
    ("binary(4)", [b"\x01", b""]),
    ("varchar(max)", ["", "x" * 9000]),
    ("varchar(max)", [42, -1]),
    ("nvarchar(max)", [0.5]),
    ("nvarchar(max)", [True]),
    ("nvarchar(max)", ["é" * 5000, ""]),
    ("varbinary(max)", [b"", b"\x07" * 9000]),
    ("xml", ["<a>é</a>", ""]),
    ("date", [date(2024, 2, 29), date(1, 1, 1)]),
    ("date", [datetime(2024, 2, 29), datetime(2024, 3, 1, 0, 0, 0, 999999),
              datetime(2024, 3, 2, tzinfo=_BCP_TZ), date(2024, 3, 3)]),
    ("time(0)", [time(3, 4, 5), datetime(2024, 1, 2, 3, 4, 5, 999999)]),
    ("time(0)", ["13:14:15", time(3, 4, 5)]),
    ("time(3)", [time(3, 4, 5, 500000), datetime(2024, 1, 2, 3, 4, 5, 123456)]),
    ("time(3)", ["13:14:15.123", "13:14:15.5"]),
    ("time(7)", [time(23, 59, 59, 999999), time(1, 2, 3, tzinfo=_BCP_TZ)]),
    ("time(7)", ["13:14:15", "23:59:59.999999", "00:00:00.1234567", time(1, 2, 3)]),
    ("datetime2(0)", [datetime(2024, 1, 2, 3, 4, 5, 999999), date(2024, 1, 2)]),
    ("datetime2(3)", [datetime(2024, 12, 31, 23, 59, 59, 999999)]),
    ("datetime2(7)", [datetime(2024, 1, 2, 3, 4, 5, 123456, tzinfo=_BCP_TZ)]),
    ("datetime", [datetime(2024, 1, 2, 3, 4, 5, ms * 1000) for ms in (1, 2, 5, 499, 998)]),
    ("datetime", [datetime(2024, 1, 2, 3, 4, 5, 999000),
                  datetime(2024, 12, 31, 23, 59, 59, 999999), date(2024, 1, 2)]),
    ("datetime", [datetime(9999, 12, 31, 23, 59, 59, 999999)]),
    ("smalldatetime", [datetime(2024, 1, 2, 3, 4, 29, 999999), datetime(2024, 1, 2, 3, 4, 30),
                       datetime(2024, 12, 31, 23, 59, 59), date(2024, 1, 2)]),
    ("uniqueidentifier", [uuid.UUID("6F9619FF-8B86-D011-B42D-00C04FC964FF"),
                          uuid.UUID(int=7)]),
    ("uniqueidentifier", ["6F9619FF-8B86-D011-B42D-00C04FC964FF",
                          "6f9619ff-8b86-d011-b42d-00c04fc964fe", uuid.UUID(int=7)]),
]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,values", _BCP_PARITY_CASES,
                         ids=[f"{i}-{t}" for i, (t, _) in enumerate(_BCP_PARITY_CASES)])
def test_bcp_matches_plain_path(bcp_cursor: pyodbc.Cursor, column_type: str, values: list):
    """
    For every pairing of Python value and column type the BCP fast path carries out itself, it
    stores exactly what the plain fast_executemany path stores.  This pins the driver's
    conversions and the rounding the path reproduces for dates and times.
    """
    bcp, plain = _load_both(bcp_cursor, column_type, values + [None])
    assert bcp == plain


_BCP_BOTH_FAIL_CASES = [
    ("decimal(10,2)", Decimal("1.234"), True),
    ("numeric(38,0)", Decimal("1" * 27 + ".25"), True),
    ("int", Decimal("2.5"), True),
    ("tinyint", 300, True),
    ("bit", 5, True),
    ("int", 2.0 ** 40, True),
    ("time(3)", time(3, 4, 5, 123456), True),
    ("time(3)", "13:14:15.123456", False),
    ("time(7)", "24:00:00", False),
    ("time(7)", "1:2:3", False),
    ("date", datetime(2024, 1, 2, 3, 4, 5), False),
    ("varchar(3)", "toolong", False),
    ("varchar(2)", "ab ", False),
    ("nvarchar(2)", "a  ", False),
    ("char(2)", "ab ", False),
    ("varchar(3)", "\U0001D518\U0001D518", False),
    ("varbinary(2)", b"\x01\x02\x00", False),
    ("uniqueidentifier", "{6F9619FF-8B86-D011-B42D-00C04FC964FF}", False),
    ("uniqueidentifier", "6F9619FF8B86D011B42D00C04FC964FF", False),
    ("uniqueidentifier", " 6F9619FF-8B86-D011-B42D-00C04FC964FF", False),
    ("uniqueidentifier", "6F9619FF-8B86-D011-B42D-00C04FC964FG", False),
    ("uniqueidentifier", 5, False),
]


def _raised_by_bcp(exc: pyodbc.Error) -> bool:
    """
    Whether an error came from a BCP load rather than the plain path: the driver names the bcp_
    function that failed, and the path's own conversion errors carry their SQLSTATE last.
    """
    return "(bcp_" in str(exc) or exc.args[1:] in (("22003",), ("22018",))


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,value,runs", _BCP_BOTH_FAIL_CASES,
                         ids=[t for t, _, _ in _BCP_BOTH_FAIL_CASES])
def test_bcp_fails_where_plain_path_fails(bcp_cursor: pyodbc.Cursor, column_type: str, value,
                                          runs: bool):
    """
    A value the plain path rejects raises with BCP enabled too: either BCP carries out the load
    and stores nothing, or it declines and the plain path stores what it always does.  `runs`
    says which, as the error shows (see _raised_by_bcp).
    """
    cur = bcp_cursor
    stored = {}
    for bcp in (False, True):
        cur.execute("drop table if exists t1")
        cur.execute(f"create table t1(id int, v {column_type})")
        with pytest.raises(pyodbc.Error) as raised:
            _executemany(cur, "insert into t1 values (?, ?)", [(1, None), (2, value)], bcp=bcp)
        assert _raised_by_bcp(raised.value) == (bcp and runs)
        stored[bcp] = [tuple(r) for r in cur.execute("select id, v from t1")]
    assert stored[True] in ([], stored[False])


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_loads_text_ntext_and_image(bcp_cursor: pyodbc.Cursor):
    """
    BCP loads the deprecated text, ntext and image types.  The plain path sizes their buffers
    from the 2 GB column size, so it fails on a large load, and on 32-bit Python on any load,
    which is why these aren't compared with it.
    """
    cur = bcp_cursor
    cur.execute("create table t1(id int, t text, n ntext, i image)")
    rows = [(0, "abc", "é", b"\x01\x02"), (1, "", "", b""), (2, None, None, None)]
    assert _executemany(cur, "insert into t1 values (?, ?, ?, ?)", rows) == len(rows)
    assert [tuple(r) for r in cur.execute("select id, t, n, i from t1 order by id")] == rows


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_datetimeoffset(bcp_cursor: pyodbc.Cursor):
    """
    datetimeoffset stores what a user would expect, not what the plain path stores (the driver
    gives it the client machine's local offset): an aware datetime keeps its own offset, a
    naive one or a date is stored at +00:00, and the fraction is truncated to the column's
    scale.  An offset SQL Server cannot hold, or a utcoffset() that returns no timedelta,
    raises and stores nothing.
    """
    cur = bcp_cursor
    cur.execute("create table t1(id int, v datetimeoffset(3))")
    newfoundland = timezone(-timedelta(hours=3, minutes=30))
    rows = [
        (0, datetime(2024, 4, 2, 3, 4, 5, 123456, tzinfo=_BCP_TZ)),
        (1, datetime(2024, 1, 2, 3, 4, 5, tzinfo=newfoundland)),
        (2, datetime(2024, 4, 2, 3, 4, 5, 999999)),
        (3, date(2024, 1, 2)),
        (4, None),
    ]
    assert _executemany(cur, "insert into t1 values (?, ?)", rows) == len(rows)
    assert _fetch(cur, "t1", "datetimeoffset") == [
        "2024-04-02 03:04:05.123 +05:30",
        "2024-01-02 03:04:05.000 -03:30",
        "2024-04-02 03:04:05.999 +00:00",
        "2024-01-02 00:00:00.000 +00:00",
        None,
    ]

    for offset in (timedelta(hours=15), timedelta(seconds=90)):
        with pytest.raises(ValueError):
            _executemany(cur, "insert into t1 values (?, ?)",
                         [(5, None), (6, datetime(2024, 1, 2, tzinfo=timezone(offset)))])

    class OddOffset(datetime):
        def utcoffset(self) -> int:
            return 5

    with pytest.raises(TypeError):
        _executemany(cur, "insert into t1 values (?, ?)",
                     [(7, OddOffset(2024, 1, 2, tzinfo=_BCP_TZ))])
    assert cur.execute("select count(*) from t1").fetchval() == len(rows)


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_decimals_follow_the_context_precision(bcp_cursor: pyodbc.Cursor):
    """
    Like the plain path, BCP rounds a Decimal to the thread's decimal context precision, so a
    context with more than the default 28 digits keeps every digit.
    """
    value = Decimal("1" * 20 + "." + "1" * 10)
    with localcontext() as context:
        context.prec = 40
        bcp, plain = _load_both(bcp_cursor, "numeric(38,10)", [value])
    assert bcp == plain == [value]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_rejects_non_finite_decimal(bcp_cursor: pyodbc.Cursor):
    """A NaN or infinite Decimal raises an error through BCP and stores nothing."""
    cur = bcp_cursor
    cur.execute("create table t1(id int, v decimal(10, 2))")
    for value in (Decimal("NaN"), Decimal("-Infinity")):
        with pytest.raises(pyodbc.Error):
            rows = [(1, Decimal("1.5")), (2, value)]
            _executemany(cur, "insert into t1 values (?, ?)", rows)
    assert cur.execute("select count(*) from t1").fetchval() == 0


_BCP_FALLBACK_CASES = [
    ("datetime2(3)", ["2024-01-02 03:04:05"]),
    ("time(7)", [" 13:14:15", "13:14:15."]),
    ("money", [1.5]),
    ("decimal(38,0)", [2 ** 70]),
    ("uniqueidentifier", [uuid.UUID(int=7).bytes_le]),
    ("nvarchar(36)", [uuid.UUID(int=7)]),
    ("varchar(36)", [uuid.UUID(int=7)]),
    ("nvarchar(20)", ["a", 1.5]),
    ("varchar(1)", [42]),
    ("varchar(3)", [12345.678]),
    ("varchar(5)", ["ab", 123456]),
    ("varchar(12) collate Japanese_CI_AS", ["東京東京東京東"]),
    ("char(4) collate Latin1_General_100_CI_AS_SC_UTF8", ["ééé"]),
]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,values", _BCP_FALLBACK_CASES,
                         ids=[t for t, _ in _BCP_FALLBACK_CASES])
def test_bcp_declines_values_it_cannot_reproduce(bcp_cursor: pyodbc.Cursor, column_type: str,
                                                 values: list):
    """Pairings BCP cannot store as the plain path does are left to that path."""
    bcp, plain = _load_both(bcp_cursor, column_type, values, expect_bcp=False)
    assert bcp == plain


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,scanned,swapped,entry", [
    ("date", date(2024, 1, 2), time(1, 2, 3), None),
    ("date", date(2024, 1, 2), 5, None),
    ("date", date(2024, 1, 2), datetime(2024, 1, 2, 3, 4, 5), None),
    ("time(3)", "13:14:15", date(2024, 1, 2), None),
    ("time(3)", "13:14:15", "13:14:15.123456", None),
    ("nvarchar(3)", "abc", "abcd", None),
    ("varbinary(2)", b"ab", b"abc", None),
    ("datetime2(3)", datetime(2024, 1, 2), Decimal(20240102),
     (pyodbc.SQL_TYPE_TIMESTAMP, 27, 7)),
    ("nvarchar(10)", "abc", "abcdefgh", (pyodbc.SQL_WVARCHAR, 5, 0)),
    ("varchar(20)", "abc", "thin space", (pyodbc.SQL_WVARCHAR, 20, 0)),
    ("int", None, 2.5, (pyodbc.SQL_DOUBLE, 15, 0)),
    ("varchar(5)", 12345, 1234567, None),
    ("varchar(30)", 1.5, 7, None),
])
def test_bcp_rejects_a_value_changed_while_sending(bcp_cursor: pyodbc.Cursor, column_type: str,
                                                   scanned, swapped, entry):
    """
    A value swapped into a row after the scan, by Python code run while the row is sent, raises
    and the call stores nothing, unless it suits the column, and its entry, as the scan would
    have required.
    """
    row = [1, None, scanned]

    class SwappingDecimal(Decimal):
        def normalize(self, context=None) -> Decimal:
            row[2] = swapped
            return super().normalize(context)

    row[1] = SwappingDecimal("1.5")
    bcp_cursor.execute(f"create table t1(id int, d decimal(10, 2), v {column_type})")
    bcp_cursor.setinputsizes([None, None, entry] if entry else None)
    try:
        with pytest.raises(TypeError):
            _executemany(bcp_cursor, "insert into t1 values (?, ?, ?)", [row])
    finally:
        bcp_cursor.setinputsizes(None)
    assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("entry", [None, (pyodbc.SQL_NUMERIC, 6, 2)])
def test_bcp_requires_normalize_to_return_a_decimal(bcp_cursor: pyodbc.Cursor, entry):
    """
    A Decimal subclass whose normalize() returns something else raises, as the plain path does
    when it reads the digits of what came back, and the call stores nothing.
    """

    class OddDecimal(Decimal):
        def normalize(self, context=None) -> int:
            return 5

    cur = bcp_cursor
    for bcp in (False, True):
        cur.execute("drop table if exists t1")
        cur.execute("create table t1(id int, v decimal(10, 2))")
        cur.setinputsizes([None, entry])
        try:
            with pytest.raises((AttributeError, TypeError)):
                _executemany(cur, "insert into t1 values (?, ?)", [(1, OddDecimal("1.5"))],
                             bcp=bcp)
        finally:
            cur.setinputsizes(None)
        assert cur.execute("select count(*) from t1").fetchval() == 0


_BCP_DECLINED_STATEMENTS = [
    ("insert into t1 values (?, ?); delete from t2", [(1, 2)]),
    ("insert into t1 values (?, ? * 2)", [(1, 2)]),
    ("insert into t1 values (?, 5)", [(1,)]),
    ("insert into t1 (id, v) values (?, ?), (?, ?)", [(1, 2, 3, 4)]),
    ("insert top (1) into t1 values (?, ?)", [(1, 2)]),
    ("insert into t1 with (tablock) values (?, ?)", [(1, 2)]),
    ("insert into t1 select ?, ?", [(1, 2)]),
    ("insert into t1 values (?, ?)", [(1, 2, 3)]),
    ('insert into "t1" values (?, ?)', [(1, 2)]),
    ("insert into t1 (id, key) values (?, ?)", [(1, 2)]),
]


def _statement_outcome(cur: pyodbc.Cursor, sql: str, rows: list, bcp: bool) -> tuple:
    """
    Runs sql against fresh t1(id, v) and t2(id) tables and returns the rowcount (or the error
    class raised) with what both tables then hold.
    """
    for table in ("t1", "t2"):
        cur.execute(f"drop table if exists {table}")
    cur.execute("create table t1(id int, v int)")
    cur.execute("create table t2(id int)")
    cur.execute("insert into t2 values (0)")
    try:
        outcome = _executemany(cur, sql, rows, bcp=bcp)
    except pyodbc.Error as ex:
        outcome = type(ex)
    t1 = [tuple(r) for r in cur.execute("select id, v from t1 order by id")]
    t2 = [r[0] for r in cur.execute("select id from t2")]
    return outcome, t1, t2


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("sql,rows", _BCP_DECLINED_STATEMENTS,
                         ids=[s for s, _ in _BCP_DECLINED_STATEMENTS])
def test_bcp_declines_statements_it_cannot_carry_out(bcp_cursor: pyodbc.Cursor, sql: str,
                                                     rows: list):
    """
    Only `INSERT INTO t [(cols)] VALUES (?, ...)` with one value per marker goes through BCP:
    anything else in the statement (more statements, expressions, literals, several rows, TOP,
    table hints, other sources, extra values, quoted or reserved identifiers) would not do as
    written under a bulk copy, so BCP declines and the outcome is exactly the plain path's.
    """
    with_bcp = _statement_outcome(bcp_cursor, sql, rows, bcp=True)
    assert with_bcp == _statement_outcome(bcp_cursor, sql, rows, bcp=False)


def _bcp_row(i: int) -> tuple:
    """One row of the type-coverage test: every Python type the BCP fast path sends."""
    month, day = (i % 12) + 1, (i % 28) + 1
    hms = (i % 24, i % 60, (i * 7) % 60)
    return (
        i,
        5_000_000_000 + i,
        bool(i % 2),
        i + 0.5,
        "x" * (i % 400),
        "é東\U0001D518" * (i % 50),
        Decimal(f"{i}.{i % 10000:04d}"),
        Decimal(10 ** 20 + i),
        time(i % 24, i % 60, (i * 7) % 60, (i * 101) % 1000000),
        date(2020, month, day),
        datetime(2021, month, day, *hms, (i * 101) % 1000000),
        datetime(2022, month, day, *hms, (i * 101) % 1000000),
        datetime(2023, month, day, *hms, (i * 1009) % 1000000, tzinfo=_BCP_TZ),
        bytes([i % 256]) * (i % 30),
    )


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_fast_executemany(bcp_cursor: pyodbc.Cursor):
    """
    A load across every supported type, with a row of NULLs, buffers that grow, values beyond
    32 and 64 bits and manual batching, stores the same rows as the plain fast path.
    """
    cur = bcp_cursor
    columns = """
        id int, c_bigint bigint, c_bit bit, c_float float, c_varchar varchar(1000),
        c_nvarchar nvarchar(200), c_decimal decimal(18, 4), c_numeric numeric(38, 0),
        c_time time(7), c_date date, c_datetime2 datetime2(7), c_datetime datetime,
        c_smalldatetime smalldatetime, c_varbinary varbinary(50)
    """
    cur.execute(f"create table t1({columns})")
    cur.execute(f"create table t2({columns})")

    n = 500
    rows = [_bcp_row(i) for i in range(n)]
    rows[7] = (7,) + (None,) * 13
    sql = "insert into {} values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    cur.bcp_batch_rows = 100
    assert _executemany(cur, sql.format("t1"), rows) == n
    _executemany(cur, sql.format("t2"), rows, bcp=False)

    fetch = ("select id, c_bigint, c_bit, c_float, c_varchar, c_nvarchar, c_decimal, "
             "c_numeric, c_time, c_date, c_datetime2, c_datetime, c_smalldatetime, "
             "c_varbinary from {} order by id")
    loaded = [tuple(r) for r in cur.execute(fetch.format("t1"))]
    expected = [tuple(r) for r in cur.execute(fetch.format("t2"))]
    assert loaded == expected
    assert all(v is None for v in loaded[7][1:])


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_fast_executemany_nvarchar(bcp_cursor: pyodbc.Cursor):
    """
    str values reach nvarchar columns intact whatever the client's code page: Latin accents,
    CJK, a character outside the BMP, an empty string distinct from NULL, values long enough to
    grow the column buffers (one encoded by the codec, a longer one widened from Latin-1), a
    short value after them, and a first row whose value is None, which must not decide the
    column type.
    """
    values = [
        None,
        "Zürich, Ångström, façade, naïve",
        "東京 北京 서울",
        "\U0001D518 lies outside the BMP",
        "",
        None,
        "東京\U0001D518" * 500,
        "é" * 3000,
        "short again",
    ]
    cur = bcp_cursor
    cur.execute("create table t1(id int, s nvarchar(4000), m nvarchar(max))")
    rows = [(i, v, v) for i, v in enumerate(values)]
    assert _executemany(cur, "insert into t1 values (?, ?, ?)", rows) == len(rows)

    stored = cur.execute("select s, m from t1 order by id").fetchall()
    assert [row.s for row in stored] == values
    assert [row.m for row in stored] == values


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_fast_executemany_varchar_latin1(bcp_cursor: pyodbc.Cursor):
    """
    str values whose characters all fit code page 1252 reach a varchar column in the default
    Latin1 collation as that code page's single bytes, not as UTF-8 read as ANSI text.
    """
    values = ["Zürich", "façade, crème brûlée", "Ångström ½ £ ©", "plain ascii"]
    cur = bcp_cursor
    cur.execute("create table t1(id int, s varchar(50) collate SQL_Latin1_General_CP1_CI_AS)")
    rows = list(enumerate(values))
    assert _executemany(cur, "insert into t1 values (?, ?)", rows) == len(values)

    stored = cur.execute("select s, cast(s as varbinary(50)) as raw from t1 order by id")
    stored = stored.fetchall()
    assert [row.s for row in stored] == values
    assert [row.raw for row in stored] == [v.encode("cp1252") for v in values]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_fast_executemany_none_first_row(bcp_cursor: pyodbc.Cursor):
    """
    A NULL in the first row does not decide a column's type: nullable int, float, bit and
    nvarchar columns with a NULL first value load, and so does a column NULL throughout.
    """
    rows = [
        (0, None, None, None, None, None),
        (1, 5, 1.5, True, "Zürich", None),
        (2, None, None, None, None, None),
        (3, -7, 2.5, False, "", None),
    ]
    cur = bcp_cursor
    cur.execute("create table t1(id int, n int, f float, b bit, s nvarchar(20), z int)")
    assert _executemany(cur, "insert into t1 values (?, ?, ?, ?, ?, ?)", rows) == len(rows)
    assert [tuple(r) for r in cur.execute("select * from t1 order by id")] == rows


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_fast_executemany_column_list(bcp_cursor: pyodbc.Cursor):
    """
    A column list is mapped to table ordinals, in any order and as a subset (the SQLAlchemy
    shape): the unlisted columns are left to the server, which fills in identity values,
    defaults and NULLs exactly as for the INSERT.
    """
    cur = bcp_cursor
    cur.execute("""
        create table t1(id int identity, a int, b varchar(20), c int, d float default 9.5)
    """)

    reordered = [(i * 10, i, f"b{i}", i + 0.5) for i in range(50)]
    sql = "insert into t1 (c, a, b, d) values (?, ?, ?, ?)"
    assert _executemany(cur, sql, reordered) == 50
    row = cur.execute("select a, b, c, d from t1 where a = 7").fetchone()
    assert tuple(row) == (7, "b7", 70, 7.5)

    cur.execute("truncate table t1")
    subset = [(i, f"name{i}") for i in range(20)]
    assert _executemany(cur, "insert into t1 (a, b) values (?, ?)", subset) == 20
    rows = cur.execute("select id, a, b, c, d from t1 order by id").fetchall()
    assert [tuple(r) for r in rows] == [(i + 1, i, f"name{i}", None, 9.5) for i in range(20)]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_positional_insert_skips_identity_and_computed(bcp_cursor: pyodbc.Cursor):
    """Without a column list the values go to the columns T-SQL fills, skipping identity and
    computed ones."""
    cur = bcp_cursor
    cur.execute("create table t1(id int identity, a int, c as a * 2, b nvarchar(10))")
    assert _executemany(cur, "insert into t1 values (?, ?)", [(5, "x"), (6, "y")]) == 2
    stored = [tuple(r) for r in cur.execute("select id, a, c, b from t1 order by id")]
    assert stored == [(1, 5, 10, "x"), (2, 6, 12, "y")]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_table_names(bcp_cursor: pyodbc.Cursor):
    """
    Temporary tables, names outside ASCII and three-part names go through BCP, the last using
    the named database's columns even where the current database has a table of the same name
    with its columns in another order.
    """
    cur = bcp_cursor
    cur.execute("create table #bcp_temp(a int, b nvarchar(5))")
    assert _executemany(cur, "insert into #bcp_temp values (?, ?)", [(1, "é")]) == 1
    assert tuple(cur.execute("select a, b from #bcp_temp").fetchone()) == (1, "é")

    cur.execute("drop table if exists [bcp_ü]")
    cur.execute("create table [bcp_ü](a int)")
    try:
        assert _executemany(cur, "insert into [bcp_ü] values (?)", [(1,), (2,)]) == 2
        assert cur.execute("select count(*) from [bcp_ü]").fetchval() == 2
    finally:
        cur.execute("drop table [bcp_ü]")

    cur.execute("drop table if exists tempdb.dbo.bcp_three_part")
    cur.execute("create table tempdb.dbo.bcp_three_part(a int, b int)")
    cur.execute("create table t1(b int, a int)")
    try:
        sql = "insert into tempdb.dbo.bcp_three_part (a, b) values (?, ?)"
        assert _executemany(cur, sql, [(1, 2)]) == 1
        row = cur.execute("select a, b from tempdb.dbo.bcp_three_part").fetchone()
        assert tuple(row) == (1, 2)
    finally:
        cur.execute("drop table tempdb.dbo.bcp_three_part")


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_keeps_insert_semantics(bcp_cursor: pyodbc.Cursor):
    """
    BCP checks CHECK and foreign key constraints and stores an explicit None as NULL rather
    than the column default, as the INSERT it replaces does.
    """
    cur = bcp_cursor
    cur.execute("create table t2(id int primary key)")
    cur.execute("insert into t2 values (1), (2), (3)")
    cur.execute("create table t1(id int check (id >= 0) references t2(id), d int default 42)")

    assert _executemany(cur, "insert into t1 values (?, ?)", [(1, None), (2, 5)]) == 2
    stored = [tuple(r) for r in cur.execute("select id, d from t1 order by id")]
    assert stored == [(1, None), (2, 5)]

    for bad in (-1, 9):
        with pytest.raises(pyodbc.IntegrityError):
            _executemany(cur, "insert into t1 values (?, ?)", [(3, 1), (bad, 1)])
    assert cur.execute("select count(*) from t1").fetchval() == 2
    untrusted = cur.execute("select count(*) from sys.check_constraints "
                            "where is_not_trusted = 1 and parent_object_id = object_id('t1')")
    untrusted = untrusted.fetchval()
    assert untrusted == 0
    cur.execute("drop table t1")


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_trigger_tables_use_plain_path(bcp_cursor: pyodbc.Cursor):
    """
    A table with an enabled INSERT trigger is left to the plain path: a trigger that rolls back
    a bulk copy run in a transaction leaves the driver unable to use the connection again.  The
    trigger fires, and after one rolls back a load the connection still works.
    """
    cur = bcp_cursor
    cur.execute("create table t1(id int)")
    cur.execute("create table t2(id int)")
    cur.execute("create trigger t1_audit on t1 after insert as "
                "insert into t2 select id from inserted; "
                "if exists (select 1 from inserted where id < 0) rollback transaction")

    assert _executemany(cur, "insert into t1 values (?)", [(1,), (2,)]) == -1
    assert [r[0] for r in cur.execute("select id from t2 order by id")] == [1, 2]
    with pytest.raises(pyodbc.Error):
        _executemany(cur, "insert into t1 values (?)", [(3,), (-4,)])
    assert cur.execute("select count(*) from t1 where id < 0").fetchval() == 0


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_failure_rolls_back_the_call(bcp_cursor: pyodbc.Cursor):
    """
    A failure part way through a load raises and leaves nothing from the call; with batching
    in autocommit mode, the batches completed before it stay committed and only the failing
    one goes.
    """
    cur = bcp_cursor
    cur.execute("create table t1(id int primary key)")
    rows = [(1,), (2,), (3,), (3,), (5,)]
    with pytest.raises(pyodbc.IntegrityError):
        _executemany(cur, "insert into t1 values (?)", rows)
    assert cur.execute("select count(*) from t1").fetchval() == 0

    cur.bcp_batch_rows = 2
    with pytest.raises(pyodbc.IntegrityError):
        _executemany(cur, "insert into t1 values (?)", rows)
    assert [r[0] for r in cur.execute("select id from t1 order by id")] == [1, 2]
    assert cur.connection.autocommit


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_failure_in_manual_commit_mode(bcp_cursor: pyodbc.Cursor):
    """
    In manual-commit mode a failed load is undone but the caller's earlier work is kept, and
    a load, batched or not, stays in the caller's transaction until it commits or rolls back.
    """
    bcp_cursor.execute("create table t1(id int primary key)")
    bcp_cursor.execute("create table t2(id int)")
    cnxn = bcp_connect(autocommit=False)
    try:
        cur = cnxn.cursor()
        cur.execute("insert into t2 values (1)")
        with pytest.raises(pyodbc.IntegrityError):
            _executemany(cur, "insert into t1 values (?)", [(1,), (1,)])
        assert cur.execute("select count(*) from t1").fetchval() == 0
        cnxn.commit()
        assert bcp_cursor.execute("select count(*) from t2").fetchval() == 1

        cur.bcp_batch_rows = 2
        assert _executemany(cur, "insert into t1 values (?)", [(1,), (2,), (3,)]) == 3
        cnxn.rollback()
        assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0
    finally:
        cnxn.close()


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_falls_back_without_bcp_connection(cursor: pyodbc.Cursor):
    """A connection opened without SQL_COPT_SS_BCP runs the plain path instead of failing."""
    cursor.execute("create table t1(id int, s nvarchar(10))")
    assert _executemany(cursor, "insert into t1 values (?, ?)", [(1, "é"), (2, None)]) == -1
    stored = [tuple(r) for r in cursor.execute("select id, s from t1 order by id")]
    assert stored == [(1, "é"), (2, None)]


@pytest.mark.skipif(not IS_MSODBCSQL or IS_BCP_DRIVER,
                    reason='Needs a SQL Server driver other than Microsoft ODBC Driver 13+')
def test_bcp_falls_back_on_older_drivers(bcp_cursor: pyodbc.Cursor):
    """
    With SQL Server Native Client or Microsoft ODBC Driver 11, the fast path runs the plain
    path and the low-level API raises NotImplementedError.
    """
    cur = bcp_cursor
    cur.execute("create table t1(id int, s nvarchar(10))")
    assert _executemany(cur, "insert into t1 values (?, ?)", [(1, "é"), (2, None)]) == -1
    stored = [tuple(r) for r in cur.execute("select id, s from t1 order by id")]
    assert stored == [(1, "é"), (2, None)]
    with pytest.raises(NotImplementedError):
        cur.connection.bcp_init("t1")


def _entry_for(values: list) -> tuple:
    """
    The setinputsizes entry a loader that types each parameter from the values it carries
    would pass: nvarchar as long as the longest str in UTF-16 units (max past 4,000), bit,
    bigint, double, numeric just wide enough for every Decimal, datetime2(7) or date, and
    nvarchar(1) for a column of NULLs, whatever its type.  None for anything else or a mix.
    """
    present = [v for v in values if v is not None]
    kinds = {type(v) for v in present}
    if not present:
        return (pyodbc.SQL_WVARCHAR, 1, 0)
    if kinds == {str}:
        longest = max(len(v.encode("utf-16-le")) // 2 for v in present)
        return (pyodbc.SQL_WVARCHAR, longest if longest <= 4000 else 0, 0)
    if kinds == {bool}:
        return (pyodbc.SQL_BIT, 1, 0)
    if kinds == {int} and all(-(2**63) <= v < 2**63 for v in present):
        return (pyodbc.SQL_BIGINT, 19, 0)
    if kinds == {float}:
        return (pyodbc.SQL_DOUBLE, 15, 0)
    if kinds == {Decimal}:
        places = max(max(-v.as_tuple().exponent, 0) for v in present)
        whole = max(len(str(int(abs(v)))) if int(abs(v)) else 0 for v in present)
        return (pyodbc.SQL_NUMERIC, max(whole + places, 1), places)
    if kinds == {datetime} and all(v.tzinfo is None for v in present):
        return (pyodbc.SQL_TYPE_TIMESTAMP, 27, 7)
    if kinds == {date}:
        return (pyodbc.SQL_TYPE_DATE, 10, 0)
    return None


_BCP_MAX_DATETIME = datetime(9999, 12, 31, 23, 59, 59, 999999)


def _day_end(*micros: int) -> list:
    """Datetimes in the last second of 2024 with the given microseconds."""
    return [datetime(2024, 12, 31, 23, 59, 59, us) for us in micros]


_BCP_ENTRY_CASES = [
    ("nvarchar(20)", ["abc", "東京 \U0001D518", ""]),
    ("nvarchar(max)", ["é" * 5000, ""]),
    ("nvarchar(max)", ["short"]),
    ("nchar(3)", ["é", ""]),
    ("varchar(20)", ["Zürich", "™ – “q” €", ""]),
    ("varchar(20) collate Polish_CI_AS", ["Łódź", "zażółć"]),
    ("varchar(20) collate Japanese_CI_AS", ["東京", "abc", "ｶﾅ"]),
    ("varchar(max)", ["x" * 9000, "a"]),
    ("char(5)", ["ab", ""]),
    ("text", ["abc", ""]),
    ("ntext", ["é", ""]),
    ("int", [0, -5, 2**31 - 1, -(2**31)]),
    ("bigint", [2**62, -(2**63), 2**63 - 1]),
    ("smallint", [-32768, 32767]),
    ("tinyint", [0, 255]),
    ("decimal(30,2)", [2**62 + 1, -7]),
    ("money", [3, -(2**40)]),
    ("smallmoney", [214748, -214748]),
    ("float", [2**53 + 1, 2**63 - 1, -(2**63), 12345678901234567]),
    ("real", [16777217, 2**62 + 1]),
    ("bit", [0, 1, 2, -1, 2**62]),
    ("bit", [True, False]),
    ("float", [0.1, 1e300, -2.5, 5e-324, -0.0]),
    ("real", [0.1, 16777217.0, 1e-40, 1e-46, 3.4028234663852886e38, -1.1754942e-38]),
    ("decimal(10,2)", [2.675, 1.235, 0.1, 9.995, -0.004, 0.005]),
    ("decimal(38,10)", [2 / 3 * 1e10, 123456789.123456789, 1e27, -86484.03774682874, 1e-11]),
    ("decimal(38,30)", [0.1, 1 / 3, 5e-31, 1.5e-30, -5e-324]),
    ("decimal(38,0)", [9.9e37, 0.5, -0.5, 2.5, 1.5]),
    ("numeric(5,5)", [0.12345, -0.000005, 0.999994]),
    ("int", [2.5, -2.5, 2.9999999, -0.5, 1e9]),
    ("bigint", [-(2.0**63), 2.0**62 + 2048, 9.2e18]),
    ("smallint", [-32768.9, 32767.9]),
    ("tinyint", [-0.5, 255.9]),
    ("bit", [0.0, 0.5, -2.5, 1e-300, -0.0]),
    ("decimal(18,4)", [Decimal(x) for x in ("1.23456", "-1.00005", "9.99995", "0.00004",
                                            "-0.00005")]),
    ("decimal(10,0)", [Decimal("2.5"), Decimal("-2.5"), Decimal("0.4999")]),
    ("numeric(38,10)", [Decimal("1" * 20 + "." + "15" * 6), Decimal("-0." + "9" * 12)]),
    ("money", [Decimal("1.23456"), Decimal("-1.00005"), Decimal("922337203685477.58074")]),
    ("smallmoney", [Decimal("-1.99999"), Decimal("214748.36474")]),
    ("int", [Decimal("2.5"), Decimal("-2.5"), Decimal(7), Decimal("1E+3")]),
    ("tinyint", [Decimal("0.5"), Decimal("-0.999"), Decimal("255.9")]),
    ("smallint", [Decimal("-32768.9"), Decimal("32767.9")]),
    ("bigint", [Decimal("9223372036854775807.9"), Decimal("-9223372036854775808.9")]),
    ("float", [Decimal("0.1"), Decimal("4.35"), Decimal("-0"), Decimal(9007199254740993)]),
    ("float", [Decimal("1.0000000000000001110223024625156540424")]),
    ("bit", [Decimal(x) for x in ("0", "0.5", "-0.00001", "2", "0.000")]),
    ("datetime", _day_end(998600, 998300, 1667, 1666) + [_BCP_MAX_DATETIME]),
    ("datetime2(0)", [*_day_end(998600, 499999, 500000), _BCP_MAX_DATETIME,
                      datetime(2024, 1, 2, 12, 0, 29, 999000)]),
    ("datetime2(2)", [datetime(9999, 12, 31, 23, 59, 59, 994999),
                      datetime(9999, 12, 31, 23, 59, 59, 995000)]),
    ("datetime2(3)", [*_day_end(999600, 1500, 1499),
                      _BCP_MAX_DATETIME.replace(microsecond=999600)]),
    ("datetime2(7)", [datetime(2024, 1, 2, 3, 4, 5, 123456)]),
    ("smalldatetime", [datetime(2024, 1, 2, 3, 4, 29, 999999), datetime(2024, 1, 2, 3, 4, 30),
                       datetime(2024, 12, 31, 23, 59, 30)]),
    ("date", _day_end(999999) + [datetime(2024, 1, 2)]),
    ("datetimeoffset(0)", _day_end(500000) + [_BCP_MAX_DATETIME]),
    ("datetimeoffset(3)", _day_end(1500, 999600)),
    ("time(0)", _day_end(500000, 499999)),
    ("time(3)", _day_end(999600, 1500)),
    ("date", [date(2024, 2, 29), date(1, 1, 1)]),
    ("datetime2(3)", [date(2024, 1, 2)]),
    ("datetime", [date(2024, 1, 2)]),
    ("smalldatetime", [date(2024, 1, 2)]),
    ("datetimeoffset(2)", [date(2024, 1, 2)]),
    ("int", []),
    ("datetime2(3)", []),
    ("decimal(10,2)", []),
    ("uniqueidentifier", []),
    ("xml", []),
]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,values", _BCP_ENTRY_CASES,
                         ids=[f"{i}-{t}" for i, (t, _) in enumerate(_BCP_ENTRY_CASES)])
def test_bcp_matches_plain_path_with_input_sizes(bcp_cursor: pyodbc.Cursor, column_type: str,
                                                 values: list):
    """
    Under setinputsizes the plain path sends each value as its entry's type and the server
    converts it to the column's, rounding where the plain path without entries truncates.  BCP
    carries out every pairing of entry and column type it supports and stores exactly what the
    plain path stores, including values that round past midnight, and the sign of a zero.  A
    column of NULLs under an nvarchar entry, which says nothing about its values, makes BCP
    decline only for a binary column, which the server rejects it for.
    """
    values = values + [None]
    sizes = [_entry_for(list(range(len(values)))), _entry_for(values)]
    bcp, plain = _load_both(bcp_cursor, column_type, values, sizes=sizes)
    assert list(map(repr, bcp)) == list(map(repr, plain))


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("values,ascii_only",
                         [(["ééé", "Zürich €", "abc"], False), (["abc"], True)])
@pytest.mark.parametrize("sized", [False, True])
def test_bcp_text_into_a_utf8_collation(bcp_cursor: pyodbc.Cursor, values: list,
                                        ascii_only: bool, sized: bool):
    """
    Text for a varchar column with a UTF-8 collation goes by BCP from Driver 17.4, the first to
    convert it to UTF-8; older drivers send bytes the server rejects, so BCP takes only ASCII
    text there.  Either way the column stores what the plain path stores, with or without an
    entry.  (Without one the plain path passes text through the database's code page, so these
    values stay within code page 1252.)
    """
    values = values + [None]
    sizes = [None, _entry_for(values)] if sized else None
    expect_bcp = ascii_only or DRIVER_VERSION[:2] >= (17, 4)
    bcp, plain = _load_both(bcp_cursor, "varchar(20) collate Latin1_General_100_CI_AS_SC_UTF8",
                            values, expect_bcp=expect_bcp, sizes=sizes)
    assert bcp == plain


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_matches_plain_path_with_input_sizes_at_random():
    """
    BCP stores what the plain path stores for random numbers and datetimes under their
    entries, across the conversions BCP reproduces itself: doubles into decimal and real,
    Decimals into decimal, money, int and float, and datetimes into the date and time types.
    """
    rng = random.Random(1496)
    floats = [rng.uniform(-1e6, 1e6) for _ in range(300)]
    floats += [rng.uniform(-1, 1) * 10 ** rng.randint(-12, 12) for _ in range(300)]
    decimals = [Decimal(f"{rng.uniform(-1e7, 1e7):.{rng.randint(0, 9)}f}") for _ in range(600)]
    stamps = [datetime(2024, 1, 1) + timedelta(seconds=rng.randint(0, 86400 * 366),
                                               microseconds=rng.randint(0, 999999))
              for _ in range(600)]
    stamps += _day_end(*range(999000, 1000000, 7))
    cases = [("decimal(18,4)", floats), ("decimal(38,10)", floats), ("real", floats),
             ("decimal(18,4)", decimals), ("money", decimals), ("int", decimals),
             ("float", decimals), ("datetime", stamps), ("datetime2(0)", stamps),
             ("datetime2(3)", stamps), ("smalldatetime", stamps), ("time(3)", stamps),
             ("datetimeoffset(2)", stamps)]
    cnxn = bcp_connect(autocommit=False)
    cur = cnxn.cursor()
    cur.execute("set lock_timeout 30000")
    try:
        for column_type, values in cases:
            for table in ("t1", "t2"):
                cur.execute(f"drop table if exists {table}")
            bcp, plain = _load_both(cur, column_type, values,
                                    sizes=[None, _entry_for(values)])
            cnxn.commit()
            assert list(map(repr, bcp)) == list(map(repr, plain)), column_type
    finally:
        cnxn.rollback()
        for table in ("t1", "t2"):
            cur.execute(f"drop table if exists {table}")
        cnxn.commit()
        cnxn.close()


_TIMESTAMP = pyodbc.SQL_TYPE_TIMESTAMP
_BCP_ENTRY_FALLBACK_CASES = [
    ("nvarchar(30)", [0.1, 1e20], None),
    ("int", ["5"], None),
    ("datetime2(3)", ["2024-01-02 03:04:05"], None),
    ("money", [1.23456], None),
    ("real", [Decimal("0.1")], None),
    ("nvarchar(10)", [5], None),
    ("int", [True], None),
    ("time(3)", [time(1, 2, 3, 456789)], (_TIMESTAMP, 27, 7)),
    ("datetime2(7)", [datetime(2024, 1, 2, 3, 4, 5, 123456)], (_TIMESTAMP, 23, 3)),
    ("datetime2(0)", [datetime(2024, 1, 2, 3, 4, 5, 600000)], [_TIMESTAMP, 27, 7]),
    ("datetime2(0)", [datetime(2024, 1, 2, 3, 4, 5, 600000)], 27),
    ("nvarchar(10)", ["abc"], (pyodbc.SQL_WCHAR, 5, 0)),
    ("nvarchar(10)", ["é東"], (pyodbc.SQL_VARCHAR, 5, 0)),
    ("float", [0.1], (pyodbc.SQL_REAL, 7, 0)),
    ("varchar(3)", ["ab   "], None),
    ("varchar(12) collate Japanese_CI_AS", ["é" * 7], None),
    ("varchar(20)", ["thin space", "łódź ā"], None),
    ("char(10) collate Polish_CI_AS", ["a b"], None),
    ("text", [" "], None),
    ("varchar(20) collate Japanese_CI_AS", ["é"], None),
]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,values,entry", _BCP_ENTRY_FALLBACK_CASES,
                         ids=[t for t, _, _ in _BCP_ENTRY_FALLBACK_CASES])
def test_bcp_declines_input_sizes_it_cannot_reproduce(bcp_cursor: pyodbc.Cursor,
                                                      column_type: str, values: list, entry):
    """
    Entries and pairings whose conversion BCP does not reproduce are left to the plain path:
    other entry forms and types, and values whose entry the server converts differently than
    BCP would, such as text into a number column, a double into text, or text with characters
    a varchar column's code page lacks, which the server and the driver substitute differently.
    `entry` None stands for the entry _entry_for gives.
    """
    sizes = [None, entry if entry is not None else _entry_for(values)]
    bcp, plain = _load_both(bcp_cursor, column_type, values, expect_bcp=False, sizes=sizes)
    assert bcp == plain


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,entry", [
    ("varbinary(10)", (pyodbc.SQL_WVARCHAR, 1, 0)),
    ("varbinary(max)", (pyodbc.SQL_WVARCHAR, 1, 0)),
    ("binary(4)", (pyodbc.SQL_WVARCHAR, 1, 0)),
    ("int", (pyodbc.SQL_TYPE_DATE, 10, 0)),
    ("int", (_TIMESTAMP, 27, 7)),
    ("date", (pyodbc.SQL_BIGINT, 19, 0)),
    ("date", (pyodbc.SQL_BIT, 1, 0)),
    ("time(3)", (pyodbc.SQL_DOUBLE, 15, 0)),
    ("uniqueidentifier", (pyodbc.SQL_NUMERIC, 5, 2)),
    ("xml", (pyodbc.SQL_BIGINT, 19, 0)),
])
def test_bcp_fails_where_plain_path_rejects_an_entry_for_nulls(bcp_cursor: pyodbc.Cursor,
                                                               column_type: str, entry):
    """
    The server converts even NULLs from their entry's type, so the plain path rejects a column
    of NULLs whose entry type has no implicit conversion to the column's, and BCP declines it.
    """
    cur = bcp_cursor
    for bcp in (False, True):
        cur.execute("drop table if exists t1")
        cur.execute(f"create table t1(id int, v {column_type})")
        cur.setinputsizes([None, entry])
        try:
            with pytest.raises(pyodbc.Error):
                _executemany(cur, "insert into t1 values (?, ?)", [(1, None), (2, None)],
                             bcp=bcp)
        finally:
            cur.setinputsizes(None)
        assert cur.execute("select count(*) from t1").fetchval() == 0


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_input_sizes_follow_the_parameters(bcp_cursor: pyodbc.Cursor):
    """
    Entries belong to the statement's parameters, not the table's columns: with a column list
    that reorders them, an identity column and a column left to its default, each entry still
    applies to its own parameter, as on the plain path.
    """
    cur = bcp_cursor
    columns = ("id int identity, t datetime2(0), n decimal(10,2), s varchar(10), "
               "d int default 7")
    rows = [(Decimal("1.005"), datetime(2024, 1, 2, 3, 4, 5, 600000), "abc"),
            (Decimal("-2.345"), datetime(2024, 1, 2, 3, 4, 5, 400000), None)]
    sizes = [(pyodbc.SQL_NUMERIC, 4, 3), (_TIMESTAMP, 27, 7), (pyodbc.SQL_WVARCHAR, 3, 0)]
    stored = {}
    for table, bcp in (("t1", True), ("t2", False)):
        cur.execute(f"create table {table}({columns})")
        cur.setinputsizes(sizes)
        try:
            copied = _executemany(cur, f"insert into {table}(n, t, s) values (?, ?, ?)", rows,
                                  bcp=bcp)
        finally:
            cur.setinputsizes(None)
        assert copied == (len(rows) if bcp else -1)
        stored[bcp] = [tuple(r) for r in cur.execute(f"select * from {table} order by id")]
    assert stored[True] == stored[False] == [
        (1, datetime(2024, 1, 2, 3, 4, 6), Decimal("1.01"), "abc", 7),
        (2, datetime(2024, 1, 2, 3, 4, 5), Decimal("-2.35"), None, 7),
    ]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_without_an_entry_keeps_the_untyped_rules(bcp_cursor: pyodbc.Cursor):
    """
    A None entry, or a parameter past the end of the entries, leaves that column to the rules
    without setinputsizes, under which datetime2 truncates its fraction as the plain path does.
    """
    value = datetime(2024, 1, 2, 3, 4, 5, 999999)
    for sizes in ([None, None], [(pyodbc.SQL_BIGINT, 19, 0)]):
        bcp_cursor.execute("drop table if exists t1")
        bcp_cursor.execute("drop table if exists t2")
        bcp, plain = _load_both(bcp_cursor, "datetime2(0)", [value], sizes=sizes)
        assert bcp == plain == [datetime(2024, 1, 2, 3, 4, 5)]


_BCP_ENTRY_BOTH_FAIL_CASES = [
    ("decimal(5,4)", Decimal("9.99995"), None, True),
    ("decimal(10,4)", Decimal("1.234"), (pyodbc.SQL_NUMERIC, 5, 2), None),
    ("decimal(10,4)", Decimal("1234.5"), (pyodbc.SQL_NUMERIC, 4, 1), True),
    ("tinyint", Decimal("-1.0"), None, True),
    ("int", 2**31, None, True),
    ("tinyint", 256, None, True),
    ("decimal(5,2)", 1000, None, True),
    ("bigint", 2.0**63, None, True),
    ("int", 2.0**31, None, True),
    ("tinyint", -1.5, None, True),
    ("decimal(3,2)", 9.996, None, True),
    ("decimal(38,10)", 1e300, None, True),
    ("real", 1e39, None, True),
    ("real", 3.4028235677973366e38, None, True),
    ("bit", float("nan"), None, True),
    ("float", float("inf"), None, True),
    ("smalldatetime", datetime(2079, 6, 6, 23, 59, 30), None, True),
    ("smalldatetime", datetime(1899, 12, 31, 23, 59, 29), None, True),
    ("datetime", datetime(1752, 12, 31), None, True),
    ("time(3)", date(2024, 1, 2), None, False),
    ("nvarchar(3)", "abcd", None, False),
    ("nvarchar(10)", "abcd", (pyodbc.SQL_WVARCHAR, 2, 0), False),
    ("varchar(5) collate Latin1_General_100_CI_AS_SC_UTF8", "ééé", None, False),
    ("char(4) collate Japanese_CI_AS", "東京東京", None, False),
]


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
@pytest.mark.parametrize("column_type,value,entry,runs", _BCP_ENTRY_BOTH_FAIL_CASES,
                         ids=[t for t, _, _, _ in _BCP_ENTRY_BOTH_FAIL_CASES])
def test_bcp_fails_where_plain_path_fails_with_input_sizes(bcp_cursor: pyodbc.Cursor,
                                                           column_type: str, value, entry,
                                                           runs: bool | None):
    """
    A value the plain path rejects under its entry raises with BCP enabled too, and BCP stores
    nothing: overflow after the server's rounding, a Decimal wider than its entry, a non-finite
    double, a datetime out of its column's range, and text longer than its column or entry,
    counted in bytes for a multi-byte collation.  `entry` None stands for the entry _entry_for
    gives.  `runs` says whether BCP carries out the load or declines it, as the error shows
    (see _raised_by_bcp), or is None where both paths raise the same error.
    """
    cur = bcp_cursor
    sizes = [None, entry if entry is not None else _entry_for([value])]
    stored = {}
    for bcp in (False, True):
        cur.execute("drop table if exists t1")
        cur.execute(f"create table t1(id int, v {column_type})")
        cur.setinputsizes(sizes)
        try:
            with pytest.raises(pyodbc.Error) as raised:
                rows = [(1, None), (2, value)]
                _executemany(cur, "insert into t1 values (?, ?)", rows, bcp=bcp)
        finally:
            cur.setinputsizes(None)
        if runs is not None:
            assert _raised_by_bcp(raised.value) == (bcp and runs)
        stored[bcp] = [tuple(r) for r in cur.execute("select id, v from t1")]
    assert stored[True] in ([], stored[False])


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_column_names_in_case_sensitive_database(bcp_cursor: pyodbc.Cursor):
    """
    Column-list names are matched as the database compares identifiers: ignoring case in the
    usual case-insensitive collations, exactly in a case-sensitive one, where a name in the
    wrong case is left to the plain path to reject.
    """
    cur = bcp_cursor
    cur.execute("drop database if exists pyodbc_bcp_cs")
    try:
        cur.execute("create database pyodbc_bcp_cs collate Latin1_General_CS_AS")
    except pyodbc.Error:
        pytest.skip("cannot create a database")
    try:
        cur.execute("create table pyodbc_bcp_cs.dbo.t(id int, abc int)")
        sql = "insert into pyodbc_bcp_cs.dbo.t ({}) values (?, ?)"
        assert _executemany(cur, sql.format("id, abc"), [(1, 2)]) == 1
        with pytest.raises(pyodbc.ProgrammingError):
            _executemany(cur, sql.format("ID, abc"), [(3, 4)])
        cur.execute("create table t1(id int, abc int)")
        assert _executemany(cur, "insert into t1 (ID, ABC) values (?, ?)", [(5, 6)]) == 1
    finally:
        cur.execute("drop database pyodbc_bcp_cs")


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_low_level_api(bcp_cursor: pyodbc.Cursor):
    """The Connection.bcp_* methods load every host type, with NULLs, and report row counts."""
    cur = bcp_cursor
    cur.execute("""
        create table t1(i2 smallint, i4 int, i8 bigint, f4 real, f8 float, b bit,
                        n nvarchar(max), c varchar(20), v varbinary(20), u uniqueidentifier)
    """)
    cnxn = cur.connection
    types = [pyodbc.SQLINT2, pyodbc.SQLINT4, pyodbc.SQLINT8, pyodbc.SQLFLT4, pyodbc.SQLFLT8,
             pyodbc.SQLBIT, pyodbc.SQLNCHAR, pyodbc.SQLCHARACTER, pyodbc.SQLBINARY,
             pyodbc.SQLUNIQUEID]
    text = "Zürich 東京 \U0001D518"
    rows = [
        (1, 2, 2**40, 1.5, 0.1, True, text, "abc", b"\x00\xff", uuid.UUID(int=1)),
        (None,) * 10,
        (-1, -2, -3, -0.5, 1e300, False, "", b"xyz", bytearray(), uuid.UUID(int=2).bytes_le),
    ]
    cnxn.bcp_init("t1", keep_nulls=True)
    ctx = cnxn.bcp_bind_columns(types)
    for row in rows[:2]:
        cnxn.bcp_setrow(ctx, row)
        cnxn.bcp_sendrow()
    assert cnxn.bcp_batch(ctx) == 2
    with bcp_connect() as observer:
        assert observer.execute("select count(*) from t1").fetchval() == 2
    cnxn.bcp_setrow(ctx, list(rows[2]))
    cnxn.bcp_sendrow()
    assert cnxn.bcp_done() == 1

    stored = [tuple(r) for r in cur.execute("select * from t1")]
    assert stored[0] == (1, 2, 2**40, 1.5, 0.1, True, text, "abc", b"\x00\xff",
                         "00000000-0000-0000-0000-000000000001")
    assert stored[1] == (None,) * 10
    assert stored[2] == (-1, -2, -3, -0.5, 1e300, False, "", "xyz", b"",
                         "00000000-0000-0000-0000-000000000002")


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_low_level_api_manual_commit(bcp_cursor: pyodbc.Cursor):
    """In manual-commit mode a low-level session's rows join the open transaction."""
    bcp_cursor.execute("create table t1(id int)")
    cnxn = bcp_connect(autocommit=False)
    try:
        cnxn.bcp_init("t1")
        ctx = cnxn.bcp_bind_columns([pyodbc.SQLINT4])
        for i in (1, 2):
            cnxn.bcp_setrow(ctx, (i,))
            cnxn.bcp_sendrow()
        assert cnxn.bcp_done() == 2
        assert cnxn.execute("select count(*) from t1").fetchval() == 2
        cnxn.rollback()
        assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0
    finally:
        cnxn.close()


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_low_level_api_misuse(bcp_cursor: pyodbc.Cursor):
    """Misuse of the Connection.bcp_* methods raises instead of corrupting data or memory."""
    cur = bcp_cursor
    cur.execute("create table t1(c varchar(10), b bit)")
    cnxn = cur.connection
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_sendrow()
    with pytest.raises(ValueError):
        cnxn.bcp_init("t1", direction=2)

    cnxn.bcp_init("t1")
    with pytest.raises(ValueError):
        cnxn.bcp_bind_columns([12345])
    ctx = cnxn.bcp_bind_columns([pyodbc.SQLCHARACTER, pyodbc.SQLBIT])
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_bind_columns([pyodbc.SQLCHARACTER, pyodbc.SQLBIT])
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_sendrow()
    cnxn.bcp_setrow(ctx, ("z", False))
    with pytest.raises(ValueError):
        cnxn.bcp_setrow(ctx, ("é", True))
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_sendrow()
    with pytest.raises(ValueError):
        cnxn.bcp_setrow(ctx, ("a", 5))
    with pytest.raises(TypeError):
        cnxn.bcp_setrow(ctx, ("a",))
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.autocommit = False
    cnxn.bcp_setrow(ctx, ("a", True))
    del ctx
    gc.collect()
    cnxn.bcp_sendrow()
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_sendrow()
    assert cnxn.bcp_done() == 1
    with pytest.raises(pyodbc.ProgrammingError):
        cnxn.bcp_done()

    other = bcp_connect()
    other.bcp_init("t1")
    stale = other.bcp_bind_columns([pyodbc.SQLCHARACTER, pyodbc.SQLBIT])
    cnxn.bcp_init("t1")
    with pytest.raises(ValueError):
        cnxn.bcp_setrow(stale, ("x", True))
    cnxn.bcp_done()
    other.bcp_setrow(stale, ("b", False))
    other.bcp_sendrow()
    other.close()
    with pytest.raises(pyodbc.ProgrammingError):
        other.bcp_setrow(stale, ("c", True))
    cur.execute("set lock_timeout 5000")
    assert cur.execute("select count(*) from t1").fetchval() == 1


class _ClosesConnection:
    """A value whose conversion closes a connection, as a hostile __float__ might."""

    def __init__(self, cnxn: pyodbc.Connection) -> None:
        self.cnxn = cnxn

    def __float__(self) -> float:
        self.cnxn.close()
        return 1.0


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_conversion_that_closes_the_connection(bcp_cursor: pyodbc.Cursor):
    """
    A value whose conversion closes the connection mid-row raises ProgrammingError, in both the
    low-level API and the fast path, instead of using the connection's freed state.  So does
    one that closes the cursor holding the only reference to the connection.
    """
    bcp_cursor.execute("create table t1(f float, s nvarchar(10))")
    victim = bcp_connect()
    victim.bcp_init("t1")
    ctx = victim.bcp_bind_columns([pyodbc.SQLFLT8, pyodbc.SQLNCHAR])
    victim.bcp_setrow(ctx, (None, "first"))
    with pytest.raises(pyodbc.ProgrammingError):
        victim.bcp_setrow(ctx, (_ClosesConnection(victim), "second"))
    assert victim.closed

    class ClosingDecimal(Decimal):
        def normalize(self, context=None) -> Decimal:
            victim2.close()
            return super().normalize(context)

    bcp_cursor.execute("drop table t1")
    bcp_cursor.execute("create table t1(id int, d decimal(10, 2))")
    victim2 = bcp_connect()
    cur = victim2.cursor()
    with pytest.raises(pyodbc.ProgrammingError):
        _executemany(cur, "insert into t1 values (?, ?)", [(1, ClosingDecimal("1.5"))])
    assert victim2.closed
    assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0

    class ClosingUUID(uuid.UUID):
        @property
        def bytes_le(self) -> bytes:
            victim3.close()
            return super().bytes_le

    bcp_cursor.execute("drop table t1")
    bcp_cursor.execute("create table t1(id int, g uniqueidentifier)")
    victim3 = bcp_connect()
    with pytest.raises(pyodbc.ProgrammingError):
        _executemany(victim3.cursor(), "insert into t1 values (?, ?)",
                     [(1, ClosingUUID(int=7))])
    assert victim3.closed
    assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0

    class ClosingCursorDecimal(Decimal):
        def normalize(self, context=None) -> Decimal:
            victim4.close()
            gc.collect()
            return super().normalize(context)

    bcp_cursor.execute("drop table t1")
    bcp_cursor.execute("create table t1(id int, d decimal(10, 2))")
    victim4 = bcp_connect().cursor()
    with pytest.raises(pyodbc.ProgrammingError, match="cursor was closed"):
        _executemany(victim4, "insert into t1 values (?, ?)",
                     [(1, ClosingCursorDecimal("1.5"))])
    assert bcp_cursor.execute("select count(*) from t1").fetchval() == 0


@pytest.mark.skipif(not IS_BCP_DRIVER, reason=_BCP_SKIP)
def test_bcp_low_level_api_batch_rows(bcp_cursor: pyodbc.Cursor):
    """bcp_init's batch_rows ends a batch every batch_rows rows, and a failing batch raises."""
    cur = bcp_cursor
    cur.execute("create table t1(id int primary key)")
    cnxn = cur.connection
    cnxn.bcp_init("t1", batch_rows=2)
    ctx = cnxn.bcp_bind_columns([pyodbc.SQLINT4])
    with pytest.raises(pyodbc.IntegrityError):
        for i in (1, 2, 3, 3):
            cnxn.bcp_setrow(ctx, (i,))
            cnxn.bcp_sendrow()
    cnxn.bcp_done()
    assert [r[0] for r in cur.execute("select id from t1 order by id")] == [1, 2]


def _test_tvp(cursor: pyodbc.Cursor, diff_schema):
    # Test table value parameters (TVP).  I like the explanation here:
    #
    # https://www.mssqltips.com/sqlservertip/1483/using-table-valued-parameters-tvp-in-sql-server/
    #
    # "At a high level the TVP allows you to populate a table declared as a T-SQL variable,
    #  then pass that table as a parameter to a stored procedure or function."
    #
    # "The TVP must be declared READONLY.  You cannot perform any DML (i.e. INSERT, UPDATE,
    #  DELETE) against the TVP; you can only reference it in a SELECT statement."
    #
    # In this test we'll create a table, pass it to a stored procedure, and have the stored
    # procedure simply return the rows from the TVP.
    #
    # Apparently the way pyodbc knows something is a TVP is because it is in a sequence.  I'm
    # not sure I like that as it is very generic and specific to SQL Server.  It would be wiser
    # to define a wrapper pyodbc.TVP or pyodbc.Table object, similar to the DB APIs `Binary`
    # object.

    procname = 'SelectTVP'
    typename = 'TestTVP'

    if diff_schema:
        schemaname = 'myschema'
        procname = schemaname + '.' + procname
        typenameonly = typename
        typename = schemaname + '.' + typename

    # (Don't use "if exists" since older SQL Servers don't support it.)
    try:
        cursor.execute("drop procedure " + procname)
    except Exception:
        pass
    try:
        cursor.execute("drop type " + typename)
    except Exception:
        pass
    if diff_schema:
        try:
            cursor.execute("drop schema " + schemaname)
        except Exception:
            pass
    cursor.commit()

    if diff_schema:
        cursor.execute("CREATE SCHEMA myschema")
        cursor.commit()

    cursor.execute(
        f"""
        CREATE TYPE {typename} AS TABLE(
                c01 VARCHAR(255),
                c02 VARCHAR(MAX),
                c03 VARBINARY(255),
                c04 VARBINARY(MAX),
                c05 BIT,
                c06 DATE,
                c07 TIME,
                c08 DATETIME2(5),
                c09 BIGINT,
                c10 FLOAT,
                c11 NUMERIC(38, 24),
                c12 UNIQUEIDENTIFIER)
        """)
    cursor.commit()
    cursor.execute(
        f"""
        CREATE PROCEDURE {procname} @TVP {typename} READONLY
          AS SELECT * FROM @TVP;
        """)
    cursor.commit()

    # The values aren't exactly VERY_LONG_LEN but close enough and *significantly* faster than
    # the loop we had before.
    VERY_LONG_LEN = 2000000
    long_string         = ''.join(chr(i) for i in range(32, 127))  # printable characters
    long_bytearray      = bytes(list(range(255)))
    very_long_string    = long_string * (VERY_LONG_LEN // len(long_string))
    very_long_bytearray = long_bytearray * (VERY_LONG_LEN // len(long_bytearray))

    params = [
        # Four rows with all of the types in the table defined above.
        (None, None, None, None, None, None, None, None, None, None, None, None),
        (
            'abc', 'abc',
            bytes([0xD1, 0xCE, 0xFA, 0xCE]),
            bytes([0x0F, 0xF1, 0xCE, 0xCA, 0xFE]), True,
            date(1997, 8, 29), time(9, 13, 39),
            datetime(2018, 11, 13, 13, 33, 26, 298420),
            1234567, 3.14, Decimal('31234567890123.141243449787580175325274'),
            uuid.UUID('4fe34a93-e574-04cc-200a-353f0d1770b1'),
        ),
        (
            '', '',
            bytes([0x00, 0x01, 0x02, 0x03, 0x04]),
            bytes([0x00, 0x01, 0x02, 0x03, 0x04, 0x05]), False,
            date(1, 1, 1), time(0, 0, 0),
            datetime(1, 1, 1, 0, 0, 0, 0),
            -9223372036854775808, -1.79E+308, Decimal('0.000000000000000000000001'),
            uuid.UUID('33f7504c-2bac-1b83-01d1-7434a7ba6a17'),
        ),
        (
            long_string, very_long_string,
            bytes(long_bytearray), bytes(very_long_bytearray), True,
            date(9999, 12, 31), time(23, 59, 59),
            datetime(9999, 12, 31, 23, 59, 59, 999990),
            9223372036854775807, 1.79E+308, Decimal('99999999999999.999999999999999999999999'),
            uuid.UUID('ffffffff-ffff-ffff-ffff-ffffffffffff'),
        )
    ]

    if diff_schema:
        p1 = [[typenameonly, schemaname] + params]
    else:
        p1 = [params]

    saved_native_uuid = pyodbc.native_uuid
    try:
        pyodbc.native_uuid = True
        result_array = [tuple(row) for row in cursor.execute(f"exec {procname} ?", p1).fetchall()]
    finally:
        pyodbc.native_uuid = saved_native_uuid

    # The values make it very difficult to troubleshoot if something is wrong, so instead of
    # asserting they are the same, we'll walk them if there is a problem to identify which is
    # wrong.
    for row, param in zip(result_array, params):
        if row != param:
            for r, p in zip(row, param):
                assert r == p

    # Now test with zero rows.

    params = []
    p1 = [params]
    if diff_schema:
        p1 = [[typenameonly, schemaname] + params]
    else:
        p1 = [params]
    result_array = cursor.execute(f"exec {procname} ?", p1).fetchall()
    assert result_array == params


@pytest.mark.skipif(IS_FREETDS, reason='FreeTDS does not support TVP')
def test_tvp(cursor: pyodbc.Cursor):
    _test_tvp(cursor, False)


@pytest.mark.skipif(IS_FREETDS, reason='FreeTDS does not support TVP')
def test_tvp_diffschema(cursor: pyodbc.Cursor):
    _test_tvp(cursor, True)


def _test_scanning_all_tvp_rows(cursor: pyodbc.Cursor, data):
    # Make sure we check all the rows of the TVP before binding.
    # Splitting into multiple tests to prevent one failure from
    # masking other problems.
    procname = "SelectFromScannedTVP"
    typename = "TestTVPForScanning"
    try:
        cursor.execute(f"DROP PROCEDURE {procname}")
    except pyodbc.ProgrammingError:
        pass
    try:
        cursor.execute(f"DROP TYPE {typename}")
    except pyodbc.ProgrammingError:
        pass
    cursor.execute(f"CREATE TYPE {typename} AS TABLE(val DECIMAL(20,4))")
    cursor.execute(f"""\
        CREATE PROCEDURE {procname}
            @TVP {typename} READONLY
        AS
        BEGIN
            SET NOCOUNT ON;
            SELECT * FROM @TVP;
        END
        """)
    cursor.commit()
    cursor.execute(f"EXEC {procname} ?", [data])
    results = [list(row) for row in cursor.fetchall()]
    assert results == data
    cursor.execute(f"DROP PROCEDURE {procname}")
    cursor.execute(f"DROP TYPE {typename}")
    cursor.commit()


@pytest.mark.skipif(SQLSERVER_YEAR < 2008, reason="TVP not supported until 2008")
@pytest.mark.skipif(IS_FREETDS, reason='FreeTDS does not support TVP')
def test_tvp_decimal_mixed_precision(cursor: pyodbc.Cursor):
    """Test for https://github.com/mkleehammer/pyodbc/issues/996."""
    _test_scanning_all_tvp_rows(cursor, [[Decimal("4.0000")], [Decimal("25.000")]])


@pytest.mark.skipif(SQLSERVER_YEAR < 2008, reason="TVP not supported until 2008")
@pytest.mark.skipif(IS_FREETDS, reason='FreeTDS does not support TVP')
def test_tvp_decimal_mixed_scale(cursor: pyodbc.Cursor):
    """Test the different number decimal digits, but same number of integer digits."""
    _test_scanning_all_tvp_rows(cursor, [[Decimal("4.000")], [Decimal("4.0000")]])


@pytest.mark.skipif(SQLSERVER_YEAR < 2008, reason="TVP not supported until 2008")
@pytest.mark.skipif(IS_FREETDS, reason='FreeTDS does not support TVP')
def test_tvp_decimal_mixed_shape(cursor: pyodbc.Cursor):
    """Test same number of digits, shifting decimal point.

    See the lengthy comment in the code for BindTVPColumns().
    """
    _test_scanning_all_tvp_rows(cursor, [[Decimal("4.0000")], [Decimal("40.000")]])
    _test_scanning_all_tvp_rows(cursor, [[Decimal("40.000")], [Decimal("4.0000")]])


def _test_tvp_with_nulls_cleanup(cursor: pyodbc.Cursor, procname: str, typename: str):
    """Leave the forest as pristine as you found it."""

    cursor.execute(f"""\
        IF OBJECT_ID(N'dbo.{procname}', N'P') IS NOT NULL
        DROP PROCEDURE dbo.{procname};
    """)
    cursor.execute(f"""
        IF TYPE_ID(N'dbo.{typename}') IS NOT NULL
            DROP TYPE dbo.{typename};
    """)


@pytest.mark.skipif(SQLSERVER_YEAR < 2008, reason="TVP not supported until 2008")
@pytest.mark.skipif(IS_FREETDS, reason="FreeTDS does not support TVP")
def test_tvp_with_nulls(cursor: pyodbc.Cursor):
    """Make sure NULL values in a TVP don't crash the interpreter."""

    # Start with a clean slate.
    typename = "typeTestNullsInTVP"
    procname = "spTestNullsInTVP"
    _test_tvp_with_nulls_cleanup(cursor, procname, typename)

    # Create the custom type and stored procedure.
    ncols = 100
    cols = ", ".join([f"col_{c:03d} DECIMAL(36,20)" for c in range(1, ncols+1)])
    cursor.execute(f"CREATE TYPE dbo.{typename} AS TABLE ({cols})")
    cursor.execute(f"""\
        CREATE PROCEDURE dbo.{procname}
            @data dbo.{typename} READONLY
        AS
        BEGIN
            RETURN 0;
        END;
    """)
    cursor.commit()

    # Invoke the stored procedure.
    tvp: list[list] = [[3.14159] * ncols, [None] * ncols]
    cursor.execute(f"EXEC [dbo].{procname} @data=?", [tvp])
    gc.collect()

    # Be a good digital citizen.
    _test_tvp_with_nulls_cleanup(cursor, procname, typename)
    cursor.commit()


@pytest.mark.skipif(SQLSERVER_YEAR < 2000, reason='sql_variant not supported until 2000')
def test_sql_variant(cursor: pyodbc.Cursor):
    """
    Tests decoding of the sql_variant data type as performed by the GetData_SqlVariant() method.
    """

    cursor.execute("create table t1 (a sql_variant)")

    # insert a number of values of disparate types. this is not exhaustive as not all
    # types that can be contained within a sql_variant field are supported by pyodbc
    cursor.execute("insert into t1 values (456.7)")
    cursor.execute("insert into t1 values ('a string')")
    cursor.execute("insert into t1 values (CAST('2024-06-03' AS DATE))")
    cursor.execute("insert into t1 values (CAST('2024-06-03 23:46:03.000' AS DATETIME))")
    cursor.execute("insert into t1 values (CAST('binary data' AS VARBINARY(200)))")
    cursor.execute(
        "insert into t1 values (CAST('0592b437-745f-4b2c-a997-97022c624cf6' AS UNIQUEIDENTIFIER))"
    )

    # Expected behavior depends on this flag being set.
    saved_native_uuid = pyodbc.native_uuid
    try:
        pyodbc.native_uuid = True
        results = [record[0] for record in cursor.execute("select a from t1").fetchall()]
    finally:
        pyodbc.native_uuid = saved_native_uuid

    # Ensure all of the fetched values have the expected types.
    for index, assertion_tuple in enumerate(
        [
            (Decimal, Decimal("456.7")),
            (str, "a string"),
            (date, date(2024, 6, 3)),
            (datetime, datetime(2024, 6, 3, 23, 46, 3)),
            (bytes, b'binary data'),
            (uuid.UUID, uuid.UUID("0592b437-745f-4b2c-a997-97022c624cf6"))
        ]
    ):
        # pylint: disable=unidiomatic-typecheck
        expected_type, expected_value = assertion_tuple

        assert type(results[index]) is expected_type
        assert results[index] == expected_value


def test_rows_as_dicts(cursor: pyodbc.Cursor):
    """Test enhancement for ticket #171"""

    # Create and populate a test table.
    cursor.execute("create table t1 (id int, name varchar(20))")
    cursor.execute("insert into t1 values (42, 'Kathleen')")

    # Verify the default behavior
    assert cursor.rows_as_dicts is False
    row = cursor.execute("select * from t1").fetchone()
    assert not isinstance(row, dict)
    assert isinstance(row, pyodbc.Row)
    assert isinstance(row[0], int)
    assert isinstance(row[1], str)
    assert len(row) == 2
    with pytest.raises(TypeError, match="row indices must be integers"):
        print(row["name"])

    # Test the dict option
    cursor.rows_as_dicts = True
    row = cursor.execute("select * from t1").fetchone()
    assert not isinstance(row, pyodbc.Row)
    assert isinstance(row, dict)
    assert row == {"id": 42, "name": "Kathleen"}
    assert isinstance(row["id"], int)
    assert isinstance(row["name"], str)
    assert len(row) == 2
    with pytest.raises(KeyError):
        print(row[1])

    # Test aliasing
    row = cursor.execute("select name as n1, name as n2 from t1").fetchone()
    assert len(row) == 2
    assert row == {"n1": "Kathleen", "n2": "Kathleen"}
    with pytest.raises(KeyError):
        print(row["name"])

    # Test with a duplicate name
    row = cursor.execute("select name, name from t1").fetchone()
    assert len(row) == 1
    assert row == {"name": "Kathleen"}


def test_handles(cursor: pyodbc.Cursor):
    """Test the exposed native ODBC handles"""

    conn = cursor.connection
    for handle in (pyodbc.henv, conn.hdbc, cursor.hstmt):
        assert isinstance(handle, ctypes.c_void_p)
        with pytest.raises(TypeError):
            if handle > 42:
                print("we should never get here")
    cursor.close()
    assert not isinstance(cursor.hstmt, ctypes.c_void_p)
    assert cursor.hstmt is None
    assert isinstance(conn.hdbc, ctypes.c_void_p)
    conn.close()
    assert not isinstance(conn.hdbc, ctypes.c_void_p)
    assert conn.hdbc is None


def get_sqlserver_version(cursor: pyodbc.Cursor):

    """
    Returns the major version: 8-->2000, 9-->2005, 10-->2008
    """
    cursor.execute("exec master..xp_msver 'ProductVersion'")
    row = cursor.fetchone()
    return int(row.Character_Value.split('.', 1)[0])


@lru_cache
def _generate_str(length, encoding=None):
    """
    Returns either a string or bytes, depending on whether encoding is provided,
    that is `length` elements long.

    If length is None, None is returned.  This simplifies the tests by letting us put None into
    an array of other lengths and pass them here, moving the special case check into one place.
    """
    if length is None:
        return None

    # Put non-ASCII characters at the front so we don't end up chopping one in half in a
    # multi-byte encoding like UTF-8.

    v = 'á'

    remaining = max(0, length - len(v))
    if remaining:
        seed = '0123456789-abcdefghijklmnopqrstuvwxyz-'

        if remaining <= len(seed):
            v += seed
        else:
            c = (remaining + len(seed) - 1 // len(seed))
            v += seed * c

    if encoding:
        v = v.encode(encoding)

    # We chop *after* encoding because if we are encoding then we want bytes.
    v = v[:length]

    return v


def test_set_string_attr(cursor: pyodbc.Cursor):
    """Confirm that set_attr() now accepts string values.

    See https://github.com/mkleehammer/pyodbc/issues/505
    """
    original_db = cursor.execute("SELECT db_name()").fetchval()
    assert original_db != "master"
    cursor.connection.set_attr(pyodbc.SQL_ATTR_CURRENT_CATALOG, "master")
    new_db = cursor.execute("SELECT db_name()").fetchval()
    assert new_db == "master"
