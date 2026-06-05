# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this
repository.

## What this is

pyodbc is a Python module implementing the DB API 2.0 spec on top of ODBC. It is a **C++
extension** (everything in `src/` compiles into a single `pyodbc` shared library); there is no
Python source for the runtime — `src/pyodbc.pyi` is only a type stub. The package targets
Python 3.10+ and links against an ODBC driver manager (unixODBC/iODBC on Unix, built-in on
Windows, via `odbc_config`).

## Build & test

Fast development loop (build in place, then run pytest from the repo root so the freshly built
library is on the path):

```sh
python setup.py build_ext --inplace
pytest tests/sqlite_test.py -vxk test_text          # single test by name substring
```

To debug a crash, build with tracing and pass `-s` (pytest otherwise swallows output on a
segfault):

```sh
python setup.py build_ext --inplace -D PYODBC_TRACE
pytest tests/sqlite_test.py -vxs -k test_text
```

Full multi-version test matrix uses tox (`pipx install tox`), covering py310–py314:

```sh
tox                 # all interpreters + all databases
tox -e py312        # one interpreter
tox -e py312 -- -rA # pass pytest args after --
```

Lint: `flake8` (max line length 95; see `.flake8`) and `pylint`. Install dev deps with `pip
install -r requirements-dev.txt`.

### Test database configuration

There is one test file per backend (`tests/sqlite_test.py`, `sqlserver_test.py`,
`postgresql_test.py`, `mysql_test.py`). Each reads its connection string from an environment
variable, falling back to a DSN default:

- `PYODBC_SQLITE`, `PYODBC_SQLSERVER`, `PYODBC_POSTGRESQL`, `PYODBC_MYSQL`

Set these in the shell or in a `pytest.ini` (with `pytest-env`); `tox.ini`'s header has an
example. SQLite needs no server (`driver={SQLite3 ODBC Driver};Database=:memory:`) and is the
easiest target for quick iteration. The real database must be running and the matching ODBC
driver installed before tests pass — see `.github/workflows/ubuntu_build.yml` for the exact
driver names and connection strings CI uses. `tests/old/` holds legacy, unmaintained test
scripts; ignore them.

Setting `PYODBC_TESTLOCAL=1` makes `tests/conftest.py` add the `build/` directory to
`sys.path`, so you can test a `build_ext` output without pip-installing (do not have pyodbc
installed in that environment if you use this).

## Scratch scripts vs. project tooling

Decide by **audience**, not by who happens to type the command:

- **Scripts only Claude runs** — issue-repro scripts, one-off experiments, debug helpers — go
  in **`.claude/scratch/`**. That directory is git-ignored (`/.claude/scratch/` in
  `.gitignore`) and must **never** be committed. It is local scratch space, not part of the
  project.
- **Scripts a human would ever run** (e.g. a "build + test across Python versions" convenience
  wrapper) are ordinary project tooling. They belong in **`utils/`** (alongside the existing
  `build-releases.sh`/`.cmd`), committed and reviewed like any other code with a neutral name —
  not in an "AI" directory. The moment a script has to be readable/maintained for a human, it
  is bucket two.

Before writing a human-facing test runner, note that **`tox` already builds and tests across
all supported Python versions** (see "Build & test"); a custom runner is only a thin
convenience and often unnecessary.

## Architecture

Each major ODBC object is a `PyTypeObject` defined in its own `.cpp`/`.h` pair. The object
lifecycle is **Connection → Cursor → Row**:

- **`pyodbcmodule.cpp`** — module entry point (`PyInit_pyodbc`), the `connect()` factory,
  global state (the shared `HENV`, pooling, `lowercase`, ODBC version), exception class
  definitions, and per-thread caching of exception classes via `GetClassForThread`.
- **`connection.cpp`** — the `Connection` type. Owns the `HDBC`, autocommit/transaction
  control, and the four `TextEnc` encodings used for the connection (read SQL_CHAR, read
  SQL_WCHAR, write unicode, read metadata — see "text encoding" below). Output-converter
  registration lives here.
- **`cursor.cpp`** — the `Cursor` type (the largest/most central file). Drives
  `execute`/`executemany`/`fetch*`, builds the DB API `description`, the column-name→index map
  shared with rows, and `messages`.
- **`params.cpp`** — binding Python parameters *into* SQL statements (`SQLBindParameter`), type
  detection, NULL handling via `SQLDescribeParam`, table-valued parameters (TVPs), and the
  array-binding "fast executemany" path. (Note: a header comment flags fast-executemany as
  being re-ported across the 4.x→5.x rewrite.)
- **`getdata.cpp`** — the reverse direction: converting fetched SQL column data *out* into
  Python objects, including user-defined output converters (`GetUserConvIndex`).
- **`row.cpp`** — the `Row` type: tuple-like, also supports access by column name via the
  shared name→index map, usable after the cursor/connection closes.
- **`cnxninfo.cpp`** — caches per-connection-string driver capabilities (`CnxnInfo`: ODBC
  version, whether `SQLDescribeParam` is supported, datetime precision, type max-lengths,
  `need_long_data_len`) so they aren't re-probed on every connect.
- **`errors.cpp`** — maps ODBC `SQLSTATE` codes to the DB API exception hierarchy
  (`RaiseErrorFromHandle`, `RaiseErrorV`).
- **`textenc.cpp`** / **`decimal.cpp`** — text encode/decode helpers and `Decimal` support.
- **`dbspecific.h`** — constants for non-standard driver types (SQL Server variant/XML/TIME2,
  DB2 DECFLOAT). **`wrapper.h`** — `Object`, an RAII wrapper for `PyObject*` refcounts;
  **`pyodbc.h`** — the umbrella header (platform shims, ODBC headers, `TRACE`).

### Text encoding is the subtle part

Drivers disagree wildly about Unicode, so encoding is *not* uniform. A separate `TextEnc` is
configured for reading SQL_CHAR, reading SQL_WCHAR, writing unicode strings, and **reading
metadata** (column names). Metadata gets its own encoding because PostgreSQL/MySQL return
column names as UTF-16LE from `SQLDescribeCol` regardless of connection
settings. `setencoding()`/`setdecoding()` on the Connection adjust these. The code deliberately
uses `uint16_t`/`SQLWCHAR` rather than `wchar_t` because unixODBC may define `SQLWCHAR` as
32-bit `wchar_t` while the buffer data is still 16-bit (see `HACKING.md`). When touching
encoding code, consult `notes.txt` for the ODBC length-argument rules (count-of-characters
vs. count-of-bytes).

## Versioning

The single source of truth for the version is the `version = "..."` line in
`pyproject.toml`. `setup.py` parses it with a regex (to avoid a TOML dependency on old Pythons)
and passes it to the compiler as the `PYODBC_VERSION` macro, which becomes
`pyodbc.version`. cibuildwheel also reads `pyproject.toml` directly. Bump the version there
only.
