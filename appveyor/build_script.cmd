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
ECHO PYTHON        : %PYTHON%
ECHO TESTS_DIR     : %TESTS_DIR%
ECHO MSSQL_INSTANCE: %MSSQL_INSTANCE%
ECHO POSTGRES_PATH : %POSTGRES_PATH%
ECHO MYSQL_PATH    : %MYSQL_PATH%

ECHO.
ECHO *** Get build info and compiler for the current Python installation:
"%PYTHON%\python" -c "import platform; print(platform.python_build(), platform.python_compiler())"

ECHO.
ECHO *** Building the pyodbc module...
%WITH_COMPILER% "%PYTHON%\python" setup.py build

ECHO.
ECHO *** Installing pyodbc...
"%PYTHON%\python" setup.py install

ECHO.
ECHO *** Get version of the built pyodbc module:
"%PYTHON%\python" -c "import pyodbc; print(pyodbc.version)"

ECHO.
