#include <stdio.h>
#include <sql.h>
#include <sqlext.h>
#include <Python.h>

int main() {
#ifdef HAVE_WCHAR_H
  if (sizeof(SQLWCHAR) != sizeof(wchar_t)) {
    printf("-DSQL_WCHART_CONVERT=1");
  }
#else
  if (sizeof(SQLWCHAR) != sizeof(Py_UNICODE)) {
    printf("-DSQL_WCHART_CONVERT=1");
  }
#endif
}