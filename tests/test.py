
from testutils import *
add_to_path()

import pyodbc

cnxn = pyodbc.connect("DRIVER={SQL Server Native Client 10.0};SERVER=localhost;DATABASE=test;Trusted_Connection=yes")
print('cnxn:', cnxn)

cursor = cnxn.cursor()
print('cursor:', cursor)

cursor.execute("select 1")
row = cursor.fetchone()
print('row:', row)


