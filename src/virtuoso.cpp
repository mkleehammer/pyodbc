#include "pyodbc.h"
#include "virtuoso.h"

bool
isVirtuoso(HDBC hdbc)
{
    char buf[0x1000];
    SQLSMALLINT len;
    SQLRETURN ret;

    ret = SQLGetInfo(hdbc, (SQLUSMALLINT)SQL_DBMS_NAME, buf, sizeof(buf), &len);
    if (!SQL_SUCCEEDED(ret))
	return false;
    if (!strncasecmp(buf, "OpenLink Virtuoso", sizeof(buf))) {
	return true;
    }

    return false;
}

bool
isSPASQL(PyObject *pSql)
{
    char *query = PyString_AS_STRING(pSql);

    if (!query)
	return false;
    while (*query && isspace(*query))
	query++;

    if (!strncasecmp(query, "SPARQL", 6))
	return true;
    return false;
}

