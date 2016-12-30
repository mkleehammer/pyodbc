#!/usr/bin/env python

from testutils import *

add_to_path()
import pyodbc

def main():
    from optparse import OptionParser
    parser = OptionParser()
    parser.add_option("-v", "--verbose", action="count", help="Increment test verbosity (can be used multiple times)")
    parser.add_option("-d", "--debug", action="store_true", default=False, help="Print debugging items")

    (options, args) = parser.parse_args()

    if len(args) > 1:
        parser.error('Only one argument is allowed.  Do you need quotes around the connection string?')

    if not args:
        connection_string = load_setup_connection_string('test')
        if not connection_string:
            print('no connection string')
            parser.print_help()
            raise SystemExit()
    else:
        connection_string = args[0]

    cnxn = pyodbc.connect(connection_string)

    if options.verbose:
        print_library_info(cnxn)

    cursor = cnxn.cursor()
    cursor.execute("select 'å' as uk, 'b' as jp")
    row = cursor.fetchone()
    print(row)

if __name__ == '__main__':
    main()


