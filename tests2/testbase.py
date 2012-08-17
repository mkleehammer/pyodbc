
import unittest

_TESTSTR = '0123456789-abcdefghijklmnopqrstuvwxyz-'

def _generate_test_string(length):
    """
    Returns a string of `length` characters, constructed by repeating _TESTSTR as necessary.

    To enhance performance, there are 3 ways data is read, based on the length of the value, so most data types are
    tested with 3 lengths.  This function helps us generate the test data.

    We use a recognizable data set instead of a single character to make it less likely that "overlap" errors will
    be hidden and to help us manually identify where a break occurs.
    """
    if length <= len(_TESTSTR):
        return _TESTSTR[:length]

    c = (length + len(_TESTSTR)-1) / len(_TESTSTR)
    v = _TESTSTR * c
    return v[:length]

class TestBase(unittest.TestCase):

    
