"""
Unit tests for PostgreSQL
"""
# -*- coding: utf-8 -*-

import os, uuid
from decimal import Decimal
from typing import Iterator

import pyodbc, pytest


CNXNSTR = os.environ.get('PYODBC_POSTGRESQL', 'DSN=pyodbc-postgres')


def connect(autocommit=False, attrs_before=None):
    return pyodbc.connect(CNXNSTR, autocommit=autocommit, attrs_before=attrs_before)


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


def _generate_str(length, encoding=None):
    """
    Returns either a string or bytes, depending on whether encoding is provided,
    that is `length` elements long.

    If length is None, None is returned.  This simplifies the tests by letting us put None into
    an array of other lengths and pass them here, moving the special case check into one place.
    """
    if length is None:
        return None

    seed = '0123456789-abcdefghijklmnopqrstuvwxyz-'

    if length <= len(seed):
        v = seed
    else:
        c = (length + len(seed) - 1 // len(seed))
        v = seed * c

    v = v[:length]
    if encoding:
        v = v.encode(encoding)

    return v


def test_text(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col text)")

    # Two different read code paths exist based on the length.  Using 100 and 4000 will ensure
    # both are tested.
    for length in [None, 0, 100, 1000, 4000]:
        cursor.execute("truncate table t1")
        param = _generate_str(length)
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_text_many(cursor: pyodbc.Cursor):

    # This shouldn't make a difference, but we'll ensure we can read and write from multiple
    # columns at the same time.

    cursor.execute("create table t1(col1 text, col2 text, col3 text)")

    v1 = 'ABCDEFGHIJ' * 30
    v2 = '0123456789' * 30
    v3 = '9876543210' * 30

    cursor.execute("insert into t1(col1, col2, col3) values (?,?,?)", v1, v2, v3)
    row = cursor.execute("select col1, col2, col3 from t1").fetchone()

    assert v1 == row.col1
    assert v2 == row.col2
    assert v3 == row.col3


def test_chinese(cursor: pyodbc.Cursor):
    v = '我的'
    row = cursor.execute("SELECT N'我的' AS name").fetchone()
    assert row[0] == v

    rows = cursor.execute("SELECT N'我的' AS name").fetchall()
    assert rows[0][0] == v


def test_bytea(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col bytea)")

    for length in [None, 0, 100, 1000, 4000]:
        cursor.execute("truncate table t1")
        param = _generate_str(length, 'utf8')
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_bytearray(cursor: pyodbc.Cursor):
    """
    We will accept a bytearray and treat it like bytes, but when reading we'll still
    get bytes back.
    """
    cursor.execute("create table t1(col bytea)")

    # Two different read code paths exist based on the length.  Using 100 and 4000 will ensure
    # both are tested.
    for length in [0, 100, 1000, 4000]:
        cursor.execute("truncate table t1")
        bytes = _generate_str(length, 'utf8')
        param = bytearray(bytes)
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == bytes


def test_int(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col int)")
    for param in [None, -1, 0, 1, 0x7FFFFFFF]:
        cursor.execute("truncate table t1")
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_bigint(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col bigint)")
    for param in [None, -1, 0, 1, 0x7FFFFFFF, 0xFFFFFFFF, 0x123456789]:
        cursor.execute("truncate table t1")
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_float(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col float)")
    for param in [None, -1, 0, 1, -200, 20000]:
        cursor.execute("truncate table t1")
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_decimal(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col decimal(20,6))")

    # Note: Use strings to initialize the decimals to eliminate floating point rounding.
    #
    # Also, the ODBC docs show the value 100010 in the C struct, so I've included it here,
    # along with a couple of shifted versions.
    params = [Decimal(n) for n in "-1000.10 -1234.56 -1 0 1 1000.10 1234.56 100010 123456789.21".split()]
    params.append(None)

    for param in params:
        cursor.execute("truncate table t1")
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_numeric(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(col numeric(20,6))")

    # Note: Use strings to initialize the decimals to eliminate floating point rounding.
    params = [Decimal(n) for n in "-1234.56  -1  0  1  1234.56  123456789.21".split()]
    params.append(None)

    for param in params:
        cursor.execute("truncate table t1")
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param


def test_maxwrite(cursor: pyodbc.Cursor):
    # If we write more than `maxwrite` bytes, pyodbc will switch from binding the data all at
    # once to providing it at execute time with SQLPutData.  The default maxwrite is 1GB so
    # this is rarely needed in PostgreSQL but I need to test the functionality somewhere.
    cursor.connection.maxwrite = 300

    cursor.execute("create table t1(col text)")
    param = _generate_str(400)
    cursor.execute("insert into t1 values (?)", param)
    result = cursor.execute("select col from t1").fetchval()
    assert result == param


def test_nonnative_uuid(cursor: pyodbc.Cursor):
    pyodbc.native_uuid = False

    param = uuid.uuid4()
    cursor.execute("create table t1(n uuid)")
    cursor.execute("insert into t1 values (?)", param)

    result = cursor.execute("select n from t1").fetchval()
    assert isinstance(result, str)
    assert result == str(param).upper()


def test_native_uuid(cursor: pyodbc.Cursor):
    pyodbc.native_uuid = True
    # When true, we should return a uuid.UUID object.

    param = uuid.uuid4()
    cursor.execute("create table t1(n uuid)")
    cursor.execute("insert into t1 values (?)", param)

    result = cursor.execute("select n from t1").fetchval()
    assert isinstance(result, uuid.UUID)
    assert param == result


def test_close_cnxn(cursor: pyodbc.Cursor):
    """Make sure using a Cursor after closing its connection doesn't crash."""

    cursor.execute("create table t1(id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, 'test')
    cursor.execute("select * from t1")

    cursor.connection.close()

    # Now that the connection is closed, we expect an exception.  (If the code attempts to use
    # the HSTMT, we'll get an access violation instead.)

    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("select * from t1")


def test_version():
    assert len(pyodbc.version.split('.')) == 3


def test_rowcount(cursor: pyodbc.Cursor):
    assert cursor.rowcount == -1
    # The spec says it should be -1 when not in use.

    cursor.execute("create table t1(col int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)

    cursor.execute("select * from t1")
    assert cursor.rowcount == count

    cursor.execute("update t1 set col=col+1")
    assert cursor.rowcount == count

    cursor.execute("delete from t1")
    assert cursor.rowcount == count

    # This is a different code path - the value internally is SQL_NO_DATA instead of an empty
    # result set.  Just make sure it doesn't crash.
    cursor.execute("delete from t1")
    assert cursor.rowcount == 0

    # IMPORTANT: The ODBC spec says it should be -1 after the create table, but the PostgreSQL
    # driver is telling pyodbc the rowcount is 0.  Since we have no way of knowing when to
    # override it, we'll just update the test to ensure it is consistently zero.

    cursor.execute("create table t2(i int)")
    assert cursor.rowcount == 0


def test_row_description(cursor: pyodbc.Cursor):
    """
    Ensure Cursor.description is accessible as Row.cursor_description.
    """
    cursor.execute("create table t1(col1 int, col2 char(3))")
    cursor.execute("insert into t1 values(1, 'abc')")

    row = cursor.execute("select col1, col2 from t1").fetchone()

    assert row.cursor_description == cursor.description


def test_lower_case(cursor: pyodbc.Cursor):
    "Ensure pyodbc.lowercase forces returned column names to lowercase."

    try:
        pyodbc.lowercase = True

        cursor.execute("create table t1(Abc int, dEf int)")
        cursor.execute("select * from t1")

        names = {t[0] for t in cursor.description}
        assert names == {'abc', 'def'}
    finally:
        pyodbc.lowercase = False


def test_executemany(cursor: pyodbc.Cursor):

    cursor.execute("create table t1(col1 int, col2 varchar(10))")
    params = [(i, str(i)) for i in range(1, 6)]

    # Without fast_executemany

    cursor.executemany("insert into t1(col1, col2) values (?,?)", params)
    cursor.execute("select col1, col2 from t1 order by col1")
    results = [tuple(row) for row in cursor]
    assert results == params

    # With fast_executemany

    try:
        pyodbc.fast_executemany = True
        cursor.execute("truncate table t1")
        cursor.executemany("insert into t1(col1, col2) values (?,?)", params)
        cursor.execute("select col1, col2 from t1 order by col1")
        results = [tuple(row) for row in cursor]
        assert results == params
    finally:
        pyodbc.fast_executemany = False


def test_executemany_failure(cursor: pyodbc.Cursor):
    """
    Ensure that an exception is raised if one query in an executemany fails.
    """
    cursor.execute("create table t1(a int, b varchar(10))")

    params = [ (1, 'good'),
               ('error', 'not an int'),
               (3, 'good') ]

    with pytest.raises(pyodbc.Error):
        cursor.executemany("insert into t1(a, b) value (?, ?)", params)


def test_row_slicing(cursor: pyodbc.Cursor):
    cursor.execute("create table t1(a int, b int, c int, d int)")
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = row[:]
    assert result is row  # returned as is

    result = row[:-1]
    assert result == (1, 2, 3)  # returned as tuple

    result = row[0:4]
    assert result is row


def test_drivers():
    p = pyodbc.drivers()
    assert isinstance(p, list)


def test_datasources():
    p = pyodbc.dataSources()
    assert isinstance(p, dict)


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

def test_cnxn_execute_error(cursor: pyodbc.Cursor):
    """
    Make sure that Connection.execute (not Cursor) errors are not "eaten".

    GitHub issue #74
    """
    cursor.execute("create table t1(a int primary key)")
    cursor.execute("insert into t1 values (1)")
    with pytest.raises(pyodbc.Error):
        cursor.connection.execute("insert into t1 values (1)")

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


def test_autocommit(cursor: pyodbc.Cursor):
    assert cursor.connection.autocommit is False
    othercnxn = connect(autocommit=True)
    assert othercnxn.autocommit is True
    othercnxn.autocommit = False
    assert othercnxn.autocommit is False

def test_exc_integrity(cursor: pyodbc.Cursor):
    "Make sure an IntegretyError is raised"
    # This is really making sure we are properly encoding and comparing the SQLSTATEs.
    cursor.execute("create table t1(s1 varchar(10) primary key)")
    cursor.execute("insert into t1 values ('one')")
    with pytest.raises(pyodbc.IntegrityError):
        cursor.execute("insert into t1 values ('one')")


def test_cnxn_set_attr_before():
    # I don't have a getattr right now since I don't have a table telling me what kind of
    # value to expect.  For now just make sure it doesn't crash.
    # From the unixODBC sqlext.h header file.
    SQL_ATTR_PACKET_SIZE = 112
    _cnxn = connect(attrs_before={ SQL_ATTR_PACKET_SIZE : 1024 * 32 })


def test_cnxn_set_attr(cursor: pyodbc.Cursor):
    # I don't have a getattr right now since I don't have a table telling me what kind of
    # value to expect.  For now just make sure it doesn't crash.
    # From the unixODBC sqlext.h header file.
    SQL_ATTR_ACCESS_MODE = 101
    SQL_MODE_READ_ONLY   = 1
    cursor.connection.set_attr(SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_ONLY)


def test_columns(cursor: pyodbc.Cursor):
    driver_version = tuple(
        int(x) for x in cursor.connection.getinfo(pyodbc.SQL_DRIVER_VER).split(".")
    )

    def _get_column_size(row):
        # the driver changed the name of the returned columns in version 13.02.
        # see https://odbc.postgresql.org/docs/release.html, release 13.02.0000, change 6.
        return row.column_size if driver_version >= (13, 2, 0) else row.precision

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
    assert row.type_name == 'int4', row.type_name
    row = results['b']
    assert row.type_name == 'varchar'
    assert _get_column_size(row) == 3, _get_column_size(row)
    row = results['xΏz']
    assert row.type_name == 'varchar'
    assert _get_column_size(row) == 4, _get_column_size(row)

    # Now do the same, but specifically pass in None to one of the keywords.  Old versions
    # were parsing arguments incorrectly and would raise an error.  (This crops up when
    # calling indirectly like columns(*args, **kwargs) which aiodbc does.)

    cursor.columns('t1', schema=None, catalog=None)
    results = {row.column_name: row for row in cursor}
    row = results['a']
    assert row.type_name == 'int4', row.type_name
    row = results['b']
    assert row.type_name == 'varchar'
    assert _get_column_size(row) == 3

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

def test_emoticons_as_literal(cursor: pyodbc.Cursor):
    # https://github.com/mkleehammer/pyodbc/issues/630

    v = "x \U0001F31C z"

    cursor.execute("CREATE TABLE t1(s varchar(100))")
    cursor.execute(f"insert into t1 values ('{v}')")

    result = cursor.execute("select s from t1").fetchone()[0]

    assert result == v


def test_cursor_messages(cursor: pyodbc.Cursor):
    """
    Test the Cursor.messages attribute.
    """
    # Using INFO message level because they are always sent to the client regardless of

    # client_min_messages: https://www.postgresql.org/docs/11/runtime-config-client.html
    for msg in ('hello world', 'ABCDEFGHIJ' * 800):
        cursor.execute(f"""
            CREATE OR REPLACE PROCEDURE test_cursor_messages()
            LANGUAGE plpgsql
            AS $$
            BEGIN
                RAISE INFO '{msg}' USING ERRCODE = '01000';
            END;
            $$;
        """)
        cursor.execute("CALL test_cursor_messages();")
        messages = cursor.messages

        # There is a maximum size for these so the second msg will actually generate a bunch of
        # messages.  To make it easier to compare, we'll stitch them back together.

        if len(messages) > 1:
            concat = ''.join(t[1] for t in messages)
            messages = [(messages[0][0], concat)]

        assert messages == [('[01000] (-1)', f'INFO: {msg}')]


def test_output_conversion(cursor: pyodbc.Cursor):
    # Note the use of SQL_WVARCHAR, not SQL_VARCHAR.

    def convert(value):
        # The value is the raw bytes (as a bytes object) read from the
        # database.  We'll simply add an X at the beginning at the end.
        return 'X' + value.decode('latin1') + 'X'

    cursor.execute("create table t1(n int, v varchar(10))")
    cursor.execute("insert into t1 values (1, '123.45')")

    cursor.connection.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    # Clear all conversions and try again.  There should be no Xs this time.
    cursor.connection.clear_output_converters()
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'

    # Same but clear using remove_output_converter.
    cursor.connection.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    cursor.connection.remove_output_converter(pyodbc.SQL_WVARCHAR)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'

    # And lastly, clear by passing None for the converter.
    cursor.connection.add_output_converter(pyodbc.SQL_WVARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == 'X123.45X'

    cursor.connection.add_output_converter(pyodbc.SQL_WVARCHAR, None)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == '123.45'
