/**
 * Types, constants, and signatures needed for BCP.
 */
#ifndef _BCP_H_
#define _BCP_H_

#include <sqlext.h>

// BCP constants.
#define SQL_BCP_ON              1L
#define SQL_COPT_SS_BCP         1219 // Allow BCP usage on connection

// Arguments for a call to the bcp() method.
struct BCP_OPTS {
    // Required positional-only arguments.
    long      action;
    PyObject* table;
    PyObject* datafile;

    // Optional keyword-only arguments.
    PyObject* formatfile;
    PyObject* errorfile;
    PyObject* firstrow;
    PyObject* lastrow;
    PyObject* maxerrors;
};
PyObject* _bcp_impl(PyObject* conn, const BCP_OPTS& opts);

#endif  // _BCP_H_
