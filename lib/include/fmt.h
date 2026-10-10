#ifndef EUNUCHS_FMT_H
#define EUNUCHS_FMT_H

/*
 * Freestanding printf core shared by the kernel, userland libc, and host tests.
 * Supports: flags "-+ #0", width/precision (incl. '*'), length hh h l ll z t j,
 * conversions d i u o x X p s c %.
 */

#include <stdarg.h>
#include <stddef.h>

#ifndef __printf_like
#define __printf_like(f, a) __attribute__((format(printf, f, a)))
#endif

typedef void (*fmt_out_fn)(char c, void *arg);

int kvcbprintf(fmt_out_fn out, void *arg, const char *fmt, va_list ap);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __printf_like(3, 4);

#endif
