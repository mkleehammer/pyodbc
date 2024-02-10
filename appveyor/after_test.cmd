IF "%APVYR_GENERATE_WHEELS%" == "true" (
  ECHO *** pip install build/wheel modules
  "%PYTHON_HOME%\python" -m pip install build wheel --quiet --no-warn-script-location
  ECHO.
  ECHO *** Generate the wheel file
  "%PYTHON_HOME%\python" -m build --wheel --no-isolation
  ECHO.
  ECHO *** \dist directory listing:
  DIR /B dist
  ECHO.
) ELSE (
  ECHO *** Skipping generation of the wheel file
  ECHO.
)
