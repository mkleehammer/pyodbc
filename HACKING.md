

# Development Testing

We use tox for complete testing, but when you are in the middle of development you need fast
turn around.  In this mode you need to be able to build and run tests using pytest manually.
To do this, build from the root of the directory using `--inplace` which will build the library
into the root.  Run pytest from the same root directory and the new pyodbc library you built
will be in the path for your test:

    python setup.py build_ext --inplace
    pytest test/test_postgresql.py -vxk test_text

If a segmentation fault occurs while running tests, pytest will have eaten the output.  Add
-s to the command line:

    python setup.py build_ext --inplace -D PYODBC_TRACE
    pytest test/test_postgresql.py -vxk test_text -vs


# Notes

## uint16_t

You'll notice we use uint16_t instead of SQLWCHAR.  The unixODBC headers would define SQLWCHAR
as wchar_t even when wchar_t as defined by the C library as uint32_t.  The data in the buffer
was still 16 bit however.

