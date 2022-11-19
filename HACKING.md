

# Development Testing

We use tox for complete testing, but when you are in the middle of development you need fast
turn around.  In this mode you need to be able to build and run tests using pytest manually.
To do this, build from the root of the directory using `--inplace` which will build the library
into the root.  Run pytest from the same root directory and the new pyodbc library you built
will be in the path for your test:

    python setup.py build_ext --inplace
    pytest test/test_postgresql.py -vxk test_text
