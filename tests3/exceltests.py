#!/usr/bin/python

# Tests for reading from Excel files.
#
# I have not been able to successfully create or modify Excel files.

import sys, os, re
import unittest
from os.path import abspath
from testutils import *

CNXNSTRING = None

class ExcelTestCase(unittest.TestCase):

    def __init__(self, method_name):
        unittest.TestCase.__init__(self, method_name)

    def setUp(self):
        self.cnxn   = pyodbc.connect(CNXNSTRING, autocommit=True)
        self.cursor = self.cnxn.cursor()

        for i in range(3):
            try:
                self.cursor.execute("drop table t%d" % i)
                self.cnxn.commit()
            except:
                pass

        self.cnxn.rollback()

    def tearDown(self):
        try:
            self.cursor.close()
            self.cnxn.close()
        except:
            # If we've already closed the cursor or connection, exceptions are thrown.
            pass

    def test_getinfo_string(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CATALOG_NAME_SEPARATOR)
        self.assertTrue(isinstance(value, str))
     
    def test_getinfo_bool(self):
        value = self.cnxn.getinfo(pyodbc.SQL_ACCESSIBLE_TABLES)
        self.assertTrue(isinstance(value, bool))
     
    def test_getinfo_int(self):
        value = self.cnxn.getinfo(pyodbc.SQL_DEFAULT_TXN_ISOLATION)
        self.assertTrue(isinstance(value, (int, long)))
     
    def test_getinfo_smallint(self):
        value = self.cnxn.getinfo(pyodbc.SQL_CONCAT_NULL_BEHAVIOR)
        self.assertTrue(isinstance(value, int))


    def test_read_sheet(self):
        # The first method of reading data is to access worksheets by name in this format [name$].
        #
        # Our second sheet is named Sheet2 and has two columns.  The first has values 10, 20, 30, etc.

        rows = self.cursor.execute("select * from [Sheet2$]").fetchall()
        self.assertEqual(len(rows), 5)

        for index, row in enumerate(rows):
            self.assertEqual(row.s2num, float(index + 1) * 10)

    def test_read_range(self):
        # The second method of reading data is to assign a name to a range of cells and access that as a table.
        #
        # Our first worksheet has a section named Table1.  The first column has values 1, 2, 3, etc.

        rows = self.cursor.execute("select * from Table1").fetchall()
        self.assertEqual(len(rows), 10)
     
        for index, row in enumerate(rows):
            self.assertEqual(row.num, float(index + 1))
            self.assertEqual(row.val, chr(ord('a') + index))

    def test_tables(self):
        # This is useful for figuring out what is available
        tables = [ row.table_name for row in self.cursor.tables() ]
        assert 'Sheet2$' in tables, 'tables: %s' % ' '.join(tables)


    # def test_append(self):
    #     rows = self.cursor.execute("select s2num, s2val from [Sheet2$]").fetchall()
    #  
    #     print rows
    #  
    #     nextnum = max([ row.s2num for row in rows ]) + 10
    #     
    #     self.cursor.execute("insert into [Sheet2$](s2num, s2val) values (?, 'z')", nextnum)
    #  
    #     row = self.cursor.execute("select s2num, s2val from [Sheet2$] where s2num=?", nextnum).fetchone()
    #     self.assertTrue(row)
    #  
    #     print 'added:', nextnum, len(rows), 'rows'
    #  
    #     self.assertEqual(row.s2num, nextnum)
    #     self.assertEqual(row.s2val, 'z')
    #  
    #     self.cnxn.commit()


def main():
    from optparse import OptionParser
    parser = OptionParser() #usage=usage)
    parser.add_option("-v", "--verbose", action="count", help="Increment test verbosity (can be used multiple times)")
    parser.add_option("-d", "--debug", action="store_true", default=False, help="Print debugging items")
    parser.add_option("-t", "--test", help="Run only the named test")

    (options, args) = parser.parse_args()

    if args:
        parser.error('no arguments expected')
    
    global CNXNSTRING

    path = dirname(abspath(__file__))
    filename = join(path, 'test.xls')
    assert os.path.exists(filename)
    CNXNSTRING = 'Driver={Microsoft Excel Driver (*.xls)};DBQ=%s;READONLY=FALSE' % filename

    if options.verbose:
        cnxn = pyodbc.connect(CNXNSTRING, autocommit=True)
        print_library_info(cnxn)
        cnxn.close()

    suite = load_tests(ExcelTestCase, options.test)

    testRunner = unittest.TextTestRunner(verbosity=options.verbose)
    result = testRunner.run(suite)

    return result


if __name__ == '__main__':

    # Add the build directory to the path so we're testing the latest build, not the installed version.
    add_to_path()
    import pyodbc
    sys.exit(0 if main().wasSuccessful() else 1)
