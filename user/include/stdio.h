#ifndef _STDIO_H
#define _STDIO_H

#include <fmt.h>        /* ksnprintf/kvsnprintf; snprintf below maps onto them */
#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int printf(const char *fmt, ...) __printf_like(1, 2);
int vprintf(const char *fmt, va_list ap);
int fprintf_fd(int fd, const char *fmt, ...) __printf_like(2, 3);
int puts(const char *s);
int putchar(int c);
int getchar(void);
void fflush_stdout(void);
#define snprintf ksnprintf
#define vsnprintf kvsnprintf
#define eprintf(...) fprintf_fd(2, __VA_ARGS__)

#endif
