/* scanf family: %d %i %u %o %x %X %c %s %[...] %f %e %g %p %n %%, with
 * widths, * (suppress) and the hh h l ll j z t L length modifiers. */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct in {
    FILE *f;            /* or NULL for a string */
    const char *s;
    int count;          /* characters consumed */
    int eof;
};

static int get(struct in *in)
{
    int c;
    if (in->f)
        c = fgetc(in->f);
    else
        c = *in->s ? (unsigned char)*in->s++ : EOF;
    if (c == EOF)
        in->eof = 1;
    else
        in->count++;
    return c;
}

static void unget(struct in *in, int c)
{
    if (c == EOF)
        return;
    in->count--;
    if (in->f)
        ungetc(c, in->f);
    else
        in->s--;
}

static int vscan(struct in *in, const char *fmt, va_list ap)
{
    int assigned = 0;
    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) {
            int c;
            while ((c = get(in)) != EOF && isspace(c))
                ;
            unget(in, c);
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%')
                p++;
            int c = get(in);
            if (c != (unsigned char)*p) {
                unget(in, c);
                return assigned ? assigned : (in->eof ? EOF : 0);
            }
            continue;
        }
        p++;
        int suppress = 0, width = 0, len = 0;   /* len: -2 hh -1 h 0 1 l 2 ll 3 L */
        if (*p == '*') {
            suppress = 1;
            p++;
        }
        while (isdigit((unsigned char)*p))
            width = width * 10 + *p++ - '0';
        for (;;) {
            if (*p == 'h') { len = len == -1 ? -2 : -1; p++; }
            else if (*p == 'l') { len = len == 1 ? 2 : 1; p++; }
            else if (*p == 'L') { len = 3; p++; }
            else if (*p == 'j' || *p == 'z' || *p == 't') { len = 1; p++; }
            else break;
        }
        char conv = *p;
        if (!conv)
            break;
        if (conv == 'n') {
            if (!suppress)
                *va_arg(ap, int *) = in->count;
            continue;
        }
        if (conv != 'c' && conv != '[') {
            int c;
            while ((c = get(in)) != EOF && isspace(c))
                ;
            unget(in, c);
        }
        if (!width)
            width = conv == 'c' ? 1 : 1 << 30;
        char buf[512];
        int n = 0;
        if (conv == 'c') {
            char *out = suppress ? 0 : va_arg(ap, char *);
            for (int i = 0; i < width; i++) {
                int c = get(in);
                if (c == EOF)
                    return assigned ? assigned : EOF;
                if (out)
                    out[i] = (char)c;
            }
            assigned += !suppress;
            continue;
        }
        if (conv == 's' || conv == '[') {
            char set[256];
            int neg = 0;
            memset(set, 0, sizeof(set));
            if (conv == '[') {
                p++;
                if (*p == '^') {
                    neg = 1;
                    p++;
                }
                if (*p == ']') {
                    set[(unsigned char)']'] = 1;
                    p++;
                }
                while (*p && *p != ']') {
                    if (p[1] == '-' && p[2] && p[2] != ']') {
                        for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++)
                            set[ch] = 1;
                        p += 3;
                    } else {
                        set[(unsigned char)*p++] = 1;
                    }
                }
            }
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            for (int i = 0; i < width; i++) {
                int c = get(in);
                int ok = c != EOF && (conv == 's' ? !isspace(c) : set[c] != neg);
                if (!ok) {
                    unget(in, c);
                    break;
                }
                if (out)
                    out[got] = (char)c;
                got++;
            }
            if (!got)
                return assigned ? assigned : (in->eof ? EOF : 0);
            if (out)
                out[got] = 0;
            assigned += !suppress;
            continue;
        }
        /* numbers: collect the characters that can belong to one */
        int is_float = conv == 'f' || conv == 'e' || conv == 'g' || conv == 'a' || conv == 'E' ||
                       conv == 'G' || conv == 'F' || conv == 'A';
        while (n < width && n < (int)sizeof(buf) - 1) {
            int c = get(in);
            int ok;
            if (c == EOF)
                break;
            if (is_float)
                ok = isxdigit(c) || c == '.' || c == '-' || c == '+' || c == 'x' || c == 'X' ||
                     c == 'p' || c == 'P' || c == 'i' || c == 'n' || c == 'I' || c == 'N' ||
                     c == 't' || c == 'y' || c == 'T' || c == 'Y';
            else
                ok = isxdigit(c) || ((c == '-' || c == '+') && n == 0) || c == 'x' || c == 'X';
            if (!ok) {
                unget(in, c);
                break;
            }
            buf[n++] = (char)c;
        }
        buf[n] = 0;
        char *end;
        if (is_float) {
            long double v = strtold(buf, &end);
            if (end == buf)
                return assigned ? assigned : (in->eof ? EOF : 0);
            for (char *q = buf + n; q > end; q--)    /* give back what wasn't used */
                unget(in, (unsigned char)q[-1]);
            if (!suppress) {
                if (len == 3)
                    *va_arg(ap, long double *) = v;
                else if (len >= 1)
                    *va_arg(ap, double *) = (double)v;
                else
                    *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            continue;
        }
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8
                 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 0;
        unsigned long long v = conv == 'd' || conv == 'i' ? (unsigned long long)strtoll(buf, &end, base)
                                                          : strtoull(buf, &end, base);
        if (end == buf)
            return assigned ? assigned : (in->eof ? EOF : 0);
        for (char *q = buf + n; q > end; q--)
            unget(in, (unsigned char)q[-1]);
        if (suppress)
            continue;
        if (conv == 'p')
            *va_arg(ap, void **) = (void *)(uintptr_t)v;
        else if (len == -2)
            *va_arg(ap, char *) = (char)v;
        else if (len == -1)
            *va_arg(ap, short *) = (short)v;
        else if (len >= 1)
            *va_arg(ap, long *) = (long)v;
        else
            *va_arg(ap, int *) = (int)v;
        assigned++;
    }
    return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct in in = { 0, s, 0, 0 };
    return vscan(&in, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct in in = { f, 0, 0, 0 };
    return vscan(&in, fmt, ap);
}

int vscanf(const char *fmt, va_list ap)
{
    return vfscanf(stdin, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
