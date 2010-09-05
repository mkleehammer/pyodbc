#!/usr/bin/python

import sys, os, re
from os.path import exists, abspath, dirname, join, isdir

try:
    # Allow use of setuptools so eggs can be built.
    from setuptools.core import setup, Command
except ImportError:
    from distutils.core import setup, Command

from distutils.extension import Extension
from distutils.errors import *

OFFICIAL_BUILD = 9999

class VersionCommand(Command):

    description = "Prints the pyodbc version, determined from git"

    user_options = []

    def initialize_options(self):
        self.verbose = 0

    def finalize_options(self):
        pass

    def run(self):
        version_str, version = get_version()
        print version_str
    

def main():

    version_str, version = get_version()

    files = [ abspath(join('src', f)) for f in os.listdir('src') if f.endswith('.cpp') ]
    libraries = []

    extra_compile_args = None
    extra_link_args    = None

    if os.name == 'nt':
        libraries.append('odbc32')
        # extra_compile_args = ['/W4']
        # extra_compile_args = ['/W4', '/Zi', '/Od']
        # extra_link_args    = ['/DEBUG']

    elif os.environ.get("OS", '').lower().startswith('windows'):
        # Windows Cygwin (posix on windows)
        # OS name not windows, but still on Windows
        libraries.append('odbc32')

    elif sys.platform == 'darwin':
        # OS/X now ships with iODBC.
        libraries.append('iodbc')

    else:
        # Other posix-like: Linux, Solaris, etc.

        # Python functions take a lot of 'char *' that really should be const.  gcc complains about this *a lot*
        extra_compile_args = ['-Wno-write-strings']

        # What is the proper way to detect iODBC, MyODBC, unixODBC, etc.?
        libraries.append('odbc')

    macros = [ ('PYODBC_VERSION', version_str) ]

    # This isn't the best or right way to do this, but I don't see how someone is supposed to sanely subclass the build
    # command.
    try:
        sys.argv.remove('--assert')
        macros.append(('PYODBC_ASSERT', 1))
    except ValueError:
        pass

    try:
        sys.argv.remove('--trace')
        macros.append(('PYODBC_TRACE', 1))
    except ValueError:
        pass

    try:
        sys.argv.remove('--leak-check')
        macros.append(('PYODBC_LEAK_CHECK', 1))
    except ValueError:
        pass

    if exists('MANIFEST'):
        os.remove('MANIFEST')

    setup (name = "pyodbc",
           version = version_str,
           description = "DB API Module for ODBC",

           long_description = ('A Python DB API 2 module for ODBC. This project provides an up-to-date, '
                               'convenient interface to ODBC using native data types like datetime and decimal.'),

           maintainer       = "Michael Kleehammer",
           maintainer_email = "michael@kleehammer.com",

           ext_modules = [Extension('pyodbc', files,
                                     libraries=libraries,
                                     define_macros = macros,
                                     extra_compile_args=extra_compile_args,
                                     extra_link_args=extra_link_args
                                     )],

           classifiers = ['Development Status :: 5 - Production/Stable',
                           'Intended Audience :: Developers',
                           'Intended Audience :: System Administrators',
                           'License :: OSI Approved :: MIT License',
                           'Operating System :: Microsoft :: Windows',
                           'Operating System :: POSIX',
                           'Programming Language :: Python',
                           'Topic :: Database',
                          ],

           url = 'http://code.google.com/p/pyodbc',
           download_url = 'http://code.google.com/p/pyodbc/downloads/list',
           cmdclass = { 'version' : VersionCommand })



def get_version():
    """
    Returns the version of the product as (description, [major,minor,micro,beta]).

    If the release is official, `beta` will be 9999 (OFFICIAL_BUILD).

      1. If in a git repository, use the latest tag (git describe).
      2. If in an unzipped source directory (from setup.py sdist),
         read the version from the PKG-INFO file.
      3. Use 2.1.0.0 and complain a lot.
    """
    # My goal is to (1) provide accurate tags for official releases but (2) not have to manage tags for every test
    # release.
    #
    # Official versions are tagged using 3 numbers: major, minor, micro.  A build of a tagged version should produce
    # the version using just these pieces, such as 2.1.4.
    #
    # Unofficial versions are "working towards" the next version.  So the next unofficial build after 2.1.4 would be a
    # beta for 2.1.5.  Using 'git describe' we can find out how many changes have been made after 2.1.4 and we'll use
    # this count as the beta id (beta1, beta2, etc.)
    #
    # Since the 4 numbers are put into the Windows DLL, we want to make sure the beta versions sort *before* the
    # official, so we set the official build number to 9999, but we don't show it.

    name    = None              # branch/feature name.  Should be None for official builds.
    numbers = None              # The 4 integers that make up the version.

    # If this is a source release the version will have already been assigned and be in the PKG-INFO file.

    name, numbers = _get_version_pkginfo()

    # If not a source release, we should be in a git repository.  Look for the latest tag.

    if not numbers:
        name, numbers = _get_version_git()

    if not numbers:
        print 'WARNING: Unable to determine version.  Using 2.1.0.0'
        name, numbers = '2.1.0-unsupported', [2,1,0,0]

    return name, numbers
            

def _get_version_pkginfo():
    filename = join(dirname(abspath(__file__)), 'PKG-INFO')
    if exists(filename):
        re_ver = re.compile(r'^Version: \s+ (\d+)\.(\d+)\.(\d+) (?: -beta(\d+))?', re.VERBOSE)
        for line in open(filename):
            match = re_ver.search(line)
            if match:
                name    = line.split(':', 1)[1].strip()
                numbers = [int(n or 0) for n in match.groups()[:3]]
                numbers.append(int(match.group(4) or OFFICIAL_BUILD)) # don't use 0 as a default for build
                return name, numbers

    return None, None


def _get_version_git():
    n, result = getoutput('git describe --tags --match 2.*')
    if n:
        print 'WARNING: git describe failed with: %s %s' % (n, result)
        return None, None

    match = re.match(r'(\d+).(\d+).(\d+) (?: -(\d+)-g[0-9a-z]+)?', result, re.VERBOSE)
    if not match:
        return None, None

    numbers = [int(n or OFFICIAL_BUILD) for n in match.groups()]
    if numbers[-1] == OFFICIAL_BUILD:
        name = '%s.%s.%s' % tuple(numbers[:3])
    if numbers[-1] != OFFICIAL_BUILD:
        # This is a beta of the next micro release, so increment the micro number to reflect this.
        numbers[-2] += 1
        name = '%s.%s.%s-beta%02d' % tuple(numbers)

    n, result = getoutput('git branch')
    branch = re.search(r'\* (\w+)', result).group(1)
    if branch != 'master' and not re.match('^v\d+$', branch):
        name = branch + '-' + name

    return name, numbers



def getoutput(cmd):
    pipe = os.popen(cmd, 'r')
    text   = pipe.read().rstrip('\n')
    status = pipe.close() or 0
    return status, text

if __name__ == '__main__':
    main()
