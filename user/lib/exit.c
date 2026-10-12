/*
 * exit and atexit, in an object of their own: tcc -run links its own
 * versions (from runmain.o), which must not clash with the rest of libc.
 */
#include <stdlib.h>
#include <unistd.h>

static void (*exit_fns[32])(void);
static int nexit_fns;

int atexit(void (*fn)(void))
{
    if (nexit_fns == 32)
        return -1;
    exit_fns[nexit_fns++] = fn;
    return 0;
}

void __stdio_flush_all(void) __attribute__((weak));

void exit(int status)
{
    while (nexit_fns > 0)
        exit_fns[--nexit_fns]();
    if (__stdio_flush_all)
        __stdio_flush_all();
    _exit(status);
}
