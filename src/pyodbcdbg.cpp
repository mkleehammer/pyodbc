
#include "pyodbc.h"

#ifdef PYODBC_TRACE
void DebugTrace(const char* szFmt, ...)
{
    va_list marker;
    va_start(marker, szFmt);
    vprintf(szFmt, marker);
    va_end(marker);
}
#endif
