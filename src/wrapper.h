
#ifndef _WRAPPER_H_
#define _WRAPPER_H_

class Object
{
private:
    PyObject* p;

    // GCC freaks out if these are private, but it doesn't use them (?)
    // Object(const Object& illegal);
    // void operator=(const Object& illegal);

public:
    Object(PyObject* _p = 0)
    {
        p = _p;
    }

    ~Object()
    {
        Py_XDECREF(p);
    }

    Object& operator=(PyObject* pNew)
    {
        Py_XDECREF(p);
        p = pNew;
        return *this;
    }

    bool IsValid() const { return p != 0; }

    bool Attach(PyObject* _p)
    {
        // Returns true if the new pointer is non-zero.

        Py_XDECREF(p);
        p = _p;
        return (_p != 0);
    }

    PyObject* Detach()
    {
        PyObject* pT = p;
        p = 0;
        return pT;
    }

    operator PyObject*() 
    {
        return p;
    }

    PyObject* Get()
    {
        return p;
    }
};


#ifdef WINVER
struct RegKey
{
    HKEY hkey;

    RegKey()
    {
        hkey = 0;
    }

    ~RegKey()
    {
        if (hkey != 0)
            RegCloseKey(hkey);
    }

    operator HKEY() { return hkey; }
};
#endif

#endif // _WRAPPER_H_
