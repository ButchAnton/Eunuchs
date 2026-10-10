#include <fmt.h>
#include <stdbool.h>

enum {
    F_LEFT  = 1 << 0,
    F_PLUS  = 1 << 1,
    F_SPACE = 1 << 2,
    F_ALT   = 1 << 3,
    F_ZERO  = 1 << 4,
    F_PTR   = 1 << 5,   /* %p: always "0x", even for zero */
};

struct fmt_state {
    fmt_out_fn out;
    void *arg;
    int count;
};

static void emit(struct fmt_state *st, char c)
{
    st->out(c, st->arg);
    st->count++;
}

static void emit_repeat(struct fmt_state *st, char c, int n)
{
    while (n-- > 0)
        emit(st, c);
}

static void emit_number(struct fmt_state *st, unsigned long long v, bool neg,
                        unsigned base, bool upper, int flags, int width, int prec)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int len = 0;
    bool zero = v == 0;

    if (!(prec == 0 && v == 0)) {
        do {
            tmp[len++] = digits[v % base];
            v /= base;
        } while (v);
    }

    char prefix[3];
    int plen = 0;
    if (neg)
        prefix[plen++] = '-';
    else if (flags & F_PLUS)
        prefix[plen++] = '+';
    else if (flags & F_SPACE)
        prefix[plen++] = ' ';
    if (((flags & F_ALT) && base == 16 && !zero) || (flags & F_PTR)) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }

    int zeros = prec > len ? prec - len : 0;
    if ((flags & F_ALT) && base == 8 && zeros == 0 && (len == 0 || tmp[len - 1] != '0'))
        zeros = 1;

    int total = plen + zeros + len;
    int pad = width > total ? width - total : 0;

    if (flags & F_LEFT) {
        for (int i = 0; i < plen; i++)
            emit(st, prefix[i]);
        emit_repeat(st, '0', zeros);
        while (len)
            emit(st, tmp[--len]);
        emit_repeat(st, ' ', pad);
    } else if ((flags & F_ZERO) && prec < 0) {
        for (int i = 0; i < plen; i++)
            emit(st, prefix[i]);
        emit_repeat(st, '0', zeros + pad);
        while (len)
            emit(st, tmp[--len]);
    } else {
        emit_repeat(st, ' ', pad);
        for (int i = 0; i < plen; i++)
            emit(st, prefix[i]);
        emit_repeat(st, '0', zeros);
        while (len)
            emit(st, tmp[--len]);
    }
}

static void emit_string(struct fmt_state *st, const char *s, int flags, int width, int prec)
{
    if (!s)
        s = "(null)";
    int len = 0;
    while (s[len] && (prec < 0 || len < prec))
        len++;
    int pad = width > len ? width - len : 0;
    if (!(flags & F_LEFT))
        emit_repeat(st, ' ', pad);
    for (int i = 0; i < len; i++)
        emit(st, s[i]);
    if (flags & F_LEFT)
        emit_repeat(st, ' ', pad);
}

enum length { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF, L_INTMAX };

int kvcbprintf(fmt_out_fn out, void *arg, const char *fmt, va_list ap)
{
    struct fmt_state st = { out, arg, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&st, *fmt);
            continue;
        }
        fmt++;

        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else if (*fmt == '0') flags |= F_ZERO;
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        enum length len = L_INT;
        switch (*fmt) {
        case 'h':
            fmt++;
            if (*fmt == 'h') { len = L_CHAR; fmt++; } else len = L_SHORT;
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') { len = L_LLONG; fmt++; } else len = L_LONG;
            break;
        case 'z': len = L_SIZE; fmt++; break;
        case 't': len = L_PTRDIFF; fmt++; break;
        case 'j': len = L_INTMAX; fmt++; break;
        default: break;
        }

        char conv = *fmt;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            switch (len) {
            case L_CHAR: v = (signed char)va_arg(ap, int); break;
            case L_SHORT: v = (short)va_arg(ap, int); break;
            case L_LONG: v = va_arg(ap, long); break;
            case L_LLONG: v = va_arg(ap, long long); break;
            case L_SIZE: v = (long long)va_arg(ap, size_t); break;
            case L_PTRDIFF: v = va_arg(ap, ptrdiff_t); break;
            case L_INTMAX: v = va_arg(ap, long long); break;
            default: v = va_arg(ap, int); break;
            }
            unsigned long long mag = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            emit_number(&st, mag, v < 0, 10, false, flags, width, prec);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;
            switch (len) {
            case L_CHAR: v = (unsigned char)va_arg(ap, unsigned); break;
            case L_SHORT: v = (unsigned short)va_arg(ap, unsigned); break;
            case L_LONG: v = va_arg(ap, unsigned long); break;
            case L_LLONG: v = va_arg(ap, unsigned long long); break;
            case L_SIZE: v = va_arg(ap, size_t); break;
            case L_PTRDIFF: v = (unsigned long long)va_arg(ap, ptrdiff_t); break;
            case L_INTMAX: v = va_arg(ap, unsigned long long); break;
            default: v = va_arg(ap, unsigned); break;
            }
            unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            emit_number(&st, v, false, base, conv == 'X', flags & ~(F_PLUS | F_SPACE), width, prec);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(size_t)va_arg(ap, void *);
            emit_number(&st, v, false, 16, false, (flags & F_LEFT) | F_PTR, width, prec);
            break;
        }
        case 's':
            emit_string(&st, va_arg(ap, const char *), flags, width, prec);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!(flags & F_LEFT))
                emit_repeat(&st, ' ', pad);
            emit(&st, c);
            if (flags & F_LEFT)
                emit_repeat(&st, ' ', pad);
            break;
        }
        case '%':
            emit(&st, '%');
            break;
        case '\0':
            return st.count;
        default:
            /* Unknown conversion: print it verbatim so the bug is visible. */
            emit(&st, '%');
            emit(&st, conv);
            break;
        }
    }
    return st.count;
}

struct buf_state {
    char *buf;
    size_t size;
    size_t pos;
};

static void buf_out(char c, void *arg)
{
    struct buf_state *b = arg;
    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct buf_state b = { buf, size, 0 };
    int n = kvcbprintf(buf_out, &b, fmt, ap);
    if (size)
        buf[b.pos < size ? b.pos : size - 1] = '\0';
    return n;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
