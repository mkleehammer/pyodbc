
#include "pyodbc.h"
#include "wrapper.h"
#include "textenc.h"
#include "decimal.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "errors.h"

static PyObject* decimal = 0;
// The Decimal constructor.

static PyObject* re_sub = 0;
static PyObject* re_compile = 0;
static PyObject* re_escape = 0;

// In Python 2.7, the 3 strings below are bytes objects.  In 3.x they are Unicode objects.


static PyObject* pDecimalPoint = 0;
// A "." object which we replace pLocaleDecimal with if they are not the same.

static PyObject* pLocaleDecimal = 0;
// The decimal character used by the locale.  This can be overridden by the user.
//
// In 2.7 this is a bytes object, otherwise unicode.

static PyObject* pLocaleDecimalEscaped = 0;
// A version of pLocaleDecimal escaped to be used in a regular expression.  (The character
// could be something special in regular expressions.)  This is zero when pLocaleDecimal is
// ".", indicating no replacement is necessary.

static PyObject* pRegExpRemove = 0;
// A regular expression that matches characters we want to remove before parsing.


bool InitializeDecimal() {
    // This is called when the module is initialized and creates globals.

    Object d(PyImport_ImportModule("decimal"));
    decimal = PyObject_GetAttrString(d, "Decimal");
    if (!decimal)
        return 0;
    Object re(PyImport_ImportModule("re"));
    re_sub     = PyObject_GetAttrString(re, "sub");
    re_escape  = PyObject_GetAttrString(re, "escape");
    re_compile = PyObject_GetAttrString(re, "compile");

    Object module(PyImport_ImportModule("locale"));
    Object ldict(PyObject_CallMethod(module, "localeconv", 0));
    Object point(PyDict_GetItemString(ldict, "decimal_point"));

    if (!point)
        return false;

    pDecimalPoint = PyUnicode_FromString(".");

    if (!pDecimalPoint)
        return false;

    if (!SetDecimalPoint(point))
        return false;

    return true;
}

PyObject* GetDecimalPoint() {
    Py_INCREF(pLocaleDecimal);
    return pLocaleDecimal;
}

bool SetDecimalPoint(PyObject* pNew)
{
    if (PyObject_RichCompareBool(pNew, pDecimalPoint, Py_EQ) == 1)
    {
        // They are the same.
        Py_XDECREF(pLocaleDecimal);
        pLocaleDecimal = pDecimalPoint;
        Py_INCREF(pLocaleDecimal);

        Py_XDECREF(pLocaleDecimalEscaped);
        pLocaleDecimalEscaped = 0;
    }
    else
    {
        // They are different, so we'll need a regular expression to match it so it can be
        // replaced in getdata GetDataDecimal.

        Py_XDECREF(pLocaleDecimal);
        pLocaleDecimal = pNew;
        Py_INCREF(pLocaleDecimal);

        Object e(PyObject_CallFunctionObjArgs(re_escape, pNew, 0));
        if (!e)
            return false;

        Py_XDECREF(pLocaleDecimalEscaped);
        pLocaleDecimalEscaped = e.Detach();
    }

    Object s(PyUnicode_FromFormat("[^0-9%U-]+", pLocaleDecimal));
    if (!s)
        return false;

    Object r(PyObject_CallFunctionObjArgs(re_compile, s.Get(), 0));
    if (!r)
        return false;

    Py_XDECREF(pRegExpRemove);
    pRegExpRemove = r.Detach();

    return true;
}

static PyObject* DecimalFromUnicode(PyObject* text)
{
    // Creates a Decimal object from a Unicode string.

    // The Decimal constructor requires the decimal point to be '.', so we need to convert the
    // locale's decimal to it.  We also need to remove non-decimal characters such as thousands
    // separators and currency symbols.
    //
    // Remember that the thousands separate will often be '.', so have to do this carefully.
    // We'll create a regular expression with 0-9 and whatever the thousands separator is.

    Object cleaned(PyObject_CallMethod(pRegExpRemove, "sub", "sO", "", text));
    if (!cleaned)
        return 0;

    if (pLocaleDecimalEscaped)
    {
        Object c2(PyObject_CallFunctionObjArgs(re_sub, pLocaleDecimalEscaped, pDecimalPoint, cleaned.Get(), 0));
        if (!c2)
            return 0;
        cleaned.Attach(c2.Detach());
    }

    return PyObject_CallFunctionObjArgs(decimal, cleaned.Get(), 0);
}

static bool MatchesNoCase(const char* pch, Py_ssize_t cch, const char* word)
{
    // Returns true if the cch characters at pch are `word`, which must be lowercase, ignoring
    // ASCII case.
    Py_ssize_t i = 0;
    for (; i < cch && word[i]; i++)
        if ((pch[i] | 0x20) != word[i])
            return false;
    return i == cch && word[i] == 0;
}

PyObject* DecimalFromStrictText(PyObject* text)
{
    // Creates a Decimal object from the text of a driver that never adds grouping separators,
    // as Connection.decimal_mode "text" describes: an optional sign, digits with at most one
    // decimal point, an optional exponent, or NaN, Inf, or Infinity.  Leading and trailing
    // spaces are allowed.
    //
    // Without grouping, the one separator can only be the decimal point, so both '.' and ','
    // are accepted whatever the locale.  (psqlODBC, for example, writes the decimal point of the
    // process's C locale, while other drivers always write '.'.)  Anything else raises rather
    // than being guessed at.

    if (!PyUnicode_IS_ASCII(text))
        goto invalid;

    {
        const char* pch = (const char*)PyUnicode_1BYTE_DATA(text);
        Py_ssize_t start = 0;
        Py_ssize_t end = PyUnicode_GET_LENGTH(text);
        while (start < end && pch[start] == ' ')
            start++;
        while (end > start && pch[end - 1] == ' ')
            end--;

        Py_ssize_t i = start;
        if (i < end && (pch[i] == '-' || pch[i] == '+'))
            i++;

        if (i < end && !(pch[i] >= '0' && pch[i] <= '9') && pch[i] != '.' && pch[i] != ',')
        {
            const char* word = pch + i;
            Py_ssize_t cch = end - i;
            if (MatchesNoCase(word, cch, "nan") || MatchesNoCase(word, cch, "inf") ||
                MatchesNoCase(word, cch, "infinity"))
            {
                return PyObject_CallFunctionObjArgs(decimal, text, 0);
            }
            goto invalid;
        }

        Py_ssize_t cDigits = 0;
        Py_ssize_t iComma = -1;
        bool point = false;
        for (; i < end; i++)
        {
            char ch = pch[i];
            if (ch >= '0' && ch <= '9')
                cDigits++;
            else if ((ch == '.' || ch == ',') && !point)
            {
                point = true;
                if (ch == ',')
                    iComma = i;
            }
            else
                break;
        }
        if (cDigits == 0)
            goto invalid;

        if (i < end && (pch[i] == 'e' || pch[i] == 'E'))
        {
            i++;
            if (i < end && (pch[i] == '-' || pch[i] == '+'))
                i++;
            Py_ssize_t cExpDigits = 0;
            while (i < end && pch[i] >= '0' && pch[i] <= '9')
            {
                i++;
                cExpDigits++;
            }
            if (cExpDigits == 0)
                goto invalid;
        }
        if (i != end)
            goto invalid;

        if (iComma == -1)
            return PyObject_CallFunctionObjArgs(decimal, text, 0);

        // Replace the decimal comma, which the Decimal constructor does not accept.
        Py_ssize_t cch = end - start;
        char* copy = (char*)PyMem_Malloc(cch);
        if (!copy)
            return PyErr_NoMemory();
        memcpy(copy, pch + start, cch);
        copy[iComma - start] = '.';
        Object point_text(PyUnicode_FromStringAndSize(copy, cch));
        PyMem_Free(copy);
        if (!point_text)
            return 0;
        return PyObject_CallFunctionObjArgs(decimal, point_text.Get(), 0);
    }

invalid:
    return RaiseErrorV(0, DataError,
        "Unexpected text %R for a DECIMAL or NUMERIC value.  Set Connection.decimal_mode = "
        "'locale' if the driver adds grouping separators or currency symbols.", text);
}

PyObject* DecimalFromText(const TextEnc& enc, const byte* pb, Py_ssize_t cb, bool strict)
{
    // Creates a Decimal object from a text buffer.  If `strict` is true, the text is parsed as
    // DecimalFromStrictText describes; otherwise it is cleaned using the decimal separator.

    Object text(TextBufferToObject(enc, pb, cb));
    if (!text)
        return 0;

    return strict ? DecimalFromStrictText(text.Get()) : DecimalFromUnicode(text.Get());
}

PyObject* DecimalFromAsciiText(const char* pch, Py_ssize_t cch)
{
    // Creates a Decimal object from text a driver wrote as SQL_C_CHAR, cleaned the same way as
    // DecimalFromText.

    Object text(PyUnicode_DecodeASCII(pch, cch, "strict"));
    if (!text)
        return 0;

    return DecimalFromUnicode(text.Get());
}

PyObject* DecimalFromNumericStruct(const SQL_NUMERIC_STRUCT& num)
{
    // Avoid the problems introduced by locale-specific delimiters in numeric strings
    // by having the driver provide the value using a well-defined binary structure.
    //
    // The value is formatted as "<digits>E<-scale>" and passed to the Decimal constructor once,
    // which is much cheaper than building a Python int, its str, and a tuple of digit objects.
    // The result is identical, including the exponent: Decimal("365688E-2") is
    // Decimal("3656.88").

    // The magnitude is a 128-bit little-endian unsigned integer.  Divide it by 10 repeatedly,
    // collecting the remainders, which are the decimal digits from least significant up.
    unsigned char val[SQL_MAX_NUMERIC_LEN];
    memcpy(val, num.val, sizeof(val));
    int top = SQL_MAX_NUMERIC_LEN;
    while (top > 0 && val[top - 1] == 0)
        top--;

    char digits[40];  // 2**128 has 39 digits
    int cDigits = 0;
    do
    {
        unsigned int rem = 0;
        for (int i = top - 1; i >= 0; i--)
        {
            unsigned int cur = (rem << 8) | val[i];
            val[i] = (unsigned char)(cur / 10);
            rem = cur % 10;
        }
        digits[cDigits++] = (char)('0' + rem);
        while (top > 0 && val[top - 1] == 0)
            top--;
    } while (top > 0);

    char text[64];
    int cch = 0;
    if (num.sign != 1)  // ODBC: 1 is positive, 0 is negative
        text[cch++] = '-';
    while (cDigits > 0)
        text[cch++] = digits[--cDigits];
    cch += snprintf(text + cch, sizeof(text) - cch, "E%d", -(int)num.scale);

    Object str(PyUnicode_FromStringAndSize(text, cch));
    if (!str)
        return NULL;

    return PyObject_CallFunctionObjArgs(decimal, str.Get(), NULL);
}
