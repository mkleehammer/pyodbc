# Unicode

## TL;DR

By default, pyodbc uses UTF-8 and UTF-16LE encodings as specified by the ODBC specification.
Many drivers behave different than the specification so you may need to configure your
connection.  I recommend creating a global connection factory where you can consolidate your
connection string and configuration:

    def connect():
        cnxn = pyodbc.connect(_connection_string)
        cnxn.setencoding('utf-8')
        return cnxn

### Configuring Specific Databases

#### Microsoft SQL Server

The pyodbc default configuration is to use UTF-8 for single byte reads and writes and UTF-16LE
for Unicode.  If you are usinga recent driver (native client 11+, I believe) the defaults are
fine.  Very old versions may use UCS-2 which will still probably work as-is for most Unicode
characters.

#### MySQL, PostgreSQL, and Teradata

Unfortunately all of these databases tend to use a single encoding and do not differentiate
between "SQL_WCHAR" and "SQL_WCHAR".  Therefore you must configure them to encode Unicode data
as UTF-8 and to decode 2-byte SQL_WCHAR data using UTF-8.

    # Python 2.7
    cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
    cnxn.setencoding(str, encoding='utf-8')
    cnxn.setencoding(unicode, encoding='utf-8', ctype=pyodbc.SQL_CHAR)

    # Python 3.x
    cnxn.setdecoding(pyodbc.SQL_WCHAR, encoding='utf-8')
    cnxn.setencoding(encoding='utf-8')

If you are using MySQL, you can add the character set to the connection string, but I'm not
sure if this is necessary.

    # MySQL
    cstring = 'DSN=mydsn;CharSet=utf8'
    cnxn = pyodbc.connect(cstring)


## Details

### Encodings

The Unicode standard specifies numeric "codes" for each character, such as 65 for 'A' and 229
for 'å' (latin small letter a with ring above).  However, it does *not* specify how these
numbers should be represented in a computer's memory.  The same characters can be represented
in multiple ways which are called *encodings*. For example, here are the example characters
above in some different encodings:

| Character | Encoding  |  Bytes |
| --------- | --------  |  ----- |
| A         | latin1    |   0x41 |
| A         | utf-8     |   0x41 |
| A         | utf-16-le | 0x4100 |
| A         | utf-16-be | 0x0041 |
| å         | latin1    |   0xe5 |
| å         | utf-8     | 0xc3a5 |
| å         | utf-16-le | 0xe500 |
| å         | utf-16-be | 0x00e5 |

ASCII characters, such as "A", have values less than 128 and are easy to store in a single
byte.  Values greater than 127 sometimes are encoded with multiple bytes even if the value
itself could fit in a single byte.  Notice the UTF-8 encoding of "å" is 0xc3a5 even though its
value fits in the single latin1 byte 0xe5.

**IMPORTANT:** The thing to note here is that when converting text to bytes, *some* encoding
must be chosen.  Even a string as simple as "A" has more than one binary format, as the table
above makes clear.

### ODBC Conversions

The ODBC specification defines two C data types for character data:

* SQL_CHAR: A single-byte type like the C `char` data type, though the sign may differ.
* SQL_WCHAR: A two-byte data type like a 2-byte `wchar_t`

Originally ODBC specified Unicode data to be encoded using UCS-2 and transferred in SQL_WCHAR
buffers.  Later this was changed to allow UTF-8 in SQL_CHAR buffers and UTF-16LE in SQL_WCHAR
buffers.

Because of this, pyodbc defaults to the following.

Writing Python objects to the database:

| Version    | Object  | encoding    | C type     |
| -------    | ------  | --------    | ------     |
| Python 2.7 | str     | UTF-8       | SQL_CHAR   |
| Python 2.7 | unicode | UTF-16LE    | SQL_WCHAR  |
| Python 3   | str     | UTF-16LE    | SQL_WCHAR  |

Reading buffers from the database into Python objects:

| Version    | Buffer Type | Encoding | Object  |
| -------    | ----------- | -------- | ------  |
| Python 2.7 | SQL_CHAR    | UTF-8    | unicode |
| Python 2.7 | SQL_WCHAR   | UTF-16LE | unicode |
| Python 3   | SQL_CHAR    | UTF-8    | str     |
| Python 3   | SQL_WCHAR   | UTF-16LE | str     |


### Configuring

If the defaults above do not match your database driver, you can use the
`Connection.setencoding` and `Connection.setdecoding` functions.

#### Python 3

    cnxn.setencoding(encoding=None, ctype=None)
    
This sets the encoding used when writing an `str` object to the database and the C data type.
(The data is always written to an array of bytes, but we must tell the database if it should
treat the buffer as an array of SQL_CHARs or SQL_WCHARs.)

The `encoding` must be a valid Python encoding that converts text to `bytes`.  Optimized code
is used for "utf-8", "utf-16", "utf-16le", and "utf-16be".

The result is always an array of bytes but we must also tell the ODBC driver if it should treat
the buffer as an array of SQL_CHAR or SQL_WCHAR elements.  If not provided, `pyodbc.SQL_WCHAR`
is used for the UTF-16 variants and `pyodbc.SQL_CHAR` is used for everything else.

    cnxn.setdecoding(sqltype, encoding=None, ctype=None)

This sets the encoding used when reading a buffer from the database and converting it to an
`str` object.

`sqltype` controls which SQL type being configured: `pyodbc.SQL_CHAR` or `pyodbc.SQL_WCHAR`.
Use SQL_CHAR to configure the encoding when reading a SQL_CHAR buffer, etc.

When a buffer is being read, the driver will tell pyodbc whether it is a SQL_CHAR or SQL_WCHAR
buffer.  However, pyodbc can request the data be converted to either of these formats.  Most of
the time this parameter should not be supplied and the same type will be used, but this can be
useful in the case where the driver reports SQL_WCHAR data but the data is actually in UTF-8.

#### Python 2

The Python 2 version of setencoding starts with a type parameter so that `str` and `unicode`
can be configured independantly.

    # cnxn.setencoding(type, encoding=None, ctype=None)

    cnxn.setencoding(str, encoding='utf-8')
    cnxn.setencoding(unicode, encoding='utf-8', ctype=pyodbc.SQL_CHAR)
