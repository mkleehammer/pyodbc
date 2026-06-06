"""Unit tests for Apache Spark

These tests use Simba Spark ODBC driver.
The DSN should be configured with UseNativeQuery=0 to pass the tests.

The port to pytest has not yet been tested (don't have access to the driver).
"""

import decimal
import os
import sys
import typing
import uuid

import pyodbc
import pytest

CNXNSTR = os.environ.get("PYODBC_SPARK", "DSN=pyodbc-spark")
INTEGERS = [-1, 0, 1, 0x7FFFFFFF]
BIGINTS = INTEGERS + [0xFFFFFFFF, 0x123456789]
FIXED = [decimal.Decimal(v) for v in "-1234.56 -1 0 1 1234.56 123456789.21".split()]
SMALL_READ = 100
LARGE_READ = 4000


#----------------------------------------------------------------------
# Helper functions
#----------------------------------------------------------------------

def connect(autocommit=False, attrs_before=None):
    """Create a connection to the Spark back end"""
    return pyodbc.connect(CNXNSTR, autocommit=autocommit, attrs_before=attrs_before)


@pytest.fixture
def cursor() -> typing.Iterator[pyodbc.Cursor]:
    """Cursor object supplied on demand to test functions"""

    cnxn = connect()
    cur = cnxn.cursor()

    cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding="utf-8")
    cnxn.setencoding(encoding="utf-8")
    cnxn.maxwrite = 1024 * 1024 * 1024

    for i in range(1, 4):
        try:
            cur.execute(f"drop table t{i:d}")
        except pyodbc.ProgrammingError:
            pass
    cnxn.commit()

    yield cur

    if not cnxn.closed:
        cur.close()
        cnxn.close()


def _generate_test_string(length, encoding=None):
    """
    Returns either a string or bytes, depending on whether encoding is provided,
    that is `length` elements long.

    If length is None, None is returned.  This simplifies the tests by letting us put None into
    an array of other lengths and pass them here, moving the special case check into one place.
    """

    if length is None:
        return None

    seed = "0123456789-abcdefghijklmnopqrstuvwxyz-"

    if length <= len(seed):
        v = seed
    else:
        c = (length + len(seed) - 1 // len(seed))
        v = seed * c

    v = v[:length]
    if encoding:
        v = v.encode(encoding)

    return v


def _simpletest(cursor, datatype, value):
    """
    A simple test that can be used for any data type where the Python
    type we write is also what we expect to receive.
    """
    cursor.execute(f"create table t1 (value {datatype})")
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select value from t1").fetchval()
    assert result == value
    cursor.execute("drop table t1")


def _test_strtype(cursor, sqltype, value, colsize=None, resulttype=None):
    """
    The implementation for string and binary tests.
    """
    assert colsize is None or value is None or colsize >= len(value)

    sqltype = f"{sqltype}({colsize})" if colsize else sqltype

    cursor.execute(f"create table t1 (v {sqltype})")
    cursor.execute("insert into t1 values(?)", value)

    result = cursor.execute("select * from t1").fetchone()[0]

    if resulttype and type(value) is not resulttype:
        value = resulttype(value)

    assert result == value


def test_drivers():
    p = pyodbc.drivers()
    assert isinstance(p, list)


def test_datasources(cursor: pyodbc.Cursor):
    p = pyodbc.dataSources()
    assert isinstance(p, dict)


# def test_gettypeinfo(cursor: pyodbc.Cursor):
#     cursor.getTypeInfo(pyodbc.SQL_VARCHAR)
#     cols = [t[0] for t in cursor.description]
#     print("cols:", cols)
#     for row in cursor:
#         for col,val in zip(cols, row):
#             print(" ", col, val)


def test_getinfo_string(cursor: pyodbc.Cursor):
    value = cursor.connection.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
    assert isinstance(value, str)


def test_getinfo_bool(cursor: pyodbc.Cursor):
    value = cursor.connection.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
    assert isinstance(value, bool)


def test_getinfo_int(cursor: pyodbc.Cursor):
    value = cursor.connection.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
    assert isinstance(value, int)


def test_getinfo_smallint(cursor: pyodbc.Cursor):
    value = cursor.connection.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
    assert isinstance(value, int)


def test_negative_float(cursor: pyodbc.Cursor):
    value = -200
    cursor.execute("create table t1(n float)")
    cursor.execute("insert into t1 values (?)", value)
    result  = cursor.execute("select n from t1").fetchone()[0]
    assert value == result

#
# VARCHAR
#

def test_empty_varchar(cursor: pyodbc.Cursor):
    _test_strtype(cursor, "varchar", "", SMALL_READ)


def test_null_varchar(cursor: pyodbc.Cursor):
    _test_strtype(cursor, "varchar", None, SMALL_READ)


def test_large_null_varchar(cursor: pyodbc.Cursor):
    # There should not be a difference, but why not find out?
    _test_strtype(cursor, "varchar", None, LARGE_READ)


def test_small_varchar(cursor: pyodbc.Cursor):
    _test_strtype(cursor, "varchar", _generate_test_string(SMALL_READ), SMALL_READ)


def test_large_varchar(cursor: pyodbc.Cursor):
    _test_strtype(cursor, "varchar", _generate_test_string(LARGE_READ), LARGE_READ)


def test_varchar_many(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(c1 varchar(300), c2 varchar(300), c3 varchar(300))")

    v1 = "ABCDEFGHIJ" * 30
    v2 = "0123456789" * 30
    v3 = "9876543210" * 30

    cursor.execute("insert into t1(c1, c2, c3) values (?,?,?)", v1, v2, v3)
    row = cursor.execute("select c1, c2, c3 from t1").fetchone()

    assert v1 == row.c1
    assert v2 == row.c2
    assert v3 == row.c3


def test_chinese(cursor: pyodbc.Cursor):
    v = "我的"
    cursor.execute("SELECT '我的' AS name")
    row = cursor.fetchone()
    assert row[0] == v

    cursor.execute("SELECT '我的' AS name")
    rows = cursor.fetchall()
    assert rows[0][0] == v


#
# NUMBERS
#

def test_int(cursor: pyodbc.Cursor):
    for value in INTEGERS:
        _simpletest(cursor, "int", value)


def test_bigint(cursor: pyodbc.Cursor):
    for value in BIGINTS:
        _simpletest(cursor, "bigint", value)


def test_decimal(cursor: pyodbc.Cursor):
    for value in FIXED:
        _simpletest(cursor, "decimal(20,6)", value)


def test_numeric(cursor: pyodbc.Cursor):
    for value in FIXED:
        _simpletest(cursor, "numeric(20,6)", value)


def test_small_decimal(cursor: pyodbc.Cursor):
    value = decimal.Decimal("100010")       # (I use this because the ODBC docs tell us how the bytes should look in the C struct)
    cursor.execute("create table t1(d numeric(19))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == decimal.Decimal
    assert v == value


def test_small_decimal_scale(cursor: pyodbc.Cursor):
    # The same as small_decimal, except with a different scale.  This value exactly matches the ODBC documentation
    # example in the C Data Types appendix.
    value = "1000.10"
    value = decimal.Decimal(value)
    cursor.execute("create table t1(d numeric(20,6))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == decimal.Decimal
    assert v == value


def test_negative_decimal_scale(cursor: pyodbc.Cursor):
    value = decimal.Decimal("-10.0010")
    cursor.execute("create table t1(d numeric(19,4))")
    cursor.execute("insert into t1 values(?)", value)
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == decimal.Decimal
    assert v == value


def test_close_cnxn(cursor: pyodbc.Cursor):
    """Make sure using a Cursor after closing its connection doesn't crash."""

    cursor.execute("create table t1(id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, "test")
    cursor.execute("select * from t1")

    cursor.connection.close()

    # Now that the connection is closed, we expect an exception.  (If the code attempts to use
    # the HSTMT, we'll get an access violation instead.)
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("select * from t1")


def test_empty_string(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "")


def test_fixed_str(cursor: pyodbc.Cursor):
    value = "testing"
    cursor.execute("create table t1(s char(7))")
    cursor.execute("insert into t1 values(?)", "testing")
    v = cursor.execute("select * from t1").fetchone()[0]
    assert type(v) == str
    assert len(v) == len(value) # If we alloc'd wrong, the test below might work because of an embedded NULL
    assert v == value


def test_fetchval(cursor: pyodbc.Cursor):
    expected = "test"
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", expected)
    result = cursor.execute("select * from t1").fetchval()
    assert result == expected


def test_negative_row_index(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(s varchar(20))")
    cursor.execute("insert into t1 values(?)", "1")
    row = cursor.execute("select * from t1").fetchone()
    assert row[0] == "1"
    assert row[-1] == "1"


def test_version(cursor: pyodbc.Cursor):
    assert 3 == len(pyodbc.version.split(".")) # 1.3.1 etc.


def test_lower_case(cursor: pyodbc.Cursor):
    "Ensure pyodbc.lowercase forces returned column names to lowercase."

    # Has to be set before creating the cursor, so we must recreate cursor.

    pyodbc.lowercase = True
    cursor = cursor.connection.cursor()

    cursor.execute("create table t1(Abc int, dEf int)")
    cursor.execute("select * from t1")

    names = [ t[0] for t in cursor.description ]
    names.sort()

    assert names == [ "abc", "def" ]

    # Put it back so other tests don't fail.
    pyodbc.lowercase = False


def test_long_column_name(cursor: pyodbc.Cursor):
    "ensure super long column names are handled correctly."
    c1 = "abcdefghij" * 50
    c2 = "klmnopqrst" * 60
    cursor = cursor.connection.cursor()

    cursor.execute("create table t1(c1 int, c2 int)")
    cursor.execute(f"select c1 as {c1}, c2 as {c2} from t1")

    names = [ t[0] for t in cursor.description ]
    names.sort()

    assert names == [ c1, c2 ]


def test_row_description(cursor: pyodbc.Cursor):
    """
    Ensure Cursor.description is accessible as Row.cursor_description.
    """
    cursor = cursor.connection.cursor()
    cursor.execute("create table t1(a int, b char(3))")
    cursor.execute("insert into t1 values(1, 'abc')")

    row = cursor.execute("select * from t1").fetchone()
    assert cursor.description == row.cursor_description


def test_executemany(cursor: pyodbc.Cursor):
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


def test_fast_executemany(cursor: pyodbc.Cursor):

    pyodbc.fast_executemany = True

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

    pyodbc.fast_executemany = False


def test_executemany_failure(cursor: pyodbc.Cursor):
    """
    Ensure that an exception is raised if one query in an executemany fails.
    """
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [ (1, "good"),
               ("error", "not an int"),
               (3, "good") ]

    with pytest.raises(pyodbc.Error):
        cursor.executemany("insert into t1(a, b) value (?, ?)", params)


def test_row_slicing(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b int, c int, d int)");
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = row[:]
    assert result is row

    result = row[:-1]
    assert result == (1,2,3)

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


def test_cnxn_set_attr_before(cursor: pyodbc.Cursor):
    # I don't have a getattr right now since I don't have a table telling me what kind of
    # value to expect.  For now just make sure it doesn't crash.
    # From the unixODBC sqlext.h header file.
    SQL_ATTR_PACKET_SIZE = 112
    othercnxn = connect(attrs_before={SQL_ATTR_PACKET_SIZE : 1024 * 32}, autocommit=True)


def test_cnxn_set_attr(cursor: pyodbc.Cursor):
    # I don't have a getattr right now since I don't have a table telling me what kind of
    # value to expect.  For now just make sure it doesn't crash.
    # From the unixODBC sqlext.h header file.
    SQL_ATTR_ACCESS_MODE = 101
    SQL_MODE_READ_ONLY   = 1
    cursor.connection.set_attr(SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_ONLY)


def test_columns(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b varchar(3), ώ decimal(8,2))")

    cursor.columns("t1")
    results = {row.column_name: row for row in cursor}
    row = results["a"]
    assert "INT" in row.type_name.upper(), row.type_name
    row = results["b"]
    assert row.type_name.upper() == "VARCHAR"
    row = results["ώ"]
    assert row.type_name.upper() in ("DECIMAL", "NUMERIC")

    # Now do the same, but specifically pass in None to one of the keywords.  Old versions
    # were parsing arguments incorrectly and would raise an error.  (This crops up when
    # calling indirectly like columns(*args, **kwargs) which aiodbc does.)

    cursor.columns("t1", schema=None, catalog=None)
    results = {row.column_name: row for row in cursor}
    row = results["a"]
    assert "INT" in row.type_name.upper(), row.type_name
    row = results["b"]
    assert row.type_name.upper() == "VARCHAR"


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

    cursor.execute("CREATE TABLE t1(s varchar(100))")
    cursor.execute("insert into t1 values (?)", v)

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v
