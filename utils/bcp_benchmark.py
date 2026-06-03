"""Standalone benchmark for the SQL Server BCP fast path.

Compares row-by-row executemany, fast_executemany, and the BCP fast path
(with and without the TABLOCK hint) for a bulk insert into a heap table.

This is a manual benchmark, not a pytest test; it lives in utils/ rather than
tests/ so it is never collected by pytest.  Run it directly:

    PYODBC_SQLSERVER="DRIVER={ODBC Driver 18 for SQL Server};SERVER=...;..." \\
        python utils/bcp_benchmark.py [bulk_rows]

The connection string is read from the PYODBC_SQLSERVER environment variable
(falling back to the DSN 'pyodbc-sqlserver'); the optional argument overrides
the number of rows used for the bulk methods.
"""

import os
import sys
from datetime import datetime
from time import perf_counter

import pyodbc

CNXNSTR = os.environ.get('PYODBC_SQLSERVER', 'DSN=pyodbc-sqlserver')

# Number of rows for the row-by-row reference (kept small) and the bulk methods.
SLOW_ROWS = 5_000
BULK_ROWS = 1_000_000
BATCH_ROWS = 20_000


def make_data(n: int) -> list[tuple]:
    return [
        (i, float(i) * 1.1, f"row {i}", datetime(2020, 1, (i % 28) + 1, 12, 0, 0, i % 1000))
        for i in range(n)
    ]


def report(label: str, n: int, secs: float) -> None:
    rate = n / secs if secs > 0 else float("inf")
    print(f"{label:<22} {n:>9,} rows in {secs:8.3f}s  ({rate:>12,.0f} rows/s)")


def run_method(cursor: pyodbc.Cursor, sql: str, data: list[tuple], *,
               fast: bool, bcp: bool, tablock: bool) -> float:
    """
    Times one executemany of `data`.  The BCP fast path declines anything it cannot store as
    fast_executemany would, so a BCP run checks the rowcount it reports to be sure it really
    took the BCP path rather than timing fast_executemany under the BCP label.
    """
    cursor.execute("truncate table bcp_benchmark")
    cursor.fast_executemany = fast
    cursor.use_bcp_fast = bcp
    if bcp:
        cursor.bcp_batch_rows = BATCH_ROWS
        cursor.bcp_tablock = tablock
    start = perf_counter()
    cursor.executemany(sql, data)
    elapsed = perf_counter() - start
    if bcp and cursor.rowcount != len(data):
        raise RuntimeError("the BCP fast path fell back to fast_executemany")
    return elapsed


def main() -> None:
    bulk_rows = int(sys.argv[1]) if len(sys.argv) > 1 else BULK_ROWS

    cnxn = pyodbc.connect(CNXNSTR, autocommit=True,
                          attrs_before={pyodbc.SQL_COPT_SS_BCP: pyodbc.SQL_BCP_ON})
    cursor = cnxn.cursor()

    rm = cursor.execute(
        "select recovery_model_desc from sys.databases where database_id = db_id()"
    ).fetchval()
    print(f"recovery model: {rm}")

    cursor.execute("drop table if exists bcp_benchmark")
    cursor.execute(
        """
        create table bcp_benchmark(
            id          int,
            value       float,
            description varchar(100),
            created_at  datetime2(7))
        """)

    sql = "insert into bcp_benchmark values (?, ?, ?, ?)"
    slow_data = make_data(SLOW_ROWS)
    bulk_data = make_data(bulk_rows)

    report("executemany", SLOW_ROWS,
           run_method(cursor, sql, slow_data, fast=False, bcp=False, tablock=False))
    report("fast_executemany", bulk_rows,
           run_method(cursor, sql, bulk_data, fast=True, bcp=False, tablock=False))
    report("BCP (no TABLOCK)", bulk_rows,
           run_method(cursor, sql, bulk_data, fast=True, bcp=True, tablock=False))
    report("BCP (TABLOCK)", bulk_rows,
           run_method(cursor, sql, bulk_data, fast=True, bcp=True, tablock=True))

    count = cursor.execute("select count(*) from bcp_benchmark").fetchval()
    print(f"final row count: {count:,} (expected {bulk_rows:,})")

    cursor.execute("drop table if exists bcp_benchmark")
    cursor.close()
    cnxn.close()


if __name__ == "__main__":
    main()
