ECHO *** Environment variables:
ECHO APPVEYOR_BUILD_FOLDER      : %APPVEYOR_BUILD_FOLDER%
ECHO APPVEYOR_BUILD_WORKER_IMAGE: %APPVEYOR_BUILD_WORKER_IMAGE%
ECHO APPVEYOR_JOB_NUMBER: %APPVEYOR_JOB_NUMBER%
ECHO APPVEYOR_JOB_ID    : %APPVEYOR_JOB_ID%
ECHO APPVEYOR_JOB_NAME  : %APPVEYOR_JOB_NAME%
ECHO APVYR_RUN_TESTS         : %APVYR_RUN_TESTS%
ECHO APVYR_RUN_MSSQL_TESTS   : %APVYR_RUN_MSSQL_TESTS%
ECHO APVYR_RUN_POSTGRES_TESTS: %APVYR_RUN_POSTGRES_TESTS%
ECHO APVYR_RUN_MYSQL_TESTS   : %APVYR_RUN_MYSQL_TESTS%
ECHO APVYR_GENERATE_WHEELS   : %APVYR_GENERATE_WHEELS%
ECHO APVYR_VERBOSE           : %APVYR_VERBOSE%
ECHO PYTHON_HOME: %PYTHON_HOME%
ECHO MSSQL_INSTANCE   : %MSSQL_INSTANCE%
ECHO MSSQL_USER       : %MSSQL_USER%
ECHO MSSQL_PASSWORD   : %MSSQL_PASSWORD%
ECHO POSTGRES_PATH    : %POSTGRES_PATH%
ECHO POSTGRES_PORT    : %POSTGRES_PORT%
ECHO POSTGRES_USER    : %POSTGRES_USER%
ECHO POSTGRES_PASSWORD: %POSTGRES_PASSWORD%
ECHO MYSQL_PATH       : %MYSQL_PATH%
ECHO MYSQL_PORT       : %MYSQL_PORT%
ECHO MYSQL_USER       : %MYSQL_USER%
ECHO MYSQL_PASSWORD   : %MYSQL_PASSWORD%

ECHO.
ECHO *** Get build info and compiler for the current Python installation:
"%PYTHON_HOME%\python" -c "import platform; print(platform.python_build(), platform.python_compiler())"

ECHO.
ECHO *** Install test requirements...
"%PYTHON_HOME%\python" -m pip install --upgrade pip setuptools --no-warn-script-location
IF ERRORLEVEL 1 (
  ECHO *** ERROR: pip/setuptools update failed
  EXIT 1
)
"%PYTHON_HOME%\python" -m pip install -r requirements-test.txt --no-warn-script-location
IF ERRORLEVEL 1 (
  ECHO *** ERROR: install test requirements failed
  EXIT 1
)
"%PYTHON_HOME%\python" -m pip freeze --all

ECHO.
ECHO *** Installing pyodbc...
SET PYTHON_ARGS=""
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=--verbose
)
"%PYTHON_HOME%\python" -m pip install %PYTHON_ARGS% .
IF ERRORLEVEL 1 (
  ECHO *** ERROR: pyodbc install failed
  EXIT 1
)

ECHO.
ECHO *** pip freeze...
"%PYTHON_HOME%\python" -m pip freeze --all

ECHO.
ECHO *** Get version of the built pyodbc module:
"%PYTHON_HOME%\python" -c "import pyodbc; print(pyodbc.version)"

ECHO.
