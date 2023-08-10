# pyodbc

[![AppVeyor](https://ci.appveyor.com/api/projects/status/github/mkleehammer/pyodbc?branch=master&svg=true&passingText=Windows%20build&failingText=Windows%20build)](https://ci.appveyor.com/project/mkleehammer/pyodbc)
[![Github Actions - Ubuntu Build](https://github.com/mkleehammer/pyodbc/actions/workflows/ubuntu_build.yml/badge.svg?branch=master)](https://github.com/mkleehammer/pyodbc/actions/workflows/ubuntu_build.yml)
[![PyPI](https://img.shields.io/pypi/v/pyodbc?color=brightgreen)](https://pypi.org/project/pyodbc/)

pyodbc is an open source Python module that makes accessing ODBC databases simple.  It
implements the [DB API 2.0](https://www.python.org/dev/peps/pep-0249) specification but is
packed with even more Pythonic convenience.

The easiest way to install pyodbc is to use pip:

```bash
pip install pyodbc
```

On Macs, you should probably install unixODBC first if you don't already have an ODBC
driver manager installed, e.g. using `Homebrew`:

```bash
brew install unixodbc
pip install pyodbc
```

Similarly, on Unix you should make sure you have an ODBC driver manager installed before
installing pyodbc.  See the [docs](https://github.com/mkleehammer/pyodbc/wiki/Install)
for more information about how to do this on different Unix flavors.  (On Windows, the
ODBC driver manager is built-in.)

Precompiled binary wheels are provided for multiple Python versions on most Windows, macOS,
and Linux platforms.  On other platforms pyodbc will be built from the source code.  Note,
pyodbc contains C++ extensions so when building from source you will need a suitable C++
compiler.  See the [docs](https://github.com/mkleehammer/pyodbc/wiki/Install) for details.

[Documentation](https://github.com/mkleehammer/pyodbc/wiki)

[Release Notes](https://github.com/mkleehammer/pyodbc/releases)

IMPORTANT: Python 2.7 support is being ended.  The pyodbc 4.x versions will be the last to
support Python 2.7.  The pyodbc 5.x versions will support only Python 3.7 and above.

## Tests

You can run tests locally using `docker` or `podman`. This walk through will assume the
use of [`podman`](https://podman.io).

After installing `podman`, download an image that contains the Microsoft SQL Server for
Linux. The SQL server you use will determine the container we will download using
`podman`. The tests use both 2017 and 2019 versions of the Microsoft SQL Server. We will
focus on the 2017 version below for how to install and run tests. To run tests against
the 2019 version is the same, except you replace 2017 with 2019 in the commands below.

```bash
podman pull mcr.microsoft.com/mssql/server:2017-latest
```

We next create a new container from the image we just downloaded above using the
following command.

```bash
podman run -e "ACCEPT_EULA=Y" -e "MSSQL_SA_PASSWORD=StrongPassword2017" -p 1401:1433 --name mssql2017 --hostname mssql2017 -d mcr.microsoft.com/mssql/server:2017-latest
```

This will start an image of the container, and you can ensure it is running by executing
the following command.

```bash
podman ps
```

You should see something similar to the below output.

```bash
CONTAINER ID  IMAGE                                       COMMAND               CREATED         STATUS       PORTS                   NAMES
7fa7d8ff3124  mcr.microsoft.com/mssql/server:2017-latest  /opt/mssql/bin/sq...  1 minutes ago  Up 1 minutes  0.0.0.0:1433->1433/tcp  mssql2017
```

The container ID will be different, but you should see a running instance of the MS SQL
2017 image. We will connect to the server and create the `test` database needed to run
the tests.

```bash
podman exec mssql2017 "bash"
```

The command will place you in the image with a `bash` prompt. At the prompt, execute the
following command.

```bash
/opt/mssql-tools18/bin/sqlcmd -S localhost -U SA -P "StrongPassword2017"
```

The password is the one used in the tests, so we will use it here as well. You will now
be in the MS SQL 2017 CLI. We will create the database with the following command, where
each line is executed individually.

```SQL
CREATE DATABASE test
GO
QUIT
```

We can now run the tests for the repo.

```bash
python ./tests3/run_tests.py -vvv --sqlserver "DRIVER={ODBC Driver 17 for SQL Server};SERVER=127.0.0.1,1401;UID=sa;PWD=StrongPassword2017;DATABASE=test"
```

This will run tests for Python 3. Tests are also run for MS SQL 2019. In order to run
tests for this database, replace all `2017` with `2019` from the above instructions.
Remember to change the port forwarding from `-p 1401:1433` to something else like
`-p 1402:1433` if you are going to run both the 2017 and 2019 servers in separate
containers while running tests.
