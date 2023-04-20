#!/usr/bin/python
import configparser
import os
import sys
from typing import List, Optional, Tuple

import testutils

import pyodbc
import pytest


def option_transform(optionstr: str) -> str:
    # the default ConfigParser() behavior is to lowercase key values,
    # override this by simply returning the original key value
    return optionstr


def generate_connection_string(attrs: List[Tuple[str, str]]) -> str:
    attrs_str_list = []
    for key, value in attrs:
        # escape/bookend values that include special characters
        #   ref: https://learn.microsoft.com/en-us/openspecs/sql_server_protocols/ms-odbcstr/348b0b4d-358a-41fb-9753-6351425809cb
        if any(c in value for c in ';} '):
            value = '{{{}}}'.format(value.replace('}', '}}'))

        attrs_str_list.append(f'{key}={value}')

    conn_str = ';'.join(attrs_str_list)
    return conn_str


def read_db_config() -> Tuple[List[str], List[str], List[str]]:
    sqlserver = []
    postgresql = []
    mysql = []

    # get the filename of the database configuration file
    pyodbc_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_cfg_file = os.path.join(pyodbc_dir, 'tmp', 'database.cfg')
    cfg_file = os.getenv('PYODBC_DATABASE_CFG', default_cfg_file)

    if os.path.exists(cfg_file):
        print(f'Using database configuration file: {cfg_file}')

        # read the contents of the config file
        config = configparser.ConfigParser()
        config.optionxform = option_transform  # prevents keys from being lowercased
        config.read(cfg_file)

        # generate the connection strings
        for section in config.sections():
            section_lower = section.lower()
            if section_lower.startswith('sqlserver'):
                conn_string = generate_connection_string(config.items(section))
                sqlserver.append(conn_string)
            elif section_lower.startswith('postgres'):
                conn_string = generate_connection_string(config.items(section))
                postgresql.append(conn_string)
            elif section_lower.startswith('mysql'):
                conn_string = generate_connection_string(config.items(section))
                mysql.append(conn_string)
    else:
        print(f'Database configuration file not found: {cfg_file}')

    return sqlserver, postgresql, mysql


def main(sqlserver: Optional[List[str]] = None,
         postgresql: Optional[List[str]] = None,
         mysql: Optional[List[str]] = None,
         verbose: int = 0,
         quiet: int = 0,
         k_expression: Optional[str] = None) -> bool:

    # read from the config file if no connection strings provided
    if not (sqlserver or postgresql or mysql):
        sqlserver, postgresql, mysql = read_db_config()

    if not (sqlserver or postgresql or mysql):
        print('No tests have been run because no database connection info was provided')
        return False

    tests_dir = os.path.dirname(os.path.abspath(__file__))

    databases = {
        'SQL Server': {
            'conn_strs': sqlserver or [],
            'discovery_patterns': [
                # FUTURE: point to dir specific to SQL Server - os.path.join(tests_dir, 'sqlserver'),
                os.path.join(tests_dir, 'sqlservertests.py'),
            ],
        },
        'PostgreSQL': {
            'conn_strs': postgresql or [],
            'discovery_patterns': [
                # FUTURE: point to dir specific to PostgreSQL - os.path.join(tests_dir, 'postgresql'),
                os.path.join(tests_dir, 'pgtests.py'),
            ],
        },
        'MySQL': {
            'conn_strs': mysql or [],
            'discovery_patterns': [
                # FUTURE: point to dir specific to MySQL - os.path.join(tests_dir, 'mysql'),
                os.path.join(tests_dir, 'mysqltests.py'),
            ],
        },
    }

    overall_result = True
    for db_name, db_attrs in databases.items():

        for db_conn_str in db_attrs['conn_strs']:
            print(f'Running tests against {db_name} with connection string: {db_conn_str}')

            if verbose > 0:
                cnxn = pyodbc.connect(db_conn_str)
                testutils.print_library_info(cnxn)
                cnxn.close()

            # it doesn't seem to be easy to pass test parameters into the test
            # discovery process, so the connection string will have to be passed
            # to the test cases via an environment variable
            os.environ['PYODBC_CONN_STR'] = db_conn_str

            # construct arguments for pytest
            pytest_args = []

            if verbose > 0:
                pytest_args.extend(['-v'] * verbose)
            elif quiet > 0:
                pytest_args.extend(['-q'] * quiet)

            if k_expression:
                pytest_args.extend(['-k', k_expression])

            pytest_args.extend(db_attrs['discovery_patterns'])

            # run the tests
            retcode = pytest.main(args=pytest_args)
            if retcode == pytest.ExitCode.NO_TESTS_COLLECTED:
                print('No tests collected during discovery')
                overall_result = False
            elif retcode != pytest.ExitCode.OK:
                overall_result = False

    return overall_result


if __name__ == '__main__':
    from argparse import ArgumentParser
    parser = ArgumentParser()
    parser.add_argument("--sqlserver", action="append", help="connection string for SQL Server")
    parser.add_argument("--postgresql", action="append", help="connection string for PostgreSQL")
    parser.add_argument("--mysql", action="append", help="connection string for MySQL")
    parser.add_argument("-k", dest="k_expression", help="run tests whose names match the expression")
    qv_group = parser.add_mutually_exclusive_group()
    qv_group.add_argument("-q", "--quiet", action="count", default=0, help="decrease test verbosity (can be used multiple times)")
    qv_group.add_argument("-v", "--verbose", action="count", default=0, help="increment test verbosity (can be used multiple times)")
    # TODO: gather any remaining args and include in call to pytest???  i.e. known_args, other_args = parser.parse_known_args()
    args = parser.parse_args()

    # run the tests
    passed = main(**vars(args))
    sys.exit(0 if passed else 1)
