/*
 * bc: an arbitrary-precision calculator language (POSIX, with the common
 * GNU extensions).
 *
 *   bc [-lqsw] [FILE...]
 *
 * Reads the files, then standard input, executing each statement as soon
 * as it is complete. -l loads the math library (s c a l e j) and sets
 * scale=20. Numbers are decimal with SCALE digits after the point; ibase
 * and obase run from 2 to 16 (obase higher prints digit groups). The
 * language: expressions with ^ * / % + - (assignments and ++/-- included),
 * relations, ! && ||, if/else, while, for, break, continue, define with
 * auto and array parameters, return, print, halt, quit, strings, # and
 * comments. Variable, array and function names are lower-case words.
 * Functions have dynamic scope, as POSIX says.
 */
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================================================================ numbers */

/* value = (neg ? -1 : 1) * sum(d[i] * 10^i) * 10^-scale; d little-endian */
typedef struct {
    int neg;
    int len;                            /* digits in d (no high zeros beyond the point) */
    int scale;
    unsigned char *d;
} num;

static jmp_buf on_error;
static int scale_v, ibase_v = 10, obase_v = 10;

static void error(const char *msg)
{
    fflush(stdout);
    eprintf("bc: %s\n", msg);
    longjmp(on_error, 1);
}

static num nnew(int len, int scale)
{
    num n;
    n.neg = 0;
    n.len = len;
    n.scale = scale;
    n.d = calloc(len > 0 ? len : 1, 1);
    if (!n.d) {
        eprintf("bc: out of memory\n");
        exit(1);
    }
    return n;
}

static void nfree(num *n)
{
    free(n->d);
    n->d = 0;
}

static num ncopy(num a)
{
    num n = nnew(a.len, a.scale);
    memcpy(n.d, a.d, a.len);
    n.neg = a.neg;
    return n;
}

/* drop zero digits above the integer part's first digit; -0 is 0 */
static void ntrim(num *n)
{
    while (n->len > n->scale && n->d[n->len - 1] == 0)
        n->len--;
    int zero = 1;
    for (int i = 0; i < n->len; i++)
        if (n->d[i])
            zero = 0;
    if (zero)
        n->neg = 0;
}

static int nzero(num n)
{
    for (int i = 0; i < n.len; i++)
        if (n.d[i])
            return 0;
    return 1;
}

static num nint(long v)
{
    num n = nnew(20, 0);
    if (v < 0)
        n.neg = 1;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    int i = 0;
    do {
        n.d[i++] = u % 10;
        u /= 10;
    } while (u);
    n.len = i;
    ntrim(&n);
    return n;
}

/* A copy of A with exactly S digits after the point (truncating or padding). */
static num nrescale(num a, int s)
{
    int shift = s - a.scale;
    int len = a.len + shift;
    if (len < 1)
        len = 1;
    num n = nnew(len, s);
    n.neg = a.neg;
    for (int i = 0; i < a.len; i++) {
        int j = i + shift;
        if (j >= 0 && j < len)
            n.d[j] = a.d[i];
    }
    n.len = len;
    ntrim(&n);
    return n;
}

/* |a| vs |b| as integers of the same scale */
static int mag_cmp(const unsigned char *a, int la, const unsigned char *b, int lb)
{
    while (la > 0 && !a[la - 1])
        la--;
    while (lb > 0 && !b[lb - 1])
        lb--;
    if (la != lb)
        return la < lb ? -1 : 1;
    for (int i = la - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int ncmp(num a, num b)
{
    if (a.neg != b.neg && !(nzero(a) && nzero(b)))
        return a.neg ? -1 : 1;
    int s = a.scale > b.scale ? a.scale : b.scale;
    num x = nrescale(a, s), y = nrescale(b, s);
    int c = mag_cmp(x.d, x.len, y.d, y.len);
    nfree(&x);
    nfree(&y);
    return a.neg ? -c : c;
}

static num nadd_sub(num a, num b, int sub)
{
    int s = a.scale > b.scale ? a.scale : b.scale;
    num x = nrescale(a, s), y = nrescale(b, s);
    int yneg = y.neg ^ sub;
    int len = (x.len > y.len ? x.len : y.len) + 1;
    num r = nnew(len, s);
    if (x.neg == yneg) {
        int carry = 0;
        for (int i = 0; i < len; i++) {
            int v = (i < x.len ? x.d[i] : 0) + (i < y.len ? y.d[i] : 0) + carry;
            r.d[i] = v % 10;
            carry = v / 10;
        }
        r.neg = x.neg;
    } else {
        /* the bigger magnitude minus the smaller */
        num *big = &x, *small = &y;
        int bneg = x.neg;
        if (mag_cmp(x.d, x.len, y.d, y.len) < 0) {
            big = &y, small = &x;
            bneg = yneg;
        }
        int borrow = 0;
        for (int i = 0; i < len; i++) {
            int v = (i < big->len ? big->d[i] : 0) - (i < small->len ? small->d[i] : 0) - borrow;
            borrow = v < 0;
            r.d[i] = v + (borrow ? 10 : 0);
        }
        r.neg = bneg;
    }
    nfree(&x);
    nfree(&y);
    ntrim(&r);
    return r;
}

static num nmul(num a, num b)
{
    num r = nnew(a.len + b.len + 1, a.scale + b.scale);
    int *acc = calloc(a.len + b.len + 2, sizeof(int));
    for (int i = 0; i < a.len; i++) {
        if (!a.d[i])
            continue;
        for (int j = 0; j < b.len; j++)
            acc[i + j] += a.d[i] * b.d[j];
        if ((i & 63) == 63)             /* keep the sums small */
            for (int k = 0; k < a.len + b.len + 1; k++)
                if (acc[k] >= 10) {
                    acc[k + 1] += acc[k] / 10;
                    acc[k] %= 10;
                }
    }
    long carry = 0;
    for (int k = 0; k < a.len + b.len + 1; k++) {
        long v = acc[k] + carry;
        r.d[k] = v % 10;
        carry = v / 10;
    }
    free(acc);
    r.neg = a.neg ^ b.neg;
    int want = a.scale + b.scale;
    int mx = a.scale > b.scale ? a.scale : b.scale;
    if (mx < scale_v)
        mx = scale_v;
    if (want > mx)
        want = mx;
    num t = nrescale(r, want);
    nfree(&r);
    return t;
}

/* floor(|A| / |B|) for digit vectors (little-endian); the remainder in *rem if wanted */
static unsigned char *idiv(const unsigned char *a, int la, const unsigned char *b, int lb, int *lq)
{
    while (lb > 0 && !b[lb - 1])
        lb--;
    unsigned char *q = calloc(la + 1, 1);
    unsigned char *r = calloc(la + lb + 2, 1);
    int lr = 0;
    for (int i = la - 1; i >= 0; i--) {
        /* r = r * 10 + a[i] */
        memmove(r + 1, r, lr);
        r[0] = a[i];
        lr++;
        while (lr > 0 && !r[lr - 1])
            lr--;
        int digit = 0;
        while (mag_cmp(r, lr, b, lb) >= 0) {
            int borrow = 0;
            for (int k = 0; k < lr; k++) {
                int v = r[k] - (k < lb ? b[k] : 0) - borrow;
                borrow = v < 0;
                r[k] = v + (borrow ? 10 : 0);
            }
            while (lr > 0 && !r[lr - 1])
                lr--;
            digit++;
        }
        q[i] = digit;
    }
    free(r);
    *lq = la > 0 ? la : 1;
    return q;
}

static num ndiv(num a, num b, int s)
{
    if (nzero(b))
        error("divide by zero");
    /* a/b = (A 10^-sa) / (B 10^-sb); the quotient with s digits:
       floor(A * 10^(s + sb - sa) / B) */
    int shift = s + b.scale - a.scale;
    num x = nrescale(a, a.scale + (shift > 0 ? shift : 0));
    if (shift < 0) {
        num t = nrescale(a, a.scale + shift);
        nfree(&x);
        x = t;
    }
    int lq;
    unsigned char *q = idiv(x.d, x.len, b.d, b.len, &lq);
    num r;
    r.neg = a.neg ^ b.neg;
    r.len = lq;
    r.scale = s;
    r.d = q;
    if (r.len < s + 1) {
        num t = nnew(s + 1, s);
        memcpy(t.d, r.d, r.len);
        t.neg = r.neg;
        free(r.d);
        r = t;
    }
    nfree(&x);
    ntrim(&r);
    return r;
}

static num nmod(num a, num b)
{
    /* a - (a / b) * b, the quotient at scale, the result at max(scale + sb, sa) */
    num q = ndiv(a, b, scale_v);
    int s = scale_v + b.scale > a.scale ? scale_v + b.scale : a.scale;
    int save = scale_v;
    scale_v = s;
    num p = nmul(q, b);
    scale_v = save;
    num r = nadd_sub(a, p, 1);
    num t = nrescale(r, s);
    nfree(&q);
    nfree(&p);
    nfree(&r);
    return t;
}

static long ntolong(num a)
{
    long v = 0;
    for (int i = a.len - 1; i >= a.scale; i--) {
        v = v * 10 + a.d[i];
        if (v > 100000000)
            error("number too big");
    }
    return a.neg ? -v : v;
}

static num npow(num a, num b)
{
    if (b.scale && !nzero(nadd_sub(b, nrescale(b, 0), 1)))
        eprintf("bc: warning: non-zero scale in exponent\n");
    long e = ntolong(b);
    int neg = e < 0;
    if (neg)
        e = -e;
    if (e == 0)
        return nint(1);
    int want = a.scale * e;
    int mx = a.scale > scale_v ? a.scale : scale_v;
    if (want > mx)
        want = mx;
    /* square and multiply, keeping extra digits */
    int save = scale_v;
    scale_v = want + 10 + a.len;
    num r = nint(1), base = ncopy(a);
    while (e) {
        if (e & 1) {
            num t = nmul(r, base);
            nfree(&r);
            r = t;
        }
        e >>= 1;
        if (e) {
            num t = nmul(base, base);
            nfree(&base);
            base = t;
        }
    }
    nfree(&base);
    scale_v = save;
    if (neg) {
        num one = nint(1);
        num t = ndiv(one, r, scale_v);
        nfree(&one);
        nfree(&r);
        return t;
    }
    num t = nrescale(r, want);
    nfree(&r);
    return t;
}

static num nsqrt(num a)
{
    if (a.neg && !nzero(a))
        error("square root of a negative number");
    int s = a.scale > scale_v ? a.scale : scale_v;
    if (nzero(a))
        return nrescale(a, s);
    /* isqrt(A * 10^(2s - sa)) has s digits after the point */
    num x = nrescale(a, 2 * s);
    x.scale = 0;
    /* Newton from above: g = 10^ceil(digits/2) */
    int digits = x.len;
    num g = nnew(digits / 2 + 2, 0);
    g.d[digits / 2 + 1] = 1;
    g.len = digits / 2 + 2;
    for (;;) {
        int lq;
        unsigned char *q = idiv(x.d, x.len, g.d, g.len, &lq);
        num qn = { 0, lq, 0, q };
        num sum = nadd_sub(g, qn, 0);
        nfree(&qn);
        /* halve */
        num h = nnew(sum.len, 0);
        int rem = 0;
        for (int i = sum.len - 1; i >= 0; i--) {
            int v = rem * 10 + sum.d[i];
            h.d[i] = v / 2;
            rem = v % 2;
        }
        h.len = sum.len;
        ntrim(&h);
        nfree(&sum);
        if (ncmp(h, g) >= 0) {
            nfree(&h);
            break;
        }
        nfree(&g);
        g = h;
    }
    nfree(&x);
    g.scale = s;
    if (g.len < s + 1) {
        num t = nnew(s + 1, s);
        memcpy(t.d, g.d, g.len);
        nfree(&g);
        g = t;
    }
    ntrim(&g);
    return g;
}

/* "12.5" in base IBASE (digits 0-9 A-F, each may exceed the base) */
static num nparse(const char *s)
{
    num r = nint(0), base = nint(ibase_v);
    const char *dot = strchr(s, '.');
    for (const char *p = s; *p && p != dot; p++) {
        int dv = isdigit(*p) ? *p - '0' : *p - 'A' + 10;
        num t = nmul(r, base);
        nfree(&r);
        num dn = nint(dv);
        r = nadd_sub(t, dn, 0);
        nfree(&t);
        nfree(&dn);
    }
    if (dot) {
        int digits = strlen(dot + 1);
        if (ibase_v == 10) {
            /* exact: append the fraction */
            num f = nnew(digits + 1, digits);
            for (int i = 0; i < digits; i++)
                f.d[digits - 1 - i] = dot[1 + i] - '0' > 9 ? 9 : dot[1 + i] - '0';
            f.len = digits + 1;
            num t = nadd_sub(r, f, 0);
            nfree(&r);
            nfree(&f);
            r = t;
        } else {
            /* sum of digit / base^k, at the scale of the digits written */
            num frac = nint(0), place = nint(1);
            int save = scale_v;
            scale_v = digits + 5;
            for (int i = 0; i < digits; i++) {
                int dv = isdigit(dot[1 + i]) ? dot[1 + i] - '0' : dot[1 + i] - 'A' + 10;
                num np = ndiv(place, base, digits + 5);
                nfree(&place);
                place = np;
                num dn = nint(dv), term = nmul(place, dn);
                num nf = nadd_sub(frac, term, 0);
                nfree(&frac);
                nfree(&dn);
                nfree(&term);
                frac = nf;
            }
            scale_v = save;
            num ft = nrescale(frac, digits);
            num t = nadd_sub(r, ft, 0);
            nfree(&r);
            nfree(&frac);
            nfree(&ft);
            nfree(&place);
            r = t;
        }
        num t = nrescale(r, digits > r.scale ? digits : r.scale);
        nfree(&r);
        r = t;
    }
    nfree(&base);
    return r;
}

/* ---- output: 70 columns, a backslash at each break ---- */

static int out_col;

static void out_char(int c)
{
    if (c != '\n' && out_col >= 69) {
        fputs("\\\n", stdout);
        out_col = 0;
    }
    putchar(c);
    out_col = c == '\n' ? 0 : out_col + 1;
}

static void out_str(const char *s)
{
    while (*s)
        out_char(*s++);
}

static void nprint(num a)
{
    if (nzero(a)) {
        out_char('0');
        return;
    }
    if (a.neg)
        out_char('-');
    if (obase_v == 10) {
        int i = a.len - 1;              /* no integer part: ".5" */
        for (; i >= a.scale; i--)
            out_char('0' + a.d[i]);
        if (a.scale) {
            out_char('.');
            for (i = a.scale - 1; i >= 0; i--)
                out_char('0' + (i < a.len ? a.d[i] : 0));
        }
        return;
    }
    /* other bases: the integer part by division, the fraction by multiplication */
    int big = obase_v > 16;
    int width = 1;
    for (int b = obase_v - 1; b >= 10; b /= 10)
        width++;
    num ip = nrescale(a, 0), fp = nadd_sub(a, ip, 1), base = nint(obase_v);
    ip.neg = fp.neg = 0;
    int save = scale_v;
    char *digits = 0;
    int nd = 0, cap = 0;
    while (!nzero(ip)) {
        num q = ndiv(ip, base, 0);
        scale_v = 0;
        num p = nmul(q, base);
        num r = nadd_sub(ip, p, 1);
        if (nd == cap)
            digits = realloc(digits, cap = cap ? cap * 2 : 64);
        digits[nd++] = ntolong(r);
        nfree(&ip);
        nfree(&p);
        nfree(&r);
        ip = q;
    }
    scale_v = save;
    if (!nd && !a.scale)
        out_char('0');
    for (int i = nd - 1; i >= 0; i--) {
        if (big) {
            char b[16];
            snprintf(b, sizeof(b), " %0*d", width, digits[i]);
            out_str(b);
        } else {
            out_char("0123456789ABCDEF"[(int)digits[i]]);
        }
    }
    if (a.scale) {
        out_char('.');
        /* digits until base^k reaches 10^scale */
        num lim = nint(1), ten = nint(10), one = nint(1);
        for (int i = 0; i < a.scale; i++) {
            num t = nmul(lim, ten);
            nfree(&lim);
            lim = t;
        }
        num prod = nint(1);
        scale_v = a.scale;
        while (ncmp(prod, lim) < 0) {
            num t = nmul(fp, base);
            nfree(&fp);
            fp = t;
            num d = nrescale(fp, 0);
            long dv = ntolong(d);
            num nf = nadd_sub(fp, d, 1);
            nfree(&fp);
            nfree(&d);
            fp = nf;
            if (big) {
                char b[16];
                snprintf(b, sizeof(b), " %0*ld", width, dv);
                out_str(b);
            } else {
                out_char("0123456789ABCDEF"[dv]);
            }
            num np = nmul(prod, base);
            nfree(&prod);
            prod = np;
        }
        scale_v = save;
        nfree(&lim);
        nfree(&ten);
        nfree(&one);
        nfree(&prod);
    }
    free(digits);
    nfree(&ip);
    nfree(&fp);
    nfree(&base);
}

/* ================================================================ lexer */

enum {
    T_EOF, T_NL, T_NUM, T_NAME, T_STR, T_OP,
    K_DEFINE, K_IF, K_ELSE, K_WHILE, K_FOR, K_BREAK, K_CONTINUE, K_RETURN, K_QUIT, K_HALT,
    K_AUTO, K_PRINT, K_LENGTH, K_SCALE, K_SQRT, K_IBASE, K_OBASE, K_LAST, K_READ, K_LIMITS,
};

static const char *const keywords[] = { "define", "if", "else", "while", "for", "break", "continue", "return",
                                        "quit", "halt", "auto", "print", "length", "scale", "sqrt", "ibase",
                                        "obase", "last", "read", "limits", 0 };

struct src {
    FILE *f;
    const char *text;                   /* or a string (the math library) */
    size_t pos;
};

static struct src srcs[64];
static int nsrcs, cur_src;
static int lookc = -2;                  /* one character pushed back */

static int rawc(void)
{
    while (cur_src < nsrcs) {
        struct src *s = &srcs[cur_src];
        int c = s->text ? (s->text[s->pos] ? (unsigned char)s->text[s->pos++] : EOF) : getc(s->f);
        if (c != EOF)
            return c;
        if (s->f && s->f != stdin)
            fclose(s->f);
        cur_src++;
        return '\n';                    /* each source ends a statement */
    }
    return EOF;
}

static int nextc(void)
{
    if (lookc != -2) {
        int c = lookc;
        lookc = -2;
        return c;
    }
    return rawc();
}

static void backc(int c)
{
    lookc = c;
}

struct tok {
    int type;
    char op[4];
    char *text;
};

static struct tok tk;                   /* the current token */
static int have_tok;

static void lex(struct tok *t)
{
    int c;
    free(t->text);
    t->text = 0;
    t->op[0] = 0;
again:
    c = nextc();
    while (c == ' ' || c == '\t' || c == '\r')
        c = nextc();
    if (c == '\\') {
        int d = nextc();
        if (d == '\n')
            goto again;                 /* a continued line */
        backc(d);
    }
    if (c == EOF) {
        t->type = T_EOF;
        return;
    }
    if (c == '#') {
        while ((c = nextc()) != '\n' && c != EOF)
            ;
        t->type = c == EOF ? T_EOF : T_NL;
        return;
    }
    if (c == '/') {
        int d = nextc();
        if (d == '*') {
            int prev = 0;
            while ((c = nextc()) != EOF && !(prev == '*' && c == '/'))
                prev = c;
            goto again;
        }
        backc(d);
    }
    if (c == '\n') {
        t->type = T_NL;
        return;
    }
    if (isdigit(c) || (c >= 'A' && c <= 'F') || c == '.') {
        char buf[4096];
        int n = 0, dot = 0;
        while (isdigit(c) || (c >= 'A' && c <= 'F') || (c == '.' && !dot) || c == '\\') {
            if (c == '\\') {
                int d = nextc();
                if (d != '\n') {
                    backc(d);
                    break;
                }
                c = nextc();
                continue;
            }
            if (c == '.')
                dot = 1;
            if (n < (int)sizeof(buf) - 1)
                buf[n++] = c;
            c = nextc();
        }
        backc(c);
        buf[n] = 0;
        if (!strcmp(buf, ".")) {
            t->type = K_LAST;           /* "." is the last value */
            return;
        }
        t->type = T_NUM;
        t->text = strdup(buf);
        return;
    }
    if (islower(c) || c == '_') {
        char buf[256];
        int n = 0;
        while (islower(c) || isdigit(c) || c == '_') {
            if (n < 255)
                buf[n++] = c;
            c = nextc();
        }
        backc(c);
        buf[n] = 0;
        for (int k = 0; keywords[k]; k++)
            if (!strcmp(buf, keywords[k])) {
                t->type = K_DEFINE + k;
                return;
            }
        t->type = T_NAME;
        t->text = strdup(buf);
        return;
    }
    if (c == '"') {
        size_t cap = 64, n = 0;
        char *s = malloc(cap);
        while ((c = nextc()) != '"' && c != EOF) {
            if (n + 2 > cap)
                s = realloc(s, cap *= 2);
            s[n++] = c;
        }
        s[n] = 0;
        t->type = T_STR;
        t->text = s;
        return;
    }
    /* operators */
    t->type = T_OP;
    t->op[0] = c;
    t->op[1] = 0;
    int d = nextc();
    static const char *const two[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "^=", "==", "!=", "<=", ">=",
                                       "&&", "||", 0 };
    for (int k = 0; two[k]; k++)
        if (two[k][0] == c && two[k][1] == d) {
            t->op[1] = d;
            t->op[2] = 0;
            return;
        }
    backc(d);
    if (!strchr("+-*/%^=<>!()[]{},;", c)) {
        char m[64];
        snprintf(m, sizeof(m), "illegal character: %c", c);
        error(m);
    }
}

static struct tok *peek(void)
{
    if (!have_tok) {
        lex(&tk);
        have_tok = 1;
    }
    return &tk;
}

static void advance(void)
{
    peek();
    have_tok = 0;
}

static int is_op(const char *op)
{
    return peek()->type == T_OP && !strcmp(tk.op, op);
}

static void expect_op(const char *op)
{
    if (!is_op(op)) {
        char m[64];
        snprintf(m, sizeof(m), "syntax error: '%s' expected", op);
        error(m);
    }
    advance();
}

/* ================================================================ syntax tree */

enum {
    E_NUM, E_VAR, E_ARR, E_CALL, E_NEG, E_BIN, E_ASSIGN, E_PREINC, E_POSTINC, E_REL, E_NOT, E_AND, E_OR,
    E_LENGTH, E_SCALEF, E_SQRT, E_SPECIAL, E_READ,
    S_EXPR, S_STR, S_PRINT, S_BLOCK, S_IF, S_WHILE, S_FOR, S_BREAK, S_CONTINUE, S_RETURN, S_HALT, S_NONE,
};

struct node {
    int type;
    char op[4];
    char *name;
    num value;
    struct node *a, *b, *c, *d;         /* operands; statements: cond, body, else / init, cond, step, body */
    struct node **list;                 /* call arguments, block statements, print items */
    int nlist;
};

static struct node *mknode(int type)
{
    struct node *n = calloc(1, sizeof(*n));
    n->type = type;
    return n;
}

static void push(struct node *n, struct node *item)
{
    n->list = realloc(n->list, (n->nlist + 1) * sizeof(*n->list));
    n->list[n->nlist++] = item;
}

static struct node *expr(void);

static int is_lvalue(struct node *n)
{
    return n->type == E_VAR || n->type == E_ARR || n->type == E_SPECIAL;
}

static struct node *primary(void)
{
    struct tok *t = peek();
    struct node *n;
    switch (t->type) {
    case T_NUM:
        n = mknode(E_NUM);
        n->name = strdup(t->text);      /* parsed at run time: ibase may change */
        advance();
        return n;
    case K_LAST:
    case K_IBASE:
    case K_OBASE:
        n = mknode(E_SPECIAL);
        n->op[0] = t->type == K_LAST ? 'l' : t->type == K_IBASE ? 'i' : 'o';
        advance();
        return n;
    case K_SCALE:
        advance();
        if (is_op("(")) {
            advance();
            n = mknode(E_SCALEF);
            n->a = expr();
            expect_op(")");
            return n;
        }
        n = mknode(E_SPECIAL);
        n->op[0] = 's';
        return n;
    case K_LENGTH:
    case K_SQRT:
        n = mknode(t->type == K_LENGTH ? E_LENGTH : E_SQRT);
        advance();
        expect_op("(");
        n->a = expr();
        expect_op(")");
        return n;
    case K_READ:
        advance();
        expect_op("(");
        expect_op(")");
        return mknode(E_READ);
    case T_NAME: {
        char *name = strdup(t->text);
        advance();
        if (is_op("[")) {
            advance();
            n = mknode(E_ARR);
            n->name = name;
            if (is_op("]")) {
                n->op[0] = 'A';         /* name[]: the whole array, as an argument */
                advance();
                return n;
            }
            n->a = expr();
            expect_op("]");
            return n;
        }
        if (is_op("(")) {
            advance();
            n = mknode(E_CALL);
            n->name = name;
            if (!is_op(")")) {
                for (;;) {
                    push(n, expr());
                    if (!is_op(","))
                        break;
                    advance();
                }
            }
            expect_op(")");
            return n;
        }
        n = mknode(E_VAR);
        n->name = name;
        return n;
    }
    case T_OP:
        if (!strcmp(t->op, "(")) {
            advance();
            n = expr();
            expect_op(")");
            return n;
        }
        break;
    }
    error("syntax error");
    return 0;
}

static struct node *postfix(void)
{
    struct node *n = primary();
    if ((is_op("++") || is_op("--")) && is_lvalue(n)) {
        struct node *p = mknode(E_POSTINC);
        p->op[0] = tk.op[0];
        p->a = n;
        advance();
        return p;
    }
    return n;
}

static struct node *unary(void)
{
    if (is_op("++") || is_op("--")) {
        char op = tk.op[0];
        advance();
        struct node *n = mknode(E_PREINC);
        n->op[0] = op;
        n->a = unary();
        if (!is_lvalue(n->a))
            error("syntax error: ++/-- needs a variable");
        return n;
    }
    if (is_op("-")) {
        advance();
        struct node *n = mknode(E_NEG);
        n->a = unary();
        return n;
    }
    if (is_op("!")) {                   /* inside an operand, as in a = !b */
        advance();
        struct node *n = mknode(E_NOT);
        n->a = unary();
        return n;
    }
    return postfix();
}

static struct node *power(void)
{
    struct node *n = unary();
    if (is_op("^")) {
        advance();
        struct node *p = mknode(E_BIN);
        p->op[0] = '^';
        p->a = n;
        p->b = power();
        return p;
    }
    return n;
}

static struct node *mulexpr(void)
{
    struct node *n = power();
    while (is_op("*") || is_op("/") || is_op("%")) {
        struct node *p = mknode(E_BIN);
        p->op[0] = tk.op[0];
        advance();
        p->a = n;
        p->b = power();
        n = p;
    }
    return n;
}

static struct node *addexpr(void)
{
    struct node *n = mulexpr();
    while (is_op("+") || is_op("-")) {
        struct node *p = mknode(E_BIN);
        p->op[0] = tk.op[0];
        advance();
        p->a = n;
        p->b = mulexpr();
        n = p;
    }
    return n;
}

static struct node *assign(void)
{
    struct node *n = addexpr();
    if (peek()->type == T_OP && (!strcmp(tk.op, "=") || (tk.op[1] == '=' && strchr("+-*/%^", tk.op[0])))) {
        if (!is_lvalue(n))
            error("syntax error: assignment to a non-variable");
        struct node *p = mknode(E_ASSIGN);
        strcpy(p->op, tk.op);
        advance();
        p->a = n;
        p->b = assign();
        return p;
    }
    return n;
}

static struct node *relexpr(void)
{
    struct node *n = assign();
    static const char *const rel[] = { "==", "!=", "<", "<=", ">", ">=", 0 };
    for (int k = 0; rel[k]; k++)
        if (is_op(rel[k])) {
            struct node *p = mknode(E_REL);
            strcpy(p->op, tk.op);
            advance();
            p->a = n;
            p->b = assign();
            return p;
        }
    return n;
}

static struct node *notexpr(void)
{
    if (is_op("!")) {
        advance();
        struct node *n = mknode(E_NOT);
        n->a = notexpr();
        return n;
    }
    return relexpr();
}

static struct node *andexpr(void)
{
    struct node *n = notexpr();
    while (is_op("&&")) {
        advance();
        struct node *p = mknode(E_AND);
        p->a = n;
        p->b = notexpr();
        n = p;
    }
    return n;
}

static struct node *expr(void)
{
    struct node *n = andexpr();
    while (is_op("||")) {
        advance();
        struct node *p = mknode(E_OR);
        p->a = n;
        p->b = andexpr();
        n = p;
    }
    return n;
}

static void skip_newlines(void)
{
    while (peek()->type == T_NL)
        advance();
}

struct func {
    char *name;
    char **params;
    int *param_array;
    int nparams;
    char **autos;
    int *auto_array;
    int nautos;
    struct node *body;
    struct func *next;
};

static struct func *funcs;
static int in_function, loop_depth;

static struct node *statement(void);

static void end_of_statement(void)
{
    if (peek()->type == T_NL || peek()->type == T_EOF || is_op(";") || is_op("}"))
        return;
    error("syntax error");
}

static struct node *statement(void)
{
    struct tok *t = peek();
    struct node *n;
    switch (t->type) {
    case T_STR:
        n = mknode(S_STR);
        n->name = strdup(t->text);
        advance();
        return n;
    case K_PRINT:
        advance();
        n = mknode(S_PRINT);
        for (;;) {
            if (peek()->type == T_STR) {
                struct node *s = mknode(S_STR);
                s->name = strdup(tk.text);
                advance();
                push(n, s);
            } else {
                push(n, expr());
            }
            if (!is_op(","))
                break;
            advance();
        }
        return n;
    case K_IF: {
        advance();
        n = mknode(S_IF);
        expect_op("(");
        n->a = expr();
        expect_op(")");
        skip_newlines();
        n->b = statement();
        if (peek()->type == K_ELSE) {
            advance();
            skip_newlines();
            n->c = statement();
        }
        return n;
    }
    case K_WHILE:
        advance();
        n = mknode(S_WHILE);
        expect_op("(");
        n->a = expr();
        expect_op(")");
        skip_newlines();
        loop_depth++;
        n->b = statement();
        loop_depth--;
        return n;
    case K_FOR:
        advance();
        n = mknode(S_FOR);
        expect_op("(");
        n->a = is_op(";") ? 0 : expr();
        expect_op(";");
        n->b = is_op(";") ? 0 : expr();
        expect_op(";");
        n->c = is_op(")") ? 0 : expr();
        expect_op(")");
        skip_newlines();
        loop_depth++;
        n->d = statement();
        loop_depth--;
        return n;
    case K_BREAK:
    case K_CONTINUE:
        if (!loop_depth)
            error(t->type == K_BREAK ? "break outside a loop" : "continue outside a loop");
        n = mknode(t->type == K_BREAK ? S_BREAK : S_CONTINUE);
        advance();
        return n;
    case K_RETURN:
        if (!in_function)
            error("return outside a function");
        advance();
        n = mknode(S_RETURN);
        if (peek()->type != T_NL && peek()->type != T_EOF && !is_op(";") && !is_op("}")) {
            if (is_op("(")) {
                advance();
                n->a = is_op(")") ? 0 : expr();
                expect_op(")");
            } else {
                n->a = expr();
            }
        }
        return n;
    case K_QUIT:
        /* at parse time, wherever it is (POSIX) */
        fflush(stdout);
        exit(0);
    case K_HALT:
        advance();
        return mknode(S_HALT);
    case K_LIMITS:
        advance();
        n = mknode(S_STR);
        n->name = strdup("BC_BASE_MAX     = 16\nBC_DIM_MAX      = 65535\nBC_SCALE_MAX    = 2147483647\n"
                         "BC_STRING_MAX   = 2147483647\n");
        return n;
    case T_OP:
        if (!strcmp(t->op, "{")) {
            advance();
            n = mknode(S_BLOCK);
            for (;;) {
                while (peek()->type == T_NL || is_op(";"))
                    advance();
                if (is_op("}"))
                    break;
                if (peek()->type == T_EOF)
                    error("unexpected end of input: '}' expected");
                push(n, statement());
                end_of_statement();
            }
            advance();
            return n;
        }
        if (!strcmp(t->op, ";"))
            return mknode(S_NONE);
        break;
    }
    n = mknode(S_EXPR);
    n->a = expr();
    return n;
}

static void define(void)
{
    advance();                          /* define */
    if (peek()->type != T_NAME)
        error("syntax error: function name expected");
    struct func *f = calloc(1, sizeof(*f));
    f->name = strdup(tk.text);
    advance();
    expect_op("(");
    while (!is_op(")")) {
        if (peek()->type != T_NAME)
            error("syntax error in parameters");
        f->params = realloc(f->params, (f->nparams + 1) * sizeof(char *));
        f->param_array = realloc(f->param_array, (f->nparams + 1) * sizeof(int));
        f->params[f->nparams] = strdup(tk.text);
        f->param_array[f->nparams] = 0;
        advance();
        if (is_op("[")) {
            advance();
            expect_op("]");
            f->param_array[f->nparams] = 1;
        }
        f->nparams++;
        if (is_op(","))
            advance();
        else if (!is_op(")"))
            error("syntax error in parameters");
    }
    advance();
    skip_newlines();
    expect_op("{");
    while (peek()->type == T_NL || is_op(";"))
        advance();
    if (peek()->type == K_AUTO) {
        advance();
        for (;;) {
            if (peek()->type != T_NAME)
                error("syntax error in auto list");
            f->autos = realloc(f->autos, (f->nautos + 1) * sizeof(char *));
            f->auto_array = realloc(f->auto_array, (f->nautos + 1) * sizeof(int));
            f->autos[f->nautos] = strdup(tk.text);
            f->auto_array[f->nautos] = 0;
            advance();
            if (is_op("[")) {
                advance();
                expect_op("]");
                f->auto_array[f->nautos] = 1;
            }
            f->nautos++;
            if (!is_op(","))
                break;
            advance();
        }
        end_of_statement();
    }
    /* the body: the statements up to the matching } */
    in_function = 1;
    struct node *body = mknode(S_BLOCK);
    for (;;) {
        while (peek()->type == T_NL || is_op(";"))
            advance();
        if (is_op("}"))
            break;
        if (peek()->type == T_EOF)
            error("unexpected end of input in a function");
        push(body, statement());
        end_of_statement();
    }
    advance();
    in_function = 0;
    f->body = body;
    /* replace an earlier definition */
    for (struct func **pp = &funcs; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, f->name)) {
            f->next = (*pp)->next;
            *pp = f;
            return;
        }
    f->next = funcs;
    funcs = f;
}

/* ================================================================ run time */

struct array {
    num *v;
    int n;
};

struct var {
    char *name;
    num value;
    int has_value;
    struct array *arr;
    struct var *next;
};

#define VHASH 211
static struct var *vars[VHASH];
static num last_v;

static struct var *lookup(const char *name)
{
    unsigned h = 0;
    for (const char *p = name; *p; p++)
        h = h * 31 + *p;
    for (struct var *v = vars[h % VHASH]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    struct var *v = calloc(1, sizeof(*v));
    v->name = strdup(name);
    v->next = vars[h % VHASH];
    vars[h % VHASH] = v;
    return v;
}

static struct array *get_array(struct var *v)
{
    if (!v->arr)
        v->arr = calloc(1, sizeof(struct array));
    return v->arr;
}

static num *array_slot(struct var *v, long i)
{
    if (i < 0 || i > 65535)
        error("array index out of range");
    struct array *a = get_array(v);
    if (i >= a->n) {
        int n = i + 16;
        a->v = realloc(a->v, n * sizeof(num));
        for (int k = a->n; k < n; k++)
            a->v[k] = nint(0);
        a->n = n;
    }
    return &a->v[i];
}

static struct array *copy_array(struct array *src)
{
    struct array *a = calloc(1, sizeof(*a));
    if (src && src->n) {
        a->v = malloc(src->n * sizeof(num));
        for (int k = 0; k < src->n; k++)
            a->v[k] = ncopy(src->v[k]);
        a->n = src->n;
    }
    return a;
}

static void free_array(struct array *a)
{
    if (!a)
        return;
    for (int k = 0; k < a->n; k++)
        nfree(&a->v[k]);
    free(a->v);
    free(a);
}

enum { FLOW_NONE, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN };
static num ret_value;
static int call_depth;

static num eval(struct node *n);
static int exec(struct node *n);

static num *lvalue(struct node *n)
{
    if (n->type == E_VAR) {
        struct var *v = lookup(n->name);
        if (!v->has_value) {
            v->value = nint(0);
            v->has_value = 1;
        }
        return &v->value;
    }
    if (n->type == E_ARR) {
        num i = eval(n->a);
        long idx = ntolong(i);
        nfree(&i);
        return array_slot(lookup(n->name), idx);
    }
    return 0;
}

static num get_special(char which)
{
    switch (which) {
    case 's': return nint(scale_v);
    case 'i': return nint(ibase_v);
    case 'o': return nint(obase_v);
    default: return ncopy(last_v);
    }
}

static void set_special(char which, num v)
{
    long x = ntolong(v);
    switch (which) {
    case 's':
        if (x < 0)
            error("negative scale");
        scale_v = x;
        break;
    case 'i':
        if (x < 2 || x > 16)
            error("ibase must be between 2 and 16");
        ibase_v = x;
        break;
    case 'o':
        if (x < 2 || x > 999)
            error("obase must be between 2 and 999");
        obase_v = x;
        break;
    default:
        nfree(&last_v);
        last_v = ncopy(v);
    }
}

static num binop(char op, num a, num b)
{
    switch (op) {
    case '+': return nadd_sub(a, b, 0);
    case '-': return nadd_sub(a, b, 1);
    case '*': return nmul(a, b);
    case '/': return ndiv(a, b, scale_v);
    case '%': return nmod(a, b);
    default: return npow(a, b);
    }
}

static num call(struct node *n)
{
    struct func *f = funcs;
    while (f && strcmp(f->name, n->name))
        f = f->next;
    if (!f) {
        char m[128];
        snprintf(m, sizeof(m), "function %s not defined", n->name);
        error(m);
    }
    if (n->nlist != f->nparams) {
        char m[128];
        snprintf(m, sizeof(m), "function %s: wrong number of arguments", n->name);
        error(m);
    }
    if (++call_depth > 2000)
        error("function calls nested too deeply");
    /* evaluate the arguments in the caller's scope */
    num *vals = calloc(f->nparams + 1, sizeof(num));
    struct array **arrs = calloc(f->nparams + 1, sizeof(*arrs));
    for (int i = 0; i < f->nparams; i++) {
        if (f->param_array[i]) {
            struct node *a = n->list[i];
            if (a->type != E_ARR || a->op[0] != 'A')
                error("an array parameter needs name[] as its argument");
            arrs[i] = copy_array(lookup(a->name)->arr);
        } else {
            if (n->list[i]->type == E_ARR && n->list[i]->op[0] == 'A')
                error("an array given for a number parameter");
            vals[i] = eval(n->list[i]);
        }
    }
    /* save what the names held, then bind (dynamic scope) */
    int nsave = f->nparams + f->nautos;
    struct { num value; int has; struct array *arr; } *saved = calloc(nsave + 1, sizeof(*saved));
    for (int i = 0; i < nsave; i++) {
        int is_param = i < f->nparams;
        const char *name = is_param ? f->params[i] : f->autos[i - f->nparams];
        int is_arr = is_param ? f->param_array[i] : f->auto_array[i - f->nparams];
        struct var *v = lookup(name);
        if (is_arr) {
            saved[i].arr = v->arr;
            v->arr = is_param ? arrs[i] : calloc(1, sizeof(struct array));
        } else {
            saved[i].value = v->value;
            saved[i].has = v->has_value;
            v->value = is_param ? vals[i] : nint(0);
            v->has_value = 1;
        }
    }
    ret_value = nint(0);
    exec(f->body);
    num result = ret_value;
    ret_value = nint(0);
    for (int i = nsave - 1; i >= 0; i--) {
        int is_param = i < f->nparams;
        const char *name = is_param ? f->params[i] : f->autos[i - f->nparams];
        int is_arr = is_param ? f->param_array[i] : f->auto_array[i - f->nparams];
        struct var *v = lookup(name);
        if (is_arr) {
            free_array(v->arr);
            v->arr = saved[i].arr;
        } else {
            nfree(&v->value);
            v->value = saved[i].value;
            v->has_value = saved[i].has;
        }
    }
    free(saved);
    free(vals);
    free(arrs);
    call_depth--;
    return result;
}

static num eval(struct node *n)
{
    switch (n->type) {
    case E_NUM:
        return nparse(n->name);
    case E_VAR:
    case E_ARR:
        if (n->type == E_ARR && n->op[0] == 'A')
            error("an array name used as a number");
        return ncopy(*lvalue(n));
    case E_SPECIAL:
        return get_special(n->op[0]);
    case E_CALL:
        return call(n);
    case E_NEG: {
        num v = eval(n->a);
        if (!nzero(v))
            v.neg = !v.neg;
        return v;
    }
    case E_BIN: {
        num a = eval(n->a), b = eval(n->b);
        num r = binop(n->op[0], a, b);
        nfree(&a);
        nfree(&b);
        return r;
    }
    case E_ASSIGN: {
        num b = eval(n->b);
        num r;
        if (n->op[0] == '=') {
            r = b;
        } else {
            num a = n->a->type == E_SPECIAL ? get_special(n->a->op[0]) : ncopy(*lvalue(n->a));
            r = binop(n->op[0], a, b);
            nfree(&a);
            nfree(&b);
        }
        if (n->a->type == E_SPECIAL) {
            set_special(n->a->op[0], r);
        } else {
            num *slot = lvalue(n->a);
            nfree(slot);
            *slot = ncopy(r);
        }
        return r;
    }
    case E_PREINC:
    case E_POSTINC: {
        num old = n->a->type == E_SPECIAL ? get_special(n->a->op[0]) : ncopy(*lvalue(n->a));
        num one = nint(1);
        num nv = nadd_sub(old, one, n->op[0] == '-');
        nfree(&one);
        if (n->a->type == E_SPECIAL) {
            set_special(n->a->op[0], nv);
        } else {
            num *slot = lvalue(n->a);
            nfree(slot);
            *slot = ncopy(nv);
        }
        if (n->type == E_PREINC) {
            nfree(&old);
            return nv;
        }
        nfree(&nv);
        return old;
    }
    case E_REL: {
        num a = eval(n->a), b = eval(n->b);
        int c = ncmp(a, b), r;
        nfree(&a);
        nfree(&b);
        if (!strcmp(n->op, "==")) r = c == 0;
        else if (!strcmp(n->op, "!=")) r = c != 0;
        else if (!strcmp(n->op, "<")) r = c < 0;
        else if (!strcmp(n->op, "<=")) r = c <= 0;
        else if (!strcmp(n->op, ">")) r = c > 0;
        else r = c >= 0;
        return nint(r);
    }
    case E_NOT: {
        num a = eval(n->a);
        int z = nzero(a);
        nfree(&a);
        return nint(z);
    }
    case E_AND:
    case E_OR: {
        num a = eval(n->a);
        int va = !nzero(a);
        nfree(&a);
        if (n->type == E_AND ? !va : va)
            return nint(va);
        num b = eval(n->b);
        int vb = !nzero(b);
        nfree(&b);
        return nint(vb);
    }
    case E_LENGTH: {
        num a = eval(n->a);
        ntrim(&a);
        /* significant digits: all of them, less leading zeros of an integer part of 0 */
        /* significant digits: from the first non-zero one (.00123: 3, 1.000: 4) */
        int len = a.len;
        while (len > 1 && !a.d[len - 1])
            len--;
        nfree(&a);
        return nint(len);
    }
    case E_SCALEF: {
        num a = eval(n->a);
        int s = a.scale;
        nfree(&a);
        return nint(s);
    }
    case E_SQRT: {
        num a = eval(n->a);
        num r = nsqrt(a);
        nfree(&a);
        return r;
    }
    case E_READ: {
        char line[1024];
        if (!fgets(line, sizeof(line), stdin))
            return nint(0);
        line[strcspn(line, "\n")] = 0;
        return nparse(line);
    }
    }
    error("internal error");
    return nint(0);
}

static void print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': out_char('\n'); break;
            case 't': out_char('\t'); break;
            case 'a': out_char('\a'); break;
            case 'b': out_char('\b'); break;
            case 'f': out_char('\f'); break;
            case 'r': out_char('\r'); break;
            case 'q': out_char('"'); break;
            case 'e': out_char('\\'); break;
            case '\\': out_char('\\'); break;
            default: out_char('\\'); out_char(*s); break;
            }
        } else {
            out_char(*s);
        }
    }
}

static int exec(struct node *n)
{
    switch (n->type) {
    case S_NONE:
        return FLOW_NONE;
    case S_EXPR: {
        num v = eval(n->a);
        if (n->a->type != E_ASSIGN) {
            nprint(v);
            out_char('\n');
            nfree(&last_v);
            last_v = ncopy(v);
        }
        nfree(&v);
        return FLOW_NONE;
    }
    case S_STR:
        out_str(n->name);
        return FLOW_NONE;
    case S_PRINT:
        for (int i = 0; i < n->nlist; i++) {
            if (n->list[i]->type == S_STR) {
                print_escaped(n->list[i]->name);
            } else {
                num v = eval(n->list[i]);
                nprint(v);
                nfree(&last_v);
                last_v = v;
            }
        }
        return FLOW_NONE;
    case S_BLOCK:
        for (int i = 0; i < n->nlist; i++) {
            int f = exec(n->list[i]);
            if (f != FLOW_NONE)
                return f;
        }
        return FLOW_NONE;
    case S_IF: {
        num c = eval(n->a);
        int t = !nzero(c);
        nfree(&c);
        if (t)
            return exec(n->b);
        return n->c ? exec(n->c) : FLOW_NONE;
    }
    case S_WHILE:
        for (;;) {
            num c = eval(n->a);
            int t = !nzero(c);
            nfree(&c);
            if (!t)
                break;
            int f = exec(n->b);
            if (f == FLOW_BREAK)
                break;
            if (f == FLOW_RETURN)
                return f;
        }
        return FLOW_NONE;
    case S_FOR:
        if (n->a) {
            num v = eval(n->a);
            nfree(&v);
        }
        for (;;) {
            if (n->b) {
                num c = eval(n->b);
                int t = !nzero(c);
                nfree(&c);
                if (!t)
                    break;
            }
            int f = exec(n->d);
            if (f == FLOW_BREAK)
                break;
            if (f == FLOW_RETURN)
                return f;
            if (n->c) {
                num v = eval(n->c);
                nfree(&v);
            }
        }
        return FLOW_NONE;
    case S_BREAK:
        return FLOW_BREAK;
    case S_CONTINUE:
        return FLOW_CONTINUE;
    case S_RETURN:
        nfree(&ret_value);
        ret_value = n->a ? eval(n->a) : nint(0);
        return FLOW_RETURN;
    case S_HALT:
        fflush(stdout);
        exit(0);
    }
    return FLOW_NONE;
}

/* ================================================================ math library */

static const char mathlib[] =
    "scale = 20\n"
    "define e(x) {\n"
    "  auto a, i, m, n, s, v\n"
    "  s = scale\n"
    "  if (x < 0) { m = 1; x = -x }\n"
    "  scale = s + 10\n"
    "  n = 0\n"
    "  while (x > 1) { x = x / 2; n = n + 1 }\n"
    "  scale = s + 10 + n\n"
    "  v = 1; a = 1\n"
    "  for (i = 1; 1; i++) { a = a * x / i; if (a == 0) break; v = v + a }\n"
    "  while (n > 0) { v = v * v; n = n - 1 }\n"
    "  scale = s\n"
    "  if (m) return (1 / v)\n"
    "  return (v / 1)\n"
    "}\n"
    "define l(x) {\n"
    "  auto a, i, m, n, s, t, v, y\n"
    "  if (x <= 0) { print \"bc: l() of a non-positive number\\n\"; return (0) }\n"
    "  s = scale\n"
    "  scale = s + 10\n"
    "  n = 0\n"
    "  while (x >= 2) { x = sqrt(x); n = n + 1 }\n"
    "  while (x <= .5) { x = sqrt(x); n = n + 1 }\n"
    "  y = (x - 1) / (x + 1)\n"
    "  t = y * y; v = y; a = y\n"
    "  for (i = 3; 1; i = i + 2) { a = a * t; m = a / i; if (m == 0) break; v = v + m }\n"
    "  v = 2 * v * 2 ^ n\n"
    "  scale = s\n"
    "  return (v / 1)\n"
    "}\n"
    "define a(x) {\n"
    "  auto a, i, m, n, q, s, t, v\n"
    "  s = scale\n"
    "  scale = s + 10\n"
    "  if (x < 0) { m = 1; x = -x }\n"
    "  n = 0\n"
    "  while (x > .2) { x = x / (1 + sqrt(1 + x * x)); n = n + 1 }\n"
    "  t = x * x; v = x; a = x\n"
    "  for (i = 3; 1; i = i + 2) { a = -a * t; q = a / i; if (q == 0) break; v = v + q }\n"
    "  v = v * 2 ^ n\n"
    "  if (m) v = -v\n"
    "  scale = s\n"
    "  return (v / 1)\n"
    "}\n"
    "define s(x) {\n"
    "  auto a, i, m, n, p, q, s, t, v\n"
    "  s = scale\n"
    "  scale = s + 10\n"
    "  if (x < 0) { m = 1; x = -x }\n"
    "  p = 4 * a(1)\n"
    "  q = 2 * p\n"
    "  scale = 0; n = x / q; scale = s + 10\n"
    "  x = x - n * q\n"
    "  if (x > p) { x = x - p; m = 1 - m }\n"
    "  t = x * x; v = x; a = x\n"
    "  for (i = 3; 1; i = i + 2) { a = -a * t / ((i - 1) * i); if (a == 0) break; v = v + a }\n"
    "  if (m) v = -v\n"
    "  scale = s\n"
    "  return (v / 1)\n"
    "}\n"
    "define c(x) {\n"
    "  auto s, v\n"
    "  s = scale\n"
    "  scale = s + 10\n"
    "  v = s(x + 2 * a(1))\n"
    "  scale = s\n"
    "  return (v / 1)\n"
    "}\n"
    "define j(n, x) {\n"
    "  auto a, b, i, m, s, v\n"
    "  s = scale\n"
    "  scale = 0; n = n / 1\n"
    "  if (n < 0) { n = -n; if (n % 2 == 1) m = 1 }\n"
    "  scale = s + 10\n"
    "  a = 1\n"
    "  for (i = 1; i <= n; i++) a = a * x / (2 * i)\n"
    "  v = a; b = -(x * x) / 4\n"
    "  for (i = 1; 1; i++) { a = a * b / (i * (n + i)); if (a == 0) break; v = v + a }\n"
    "  if (m) v = -v\n"
    "  scale = s\n"
    "  return (v / 1)\n"
    "}\n";

/* ================================================================ main */

int main(int argc, char **argv)
{
    int mathl = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--mathlib")) { mathl = 1; continue; }
        if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--standard") || !strcmp(argv[i], "--warn"))
            continue;
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') mathl = 1;
            else if (*f == 'q' || *f == 's' || *f == 'w') ;
            else {
                eprintf("usage: bc [-lqsw] [FILE...]\n");
                return 1;
            }
        }
    }
    if (mathl)
        srcs[nsrcs++].text = mathlib;
    for (; i < argc && nsrcs < 63; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            eprintf("bc: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        srcs[nsrcs++].f = f;
    }
    srcs[nsrcs++].f = stdin;
    last_v = nint(0);
    ret_value = nint(0);
    for (;;) {
        if (setjmp(on_error)) {
            /* skip the rest of the line after an error */
            in_function = loop_depth = call_depth = 0;
            if (have_tok && tk.type != T_NL && tk.type != T_EOF) {
                int c;
                while ((c = nextc()) != '\n' && c != EOF)
                    ;
            }
            have_tok = 0;
            if (out_col)
                out_char('\n');
            continue;
        }
        while (peek()->type == T_NL || is_op(";"))
            advance();
        if (peek()->type == T_EOF)
            break;
        if (peek()->type == K_DEFINE) {
            define();
            continue;
        }
        struct node *s = statement();
        end_of_statement();
        if (is_op("}"))
            error("syntax error: unexpected '}'");
        exec(s);
        fflush(stdout);
    }
    fflush(stdout);
    return 0;
}
