REM 0 = success, 1 = failure
SET OVERALL_RESULT=0


REM Output a list of the ODBC drivers available to pyodbc
ECHO *** Available ODBC Drivers:
"%PYTHON_HOME%\python" -c "import pyodbc; print('\n'.join(sorted(pyodbc.drivers())))"


REM check if any testing should be done at all
IF NOT "%APVYR_RUN_TESTS%" == "true" (
  ECHO.
  ECHO *** Skipping all the unit tests
  GOTO :end
)


REM Extract the bitness of the current Python interpreter
FOR /F "tokens=* USEBACKQ" %%F IN (`%PYTHON_HOME%\python -c "import sys; sys.stdout.write('64' if sys.maxsize > 2**32 else '32')"`) DO (
SET PYTHON_ARCH=%%F
)

SET TESTS_DIR=tests3


:mssql
ECHO.
ECHO ############################################################
ECHO # MS SQL Server
ECHO ############################################################
IF NOT "%APVYR_RUN_MSSQL_TESTS%" == "true" (
  ECHO *** Skipping the MS SQL Server unit tests
  GOTO :postgresql
)
ECHO *** Get MS SQL Server version:
sqlcmd -S "%MSSQL_INSTANCE%" -U sa -P "%MSSQL_PASSWORD%" -Q "SELECT @@VERSION"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect to instance
  SET OVERALL_RESULT=1
  GOTO :postgresql
)
ECHO *** Create test database
sqlcmd -S "%MSSQL_INSTANCE%" -U sa -P "%MSSQL_PASSWORD%" -Q "CREATE DATABASE test_db"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not create the test database
  SET OVERALL_RESULT=1
  GOTO :postgresql
)

:mssql1
REM Native Client 10.0 is so old, it might not be available on the server
SET DRIVER={SQL Server Native Client 10.0}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  REM Don't fail the tests if the driver can't be found
  ECHO *** INFO: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  GOTO :mssql2
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql2
REM Native Client 11.0 is so old, it might not be available on the server
SET DRIVER={SQL Server Native Client 11.0}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  REM Don't fail the tests if the driver can't be found
  ECHO *** INFO: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  GOTO :mssql3
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql3
SET DRIVER={ODBC Driver 11 for SQL Server}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :mssql4
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql4
SET DRIVER={ODBC Driver 13 for SQL Server}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :mssql5
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql5
SET DRIVER={ODBC Driver 17 for SQL Server}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :mssql6
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql6
SET DRIVER={ODBC Driver 18 for SQL Server}
SET CONN_STR=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=%MSSQL_USER%;PWD=%MSSQL_PASSWORD%;Encrypt=Optional;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :postgresql
)
SET PYTHON_ARGS=--sqlserver "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1


:postgresql
REM TODO: create a separate database for the tests?
ECHO.
ECHO ############################################################
ECHO # PostgreSQL
ECHO ############################################################
IF NOT "%APVYR_RUN_POSTGRES_TESTS%" == "true" (
  ECHO *** Skipping the PostgreSQL unit tests
  GOTO :mysql
)
ECHO *** Get PostgreSQL version
SET PGPASSWORD=%POSTGRES_PASSWORD%
"%POSTGRES_PATH%\bin\psql" -U %POSTGRES_USER% -d postgres -c "SELECT version()"

IF %PYTHON_ARCH% EQU 32 (
  SET DRIVER={PostgreSQL Unicode}
) ELSE (
  SET DRIVER={PostgreSQL Unicode^(x64^)}
)
SET CONN_STR=Driver=%DRIVER%;Server=localhost;Port=%POSTGRES_PORT%;Database=postgres;Uid=%POSTGRES_USER%;Pwd=%POSTGRES_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :mysql
)
SET PYTHON_ARGS=--postgresql "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1


:mysql
REM TODO: create a separate database for the tests?  (with the right collation)
REM       https://dev.mysql.com/doc/refman/5.7/en/charset-charsets.html
REM       e.g. CREATE DATABASE test_db CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
ECHO.
ECHO ############################################################
ECHO # MySQL
ECHO ############################################################
IF NOT "%APVYR_RUN_MYSQL_TESTS%" == "true" (
  ECHO *** Skipping the MySQL unit tests
  GOTO :end
)
ECHO *** Get MySQL version
"%MYSQL_PATH%\bin\mysql" -u %MYSQL_USER% -p%MYSQL_PASSWORD% -e "STATUS"

:mysql1
SET DRIVER={MySQL ODBC 8.0 ANSI Driver}
SET CONN_STR=Driver=%DRIVER%;Charset=utf8mb4;Server=localhost;Port=%MYSQL_PORT%;Database=mysql;Uid=%MYSQL_USER%;Pwd=%MYSQL_PASSWORD%;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%CONN_STR%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%CONN_STR%"
  SET OVERALL_RESULT=1
  GOTO :end
)
SET PYTHON_ARGS=--mysql "%CONN_STR:"=\"%"
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
"%PYTHON_HOME%\python" "%TESTS_DIR%\run_tests.py" %PYTHON_ARGS%
IF ERRORLEVEL 1 SET OVERALL_RESULT=1


:end
ECHO.
EXIT /B %OVERALL_RESULT%
