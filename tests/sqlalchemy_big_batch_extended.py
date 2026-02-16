# benchmark_bcp.py
import time
import random
import string
import datetime as dt
from contextlib import contextmanager
from decimal import Decimal

import sqlalchemy as sa
from sqlalchemy import (
    Table, Column, Integer, BigInteger, String, Float,
    Boolean, Time, MetaData, insert, event
)
# SQL Server-specific types for precise mapping
from sqlalchemy.dialects.mssql import DATETIME2, DATETIMEOFFSET

SQL_COPT_SS_BCP = 1219
SQL_BCP_ON = 1

# ---------- CONFIG ----------
# Adjust connection string as needed
CONN_URL = (
    "mssql+pyodbc://sa:YourStrong!Passw0rd@localhost,1433/BcpTest"
    "?driver=ODBC+Driver+18+for+SQL+Server&Encrypt=no"
)
TOTAL_ROWS   = 50_000
CHUNK_ROWS   = 10_000
BCP_BATCH    = 10_000
NAME_LEN     = 20
# ----------------------------

md = MetaData()
t = Table(
    "BcpTest_extend", md,
    Column("id", Integer, nullable=False),           # SQLINT4
    Column("id_big", BigInteger, nullable=False),    # SQL_BIGINT
    Column("is_active", Boolean, nullable=False),    # BIT
    Column("name", String(50)),                      # VARCHAR
    Column("val", Float),                            # FLOAT(53)
    Column("t", Time, nullable=True),                # TIME (Python datetime.time)
    Column("d_str", String(10)),                     # DATE as string
    Column("dt_str", String(32)),                    # DATETIME as string
    Column("amount_dec_18_4", sa.DECIMAL(18, 4)),    # DECIMAL(18,4)
    Column("amount_num_38_0", sa.NUMERIC(38, 0)),    # NUMERIC(38,0)
    Column("d_native", sa.DATE),                     # SQL Server DATE
    Column("dt2_native", DATETIME2(precision=7)),    # SQL Server DATETIME2(7)
    Column("dto_native", DATETIMEOFFSET(precision=7)),  # SQL Server DATETIMEOFFSET(7)
    schema="dbo",
)

def make_engine(use_sa_fast_executemany: bool):
    return sa.create_engine(
        CONN_URL,
        connect_args={"attrs_before": {SQL_COPT_SS_BCP: SQL_BCP_ON}},
        fast_executemany=use_sa_fast_executemany,
        pool_pre_ping=True,
    )

def set_bcp_event(engine, enable_bcp: bool, bcp_batch_rows: int):
    @event.listens_for(engine, "before_cursor_execute")
    def _apply_bcp_options(conn, cursor, statement, parameters, context, executemany):
        if not executemany:
            return
        try:
            cursor.use_bcp_fast = bool(enable_bcp)
            if enable_bcp:
                cursor.bcp_batch_rows = int(bcp_batch_rows)
        except AttributeError:
            pass

@contextmanager
def timer(label):
    t0 = time.perf_counter()
    yield lambda: time.perf_counter() - t0
    dt_elapsed = time.perf_counter() - t0
    print(f"{label}: {dt_elapsed:.3f}s")

def rand_name(n=NAME_LEN):
    alph = string.ascii_letters
    return "".join(random.choice(alph) for _ in range(n))

def rand_time():
    # Random time with microseconds
    return dt.time(
        hour=random.randint(0, 23),
        minute=random.randint(0, 59),
        second=random.randint(0, 59),
        microsecond=random.randint(0, 999999),
    )

def rand_date_str():
    # 'YYYY-MM-DD'
    start = dt.date(2000, 1, 1)
    end   = dt.date(2030, 12, 31)
    days = (end - start).days
    d = start + dt.timedelta(days=random.randint(0, days))
    return d.isoformat()

def rand_datetime_str():
    # 'YYYY-MM-DD HH:MM:SS.ffffff'
    start = dt.datetime(2000, 1, 1)
    end   = dt.datetime(2030, 12, 31, 23, 59, 59, 999999)
    delta = end - start
    us_total = random.randrange(delta.days * 24 * 3600 * 1_000_000 + delta.seconds * 1_000_000 + delta.microseconds)
    d = start + dt.timedelta(microseconds=us_total)
    return d.isoformat(sep=" ")

# NEW: native generators
def rand_date():
    # Python datetime.date in a broad range SQL Server DATE supports (0001-01-01..9999-12-31)
    # Keep it reasonable for tests:
    start = dt.date(2000, 1, 1)
    end   = dt.date(2030, 12, 31)
    days = (end - start).days
    return start + dt.timedelta(days=random.randint(0, days))

def rand_datetime2():
    # Naive datetime (no tzinfo), full microseconds; SQL Server DATETIME2(7) stores 100ns
    start = dt.datetime(2000, 1, 1)
    end   = dt.datetime(2030, 12, 31, 23, 59, 59, 999999)
    delta = end - start
    us_total = random.randrange(delta.days * 24 * 3600 * 1_000_000 + delta.seconds * 1_000_000 + delta.microseconds)
    return start + dt.timedelta(microseconds=us_total)

def rand_datetimeoffset():
    # tz-aware datetime with minute-aligned UTC offset within SQL Server’s +/-14:00 bounds
    base = rand_datetime2()
    # choose offsets like -12:00 .. +14:00, minute aligned (often 15/30/45 blocks)
    hour = random.randint(-12, 14)
    minute = random.choice([0, 15, 30, 45])
    # clamp boundary cases to keep within [-14:00, +14:00] safely
    if hour == -12 and random.random() < 0.2:
        minute = 0
    if hour == 14:
        minute = 0
    sign = -1 if hour < 0 else 1
    tz = dt.timezone(dt.timedelta(hours=hour, minutes=sign*minute))
    return base.replace(tzinfo=tz)

# Fast, safe Decimal generators (stay comfortably within column precision)
def rand_decimal(precision: int, scale: int, max_int_digits: int = 12) -> Decimal:
    int_digits = max(1, min(precision - scale, max_int_digits))
    int_part = random.randint(0, 10**int_digits - 1)
    sign = -1 if random.random() < 0.2 else 1
    if scale > 0:
        frac_digits = min(scale, 6)  # keep generator fast
        frac_part = random.randint(0, 10**frac_digits - 1)
        val = Decimal(sign) * (Decimal(int_part) + (Decimal(frac_part) / (Decimal(10) ** frac_digits)))
        q = Decimal(1) / (Decimal(10) ** scale)   # quantize to exact scale
        return val.quantize(q)
    else:
        return Decimal(sign) * Decimal(int_part)

def rows_generator(start_id: int, count: int):
    big_base = 9_000_000_000  # ensure > 32-bit range sometimes
    for i in range(start_id, start_id + count):
        yield {
            "id": i,
            "id_big": big_base + i,
            "is_active": bool(i & 1),
            "name": rand_name(),
            "val": (random.random() * 100.0),
            "t": rand_time(),
            "d_str": rand_date_str(),
            "dt_str": rand_datetime_str(),
            "amount_dec_18_4": rand_decimal(18, 4),
            "amount_num_38_0": rand_decimal(38, 0),
            "d_native":   rand_date(),          # DATE
            "dt2_native": rand_datetime2(),     # DATETIME2(7)
            "dto_native": rand_datetimeoffset() # DATETIMEOFFSET(7)
        }

def ensure_table(engine):
    with engine.begin() as conn:
        conn.exec_driver_sql("""
            IF OBJECT_ID('dbo.BcpTest_extend', 'U') IS NOT NULL
                DROP TABLE dbo.BcpTest_extend;
        """)
        md.create_all(conn)
        # Optional (benchmarking):
        # conn.exec_driver_sql("ALTER DATABASE BcpTest SET RECOVERY SIMPLE;")
        # conn.exec_driver_sql("ALTER DATABASE BcpTest MODIFY FILE (NAME = BcpTest_log, SIZE = 1024MB, FILEGROWTH = 256MB);")

def bulk_insert(engine, use_bcp: bool, total_rows: int, chunk_rows: int):
    total_inserted = 0
    total_time = 0.0

    with timer(f"TOTAL {'BCP' if use_bcp else 'NORMAL'}"):
        if use_bcp:
            with engine.connect() as conn:
                conn = conn.execution_options(isolation_level="AUTOCOMMIT")
                ins = insert(t)

                remaining = total_rows
                next_id = 1
                while remaining > 0:
                    n = min(remaining, chunk_rows)
                    chunk = list(rows_generator(next_id, n))

                    t0 = time.perf_counter()
                    r = conn.execute(ins, chunk)
                    dt_chunk = time.perf_counter() - t0

                    total_time += dt_chunk
                    rc = getattr(r, "rowcount", -1)
                    total_inserted += rc if (rc is not None and rc >= 0) else n

                    done = total_rows - (remaining - n)
                    rate = n / dt_chunk if dt_chunk > 0 else float("inf")
                    print(f"Chunk {done:>9}/{total_rows} rows in {dt_chunk:.3f}s  ({rate:,.0f} rows/s)")

                    next_id += n
                    remaining -= n
        else:
            with engine.begin() as conn:
                ins = insert(t)

                remaining = total_rows
                next_id = 1
                while remaining > 0:
                    n = min(remaining, chunk_rows)
                    chunk = list(rows_generator(next_id, n))

                    t0 = time.perf_counter()
                    r = conn.execute(ins, chunk)
                    dt_chunk = time.perf_counter() - t0

                    total_time += dt_chunk
                    rc = getattr(r, "rowcount", -1)
                    total_inserted += rc if (rc is not None and rc >= 0) else n

                    done = total_rows - (remaining - n)
                    rate = n / dt_chunk if dt_chunk > 0 else float("inf")
                    print(f"Chunk {done:>9}/{total_rows} rows in {dt_chunk:.3f}s  ({rate:,.0f} rows/s)")

                    next_id += n
                    remaining -= n

    rows_per_s = total_rows / total_time if total_time > 0 else float("inf")
    print(
        f"\nSummary [{'BCP' if use_bcp else 'NORMAL'}]: "
        f"{total_rows:,} rows in {total_time:.3f}s  →  {rows_per_s:,.0f} rows/s  "
        f"(reported inserted: {total_inserted:,})\n"
    )

def verify_count(engine, expected: int):
    with engine.begin() as conn:
        c = conn.exec_driver_sql("SELECT COUNT(*) FROM dbo.BcpTest_extend;").scalar()
        print(f"Verify table count: {c:,} (expected {expected:,})")
        return c

def main():
    # NORMAL
    eng_normal = make_engine(use_sa_fast_executemany=False)
    ensure_table(eng_normal)
    bulk_insert(eng_normal, use_bcp=False, total_rows=TOTAL_ROWS, chunk_rows=CHUNK_ROWS)
    verify_count(eng_normal, TOTAL_ROWS)

    # reset
    with eng_normal.begin() as conn:
        conn.exec_driver_sql("TRUNCATE TABLE dbo.BcpTest_extend;")

    # BCP fast path
    eng_bcp = sa.create_engine(
        CONN_URL,
        connect_args={"attrs_before": {SQL_COPT_SS_BCP: SQL_BCP_ON}},
        fast_executemany=True,
        isolation_level="AUTOCOMMIT",
        pool_pre_ping=True,
    )
    set_bcp_event(eng_bcp, enable_bcp=True, bcp_batch_rows=BCP_BATCH)
    ensure_table(eng_bcp)
    bulk_insert(eng_bcp, use_bcp=True, total_rows=TOTAL_ROWS, chunk_rows=CHUNK_ROWS)
    verify_count(eng_bcp, TOTAL_ROWS)

if __name__ == "__main__":
    main()
