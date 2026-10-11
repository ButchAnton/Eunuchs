/* esh: arithmetic expressions for $(( )), (( )), let, array subscripts. */
#include "esh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ar {
    const char *s;
    int pos;
    int err;
    int noeval;         /* inside an untaken branch: no side effects */
    int depth;
    char lv[64];        /* name of the last plain variable read (an lvalue) */
};

static long long expr_comma(struct ar *a);
static long long expr_assign(struct ar *a);

static void ws(struct ar *a)
{
    while (is_space(a->s[a->pos]))
        a->pos++;
}

static void fail(struct ar *a, const char *msg)
{
    if (!a->err)
        sh_error("math error: %s", msg);
    a->err = 1;
}

static int peekop(struct ar *a, const char *op)
{
    ws(a);
    int n = strlen(op);
    return !strncmp(a->s + a->pos, op, n);
}

static int take(struct ar *a, const char *op)
{
    if (!peekop(a, op))
        return 0;
    a->pos += strlen(op);
    return 1;
}

/* Is the operator at pos exactly op (not a prefix of a longer one)? */
static int take_exact(struct ar *a, const char *op, const char *not_followed)
{
    if (!peekop(a, op))
        return 0;
    char next = a->s[a->pos + strlen(op)];
    if (next && strchr(not_followed, next))
        return 0;
    a->pos += strlen(op);
    return 1;
}

static long long var_value(struct ar *a, const char *name)
{
    const char *v = var_get(name);
    if (!v || !*v)
        return 0;
    char *end;
    long long n = strtol(v, &end, 10);
    while (is_space(*end))
        end++;
    if (!*end)
        return n;
    if (a->depth > 16) {
        fail(a, "recursion too deep");
        return 0;
    }
    struct ar sub = { .s = v, .depth = a->depth + 1, .noeval = a->noeval };
    long long r = expr_comma(&sub);
    ws(&sub);
    if (sub.err || sub.s[sub.pos]) {
        fail(a, "bad value in variable");
        return 0;
    }
    return r;
}

static void assign(struct ar *a, const char *name, long long v)
{
    if (a->noeval)
        return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", v);
    if (var_set(name, buf, 0) < 0)
        a->err = 1;
}

static long long primary(struct ar *a)
{
    ws(a);
    a->lv[0] = 0;
    const char *s = a->s;
    if (s[a->pos] == '(') {
        a->pos++;
        long long v = expr_comma(a);
        if (!take(a, ")"))
            fail(a, "missing )");
        a->lv[0] = 0;
        return v;
    }
    if (is_digit(s[a->pos])) {
        long long v = 0;
        int base = 10;
        if (s[a->pos] == '0' && (s[a->pos + 1] == 'x' || s[a->pos + 1] == 'X')) {
            base = 16;
            a->pos += 2;
        } else {
            /* base#digits ? */
            int k = a->pos;
            long long b = 0;
            while (is_digit(s[k]))
                b = b * 10 + s[k++] - '0';
            if (s[k] == '#' && b >= 2 && b <= 36) {
                base = b;
                a->pos = k + 1;
            }
        }
        int any = 0;
        for (;;) {
            int c = s[a->pos], d;
            if (is_digit(c))
                d = c - '0';
            else if (is_alpha(c))
                d = to_lower(c) - 'a' + 10;
            else if (c == '_')
                { a->pos++; continue; }
            else
                break;
            if (d >= base)
                break;
            v = v * base + d;
            a->pos++;
            any = 1;
        }
        if (!any)
            fail(a, "bad number");
        if (is_name_char(s[a->pos]))
            fail(a, "bad number");
        return v;
    }
    if (is_name_start(s[a->pos])) {
        char name[64];
        int k = 0;
        while (is_name_char(s[a->pos])) {
            if (k < 63)
                name[k++] = s[a->pos];
            a->pos++;
        }
        name[k] = 0;
        /* name[index] */
        if (s[a->pos] == '[') {
            a->pos++;
            long long idx = expr_comma(a);
            if (!take(a, "]"))
                fail(a, "missing ]");
            struct var *v = var_lookup(name);
            a->lv[0] = 0;
            if (v && (v->flags & V_ARRAY)) {
                if (idx < 0)
                    idx += v->narr + 1;
                if (idx >= 1 && idx <= v->narr) {
                    char *e;
                    return strtol(v->arr[idx - 1], &e, 10);
                }
            }
            return 0;
        }
        long long v = var_value(a, name);
        snprintf(a->lv, sizeof(a->lv), "%s", name);
        return v;
    }
    if (!s[a->pos])
        fail(a, "operand expected");
    else {
        char msg[48];
        snprintf(msg, sizeof(msg), "unexpected '%c'", s[a->pos]);
        fail(a, msg);
    }
    return 0;
}

static long long postfix(struct ar *a)
{
    long long v = primary(a);
    if (a->lv[0] && (peekop(a, "++") || peekop(a, "--"))) {
        int inc = a->s[a->pos] == '+';
        a->pos += 2;
        assign(a, a->lv, v + (inc ? 1 : -1));
        a->lv[0] = 0;
    }
    return v;
}

static long long unary(struct ar *a)
{
    ws(a);
    if (take(a, "++") || take(a, "--")) {
        int inc = a->s[a->pos - 1] == '+';
        long long v = unary(a);
        if (!a->lv[0]) {
            fail(a, "lvalue required");
            return 0;
        }
        v += inc ? 1 : -1;
        assign(a, a->lv, v);
        a->lv[0] = 0;
        return v;
    }
    if (take(a, "!")) {
        long long v = !unary(a);
        a->lv[0] = 0;
        return v;
    }
    if (take(a, "~")) {
        long long v = ~unary(a);
        a->lv[0] = 0;
        return v;
    }
    if (take(a, "-")) {
        long long v = -unary(a);
        a->lv[0] = 0;
        return v;
    }
    if (take(a, "+")) {
        long long v = unary(a);
        a->lv[0] = 0;
        return v;
    }
    return postfix(a);
}

static long long power(struct ar *a)
{
    long long b = unary(a);
    if (take(a, "**")) {
        long long e = power(a);
        long long r = 1;
        if (e < 0) {
            fail(a, "negative exponent");
            return 0;
        }
        while (e-- > 0)
            r *= b;
        a->lv[0] = 0;
        return r;
    }
    return b;
}

static long long mul(struct ar *a)
{
    long long v = power(a);
    for (;;) {
        char op;
        if (take_exact(a, "*", "*="))
            op = '*';
        else if (take_exact(a, "/", "="))
            op = '/';
        else if (take_exact(a, "%", "="))
            op = '%';
        else
            return v;
        long long r = power(a);
        a->lv[0] = 0;
        if (op == '*')
            v *= r;
        else if (r == 0) {
            if (!a->noeval)
                fail(a, "division by zero");
            v = 0;
        } else
            v = op == '/' ? v / r : v % r;
    }
}

static long long add(struct ar *a)
{
    long long v = mul(a);
    for (;;) {
        if (take_exact(a, "+", "+="))
            v += mul(a);
        else if (take_exact(a, "-", "-="))
            v -= mul(a);
        else
            return v;
        a->lv[0] = 0;
    }
}

static long long shift(struct ar *a)
{
    long long v = add(a);
    for (;;) {
        if (take_exact(a, "<<", "="))
            v <<= add(a);
        else if (take_exact(a, ">>", "="))
            v >>= add(a);
        else
            return v;
        a->lv[0] = 0;
    }
}

static long long rel(struct ar *a)
{
    long long v = shift(a);
    for (;;) {
        if (take(a, "<="))
            v = v <= shift(a);
        else if (take(a, ">="))
            v = v >= shift(a);
        else if (take_exact(a, "<", "<"))
            v = v < shift(a);
        else if (take_exact(a, ">", ">"))
            v = v > shift(a);
        else
            return v;
        a->lv[0] = 0;
    }
}

static long long eq(struct ar *a)
{
    long long v = rel(a);
    for (;;) {
        if (take(a, "=="))
            v = v == rel(a);
        else if (take(a, "!="))
            v = v != rel(a);
        else
            return v;
        a->lv[0] = 0;
    }
}

static long long band(struct ar *a)
{
    long long v = eq(a);
    while (take_exact(a, "&", "&=")) {
        v &= eq(a);
        a->lv[0] = 0;
    }
    return v;
}

static long long bxor(struct ar *a)
{
    long long v = band(a);
    while (take_exact(a, "^", "=")) {
        v ^= band(a);
        a->lv[0] = 0;
    }
    return v;
}

static long long bor(struct ar *a)
{
    long long v = bxor(a);
    while (take_exact(a, "|", "|=")) {
        v |= bxor(a);
        a->lv[0] = 0;
    }
    return v;
}

static long long land(struct ar *a)
{
    long long v = bor(a);
    while (take(a, "&&")) {
        int save = a->noeval;
        if (!v)
            a->noeval = 1;
        long long r = bor(a);
        a->noeval = save;
        v = v && r;
        a->lv[0] = 0;
    }
    return v;
}

static long long lor(struct ar *a)
{
    long long v = land(a);
    while (take(a, "||")) {
        int save = a->noeval;
        if (v)
            a->noeval = 1;
        long long r = land(a);
        a->noeval = save;
        v = v || r;
        a->lv[0] = 0;
    }
    return v;
}

static long long ternary(struct ar *a)
{
    long long c = lor(a);
    if (!take(a, "?"))
        return c;
    int save = a->noeval;
    if (!c)
        a->noeval = 1;
    long long x = expr_assign(a);
    a->noeval = save;
    if (!take(a, ":")) {
        fail(a, "':' expected");
        return 0;
    }
    if (c)
        a->noeval = 1;
    long long y = ternary(a);
    a->noeval = save;
    a->lv[0] = 0;
    return c ? x : y;
}

static long long expr_assign(struct ar *a)
{
    int start = a->pos;
    long long v = ternary(a);
    if (!a->lv[0])
        return v;
    static const char *const ops[] = { "<<=", ">>=", "**=", "+=", "-=", "*=", "/=", "%=", "&=",
                                       "^=", "|=", "=", 0 };
    ws(a);
    for (int i = 0; ops[i]; i++) {
        int n = strlen(ops[i]);
        if (strncmp(a->s + a->pos, ops[i], n))
            continue;
        if (!strcmp(ops[i], "=") && a->s[a->pos + 1] == '=')
            break;
        char name[64];
        snprintf(name, sizeof(name), "%s", a->lv);
        a->pos += n;
        long long r = expr_assign(a);
        switch (ops[i][0]) {
        case '=': v = r; break;
        case '+': v += r; break;
        case '-': v -= r; break;
        case '*': if (ops[i][1] == '*') { long long p = 1; while (r-- > 0) p *= v; v = p; } else v *= r; break;
        case '/': case '%':
            if (!r) {
                fail(a, "division by zero");
                return 0;
            }
            v = ops[i][0] == '/' ? v / r : v % r;
            break;
        case '&': v &= r; break;
        case '^': v ^= r; break;
        case '|': v |= r; break;
        case '<': v <<= r; break;
        case '>': v >>= r; break;
        }
        assign(a, name, v);
        a->lv[0] = 0;
        return v;
    }
    (void)start;
    return v;
}

static long long expr_comma(struct ar *a)
{
    long long v = expr_assign(a);
    while (take(a, ","))
        v = expr_assign(a);
    return v;
}

int arith_eval(const char *expr, long long *result)
{
    struct ar a = { .s = expr };
    ws(&a);
    if (!a.s[a.pos]) {
        *result = 0;
        return 0;
    }
    long long v = expr_comma(&a);
    ws(&a);
    if (!a.err && a.s[a.pos]) {
        char msg[64];
        snprintf(msg, sizeof(msg), "syntax error near '%.20s'", a.s + a.pos);
        fail(&a, msg);
    }
    if (a.err)
        return -1;
    *result = v;
    return 0;
}
