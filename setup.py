#!/usr/bin/env python

import sys, os, re
from os.path import exists, abspath, dirname, join, isdir, relpath, expanduser

try:
    # Allow use of setuptools so eggs can be built.
    from setuptools import setup, Command
except ImportError:
    from distutils.core import setup, Command

from distutils.extension import Extension
from distutils.errors import *

if sys.hexversion >= 0x03000000:
    from configparser import ConfigParser
else:
    from ConfigParser import ConfigParser

OFFICIAL_BUILD = 9999


def _print(s):
    # Python 2/3 compatibility
    sys.stdout.write(s + '\n')


class VersionCommand(Command):

    description = "prints the pyodbc version, determined from git"

    user_options = []

    def initialize_options(self):
        self.verbose = 0

    def finalize_options(self):
        pass

    def run(self):
        version_str, _version = get_version()
        sys.stdout.write(version_str + '\n')


class TagsCommand(Command):

    description = 'runs etags'

    user_options = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        # Windows versions of etag do not seem to expand wildcards (which Unix shells normally do for Unix utilities),
        # so find all of the files ourselves.
        files = [ join('src', f) for f in os.listdir('src') if f.endswith(('.h', '.cpp')) ]
        cmd = 'etags %s' % ' '.join(files)
        return os.system(cmd)



def main():

    version_str, version = get_version()

    with open(join(dirname(abspath(__file__)), 'README.md')) as f:
        long_description = f.read()

    settings = get_compiler_settings(version_str)

    files = [ relpath(join('src', f)) for f in os.listdir('src') if f.endswith('.cpp') ]

    if exists('MANIFEST'):
        os.remove('MANIFEST')

    kwargs = {
        'name': "pyodbc",
        'version': version_str,
        'description': "DB API Module for ODBC",

        'long_description': long_description,
        'long_description_content_type': 'text/markdown',

        'maintainer':       "Michael Kleehammer",
        'maintainer_email': "michael@kleehammer.com",

        'ext_modules': [Extension('pyodbc', sorted(files), **settings)],

        'data_files': [
            ('', ['src/pyodbc.pyi'])  # places pyodbc.pyi alongside pyodbc.py in site-packages
        ],

        'license': 'MIT',

        'classifiers': ['Development Status :: 5 - Production/Stable',
                       'Intended Audience :: Developers',
                       'Intended Audience :: System Administrators',
                       'License :: OSI Approved :: MIT License',
                       'Operating System :: Microsoft :: Windows',
                       'Operating System :: POSIX',
                       'Programming Language :: Python',
                       'Programming Language :: Python :: 2',
                       'Programming Language :: Python :: 2.7',
                       'Programming Language :: Python :: 3',
                       'Programming Language :: Python :: 3.5',
                       'Programming Language :: Python :: 3.6',
                       'Programming Language :: Python :: 3.7',
                       'Programming Language :: Python :: 3.8',
                       'Topic :: Database',
                       ],

        'url': 'https://github.com/mkleehammer/pyodbc',
        'cmdclass': { 'version' : VersionCommand,
                     'tags'    : TagsCommand }
        }

    if sys.hexversion >= 0x02060000:
        kwargs['options'] = {
            'bdist_wininst': {'user_access_control' : 'auto'}
            }

    setup(**kwargs)


def get_compiler_settings(version_str):

    settings = {
        'extra_compile_args' : [],
        'extra_link_args': [],
        'libraries': [],
        'include_dirs': [],
        'define_macros' : [ ('PYODBC_VERSION', version_str) ]
    }

    # This isn't the best or right way to do this, but I don't see how someone is supposed to sanely subclass the build
    # command.
    for option in ['assert', 'trace', 'leak-check']:
        try:
            sys.argv.remove('--%s' % option)
            settings['define_macros'].append(('PYODBC_%s' % option.replace('-', '_').upper(), 1))
        except ValueError:
            pass

    if os.name == 'nt':
        settings['extra_compile_args'].extend([
            '/Wall',
            '/wd4514',          # unreference inline function removed
            '/wd4820',          # padding after struct member
            '/wd4668',          # is not defined as a preprocessor macro
            '/wd4711', # function selected for automatic inline expansion
            '/wd4100', # unreferenced formal parameter
            '/wd4127', # "conditional expression is constant" testing compilation constants
            '/wd4191', # casts to PYCFunction which doesn't have the keywords parameter
        ])

        if '--windbg' in sys.argv:
            # Used only temporarily to add some debugging flags to get better stack traces in
            # the debugger.  This is not related to building debug versions of Python which use
            # "--debug".
            sys.argv.remove('--windbg')
            settings['extra_compile_args'].extend('/Od /Ge /GS /GZ /RTC1 /Wp64 /Yd'.split())

        # Visual Studio 2019 defaults to using __CxxFrameHandler4 which is in
        # VCRUNTIME140_1.DLL which Python 3.7 and earlier are not linked to.  This requirement
        # means pyodbc will not load unless the user has installed a UCRT update.  Turn this
        # off to match the Python 3.7 settings.
        #
        # Unfortunately these are *hidden* settings.  I guess we should be glad they actually
        # made the settings.
        # https://lectem.github.io/msvc/reverse-engineering/build/2019/01/21/MSVC-hidden-flags.html

        if sys.hexversion >= 0x03050000:
            settings['extra_compile_args'].append('/d2FH4-')
            settings['extra_link_args'].append('/d2:-FH4-')

        settings['libraries'].append('odbc32')
        settings['libraries'].append('advapi32')

    elif os.environ.get("OS", '').lower().startswith('windows'):
        # Windows Cygwin (posix on windows)
        # OS name not windows, but still on Windows
        settings['libraries'].append('odbc32')

    elif sys.platform == 'darwin':
        # The latest versions of OS X no longer ship with iodbc.  Assume
        # unixODBC for now.
        settings['libraries'].append('odbc')

        # Python functions take a lot of 'char *' that really should be const.  gcc complains about this *a lot*
        settings['extra_compile_args'].extend([
            '-Wno-write-strings',
            '-Wno-deprecated-declarations'
        ])

        # Apple has decided they won't maintain the iODBC system in OS/X and has added
        # deprecation warnings in 10.8.  For now target 10.7 to eliminate the warnings.
        settings['define_macros'].append(('MAC_OS_X_VERSION_10_7',))

        # Add directories for MacPorts and Homebrew.
        dirs = ['/usr/local/include', '/opt/local/include', expanduser('~/homebrew/include')]
        settings['include_dirs'].extend(dir for dir in dirs if isdir(dir))

        # unixODBC make/install places libodbc.dylib in /usr/local/lib/ by default
        # ( also OS/X since El Capitan prevents /usr/lib from being accessed )
        settings['library_dirs'] = ['/usr/local/lib']

    else:
        # Other posix-like: Linux, Solaris, etc.

        # Python functions take a lot of 'char *' that really should be const.  gcc complains about this *a lot*
        settings['extra_compile_args'].append('-Wno-write-strings')

        cflags = os.popen('odbc_config --cflags 2>/dev/null').read().strip()
        if cflags:
            settings['extra_compile_args'].extend(cflags.split())
        ldflags = os.popen('odbc_config --libs 2>/dev/null').read().strip()
        if ldflags:
            settings['extra_link_args'].extend(ldflags.split())

        from array import array
        UNICODE_WIDTH = array('u').itemsize
#        if UNICODE_WIDTH == 4:
#            # This makes UnixODBC use UCS-4 instead of UCS-2, which works better with sizeof(wchar_t)==4.
#            # Thanks to Marc-Antoine Parent
#            settings['define_macros'].append(('SQL_WCHART_CONVERT', '1'))

        # What is the proper way to detect iODBC, MyODBC, unixODBC, etc.?
        settings['libraries'].append('odbc')

    return settings


def get_version():
    """
    Returns the version of the product as (description, [major,minor,micro,beta]).

    If the release is official, `beta` will be 9999 (OFFICIAL_BUILD).

      1. If in a git repository, use the latest tag (git describe).
      2. If in an unzipped source directory (from setup.py sdist),
         read the version from the PKG-INFO file.
      3. Use 4.0.0.0 and complain a lot.
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
        _print('WARNING: Unable to determine version.  Using 4.0.0.0')
        name, numbers = '4.0.0-unsupported', [4,0,0,0]

    return name, numbers


def _get_version_pkginfo():
    filename = join(dirname(abspath(__file__)), 'PKG-INFO')
    if exists(filename):
        re_ver = re.compile(r'^Version: \s+ (\d+)\.(\d+)\.(\d+) (?: b(\d+))?', re.VERBOSE)
        for line in open(filename):
            match = re_ver.search(line)
            if match:
                name    = line.split(':', 1)[1].strip()
                numbers = [int(n or 0) for n in match.groups()[:3]]
                numbers.append(int(match.group(4) or OFFICIAL_BUILD)) # don't use 0 as a default for build
                return name, numbers

    return None, None


def _get_version_git():
    n, result = getoutput("git describe --tags --match [0-9]*")
    if n:
        _print('WARNING: git describe failed with: %s %s' % (n, result))
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
        name = '%s.%s.%sb%d' % tuple(numbers)

    n, result = getoutput('git rev-parse --abbrev-ref HEAD')

    if result == 'HEAD':
        # We are not on a branch, so use the last revision instead
        n, result = getoutput('git rev-parse --short HEAD')
        name = name + '+commit' + result
    else:
        if result != 'master' and not re.match(r'^v\d+$', result):
            name = name + '+' + result.replace('-', '')

    return name, numbers



def getoutput(cmd):
    pipe = os.popen(cmd, 'r')
    text   = pipe.read().rstrip('\n')
    status = pipe.close() or 0
    return status, text

if __name__ == '__main__':
    main()
