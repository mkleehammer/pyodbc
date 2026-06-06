"""Unit tests for Access

Access SQL data types: http://msdn2.microsoft.com/en-us/library/bb208866.aspx
"""

import datetime
import decimal
import pathlib
import uuid

import pyodbc
import pytest

DRIVER = "{Microsoft Access Driver (*.mdb, *.accdb)}"
DIRECTORY = pathlib.Path(__file__).parent
LEGACY_PATH = DIRECTORY / f"xxx_pyodbc_test.mdb"
MODERN_PATH = DIRECTORY / f"xxx_pyodbc_test.accdb"
LEGACY_BYTES = (DIRECTORY / "empty.mdb").read_bytes()
MODERN_BYTES = (DIRECTORY / "empty.accdb").read_bytes()
LEGACY_INITIAL_SIZE = LEGACY_PATH.write_bytes(LEGACY_BYTES)
MODERN_INITIAL_SIZE = MODERN_PATH.write_bytes(MODERN_BYTES)
LEGACY_CONNSTR = f"DRIVER={DRIVER};DBQ={LEGACY_PATH};ExtendedAnsiSQL=1"
MODERN_CONNSTR = f"DRIVER={DRIVER};DBQ={MODERN_PATH};ExtendedAnsiSQL=1"
SMALL_FENCEPOST_SIZES = 0, 1, 254, 255  # text fields <= 255
LARGE_FENCEPOST_SIZES = 256, 270, 304, 508, 510, 511, 512, 1023, 1024, 2047, 2048, 4000, 4095, 4096, 4097, 10 * 1024, 20 * 1024

#----------------------------------------------------------------------
# Helper functions
#----------------------------------------------------------------------

def connect(legacy=False, autocommit=False):
    """Helper function to create a new Connection object"""

    connstr = LEGACY_CONNSTR if legacy else MODERN_CONNSTR
    return pyodbc.connect(connstr, autocommit=autocommit)


@pytest.fixture
def cursors():
    """Create a pair of Cursor objects and remove any leftover test tables"""

    connections = connect(legacy=True), connect(legacy=False)
    cursors = [conn.cursor() for conn in connections]
    for cursor in cursors:
        for i in range(3):
            try:
                cursor.execute(f"drop table t{i + 1}")
            except pyodbc.ProgrammingError:
                pass
        cursor.connection.commit()

    yield cursors

    for cursor in cursors:
        if not cursor.connection.closed:
            connection = cursor.connection
            cursor.close()
            connection.close()


def _generate_str(length, encoding=None):
    """
    Returns either a string or bytes, depending on whether encoding is specified, of the requested length.

    To enhance performance, there are 3 ways data is read, based on the length of the value, so most data types are
    tested with 3 lengths.  This function helps us generate the test data.

    We use a recognizable data set instead of a single character to make it less likely that "overlap" errors will
    be hidden and to help us manually identify where a break occurs.
    """

    characters = "0123456789-abcdefghijklmnopqrstuvwxyz-"
    if length <= len(characters):
        v = characters
    else:
        c = (length + len(characters)-1) // len(characters)
        v = characters * c
    v =  v[:length]
    if encoding:
        v = v.encode(encoding)
    return v


def _test_strtype(cursor, sqltype, value, colsize=None):
    """Helper function for testing string and byte columns"""

    assert colsize is None or value is None or colsize >= len(value), f"colsize={colsize} vallen={len(value)}"
    sqltype = f"{sqltype}({colsize})" if colsize else sqltype
    cursor.execute(f"create table t1 (n1 int not null, s1 {sqltype}, s2 {sqltype})")
    cursor.execute("insert into t1 values (1, ?, ?)", (value, value))
    row = cursor.execute("select s1, s2 from t1").fetchone()
    for i in range(2):
        v = row[i]
        assert type(v) == type(value)
        if value is not None:
            assert len(v) == len(value)
        assert v == value
    cursor.execute("drop table t1")


#----------------------------------------------------------------------
# Tests for value types supported by Access
#----------------------------------------------------------------------

def test_binary_null(cursors):
    """Test handling of a NULL value for a binary column"""
    
    for cursor in cursors:
        _test_strtype(cursor, "binary", None)


def test_bit(cursors):
    """Test handling of BIT column values"""

    value = True
    for cursor in cursors:
        cursor.execute("create table t1 (b bit)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select b from t1").fetchval()
        assert type(result) is bool
        assert value == result


def test_bit_null(cursors):
    """Test handling of NULL in BIT columns"""

    value = None
    for cursor in cursors:
        cursor.execute("create table t1 (b bit)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select b from t1").fetchval()
        assert type(result) is bool
        assert False == result


def test_datetime(cursors):
    """Verify proper handling of DATETIME column values"""

    value = datetime.datetime(2007, 1, 15, 3, 4, 5)
    for cursor in cursors:
        cursor.execute("create table t1 (dt datetime)")
        cursor.execute("insert into t1 values (?)", value)

        result = cursor.execute("select dt from t1").fetchval()
        assert result == value


def test_decimal(cursors):
    """Test handling of DECIMAL column values"""

    value = decimal.Decimal("12345.6789")
    for cursor in cursors:
        cursor.execute("create table t1 (n numeric(10,4))")
        cursor.execute("insert into t1 values(?)", value)
        v = cursor.execute("select n from t1").fetchval()
        assert isinstance(v, decimal.Decimal)
        assert v == value


def test_negative_decimal(cursors):
    """Test handling of negative DECIMAL column values"""

    value = decimal.Decimal("-10.0010")
    for cursor in cursors:
        cursor.execute("create table t1 (d numeric(19,4))")
        cursor.execute("insert into t1 values(?)", value)
        v = cursor.execute("select * from t1").fetchval()
        assert isinstance(v, decimal.Decimal)
        assert v == value


def test_float(cursors):
    """Test handling of FLOAT column values"""

    for cursor in cursors:
        value = 1234.567
        cursor.execute("create table t1 (n float)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert result == value


def test_negative_float(cursors):
    """Test handling of negative FLOAT column values"""

    for cursor in cursors:
        value = -200.5
        cursor.execute("create table t1 (n float)")
        cursor.execute("insert into t1 values (?)", value)
        result  = cursor.execute("select n from t1").fetchval()
        assert value == result


def test_guid(cursors):
    """Test handling of GUID column values"""

    values = "de2ac9c6-8676-4b0b-b8a6-217a8580cbee", uuid.uuid4()
    for cursor in cursors:
        cursor.execute("create table t1 (i int, g uniqueidentifier)")
        for i, value in enumerate(values, 1):
            cursor.execute("insert into t1 values (?, ?)", (i, value))
        results = cursor.execute("select g from t1 order by i")
        results = [row.g for row in cursor.fetchall()]
        for result, value in zip(results, map(str, values)):
            print(type(result), type(value))
            assert type(result) == type(value)
            assert len(result) == len(value)


def test_image(cursors):
    """Test handling of IMAGE columns of various lengths"""

    for cursor in cursors:
        for size in SMALL_FENCEPOST_SIZES + LARGE_FENCEPOST_SIZES:
            value = _generate_str(size, encoding="utf-8")
            _test_strtype(cursor, "image", value)


def test_image_null(cursors):
    """Test handling of a NULL value for an IMAGE column"""
    
    for cursor in cursors:
        _test_strtype(cursor, "image", None)


def test_int(cursors):
    """Test handling of INT column values"""

    value = 1234
    for cursor in cursors:
        cursor.execute("create table t1 (n int)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert result == value


def test_smallint(cursors):
    """Test handling of SMALLINT column values"""

    for cursor in cursors:
        value = 32767
        cursor.execute("create table t1 (n smallint)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert result == value


def test_tinyint(cursors):
    """Test handling of TINYINT column values"""

    for cursor in cursors:
        cursor.execute("create table t1 (n tinyint)")
        value = 10
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert type(result) == type(value)
        assert value == result


def test_negative_int(cursors):
    """Test handling of negative INT column values"""

    for cursor in cursors:
        value = -1
        cursor.execute("create table t1 (n int)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert result == value


def test_memo(cursors):
    """Test handling of MEMO columns of various lengths"""

    for cursor in cursors:
        for size in SMALL_FENCEPOST_SIZES + LARGE_FENCEPOST_SIZES:
            value = _generate_str(size)
            _test_strtype(cursor, "memo", value)


def test_memo_null(cursors):
    """Test handling of a NULL value for a memo column"""
    
    for cursor in cursors:
        _test_strtype(cursor, "memo", None)


def test_money(cursors):
    """Test handling of MONEY column values"""

    value = decimal.Decimal("1234.45")
    for cursor in cursors:
        cursor.execute("create table t1 (n money)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert type(result) == type(value)
        assert value == result


def test_real(cursors):
    """Test handling of REAL column values"""

    for cursor in cursors:
        value = 1234.5
        cursor.execute("create table t1 (n real)")
        cursor.execute("insert into t1 values (?)", value)
        result = cursor.execute("select n from t1").fetchval()
        assert result == value


def test_negative_real(cursors):
    """Test handling of negative REAL column values"""

    for cursor in cursors:
        value = -200.5
        cursor.execute("create table t1 (n real)")
        cursor.execute("insert into t1 values (?)", value)
        result  = cursor.execute("select n from t1").fetchval()
        assert value == result


def test_varbinary(cursors):
    """Test handling of VARBINARY columns of various lengths"""

    for cursor in cursors:
        for size in SMALL_FENCEPOST_SIZES:
            value = _generate_str(size, encoding="utf-8")
            _test_strtype(cursor, "varbinary", value, size)


def test_varchar_null(cursors):
    """Test handling of a NULL value for a varchar column"""
    
    for cursor in cursors:
        _test_strtype(cursor, "varchar", None)


#----------------------------------------------------------------------
# Tests for module-level properties and methods
#----------------------------------------------------------------------

def test_datasources():
    """Confirm that the dataSource() method is implemented"""

    p = pyodbc.dataSources()
    assert isinstance(p, dict)


def test_drivers():
    """Verify that the drivers() method is implemented"""

    p = pyodbc.drivers()
    assert isinstance(p, list)


def test_lower_case():
    "Verify that pyodbc.lowercase forces returned column names to lowercase."

    # Has to be set before creating the cursor, so we must create our own cursors.
    pyodbc.lowercase = True
    for legacy in (True, False):
        cursor = connect(legacy=legacy).cursor()
        cursor.execute("create table t1 (Abc int, dEf int)")
        cursor.execute("select * from t1")
        names = sorted([column[0] for column in cursor.description])
        assert names == ["abc", "def"]

    # Put it back so other tests don't fail.
    pyodbc.lowercase = False


def test_version():
    """Confirm that the module version property returns a well-formed version number (e.g., 5.3.0)"""
    assert len(pyodbc.version.split(".")) == 3


#----------------------------------------------------------------------
# Tests for Connection methods and properties
#----------------------------------------------------------------------

def test_autocommit(cursors):
    """Confirm the independence of the autocommit property of different Connection objects"""

    legacy = True
    for cursor in cursors:
        assert cursor.connection.autocommit is False
        othercnxn = connect(autocommit=True, legacy=legacy)
        legacy = False
        assert othercnxn.autocommit is True
        assert cursor.connection.autocommit is False
        cursor.connection.autocommit = True
        othercnxn.autocommit = False
        assert othercnxn.autocommit is False
        assert cursor.connection.autocommit is True


def test_closed_reflects_connection_state(cursors):
    """Confirm that the closed property tracks the state of the Connection object"""

    for cursor in cursors:
        assert not cursor.connection.closed
        cursor.connection.close()
        assert cursor.connection.closed


def test_getinfo_bool(cursors):
    """Test retrieval of a Boolean property of a Connection object"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
        assert isinstance(value, bool)


def test_getinfo_int(cursors):
    """Test retrieval of an integer property of a Connection object"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        assert isinstance(value, int)


def test_getinfo_smallint(cursors):
    """Test retrieval of a SMALLINT property of a Connection object"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        assert isinstance(value, int)


def test_getinfo_string(cursors):
    """Test retrieval of a string property of a Connection object"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
        assert isinstance(value, str)


#----------------------------------------------------------------------
# Tests for Cursor methods and properties
#----------------------------------------------------------------------

def test_cursor_with_closed_connection(cursors):
    """Make sure using a Cursor after closing its connection fails"""

    for cursor in cursors:
        cursor.execute("create table t1 (id integer, s varchar(20))")
        cursor.execute("insert into t1 values (?,?)", (1, "test"))
        cursor.execute("select * from t1")
        cursor.connection.close()
        
        # Now that the connection is closed, we expect an exception.  (If the code attempts to use
        # the HSTMT, we'll get an access violation instead.)
        with pytest.raises(pyodbc.ProgrammingError):
            cursor.execute("select * from t1")


def test_concatenation(cursors):
    """Test string concatenation expressions in SELECT statements"""

    v2 = "0123456789" * 25
    v3 = "9876543210" * 25
    expected = v2 + "x" + v3
    for cursor in cursors:
        cursor.execute("create table t1 (c2 varchar(250), c3 varchar(250))")
        cursor.execute("insert into t1 (c2, c3) values (?,?)", v2, v3)
        observed = cursor.execute("select c2 + 'x' + c3 from t1").fetchval()
        assert observed == expected


def test_different_bindings(cursors):
    """Test use of a single cursor using more than one table"""

    for cursor in cursors:
        cursor.execute("create table t1 (n int)")
        cursor.execute("create table t2 (d datetime)")
        cursor.execute("insert into t1 values (?)", 1)
        cursor.execute("insert into t2 values (?)", datetime.datetime.now())


def test_executemany(cursors):
    """Test inserting multiple rows in a single call"""

    params = [ (i, str(i)) for i in range(1, 6) ]
    for cursor in cursors:
        cursor.execute("create table t1 (a int, b varchar(10))")
        cursor.executemany("insert into t1 (a, b) values (?,?)", params)
        count = cursor.execute("select count(*) from t1").fetchval()
        assert count == len(params)
        cursor.execute("select a, b from t1 order by a")
        rows = cursor.fetchall()
        assert count == len(rows)
        for param, row in zip(params, rows):
            assert param[0] == row[0]
            assert param[1] == row[1]


def test_executemany_failure(cursors):
    """Verify that an exception is raised if one query in an executemany fails"""

    for cursor in cursors:
        cursor.execute("create table t1 (a int, b varchar(10))")
        params = [
            (1, "good"),
            ("error", "not an int"),
            (3, "good"),
        ]
        with pytest.raises(pyodbc.Error):
            cursor.executemany("insert into t1 (a, b) values (?, ?)", params)

        # This check would fail, because the driver misbehaves and lets one of the
        # inserts through, even though autocommit is off and we haven't committed
        # anything.
        # rows = cursor.execute("select * from t1").fetchall()
        # assert rows == []

        
def test_multiple_bindings(cursors):
    """Test multiple binds and selects on a cursor"""

    for cursor in cursors:
        cursor.execute("create table t1 (n int)")
        cursor.execute("insert into t1 values (?)", 1)
        cursor.execute("insert into t1 values (?)", 2)
        cursor.execute("insert into t1 values (?)", 3)
        for i in range(3):
            cursor.execute("select n from t1 where n < ?", 10)
            cursor.execute("select n from t1 where n < 3")
    

def test_rowcount_delete(cursors):
    """Verify that we get the correct rowcount following a DELETE"""

    count = 4
    for cursor in cursors:
        assert cursor.rowcount == -1
        cursor.execute("create table t1 (i int)")
        for i in range(count):
            cursor.execute("insert into t1 values (?)", i)
        cursor.execute("delete from t1")
        assert cursor.rowcount == count


def test_rowcount_nodata(cursors):
    """Verify that we get a zero rowcount for a DELETE which resulted in SQL_NO_DATA"""

    for cursor in cursors:
        """
        This represents a different code path than a delete that deleted something.

        The return value is SQL_NO_DATA and code after it was causing an error.  We could use SQL_NO_DATA to step over
        the code that errors out and drop down to the same SQLRowCount code.  On the other hand, we could hardcode a
        zero return value.
        """

        cursor.execute("create table t1 (i int)")
        # This is a different code path internally.
        cursor.execute("delete from t1")
        assert cursor.rowcount == 0


def test_rowcount_reset(cursors):
    """Verify that rowcount is reset to -1"""

    count = 4
    for cursor in cursors:
        cursor.execute("create table t1 (i int)")
        for i in range(count):
            cursor.execute("insert into t1 values (?)", i)
        assert cursor.rowcount == 1
        cursor.execute("create table t2(i int)")
        assert cursor.rowcount == -1


def test_rowcount_select(cursors):
    """Verify that Cursor.rowcount is set properly after a select statement"""

    count = 4
    for cursor in cursors:
        cursor.execute("create table t1 (i int)")
        count = 4
        for i in range(count):
            cursor.execute("insert into t1 values (?)", i)
        cursor.execute("select * from t1")
        assert cursor.rowcount == -1
        rows = cursor.fetchall()
        assert len(rows) == count
        assert cursor.rowcount == -1


def test_row_description(cursors):
    """Make sure Cursor.description is accessible as Row.cursor_description"""

    for cursor in cursors:
        cursor.execute("create table t1 (a int, b char(3))")
        cursor.commit()
        cursor.execute("insert into t1 values(1, 'abc')")
        row = cursor.execute("select * from t1").fetchone()
        assert cursor.description == row.cursor_description
        

def test_subquery_params(cursors):
    """Verify that parameter markers work in a subquery"""

    for cursor in cursors:
        cursor.execute("create table t1 (id integer, s varchar(20))")
        cursor.execute("insert into t1 values (?,?)", 1, "test")
        row = cursor.execute("""\
            select x.id
            from (
                select id
                from t1
                where s = ?
                    and id between ? and ?
            ) x
            """, ("test", 1, 10)).fetchone()
        assert row is not None
        assert row.id == 1
        cursor.execute("drop table t1")


def test_unicode_query(cursors):
    """Verify that non-ASCII parameters are handled correctly"""

    name = "王伟"
    for cursor in cursors:
        cursor.execute("create table t1 (id int, name varchar)")
        cursor.execute("insert into t1 values (?, ?)", (1, "John Smith"))
        cursor.execute("insert into t1 values (?, ?)", (2, name))
        cursor.execute("select id from t1 where name = ?", name)
        rows = cursor.fetchall()
        assert len(rows) == 1
        assert rows[0].id == 2


#----------------------------------------------------------------------
# Tests for Row methods and properties
#----------------------------------------------------------------------

def test_negative_row_index(cursors):
    """Verify that the Row class handles negative index access correctly"""

    for cursor in cursors:
        cursor.execute("create table t1 (s varchar(20))")
        cursor.execute("insert into t1 values (?)", "1")
        row = cursor.execute("select * from t1").fetchone()
        assert row[0] == "1"
        assert row[-1] == "1"


def test_row_repr(cursors):
    """Test serialization of Row class objects"""

    for cursor in cursors:
        cursor.execute("create table t1 (a int, b int, c int, d int)");
        cursor.execute("insert into t1 values (1,2,3,4)")
        row = cursor.execute("select * from t1").fetchone()
        assert str(row) == "(1, 2, 3, 4)"
        assert str(row[:-1]) == "(1, 2, 3)"
        assert str(row[:1]) == "(1,)"


def test_row_slicing(cursors):
    """Verify that the Row class handles slicing operations correctly"""

    for cursor in cursors:
        cursor.execute("create table t1 (a int, b int, c int, d int)");
        cursor.execute("insert into t1 values (1,2,3,4)")
        row = cursor.execute("select * from t1").fetchone()
        assert row[:] is row
        assert row[:-1] == (1,2,3)
        assert row[0:4] is row
