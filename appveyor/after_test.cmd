IF "%APVYR_GENERATE_WHEELS%" == "true" (
  ECHO *** pip install the "wheel" module
  "%PYTHON_HOME%\python" -m pip install wheel --quiet --no-warn-script-location
  ECHO.
  ECHO *** Generate the wheel file
  %WITH_COMPILER% "%PYTHON_HOME%\python" setup.py bdist_wheel
  ECHO.
  ECHO *** \dist directory listing:
  DIR /B dist
) ELSE (
  ECHO *** Skipping generation of the wheel file
  ECHO.
)
