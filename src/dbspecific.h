
#ifndef DBSPECIFIC_H
#define DBSPECIFIC_H

// Items specific to databases.
//
// Obviously we'd like to minimize this, but if they are needed this file isolates them.  I'd like for there to be a
// single build of pyodbc on each platform and not have a bunch of defines for supporting different databases.


// ---------------------------------------------------------------------------------------------------------------------
// SQL Server


// SQL Server 2005 xml type

#define SQL_SS_XML -152


// SQL Server 2008 time type

#define SQL_SS_TIME2 -154

struct SQL_SS_TIME2_STRUCT
{
   SQLUSMALLINT hour;
   SQLUSMALLINT minute;
   SQLUSMALLINT second;
   SQLUINTEGER  fraction;
};

#endif // DBSPECIFIC_H
