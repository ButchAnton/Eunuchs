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

enum length { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF, L_INTMAX, L_LDOUBLE };

#ifdef FMT_FLOAT
/*
 * Floating point (user programs only: the kernel has no FP). Scaling is
 * done in 128-bit long double, so the 17 significant digits a double can
 * need come out right.
 */
static long double pow10l_(int n)
{
    long double r = 1, b = 10;
    int neg = n < 0;
    unsigned u = neg ? -n : n;
    while (u) {
        if (u & 1)
            r *= b;
        b *= b;
        u >>= 1;
    }
    return neg ? 1 / r : r;
}

/* v > 0, finite: digits (up to 18) of v rounded to `sig` significant digits,
 * and the decimal exponent of the first digit. */
static unsigned long long sig_digits(double v, int sig, int *exp10)
{
    long double x = v;
    int e = 0;
    /* estimate the exponent from the binary one, then fix it up */
    union { double d; unsigned long long u; } bits = { v };
    int e2 = (int)((bits.u >> 52) & 0x7ff) - 1023;
    e = (int)(e2 * 0.30102999566398119521);
    long double scaled = x / pow10l_(e);
    while (scaled >= 10) { scaled /= 10; e++; }
    while (scaled < 1) { scaled *= 10; e--; }
    long double m = scaled * pow10l_(sig - 1);
    unsigned long long d = (unsigned long long)m;
    long double frac = m - (long double)d;
    if (frac > 0.5L || (frac == 0.5L && (d & 1)))     /* exact halves round to even */
        d++;
    if (d >= (unsigned long long)pow10l_(sig)) {     /* 9.99.. rounded up */
        d /= 10;
        e++;
    }
    *exp10 = e;
    return d;
}

static void emit_digits_str(struct fmt_state *st, const char *s)
{
    while (*s)
        emit(st, *s++);
}

static void emit_float(struct fmt_state *st, long double lv, char conv, int flags, int width, int prec)
{
    char body[400];
    int n = 0;
    double v = (double)lv;
    union { double d; unsigned long long u; } bits = { v };
    int neg = bits.u >> 63;
    if (neg)
        v = -v;
    int upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
    char sign = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;

    if (v != v || v > 1.7976931348623157e308) {
        const char *t = v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        while (*t)
            body[n++] = *t++;
        flags &= ~F_ZERO;
    } else if (conv == 'a' || conv == 'A') {
        unsigned long long mant = bits.u & ((1ULL << 52) - 1);
        int e2 = (int)((bits.u >> 52) & 0x7ff);
        int lead = e2 != 0;
        e2 = e2 ? e2 - 1023 : (mant ? -1022 : 0);
        const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        int nd = 13;
        if (prec >= 0 && prec < 13) {
            /* round to prec hex digits */
            int drop = (13 - prec) * 4;
            unsigned long long half = 1ULL << (drop - 1);
            unsigned long long full = ((unsigned long long)lead << 52) | mant;
            full = (full + half) >> drop << drop;
            lead = (int)(full >> 52);
            mant = full & ((1ULL << 52) - 1);
            nd = prec;
        } else if (prec < 0) {
            while (nd > 0 && !((mant >> ((13 - nd) * 4)) & 0xf))
                nd--;
        }
        body[n++] = '0';
        body[n++] = upper ? 'X' : 'x';
        body[n++] = '0' + lead;
        if (nd || (flags & F_ALT))
            body[n++] = '.';
        for (int i = 0; i < nd; i++)
            body[n++] = hex[(mant >> (48 - 4 * i)) & 0xf];
        for (int i = 13; i < prec && n < 300; i++)
            body[n++] = '0';
        n += ksnprintf(body + n, sizeof(body) - n, "%c%c%d", upper ? 'P' : 'p', e2 < 0 ? '-' : '+',
                       e2 < 0 ? -e2 : e2);
    } else {
        if (prec < 0)
            prec = 6;
        int style = conv == 'e' || conv == 'E' ? 'e' : conv == 'g' || conv == 'G' ? 'g' : 'f';
        int strip = 0;
        if (style == 'g') {
            int p = prec ? prec : 1;
            int x = 0;
            if (v != 0)
                sig_digits(v, p > 17 ? 17 : p, &x);
            if (x < p && x >= -4) {
                style = 'f';
                prec = p - 1 - x;
            } else {
                style = 'e';
                prec = p - 1;
            }
            strip = !(flags & F_ALT);
        }
        char digits[400];
        int nd = 0, point;               /* digits[] with the point after `point` digits */
        if (v == 0) {
            digits[nd++] = '0';
            point = 1;
            if (style == 'f')
                for (int i = 0; i < prec && nd < 350; i++)
                    digits[nd++] = '0';
            else
                for (int i = 0; i < prec && nd < 350; i++)
                    digits[nd++] = '0';
        } else if (style == 'e') {
            int x, sig = prec + 1 > 17 ? 17 : prec + 1;
            unsigned long long d = sig_digits(v, sig, &x);
            char tmp[24];
            int k = ksnprintf(tmp, sizeof(tmp), "%llu", d);
            for (int i = 0; i < k; i++)
                digits[nd++] = tmp[i];
            while (nd < prec + 1 && nd < 350)
                digits[nd++] = '0';
            point = 1;
            /* exponent appended below */
            body[0] = 0;
            int e = x;
            /* assemble mantissa then exponent */
            int m = 0;
            char mant[400];
            mant[m++] = digits[0];
            if (prec > 0 || (flags & F_ALT))
                mant[m++] = '.';
            for (int i = 1; i < nd; i++)
                mant[m++] = digits[i];
            if (strip && m > 1) {
                while (m > 1 && mant[m - 1] == '0')
                    m--;
                if (mant[m - 1] == '.')
                    m--;
            }
            for (int i = 0; i < m; i++)
                body[n++] = mant[i];
            n += ksnprintf(body + n, sizeof(body) - n, "%c%c%02d", upper ? 'E' : 'e', e < 0 ? '-' : '+',
                           e < 0 ? -e : e);
            goto out;
        } else {
            /* %f: all integer digits, then prec decimals; at most 17 significant */
            int x;
            int total = 0;
            sig_digits(v, 17, &x);       /* exponent of the first digit */
            total = x + 1 + prec;        /* significant digits we would print */
            if (total <= 0) {
                /* rounds to 0.000..., or the last place rounds up to 1 */
                long double r = (long double)v * pow10l_(prec);
                digits[nd++] = '0';
                for (int i = 0; i < prec && nd < 350; i++)
                    digits[nd++] = '0';
                if (r > 0.5L && prec < 350)     /* 0.5 exactly rounds to the even 0 */
                    digits[nd - 1] = '1';
                point = 1;
            } else {
                int sig = total > 17 ? 17 : total;
                unsigned long long d = sig_digits(v, sig, &x);
                char tmp[24];
                int k = ksnprintf(tmp, sizeof(tmp), "%llu", d);
                if (k > sig)             /* rounding added a digit */
                    sig = k;
                /* digit string with exponent x for the first digit */
                if (x < 0) {
                    digits[nd++] = '0';
                    point = 1;
                    for (int i = 0; i < -x - 1 && nd < 350; i++)
                        digits[nd++] = '0';
                    for (int i = 0; i < k && nd < 350; i++)
                        digits[nd++] = tmp[i];
                } else {
                    for (int i = 0; i < k && nd < 350; i++)
                        digits[nd++] = tmp[i];
                    while (nd < x + 1 && nd < 350)
                        digits[nd++] = '0';
                    point = x + 1;
                }
                while (nd < point + prec && nd < 350)
                    digits[nd++] = '0';
                nd = point + prec < nd ? point + prec : nd;
            }
        }
        int m = 0;
        char out[400];
        for (int i = 0; i < point; i++)
            out[m++] = digits[i];
        if (prec > 0 || (flags & F_ALT))
            out[m++] = '.';
        for (int i = point; i < nd && m < 390; i++)
            out[m++] = digits[i];
        if (strip) {
            int has_point = 0;
            for (int i = 0; i < m; i++)
                has_point |= out[i] == '.';
            if (has_point) {
                while (m > 1 && out[m - 1] == '0')
                    m--;
                if (out[m - 1] == '.')
                    m--;
            }
        }
        for (int i = 0; i < m; i++)
            body[n++] = out[i];
    }
out:
    body[n] = 0;
    int len = n + (sign != 0);
    int pad = width > len ? width - len : 0;
    if (!(flags & F_LEFT) && !(flags & F_ZERO))
        emit_repeat(st, ' ', pad);
    if (sign)
        emit(st, sign);
    if (!(flags & F_LEFT) && (flags & F_ZERO))
        emit_repeat(st, '0', pad);
    emit_digits_str(st, body);
    if (flags & F_LEFT)
        emit_repeat(st, ' ', pad);
}
#endif

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
        case 'L': len = L_LDOUBLE; fmt++; break;
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
#ifdef FMT_FLOAT
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            long double v = len == L_LDOUBLE ? va_arg(ap, long double) : va_arg(ap, double);
            emit_float(&st, v, conv, flags, width, prec);
            break;
        }
        case 'n': {
            int *np = va_arg(ap, int *);
            if (np)
                *np = st.count;
            break;
        }
#endif
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
