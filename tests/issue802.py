"""
This tests ensures that there is no memory leakage
when params.cpp:ExecuteMulti function does conversion of Unicode to Bytes.

In ExecuteMulti function after DoExecute label

SQLExecute returns

One scenario where SQLParamData function will be used is when there is a varchar(max),
a parameter with an unknown size in the INSERT INTO query.
In this case, a unicode string is being added to a varchar(max) field.

In order to execute the INSERT INTO query, SQLExecute is used. SQLExecute will return
SQL_NEED_DATA (SQL_NEED_DATA = 99). Then SQLParamData will be used to create a SQL
parameter and will return SQL_NEED_DATA too. When PyUnicode_Check(pInfo->cell) is true,
a conversion of Unicode to Bytes is required before it can be used by SQLPutData.
During this conversion a new PyObject, called bytes, is created and assigned to objCell.
This object never gets Py_XDECREF, and the data will stay stuck in the memory without a
reference.

This memory leak is only visible when using varchar(max) because varchar(max) required
additional allocation of memory that correspond to the size of the input while
varchar(100) for example will not case another SQL_NEED_DATA status.

To see how to reproduce the memory leak,
look at https://github.com/mkleehammer/pyodbc/issues/802
"""
import os
import unittest

import psutil

from tests3.testutils import add_to_path, load_setup_connection_string

add_to_path()
import pyodbc

KB = 1024
MB = KB * 1024

CONNECTION_STRING = None

CONNECTION_STRING_ERROR_MESSAGE = (
    r"Please create tmp\setup.cfg file or set a valid value to CONNECTION_STRING."
)

process = psutil.Process()


def memory():
    return process.memory_info().vms


class SQLPutDataUnicodeToBytesMemoryLeakTestCase(unittest.TestCase):
    driver = pyodbc

    @classmethod
    def setUpClass(cls):
        filename = os.path.splitext(os.path.basename(__file__))[0]
        cls.connection_string = (
            load_setup_connection_string(filename) or CONNECTION_STRING
        )

        if not cls.connection_string:
            return ValueError(CONNECTION_STRING_ERROR_MESSAGE)

    def test__varchar_max__inserting_many_rows__same_memory_usage(self):
        varchar_limit = "max"
        num_rows = 50_000
        data = [(i, f"col{i:06}", 3.14159265 * (i + 1)) for i in range(num_rows)]
        table_name = "pd_test"
        col_names = ["id", "txt_col", "float_col"]
        ins_sql = f"INSERT INTO {table_name} ({','.join(col_names)}) VALUES ({','.join('?' * len(col_names))})"

        with pyodbc.connect(self.connection_string, autocommit=True) as cnxn:
            # First time adds memory, not related to the test.
            self.action(cnxn, data, ins_sql, table_name, varchar_limit)
            for iteration in range(3):
                start_memory = memory()
                self.action(cnxn, data, ins_sql, table_name, varchar_limit)
                end_memory = memory()
                memory_diff = end_memory - start_memory
                self.assertLess(memory_diff, 100 * KB)

    def action(self, cnxn, data, ins_sql, table_name, varchar_limit):
        crsr = cnxn.cursor()
        crsr.execute(f"DROP TABLE IF EXISTS {table_name}")
        crsr.execute(
            f"CREATE TABLE {table_name} (id int, txt_col varchar({varchar_limit}), float_col float(53))"
        )
        crsr.fast_executemany = True
        crsr.executemany(ins_sql, data)
        crsr.close()


def main():
    unittest.main()


if __name__ == "__main__":
    main()
