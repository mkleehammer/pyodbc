import sys
import unittest
from testutils import *
import dbapi20

def main():
    add_to_path()
    import pyodbc

    from argparse import ArgumentParser
    parser = ArgumentParser(usage="%(prog)s [options] connection_string")
    parser.add_argument("-v", "--verbose", action="count", help="increment test verbosity (can be used multiple times)")
    parser.add_argument("-d", "--debug", action="store_true", default=False, help="print debugging items")
    parser.add_argument("conn_str", nargs="*", help="database connection string")

    args = parser.parse_args()

    if len(args.conn_str) > 1:
        parser.error('Only one argument is allowed.  Do you need quotes around the connection string?')

    if not args:
        connection_string = load_setup_connection_string('dbapitests')

        if not connection_string:
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args.conn_str[0]

    class test_pyodbc(dbapi20.DatabaseAPI20Test):
        driver = pyodbc
        connect_args = [ connection_string ]
        connect_kw_args = {}
    
        def test_nextset(self): pass
        def test_setoutputsize(self): pass
        def test_ExceptionsAsConnectionAttributes(self): pass
    
    suite = unittest.makeSuite(test_pyodbc, 'test')
    testRunner = unittest.TextTestRunner(verbosity=(args.verbose > 1) and 9 or 0)
    result = testRunner.run(suite)

    return result


if __name__ == '__main__':
    sys.exit(0 if main().wasSuccessful() else 1)
