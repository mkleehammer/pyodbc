#!/usr/bin/env python

import sys, os, shlex, re
from os.path import exists, join, isdir, relpath, expanduser
from pathlib import Path
from inspect import cleandoc

from setuptools import setup
from setuptools.extension import Extension


def _getversion():
    # CAREFUL: We need the version in this file so we can set it in a C macro to set
    # pyodbc.__version__, plus the setup function might require it.  We also need it in the
    # toml file or cibuildwheel will fail.  Instead of requiring a toml parser for older
    # versions of Python, we'll parse it out with a regexp, which is very simple.
    path = Path(__file__).parent / 'pyproject.toml'
    assert path.exists(), f'Cannot find {path}'
    text = path.read_text(encoding='utf8')
    m = re.search(
        r"""
        ^ \s* version \s*=\s* "([^"]+)"
        """,
        text,
        flags=re.VERBOSE | re.MULTILINE | re.IGNORECASE)
    if not m:
        sys.exit(f'Did not find version in {path}')
    return m.group(1)


VERSION = _getversion()


def main():
    settings = get_compiler_settings()

    files = [relpath(join('src', f)) for f in os.listdir('src') if f.endswith('.cpp')]

    if exists('MANIFEST'):
        os.remove('MANIFEST')

    setup(
        name="pyodbc",
        version=VERSION,
        description="DB API Module for ODBC",
        long_description=cleandoc("""
            pyodbc is an open source Python module that makes accessing ODBC databases simple.
            It implements the [DB API 2.0](https://www.python.org/dev/peps/pep-0249)
            specification but is packed with even more Pythonic convenience."""),
        maintainer="Michael Kleehammer",
        maintainer_email="michael@kleehammer.com",
        url='https://github.com/mkleehammer/pyodbc',
        ext_modules=[Extension('pyodbc', sorted(files), **settings)],
        include_package_data=False,
        packages=[''],
        package_dir={'': 'src'},
        package_data={'': ['pyodbc.pyi']},  # places pyodbc.pyi alongside pyodbc.{platform}.{pyd|so} in site-packages
        license='MIT',
        python_requires='>=3.7',
        classifiers=['Development Status :: 5 - Production/Stable',
                     'Intended Audience :: Developers',
                     'Intended Audience :: System Administrators',
                     'License :: OSI Approved :: MIT License',
                     'Operating System :: Microsoft :: Windows',
                     'Operating System :: POSIX',
                     'Programming Language :: Python',
                     'Programming Language :: Python :: 3',
                     'Topic :: Database',
                     ],
        options={
            'bdist_wininst': {'user_access_control': 'auto'}
        }
    )


def get_compiler_settings():

    settings = {
        'extra_compile_args': [],
        'extra_link_args': [],
        'libraries': [],
        'include_dirs': [],
        'define_macros': [('PYODBC_VERSION', VERSION)]
    }

    if os.name == 'nt':
        settings['extra_compile_args'].extend([
            '/Wall',
            '/wd4514',     # unreference inline function removed
            '/wd4820',     # padding after struct member
            '/wd4668',     # is not defined as a preprocessor macro
            '/wd4711',     # function selected for automatic inline expansion
            '/wd4100',     # unreferenced formal parameter
            '/wd4127',     # "conditional expression is constant" testing compilation constants
            '/wd4191',     # casts to PYCFunction which doesn't have the keywords parameter
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
        # Python functions take a lot of 'char *' that really should be const.  gcc complains
        # about this *a lot*.
        settings['extra_compile_args'].extend([
            '-Wno-write-strings',
            '-Wno-deprecated-declarations'
        ])

        # Homebrew installs odbc_config
        pipe = os.popen('odbc_config --cflags --libs 2>/dev/null')
        cflags, ldflags = pipe.readlines()
        exit_status = pipe.close()

        if exit_status is None:
            settings['extra_compile_args'].extend(shlex.split(cflags))
            settings['extra_link_args'].extend(shlex.split(ldflags))
        else:
            settings['libraries'].append('odbc')
            # Add directories for MacPorts and Homebrew.
            dirs = [
                '/usr/local/include',
                '/opt/local/include',
                '/opt/homebrew/include',
                expanduser('~/homebrew/include'),
            ]
            settings['include_dirs'].extend(dir for dir in dirs if isdir(dir))
            # unixODBC make/install places libodbc.dylib in /usr/local/lib/ by default
            # ( also OS/X since El Capitan prevents /usr/lib from being accessed )
            settings['library_dirs'] = ['/usr/local/lib', '/opt/homebrew/lib']
    else:
        # Other posix-like: Linux, Solaris, etc.

        # Python functions take a lot of 'char *' that really should be const.  gcc complains
        # about this *a lot*.
        settings['extra_compile_args'].append('-Wno-write-strings')

        fd = os.popen('odbc_config --cflags 2>/dev/null')
        cflags = fd.read().strip()
        fd.close()
        if cflags:
            settings['extra_compile_args'].extend(cflags.split())
        fd = os.popen('odbc_config --libs 2>/dev/null')
        ldflags = fd.read().strip()
        fd.close()
        if ldflags:
            settings['extra_link_args'].extend(ldflags.split())

        # What is the proper way to detect iODBC, MyODBC, unixODBC, etc.?
        settings['libraries'].append('odbc')

    return settings


if __name__ == '__main__':
    main()
