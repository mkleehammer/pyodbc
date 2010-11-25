
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
#ifdef __MINGW32__
  #include <windef.h>
  #include <malloc.h>
#endif
#endif

#define PY_SSIZE_T_CLEAN 1

#include <Python.h>
#include <unicodeobject.h>
#include <structmember.h>

#include <sql.h>
#include <sqlext.h>

#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;
#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof(a[0]))
#endif

#ifdef UNUSED
#undef UNUSED
#endif

inline void UNUSED(...) { }

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static PyObject* mod_configure(PyObject* self)
{
    FILE* f = fopen("pyodbc.conf", "w");
    if (f == 0)
    {
        perror("Unable to create pyodbc.conf");
        return 0;
    }
    
    fprintf(f, "[define_macros]\n");
    fprintf(f, "PYODBC_VERSION: %s\n", TOSTRING(PYODBC_VERSION));
    fprintf(f, "SQLWCHAR_SIZE: %d\n", (int)sizeof(SQLWCHAR));

#if HAVE_WCHAR_H
    fprintf(f, "WCHAR_T_SIZE: %d\n", (int)sizeof(wchar_t));
#endif

    fclose(f);
    
    Py_RETURN_NONE;
}

static PyMethodDef methods[] =
{
    { "configure", (PyCFunction)mod_configure, METH_NOARGS, 0 },
    { 0, 0, 0, 0 }
};

PyMODINIT_FUNC initpyodbcconf()
{
    Py_InitModule4("pyodbcconf", methods, 0, 0, PYTHON_API_VERSION);
}
