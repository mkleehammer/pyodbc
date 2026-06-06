"""Test pyodbc with Excel workbooks

Ported from the old unit tests.

These are read-only tests, using tests/pyodbc_test.xlsx. Microsoft's driver
produces a corrupted Excel file when used for creating and populating tables.
"""

import pathlib
import pyodbc
import pytest

DRIVER = "{Microsoft Excel Driver (*.xls, *.xlsx, *.xlsm, *.xlsb)}"
DIRECTORY = pathlib.Path(__file__).parent
LEGACY_PATH = DIRECTORY / f"pyodbc_test.xls"
MODERN_PATH = DIRECTORY / f"pyodbc_test.xlsx"
LEGACY_CONNSTR = f"DRIVER={DRIVER};DBQ={LEGACY_PATH};Extended Properties='HDR=YES';"
MODERN_CONNSTR = f"DRIVER={DRIVER};DBQ={MODERN_PATH};Extended Properties='HDR=YES';"


#----------------------------------------------------------------------
# Helper functions
#----------------------------------------------------------------------

def connect(legacy=False):
    """Helper function to create a new Connection object"""

    connstr = LEGACY_CONNSTR if legacy else MODERN_CONNSTR
    return pyodbc.connect(connstr, autocommit=True)


@pytest.fixture
def cursors():
    """Create a pair of Cursor objects"""

    connections = connect(legacy=True), connect(legacy=False)
    cursors = [conn.cursor() for conn in connections]

    yield cursors

    for cursor in cursors:
        if not cursor.connection.closed:
            connection = cursor.connection
            cursor.close()
            connection.close()


def test_getinfo_string(cursors):
    """Confirm that we can fetch string attributes from the driver"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
        assert isinstance(value, str)


def test_getinfo_bool(cursors):
    """Confirm that we can fetch Boolean attributes from the driver"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
        assert isinstance(value, bool)


def test_getinfo_int(cursors):
    """Confirm that we can fetch integer attributes from the driver"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        assert isinstance(value, int)


def test_getinfo_smallint(cursors):
    """Confirm that we can fetch SMALLINT attributes from the driver"""

    for cursor in cursors:
        value = cursor.connection.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        assert isinstance(value, int)


def test_read_sheet(cursors):
    """Verify that we can read values from a sheet as a table"""

    for cursor in cursors:
        rows = cursor.execute("select * from [Sheet1$]").fetchall()
        assert len(rows) == 3


def test_read_range(cursors):
    """Verify that we can read values from a named range as a table"""

    for cursor in cursors:
        rows = cursor.execute("select * from Sales").fetchall()
        assert len(rows) == 4
    

def test_tables(cursors):
    """Verify that we can find out which tables are available"""

    for cursor in cursors:
        tables = [row.table_name for row in cursor.tables()]
        assert "Sheet1$" in tables
        assert "Sales" in tables


def test_join(cursors):
    """Actually the driver doesn't understand JOIN so we have to use stone age syntax"""

    for cursor in cursors:
        cursor.execute("""\
            select e.name, sum(s.sales)
            from [Sheet1$] e, Sales s
            where s.eid = e.id
            group by e.name
            order by 1"""
        )
        expected = pytest.approx([("Bart Schmidt", 397000.0), ("Sally Jones", 613567.89)])
        observed = [tuple(row) for row in cursor]
        assert observed == expected
