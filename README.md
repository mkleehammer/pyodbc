# pyodbc

[![Windows Status](https://ci.appveyor.com/api/projects/status/github/mkleehammer/pyodbc?branch=master&svg=true&passingText=Windows%20build)](https://ci.appveyor.com/project/mkleehammer/pyodbc)
[![Ubuntu build](https://github.com/mkleehammer/pyodbc/actions/workflows/ubuntu_build.yml/badge.svg)](https://github.com/mkleehammer/pyodbc/actions/workflows/ubuntu_build.yml)
[![PyPI](https://img.shields.io/pypi/v/pyodbc?color=brightgreen)](https://pypi.org/project/pyodbc/)

pyodbc is an open source Python module that makes accessing ODBC databases simple.  It
implements the [DB API 2.0](https://www.python.org/dev/peps/pep-0249) specification but is
packed with even more Pythonic convenience.

The easiest way to install is to use pip:

    pip install pyodbc

Precompiled binary wheels are provided for most Python versions on Windows and macOS.  On other
operating systems this will build from source.  Note, pyodbc contains C++ extensions so you will
need a suitable C++ compiler on your computer to install pyodbc, for all operating systems.  See
the [docs](https://github.com/mkleehammer/pyodbc/wiki/Install) for details.

[Documentation](https://github.com/mkleehammer/pyodbc/wiki)

[Release Notes](https://github.com/mkleehammer/pyodbc/releases)
