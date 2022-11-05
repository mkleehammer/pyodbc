from datetime import datetime
import importlib.machinery
import os
from os.path import join, dirname, abspath
import platform
import sys
import unittest


def add_to_path():
    """
    Prepends the build directory to the path so that newly built pyodbc libraries are
    used, allowing it to be tested without pip-installing it.
    """
    # look for the suffixes used in the build filenames, e.g. ".cp38-win_amd64.pyd",
    # ".cpython-38-darwin.so", ".cpython-38-x86_64-linux-gnu.so", etc.
    library_exts = [ext for ext in importlib.machinery.EXTENSION_SUFFIXES if ext != '.pyd']
    # generate the name of the pyodbc build file(s)
    library_names = ['pyodbc%s' % ext for ext in library_exts]

    # the build directory is assumed to be one directory up from this file
    build_dir = join(dirname(dirname(abspath(__file__))), 'build')

    # find all the relevant pyodbc build files, and get their modified dates
    file_info = [
        (os.path.getmtime(join(dirpath, file)), join(dirpath, file))
        for dirpath, dirs, files in os.walk(build_dir)
        for file in files
        if file in library_names
    ]
    if file_info:
        file_info.sort()  # put them in chronological order
        library_modified_dt, library_path = file_info[-1]  # use the latest one
        # add the build directory to the Python path
        sys.path.insert(0, dirname(library_path))
        print('Library: {} (last modified {})'.format(library_path, datetime.fromtimestamp(library_modified_dt)))
    else:
        print('Did not find the pyodbc library in the build directory.  Will use the installed version.')


def print_library_info(cnxn):
    import pyodbc
    print('python:  %s' % sys.version)
    print('pyodbc:  %s %s' % (pyodbc.version, os.path.abspath(pyodbc.__file__)))
    print('odbc:    %s' % cnxn.getinfo(pyodbc.SQL_ODBC_VER))
    print('driver:  %s %s' % (cnxn.getinfo(pyodbc.SQL_DRIVER_NAME), cnxn.getinfo(pyodbc.SQL_DRIVER_VER)))
    print('         supports ODBC version %s' % cnxn.getinfo(pyodbc.SQL_DRIVER_ODBC_VER))
    print('os:      %s' % platform.system())
    print('unicode: Py_Unicode=%s SQLWCHAR=%s' % (pyodbc.UNICODE_SIZE, pyodbc.SQLWCHAR_SIZE))

    cursor = cnxn.cursor()
    for typename in ['VARCHAR', 'WVARCHAR', 'BINARY']:
        t = getattr(pyodbc, 'SQL_' + typename)
        cursor.getTypeInfo(t)
        row = cursor.fetchone()
        print('Max %s = %s' % (typename, row and row[2] or '(not supported)'))

    if platform.system() == 'Windows':
        print('         %s' % ' '.join([s for s in platform.win32_ver() if s]))


def discover_and_run(top_level_dir='.', start_dir='.', pattern='test*.py', verbosity=0):
    """Finds all the test cases in the start directory and runs them"""
    tests = unittest.defaultTestLoader.discover(top_level_dir=top_level_dir, start_dir=start_dir, pattern=pattern)
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(tests)
    return result


def load_tests(testclass, name, *args):
    """
    Returns a TestSuite for tests in `testclass`.

    name
      Optional test name if you only want to run 1 test.  If not provided all tests in `testclass` will be loaded.

    args
      Arguments for the test class constructor.  These will be passed after the test method name.
    """
    if name:
        if not name.startswith('test_'):
            name = 'test_%s' % name
        names = [ name ]

    else:
        names = [ method for method in dir(testclass) if method.startswith('test_') ]

    return unittest.TestSuite([ testclass(name, *args) for name in names ])


def load_setup_connection_string(section):
    """
    Attempts to read the default connection string from the setup.cfg file.

    If the file does not exist or if it exists but does not contain the connection string, None is returned.  If the
    file exists but cannot be parsed, an exception is raised.
    """
    from os.path import exists, join, dirname
    from configparser import ConfigParser

    FILENAME = 'setup.cfg'
    KEY      = 'connection-string'

    path = dirname(abspath(__file__))
    while True:
        fqn = join(path, 'tmp', FILENAME)
        if exists(fqn):
            break
        parent = dirname(path)
        print('{} --> {}'.format(path, parent))
        if parent == path:
            return None
        path = parent

    try:
        p = ConfigParser()
        p.read(fqn)
    except:
        raise SystemExit('Unable to parse %s: %s' % (path, sys.exc_info()[1]))

    if p.has_option(section, KEY):
        return p.get(section, KEY)
