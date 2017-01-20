rem Run this from the project root.

del dist\*

for /D %%d in (venv*) do cmd /c "%%d\Scripts\activate.bat & setup clean -a bdist_wheel & deactivate"

