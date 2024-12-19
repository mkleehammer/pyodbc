IF "%TEST_MSYS2%" == "true" (
  ECHO *** pacman install dev requirements ***
  bash -lc "pacman --noconfirm -Syuu"
  bash -lc "pacman --noconfirm -Syuu"
  bash -lc "pacman --noconfirm --needed -S - < ./appveyor/mingw_pkglist.txt"
) ELSE (
  ECHO *** pip install pytest and other dev requirements ***
  "%PYTHON_HOME%\python" -m pip install -r requirements-dev.txt --quiet --no-warn-script-location
)
ECHO.
