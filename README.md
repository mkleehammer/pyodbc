## pyodbc support for DBMaker

pyodbc is an open source Python module that makes accessing ODBC databases simple.  
DBMaker odbc driver is the odbc-compatible driver for DBMaker database management system, so pyodbc can use it directly, it is more efficiently then unixODBC or odbc driver manager.


### How to install
```
git clone  https://github.com/lidonglifighting/pyodbc.git
cd pyodbc
```

**windows**
Please install python and visual studio first, for example: python3.8 and visual 2019
open cmd with administrator
cd pyodbc
python setup.py build_ext  --define DBMAKER --include-dirs C:/dbmaker/5.4/include   --library-dirs C:/dbmaker/5.4/lib  --libraries "dmapi54 odbc32"
If it is caused Error ¡°error: Microsoft Visual C++ 14.0 is required. Get it with "Microsoft Visual C++ Build Tools": https://visualstudio.microsoft.com/downloads/,
please intall "Desktop Development with C++" with Visual Studio installer.
Then rebuild it.

**linux**
sudo apt-get install build-essential python3-dev libssl-dev libffi-dev libxml2 libxml2-dev libxslt1-dev zlib1g-dev
sudo apt install unixodbc-dev unixodbc
python3 setup.py build_ext  --define DBMAKER  --include-dirs /home/dbmaker/5.4/include   --library-dirs /home/dbmaker/5.4/lib  --rpath /home/dbmaker/5.4/lib/so   --libraries "dmapic odbc"

### unit test for DBMaker


**1) create db with utf-8 lcode**
**2) cd tests3**
**3) python dbmakertests.py -v "Driver=DBMaker 5.4 Driver; Database=utf8db; uid=sysadm; pwd="

Note£ºfor windows, please copy dmapi54.dll to build path of pyodbc.
for example£º
```
 copy "C:\DBMaker\5.4\bin\dmapi54.dll" "E:\pyodbc\pyodbc\build\lib.win-amd64-3.8\" 
```

