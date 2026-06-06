"""Unit tests for Informix DB."""

from datetime import date, datetime
from decimal import Decimal
from json import dumps, loads
from os import environ
from pathlib import Path
from typing import Iterator
from uuid import UUID

import pyodbc
import pytest

CNXNSTR = environ.get("PYODBC_INFORMIX", "DSN=pyodbc-informix")


#----------------------------------------------------------------------
# Helper functions
#----------------------------------------------------------------------

def connect(autocommit=False, attrs_before=None):
    """Create a connection to the Informix database"""
    return pyodbc.connect(CNXNSTR, autocommit=autocommit, attrs_before=attrs_before)


DRIVER = connect().getinfo(pyodbc.SQL_DRIVER_NAME)


@pytest.fixture
def cursor() -> Iterator[pyodbc.Cursor]:
    """Cursor object supplied on demand to test functions"""

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


#----------------------------------------------------------------------
# Tests for individual data types supported by Informix
#----------------------------------------------------------------------

def test_bigint(cursor: pyodbc.Cursor):
    """Test the Informix BIGINT type (a.k.a. INT8), including the reserved sentinel value"""

    values =-1, 0, 1, 0x7FFF_FFFE, 0x1_2345_6789, -9_223_372_036_854_775_807, 9_223_372_036_854_775_807
    for typename in ("bigint", "int8"):
        for value in values:
            cursor.execute(f"create table t1 (col {typename})")
            cursor.execute("insert into t1 values (?)", value)
            result = cursor.execute("select col from t1").fetchval()
            assert result == value
            cursor.execute("drop table t1")
        cursor.execute(f"create table t1 (col {typename})")
        reserved = 9_223_372_036_854_775_808
        with pytest.raises(OverflowError):
            cursor.execute("insert into t1 values (?)", reserved)
        cursor.execute("drop table t1")


def test_bigserial(cursor: pyodbc.Cursor):
    """Test the Informix BIGSERIAL type"""

    cursor.execute("create table t1 (id bigserial, name varchar(32))")
    cursor.execute("insert into t1 (name) values (?)", "John")
    cursor.execute("insert into t1 (name) values (?)", "Paul")
    cursor.execute("insert into t1 (name) values (?)", "George")
    cursor.execute("insert into t1 (id, name) values (?, ?)", (9_223_372_036_854_775_807, "Ringo"))
    cursor.execute("select * from t1 order by id")
    rows = [tuple(row) for row in cursor.fetchall()]
    assert rows == [(1, "John"), (2, "Paul"), (3, "George"), (9_223_372_036_854_775_807, "Ringo")]


def test_blob(cursor: pyodbc.Cursor, tmp_path):
    """Test the BLOB data type

    This type and the sibling CLOB type don't work the way normal column values do.
    They're basically intended to come and go directly from/to files, not memory.
    If anyone knows otherwise, feel free to jump in and improve this test.
    The odd-looking lotofile function has nothing to do with playing the numbers.
    It stands for "large object to file" (and we'll see it again when we test CLOB).
    As you can see the results set gives you the *real* path for the file into which
    the BLOB value was written, based on the path we gave it.
    """

    value = _generate_str(4096, encoding="ascii")
    insert_path = tmp_path / "blob.bin"
    insert_path.write_bytes(value)
    cursor.execute("create table t1 (id int, value blob)")
    cursor.execute("insert into t1 (id, value) values (42, filetoblob(?, 'client'))", str(insert_path))
    cursor.commit()
    fetch_path = tmp_path / "retrieved_blob.bin"
    cursor.execute("select lotofile(value, ?, 'client') as output_path from t1 where id = ?",  (str(fetch_path), 42))
    output_path = Path(cursor.fetchval())
    retrieved_bytes = output_path.read_bytes()
    assert retrieved_bytes == value


def test_boolean(cursor: pyodbc.Cursor):
    """Test the Informix BOOLEAN type"""

    cursor.execute("create table t1 (id int, flag boolean)")
    cursor.execute("insert into t1 values (?, ?)", (1, True))
    cursor.execute("insert into t1 values (?, ?)", (2, False))
    cursor.execute("select * from t1 order by id")
    rows = [tuple(row) for row in cursor.fetchall()]
    assert rows == [(1, True), (2, False)]


def test_bson(cursor: pyodbc.Cursor):
    """Test the Informix BSON column type"""

    cursor.execute("create table t1 (id int, person bson)")
    cursor.execute("""\
        create index person_surname_idx
        on t1(bson_get(person, 'name.family')) using bson
    """)
    people = [
        {"name": {"given": "Jim", "family": "Flynn"}, "age": 29, "cars": ["Dodge", "Olds"]},
        {"name": {"given": "Penélope", "family": "Cruz"}, "age": 32, "cars": ["Chevy"]},
        {"name": {"given": "Günther", "family": "Schmidt"}, "age": 19, "cars": ["Toyota", "Ford"]},
    ]
    for i, record in enumerate(people, 1):
        cursor.execute(f"insert into t1 values (?, ?::json)", (i, dumps(record)))
    cursor.execute("""
        select person::json::lvarchar(8192) as person
        from t1
        where bson_value_lvarchar(person, 'name.family') < 'M'
        order by bson_value_lvarchar(person, 'name.family')
    """)
    observed = [loads(row.person) for row in cursor.fetchall()]
    expected = [people[1], people[0]]
    assert observed == expected


def test_byte(cursor: pyodbc.Cursor):
    """Test the Informix BYTE type

    Same problem as with the TEXT type (see test_text() below): we can't fetch the data
    using ODBC. Trying to do so with isql gets "[Informix ODBC Driver]Communication link
    failure. [Informix ODBC Driver]Data truncated."
    """

    lengths = None, 0, 100, 1000, 4000
    values = [_generate_str(length, encoding="utf-8") for length in lengths]
    cursor.execute("create table t1 (id int, val byte)")
    for i, value in enumerate(values, 1):
        cursor.execute("insert into t1 values (?, ?)", (i, value))
    cursor.commit()
    cursor.execute("select length(val) as vlen from t1 where val is not null order by 1")
    observed_lengths = [row.vlen for row in cursor.fetchall()]
    expected_lengths = sorted([length for length in lengths if length is not None])
    assert observed_lengths == expected_lengths


def test_char(cursor: pyodbc.Cursor):
    """Test the Informix CHAR type"""

    lengths = 10, 100, 1000, 4000, 32767
    table = "t1"
    for length in lengths:
        value = _generate_str(length)
        cursor.execute(f"create table {table} (val char({length}))")
        cursor.execute(f"insert into {table} values (?)", value)
        cursor.execute(f"insert into {table} values (?)", None)
        cursor.execute(f"select val from {table} where val is not null")
        result = cursor.fetchval()
        assert result == value
        cursor.execute(f"drop table {table}")
    with pytest.raises(pyodbc.Error):
        cursor.execute(f"create table {table} (val char(32768))")


def test_clob(cursor: pyodbc.Cursor, tmp_path):
    """Test the CLOB data type (see notes on BLOB type above)"""

    value = _generate_str(4096, encoding="utf-8")
    insert_path = tmp_path / "clob.bin"
    insert_path.write_bytes(value)
    cursor.execute("create table t1 (id int, value clob)")
    cursor.execute("insert into t1 (id, value) values (42, filetoclob(?, 'client'))", str(insert_path))
    cursor.commit()
    fetch_path = tmp_path / "retrieved_clob.bin"
    cursor.execute("select lotofile(value, ?, 'client') as output_path from t1 where id = ?",  (str(fetch_path), 42))
    output_path = Path(cursor.fetchval())
    retrieved_bytes = output_path.read_bytes()
    assert retrieved_bytes == value


def test_date(cursor: pyodbc.Cursor):
    """Test the Informix DATE type"""

    cursor.execute("create table t1 (d date)")
    values = date(1800, 1, 1), date(3456, 12, 25), date(1776, 7, 4), date(1066, 10, 14)
    for value in values:
        cursor.execute("insert into t1 values (?)", value)
        cursor.execute("insert into t1 values (?)", None)
    n = cursor.execute("select count(*) from t1").fetchval()
    assert n == len(values) * 2
    rows = cursor.execute("select * from t1 where d is not null").fetchall()
    dates = sorted([row.d for row in rows])
    for d in dates:
        assert type(d) is date
    assert dates == sorted(values)


def test_datetime(cursor: pyodbc.Cursor):
    """Test the Informix DATETIME type"""

    cases = (
        ("year to year", "2001", datetime(2001, 1, 1)),
        ("year to month", "2001-02", datetime(2001, 2, 1)),
        ("year to day", "2001-02-03", date(2001, 2, 3)),
        ("year to hour", "2001-02-03 04", datetime(2001, 2, 3, 4)),
        ("year to minute", "2001-02-03 04:05", datetime(2001, 2, 3, 4, 5)),
        ("year to second", "2001-02-03 04:05:06", datetime(2001, 2, 3, 4, 5, 6)),
        ("year to fraction(1)", "2001-02-03 04:05:06.7", datetime(2001, 2, 3, 4, 5, 6, 700000)),
        ("year to fraction(2)", "2001-02-03 04:05:06.78", datetime(2001, 2, 3, 4, 5, 6, 780000)),
        ("year to fraction(3)", "2001-02-03 04:05:06.789", datetime(2001, 2, 3, 4, 5, 6, 789000)),
        ("year to fraction(4)", "2001-02-03 04:05:06.7891", datetime(2001, 2, 3, 4, 5, 6, 789100)),
        ("year to fraction(5)", "2001-02-03 04:05:06.78912", datetime(2001, 2, 3, 4, 5, 6, 789120)),
        ("month to minute", "02-03 04:05", datetime(1200, 2, 3, 4, 5)),
    )
    for precision, value, expected in cases:
        cursor.execute(f"create table t1 (dt datetime {precision})")
        cursor.execute("insert into t1 values (?)", value)
        value = cursor.execute("select dt from t1").fetchval()
        assert value == expected
        cursor.execute("drop table t1")
    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("create table t1 (dt datetime year to fraction(6)")


def test_decimal(cursor: pyodbc.Cursor):
    """Test the Informix DECIMAL type (for which NUMERIC is an alias)"""

    typenames = "decimal", "numeric"
    params = [Decimal(n) for n in "-1000.10 -1234.56 -1 0 1 1000.10 1234.56 100010 123456789.21".split()]
    params.append(None)
    for alias in typenames:

        # If you specify a scale, you get the true ANSI SQL DECIMAL/NUMERIC type.
        cursor.execute(f"create table t1 (val {alias}(20,6))")
        for param in params:
            cursor.execute("truncate table t1")
            cursor.commit()
            cursor.execute("insert into t1 values (?)", param)
            result = cursor.execute("select val from t1").fetchval()
            assert result == param

        # If you leave off the scale, you get Informix's "floating" decimal type if not in ANSI-compliant mode.
        try:
            dbname = cursor.execute("SELECT DBINFO('dbname') FROM systables WHERE tabid = 1").fetchval().strip()
            is_ansi = cursor.execute("SELECT is_ansi FROM sysmaster:sysdatabases WHERE name = ?", dbname).fetchval()
        except:
            # We could skip the rest of the test altogether, but let's go with the default setting.
            is_ansi = False
        cursor.execute("drop table t1")
        cursor.execute(f"create table t1 (id int, val {alias}(5))")
        cursor.execute("insert into t1 values (?, ?)", (1, "12345"))
        cursor.execute("insert into t1 values (?, ?)", (2, "1.2345"))
        cursor.execute("insert into t1 values (?, ?)", (3, "12.345"))
        cursor.execute("insert into t1 values (?, ?)", (4, "123.45"))
        cursor.execute("insert into t1 values (?, ?)", (5, "1234.5"))
        if not is_ansi:
            cursor.execute("insert into t1 values (?, ?)", (6, "123456789"))
            cursor.execute("insert into t1 values (?, ?)", (7, ".123456789"))
        rows = cursor.execute("select val from t1 order by id").fetchall()
        if is_ansi:
            expected = [Decimal("12345"), Decimal("1"), Decimal("12"), Decimal("123"), Decimal("1235")]
        else:
            expected = [
                Decimal("12345.0"),
                Decimal("1.2345"),
                Decimal("12.345"),
                Decimal("123.45"),
                Decimal("1234.5"),
                Decimal("123460000.0"),
                Decimal("0.12346"),
            ]
        observed = [row.val for row in rows]
        assert observed == expected
        cursor.execute("drop table t1")


def test_float(cursor: pyodbc.Cursor):
    """Test the Informix FLOAT data type"""

    values = [None, -200, -1, 0, 1, 1234.5, -200, .00012345]
    cursor.execute("create table t1 (i int, f float)")
    for i, v in enumerate(values, 1):
        cursor.execute("insert into t1 values (?, ?)", (i, v))
    cursor.execute("select f from t1 order by i")
    results = [row.f for row in cursor.fetchall()]
    assert pytest.approx(results) == values


def test_integer(cursor: pyodbc.Cursor):
    """Test the Informix INTEGER type, including the reserved sentinel value"""

    for alias in ("int", "integer"):
        cursor.execute(f"create table t1 (col {alias})")
        for param in [-1, 0, 1, -2_147_483_647, 2_147_483_647]:
            cursor.execute("truncate table t1")
            cursor.commit()
            cursor.execute("insert into t1 values (?)", param)
            result = cursor.execute("select col from t1").fetchval()
            assert result == param
        reserved = 2_147_483_648
        with pytest.raises(pyodbc.Error):
            cursor.execute("insert into t1 values (?)", reserved)
        cursor.execute("drop table t1")


def test_interval(cursor: pyodbc.Cursor):
    """Test the Informix INTERVAL type

    Until https://github.com/mkleehammer/pyodbc/issues/60 is resolved, we won't be able
    to retrieve the values properly. Best we can do is retrieve the values cast to strings.
    """

    cursor.execute("create table t1 (i interval year to month, s varchar(32))")
    cursor.execute("insert into t1 (i, s) values ('3-6', 'time to market')")
    cursor.execute("insert into t1 (i, s) values (?, ?)", ("1-3", "time to obsolescence"))
    observed = [row.s for row in cursor.execute("select s from t1 order by 1").fetchall()]
    expected = ["time to market", "time to obsolescence"]
    assert observed == expected
    cursor.execute("select * from t1")
    with pytest.raises(pyodbc.ProgrammingError):
        rows = cursor.fetchall()
    cursor.execute("select cast(i as varchar(25)) as i from t1 order by s")
    observed = [row.i.strip() for row in cursor.fetchall()]
    expected = ["3-06", "1-03"]
    assert observed == expected


def test_json(cursor: pyodbc.Cursor):
    """Test the Informix JSON column type (unsupported type -115 so we use cast)"""

    people = [
        {"name": {"given": "Jim", "family": "Flynn"}, "age": 29, "cars": ["Dodge", "Olds"]},
        {"name": {"given": "Penélope", "family": "Cruz"}, "age": 32, "cars": ["Chevy"]},
        {"name": {"given": "Günther", "family": "Schmidt"}, "age": 19, "cars": ["Toyota", "Ford"]},
    ]
    cursor.execute("create table t1 (id int, person json)")
    for i, person in enumerate(people, 1):
        cursor.execute("insert into t1 values(?, ?)", (i, dumps(person)))
    cursor.execute("select person::json::lvarchar(8192) as person from t1 order by id")
    results = [loads(row.person) for row in cursor.fetchall()]
    assert results == people


def test_list(cursor: pyodbc.Cursor):
    """Test the Informix LIST data type

    This type (SQL type -107) is not yet directly supported, so we use a cast.
    """

    cursor.execute("create table t1 (name varchar(50), sales list(money not null))")
    cursor.execute("insert into t1 values (?, ?)", ("Jenny Chow", "list{587900, 600000}"));
    cursor.execute("select * from t1")
    with pytest.raises(pyodbc.ProgrammingError):
        rows = cursor.fetchall()
    cursor.execute("select name, cast(sales as lvarchar(255)) from t1")
    observed = tuple(cursor.fetchone())
    expected = "Jenny Chow", "LIST{'$587900.00','$600000.00'}"
    assert observed == expected


def test_lvarchar(cursor: pyodbc.Cursor):
    """Test the Informix LVARCHAR type"""

    lengths = 10, 100, 255, 32739
    table = "t1"
    for length in lengths:
        value = _generate_str(length)
        cursor.execute(f"create table {table} (val lvarchar({length}))")
        cursor.execute(f"insert into {table} values (?)", value)
        cursor.execute(f"select val from {table}")
        result = cursor.fetchval()
        assert result == value
        cursor.execute(f"drop table {table}")
    with pytest.raises(pyodbc.Error):
        cursor.execute(f"create table {table} (val character varying(32740))")


def test_money(cursor: pyodbc.Cursor):
    """Test the Informix MONEY data type"""

    cursor.execute("create table t1 (m money)")
    params = [Decimal(n) for n in "-1000.10 -1234.56 -1 0 1 1000.10 1234.56 100010 123456789.21".split()]
    params.append(None)
    for param in params:
        cursor.execute("truncate table t1")
        cursor.commit()
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select m from t1").fetchval()
        assert result == param


def test_multiset(cursor: pyodbc.Cursor):
    """Test the Informix MULTISET data type

    This type (SQL type -107) is not yet directly supported, so we use a cast.
    """

    cursor.execute("create table t1 (id int, colors multiset(varchar(32) not null))")
    cursor.execute("insert into t1 values (?, ?)", (1, "multiset{'blue', 'green', 'yellow'}"));
    cursor.execute("insert into t1 values (?, ?)", (2, "multiset{'blue', 'green', 'yellow', 'blue'}"));
    cursor.execute("select * from t1")
    with pytest.raises(pyodbc.ProgrammingError):
        rows = cursor.fetchall()
    cursor.execute("select cast(colors as lvarchar(255)) as colors from t1 order by id")
    expected = ["MULTISET{'blue','green','yellow'}", "MULTISET{'blue','green','yellow','blue'}"]
    observed = [row.colors for row in cursor.fetchall()]
    assert observed == expected


def test_nchar(cursor: pyodbc.Cursor):
    """Test the Informix NCHAR type"""

    # Careful, Informix counts bytes, not characters!
    table = "t1"
    value = "我的"
    cursor.execute(f"create table {table} (val nchar(4))")
    cursor.execute(f"insert into {table} values (?)", value)
    stored = cursor.execute(f"select * from {table}").fetchval()
    assert stored != value  # Informix silently truncates the value
    lengths = 10, 100, 1000, 4000, 32767
    cursor.execute(f"drop table {table}")
    for length in lengths:
        value = _generate_str(length)
        cursor.execute(f"create table {table} (val char({length}))")
        cursor.execute(f"insert into {table} values (?)", value)
        cursor.execute(f"insert into {table} values (?)", None)
        cursor.execute(f"select val from {table} where val is not null")
        result = cursor.fetchval()
        assert result == value
        cursor.execute(f"drop table {table}")
    with pytest.raises(pyodbc.Error):
        cursor.execute(f"create table {table} (val nchar(32768))")


def test_nvarchar(cursor: pyodbc.Cursor):
    """Test the Informix NVARCHAR type"""

    lengths = 10, 100, 255
    table = "t1"
    for length in lengths:
        value = _generate_str(length)
        cursor.execute(f"create table {table} (val nvarchar({length}))")
        cursor.execute(f"insert into {table} values (?)", value)
        cursor.execute(f"select val from {table}")
        result = cursor.fetchval()
        assert result == value
        cursor.execute(f"drop table {table}")
    with pytest.raises(pyodbc.Error):
        cursor.execute(f"create table {table} (val nvarchar(256))")


def test_real(cursor: pyodbc.Cursor):
    """Test the Informix REAL data type (synonym for SMALLFLOAT)"""

    values = [None, -200, -1, 0, 1, 1234.5, -200, .00012345]
    for typename in ("real", "smallfloat"):
        cursor.execute(f"create table t1 (i int, f {typename})")
        for i, v in enumerate(values, 1):
            cursor.execute("insert into t1 values (?, ?)", (i, v))
        cursor.execute("select f from t1 order by i")
        results = [row.f for row in cursor.fetchall()]
        assert pytest.approx(results) == values
        cursor.execute(f"drop table t1")


def test_row(cursor: pyodbc.Cursor):
    """Test the Informix ROW data type (SQL type -105, not yet directly supported)"""

    # Named ROW type
    values = [(1, "Joe", "123 Main", "Our Town"), (2, "Ann", "23 Skidoo", "Kalamazoo")]
    cursor.execute("create row type address_t (street varchar(20), city varchar(20))")
    cursor.execute("create table t1 (id int, name varchar(20), address address_t)")
    for id, name, street, city in values:
        cursor.execute("insert into t1 values (?, ?, ?)", (id, name, f"row('{street}', '{city}')"))
    cursor.execute("select * from t1")
    with pytest.raises(pyodbc.ProgrammingError):
        row = cursor.fetchall()
    cursor.execute("select id, name, address.street, address.city from t1 order by id")
    results = [tuple(row) for row in cursor.fetchall()]
    assert results == values
    cursor.execute("drop table t1")

    # Unnamed ROW type
    values = [(1, "Raul", "Kaminsky"), (2, "Esmé", "Squalor")]
    cursor.execute("create table t1 (id int, name row(given varchar(50), family varchar(50)))")
    for id, forename, surname in values:
        cursor.execute("insert into t1 values (?, ?)", (id, f"row('{forename}', '{surname}')"))
    cursor.execute("select id, name.given, name.family from t1 order by id")
    results = [tuple(row) for row in cursor.fetchall()]
    assert results == values
    cursor.execute("drop table t1")

    # Table based on named ROW type
    wettest = [
        (1, "Hawaii", 63.7),
        (2, "Louisiana", 60.1),
        (3, "Mississippi", 59),
        (4, "Alabama", 58.3),
        (5, "Florida", 54.5),
    ]
    cursor.execute("create row type annual_precipitation_t (rank int, state varchar(32), amount float)")
    cursor.execute("create table rainfall of type annual_precipitation_t")
    for values in wettest:
        cursor.execute("insert into rainfall values (?, ?, ?)", values)
    cursor.execute("select rank, state, amount from rainfall order by rank")
    results = [tuple(row) for row in cursor.fetchall()]
    assert pytest.approx(results) == wettest


def test_serial(cursor: pyodbc.Cursor):
    """Test the Informix SERIAL type"""

    cursor.execute("create table t1 (id serial, name varchar(32))")
    cursor.execute("insert into t1 (name) values (?)", "Manny")
    cursor.execute("insert into t1 (name) values (?)", "Moe")
    cursor.execute("insert into t1 (id, name) values (?, ?)", (2_147_483_647, "Jack"))
    cursor.execute("select * from t1 order by id")
    rows = [tuple(row) for row in cursor.fetchall()]
    assert rows == [(1, "Manny"), (2, "Moe"), (2_147_483_647, "Jack")]
    cursor.execute("drop table t1")
    cursor.execute("create table t1 (id serial(501), name varchar(20))")
    cursor.execute("insert into t1 (name) values (?)", "Matthew")
    cursor.execute("insert into t1 (name) values (?)", "Mark")
    cursor.execute("insert into t1 (id, name) values (?, ?)", (801, "Luke"))
    cursor.execute("insert into t1 (name) values (?)", "John")
    cursor.execute("select * from t1 order by id")
    rows = [tuple(row) for row in cursor.fetchall()]
    assert rows == [(501, "Matthew"), (502, "Mark"), (801, "Luke"), (802, "John")]


def test_serial8(cursor: pyodbc.Cursor):
    """Test the Informix SERIAL8 type, including the reserved sentinel value"""

    values = [
        (9_000_000_000_000_000_001, "Antennaria neglecta"),
        (9_000_000_000_000_000_002, "Antennaria plantaginifolia"),
        (9_000_000_000_000_000_003, "Aquilegia canadensis"),
        (9_000_000_000_000_000_004, "Arisaema triphyllum"),
    ]
    cursor.execute("create table t1 (id serial8(9000000000000000001), flower varchar(50))")
    for _, flower in values:
        cursor.execute("insert into t1 (flower) values (?)", flower)
    cursor.execute("select * from t1 order by 1")
    results = [tuple(row) for row in cursor.fetchall()]
    assert results == values


def test_set(cursor: pyodbc.Cursor):
    """Test the Informix SET data type

    This type (SQL type -108) is not yet directly supported, so we use a cast.
    The semantics for this type match Python's (duplicate values are eliminated).
    """

    cursor.execute("create table t1 (id int, colors set(varchar(32) not null))")
    cursor.execute("insert into t1 values (?, ?)", (1, "set{'blue', 'green', 'yellow'}"));
    cursor.execute("insert into t1 values (?, ?)", (2, "set{'blue', 'green', 'yellow', 'blue'}"));
    cursor.execute("select * from t1")
    with pytest.raises(pyodbc.ProgrammingError):
        rows = cursor.fetchall()
    cursor.execute("select cast(colors as lvarchar(255)) as colors from t1 order by id")
    expected = ["SET{'blue','green','yellow'}", "SET{'blue','green','yellow'}"]
    observed = [row.colors for row in cursor.fetchall()]
    assert observed == expected


def test_smallint(cursor: pyodbc.Cursor):
    """Test the Informix SMALLINT type, including the reserved sentinel value"""

    cursor.execute("create table t1 (col smallint)")
    for param in [-1, 0, 1, -32_767, 32_767]:
        cursor.execute("truncate table t1")
        cursor.commit()
        cursor.execute("insert into t1 values (?)", param)
        result = cursor.execute("select col from t1").fetchval()
        assert result == param
    reserved = -32_768
    with pytest.raises(pyodbc.Error):
        cursor.execute("insert into t1 values (?)", reserved)


def test_text(cursor: pyodbc.Cursor):
    """Test the TEXT data type

    We can get the data into the table, but we can't fetch it using ODBC (isql hangs,
    so it's not a problem with pyodbc). By running this test by itself (thereby leaving
    the data committed) we can confirm the data in the table is what it should be using
    IBM's dbaccess client. Best we can do here in this test is fetch and confirm the
    expected value lengths (in bytes). We can't even do a "select count(*) ... where
    v = ?" because "Blobs are not allowed in this expression (-615)."

    This is an ancient data type, and it works more like a BLOB than what the name implies
    (we have to pass the parameter value as bytes instead of a string, otherwise we get
    "Illegal attempt to convert Text/Byte blob type (-608)").

    There is a function provided by Informix for converting large objects to strings,
    but when I tested it values over 2048 bytes long were truncated (even though the
    documentation claims truncation doesn't kick in until 32000 bytes). You can cast
    the text value to a smart character large object (CLOB) and get it out via the file
    system. See what we're doing with test_blob() and test_clob() above.

    TODO: figure out if the bugs are in the driver manager or the ODBC driver.
    """

    cursor.execute("create table t1 (v text)")
    lengths = None, 0, 100, 1000, 4000
    values = [_generate_str(length, encoding="utf-8") for length in lengths]
    for value in values:
        cursor.execute("insert into t1 values (?)", value)
    cursor.commit()
    cursor.execute("select length(v) as vlen from t1 where v is not null order by 1")
    observed_lengths = [row.vlen for row in cursor.fetchall()]
    expected_lengths = sorted([length for length in lengths if length is not None])
    assert observed_lengths == expected_lengths


def test_uuid(cursor: pyodbc.Cursor):
    """Test creation of UUID

    Informix does not have a UUID column type, but it does provide a way to generate the values.
    """

    cursor.execute("""\
        create function make_uuid() returning char(36)
            external name 'com.informix.judrs.IfxStrings.getUUID()'
            language java;
        grant execute on function make_uuid() to public;
    """)
    cursor.execute("create table t1 (uuid char(36), title varchar(255))")
    cursor.execute("insert into t1 values (make_uuid(), 'Count of Monte Cristo')")
    row = cursor.execute("select * from t1").fetchone()
    assert row.title == "Count of Monte Cristo"
    assert len(row.uuid) == 36
    uuid = UUID(row.uuid)
    assert type(uuid) is UUID
    assert str(uuid).lower() == row.uuid.lower()


def test_varchar(cursor: pyodbc.Cursor):
    """Test the Informix CHARACTER VARYING (a.k.a. VARCHAR) type"""

    lengths = 10, 100, 255
    table = "t1"
    for typename in ("character varying", "varchar"):
        for length in lengths:
            value = _generate_str(length)
            cursor.execute(f"create table {table} (val {typename}({length}))")
            cursor.execute(f"insert into {table} values (?)", value)
            cursor.execute(f"select val from {table}")
            result = cursor.fetchval()
            assert result == value
            cursor.execute(f"drop table {table}")
        with pytest.raises(pyodbc.Error):
            cursor.execute(f"create table {table} (val {typename}(256))")


#----------------------------------------------------------------------
# User-defined functions and procedures
#----------------------------------------------------------------------

def test_create_function(cursor: pyodbc.Cursor):
    """Test creating and invoking a custom function"""

    cursor.execute("drop function if exists add_numbers")
    cursor.execute("""\
        create function add_numbers(a real, b real)
        returning real
            return a + b;
        end function
    """)
    cursor.execute("create table t1 (id int, r1 real, r2 real)")
    values = [(-2, 1.1), (42, 43.3333), (1234.56789, -1234.56789), (3.14, 1.414)]
    expected = pytest.approx([(a + b) for a, b in values])
    for i, pair in enumerate(values, 1):
        cursor.execute("insert into t1 values (?, ?, ?)", (i, *pair))
    cursor.execute("select add_numbers(r1, r2) as s from t1 order by id")
    observed = [row.s for row in cursor]
    assert observed == expected


def test_create_procedure(cursor: pyodbc.Cursor):
    """Test creating and invoke a custom stored procedure"""

    cursor.execute("create table t1 (name varchar(100), value int)")
    cursor.execute("drop procedure if exists insert_t1")
    cursor.execute("""\
        create procedure insert_t1(p_name varchar(100), p_value int)
            insert into t1 (name, value) values (p_name, p_value);
        end procedure;
    """)
    values = [("Kathy", 98), ("Abdul", 86), ("Mac", 99), ("Rolf", 75)]
    for n, v in values:
        cursor.execute("execute procedure insert_t1(?, ?)", (n, v))
    cursor.execute("select value, name from t1 order by value")
    observed = [tuple(row) for row in cursor.fetchall()]
    expected = [(75, "Rolf"), (86, "Abdul"), (98, "Kathy"), (99, "Mac")]
    assert observed == expected


#----------------------------------------------------------------------
# Module-level properties and methods
#----------------------------------------------------------------------

def test_datasources():
    """Confirm that the dataSources() method is implemented and returns a list"""

    p = pyodbc.dataSources()
    assert isinstance(p, dict)


def test_drivers():
    """Confirm that the drivers() method is implemented and returns a list"""

    p = pyodbc.drivers()
    assert isinstance(p, list)


def test_lower_case(cursor: pyodbc.Cursor):
    """Verify that  pyodbc.lowercase forces returned column names to lowercase."""

    passed = False
    try:
        pyodbc.lowercase = True
        cursor.execute("create table t1 (Abc int, dEf int)")
        cursor.execute("select * from t1")
        names = {col[0] for col in cursor.description}
        assert names == {"abc", "def"}
        passed = True
    finally:
        pyodbc.lowercase = False
    assert passed


def test_version():
    """Verify that we get a well-formed version string"""
    assert len(pyodbc.version.split('.')) == 3


#----------------------------------------------------------------------
# Connection methods and properties
#----------------------------------------------------------------------

def test_autocommit(cursor: pyodbc.Cursor):
    """Verify that autocommit properties for different connections are independent of each other"""

    assert cursor.connection.autocommit is False
    othercnxn = connect(autocommit=True)
    assert othercnxn.autocommit is True
    othercnxn.autocommit = False
    assert othercnxn.autocommit is False


def test_cnxn_execute_error(cursor: pyodbc.Cursor):
    """Make sure that Connection.execute (not Cursor) errors are not "eaten"

    See GitHub issue #74
    """
    cursor.execute("create table t1(a int primary key)")
    cursor.execute("insert into t1 values (1)")
    with pytest.raises(pyodbc.Error):
        cursor.connection.execute("insert into t1 values (1)")


def test_cnxn_set_attr(cursor: pyodbc.Cursor):
    """We can't test more than that this doesn't crash

    The values are from the unixODBC sqlext.h header file.
    """

    SQL_ATTR_ACCESS_MODE = 101
    SQL_MODE_READ_ONLY   = 1
    cursor.connection.set_attr(SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_ONLY)


def test_cnxn_set_attr_before():
    """We can't test more than that this doesn't crash

    The value is from the unixODBC sqlext.h header file.
    """

    SQL_ATTR_PACKET_SIZE = 112
    _cnxn = connect(attrs_before={ SQL_ATTR_PACKET_SIZE : 1024 * 32 })


def test_getinfo_bool(cursor: pyodbc.Cursor):
    """Test getting an ODBC Boolean attribute"""

    value = cursor.connection.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
    assert isinstance(value, bool)


def test_getinfo_int(cursor: pyodbc.Cursor):
    """Test getting an ODBC integer attribute"""

    value = cursor.connection.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
    assert isinstance(value, int)


def test_getinfo_smallint(cursor: pyodbc.Cursor):
    """Test getting an ODBC small integer attribute"""

    value = cursor.connection.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
    assert isinstance(value, int)


def test_getinfo_string(cursor: pyodbc.Cursor):
    """Test getting an ODBC string attribute"""

    value = cursor.connection.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
    assert isinstance(value, str)


def test_output_conversion(cursor: pyodbc.Cursor):
    """Make sure output converters are invoked

    Note the use of SQL_VARCHAR, not SQL_WVARCHAR.
    """

    def convert(value):
        # The value is the raw bytes (as a bytes object) read from the
        # database.  We'll simply add an X at the beginning at the end.
        return "X" + value.decode("latin1") + "X"

    cursor.execute("create table t1(n int, v varchar(10))")
    cursor.execute("insert into t1 values (1, '123.45')")

    cursor.connection.add_output_converter(pyodbc.SQL_VARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "X123.45X"

    # Clear all conversions and try again.  There should be no Xs this time.
    cursor.connection.clear_output_converters()
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "123.45"

    # Same but clear using remove_output_converter.
    cursor.connection.add_output_converter(pyodbc.SQL_VARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "X123.45X"

    cursor.connection.remove_output_converter(pyodbc.SQL_VARCHAR)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "123.45"

    # And lastly, clear by passing None for the converter.
    cursor.connection.add_output_converter(pyodbc.SQL_VARCHAR, convert)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "X123.45X"

    cursor.connection.add_output_converter(pyodbc.SQL_VARCHAR, None)
    value = cursor.execute("select v from t1").fetchone()[0]
    assert value == "123.45"


#----------------------------------------------------------------------
# Cursor methods and properties
#----------------------------------------------------------------------

def test_cancel(cursor: pyodbc.Cursor):
    """No reliable way to cause things to hang, so just make sure nothing blows up"""

    cursor.execute("select 1")
    cursor.cancel()


def test_close_cnxn(cursor: pyodbc.Cursor):
    """Make sure using a Cursor after closing its connection fails."""

    cursor.execute("create table t1 (id integer, s varchar(20))")
    cursor.execute("insert into t1 values (?,?)", 1, 'test')
    cursor.execute("select * from t1")

    cursor.connection.close()

    # Now that the connection is closed, we expect an exception.  (If the code attempts to use
    # the HSTMT, we'll get an access violation instead.)

    with pytest.raises(pyodbc.ProgrammingError):
        cursor.execute("select * from t1")


def test_columns(cursor: pyodbc.Cursor):
    """Make sure the driver handles SQLColumnsW() correctly"""

    expected = {
        "a": {"type_name": "INTEGER", "column_size": 10, "buffer_length": 4},
        "b": {"type_name": "VARCHAR", "column_size":  3, "buffer_length": 3},
        "ώ": {"type_name": "VARCHAR", "column_size":  4, "buffer_length": 4},
    }
    cursor.execute("create table t1(a int, b varchar(3), ώ varchar(4))")
    cursor.columns("t1")
    observed = {row.column_name: row for row in cursor}
    for name, column in observed.items():
        for key in expected[name]:
            assert getattr(column, key) == expected[name][key]
    cursor.columns("t1", schema=None, catalog=None)
    observed = {row.column_name: row for row in cursor}
    for name, column in observed.items():
        for key in expected[name]:
            assert getattr(column, key) == expected[name][key]


def test_exc_integrity(cursor: pyodbc.Cursor):
    """Make sure an IntegretyError is raised

    This is really making sure we are properly encoding and comparing the SQLSTATEs.
    """
    cursor.execute("create table t1(s1 varchar(10) primary key)")
    cursor.execute("insert into t1 values ('one')")
    with pytest.raises(pyodbc.IntegrityError):
        cursor.execute("insert into t1 values ('one')")


def test_executemany(cursor: pyodbc.Cursor):
    """Make sure executemany() works correctly"""

    # Without fast_executemany
    params = [(i, str(i)) for i in range(1, 6)]
    cursor.execute("create table t1 (col1 int, col2 varchar(10))")
    cursor.executemany("insert into t1 (col1, col2) values (?, ?)", params)
    cursor.execute("select col1, col2 from t1 order by col1")
    results = [tuple(row) for row in cursor]
    assert results == params
    cursor.execute("drop table t1")

    # With fast_executemany
    pyodbc.fast_executemany = True
    cursor.execute("create table t1 (col1 int, col2 varchar(10))")
    cursor.executemany("insert into t1(col1, col2) values (?,?)", params)
    cursor.execute("select col1, col2 from t1 order by col1")
    results = [tuple(row) for row in cursor]
    assert results == params
    pyodbc.fast_executemany = False


def test_executemany_failure(cursor: pyodbc.Cursor):
    """Confirm that an exception is raised if one query in an executemany fails"""

    cursor.execute("create table t1(a int, b varchar(10))")
    params = [(1, 'good'), ('error', 'not an int'), (3, 'good')]
    with pytest.raises(pyodbc.Error):
        cursor.executemany("insert into t1 (a, b) value (?, ?)", params)
    rows = cursor.execute("select * from t1").fetchall()
    assert rows == []


def test_rowcount():
    """Make sure SQLRowCount() behavior complies with the specification"""

    cursor = connect().cursor()
    assert cursor.rowcount == -1
    cursor.execute("drop table if exists t1")
    cursor.execute("create table t1 (col int)")
    count = 4
    for i in range(count):
        cursor.execute("insert into t1 values (?)", i)
    cursor.execute("select * from t1")
    assert cursor.rowcount == -1
    cursor.execute("update t1 set col=col+1")
    assert cursor.rowcount == count
    cursor.execute("delete from t1")
    assert cursor.rowcount == count
    cursor.execute("delete from t1")
    assert cursor.rowcount == 0


#----------------------------------------------------------------------
# Row class properties and methods
#----------------------------------------------------------------------

def test_row_description(cursor: pyodbc.Cursor):
    """Verify that Cursor.description is accessible as Row.cursor_description"""

    cursor.execute("create table t1 (col1 int, col2 char(3))")
    cursor.execute("insert into t1 values (1, 'abc')")
    row = cursor.execute("select col1, col2 from t1").fetchone()
    assert row.cursor_description == cursor.description


def test_row_repr(cursor: pyodbc.Cursor):
    """Validate the serialization of Row objects"""

    cursor.execute("create table t1(a int, b int, c int, d int)")
    cursor.execute("insert into t1 values(1,2,3,4)")

    row = cursor.execute("select * from t1").fetchone()

    result = str(row)
    assert result == "(1, 2, 3, 4)"

    result = str(row[:-1])
    assert result == "(1, 2, 3)"

    result = str(row[:1])
    assert result == "(1,)"


def test_row_slicing(cursor: pyodbc.Cursor):
    """Confirm that the Row class implements slicing correctly"""

    cursor.execute("create table t1 (a int, b int, c int, d int)")
    cursor.execute("insert into t1 values(1,2,3,4)")
    row = cursor.execute("select * from t1").fetchone()
    assert row[:] is row
    assert row[:-1] == (1, 2, 3)
    assert row[0:4] is row


#----------------------------------------------------------------------
# Testing of pyodbc internals
#----------------------------------------------------------------------

def test_refcount_encoding():
    """Confirm we handle the reference count to `encoding` properly

    In the past we freed a borrowed reference.  This would cause a segfault.
    See https://github.com/mkleehammer/pyodbc/issues/1343.
    """

    import sys
    encoding = "utf-16le"
    count_before = sys.getrefcount(encoding)

    def _test():
        # I've moved this into a function so the exception's stack trace will be freed under
        # the covers when we leave the function.  Otherwise we'd have a 2nd reference to
        # `encoding` in the stack trace of the exception.
        try:
            cnxn = pyodbc.connect(CNXNSTR, encoding=encoding)
        except:
            pass

    for i in range(10):
        _test()

    count_after = sys.getrefcount(encoding)

    assert count_after == count_before


#----------------------------------------------------------------------
# Miscellaneous tests
#----------------------------------------------------------------------

def test_chinese(cursor: pyodbc.Cursor):
    """Can we round-trip Unicode values?"""

    cursor.execute("create table t1 (col nvarchar(10))")
    value = "我的"
    cursor.execute("insert into t1 values (?)", value)
    result = cursor.execute("select col from t1").fetchval()
    assert result == value


#----------------------------------------------------------------------
# Skipped tests
#----------------------------------------------------------------------

@pytest.mark.skip("Informix uses file-based tracing instead of messages for the caller")
def test_cursor_messages(cursor: pyodbc.Cursor):
    """Test the Cursor.messages attribute"""


@pytest.mark.skip(reason="driver returns 'An illegal character has been found in the statement (-202)' for this test")
def test_emoticons(cursor: pyodbc.Cursor):
    """Verify that the back end handles 4-byte Unicode characters correctly"""

    v = "x \U0001F31C z"
    cursor.execute("create table t1 (s varchar(100))")
    cursor.execute("insert into t1 values (?)", v)
    result = cursor.execute("select s from t1").fetchval()
    assert result == v
    cursor.execute("drop table t1")
    cursor.execute("create table t1 (s varchar(100))")
    cursor.execute(f"insert into t1 values ('{v}')")
    result = cursor.execute("select s from t1").fetchval()
    assert result == v


@pytest.mark.skip(reason="driver returns 'Communication link failure (-11010)' for this test")
def test_maxwrite(cursor: pyodbc.Cursor):
    """Ensure that setting the maxwrite property doesn't break anything"""

    cursor.connection.maxwrite = 500
    cursor.execute("create table t1 (col lvarchar(2000))")
    param = _generate_str(1000)
    cursor.execute("insert into t1 values (?)", param)
    cursor.execute("select col from t1")
    result = cursor.fetchval()
    assert result == param
