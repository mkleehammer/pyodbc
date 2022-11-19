#!/usr/bin/env python3
"""
Verify that no warning is emitted for `PyUnicode_FromUnicode(NULL, size)`.

See https://github.com/mkleehammer/pyodbc/issues/998.
See also https://bugs.python.org/issue36346.
"""

import io
import os
import sys
import unittest

# pylint: disable-next=import-error
from tests3.testutils import add_to_path, load_setup_connection_string

add_to_path()
import pyodbc  # pylint: disable=wrong-import-position

KB = 1024
MB = KB * 1024

CONNECTION_STRING = None

CONNECTION_STRING_ERROR_MESSAGE = (
    "Please create tmp/setup.cfg file or "
    "set a valid value to CONNECTION_STRING."
)
NO_ERROR = None


class SQLPutDataUnicodeToBytesMemoryLeakTestCase(unittest.TestCase):
    """Test case for issue998 bug fix."""

    driver = pyodbc

    @classmethod
    def setUpClass(cls):
        """Set the connection string."""

        filename = os.path.splitext(os.path.basename(__file__))[0]
        cls.connection_string = (
            load_setup_connection_string(filename) or CONNECTION_STRING
        )

        if cls.connection_string:
            return NO_ERROR
        return ValueError(CONNECTION_STRING_ERROR_MESSAGE)

    def test_use_correct_unicode_factory_function(self):
        """Verify that the obsolete function call has been replaced."""

        # Create a results set.
        with pyodbc.connect(self.connection_string, autocommit=True) as cnxn:
            cursor = cnxn.cursor()
            cursor.execute("SELECT 1 AS a, 2 AS b")
            rows = cursor.fetchall()

        # Redirect stderr so we can detect the warning.
        sys.stderr = redirected_stderr = io.StringIO()

        # Convert the results object to a string.
        self.assertGreater(len(str(rows)), 0)

        # Restore stderr to the original stream.
        sys.stderr = sys.__stderr__

        # If the bug has been fixed, nothing will have been written to stderr.
        self.assertEqual(len(redirected_stderr.getvalue()), 0)


def main():
    """Top-level driver for the test."""
    unittest.main()


if __name__ == "__main__":
    main()
