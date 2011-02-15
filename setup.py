#!/usr/bin/python

import sys, os, re, platform
from os.path import exists, abspath, dirname, join, isdir
from ConfigParser import SafeConfigParser

try:
    # Allow use of setuptools so eggs can be built.
    from setuptools.core import setup, Command
except ImportError:
    from distutils.core import setup, Command

from distutils.extension import Extension
from distutils.errors import *

OFFICIAL_BUILD = 9999


class VersionCommand(Command):

    description = "prints the pyodbc version, determined from git"

    user_options = []

    def initialize_options(self):
        self.verbose = 0

    def finalize_options(self):
        pass

    def run(self):
        version_str, version = get_version()
        print version_str
    

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

    settings = get_compiler_settings(version_str)

    files = [ abspath(join('src', f)) for f in os.listdir('src') if f.endswith('.cpp') ]

    if exists('MANIFEST'):
        os.remove('MANIFEST')

    setup (name = "pyodbc",
           version = version_str,
           description = "DB API Module for ODBC",

           long_description = ('A Python DB API 2 module for ODBC. This project provides an up-to-date, '
                               'convenient interface to ODBC using native data types like datetime and decimal.'),

           maintainer       = "Michael Kleehammer",
           maintainer_email = "michael@kleehammer.com",

           ext_modules = [Extension('pyodbc', files, **settings)],

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
           cmdclass = { 'version' : VersionCommand,
                        'tags'    : TagsCommand })



def get_compiler_settings(version_str):

    settings = { 'libraries': [],
                 'define_macros' : [ ('PYODBC_VERSION', version_str) ] }

    # This isn't the best or right way to do this, but I don't see how someone is supposed to sanely subclass the build
    # command.
    for option in ['assert', 'trace', 'leak-check']:
        try:
            sys.argv.remove('--%s' % option)
            settings['define_macros'].append(('PYODBC_%s' % option.replace('-', '_'), 1))
        except ValueError:
            pass

    if os.name == 'nt':
        settings['libraries'].append('odbc32')
        settings['libraries'].append('advapi32')

    elif os.environ.get("OS", '').lower().startswith('windows'):
        # Windows Cygwin (posix on windows)
        # OS name not windows, but still on Windows
        settings['libraries'].append('odbc32')

    elif sys.platform == 'darwin':
        # OS/X now ships with iODBC.
        settings['libraries'].append('iodbc')

    elif sys.platform.startswith('freebsd'):
        try:
            include = '-I'+os.environ['PREFIX']+'/include'
            lib = '-L'+os.environ['PREFIX']+'/lib'
        except:
            include = '-I/usr/local/include'
            lib = '-L/usr/local/lib'

        settings['extra_compile_args'] = ['-Wno-write-strings', include, lib]
        settings['extra_link_args'] = [ lib ]
        settings['libraries'].append('odbc')

    else:
        # Other posix-like: Linux, Solaris, etc.

        # Python functions take a lot of 'char *' that really should be const.  gcc complains about this *a lot*
        settings['extra_compile_args'] = ['-Wno-write-strings']

        # What is the proper way to detect iODBC, MyODBC, unixODBC, etc.?
        settings['libraries'].append('odbc')

    get_config(settings, version_str)

    return settings


def get_config(settings, version_str):
    """
    Adds configuration macros from pyodbc.conf to the compiler settings dictionary.

    If pyodbc.conf does not exist, it will compile and run the pyodbcconf utility.

    This is similar to what autoconf provides, but only uses the infrastructure provided by Python, which is important
    for building on *nix and Windows.
    """
    filename = 'pyodbc.conf'

    # If the file exists, make sure that the version in it is the same as the version we are compiling.  Otherwise we
    # might have added configuration items that aren't there.
    if exists(filename):
        try:
            config = SafeConfigParser()
            config.read(filename)

            if (not config.has_option('define_macros', 'pyodbc_version') or
                config.get('define_macros', 'pyodbc_version') != version_str):
                print 'Recreating pyodbc.conf for new version'
                os.remove(filename)

        except:
            config = None
            # Assume the file has been corrupted.  Delete and recreate
            print 'Unable to read %s.  Recreating' % filename
            os.remove(filename)

    if not exists('pyodbc.conf'):
        # Doesn't exist, so build the pyodbcconf module and use it.

        oldargv = sys.argv
        sys.argv = [ oldargv[0], 'build' ]

        setup(name="pyodbcconf",
              ext_modules = [ Extension('pyodbcconf',
                                        [join('utils', 'pyodbcconf', 'pyodbcconf.cpp')],
                                        **settings) ])

        sys.argv = oldargv

        add_to_path()

        import pyodbcconf
        pyodbcconf.configure()

    config = SafeConfigParser()
    config.read(filename)

    for section in config.sections():
        for key, value in config.items(section):
            settings[section].append( (key.upper(), value) )



def add_to_path():
    """
    Prepends the build directory to the path so pyodbcconf can be imported without installing it.
    """
    # Now run the utility
  
    import imp
    library_exts  = [ t[0] for t in imp.get_suffixes() if t[-1] == imp.C_EXTENSION ]
    library_names = [ 'pyodbcconf%s' % ext for ext in library_exts ]
     
    # Only go into directories that match our version number. 
     
    dir_suffix = '-%s.%s' % (sys.version_info[0], sys.version_info[1])
     
    build = join(dirname(abspath(__file__)), 'build')

    for top, dirs, files in os.walk(build):
        dirs = [ d for d in dirs if d.endswith(dir_suffix) ]
        for name in library_names:
            if name in files:
                sys.path.insert(0, top)
                return
  
    raise SystemExit('Did not find pyodbcconf')


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
