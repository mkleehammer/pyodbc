:: To build extensions for 64 bit Python 2, we need to configure environment
:: variables to use the MSVC 2008 C++ compilers from GRMSDKX_EN_DVD.iso of:
:: MS Windows SDK for Windows 7 and .NET Framework 3.5 (SDK v7.0)
::
:: To build extensions for 64 bit Python 3, we need to configure environment
:: variables to use the MSVC 2010 C++ compilers from GRMSDKX_EN_DVD.iso of:
:: MS Windows SDK for Windows 7 and .NET Framework 4 (SDK v7.1)
::
:: 32 bit builds, and 64-bit builds for 3.5 and beyond, do not require specific
:: environment configurations.
::
:: Note: this script needs to be run with the /E:ON and /V:ON flags for the
:: cmd interpreter, at least for (SDK v7.0)
::
:: More details at:
:: https://github.com/cython/cython/wiki/64BitCythonExtensionsOnWindows
:: http://stackoverflow.com/a/13751649/163740
::
:: Author: Olivier Grisel
:: License: CC0 1.0 Universal: http://creativecommons.org/publicdomain/zero/1.0/
::
:: The repeated CALL commands at the end of this file look redundant, but
:: if you move them outside the IF clauses, they do not run properly in
:: the SET_SDK_64==Y case, I don't know why.
@ECHO OFF

SET COMMAND_TO_RUN=%*
SET WIN_SDK_ROOT=C:\Program Files\Microsoft SDKs\Windows
SET WIN_WDK=C:\Program Files (x86)\Windows Kits\10\Include\wdf

:: Extract the major and minor versions of the current Python interpreter, and bitness
FOR /F "tokens=* USEBACKQ" %%F IN (`%PYTHON_HOME%\python -c "import sys; sys.stdout.write(str(sys.version_info.major))"`) DO (
SET PYTHON_MAJOR_VERSION=%%F
)
FOR /F "tokens=* USEBACKQ" %%F IN (`%PYTHON_HOME%\python -c "import sys; sys.stdout.write(str(sys.version_info.minor))"`) DO (
SET PYTHON_MINOR_VERSION=%%F
)
FOR /F "tokens=* USEBACKQ" %%F IN (`%PYTHON_HOME%\python -c "import sys; sys.stdout.write('64' if sys.maxsize > 2**32 else '32')"`) DO (
SET PYTHON_ARCH=%%F
)
ECHO Inferred Python version (major, minor, arch): %PYTHON_MAJOR_VERSION% %PYTHON_MINOR_VERSION% %PYTHON_ARCH%

:: Based on the Python version, determine what SDK version to use, and whether
:: to set the SDK for 64-bit.
IF %PYTHON_MAJOR_VERSION% EQU 2 (
    SET WINDOWS_SDK_VERSION="v7.0"
    SET SET_SDK_64=Y
) ELSE (
    IF %PYTHON_MAJOR_VERSION% EQU 3 (
        SET WINDOWS_SDK_VERSION="v7.1"
        IF %PYTHON_MINOR_VERSION% LEQ 4 (
            SET SET_SDK_64=Y
        ) ELSE (
            SET SET_SDK_64=N
            IF EXIST "%WIN_WDK%" (
                :: See: https://connect.microsoft.com/VisualStudio/feedback/details/1610302/
                REN "%WIN_WDK%" 0wdf
            )
        )
    ) ELSE (
        ECHO Unsupported Python version: "%PYTHON_MAJOR_VERSION%"
        EXIT 1
    )
)

IF %PYTHON_ARCH% EQU 64 (
    IF %SET_SDK_64% == Y (
        ECHO Configuring Windows SDK %WINDOWS_SDK_VERSION% for Python %PYTHON_MAJOR_VERSION% on a 64 bit architecture
        SET DISTUTILS_USE_SDK=1
        SET MSSdk=1
        "%WIN_SDK_ROOT%\%WINDOWS_SDK_VERSION%\Setup\WindowsSdkVer.exe" -q -version:%WINDOWS_SDK_VERSION%
        "%WIN_SDK_ROOT%\%WINDOWS_SDK_VERSION%\Bin\SetEnv.cmd" /x64 /release
        ECHO Executing: %COMMAND_TO_RUN%
        CALL %COMMAND_TO_RUN% || EXIT 1
    ) ELSE (
        ECHO Using default MSVC build environment for 64 bit architecture
        ECHO Executing: %COMMAND_TO_RUN%
        CALL %COMMAND_TO_RUN% || EXIT 1
    )
) ELSE (
    ECHO Using default MSVC build environment for 32 bit architecture
    ECHO Executing: %COMMAND_TO_RUN%
    CALL %COMMAND_TO_RUN% || EXIT 1
)
