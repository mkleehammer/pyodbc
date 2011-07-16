
/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef PYODBC_H
#define PYODBC_H

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <malloc.h>
typedef __int64 INT64;
typedef unsigned __int64 UINT64;
#else
typedef unsigned char byte;
typedef unsigned int UINT;
typedef long long INT64;
typedef unsigned long long UINT64;
#define _strcmpi strcasecmp
#ifdef __MINGW32__
  #include <windef.h>
  #include <malloc.h>
#else
  inline int max(int lhs, int rhs) { return (rhs > lhs) ? rhs : lhs; }
#endif
#endif

#ifdef __SUN__
#include <alloca.h>
#endif

#define PY_SSIZE_T_CLEAN 1

#include <Python.h>
#include <stringobject.h>
#include <intobject.h>
#include <floatobject.h>
#include <longobject.h>
#include <boolobject.h>
#include <bufferobject.h>
#include <unicodeobject.h>
#include <structmember.h>
#include <datetime.h>

// Whoever wrote the datetime C module declared a static variable in the header file.  A properly conforming C/C++
// compiler will create a new copy in every source file, meaning you can't set the value globally.  Criminy.  We'll
// declare our own global which will be set during initialization.
//
// We could initialize PyDateTimeAPI in each module, but we don't have a function in each module that is guaranteed to
// be called first and I don't want to create an Init function just for this datetime bug.

#undef PyDate_Check
#undef PyDate_CheckExact
#undef PyDateTime_Check
#undef PyDateTime_CheckExact
#undef PyTime_Check
#undef PyTime_CheckExact

extern _typeobject* OurDateTimeType;
extern _typeobject* OurDateType;
extern _typeobject* OurTimeType;

#define PyDate_Check(op) PyObject_TypeCheck(op, OurDateType)
#define PyDate_CheckExact(op) ((op)->ob_type == OurDateType)
#define PyDateTime_Check(op) PyObject_TypeCheck(op, OurDateTimeType)
#define PyDateTime_CheckExact(op) ((op)->ob_type == OurDateTimeType)
#define PyTime_Check(op) PyObject_TypeCheck(op, OurTimeType)
#define PyTime_CheckExact(op) ((op)->ob_type == OurTimeType)

#include <sql.h>
#include <sqlext.h>

#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;
#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#define PyInt_AsSsize_t PyInt_AsLong
#define lenfunc inquiry
#define ssizeargfunc intargfunc
#define ssizeobjargproc intobjargproc
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof(a[0]))
#endif

inline bool IsSet(DWORD grf, DWORD flags)
{
    return (grf & flags) == flags;
}

#ifdef UNUSED
#undef UNUSED
#endif

inline void UNUSED(...) { }

#include <stdarg.h>

#if defined(__SUNPRO_CC) || defined(__SUNPRO_C) || (defined(__GNUC__) && !defined(__MINGW32__))
#include <alloca.h>
#define CDECL cdecl
#define min(X,Y) ((X) < (Y) ? (X) : (Y))
#define max(X,Y) ((X) > (Y) ? (X) : (Y))
#define _alloca alloca
inline void _strlwr(char* name)
{
    while (*name) { *name = tolower(*name); name++; }
}
#else
#define CDECL
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Building an actual debug version of Python is so much of a pain that it never happens.  I'm providing release-build
// versions of assertions.

#ifdef PYODBC_ASSERT
  #ifdef _MSC_VER
    #include <crtdbg.h>
    inline void FailAssert(const char* szFile, size_t line, const char* szExpr)
    {
        printf("assertion failed: %s(%d)\n%s\n", szFile, line, szExpr);
        __debugbreak(); // _CrtDbgBreak();
    }
    #define I(expr) if (!(expr)) FailAssert(__FILE__, __LINE__, #expr);
    #define N(expr) if (expr) FailAssert(__FILE__, __LINE__, #expr);
  #else
    #define I(expr)
    #define N(expr)
  #endif
#else
  #define I(expr)
  #define N(expr)
#endif

#ifdef PYODBC_TRACE
void CDECL DebugTrace(const char* szFmt, ...);
#else  
inline void DebugTrace(const char* szFmt, ...) { UNUSED(szFmt); }
#endif
#define TRACE DebugTrace

#ifdef PYODBC_LEAK_CHECK
#define pyodbc_malloc(len) _pyodbc_malloc(__FILE__, __LINE__, len)
void* _pyodbc_malloc(const char* filename, int lineno, size_t len);
void pyodbc_free(void* p);
void pyodbc_leak_check();
#else
#define pyodbc_malloc malloc
#define pyodbc_free free
#endif

void PrintBytes(void* p, size_t len);

#endif // pyodbc_h
