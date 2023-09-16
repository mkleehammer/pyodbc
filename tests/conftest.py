# If pyodbc has not been installed in this virtual environment, add the appropriate build
# directory.  This allows us to compile and run pytest without an install step in between
# slowing things down.  If you are going to use this, do not install pyodbc.  If you already
# have, uninstall it.
#
# This is useful for me for very fast testing, but I realize some people may not like it, so
# I'll only enable it if PYODBC_TESTLOCAL is set.

import os, sys
import importlib.machinery
from datetime import datetime
from os.path import join, dirname, abspath, getmtime


def pytest_configure(config):
    if os.environ.get('PYODBC_TESTLOCAL') != '1':
        return

    try:
        import pyodbc
        return
    except:
        _add_to_path()


def _add_to_path():
    """
    Prepends the build directory to the path so that newly built pyodbc libraries are
    used, allowing it to be tested without pip-installing it.
    """
    # look for the suffixes used in the build filenames, e.g. ".cp38-win_amd64.pyd",
    # ".cpython-38-darwin.so", ".cpython-38-x86_64-linux-gnu.so", etc.
    library_exts = [ext for ext in importlib.machinery.EXTENSION_SUFFIXES if ext != '.pyd']
    # generate the name of the pyodbc build file(s)
    library_names = [f'pyodbc{ext}' for ext in library_exts]

    # the build directory is assumed to be one directory up from this file
    build_dir = join(dirname(dirname(abspath(__file__))), 'build')

    # find all the relevant pyodbc build files, and get their modified dates
    file_info = [
        (getmtime(join(dirpath, file)), join(dirpath, file))
        for dirpath, dirs, files in os.walk(build_dir)
        for file in files
        if file in library_names
    ]
    if file_info:
        file_info.sort()  # put them in chronological order
        library_modified_dt, library_path = file_info[-1]  # use the latest one
        # add the build directory to the Python path
        sys.path.insert(0, dirname(library_path))
        print(f'Library: {library_path} (last modified {datetime.fromtimestamp(library_modified_dt)})')
    else:
        print('Did not find the pyodbc library in the build directory.  Will use the installed version.')
