/* segv KIND: crash on purpose to see the kernel's fault report. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int limit = 1 << 30;    /* volatile: GCC can't prove it's infinite */

static int __attribute__((noinline)) recurse(int n)
{
    volatile char pad[512];
    pad[0] = (char)n;
    if (n >= limit)
        return 0;
    return recurse(n + 1) + pad[0];
}

static void __attribute__((noinline)) deref(int *p)
{
    printf("value: %d\n", *p);
}

int main(int argc, char **argv)
{
    const char *kind = argc > 1 ? argv[1] : "null";
    printf("segv: %s\n", kind);
    if (!strcmp(kind, "null"))
        deref((int *)0);
    else if (!strcmp(kind, "text"))
        *(volatile char *)(void *)main = 0;            /* write to r-x */
    else if (!strcmp(kind, "stack"))
        recurse(0);
    else if (!strcmp(kind, "illegal"))
        asm volatile(".word 0");
    else if (!strcmp(kind, "kernel"))
        deref((int *)0xffffffc080200000UL);             /* kernel memory */
    else
        printf("kinds: null text stack illegal kernel\n");
    return 1;
}
