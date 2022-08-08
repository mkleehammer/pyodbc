#!/usr/bin/python
import os

import testutils


def main(sqlserver=None, postgresql=None, mysql=None, verbose=0):

    # there is an assumption here about where this file is located
    pyodbc_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # TODO: move the test scripts into separate folders for each database so that
    #       multiple test scripts for each database can easily be discovered
    databases = {
        'SQL Server': {
            'conn_strs': sqlserver or [],
            'discovery_start_dir': os.path.join(pyodbc_dir, 'tests3'),
            'discovery_pattern': 'sqlservertests.py',
        },
        'PostgreSQL': {
            'conn_strs': postgresql or [],
            'discovery_start_dir': os.path.join(pyodbc_dir, 'tests3'),
            'discovery_pattern': 'pgtests.py',
        },
        'MySQL': {
            'conn_strs': mysql or [],
            'discovery_start_dir': os.path.join(pyodbc_dir, 'tests3'),
            'discovery_pattern': 'mysqltests.py',
        },
    }

    overall_result = True
    for db_name, db_attrs in databases.items():

        for db_conn_str in db_attrs['conn_strs']:

            print(f'Running tests against {db_name} with connection string: {db_conn_str}')
            os.environ['PYODBC_CONN_STR'] = db_conn_str

            if verbose > 0:
                cnxn = pyodbc.connect(db_conn_str)
                testutils.print_library_info(cnxn)
                cnxn.close()

            result = testutils.discover_and_run(
                top_level_dir=pyodbc_dir,
                start_dir=db_attrs['discovery_start_dir'],
                pattern=db_attrs['discovery_pattern'],
                verbosity=verbose,
            )
            if not result.wasSuccessful():
                overall_result = False

    return overall_result


if __name__ == '__main__':
    from argparse import ArgumentParser
    parser = ArgumentParser()
    parser.add_argument("--sqlserver", nargs='*', help="connection string(s) for SQL Server")
    parser.add_argument("--postgresql", nargs='*', help="connection string(s) for PostgreSQL")
    parser.add_argument("--mysql", nargs='*', help="connection string(s) for MySQL")
    parser.add_argument("-v", "--verbose", action="count", default=0, help="increment test verbosity (can be used multiple times)")
    args = parser.parse_args()

    # add the build directory to the path so we're testing the latest build, not the installed version
    testutils.add_to_path()

    # only after setting the path, import pyodbc
    import pyodbc

    # run the tests
    main(**vars(args))
