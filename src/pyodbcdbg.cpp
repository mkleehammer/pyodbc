
#include "pyodbc.h"

void PrintBytes(void* p, size_t len)
{
    unsigned char* pch = (unsigned char*)p;
    for (size_t i = 0; i < len; i++)
        printf("%02x ", (int)pch[i]);
    printf("\n");
}


#ifdef PYODBC_TRACE
void DebugTrace(const char* szFmt, ...)
{
    va_list marker;
    va_start(marker, szFmt);
    vprintf(szFmt, marker);
    va_end(marker);
}
#endif

#ifdef PYODBC_LEAK_CHECK

// THIS IS NOT THREAD SAFE: This is only designed for the single-threaded unit tests!

struct Allocation
{
    const char* filename;
    int lineno;
    size_t len;
    void* pointer;
    int counter;
};

static Allocation* allocs = 0;
static int bufsize = 0;
static int count = 0;
static int allocCounter = 0;

void* _pyodbc_malloc(const char* filename, int lineno, size_t len)
{
    void* p = malloc(len);
    if (p == 0)
        return 0;

    if (count == bufsize)
    {
        allocs = (Allocation*)realloc(allocs, (bufsize + 20) * sizeof(Allocation));
        if (allocs == 0)
        {
            // Yes we just lost the original pointer, but we don't care since everything is about to fail.  This is a
            // debug leak check, not a production malloc that needs to be robust in low memory.
            bufsize = 0;
            count   = 0;
            return 0;
        }
        bufsize += 20;
    }

    allocs[count].filename = filename;
    allocs[count].lineno   = lineno;
    allocs[count].len      = len;
    allocs[count].pointer  = p;
    allocs[count].counter  = allocCounter++;

    printf("malloc(%d): %s(%d) %d %p\n", allocs[count].counter, filename, lineno, (int)len, p);

    count += 1;

    return p;
}

void pyodbc_free(void* p)
{
    if (p == 0)
        return;

    for (int i = 0; i < count; i++)
    {
        if (allocs[i].pointer == p)
        {
            printf("free(%d): %s(%d) %d %p i=%d\n", allocs[i].counter, allocs[i].filename, allocs[i].lineno, (int)allocs[i].len, allocs[i].pointer, i);
            memmove(&allocs[i], &allocs[i + 1], sizeof(Allocation) * (count - i - 1));
            count -= 1;
            free(p);
            return;
        }
    }

    printf("FREE FAILED: %p\n", p);
    free(p);
}

void pyodbc_leak_check()
{
    if (count == 0)
    {
        printf("NO LEAKS\n");
    }
    else
    {
        printf("********************************************************************************\n");
        printf("%d leaks\n", count);
        for (int i = 0; i < count; i++)
            printf("LEAK: %d %s(%d) len=%d\n", allocs[i].counter, allocs[i].filename, allocs[i].lineno, allocs[i].len);
    }
}

#endif
