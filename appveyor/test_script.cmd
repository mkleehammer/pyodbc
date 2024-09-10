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


FOR /F "tokens=* USEBACKQ" %%F IN (`%PYTHON_HOME%\python -c "import sys; sys.stdout.write('64' if sys.maxsize > 2**32 else '32')"`) DO (
SET PYTHON_ARCH=%%F
)


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
sqlcmd -S "%MSSQL_INSTANCE%" -U sa -P "Password12!" -Q "SELECT @@VERSION"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect to instance
  GOTO :postgresql
)
ECHO *** Create test database
sqlcmd -S "%MSSQL_INSTANCE%" -U sa -P "Password12!" -Q "CREATE DATABASE test_db"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not create the test database
  GOTO :postgresql
)


:mssql2
REM Native Client 11.0 is so old, it might not be available on the server
SET DRIVER={SQL Server Native Client 11.0}
SET PYODBC_SQLSERVER=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=sa;PWD=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_SQLSERVER%"
IF ERRORLEVEL 1 (
  REM Don't fail the tests if the driver can't be found
  ECHO *** INFO: Could not connect using the connection string:
  ECHO "%PYODBC_SQLSERVER%"
  GOTO :mssql3
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\sqlserver_test.py"
IF ERRORLEVEL 1 SET OVERALL_RESULT=1


:mssql3
SET DRIVER={ODBC Driver 11 for SQL Server}
SET PYODBC_SQLSERVER=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=sa;PWD=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_SQLSERVER%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_SQLSERVER%"
  SET OVERALL_RESULT=1
  GOTO :mssql4
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\sqlserver_test.py"
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql4
SET DRIVER={ODBC Driver 13 for SQL Server}
SET PYODBC_SQLSERVER=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=sa;PWD=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_SQLSERVER%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_SQLSERVER%"
  SET OVERALL_RESULT=1
  GOTO :mssql5
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\sqlserver_test.py"
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql5
SET DRIVER={ODBC Driver 17 for SQL Server}
SET PYODBC_SQLSERVER=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=sa;PWD=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_SQLSERVER%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_SQLSERVER%"
  SET OVERALL_RESULT=1
  GOTO :mssql6
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\sqlserver_test.py"
IF ERRORLEVEL 1 SET OVERALL_RESULT=1

:mssql6
SET DRIVER={ODBC Driver 18 for SQL Server}
SET PYODBC_SQLSERVER=Driver=%DRIVER%;Server=%MSSQL_INSTANCE%;Database=test_db;UID=sa;PWD=Password12!;Encrypt=Optional;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_SQLSERVER%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_SQLSERVER%"
  SET OVERALL_RESULT=1
  GOTO :postgresql
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\sqlserver_test.py"
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
SET PGPASSWORD=Password12!
"%POSTGRES_PATH%\bin\psql" -U postgres -d postgres -c "SELECT version()"

IF %PYTHON_ARCH% EQU 32 (
  SET DRIVER={PostgreSQL Unicode}
) ELSE (
  SET DRIVER={PostgreSQL Unicode^(x64^)}
)
SET PYODBC_POSTGRESQL=Driver=%DRIVER%;Server=localhost;Port=5432;Database=postgres;Uid=postgres;Pwd=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_POSTGRESQL%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_POSTGRESQL%"
  SET OVERALL_RESULT=1
  GOTO :mysql
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\postgresql_test.py"
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
"%MYSQL_PATH%\bin\mysql" -u root -pPassword12! -e "STATUS"

:mysql1
IF %PYTHON_ARCH% EQU 32 (
  SET DRIVER={MySQL ODBC 8.0 ANSI Driver}
) ELSE (
  SET DRIVER={MySQL ODBC 8.4 ANSI Driver}
)
SET PYODBC_MYSQL=Driver=%DRIVER%;Charset=utf8mb4;Server=localhost;Port=3306;Database=mysql;Uid=root;Pwd=Password12!;
ECHO.
ECHO *** Run tests using driver: "%DRIVER%"
"%PYTHON_HOME%\python" appveyor\test_connect.py "%PYODBC_MYSQL%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: Could not connect using the connection string:
  ECHO "%PYODBC_MYSQL%"
  SET OVERALL_RESULT=1
  GOTO :end
)
SET PYTHON_ARGS="%PYTHON_HOME%\python" -m pytest
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=%PYTHON_ARGS% --verbose
)
%PYTHON_ARGS% "tests\mysql_test.py"
IF ERRORLEVEL 1 SET OVERALL_RESULT=1


:end
ECHO.
EXIT /B %OVERALL_RESULT%
