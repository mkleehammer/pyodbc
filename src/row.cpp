
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "row.h"
#include "wrapper.h"

struct Row
{
    // A Row must act like a sequence (a tuple of results) to meet the DB API specification, but we also allow values
    // to be accessed via lowercased column names.  We also supply a `columns` attribute which returns the list of
    // column names.

    PyObject_HEAD

    // cursor.description, accessed as _description
    PyObject* description;

    // A Python dictionary mapping from column name to a PyInteger, used to access columns by name.
    PyObject* map_name_to_index;

    // The number of values in apValues.
    Py_ssize_t cValues;

    // The column values, stored as an array.
    PyObject** apValues;
};

#define Row_Check(op) PyObject_TypeCheck(op, &RowType)
#define Row_CheckExact(op) ((op)->ob_type == &RowType)

void FreeRowValues(Py_ssize_t cValues, PyObject** apValues)
{
    if (apValues)
    {
        for (Py_ssize_t i = 0; i < cValues; i++)
            Py_XDECREF(apValues[i]);
        pyodbc_free(apValues);
    }
}

static void Row_dealloc(Row* self)
{
    // Note: Now that __newobj__ is available, our variables could be zero...

    Py_XDECREF(self->description);
    Py_XDECREF(self->map_name_to_index);
    FreeRowValues(self->cValues, self->apValues);
    PyObject_Del(self);
}

Row* Row_New(PyObject* description, PyObject* map_name_to_index, Py_ssize_t cValues, PyObject** apValues)
{
    // Called by other modules to create rows.  Takes ownership of apValues.

    Row* row = PyObject_NEW(Row, &RowType);

    if (row)
    {
        Py_INCREF(description);
        row->description = description;
        Py_INCREF(map_name_to_index);
        row->map_name_to_index = map_name_to_index;
        row->apValues          = apValues;
        row->cValues           = cValues;
    }
    else
    {
        FreeRowValues(cValues, apValues);
    }

    return row;
}

static PyObject*
Row_getattro(PyObject* o, PyObject* name)
{
    // Called to handle 'row.colname'.

    Row* self = (Row*)o;

    PyObject* index = PyDict_GetItem(self->map_name_to_index, name);

    if (index)
    {
        // REVIEW: How is this going to work on a 64-bit system?  First, will the value be a PyInt or a PyLong?  
        Py_ssize_t i = PyInt_AsSsize_t(index);
        Py_INCREF(self->apValues[i]);
        return self->apValues[i];
    }

    return PyObject_GenericGetAttr(o, name);
}

static Py_ssize_t
Row_length(Row* self)
{
    return self->cValues;
}


static int
Row_contains(Row *self, PyObject *el)
{
    // Implementation of contains.  The documentation is not good (non-existent?), so I copied the following from the
    // PySequence_Contains documentation: Return -1 if error; 1 if ob in seq; 0 if ob not in seq.

    int cmp = 0;

	for (Py_ssize_t i = 0, c = self->cValues ; cmp == 0 && i < c; ++i)
		cmp = PyObject_RichCompareBool(el, self->apValues[i], Py_EQ);

	return cmp;
}

static PyObject *
Row_item(Row* self, Py_ssize_t i)
{
    // Apparently, negative indexes are handled by magic ;) -- they never make it here.

	if (i < 0 || i >= self->cValues)
    {
		PyErr_SetString(PyExc_IndexError, "tuple index out of range");
		return NULL;
	}

	Py_INCREF(self->apValues[i]);
	return self->apValues[i];
}


static int
Row_ass_item(Row* self, Py_ssize_t i, PyObject* v)
{
    // Implements row[i] = value.

	if (i < 0 || i >= self->cValues)
    {
		PyErr_SetString(PyExc_IndexError, "Row assignment index out of range");
		return -1;
	}

    Py_XDECREF(self->apValues[i]);
    Py_INCREF(v);
    self->apValues[i] = v;

	return 0;
}

static int
Row_setattro(PyObject* o, PyObject *name, PyObject* v)
{
    Row* self = (Row*)o;

    PyObject* index = PyDict_GetItem(self->map_name_to_index, name);

    if (index)
        return Row_ass_item(self, PyInt_AsSsize_t(index), v);

    return PyObject_GenericSetAttr(o, name, v);
}

static PyObject *
Row_slice(PyObject* o, Py_ssize_t iFirst, Py_ssize_t iMax)
{
    // Note: Negative indexes will have already been converted to positive ones before this is called.  It is possible
    // for the iMax value to be too high if row[:] or row[1:] is used.
    //
    // I don't think iFirst can ever be below zero, but the tuple slice function checks for it, so we will too.

    Row* self = (Row*)o;
    
	if (iFirst < 0)
		iFirst = 0;
	if (iMax > self->cValues)
		iMax = self->cValues;
	if (iMax < iFirst)
		iMax = iFirst;

    if (iFirst == 0 && iMax == self->cValues)
    {
        Py_INCREF(o);
        return o;
    }
    
	Py_ssize_t len = iMax - iFirst;
    PyObject* result = PyTuple_New(len);
	if (!result)
		return 0;

    for (Py_ssize_t i = 0; i < len; i++)
    {
        PyObject* item = self->apValues[iFirst + i];
        PyTuple_SET_ITEM(result, i, item);
        Py_INCREF(item);
    }
    
    return result;
}

static PyObject *
Row_repr(PyObject* o)
{
    Row* self = (Row*)o;

    if (self->cValues == 0)
        return PyString_FromString("()");

    Object pieces = PyTuple_New(self->cValues);
    if (!pieces)
        return 0;

    for (Py_ssize_t i = 0; i < self->cValues; i++)
    {
        PyObject* piece = PyObject_Repr(self->apValues[i]);
        if (!piece)
            return 0;
        PyTuple_SET_ITEM(pieces.Get(), i, piece);
	}

    Object sep = PyString_FromString(", ");
    if (!sep)
        return 0;

    Object s = _PyString_Join(sep, pieces);
    if (!s)
        return 0;

    const char* szWrapper = (self->cValues == 1) ? "(%s, )" : "(%s)";

    Object result = PyString_FromFormat(szWrapper, PyString_AsString(s.Get()));
    return result.Detach();
}

static PyObject* Row_richcompare(PyObject* olhs, PyObject* orhs, int op)
{
    if (!Row_Check(olhs) || !Row_Check(orhs))
    {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    Row* lhs = (Row*)olhs;
    Row* rhs = (Row*)orhs;

    if (lhs->cValues != rhs->cValues)
    {
        // Different sizes, so use the same rules as the tuple class.
        bool result;
		switch (op)
        {
		case Py_EQ: result = (lhs->cValues == rhs->cValues); break;
		case Py_GE: result = (lhs->cValues >= rhs->cValues); break;
		case Py_GT: result = (lhs->cValues >  rhs->cValues); break;
		case Py_LE: result = (lhs->cValues <= rhs->cValues); break;
		case Py_LT: result = (lhs->cValues <  rhs->cValues); break;
		case Py_NE: result = (lhs->cValues != rhs->cValues); break;
        default:
            // Can't get here, but don't have a cross-compiler way to silence this.
            result = false;
		}
        PyObject* p = result ? Py_True : Py_False;
        Py_INCREF(p);
        return p;
    }

    for (Py_ssize_t i = 0, c = lhs->cValues; i < c; i++)
        if (!PyObject_RichCompareBool(lhs->apValues[i], rhs->apValues[i], Py_EQ))
            return PyObject_RichCompare(lhs->apValues[i], rhs->apValues[i], op);

    // All items are equal.
    switch (op)
    {
    case Py_EQ:
    case Py_GE:
    case Py_LE:
        Py_RETURN_TRUE;

    case Py_GT:
    case Py_LT:
    case Py_NE:
        break;
    }

    Py_RETURN_FALSE;
}

static PySequenceMethods row_as_sequence =
{
    (lenfunc)Row_length,               // sq_length
    0,                                 // sq_concat
    0,                                 // sq_repeat
    (ssizeargfunc)Row_item,            // sq_item
    Row_slice,                         // sq_slice
	(ssizeobjargproc)Row_ass_item,     // sq_ass_item
    0,                                 // sq_ass_slice
    (objobjproc)Row_contains,          // sq_contains
};

static char description_doc[] = "The Cursor.description sequence from the Cursor that created this row.";

static PyMemberDef Row_members[] =
{
    { "cursor_description", T_OBJECT_EX, offsetof(Row, description), READONLY, description_doc },
    { 0 }
};


static char row_doc[] =
    "Row objects are sequence objects that hold query results.\n" 
    "\n" 
    "They are similar to tuples in that they cannot be resized and new attributes\n" 
    "cannot be added, but individual elements can be replaced.  This allows data to\n" 
    "be \"fixed up\" after being fetched.  (For example, datetimes may be replaced by\n" 
    "those with time zones attached.)\n" 
    "\n" 
    "  row[0] = row[0].replace(tzinfo=timezone)\n" 
    "  print row[0]\n" 
    "\n" 
    "Additionally, individual values can be optionally be accessed or replaced by\n" 
    "name.  Non-alphanumeric characters are replaced with an underscore.\n"
    "\n" 
    "  cursor.execute(\"select customer_id, [Name With Spaces] from tmp\")\n" 
    "  row = cursor.fetchone()\n" 
    "  print row.customer_id, row.Name_With_Spaces\n" 
    "\n" 
    "If using this non-standard feature, it is often convenient to specifiy the name\n" 
    "using the SQL 'as' keyword:\n" 
    "\n" 
    "  cursor.execute(\"select count(*) as total from tmp\")\n" 
    "  row = cursor.fetchone()\n" 
    "  print row.total";
  
PyTypeObject RowType =
{
	PyObject_HEAD_INIT(0)
    0,                                                      // ob_size                                        
    "pyodbc.Row",                                           // tp_name
    sizeof(Row),                                            // tp_basicsize
    0,                                                      // tp_itemsize
    (destructor)Row_dealloc,                                // destructor tp_dealloc
    0,                                                      // tp_print
    0,                                                      // tp_getattr
    0,                                                      // tp_setattr
    0,                                                      // tp_compare
    Row_repr,                                               // tp_repr
    0,                                                      // tp_as_number
	&row_as_sequence,                                       // tp_as_sequence
    0,                                                      // tp_as_mapping
    0,                                                      // tp_hash
    0,                                                      // tp_call
    0,                                                      // tp_str
    Row_getattro,                                           // tp_getattro
    Row_setattro,                                           // tp_setattro
    0,                                                      // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                                     // tp_flags
    row_doc,                                                // tp_doc
    0,                                                      // tp_traverse
    0,                                                      // tp_clear
    Row_richcompare,                                        // tp_richcompare
    0,                                                      // tp_weaklistoffset
    0,                                                      // tp_iter
    0,                                                      // tp_iternext
    0,                                                      // tp_methods
    Row_members,                                            // tp_members
    0,                                                      // tp_getset
    0,                                                      // tp_base
    0,                                                      // tp_dict
    0,                                                      // tp_descr_get
    0,                                                      // tp_descr_set
    0,                                                      // tp_dictoffset
    0,                                                      // tp_init
    0,                                                      // tp_alloc
    0,                                                      // tp_new
    0,                                                      // tp_free
    0,                                                      // tp_is_gc
    0,                                                      // tp_bases
    0,                                                      // tp_mro
    0,                                                      // tp_cache
    0,                                                      // tp_subclasses
    0,                                                      // tp_weaklist
};
