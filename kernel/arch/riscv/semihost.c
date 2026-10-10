/*
 * RISC-V semihosting: the magic `slli x0,x0,0x1f; ebreak; srai x0,x0,7`
 * sequence asks QEMU (run with -semihosting-config enable=on,target=native)
 * to perform host I/O. Paths are relative to QEMU's working directory (the
 * project root). Without semihosting the ebreak traps to us; trap.c
 * recognizes the sequence and returns -1, so callers just see a failure.
 */
#include <irq.h>
#include <kstring.h>

#define SYS_OPEN  0x01
#define SYS_CLOSE 0x02
#define SYS_WRITE 0x05

static long semihost_call(long op, void *params)
{
    register long a0 asm("a0") = op;
    register void *a1 asm("a1") = params;
    asm volatile(".option push\n"
                 ".option norvc\n"
                 ".balign 4\n"
                 "slli x0, x0, 0x1f\n"
                 "ebreak\n"
                 "srai x0, x0, 7\n"
                 ".option pop\n"
                 : "+r"(a0)
                 : "r"(a1)
                 : "memory");
    return a0;
}

long semihost_open(const char *path, int mode)
{
    long p[3] = { (long)path, mode, (long)strlen(path) };
    return semihost_call(SYS_OPEN, p);
}

/* Returns bytes written, or -1. */
long semihost_write(long fd, const void *buf, size_t len)
{
    long p[3] = { fd, (long)buf, (long)len };
    long not_written = semihost_call(SYS_WRITE, p);
    return not_written < 0 ? -1 : (long)len - not_written;
}

long semihost_close(long fd)
{
    long p[1] = { fd };
    return semihost_call(SYS_CLOSE, p);
}
