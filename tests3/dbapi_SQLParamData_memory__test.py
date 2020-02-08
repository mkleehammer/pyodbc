"""
This tests ensures that there is no memory leakage
after using SQLParamData that returns -1.

One scenario where SQLParamData function will be used is when there is a parameterized
INSERT INTO query with at least one parameter's length is too big.
Note that In my case, 'too big' means pGetLen(pInfo->pObject) was more than 4000.

In order to execute the INSERT INTO query, SQLExecute is used.
SQLExecute will return SQL_NEED_DATA (SQL_NEED_DATA = 99),
then SQLParamData will be used to create a SQL parameter and will return SQL_NEED_DATA.
This call will create the pObject (PyObject) that should be freed.
After that SQLPutData will be used in a loop to save the data in this SQL parameter.
Then SQLParamData is called again, and if there is an error (-1), the data of newly
created SQL Parameter should be freed.

This test should be tested against a table that has no space at all or no space in the
transaction log in order to get -1 value on the second call to SQLParamData.
The name of the table is stored in `TABLE_NAME`, change it to be your table's name.
"""
import gc
import os
import unittest

import math
import psutil

from tests3.testutils import add_to_path, load_setup_connection_string

add_to_path()
import pyodbc

KB = 1024
MB = KB * 1024

ACCEPTABLE_MEMORY_DIFF = 500 * KB

TABLE_NAME = "FullTable"

CONNECTION_STRING = None

CONNECTION_STRING_ERROR_MESSAGE = (
    r"Please create tmp\setup.cfg file or set a valid value to CONNECTION_STRING."
)


def current_total_memory_usage():
    """
    :return: Current total memory usage in bytes.
    """
    process = psutil.Process(os.getpid())
    return process.memory_info().rss


class MemoryLeakSQLParamDataTestCase(unittest.TestCase):
    driver = pyodbc

    @classmethod
    def setUpClass(cls):
        filename = os.path.splitext(os.path.basename(__file__))[0]
        cls.connection_string = (
            load_setup_connection_string(filename) or CONNECTION_STRING
        )

        if not cls.connection_string:
            return ValueError(CONNECTION_STRING_ERROR_MESSAGE)

    def test_memory_leak(self):
        query = "INSERT INTO {table_name} VALUES (?)".format(table_name=TABLE_NAME)

        with pyodbc.connect(self.connection_string) as conn:
            cursor = conn.cursor()

            current_memory_usage = current_total_memory_usage()

            try:
                cur = cursor.execute(query, "a" * 10 * MB)
            except self.driver.ProgrammingError as e:
                self.assertEqual("42000", e.args[0])
                self.assertIn("SQLParamData", e.args[1])
            finally:
                cursor.close()

            after_excpetion_memory_usage = current_total_memory_usage()

            diff = math.fabs(after_excpetion_memory_usage - current_memory_usage)
            self.assertLess(diff, ACCEPTABLE_MEMORY_DIFF)


def main():
    unittest.main()


if __name__ == "__main__":
    main()
