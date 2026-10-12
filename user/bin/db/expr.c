/*
 * db: C expressions over the stopped process, and printing values.
 *
 * Names are looked up in the selected frame's scopes (DWARF), then the
 * symbol table. $pc, $sp, $a0 ... $fa0 ... are registers. Supported: the C
 * operators except ++ -- and compound assignment, member access, indexing,
 * casts to base types and pointers to them, sizeof, and = to change a
 * variable or a register.
 */
#include "db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *src;         /* what is left to parse */
static int failed;
int eval_quiet;                 /* no error messages (displays out of scope) */

static void fail(const char *fmt, const char *arg)
{
    if (!failed && !eval_quiet) {
        char m[200];
        snprintf(m, sizeof(m), fmt, arg ? arg : "");
        err("%s", m);
    }
    failed = 1;
}

/* ---- values ---- */

static struct type *strip(struct type *t)
{
    return t;
}

static void load(struct value *v)
{
    struct type *t = strip(v->type);
    size_t n = t->size < 16 ? t->size : 16;
    if (v->loc == V_MEM) {
        memset(v->bytes, 0, sizeof(v->bytes));
        if (t->kind != T_STRUCT && t->kind != T_UNION && t->kind != T_ARRAY && n &&
            read_mem(v->addr, v->bytes, n) < 0)
            fail("cannot access memory at %s", 0);
    } else if (v->loc == V_REG) {
        u64 x = v->reg == 32 ? regs.pc : regs.x[v->reg];
        memset(v->bytes, 0, sizeof(v->bytes));
        memcpy(v->bytes, &x, 8);
    } else if (v->loc == V_FREG) {
        u64 f[32], fcsr;
        memset(v->bytes, 0, sizeof(v->bytes));
        if (fetch_fpregs(f, &fcsr) == 0)
            memcpy(v->bytes, &f[v->reg], 8);
    }
    if (v->bitfield) {
        u64 x;
        memcpy(&x, v->bytes, 8);
        x >>= v->bit_offset;
        x &= v->bitfield >= 64 ? ~0ULL : (1ULL << v->bitfield) - 1;
        if (t->is_signed && v->bitfield < 64 && (x >> (v->bitfield - 1)) & 1)
            x |= ~0ULL << v->bitfield;
        memcpy(v->bytes, &x, 8);
    }
}

static i64 as_int(struct value *v)
{
    struct type *t = v->type;
    u64 raw = 0;
    memcpy(&raw, v->bytes, 8);
    if (t->kind == T_FLOAT) {
        if (t->size == 4) {
            float f;
            memcpy(&f, v->bytes, 4);
            return (i64)f;
        }
        if (t->size == 16) {
            long double d;
            memcpy(&d, v->bytes, 16);
            return (i64)d;
        }
        double d;
        memcpy(&d, v->bytes, 8);
        return (i64)d;
    }
    if (t->kind == T_ARRAY || t->kind == T_FUNC)
        return (i64)v->addr;            /* decays to a pointer */
    if (v->bitfield)
        return (i64)raw;
    switch (t->size) {
    case 1: return t->is_signed && !(t->is_char && !t->is_signed) ? (i64)(int8_t)raw : (i64)(u8)raw;
    case 2: return t->is_signed ? (i64)(int16_t)raw : (i64)(u16)raw;
    case 4: return t->is_signed ? (i64)(int32_t)raw : (i64)(u32)raw;
    default: return (i64)raw;
    }
}

static long double as_float(struct value *v)
{
    struct type *t = v->type;
    if (t->kind == T_FLOAT) {
        if (t->size == 4) {
            float f;
            memcpy(&f, v->bytes, 4);
            return f;
        }
        if (t->size == 16) {
            long double d;
            memcpy(&d, v->bytes, 16);
            return d;
        }
        double d;
        memcpy(&d, v->bytes, 8);
        return d;
    }
    i64 i = as_int(v);
    return t->is_signed ? (long double)i : (long double)(u64)i;
}

static void set_int(struct value *v, struct type *t, i64 x)
{
    memset(v, 0, sizeof(*v));
    v->type = t;
    v->loc = V_NONE;
    memcpy(v->bytes, &x, 8);
}

static void set_float(struct value *v, long double x)
{
    memset(v, 0, sizeof(*v));
    v->type = type_double();
    double d = (double)x;
    memcpy(v->bytes, &d, 8);
}

int value_as_int(struct value *v, i64 *out)
{
    if (v->type->kind == T_STRUCT || v->type->kind == T_UNION) {
        err("not a number");
        return -1;
    }
    *out = as_int(v);
    return 0;
}

/* ---- names ---- */

static int frame_scope(struct frame *f, u64 *scope_pc)
{
    if (frame_get(cur_frame, f) < 0) {
        f->pc = regs.pc;
        f->sp = regs.x[2];
        f->fp = regs.x[8];
        f->cfa = f->fp;
    }
    /* an outer frame's pc is a return address: look just before it */
    *scope_pc = cur_frame ? f->pc - 1 : f->pc;
    return 0;
}

static int var_value(struct var *var, struct value *out)
{
    memset(out, 0, sizeof(*out));
    out->type = var->type;
    if (var->has_const) {
        memcpy(out->bytes, &var->const_value, 8);
        return 0;
    }
    struct frame f;
    u64 pc;
    frame_scope(&f, &pc);
    const u8 *e;
    size_t n;
    if (!var_location(var, pc, &e, &n)) {
        fail("'%s' is not available here (optimized out)", var->name);
        return -1;
    }
    u64 fb = 0;
    int has_fb = frame_base_of(func_at(pc), &f, cur_frame, &fb);
    if (dwarf_eval(e, n, &f, cur_frame, fb, has_fb, out)) {
        fail("'%s' is not available here (optimized out, or in a register of another frame)", var->name);
        return -1;
    }
    out->type = var->type;
    out->lvalue = out->loc != V_NONE;
    if (out->loc != V_NONE)
        load(out);
    return 0;
}

static int lookup(const char *name, struct value *out)
{
    memset(out, 0, sizeof(*out));
    if (!pid) {
        fail("the program is not running", 0);
        return -1;
    }
    struct frame f;
    u64 pc;
    frame_scope(&f, &pc);
    struct var *v = var_lookup(pc, name);
    if (v)
        return var_value(v, out);
    struct func *fn = func_named(name);
    if (fn) {
        static struct type ft = { T_FUNC, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
        out->type = &ft;
        out->loc = V_MEM;
        out->addr = fn->low;
        return 0;
    }
    struct sym *s = sym_named(name);
    if (s) {
        if (s->is_func) {
            static struct type ft = { T_FUNC, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
            out->type = &ft;
            out->loc = V_MEM;
            out->addr = s->addr;
            return 0;
        }
        /* data without debug information: a number of its size */
        out->type = s->size == 1 ? type_char() : s->size == 4 ? type_int() : type_long();
        out->loc = V_MEM;
        out->addr = s->addr;
        out->lvalue = 1;
        load(out);
        return 0;
    }
    fail("No symbol \"%s\" in current context.", name);
    return -1;
}

/* ---- the parser ---- */

static void skip_ws(void)
{
    while (isspace((unsigned char)*src))
        src++;
}

static int accept(const char *tok)
{
    skip_ws();
    size_t n = strlen(tok);
    if (strncmp(src, tok, n))
        return 0;
    /* don't take '<' out of '<<' or '<=' etc. */
    if (n == 1 && strchr("<>=!&|", tok[0]) && (src[1] == '=' || (src[1] == tok[0] && tok[0] != '!')))
        return 0;
    if (n == 1 && tok[0] == '-' && src[1] == '>')
        return 0;
    src += n;
    return 1;
}

static void expr(struct value *v);
static void unary(struct value *v);

static void deref(struct value *v)
{
    struct type *t = v->type;
    if (t->kind != T_PTR && t->kind != T_ARRAY) {
        fail("attempt to take contents of a non-pointer value.", 0);
        return;
    }
    u64 addr = t->kind == T_ARRAY ? v->addr : (u64)as_int(v);
    struct type *target = t->target;
    memset(v, 0, sizeof(*v));
    v->type = target && target->kind != T_VOID ? target : type_char();
    v->loc = V_MEM;
    v->addr = addr;
    v->lvalue = 1;
    load(v);
}

static struct type *parse_typename(void)
{
    static const struct {
        const char *name;
        int size, is_signed, is_char, is_float;
    } names[] = {
        { "unsigned char", 1, 0, 1, 0 }, { "signed char", 1, 1, 1, 0 }, { "char", 1, 1, 1, 0 },
        { "unsigned short", 2, 0, 0, 0 }, { "short", 2, 1, 0, 0 },
        { "unsigned long long", 8, 0, 0, 0 }, { "long long", 8, 1, 0, 0 },
        { "unsigned long", 8, 0, 0, 0 }, { "long", 8, 1, 0, 0 },
        { "unsigned int", 4, 0, 0, 0 }, { "unsigned", 4, 0, 0, 0 }, { "int", 4, 1, 0, 0 },
        { "float", 4, 1, 0, 1 }, { "long double", 16, 1, 0, 1 }, { "double", 8, 1, 0, 1 },
        { "void", 1, 0, 0, 0 }, { "size_t", 8, 0, 0, 0 }, { "uint64_t", 8, 0, 0, 0 },
        { "int64_t", 8, 1, 0, 0 }, { "uint32_t", 4, 0, 0, 0 }, { "int32_t", 4, 1, 0, 0 },
        { "uint8_t", 1, 0, 0, 0 }, { "uintptr_t", 8, 0, 0, 0 },
    };
    skip_ws();
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t n = strlen(names[i].name);
        if (strncmp(src, names[i].name, n) || isalnum((unsigned char)src[n]) || src[n] == '_')
            continue;
        const char *save = src;
        src += n;
        struct type *t = xcalloc(1, sizeof(*t));
        t->kind = names[i].is_float ? T_FLOAT : !strcmp(names[i].name, "void") ? T_VOID : T_INT;
        t->name = xstrdup(names[i].name);
        t->size = names[i].size;
        t->is_signed = names[i].is_signed;
        t->is_char = names[i].is_char;
        skip_ws();
        while (*src == '*') {
            t = type_pointer_to(t);
            src++;
            skip_ws();
        }
        if (*src != ')') {
            src = save;             /* "long" of "long double": try the longer names */
            continue;
        }
        return t;
    }
    return 0;
}

static void primary(struct value *v)
{
    skip_ws();
    memset(v, 0, sizeof(*v));
    if (isdigit((unsigned char)*src) || (*src == '.' && isdigit((unsigned char)src[1]))) {
        char *end;
        const char *p = src;
        int is_float = 0;
        if (!(p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) {
            while (isdigit((unsigned char)*p))
                p++;
            is_float = *p == '.' || *p == 'e' || *p == 'E';
        }
        if (is_float) {
            set_float(v, strtold(src, &end));
        } else {
            u64 x = strtoull(src, &end, 0);
            set_int(v, x > 0x7fffffff ? type_long() : type_int(), (i64)x);
        }
        src = end;
        while (*src == 'u' || *src == 'U' || *src == 'l' || *src == 'L' || *src == 'f')
            src++;
        return;
    }
    if (*src == '\'') {
        src++;
        int c = (unsigned char)*src++;
        if (c == '\\') {
            c = *src++;
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? 0 : c == 'r' ? '\r' : c;
        }
        if (*src == '\'')
            src++;
        set_int(v, type_char(), c);
        return;
    }
    if (*src == '$') {
        src++;
        char name[32];
        int n = 0;
        while ((isalnum((unsigned char)*src) || *src == '_') && n < 31)
            name[n++] = *src++;
        name[n] = 0;
        if (!pid) {
            fail("the program is not running", 0);
            return;
        }
        int r = reg_number(name);
        int is_f = 0;
        if (r < 0 && name[0] == 'f') {
            /* $ft0 $fs0 $fa0 or $f5 */
            static const char *const fabi[32] = {
                "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1",
                "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
                "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
            };
            for (int i = 0; i < 32; i++)
                if (!strcmp(name, fabi[i]))
                    r = i, is_f = 1;
            if (r < 0 && isdigit((unsigned char)name[1]) && atoi(name + 1) < 32)
                r = atoi(name + 1), is_f = 1;
        }
        if (r < 0) {
            fail("unknown register $%s", name);
            return;
        }
        if (cur_frame && !is_f && r != 2 && r != 8 && r != 32) {
            /* outer frames: pc, sp and fp come from the unwinder */
        }
        v->type = is_f ? type_double() : r == 32 || r == 1 || r == 2 || r == 8 ? type_pointer_to(0)
                                                                            : type_long();
        v->loc = is_f ? V_FREG : V_REG;
        v->reg = r;
        v->lvalue = 1;
        load(v);
        if (cur_frame && !is_f && (r == 32 || r == 2 || r == 8)) {
            struct frame f;
            if (frame_get(cur_frame, &f) == 0) {
                u64 x = r == 32 ? f.pc : r == 2 ? f.sp : f.fp;
                memcpy(v->bytes, &x, 8);
                v->lvalue = 0;
            }
        }
        return;
    }
    if (*src == '"') {
        fail("string literals are not supported here", 0);
        return;
    }
    if (isalpha((unsigned char)*src) || *src == '_') {
        char name[128];
        int n = 0;
        while ((isalnum((unsigned char)*src) || *src == '_') && n < 127)
            name[n++] = *src++;
        name[n] = 0;
        if (!strcmp(name, "sizeof")) {
            skip_ws();
            const char *save = src;
            if (*src == '(') {
                src++;
                struct type *t = parse_typename();
                if (t) {
                    src++;
                    set_int(v, type_ulong(), (i64)t->size);
                    return;
                }
                src = save;
            }
            unary(v);
            set_int(v, type_ulong(), (i64)v->type->size);
            return;
        }
        lookup(name, v);
        return;
    }
    if (accept("(")) {
        expr(v);
        if (!accept(")"))
            fail("missing )", 0);
        return;
    }
    fail("syntax error near '%s'", src);
}

static void postfix(struct value *v)
{
    primary(v);
    for (;;) {
        if (failed)
            return;
        if (accept("[")) {
            struct value idx;
            expr(&idx);
            if (!accept("]"))
                fail("missing ]", 0);
            if (failed)
                return;
            struct type *t = v->type;
            if (t->kind != T_PTR && t->kind != T_ARRAY) {
                fail("cannot subscript something of this type", 0);
                return;
            }
            u64 base = t->kind == T_ARRAY ? v->addr : (u64)as_int(v);
            struct type *et = t->target && t->target->kind != T_VOID ? t->target : type_char();
            memset(v, 0, sizeof(*v));
            v->type = et;
            v->loc = V_MEM;
            v->addr = base + (u64)as_int(&idx) * et->size;
            v->lvalue = 1;
            load(v);
        } else if (accept(".") || accept("->")) {
            int arrow = src[-1] == '>';
            if (arrow)
                deref(v);
            skip_ws();
            char name[128];
            int n = 0;
            while ((isalnum((unsigned char)*src) || *src == '_') && n < 127)
                name[n++] = *src++;
            name[n] = 0;
            struct type *t = v->type;
            if (t->kind != T_STRUCT && t->kind != T_UNION) {
                fail("attempt to extract a component of a value that is not a structure", 0);
                return;
            }
            if (v->loc != V_MEM) {
                fail("the structure is not in memory", 0);
                return;
            }
            struct member *m = 0;
            for (int i = 0; i < t->nmembers; i++)
                if (!strcmp(t->members[i].name, name))
                    m = &t->members[i];
            if (!m) {
                fail("There is no member named %s.", name);
                return;
            }
            u64 addr = v->addr + m->offset;
            memset(v, 0, sizeof(*v));
            v->type = m->type;
            v->loc = V_MEM;
            v->addr = addr;
            v->lvalue = 1;
            v->bitfield = m->bit_size;
            v->bit_offset = m->bit_offset;
            if (m->bit_size)
                v->type = m->type->size ? m->type : type_int();
            load(v);
        } else {
            return;
        }
    }
}

static void unary(struct value *v)
{
    skip_ws();
    if (accept("-")) {
        unary(v);
        if (v->type->kind == T_FLOAT)
            set_float(v, -as_float(v));
        else
            set_int(v, v->type->size == 8 ? type_long() : type_int(), -as_int(v));
        return;
    }
    if (accept("+")) {
        unary(v);
        return;
    }
    if (accept("!")) {
        unary(v);
        set_int(v, type_int(), v->type->kind == T_FLOAT ? as_float(v) == 0 : as_int(v) == 0);
        return;
    }
    if (accept("~")) {
        unary(v);
        set_int(v, v->type->size == 8 ? type_long() : type_int(), ~as_int(v));
        return;
    }
    if (accept("*")) {
        unary(v);
        if (!failed)
            deref(v);
        return;
    }
    if (accept("&")) {
        unary(v);
        if (failed)
            return;
        if (v->loc != V_MEM) {
            fail("attempt to take the address of a value not in memory", 0);
            return;
        }
        struct type *t = type_pointer_to(v->type);
        u64 a = v->addr;
        set_int(v, t, (i64)a);
        return;
    }
    if (*src == '(') {
        const char *save = src;
        src++;
        struct type *t = parse_typename();
        if (t) {
            src++;                  /* ) */
            unary(v);
            if (failed)
                return;
            if (t->kind == T_FLOAT) {
                long double x = as_float(v);
                memset(v, 0, sizeof(*v));
                v->type = t;
                if (t->size == 4) {
                    float f = (float)x;
                    memcpy(v->bytes, &f, 4);
                } else if (t->size == 16) {
                    memcpy(v->bytes, &x, 16);
                } else {
                    double d = (double)x;
                    memcpy(v->bytes, &d, 8);
                }
            } else {
                i64 x = v->type->kind == T_FLOAT ? (i64)as_float(v) : as_int(v);
                set_int(v, t, x);
                memset(v->bytes, 0, sizeof(v->bytes));
                memcpy(v->bytes, &x, t->size < 8 ? t->size : 8);
            }
            return;
        }
        src = save;
    }
    postfix(v);
}

static struct type *arith_type(struct value *a, struct value *b)
{
    struct type *x = a->type, *y = b->type;
    if (x->size == 8 || y->size == 8)
        return (!x->is_signed && x->size == 8) || (!y->is_signed && y->size == 8) ? type_ulong() : type_long();
    return type_int();
}

static int is_ptr(struct value *v)
{
    return v->type->kind == T_PTR || v->type->kind == T_ARRAY;
}

static void binary(struct value *a, const char *op, struct value *b)
{
    if (failed)
        return;
    int isf = a->type->kind == T_FLOAT || b->type->kind == T_FLOAT;
    if (!strcmp(op, "+") && !is_ptr(a) && is_ptr(b)) {
        struct value t = *a;        /* int + pointer: as pointer + int */
        *a = *b;
        *b = t;
    }
    if (!strcmp(op, "+") || !strcmp(op, "-")) {
        if (is_ptr(a) && !is_ptr(b)) {
            struct type *et = a->type->target && a->type->target->size ? a->type->target : type_char();
            u64 base = a->type->kind == T_ARRAY ? a->addr : (u64)as_int(a);
            i64 d = as_int(b) * (i64)et->size;
            set_int(a, a->type->kind == T_ARRAY ? type_pointer_to(et) : a->type,
                    (i64)(op[0] == '+' ? base + d : base - d));
            return;
        }
        if (is_ptr(a) && is_ptr(b) && op[0] == '-') {
            struct type *et = a->type->target && a->type->target->size ? a->type->target : type_char();
            u64 x = a->type->kind == T_ARRAY ? a->addr : (u64)as_int(a);
            u64 y = b->type->kind == T_ARRAY ? b->addr : (u64)as_int(b);
            set_int(a, type_long(), (i64)(x - y) / (i64)et->size);
            return;
        }
    }
    if (isf && strchr("+-*/", op[0]) && !op[1]) {
        long double x = as_float(a), y = as_float(b);
        set_float(a, op[0] == '+' ? x + y : op[0] == '-' ? x - y : op[0] == '*' ? x * y : x / y);
        return;
    }
    if (isf && (op[0] == '<' || op[0] == '>' || op[0] == '=' || op[0] == '!')) {
        long double x = as_float(a), y = as_float(b);
        int r = !strcmp(op, "<") ? x < y : !strcmp(op, ">") ? x > y : !strcmp(op, "<=") ? x <= y
              : !strcmp(op, ">=") ? x >= y : !strcmp(op, "==") ? x == y : x != y;
        set_int(a, type_int(), r);
        return;
    }
    struct type *rt = is_ptr(a) ? type_ulong() : arith_type(a, b);
    i64 x = is_ptr(a) && a->type->kind == T_ARRAY ? (i64)a->addr : as_int(a);
    i64 y = is_ptr(b) && b->type->kind == T_ARRAY ? (i64)b->addr : as_int(b);
    int uns = !rt->is_signed;
    i64 r = 0;
    if (!strcmp(op, "*")) r = x * y;
    else if (!strcmp(op, "/") || !strcmp(op, "%")) {
        if (!y) {
            fail("Division by zero", 0);
            return;
        }
        if (uns)
            r = op[0] == '/' ? (i64)((u64)x / (u64)y) : (i64)((u64)x % (u64)y);
        else
            r = op[0] == '/' ? x / y : x % y;
    }
    else if (!strcmp(op, "+")) r = x + y;
    else if (!strcmp(op, "-")) r = x - y;
    else if (!strcmp(op, "<<")) r = x << y;
    else if (!strcmp(op, ">>")) r = uns ? (i64)((u64)x >> y) : x >> y;
    else if (!strcmp(op, "&")) r = x & y;
    else if (!strcmp(op, "|")) r = x | y;
    else if (!strcmp(op, "^")) r = x ^ y;
    else {
        int c;
        if (uns || is_ptr(a))
            c = !strcmp(op, "<") ? (u64)x < (u64)y : !strcmp(op, ">") ? (u64)x > (u64)y
              : !strcmp(op, "<=") ? (u64)x <= (u64)y : !strcmp(op, ">=") ? (u64)x >= (u64)y
              : !strcmp(op, "==") ? x == y : x != y;
        else
            c = !strcmp(op, "<") ? x < y : !strcmp(op, ">") ? x > y : !strcmp(op, "<=") ? x <= y
              : !strcmp(op, ">=") ? x >= y : !strcmp(op, "==") ? x == y : x != y;
        set_int(a, type_int(), c);
        return;
    }
    if (rt->size == 4)
        r = rt->is_signed ? (i64)(int32_t)r : (i64)(u32)r;
    set_int(a, rt, r);
}

/* precedence climbing over the binary operators */
static const struct {
    const char *op;
    int prec;
} ops[] = {
    { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "==", 6 }, { "!=", 6 },
    { "<=", 7 }, { ">=", 7 }, { "<<", 8 }, { ">>", 8 }, { "<", 7 }, { ">", 7 }, { "+", 9 },
    { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
};

static const char *peek_op(int *prec)
{
    skip_ws();
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        size_t n = strlen(ops[i].op);
        if (strncmp(src, ops[i].op, n))
            continue;
        if (n == 1 && (src[1] == '=' && ops[i].op[0] != '<' && ops[i].op[0] != '>'))
            continue;               /* +=, -=, ... are not ours; a = is assignment */
        if (n == 1 && strchr("<>&|", src[0]) && src[1] == src[0])
            continue;
        if (n == 1 && (src[0] == '<' || src[0] == '>') && src[1] == '=')
            continue;
        if (n == 1 && src[0] == '-' && src[1] == '>')
            continue;
        *prec = ops[i].prec;
        return ops[i].op;
    }
    return 0;
}

static void binexpr(struct value *v, int min_prec)
{
    unary(v);
    for (;;) {
        if (failed)
            return;
        int prec;
        const char *op = peek_op(&prec);
        if (!op || prec < min_prec)
            return;
        src += strlen(op);
        if (!strcmp(op, "&&") || !strcmp(op, "||")) {
            int left = v->type->kind == T_FLOAT ? as_float(v) != 0 : as_int(v) != 0;
            struct value rhs;
            binexpr(&rhs, prec + 1);
            int right = rhs.type && (rhs.type->kind == T_FLOAT ? as_float(&rhs) != 0 : as_int(&rhs) != 0);
            set_int(v, type_int(), op[0] == '&' ? left && right : left || right);
            continue;
        }
        struct value rhs;
        binexpr(&rhs, prec + 1);
        binary(v, op, &rhs);
    }
}

static void expr(struct value *v)
{
    binexpr(v, 1);
    if (failed)
        return;
    skip_ws();
    if (*src == '?') {
        src++;
        int c = v->type->kind == T_FLOAT ? as_float(v) != 0 : as_int(v) != 0;
        struct value a, b;
        expr(&a);
        if (!accept(":")) {
            fail("missing : in ?:", 0);
            return;
        }
        expr(&b);
        *v = c ? a : b;
        return;
    }
    if (*src == '=' && src[1] != '=') {
        src++;
        struct value rhs;
        expr(&rhs);
        if (!failed && assign(v, &rhs) == 0)
            load(v);
    }
}

int eval(const char *text, struct value *out)
{
    src = text;
    failed = 0;
    expr(out);
    if (!failed) {
        skip_ws();
        if (*src)
            fail("junk at the end of the expression: '%s'", src);
    }
    return failed ? -1 : 0;
}

int assign(struct value *lhs, struct value *rhs)
{
    if (!lhs->lvalue) {
        err("Left operand of assignment is not an lvalue.");
        return -1;
    }
    struct type *t = lhs->type;
    u8 buf[16];
    memset(buf, 0, sizeof(buf));
    if (t->kind == T_FLOAT) {
        long double x = as_float(rhs);
        if (t->size == 4) {
            float f = (float)x;
            memcpy(buf, &f, 4);
        } else if (t->size == 16) {
            memcpy(buf, &x, 16);
        } else {
            double d = (double)x;
            memcpy(buf, &d, 8);
        }
    } else if (t->kind == T_STRUCT || t->kind == T_UNION || t->kind == T_ARRAY) {
        err("cannot assign whole aggregates");
        return -1;
    } else {
        i64 x = rhs->type->kind == T_FLOAT ? (i64)as_float(rhs) : as_int(rhs);
        memcpy(buf, &x, 8);
    }
    if (lhs->loc == V_MEM) {
        if (lhs->bitfield) {
            u64 word = 0, x;
            size_t n = t->size < 8 ? t->size : 8;
            read_mem(lhs->addr, &word, n);
            memcpy(&x, buf, 8);
            u64 mask = (lhs->bitfield >= 64 ? ~0ULL : (1ULL << lhs->bitfield) - 1) << lhs->bit_offset;
            word = (word & ~mask) | ((x << lhs->bit_offset) & mask);
            return write_mem(lhs->addr, &word, n) ? (err("cannot write memory"), -1) : 0;
        }
        if (write_mem(lhs->addr, buf, t->size < 16 ? t->size : 16) < 0) {
            err("cannot write memory at %#llx", (unsigned long long)lhs->addr);
            return -1;
        }
        return 0;
    }
    if (lhs->loc == V_REG) {
        u64 x;
        memcpy(&x, buf, 8);
        if (lhs->reg == 32)
            regs.pc = x;
        else if (lhs->reg)
            regs.x[lhs->reg] = x;
        return store_regs();
    }
    if (lhs->loc == V_FREG) {
        u64 f[32], fcsr;
        if (fetch_fpregs(f, &fcsr) < 0)
            return -1;
        memcpy(&f[lhs->reg], buf, 8);
        return store_fpregs(f, fcsr);
    }
    err("cannot assign to that");
    return -1;
}

/* ---- printing ---- */

const char *frame_desc(u64 pc, char *buf, size_t size)
{
    struct func *f = func_at(pc);
    struct sym *s = f ? 0 : sym_at(pc);
    if (f && pc != f->low)
        snprintf(buf, size, "%s+%llu", f->name, (unsigned long long)(pc - f->low));
    else if (f)
        snprintf(buf, size, "%s", f->name);
    else if (s && pc == s->addr)
        snprintf(buf, size, "%s", s->name);
    else if (s)
        snprintf(buf, size, "%s+%llu", s->name, (unsigned long long)(pc - s->addr));
    else
        snprintf(buf, size, "??");
    return buf;
}

static void print_string(u64 addr, int max)
{
    char c;
    int n = 0;
    putchar('"');
    for (; n < max; n++) {
        if (read_mem(addr + n, &c, 1) < 0) {
            printf("\" <error reading memory>");
            return;
        }
        if (!c)
            break;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c == '\n')
            printf("\\n");
        else if (c == '\t')
            printf("\\t");
        else if ((unsigned char)c < 32 || (unsigned char)c == 127)
            printf("\\%03o", (unsigned char)c);
        else
            putchar(c);
    }
    putchar('"');
    if (n == max)
        printf("...");
}

static void print_char(int c)
{
    if (c == '\'' || c == '\\')
        printf("'\\%c'", c);
    else if (c == '\n')
        printf("'\\n'");
    else if (c == 0)
        printf("'\\000'");
    else if (c < 32 || c >= 127)
        printf("'\\%03o'", c & 0xff);
    else
        printf("'%c'", c);
}

static void print_int_fmt(i64 x, struct type *t, int fmt)
{
    u64 u = (u64)x;
    if (t->size < 8)
        u &= (1ULL << (t->size * 8)) - 1;
    switch (fmt) {
    case 'x': printf("0x%llx", (unsigned long long)u); return;
    case 'o': printf("0%llo", (unsigned long long)u); return;
    case 'u': printf("%llu", (unsigned long long)u); return;
    case 'd': printf("%lld", (long long)x); return;
    case 'c': printf("%lld ", (long long)x); print_char((int)(u & 0xff)); return;
    case 't': {
        char b[65];
        int n = 0;
        for (int i = (int)t->size * 8 - 1; i >= 0; i--)
            if (n || (u >> i) & 1 || i == 0)
                b[n++] = (u >> i) & 1 ? '1' : '0';
        b[n] = 0;
        printf("%s", b);
        return;
    }
    case 'a': {
        char d[128];
        printf("0x%llx <%s>", (unsigned long long)u, frame_desc(u, d, sizeof(d)));
        return;
    }
    }
    if (t->is_signed)
        printf("%lld", (long long)x);
    else
        printf("%llu", (unsigned long long)u);
}

static void print_at(struct type *t, u64 addr, int fmt, int depth);

static void print_scalar(struct value *v, int fmt, int depth)
{
    struct type *t = v->type;
    switch (t->kind) {
    case T_VOID:
        printf("void");
        return;
    case T_BOOL:
        if (fmt)
            print_int_fmt(as_int(v), t, fmt);
        else
            printf("%s", as_int(v) ? "true" : "false");
        return;
    case T_INT:
        if (!fmt && t->is_char) {
            print_int_fmt(as_int(v), t, 0);
            putchar(' ');
            print_char((int)(as_int(v) & 0xff));
            return;
        }
        print_int_fmt(as_int(v), t, fmt);
        return;
    case T_ENUM: {
        i64 x = as_int(v);
        if (!fmt)
            for (int i = 0; i < t->nenums; i++)
                if (t->enums[i].value == x) {
                    printf("%s", t->enums[i].name);
                    return;
                }
        print_int_fmt(x, t, fmt ? fmt : 'd');
        return;
    }
    case T_FLOAT:
        if (fmt == 'x') {
            u64 raw;
            memcpy(&raw, v->bytes, 8);
            printf("0x%llx", (unsigned long long)raw);
            return;
        }
        printf("%.17Lg", as_float(v));
        return;
    case T_PTR: {
        u64 p = (u64)as_int(v);
        struct type *tt = t->target;
        if (fmt && fmt != 'a' && fmt != 's') {
            print_int_fmt((i64)p, type_ulong(), fmt);
            return;
        }
        if (tt && tt->kind == T_FUNC) {
            char d[128];
            printf("0x%llx <%s>", (unsigned long long)p, frame_desc(p, d, sizeof(d)));
            return;
        }
        char tn[128];
        if (depth == 0 && !fmt)
            printf("(%s) ", type_name(t, tn, sizeof(tn)));
        printf("0x%llx", (unsigned long long)p);
        if (tt && tt->kind == T_INT && tt->is_char && tt->size == 1 && p) {
            putchar(' ');
            print_string(p, 200);
        } else if (p && (fmt == 'a' || (p >= prog.text_start && p < prog.text_end))) {
            char d[128];
            printf(" <%s>", frame_desc(p, d, sizeof(d)));
        }
        return;
    }
    case T_FUNC: {
        char d[128];
        printf("{<text>} 0x%llx <%s>", (unsigned long long)v->addr, frame_desc(v->addr, d, sizeof(d)));
        return;
    }
    default:
        if (v->loc == V_MEM)
            print_at(t, v->addr, fmt, depth);
        else
            printf("<not in memory>");
    }
}

static void print_at(struct type *t, u64 addr, int fmt, int depth)
{
    if (depth > 6) {
        printf("{...}");
        return;
    }
    if (t->kind == T_STRUCT || t->kind == T_UNION) {
        printf("{");
        for (int i = 0; i < t->nmembers; i++) {
            struct member *m = &t->members[i];
            if (i)
                printf(", ");
            if (m->name[0])
                printf("%s = ", m->name);
            struct value mv;
            memset(&mv, 0, sizeof(mv));
            mv.type = m->type;
            mv.loc = V_MEM;
            mv.addr = addr + m->offset;
            mv.bitfield = m->bit_size;
            mv.bit_offset = m->bit_offset;
            if (m->type->kind == T_STRUCT || m->type->kind == T_UNION || m->type->kind == T_ARRAY) {
                print_at(m->type, mv.addr, fmt, depth + 1);
            } else {
                load(&mv);
                print_scalar(&mv, fmt, depth + 1);
            }
        }
        printf("}");
        return;
    }
    if (t->kind == T_ARRAY) {
        struct type *et = t->target;
        if (!fmt && et && et->kind == T_INT && et->is_char && et->size == 1) {
            print_string(addr, t->count ? (int)t->count : 200);
            return;
        }
        u64 n = t->count ? t->count : 1;
        int limit = 200;
        printf("{");
        /* runs of the same value: <repeats N times>, as gdb does */
        for (u64 i = 0; i < n && (int)i < limit; i++) {
            if (i)
                printf(", ");
            if (et->kind == T_STRUCT || et->kind == T_UNION || et->kind == T_ARRAY) {
                print_at(et, addr + i * et->size, fmt, depth + 1);
                continue;
            }
            struct value ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = et;
            ev.loc = V_MEM;
            ev.addr = addr + i * et->size;
            load(&ev);
            u64 j = i + 1;
            while (j < n) {
                struct value nv = ev;
                nv.addr = addr + j * et->size;
                load(&nv);
                if (memcmp(nv.bytes, ev.bytes, et->size < 16 ? et->size : 16))
                    break;
                j++;
            }
            print_scalar(&ev, fmt, depth + 1);
            if (j - i >= 10) {
                printf(" <repeats %llu times>", (unsigned long long)(j - i));
                i = j - 1;
            }
        }
        if ((int)n > limit)
            printf("...");
        printf("}");
        return;
    }
    struct value v;
    memset(&v, 0, sizeof(v));
    v.type = t;
    v.loc = V_MEM;
    v.addr = addr;
    load(&v);
    print_scalar(&v, fmt, depth);
}

void print_value(struct value *v, int fmt)
{
    struct type *t = v->type;
    failed = 0;
    if (fmt == 's' && t->kind == T_PTR) {
        printf("0x%llx ", (unsigned long long)as_int(v));
        print_string((u64)as_int(v), 200);
        return;
    }
    if ((t->kind == T_STRUCT || t->kind == T_UNION || t->kind == T_ARRAY) && v->loc == V_MEM) {
        print_at(t, v->addr, fmt, 0);
        return;
    }
    print_scalar(v, fmt, 0);
}
