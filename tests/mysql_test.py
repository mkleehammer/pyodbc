"""
pytest unit tests for MySQL.  Uses a DNS name 'mysql' and uses UTF-8
"""
# -*- coding: utf-8 -*-

import os
from decimal import Decimal
from datetime import date, datetime
from functools import lru_cache
from typing import Iterator

import pyodbc, pytest


CNXNSTR = os.environ.get('PYODBC_MYSQL', 'DSN=mysql;charset=utf8mb4')


def connect(autocommit=False, attrs_before=None):
    c = pyodbc.connect(CNXNSTR, autocommit=autocommit, attrs_before=attrs_before)

    # As of libmyodbc5w 5.3 SQLGetTypeInfo returns absurdly small sizes
    # leading to slow writes.  Override them:
    c.maxwrite = 1024 * 1024 * 1024

    # My MySQL configuration (and I think the default) sends *everything*
    # in UTF-8.  The pyodbc default is to send Unicode as UTF-16 and to
    # decode WCHAR via UTF-16.  Change them both to UTF-8.
    c.setdecoding(pyodbc.SQL_CHAR, encoding='utf-8')
    c.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
    c.setencoding(encoding='utf-8')

    return c


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


def test_varbinary(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'varbinary')


def test_blob(cursor: pyodbc.Cursor):
    _test_vartype(cursor, 'blob')


def _test_vartype(cursor, datatype):
    cursor.execute(f"create table t1(c1 {datatype}(4000))")

    for length in [None, 0, 100, 1000, 4000]:
        cursor.execute("delete from t1")

        encoding = (datatype in ('blob', 'varbinary')) and 'utf8' or None
        value = _generate_str(length, encoding=encoding)

        cursor.execute("insert into t1 values(?)", value)
        v = cursor.execute("select * from t1").fetchone()[0]
        assert v == value


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


def test_float(cursor: pyodbc.Cursor):
    _test_scalar(cursor, 'float', [None, -1, 0, 1, 1234.5, -200])


def _test_scalar(cursor: pyodbc.Cursor, datatype, values):
    cursor.execute(f"create table t1(c1 {datatype})")
    for value in values:
        cursor.execute("delete from t1")
        cursor.execute("insert into t1 values (?)", value)
        v = cursor.execute("select c1 from t1").fetchone()[0]
        assert v == value


def test_decimal(cursor: pyodbc.Cursor):
    tests = [
        ('100010', '19'),  # The ODBC docs tell us how the bytes should look in the C struct
        ('1000.10', '20,6'),
        ('-10.0010', '19,4')
    ]

    for value, prec in tests:
        value = Decimal(value)
        cursor.execute("drop table if exists t1")
        cursor.execute(f"create table t1(c1 numeric({prec}))")
        cursor.execute("insert into t1 values (?)", value)
        v = cursor.execute("select c1 from t1").fetchone()[0]
        assert v == value


def test_multiple_bindings(cursor: pyodbc.Cursor):
    "More than one bind and select on a cursor"
    cursor.execute("create table t1(n int)")
    cursor.execute("insert into t1 values (?)", 1)
    cursor.execute("insert into t1 values (?)", 2)
    cursor.execute("insert into t1 values (?)", 3)
    for i in range(3):
        cursor.execute("select n from t1 where n < ?", 10)
        cursor.execute("select n from t1 where n < 3")


def test_different_bindings(cursor: pyodbc.Cursor):
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


def test_negative_row_index(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "1")
    row = cursor.execute("select * from t1").fetchone()
    assert row[0] == "1"
    assert row[-1] == "1"


def test_version():
    assert 3 == len(pyodbc.version.split('.'))  # 1.3.1 etc.


def test_date(cursor: pyodbc.Cursor):
    value = date(2001, 1, 1)

    cursor.execute("create table t1(dt date)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert type(result) == type(value)
    assert result == value


def test_time(cursor: pyodbc.Cursor):
    value = datetime.now().time()

    # We aren't yet writing values using the new extended time type so the value written to the
    # database is only down to the second.
    value = value.replace(microsecond=0)

    cursor.execute("create table t1(t time)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select t from t1").fetchone()[0]
    assert value == result


def test_datetime(cursor: pyodbc.Cursor):
    value = datetime(2007, 1, 15, 3, 4, 5)

    cursor.execute("create table t1(dt datetime)")
    cursor.execute("insert into t1 values (?)", value)

    result = cursor.execute("select dt from t1").fetchone()[0]
    assert value == result


def test_rowcount_delete(cursor: pyodbc.Cursor):
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

    pyodbc calls SQLRowCount after each execute and sets Cursor.rowcount.  Databases can return
    the actual rowcount or they can return -1 if it would help performance.  MySQL seems to
    always return the correct rowcount.
    """
    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    cursor.execute("select * from t1")
    assert cursor.rowcount == count

    rows = cursor.fetchall()
    assert len(rows) == count
    assert cursor.rowcount == count


def test_rowcount_reset(cursor: pyodbc.Cursor):
    "Ensure rowcount is reset to -1"

    # The Python DB API says that rowcount should be set to -1 and most ODBC drivers let us
    # know there are no records.  MySQL always returns 0, however.  Without parsing the SQL
    # (which we are not going to do), I'm not sure how we can tell the difference and set the
    # value to -1.  For now, I'll have this test check for 0.

    cursor.execute("create table t1(i int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    assert cursor.rowcount == 1

    cursor.execute("create table t2(i int)")
    assert cursor.rowcount == 0


def test_lower_case():
    "Ensure pyodbc.lowercase forces returned column names to lowercase."

    # Has to be set before creating the cursor
    cnxn = connect()
    pyodbc.lowercase = True
    cursor = cnxn.cursor()

    cursor.execute("drop table if exists t1")

    cursor.execute("create table t1(Abc int, dEf int)")
    cursor.execute("select * from t1")

    names = [t[0] for t in cursor.description]
    names.sort()

    assert names == ["abc", "def"]

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
    cursor.execute("create table t1(a int, b int, c int, d int)")
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = str(row)
    assert result == "(1, 2, 3, 4)"

    result = str(row[:-1])
    assert result == "(1, 2, 3)"

    result = str(row[:1])
    assert result == "(1,)"


def test_emoticons_as_parameter(cursor: pyodbc.Cursor):
    # https://github.com/mkleehammer/pyodbc/issues/423
    #
    # When sending a varchar parameter, pyodbc is supposed to set ColumnSize to the number
    # of characters.  Ensure it works even with 4-byte characters.
    #
    # http://www.fileformat.info/info/unicode/char/1f31c/index.htm

    v = "x \U0001F31C z"

    cursor.execute("CREATE TABLE t1(s varchar(100)) DEFAULT CHARSET=utf8mb4")
    cursor.execute("insert into t1 values (?)", v)

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v


def test_emoticons_as_literal(cursor: pyodbc.Cursor):
    # https://github.com/mkleehammer/pyodbc/issues/630

    v = "x \U0001F31C z"

    cursor.execute("CREATE TABLE t1(s varchar(100)) DEFAULT CHARSET=utf8mb4")
    cursor.execute("insert into t1 values ('%s')" % v)

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v


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
