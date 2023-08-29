#!/usr/bin/python
# -*- coding: utf-8 -*-

import os, uuid, re, sys
from decimal import Decimal
from datetime import date, time, datetime
from functools import lru_cache
from typing import Iterator

import pyodbc, pytest


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

IS_FREEDTS   = bool(re.search('tsodbc', DRIVER, flags=re.IGNORECASE))
IS_MSODBCSQL = bool(re.search(r'(msodbcsql|sqlncli|sqlsrv32\.dll)', DRIVER, re.IGNORECASE))


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
        '13': 2016, '14': 2017, '15': 2019, '16': 2022
    }[major]


SQLSERVER_YEAR = _get_sqlserver_year()


@pytest.fixture()
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
    input = 9999999999999999999999999999999999999
    cursor.execute("create table t1(d bigint)")
    with pytest.raises(OverflowError):
        cursor.execute("insert into t1 values (?)", input)
    result = cursor.execute("select * from t1").fetchall()
    assert result == []


def test_float(cursor: pyodbc.Cursor):
    _test_scalar(cursor, 'float', [None, -200, -1, 0, 1, 1234.5, -200, .00012345])


def test_non_numeric_float(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(d float)")
    for input in (float('+Infinity'), float('-Infinity'), float('NaN')):
        with pytest.raises(pyodbc.ProgrammingError):
            cursor.execute("insert into t1 values (?)", input)


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

    if datatype == 'text':
        lengths = LARGE_FENCEPOST_SIZES
    else:
        lengths = SMALL_FENCEPOST_SIZES

    if datatype == 'text':
        cursor.execute(f"create table t1(c1 {datatype})")
    else:
        maxlen = lengths[-1]
        cursor.execute(f"create table t1(c1 {datatype}({maxlen}))")

    for length in lengths:
        cursor.execute("delete from t1")

        encoding = (datatype in ('blob', 'varbinary')) and 'utf8' or None
        value = _generate_str(length, encoding=encoding)

        try:
            cursor.execute("insert into t1 values(?)", value)
        except pyodbc.Error as ex:
            msg = f'{datatype} insert failed: length={length} len={len(value)}'
            raise Exception(msg) from ex

        v = cursor.execute("select * from t1").fetchone()[0]
        assert v == value


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
    # The default is False meaning we should return a string.  Note that
    # SQL Server seems to always return uppercase.
    value = uuid.uuid4()
    cursor.execute("create table t1(n uniqueidentifier)")
    cursor.execute("insert into t1 values (?)", value)

    pyodbc.native_uuid = False
    result = cursor.execute("select n from t1").fetchval()
    assert isinstance(result, str)
    assert result == str(value).upper()
    pyodbc.native_uuid = True


def test_native_uuid(cursor: pyodbc.Cursor):
    # When true, we should return a uuid.UUID object.
    value = uuid.uuid4()
    cursor.execute("create table t1(n uniqueidentifier)")
    cursor.execute("insert into t1 values (?)", value)

    pyodbc.native_uuid = True
    result = cursor.execute("select n from t1").fetchval()
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


@pytest.mark.skipif(IS_FREEDTS, reason='https://github.com/FreeTDS/freetds/issues/230')
def test_nextset_with_raiserror(cursor: pyodbc.Cursor):
    cursor.execute("select i = 1; RAISERROR('c', 16, 1);")
    row = next(cursor)
    assert 1 == row.i
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

    for (precision, scale, negative) in [
            (1, 0, False), (1, 0, True), (6, 0, False), (6, 2, False), (6, 4, True),
            (6, 6, True), (38, 0, False), (38, 10, False), (38, 38, False), (38, 0, True),
            (38, 10, True), (38, 38, True)]:

        try:
            cursor.execute("drop table t1")
        except:
            pass

        cursor.execute(f"create table t1(d decimal({precision}, {scale}))")

        # Construct a decimal that uses the maximum precision and scale.
        sign   = negative and '-' or ''
        before = '9' * (precision - scale)
        after  = scale and ('.' + '9' * scale) or ''
        decStr = f'{sign}{before}{after}'
        value = Decimal(decStr)

        cursor.execute("insert into t1 values(?)", value)

        v = cursor.execute("select d from t1").fetchone()[0]
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
    assert 3 == len(pyodbc.version.split('.'))  # 1.3.1 etc.


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
    ddl_rowcount = (0 if IS_FREEDTS else -1)
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
    except:
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
    assert t[1] == int
    assert t[5] == 0       # scale
    assert t[6] is True    # nullable

    # varchar(8)
    t = cursor.description[1]
    assert t[0] == 's'
    assert t[1] == str
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

    # SQL Server PRINT statements are never more than 8000 characters
    # https://docs.microsoft.com/en-us/sql/t-sql/language-elements/print-transact-sql#remarks
    for msg in ('hello world', 'ABCDEFGHIJ' * 800):
        cursor.execute(f"PRINT '{msg}'")
        messages = cursor.messages
        assert isinstance(messages, list)
        assert len(messages) == 1
        assert isinstance(messages[0], tuple)
        assert len(messages[0]) == 2
        assert isinstance(messages[0][0], str)
        assert isinstance(messages[0][1], str)
        assert '[01000] (0)' == messages[0][0]
        assert messages[0][1].endswith(msg)


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
        if IS_FREEDTS:
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
    except:
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

    pyodbc.native_uuid = True
    # This is the default, but we'll reset it in case a previous test fails to.

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
    except:
        pass
    try:
        cursor.execute("drop type " + typename)
    except:
        pass
    if diff_schema:
        try:
            cursor.execute("drop schema " + schemaname)
        except:
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
        # Three rows with all of the types in the table defined above.
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
    result_array = [tuple(row) for row in cursor.execute(f"exec {procname} ?", p1).fetchall()]

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


@pytest.mark.skipif(IS_FREEDTS, reason='FreeTDS does not support TVP')
def test_tvp(cursor: pyodbc.Cursor):
    _test_tvp(cursor, False)


@pytest.mark.skipif(IS_FREEDTS, reason='FreeTDS does not support TVP')
def test_tvp_diffschema(cursor: pyodbc.Cursor):
    _test_tvp(cursor, True)


def get_sqlserver_version(cursor: pyodbc.Cursor):

    """
    Returns the major version: 8-->2000, 9-->2005, 10-->2008
    """
    cursor.execute("exec master..xp_msver 'ProductVersion'")
    row = cursor.fetchone()
    return int(row.Character_Value.split('.', 1)[0])


@lru_cache()
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
