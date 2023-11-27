ECHO *** pip install pytest and other dev requirements ***
"%PYTHON_HOME%\python" -m pip install .[qa,test] --quiet --no-warn-script-location
ECHO.
