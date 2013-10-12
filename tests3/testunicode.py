import pyodbc
cnxn = pyodbc.connect("DSN=VOS")
cursor = cnxn.cursor()
ustr=u"a\u8c22\u00e9"
cursor.execute("CREATE TABLE unicode_test (name NVARCHAR)")
cursor.execute("INSERT into unicode_test values (?)", ustr)
cursor.execute("SELECT name from unicode_test")
result = cursor.fetchone()[0]
cursor.execute("DROP TABLE unicode_test")
assert result == ustr
