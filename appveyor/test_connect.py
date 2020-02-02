import sys
import pyodbc
c = pyodbc.connect(sys.argv[1])
c.close()
