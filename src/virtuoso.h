#ifndef VIRTUOSO_H
#define VIRTUOSO_H

#ifdef HAVE_IODBC
#include <iodbcext.h>
#endif

/*
 *  Include Virtuoso ODBC extensions for SPASQL result set
 */
#if !defined (SQL_DESC_COL_DV_TYPE)

/*
 *  ODBC extensions for SQLGetDescField
 */
# define SQL_DESC_COL_DV_TYPE               1057L
# define SQL_DESC_COL_DT_DT_TYPE            1058L
# define SQL_DESC_COL_LITERAL_ATTR          1059L
# define SQL_DESC_COL_BOX_FLAGS             1060L
# define SQL_DESC_COL_LITERAL_LANG          1061L
# define SQL_DESC_COL_LITERAL_TYPE          1062L

/*
 *  Virtuoso - ODBC SQL_DESC_COL_DV_TYPE
 */
# define VIRTUOSO_DV_DATE                   129
# define VIRTUOSO_DV_DATETIME               211
# define VIRTUOSO_DV_DOUBLE_FLOAT           191
# define VIRTUOSO_DV_IRI_ID                 243
# define VIRTUOSO_DV_LONG_INT               189
# define VIRTUOSO_DV_NUMERIC                219
# define VIRTUOSO_DV_RDF                    246
# define VIRTUOSO_DV_SINGLE_FLOAT           190
# define VIRTUOSO_DV_STRING                 182
# define VIRTUOSO_DV_TIME                   210
# define VIRTUOSO_DV_TIMESTAMP              128
# define VIRTUOSO_DV_TIMESTAMP_OBJ          208

/*
 *  Virtuoso - ODBC SQL_DESC_COL_DT_DT_TYPE
 */
# define VIRTUOSO_DT_TYPE_DATETIME          1
# define VIRTUOSO_DT_TYPE_DATE              2
# define VIRTUOSO_DT_TYPE_TIME              3

#endif /* SQL_DESC_COL_DV_TYPE */

bool isVirtuoso(HDBC);
bool isSPASQL(PyObject *);

#endif /* VIRTUOSO_H */
