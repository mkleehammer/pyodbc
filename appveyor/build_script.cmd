ECHO *** Environment variables:
ECHO APPVEYOR_BUILD_FOLDER       : %APPVEYOR_BUILD_FOLDER%
ECHO APPVEYOR_BUILD_WORKER_IMAGE : %APPVEYOR_BUILD_WORKER_IMAGE%
ECHO APPVEYOR_JOB_NUMBER         : %APPVEYOR_JOB_NUMBER%
ECHO APPVEYOR_JOB_ID             : %APPVEYOR_JOB_ID%
ECHO APPVEYOR_JOB_NAME           : %APPVEYOR_JOB_NAME%
ECHO APPVEYOR_SAVE_CACHE_ON_ERROR: %APPVEYOR_SAVE_CACHE_ON_ERROR%
ECHO.
ECHO APVYR_RUN_TESTS         : %APVYR_RUN_TESTS%
ECHO APVYR_RUN_MSSQL_TESTS   : %APVYR_RUN_MSSQL_TESTS%
ECHO APVYR_RUN_POSTGRES_TESTS: %APVYR_RUN_POSTGRES_TESTS%
ECHO APVYR_RUN_MYSQL_TESTS   : %APVYR_RUN_MYSQL_TESTS%
ECHO APVYR_GENERATE_WHEELS   : %APVYR_GENERATE_WHEELS%
ECHO APVYR_VERBOSE           : %APVYR_VERBOSE%
ECHO.
ECHO PYTHON_HOME   : %PYTHON_HOME%
ECHO TEST_MSYS2   : %TEST_MSYS2%
ECHO MSSQL_INSTANCE: %MSSQL_INSTANCE%
ECHO POSTGRES_PATH : %POSTGRES_PATH%
ECHO MYSQL_PATH    : %MYSQL_PATH%

ECHO.
IF "%TEST_MSYS2%" == "true" GOTO :msys2
ECHO *** Get build info and compiler for the current Python installation:
"%PYTHON_HOME%\python" -c "import platform; print(platform.python_build(), platform.python_compiler())"

ECHO.
ECHO *** Update pip and setuptools...
"%PYTHON_HOME%\python" -m pip install --upgrade pip setuptools --quiet --no-warn-script-location
IF ERRORLEVEL 1 (
  ECHO *** ERROR: pip/setuptools update failed
  EXIT 1
)
"%PYTHON_HOME%\python" -m pip freeze --all

ECHO.
ECHO *** Installing pyodbc...
SET PYTHON_ARGS=.
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=--verbose %PYTHON_ARGS%
)
"%PYTHON_HOME%\python" -m pip install %PYTHON_ARGS%
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

GOTO :end

:msys2
ECHO *** Get build info and compiler for the current Python installation:
bash -lc "python -c ""import platform; print(platform.python_build(), platform.python_compiler())"""

ECHO.
bash -lc "python -m pip freeze --all"

ECHO.
ECHO *** Installing pyodbc...
SET PYTHON_ARGS=.
IF "%APVYR_VERBOSE%" == "true" (
  SET PYTHON_ARGS=--verbose %PYTHON_ARGS%
)
SET PIP_BREAK_SYSTEM_PACKAGES=1
bash -lc "python -m pip install %PYTHON_ARGS%"
IF ERRORLEVEL 1 (
  ECHO *** ERROR: pyodbc install failed
  EXIT 1
)

ECHO.
ECHO *** pip freeze...
bash -lc "python -m pip freeze --all"

ECHO.
ECHO *** Get version of the built pyodbc module:
bash -lc "python -c ""import pyodbc; print(pyodbc.version)"""

:end
ECHO.
