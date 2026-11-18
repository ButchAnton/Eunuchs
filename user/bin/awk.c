/*
 * awk: pattern scanning and processing (POSIX, with the common extensions).
 *
 *   awk [-F FS] [-v VAR=VALUE]... 'PROGRAM' [FILE | VAR=VALUE]...
 *   awk [-F FS] [-v VAR=VALUE]... -f PROGFILE [-f PROGFILE]... [FILE | VAR=VALUE]...
 *
 * The program is parsed into a tree and interpreted. Values are numbers,
 * strings, or "strnums" (input that looks numeric); comparisons follow
 * POSIX. Regular expressions are extended (libc's regcomp), literal or
 * dynamic. Fields split on FS (" ": blanks; one character: itself;
 * otherwise a regex; "": each character); RS is a character or "" for
 * paragraphs. Everything POSIX has: patterns and ranges, BEGIN/END, user
 * functions with array references, getline in all six forms, output to
 * files and pipes, close, system, printf, sub/gsub/match/split/substr/index/
 * sprintf/length/tolower/toupper and the math functions; also fflush,
 * nextfile, delete ARRAY, length(ARRAY), ** and **=, func, ENVIRON.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <regex.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void fatal(const char *fmt, ...)
{
    fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    eprintf("awk: %s\n", buf);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- growable string ---- */

struct sb {
    char *s;
    size_t n, cap;
};

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_addc(struct sb *b, int c)
{
    char ch = c;
    sb_add(b, &ch, 1);
}

static void sb_adds(struct sb *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static char *sb_take(struct sb *b)
{
    if (!b->s)
        return xstrdup("");
    return b->s;
}

/* ================================================================ values */

enum { V_NUM = 1, V_STR = 2, V_STRNUM = 4 };

typedef struct {
    int flags;
    double num;
    char *str;
} cell;

static char convfmt[64] = "%.6g", ofmt[64] = "%.6g";

static cell mknum(double d)
{
    cell c = { V_NUM, d, 0 };
    return c;
}

static cell mkstr_own(char *s)
{
    cell c = { V_STR, 0, s };
    return c;
}

static cell mkstr(const char *s)
{
    return mkstr_own(xstrdup(s));
}

static int looks_numeric(const char *s, double *out)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    const char *start = p;
    if (*p == '+' || *p == '-')
        p++;
    if (!(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))))
        return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return 0;
    char *end;
    double d = strtod(start, &end);
    if (end == start)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    if (*end)
        return 0;
    if (out)
        *out = d;
    return 1;
}

/* input text: a strnum when it looks like a number */
static cell mkstrnum_own(char *s)
{
    cell c = { V_STR, 0, s };
    double d;
    if (looks_numeric(s, &d)) {
        c.flags = V_STR | V_STRNUM | V_NUM;
        c.num = d;
    }
    return c;
}

static cell copy_cell(cell c)
{
    if (c.str)
        c.str = xstrdup(c.str);
    return c;
}

static void free_cell(cell *c)
{
    free(c->str);
    c->str = 0;
    c->flags = 0;
}

static double str_to_num(const char *s)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    const char *q = p;
    if (*q == '+' || *q == '-')
        q++;
    if (!(isdigit((unsigned char)*q) || *q == '.'))
        return 0;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X'))
        return 0;
    return strtod(p, 0);
}

static double to_num(cell c)
{
    if (c.flags & V_NUM)
        return c.num;
    if (c.str)
        return str_to_num(c.str);
    return 0;
}

static char *num_to_str(double d, const char *fmt)
{
    char buf[512];
    if (fabs(d) < 1e16 && d == (double)(long long)d)
        snprintf(buf, sizeof(buf), "%lld", (long long)d);
    else if (fabs(d) < 1e30 && d == floor(d))
        snprintf(buf, sizeof(buf), "%.0f", d);  /* integral: as an integer, like gawk */
    else if (isnan(d))
        snprintf(buf, sizeof(buf), "%snan", signbit(d) ? "-" : "");
    else if (isinf(d))
        snprintf(buf, sizeof(buf), "%sinf", d < 0 ? "-" : "");
    else
        snprintf(buf, sizeof(buf), fmt, d);
    return xstrdup(buf);
}

/* the string value, as a new string */
static char *to_str_fmt(cell c, const char *fmt)
{
    if (c.flags & V_STR)
        return xstrdup(c.str ? c.str : "");
    if (c.flags & V_NUM)
        return num_to_str(c.num, fmt);
    return xstrdup("");
}

static char *to_str(cell c)
{
    return to_str_fmt(c, convfmt);
}

static int to_bool(cell c)
{
    if (c.flags & V_STRNUM)
        return c.num != 0;
    if (c.flags & V_STR)
        return c.str && *c.str;
    if (c.flags & V_NUM)
        return c.num != 0;
    return 0;
}

static int is_numeric_cmp(cell c)
{
    return (c.flags & V_NUM) || !c.flags;   /* numbers, strnums, uninitialized */
}

static int compare(cell a, cell b)
{
    if (is_numeric_cmp(a) && is_numeric_cmp(b)) {
        double x = to_num(a), y = to_num(b);
        return x < y ? -1 : x > y;
    }
    char *s = to_str(a), *t = to_str(b);
    int r = strcmp(s, t);
    free(s);
    free(t);
    return r < 0 ? -1 : r > 0;
}

/* ================================================================ arrays */

struct elem {
    char *key;
    cell val;
    struct elem *next;
};

struct array {
    struct elem **b;
    int nb, n;
};

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static struct array *new_array(void)
{
    struct array *a = xmalloc(sizeof(*a));
    a->nb = 64;
    a->n = 0;
    a->b = calloc(a->nb, sizeof(*a->b));
    return a;
}

static struct elem *arr_find(struct array *a, const char *key, int create)
{
    unsigned h = hash_str(key) % a->nb;
    for (struct elem *e = a->b[h]; e; e = e->next)
        if (!strcmp(e->key, key))
            return e;
    if (!create)
        return 0;
    if (a->n > a->nb * 2) {
        int nb = a->nb * 4;
        struct elem **nbk = calloc(nb, sizeof(*nbk));
        for (int i = 0; i < a->nb; i++)
            for (struct elem *e = a->b[i], *nx; e; e = nx) {
                nx = e->next;
                unsigned k = hash_str(e->key) % nb;
                e->next = nbk[k];
                nbk[k] = e;
            }
        free(a->b);
        a->b = nbk;
        a->nb = nb;
        h = hash_str(key) % a->nb;
    }
    struct elem *e = xmalloc(sizeof(*e));
    e->key = xstrdup(key);
    e->val.flags = 0;
    e->val.num = 0;
    e->val.str = 0;
    e->next = a->b[h];
    a->b[h] = e;
    a->n++;
    return e;
}

static void arr_delete(struct array *a, const char *key)
{
    unsigned h = hash_str(key) % a->nb;
    for (struct elem **pp = &a->b[h]; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->key, key)) {
            struct elem *e = *pp;
            *pp = e->next;
            free(e->key);
            free_cell(&e->val);
            free(e);
            a->n--;
            return;
        }
}

static void arr_clear(struct array *a)
{
    for (int i = 0; i < a->nb; i++) {
        for (struct elem *e = a->b[i], *nx; e; e = nx) {
            nx = e->next;
            free(e->key);
            free_cell(&e->val);
            free(e);
        }
        a->b[i] = 0;
    }
    a->n = 0;
}

/* ================================================================ variables */

enum { K_UNTYPED, K_SCALAR, K_ARRAY };
enum { SP_NONE, SP_NF, SP_FS, SP_RS, SP_SUBSEP, SP_CONVFMT, SP_OFMT, SP_OFS, SP_ORS };

struct var {
    char *name;
    int kind;
    cell val;
    struct array *arr;
    int special;
    struct var *ref;                    /* a parameter: the caller's variable, for array use */
    struct var *next;
};

#define VHASH 256
static struct var *globals[VHASH];

static struct var *global(const char *name)
{
    unsigned h = hash_str(name) % VHASH;
    for (struct var *v = globals[h]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    struct var *v = calloc(1, sizeof(*v));
    v->name = xstrdup(name);
    v->next = globals[h];
    globals[h] = v;
    return v;
}

static struct var *v_NF, *v_NR, *v_FNR, *v_FS, *v_OFS, *v_ORS, *v_RS, *v_SUBSEP, *v_FILENAME, *v_RSTART,
    *v_RLENGTH, *v_ARGC, *v_ARGV, *v_ENVIRON, *v_CONVFMT, *v_OFMT;

static void set_var_str(struct var *v, const char *s)
{
    free_cell(&v->val);
    v->val = mkstr(s);
    v->kind = K_SCALAR;
}

static void set_var_num(struct var *v, double d)
{
    free_cell(&v->val);
    v->val = mknum(d);
    v->kind = K_SCALAR;
}

static struct array *var_array(struct var *v)
{
    if (v->ref)
        return var_array(v->ref);
    if (v->kind == K_SCALAR)
        fatal("can't use scalar %s as an array", v->name);
    if (!v->arr) {
        v->arr = new_array();
        v->kind = K_ARRAY;
    }
    return v->arr;
}

/* ================================================================ regexes */

struct re_entry {
    char *src;
    regex_t re;
    struct re_entry *next;
};

static struct re_entry *re_cache[128];

static regex_t *get_regex(const char *src)
{
    unsigned h = hash_str(src) % 128;
    for (struct re_entry *e = re_cache[h]; e; e = e->next)
        if (!strcmp(e->src, src))
            return &e->re;
    struct re_entry *e = xmalloc(sizeof(*e));
    e->src = xstrdup(src);
    int r = regcomp(&e->re, src, REG_EXTENDED);
    if (r) {
        char msg[128];
        regerror(r, &e->re, msg, sizeof(msg));
        fatal("regular expression /%s/: %s", src, msg);
    }
    e->next = re_cache[h];
    re_cache[h] = e;
    return &e->re;
}

static int re_match(regex_t *re, const char *s, int notbol, regmatch_t *m)
{
    return regexec(re, s, m ? 1 : 0, m, notbol ? REG_NOTBOL : 0) == 0;
}

/* ================================================================ lexer */

enum {
    T_EOF, T_NL, T_NUMBER, T_STRING, T_ERE, T_NAME, T_FUNC_NAME, T_BUILTIN,
    T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT,
    T_NEXTFILE, T_EXIT, T_RETURN, T_DELETE, T_GETLINE, T_PRINT, T_PRINTF, T_IN,
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW, T_NOT, T_GT, T_LT, T_PIPE, T_QUESTION, T_COLON,
    T_MATCH, T_NOMATCH, T_DOLLAR, T_ASSIGN, T_ADD_ASSIGN, T_SUB_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN,
    T_MOD_ASSIGN, T_POW_ASSIGN, T_EQ, T_LE, T_GE, T_NE, T_INCR, T_DECR, T_AND, T_OR, T_APPEND,
};

static const struct { const char *name; int tok; } kwtab[] = {
    { "BEGIN", T_BEGIN }, { "END", T_END }, { "function", T_FUNCTION }, { "func", T_FUNCTION },
    { "if", T_IF }, { "else", T_ELSE }, { "while", T_WHILE }, { "for", T_FOR }, { "do", T_DO },
    { "break", T_BREAK }, { "continue", T_CONTINUE }, { "next", T_NEXT }, { "nextfile", T_NEXTFILE },
    { "exit", T_EXIT }, { "return", T_RETURN }, { "delete", T_DELETE }, { "getline", T_GETLINE },
    { "print", T_PRINT }, { "printf", T_PRINTF }, { "in", T_IN }, { 0, 0 } };

enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2,
       B_EXP, B_LOG, B_SQRT, B_INT, B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH };

static const char *const builtin_names[] = { "length", "substr", "index", "split", "sub", "gsub", "match",
                                             "sprintf", "sin", "cos", "atan2", "exp", "log", "sqrt", "int",
                                             "rand", "srand", "tolower", "toupper", "system", "close",
                                             "fflush", 0 };

static const char *prog_text;
static size_t prog_pos;
static int lineno = 1;

static int tok;                         /* the current token */
static char *tok_text;
static double tok_num;
static int tok_builtin;
static int prev_tok = T_NL;

static void syntax(const char *what)
{
    fatal("syntax error at source line %d: %s", lineno, what);
}

static int pc(void)
{
    return (unsigned char)prog_text[prog_pos];
}

static int gc(void)
{
    int c = (unsigned char)prog_text[prog_pos];
    if (c)
        prog_pos++;
    if (c == '\n')
        lineno++;
    return c;
}

static int regex_allowed(void)
{
    switch (prev_tok) {
    case T_NUMBER: case T_STRING: case T_ERE: case T_NAME: case T_BUILTIN: case T_RPAREN:
    case T_RBRACKET: case T_DOLLAR: case T_INCR: case T_DECR:
        return 0;
    }
    return 1;
}

static int escape_char(int c, const char **p)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    case '\\': return '\\';
    case '"': return '"';
    case '/': return '/';
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0';
            for (int k = 0; k < 2 && **p >= '0' && **p <= '7'; k++)
                v = v * 8 + (*(*p)++ - '0');
            return v;
        }
        return -1;
    }
}

static void next_token(void)
{
    free(tok_text);
    tok_text = 0;
    int c;
again:
    c = gc();
    while (c == ' ' || c == '\t' || c == '\r')
        c = gc();
    if (c == '\\' && pc() == '\n') {
        gc();
        goto again;
    }
    if (c == '\\' && pc() == '\r') {
        gc();
        if (pc() == '\n')
            gc();
        goto again;
    }
    if (c == '#') {
        while (pc() && pc() != '\n')
            gc();
        goto again;
    }
    if (!c) {
        tok = T_EOF;
        goto done;
    }
    if (c == '\n') {
        tok = T_NL;
        goto done;
    }
    if (isdigit(c) || (c == '.' && isdigit(pc()))) {
        const char *start = prog_text + prog_pos - 1;
        char *end;
        tok_num = strtod(start, &end);
        if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
            tok_num = 0;
            end = (char *)start + 1;
        }
        prog_pos = end - prog_text;
        tok = T_NUMBER;
        goto done;
    }
    if (isalpha(c) || c == '_') {
        struct sb b = { 0 };
        sb_addc(&b, c);
        while (isalnum(pc()) || pc() == '_')
            sb_addc(&b, gc());
        tok_text = b.s;
        for (int k = 0; kwtab[k].name; k++)
            if (!strcmp(tok_text, kwtab[k].name)) {
                tok = kwtab[k].tok;
                goto done;
            }
        for (int k = 0; builtin_names[k]; k++)
            if (!strcmp(tok_text, builtin_names[k])) {
                tok = T_BUILTIN;
                tok_builtin = k;
                goto done;
            }
        tok = pc() == '(' ? T_FUNC_NAME : T_NAME;
        goto done;
    }
    if (c == '"') {
        struct sb b = { 0 };
        sb_add(&b, "", 0);
        while ((c = gc()) != '"') {
            if (!c || c == '\n')
                syntax("non-terminated string");
            if (c == '\\') {
                c = gc();
                if (c == '\n')
                    continue;
                const char *p = prog_text + prog_pos;
                int e = escape_char(c, &p);
                prog_pos = p - prog_text;
                if (e < 0) {
                    sb_addc(&b, '\\');  /* kept for regexes: "\." */
                    sb_addc(&b, c);
                } else {
                    sb_addc(&b, e);
                }
                continue;
            }
            sb_addc(&b, c);
        }
        tok_text = b.s;
        tok = T_STRING;
        goto done;
    }
    if (c == '/' && regex_allowed()) {
        struct sb b = { 0 };
        sb_add(&b, "", 0);
        int in_br = 0;
        for (;;) {
            c = gc();
            if (!c || c == '\n')
                syntax("non-terminated regular expression");
            if (c == '/' && !in_br)
                break;
            if (c == '\\') {
                int d = gc();
                if (d == '/') {
                    sb_addc(&b, '/');
                } else if (d == 'n' || d == 't' || d == 'r' || d == 'f' || d == 'v' || d == 'a') {
                    const char *p = prog_text + prog_pos;
                    sb_addc(&b, escape_char(d, &p));
                } else {
                    sb_addc(&b, '\\');
                    sb_addc(&b, d);
                }
                continue;
            }
            if (c == '[' && !in_br) {
                in_br = 1;
                sb_addc(&b, c);
                if (pc() == '^')
                    sb_addc(&b, gc());
                if (pc() == ']')
                    sb_addc(&b, gc());
                continue;
            }
            if (c == ']' && in_br)
                in_br = 0;
            sb_addc(&b, c);
        }
        tok_text = b.s;
        tok = T_ERE;
        goto done;
    }
    int d = pc();
#define TWO(ch, t2, t1) if (d == (ch)) { gc(); tok = (t2); } else tok = (t1)
    switch (c) {
    case '{': tok = T_LBRACE; break;
    case '}': tok = T_RBRACE; break;
    case '(': tok = T_LPAREN; break;
    case ')': tok = T_RPAREN; break;
    case '[': tok = T_LBRACKET; break;
    case ']': tok = T_RBRACKET; break;
    case ';': tok = T_SEMI; break;
    case ',': tok = T_COMMA; break;
    case '?': tok = T_QUESTION; break;
    case ':': tok = T_COLON; break;
    case '~': tok = T_MATCH; break;
    case '$': tok = T_DOLLAR; break;
    case '+': if (d == '+') { gc(); tok = T_INCR; } else { TWO('=', T_ADD_ASSIGN, T_PLUS); } break;
    case '-': if (d == '-') { gc(); tok = T_DECR; } else { TWO('=', T_SUB_ASSIGN, T_MINUS); } break;
    case '*':
        if (d == '*') {
            gc();
            if (pc() == '=') { gc(); tok = T_POW_ASSIGN; } else tok = T_POW;
        } else {
            TWO('=', T_MUL_ASSIGN, T_STAR);
        }
        break;
    case '/': TWO('=', T_DIV_ASSIGN, T_SLASH); break;
    case '%': TWO('=', T_MOD_ASSIGN, T_PERCENT); break;
    case '^': TWO('=', T_POW_ASSIGN, T_POW); break;
    case '=': TWO('=', T_EQ, T_ASSIGN); break;
    case '<': TWO('=', T_LE, T_LT); break;
    case '>':
        if (d == '=') { gc(); tok = T_GE; } else if (d == '>') { gc(); tok = T_APPEND; } else tok = T_GT;
        break;
    case '!':
        if (d == '=') { gc(); tok = T_NE; } else if (d == '~') { gc(); tok = T_NOMATCH; } else tok = T_NOT;
        break;
    case '&':
        if (d != '&')
            syntax("'&' alone");
        gc();
        tok = T_AND;
        break;
    case '|': TWO('|', T_OR, T_PIPE); break;
    default: {
        char m[32];
        snprintf(m, sizeof(m), "unexpected character '%c'", c);
        syntax(m);
    }
    }
#undef TWO
done:
    prev_tok = tok;
}

static void opt_nls(void)
{
    while (tok == T_NL)
        next_token();
}

static void expect(int t, const char *what)
{
    if (tok != t) {
        char m[64];
        snprintf(m, sizeof(m), "%s expected", what);
        syntax(m);
    }
    next_token();
}

/* ================================================================ syntax tree */

enum {
    N_NUM, N_STR, N_ERE, N_VAR, N_INDEX, N_FIELD, N_ASSIGN, N_COND, N_OR, N_AND, N_IN, N_MATCH, N_REL,
    N_CONCAT, N_BINOP, N_NEG, N_PLUS, N_NOT, N_PREINC, N_POSTINC, N_CALL, N_BUILTIN, N_GETLINE, N_GROUP,
    /* statements */
    S_EXPR, S_PRINT, S_PRINTF, S_BLOCK, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BREAK, S_CONTINUE,
    S_NEXT, S_NEXTFILE, S_EXIT, S_RETURN, S_DELETE, S_NONE,
};

struct func;

struct node {
    int type;
    int op;                             /* operator token, builtin number, getline kind, redirection */
    struct node *a, *b, *c, *d;
    struct node *next;                  /* lists */
    double num;
    char *str;
    regex_t *re;
    struct var *var;                    /* a global */
    int local;                          /* a parameter's index, or -1 */
    struct func *fn;
    char *fname;
};

struct func {
    char *name;
    char **params;
    int nparams;
    struct node *body;
    int defined;
    struct func *next;
};

static struct func *funcs;
static struct func *cur_func;           /* being parsed: its parameters are locals */

static struct node *mk(int type)
{
    struct node *n = calloc(1, sizeof(*n));
    n->type = type;
    n->local = -1;
    return n;
}

static struct func *find_func(const char *name, int create)
{
    for (struct func *f = funcs; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    if (!create)
        return 0;
    struct func *f = calloc(1, sizeof(*f));
    f->name = xstrdup(name);
    f->next = funcs;
    funcs = f;
    return f;
}

static struct node *mk_var(const char *name)
{
    struct node *n = mk(N_VAR);
    if (cur_func)
        for (int i = 0; i < cur_func->nparams; i++)
            if (!strcmp(cur_func->params[i], name)) {
                n->local = i;
                return n;
            }
    if (find_func(name, 0) && find_func(name, 0)->defined)
        syntax("function name used as a variable");
    n->var = global(name);
    return n;
}

static int is_lvalue(struct node *n)
{
    return n->type == N_VAR || n->type == N_INDEX || n->type == N_FIELD;
}

static struct node *expr(int no_gt, int no_in);
static struct node *ternary(int no_gt, int no_in);
static struct node *unary(int no_gt, int no_in);
static struct node *primary(int no_gt, int no_in);

static struct node *expr_list(int no_gt, int *count)
{
    struct node *head = 0, **tail = &head;
    int n = 0;
    for (;;) {
        *tail = expr(no_gt, 0);
        tail = &(*tail)->next;
        n++;
        if (tok != T_COMMA)
            break;
        next_token();
        opt_nls();
    }
    if (count)
        *count = n;
    return head;
}

/* the subscript of a[i, j]: the expressions joined with SUBSEP at run time */
static struct node *subscript(void)
{
    return expr_list(0, 0);
}

static int starts_operand(int t)
{
    switch (t) {
    case T_NUMBER: case T_STRING: case T_ERE: case T_NAME: case T_FUNC_NAME: case T_BUILTIN:
    case T_DOLLAR: case T_NOT: case T_LPAREN: case T_MINUS: case T_PLUS: case T_INCR: case T_DECR:
        return 1;
    }
    return 0;
}

/* getline [lvalue] [< file]: the simple forms */
static struct node *simple_getline(void)
{
    struct node *n = mk(N_GETLINE);
    n->op = 0;                          /* 0: main input, '<': a file, '|': a command */
    if (tok == T_DOLLAR || tok == T_NAME) {
        n->a = primary(1, 0);           /* the variable */
        if (!is_lvalue(n->a))
            syntax("getline needs a variable");
    }
    if (tok == T_LT) {
        next_token();
        n->op = '<';
        n->b = primary(1, 0);
    }
    return n;
}

static struct node *primary(int no_gt, int no_in)
{
    struct node *n;
    switch (tok) {
    case T_NUMBER:
        n = mk(N_NUM);
        n->num = tok_num;
        next_token();
        return n;
    case T_STRING:
        n = mk(N_STR);
        n->str = xstrdup(tok_text);
        next_token();
        return n;
    case T_ERE:
        n = mk(N_ERE);
        n->str = xstrdup(tok_text);
        n->re = get_regex(tok_text);
        next_token();
        return n;
    case T_DOLLAR: {
        next_token();
        n = mk(N_FIELD);
        if (tok == T_INCR || tok == T_DECR || tok == T_MINUS || tok == T_PLUS || tok == T_NOT)
            n->a = unary(no_gt, no_in);
        else
            n->a = primary(no_gt, no_in);
        return n;
    }
    case T_INCR:
    case T_DECR: {
        int op = tok;
        next_token();
        n = mk(N_PREINC);
        n->op = op;
        n->a = primary(no_gt, no_in);
        if (!is_lvalue(n->a))
            syntax("++ or -- needs a variable");
        return n;
    }
    case T_LPAREN: {
        next_token();
        opt_nls();
        int count;
        struct node *l = expr_list(0, &count);
        opt_nls();
        expect(T_RPAREN, "')'");
        if (count > 1) {
            if (tok == T_IN) {
                next_token();
                if (tok != T_NAME)
                    syntax("array name expected after 'in'");
                n = mk(N_IN);
                n->a = l;
                n->b = mk_var(tok_text);
                next_token();
                return n;
            }
            n = mk(N_GROUP);            /* print (a, b) > "file" */
            n->a = l;
            return n;
        }
        n = mk(N_GROUP);
        n->a = l;
        n->op = 1;                      /* a parenthesized expression */
        return n;
    }
    case T_NAME: {
        char *name = xstrdup(tok_text);
        next_token();
        if (tok == T_LBRACKET) {
            next_token();
            n = mk(N_INDEX);
            n->a = mk_var(name);
            n->b = subscript();
            expect(T_RBRACKET, "']'");
        } else {
            n = mk_var(name);
        }
        free(name);
        return n;
    }
    case T_FUNC_NAME: {
        n = mk(N_CALL);
        n->fname = xstrdup(tok_text);
        n->fn = find_func(tok_text, 1);
        next_token();
        expect(T_LPAREN, "'('");
        opt_nls();
        if (tok != T_RPAREN)
            n->a = expr_list(0, 0);
        opt_nls();
        expect(T_RPAREN, "')'");
        return n;
    }
    case T_BUILTIN: {
        n = mk(N_BUILTIN);
        n->op = tok_builtin;
        next_token();
        if (tok == T_LPAREN) {
            next_token();
            opt_nls();
            if (tok != T_RPAREN)
                n->a = expr_list(0, 0);
            opt_nls();
            expect(T_RPAREN, "')'");
        } else if (n->op != B_LENGTH) {
            syntax("'(' expected after a builtin function");
        }
        return n;
    }
    case T_GETLINE:
        next_token();
        return simple_getline();
    }
    syntax("unexpected token");
    return 0;
}

static struct node *postfix(int no_gt, int no_in)
{
    struct node *n = primary(no_gt, no_in);
    if ((tok == T_INCR || tok == T_DECR) && is_lvalue(n)) {
        struct node *p = mk(N_POSTINC);
        p->op = tok;
        p->a = n;
        next_token();
        return p;
    }
    return n;
}

static struct node *power(int no_gt, int no_in)
{
    struct node *n = postfix(no_gt, no_in);
    if (tok == T_POW) {
        next_token();
        struct node *p = mk(N_BINOP);
        p->op = T_POW;
        p->a = n;
        /* right associative, and the exponent may have a sign: 2^-1 */
        if (tok == T_MINUS || tok == T_PLUS || tok == T_NOT)
            p->b = unary(no_gt, no_in);
        else
            p->b = power(no_gt, no_in);
        return p;
    }
    return n;
}

static struct node *unary(int no_gt, int no_in)
{
    if (tok == T_NOT || tok == T_MINUS || tok == T_PLUS) {
        int t = tok;
        next_token();
        struct node *n = mk(t == T_NOT ? N_NOT : t == T_MINUS ? N_NEG : N_PLUS);
        n->a = unary(no_gt, no_in);
        return n;
    }
    return power(no_gt, no_in);
}

static struct node *mul(int no_gt, int no_in)
{
    struct node *n = unary(no_gt, no_in);
    while (tok == T_STAR || tok == T_SLASH || tok == T_PERCENT) {
        struct node *p = mk(N_BINOP);
        p->op = tok;
        next_token();
        p->a = n;
        p->b = unary(no_gt, no_in);
        n = p;
    }
    return n;
}

static struct node *add(int no_gt, int no_in)
{
    struct node *n = mul(no_gt, no_in);
    while (tok == T_PLUS || tok == T_MINUS) {
        struct node *p = mk(N_BINOP);
        p->op = tok;
        next_token();
        p->a = n;
        p->b = mul(no_gt, no_in);
        n = p;
    }
    return n;
}

static struct node *concat(int no_gt, int no_in)
{
    struct node *n = add(no_gt, no_in);
    for (;;) {
        if (tok == T_PIPE) {
            /* cmd | getline [var] */
            size_t save_pos = prog_pos;
            int save_line = lineno;
            next_token();
            if (tok != T_GETLINE) {
                /* an output pipe: give the '|' back to the print statement */
                prog_pos = save_pos;
                lineno = save_line;
                tok = T_PIPE;
                prev_tok = T_PIPE;
                return n;
            }
            next_token();
            struct node *g = mk(N_GETLINE);
            g->op = '|';
            g->b = n;
            if (tok == T_DOLLAR || tok == T_NAME) {
                g->a = primary(1, 0);
                if (!is_lvalue(g->a))
                    syntax("getline needs a variable");
            }
            n = g;
            continue;
        }
        if (!starts_operand(tok) || tok == T_MINUS || tok == T_PLUS)
            return n;
        if (tok == T_NOT)
            return n;
        if (no_in && tok == T_IN)
            return n;
        struct node *p = mk(N_CONCAT);
        p->a = n;
        p->b = add(no_gt, no_in);
        n = p;
    }
}

static struct node *relational(int no_gt, int no_in)
{
    struct node *n = concat(no_gt, no_in);
    if (tok == T_LT || tok == T_LE || tok == T_NE || tok == T_EQ || tok == T_GE || (tok == T_GT && !no_gt)) {
        struct node *p = mk(N_REL);
        p->op = tok;
        next_token();
        p->a = n;
        p->b = concat(no_gt, no_in);
        return p;
    }
    return n;
}

static struct node *matchexpr(int no_gt, int no_in)
{
    struct node *n = relational(no_gt, no_in);
    while (tok == T_MATCH || tok == T_NOMATCH) {
        struct node *p = mk(N_MATCH);
        p->op = tok;
        next_token();
        p->a = n;
        p->b = relational(no_gt, no_in);
        n = p;
    }
    return n;
}

static struct node *inexpr(int no_gt, int no_in)
{
    struct node *n = matchexpr(no_gt, no_in);
    while (tok == T_IN && !no_in) {
        next_token();
        if (tok != T_NAME)
            syntax("array name expected after 'in'");
        struct node *p = mk(N_IN);
        p->a = n;
        p->b = mk_var(tok_text);
        next_token();
        n = p;
    }
    return n;
}

static struct node *andexpr(int no_gt, int no_in)
{
    struct node *n = inexpr(no_gt, no_in);
    while (tok == T_AND) {
        next_token();
        opt_nls();
        struct node *p = mk(N_AND);
        p->a = n;
        p->b = inexpr(no_gt, no_in);
        n = p;
    }
    return n;
}

static struct node *orexpr(int no_gt, int no_in)
{
    struct node *n = andexpr(no_gt, no_in);
    while (tok == T_OR) {
        next_token();
        opt_nls();
        struct node *p = mk(N_OR);
        p->a = n;
        p->b = andexpr(no_gt, no_in);
        n = p;
    }
    return n;
}

static struct node *ternary(int no_gt, int no_in)
{
    struct node *n = orexpr(no_gt, no_in);
    if (tok == T_QUESTION) {
        next_token();
        opt_nls();
        struct node *p = mk(N_COND);
        p->a = n;
        p->b = expr(no_gt, no_in);
        opt_nls();
        expect(T_COLON, "':'");
        opt_nls();
        p->c = expr(no_gt, no_in);
        return p;
    }
    return n;
}

static struct node *expr(int no_gt, int no_in)
{
    struct node *n = ternary(no_gt, no_in);
    if (tok == T_ASSIGN || tok == T_ADD_ASSIGN || tok == T_SUB_ASSIGN || tok == T_MUL_ASSIGN ||
        tok == T_DIV_ASSIGN || tok == T_MOD_ASSIGN || tok == T_POW_ASSIGN) {
        struct node *target = n;
        if (target->type == N_GROUP && target->op == 1 && !target->a->next)
            target = target->a;
        if (!is_lvalue(target))
            syntax("assignment to a non-variable");
        struct node *p = mk(N_ASSIGN);
        p->op = tok;
        next_token();
        opt_nls();
        p->a = target;
        p->b = expr(no_gt, no_in);
        return p;
    }
    return n;
}

/* ---- statements ---- */

static struct node *statement(void);

static void end_simple(void)
{
    if (tok == T_SEMI || tok == T_NL) {
        next_token();
        return;
    }
    if (tok == T_RBRACE || tok == T_EOF)
        return;
    syntax("';' or newline expected");
}

static struct node *block(void)
{
    expect(T_LBRACE, "'{'");
    struct node *b = mk(S_BLOCK), **tail = &b->a;
    for (;;) {
        while (tok == T_NL || tok == T_SEMI)
            next_token();
        if (tok == T_RBRACE)
            break;
        if (tok == T_EOF)
            syntax("'}' expected");
        *tail = statement();
        tail = &(*tail)->next;
    }
    next_token();
    return b;
}

static struct node *simple_statement(void)
{
    struct node *n;
    switch (tok) {
    case T_PRINT:
    case T_PRINTF: {
        n = mk(tok == T_PRINT ? S_PRINT : S_PRINTF);
        next_token();
        if (tok != T_SEMI && tok != T_NL && tok != T_RBRACE && tok != T_GT && tok != T_APPEND &&
            tok != T_PIPE && tok != T_EOF) {
            int count;
            n->a = expr_list(1, &count);
            if (count == 1 && n->a->type == N_GROUP && n->a->op != 1)
                n->a = n->a->a;         /* print (a, b) */
        }
        if (n->type == S_PRINTF && !n->a)
            syntax("printf needs a format");
        if (tok == T_GT || tok == T_APPEND || tok == T_PIPE) {
            n->op = tok == T_GT ? '>' : tok == T_APPEND ? 'a' : '|';
            next_token();
            n->b = concat(1, 0);
        }
        return n;
    }
    case T_DELETE:
        next_token();
        if (tok != T_NAME)
            syntax("array name expected after delete");
        n = mk(S_DELETE);
        n->a = mk_var(tok_text);
        next_token();
        if (tok == T_LBRACKET) {
            next_token();
            n->b = subscript();
            expect(T_RBRACKET, "']'");
        }
        return n;
    case T_NEXT:
        next_token();
        return mk(S_NEXT);
    case T_NEXTFILE:
        next_token();
        return mk(S_NEXTFILE);
    case T_EXIT:
        next_token();
        n = mk(S_EXIT);
        if (tok != T_SEMI && tok != T_NL && tok != T_RBRACE && tok != T_EOF)
            n->a = expr(0, 0);
        return n;
    case T_RETURN:
        if (!cur_func)
            syntax("return outside a function");
        next_token();
        n = mk(S_RETURN);
        if (tok != T_SEMI && tok != T_NL && tok != T_RBRACE && tok != T_EOF)
            n->a = expr(0, 0);
        return n;
    case T_BREAK:
        next_token();
        return mk(S_BREAK);
    case T_CONTINUE:
        next_token();
        return mk(S_CONTINUE);
    }
    n = mk(S_EXPR);
    n->a = expr(0, 0);
    return n;
}

static struct node *statement(void)
{
    struct node *n;
    switch (tok) {
    case T_LBRACE:
        return block();
    case T_IF:
        next_token();
        expect(T_LPAREN, "'('");
        n = mk(S_IF);
        n->a = expr(0, 0);
        expect(T_RPAREN, "')'");
        opt_nls();
        n->b = statement();
        /* else may follow newlines and a ';' */
        while (tok == T_NL || tok == T_SEMI)
            next_token();
        if (tok == T_ELSE) {
            next_token();
            opt_nls();
            n->c = statement();
        }
        return n;
    case T_WHILE:
        next_token();
        expect(T_LPAREN, "'('");
        n = mk(S_WHILE);
        n->a = expr(0, 0);
        expect(T_RPAREN, "')'");
        if (tok == T_SEMI) {            /* while (x); */
            next_token();
            n->b = mk(S_NONE);
            return n;
        }
        opt_nls();
        n->b = statement();
        return n;
    case T_DO:
        next_token();
        opt_nls();
        n = mk(S_DO);
        n->b = statement();
        while (tok == T_NL || tok == T_SEMI)
            next_token();
        expect(T_WHILE, "'while'");
        expect(T_LPAREN, "'('");
        n->a = expr(0, 0);
        expect(T_RPAREN, "')'");
        end_simple();
        return n;
    case T_FOR: {
        next_token();
        expect(T_LPAREN, "'('");
        /* for (name in array) */
        if (tok == T_NAME) {
            size_t save_pos = prog_pos;
            int save_line = lineno;
            char *name = xstrdup(tok_text);
            next_token();
            if (tok == T_IN) {
                next_token();
                if (tok != T_NAME)
                    syntax("array name expected");
                struct node *arr = mk_var(tok_text);
                next_token();
                if (tok == T_RPAREN) {
                    next_token();
                    opt_nls();
                    n = mk(S_FORIN);
                    n->a = mk_var(name);
                    n->b = arr;
                    n->c = statement();
                    free(name);
                    return n;
                }
                syntax("')' expected in for (name in array)");
            }
            /* not that form: start again after the '(' */
            prog_pos = save_pos;
            lineno = save_line;
            free(tok_text);
            tok_text = name;
            tok = pc() == '(' ? T_FUNC_NAME : T_NAME;
            prev_tok = tok;
        }
        n = mk(S_FOR);
        if (tok != T_SEMI)
            n->a = simple_statement();
        expect(T_SEMI, "';'");
        opt_nls();
        if (tok != T_SEMI)
            n->b = expr(0, 0);
        expect(T_SEMI, "';'");
        opt_nls();
        if (tok != T_RPAREN)
            n->c = simple_statement();
        expect(T_RPAREN, "')'");
        if (tok == T_SEMI) {
            next_token();
            n->d = mk(S_NONE);
            return n;
        }
        opt_nls();
        n->d = statement();
        return n;
    }
    case T_SEMI:
        next_token();
        return mk(S_NONE);
    }
    n = simple_statement();
    end_simple();
    return n;
}

/* ---- the program ---- */

struct rule {
    struct node *pattern, *pattern2;    /* pattern2: a range */
    struct node *action;                /* NULL: print */
    int in_range;
    struct rule *next;
};

static struct rule *begin_rules, *end_rules, *main_rules;
static struct rule **begin_tail = &begin_rules, **end_tail = &end_rules, **main_tail = &main_rules;

static void parse_program(void)
{
    next_token();
    for (;;) {
        while (tok == T_NL || tok == T_SEMI)
            next_token();
        if (tok == T_EOF)
            break;
        if (tok == T_FUNCTION) {
            next_token();
            if (tok != T_NAME && tok != T_FUNC_NAME)
                syntax("function name expected");
            struct func *f = find_func(tok_text, 1);
            if (f->defined)
                syntax("function defined twice");
            if (global(tok_text)->kind != K_UNTYPED)
                syntax("function name used as a variable");
            f->defined = 1;
            next_token();
            expect(T_LPAREN, "'('");
            while (tok != T_RPAREN) {
                if (tok != T_NAME)
                    syntax("parameter name expected");
                f->params = xrealloc(f->params, (f->nparams + 1) * sizeof(char *));
                f->params[f->nparams++] = xstrdup(tok_text);
                next_token();
                if (tok == T_COMMA) {
                    next_token();
                    opt_nls();
                } else if (tok != T_RPAREN) {
                    syntax("',' or ')' expected");
                }
            }
            next_token();
            opt_nls();
            cur_func = f;
            f->body = block();
            cur_func = 0;
            continue;
        }
        struct rule *r = calloc(1, sizeof(*r));
        if (tok == T_BEGIN || tok == T_END) {
            int b = tok == T_BEGIN;
            next_token();
            opt_nls();
            if (tok != T_LBRACE)
                syntax(b ? "BEGIN needs an action" : "END needs an action");
            r->action = block();
            if (b) {
                *begin_tail = r;
                begin_tail = &r->next;
            } else {
                *end_tail = r;
                end_tail = &r->next;
            }
            continue;
        }
        if (tok != T_LBRACE) {
            r->pattern = expr(0, 0);
            if (tok == T_COMMA) {
                next_token();
                opt_nls();
                r->pattern2 = expr(0, 0);
            }
        }
        if (tok == T_LBRACE)
            r->action = block();
        *main_tail = r;
        main_tail = &r->next;
    }
    for (struct func *f = funcs; f; f = f->next)
        if (!f->defined)
            fatal("function %s called but never defined", f->name);
}

/* ================================================================ I/O */

struct stream {
    char *name;
    int type;                           /* '>' 'a' '|' (output), '<' file, 'r' command (input) */
    FILE *fp;
    pid_t pid;
    struct stream *next;
};

static struct stream *streams;

static FILE *spawn(const char *cmd, int writing, pid_t *pid)
{
    int p[2];
    fflush(0);
    if (pipe(p) < 0)
        return 0;
    *pid = fork();
    if (*pid < 0) {
        close(p[0]);
        close(p[1]);
        return 0;
    }
    if (*pid == 0) {
        if (writing) {
            dup2(p[0], 0);
        } else {
            dup2(p[1], 1);
        }
        close(p[0]);
        close(p[1]);
        /* other streams' descriptors are not the child's business */
        for (struct stream *s = streams; s; s = s->next)
            if (s->fp)
                close(fileno(s->fp));
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    if (writing) {
        close(p[0]);
        return fdopen(p[1], "w");
    }
    close(p[1]);
    return fdopen(p[0], "r");
}

static int wait_status(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st);
}

static struct stream *get_stream(const char *name, int type)
{
    int out = type == '>' || type == 'a' || type == '|';
    for (struct stream *s = streams; s; s = s->next)
        if (!strcmp(s->name, name) && ((s->type == '>' || s->type == 'a' || s->type == '|') == out) &&
            (type == '|' || type == 'r' ? s->type == type : s->type != '|' && s->type != 'r'))
            return s;
    struct stream *s = calloc(1, sizeof(*s));
    s->name = xstrdup(name);
    s->type = type;
    if (type == '|' || type == 'r') {
        s->fp = spawn(name, type == '|', &s->pid);
    } else if (!strcmp(name, "/dev/stdout") || (!strcmp(name, "-") && out)) {
        s->fp = stdout;
    } else if (!strcmp(name, "/dev/stderr")) {
        s->fp = stderr;
    } else if (!strcmp(name, "/dev/stdin") || (!strcmp(name, "-") && !out)) {
        s->fp = stdin;
    } else {
        s->fp = fopen(name, type == '>' ? "w" : type == 'a' ? "a" : "r");
    }
    if (!s->fp) {
        free(s->name);
        free(s);
        return 0;
    }
    s->next = streams;
    streams = s;
    return s;
}

static int close_stream(const char *name)
{
    int r = -1;
    for (struct stream **pp = &streams; *pp;) {
        struct stream *s = *pp;
        if (strcmp(s->name, name)) {
            pp = &s->next;
            continue;
        }
        *pp = s->next;
        if (s->fp == stdout || s->fp == stderr) {
            fflush(s->fp);
            r = 0;
        } else if (s->fp == stdin) {
            r = 0;
        } else {
            r = fclose(s->fp) == 0 ? 0 : -1;
            if (s->pid)
                r = wait_status(s->pid);
        }
        free(s->name);
        free(s);
    }
    return r;
}

static void close_all(void)
{
    fflush(stdout);
    while (streams)
        close_stream(streams->name);
}

/* ---- records ---- */

/* Read one record from F as RS says; 1 if got one. */
static int read_record(FILE *f, struct sb *b)
{
    b->n = 0;
    sb_add(b, "", 0);
    const char *rs = v_RS->val.flags ? (v_RS->val.str ? v_RS->val.str : "") : "\n";
    char rsbuf[64];
    if (!(v_RS->val.flags & V_STR)) {
        char *t = to_str(v_RS->val);
        snprintf(rsbuf, sizeof(rsbuf), "%s", t);
        free(t);
        rs = rsbuf;
    }
    int c;
    if (*rs == 0) {
        /* paragraph mode: skip leading newlines, end at a blank line */
        while ((c = getc(f)) == '\n')
            ;
        if (c == EOF)
            return 0;
        for (;;) {
            if (c == EOF)
                break;
            if (c == '\n') {
                int d = getc(f);
                if (d == '\n') {
                    while ((d = getc(f)) == '\n')
                        ;
                    if (d != EOF)
                        ungetc(d, f);
                    break;
                }
                if (d == EOF)
                    break;
                sb_addc(b, '\n');
                c = d;
                continue;
            }
            sb_addc(b, c);
            c = getc(f);
        }
        return 1;
    }
    int sep = (unsigned char)rs[0];
    int got = 0;
    while ((c = getc(f)) != EOF) {
        got = 1;
        if (c == sep)
            return 1;
        sb_addc(b, c);
    }
    return got;
}

/* ---- fields ---- */

static char *record;                    /* $0 */
static cell *fields;                    /* fields[1..nf] */
static int nf, fields_cap;
static int fields_valid = 1, record_valid = 1;

static void set_nf_var(void)
{
    free_cell(&v_NF->val);
    v_NF->val = mknum(nf);
    v_NF->kind = K_SCALAR;
}

static void clear_fields(void)
{
    for (int i = 1; i <= nf; i++)
        free_cell(&fields[i]);
    nf = 0;
}

static void add_field(char *s)
{
    if (nf + 1 >= fields_cap) {
        fields_cap = fields_cap ? fields_cap * 2 : 64;
        fields = xrealloc(fields, fields_cap * sizeof(cell));
    }
    fields[++nf] = mkstrnum_own(s);
}

/* Split S by FS rules into a list of new strings; returns the count. */
static int split_string(const char *s, const char *fs, int paragraph, char ***out)
{
    char **v = 0;
    int n = 0, cap = 0;
#define PUSH(str) do { if (n == cap) v = xrealloc(v, (cap = cap ? cap * 2 : 16) * sizeof(char *)); v[n++] = (str); } while (0)
    if (!strcmp(fs, " ")) {
        const char *p = s;
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\n')
                p++;
            if (!*p)
                break;
            const char *e = p;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n')
                e++;
            PUSH(xstrndup(p, e - p));
            p = e;
        }
    } else if (!*fs) {
        for (const char *p = s; *p; p++)
            PUSH(xstrndup(p, 1));
    } else if (*s) {
        int single = strlen(fs) == 1 && fs[0] != ' ';
        if (fs[0] == '\\' && fs[1] && !fs[2])
            single = 2;
        if (single && !paragraph) {
            char sep = single == 2 ? fs[1] : fs[0];
            const char *p = s;
            for (;;) {
                const char *e = strchr(p, sep);
                if (!e) {
                    PUSH(xstrdup(p));
                    break;
                }
                PUSH(xstrndup(p, e - p));
                p = e + 1;
            }
        } else {
            /* a regex (in paragraph mode, newline separates too) */
            char pat[1024];
            if (paragraph)
                snprintf(pat, sizeof(pat), "(%s)|\n", single == 2 ? fs + 1 : fs);
            else
                snprintf(pat, sizeof(pat), "%s", fs);
            if (single && paragraph) {
                char lit[8];
                char ch = single == 2 ? fs[1] : fs[0];
                if (strchr("\\^$.[]|()*+?{}", ch))
                    snprintf(lit, sizeof(lit), "\\%c", ch);
                else
                    snprintf(lit, sizeof(lit), "%c", ch);
                snprintf(pat, sizeof(pat), "%s|\n", lit);
            }
            regex_t *re = get_regex(pat);
            const char *p = s;
            regmatch_t m;
            int notbol = 0;
            while (*p && re_match(re, p, notbol, &m) && m.rm_eo > 0) {
                if (m.rm_eo == m.rm_so) {
                    /* an empty match: no split here */
                    break;
                }
                PUSH(xstrndup(p, m.rm_so));
                p += m.rm_eo;
                notbol = 1;
            }
            PUSH(xstrdup(p));
        }
    }
#undef PUSH
    *out = v;
    return n;
}

static int paragraph_mode(void)
{
    return (v_RS->val.flags & V_STR) && v_RS->val.str && !*v_RS->val.str;
}

static void split_record(void)
{
    if (fields_valid)
        return;
    clear_fields();
    char *fs = to_str(v_FS->val);
    char **v;
    int n = split_string(record ? record : "", fs, paragraph_mode(), &v);
    for (int i = 0; i < n; i++)
        add_field(v[i]);
    free(v);
    free(fs);
    fields_valid = 1;
    set_nf_var();
}

static void rebuild_record(void)
{
    if (record_valid)
        return;
    struct sb b = { 0 };
    sb_add(&b, "", 0);
    char *ofs = to_str(v_OFS->val);
    for (int i = 1; i <= nf; i++) {
        if (i > 1)
            sb_adds(&b, ofs);
        char *s = to_str_fmt(fields[i], convfmt);
        sb_adds(&b, s);
        free(s);
    }
    free(ofs);
    free(record);
    record = sb_take(&b);
    record_valid = 1;
}

static void set_record(const char *s)
{
    free(record);
    record = xstrdup(s);
    record_valid = 1;
    fields_valid = 0;
    split_record();
}

static cell get_field(int i)
{
    if (i < 0)
        fatal("trying to access out of range field %d", i);
    if (i == 0) {
        rebuild_record();
        return mkstrnum_own(xstrdup(record ? record : ""));
    }
    split_record();
    if (i > nf)
        return mkstr("");
    return copy_cell(fields[i]);
}

static void set_nf(int n)
{
    split_record();
    if (n < 0)
        n = 0;
    while (nf < n)
        add_field(xstrdup(""));
    while (nf > n)
        free_cell(&fields[nf--]);
    set_nf_var();
    record_valid = 0;
}

static void set_field(int i, cell v)
{
    if (i < 0)
        fatal("trying to access out of range field %d", i);
    if (i == 0) {
        char *s = to_str(v);
        set_record(s);
        free(s);
        return;
    }
    split_record();
    if (i > 100000)
        fatal("field %d is too big", i);
    while (nf < i)
        add_field(xstrdup(""));
    free_cell(&fields[i]);
    fields[i] = copy_cell(v);
    set_nf_var();
    record_valid = 0;
}

/* ---- the main input ---- */

static FILE *main_in;
static int argv_index = 1;
static struct sb rec_buf;

static void assign_var(const char *assignment);

static int open_next_main(void)
{
    for (;;) {
        int argc = (int)to_num(v_ARGC->val);
        if (argv_index >= argc) {
            if (argv_index == argc && !v_FILENAME->val.flags) {
                /* no file operands: standard input */
                argv_index++;
                main_in = stdin;
                return 1;
            }
            return 0;
        }
        char key[32];
        snprintf(key, sizeof(key), "%d", argv_index++);
        struct elem *e = arr_find(var_array(v_ARGV), key, 0);
        char *arg = e ? to_str(e->val) : xstrdup("");
        if (!*arg) {
            free(arg);
            continue;
        }
        const char *eq = strchr(arg, '=');
        if (eq && eq > arg && (isalpha((unsigned char)arg[0]) || arg[0] == '_')) {
            int ok = 1;
            for (const char *p = arg; p < eq; p++)
                if (!isalnum((unsigned char)*p) && *p != '_')
                    ok = 0;
            if (ok) {
                assign_var(arg);
                free(arg);
                continue;
            }
        }
        set_var_str(v_FILENAME, arg);
        if (!strcmp(arg, "-") || !strcmp(arg, "/dev/stdin")) {
            main_in = stdin;
        } else if (!(main_in = fopen(arg, "r"))) {
            eprintf("awk: cannot open \"%s\" (%s)\n", arg, strerror(errno));
            free(arg);
            exit(2);
        }
        free(arg);
        set_var_num(v_FNR, 0);
        return 1;
    }
}

/* the next record of the main input into REC_BUF; 1 if got one */
static int next_main_record(void)
{
    for (;;) {
        if (!main_in && !open_next_main())
            return 0;
        if (read_record(main_in, &rec_buf)) {
            set_var_num(v_NR, to_num(v_NR->val) + 1);
            set_var_num(v_FNR, to_num(v_FNR->val) + 1);
            return 1;
        }
        if (main_in != stdin)
            fclose(main_in);
        main_in = 0;
        /* the FILENAME being set keeps an empty ARGV from meaning stdin */
        if (!v_FILENAME->val.flags)
            set_var_str(v_FILENAME, "");
    }
}

/* ================================================================ interpreter */

enum { F_NONE, F_BREAK, F_CONTINUE, F_RETURN };

static jmp_buf next_jmp, exit_jmp;
static int in_end, exit_code;

struct frame {
    struct var *locals;
    int n;
};

static struct frame frames[1024];
static int nframes;
static cell ret_val;

static struct var *resolve(struct node *n)
{
    if (n->local >= 0)
        return &frames[nframes - 1].locals[n->local];
    return n->var;
}

static cell eval(struct node *n);
static int exec(struct node *n);

static char *subscript_key(struct node *l)
{
    if (!l->next) {
        cell c = eval(l);
        char *s = to_str(c);
        free_cell(&c);
        return s;
    }
    struct sb b = { 0 };
    sb_add(&b, "", 0);
    char *sep = to_str(v_SUBSEP->val);
    for (struct node *e = l; e; e = e->next) {
        cell c = eval(e);
        char *s = to_str(c);
        if (e != l)
            sb_adds(&b, sep);
        sb_adds(&b, s);
        free(s);
        free_cell(&c);
    }
    free(sep);
    return sb_take(&b);
}

/* the value of a variable (a copy) */
static cell var_value(struct var *v)
{
    if (v->kind == K_ARRAY || (v->ref && v->ref->kind == K_ARRAY && !v->val.flags && v->kind != K_SCALAR))
        fatal("can't use array %s in a scalar context", v->name);
    if (v == v_NF)
        split_record();
    return copy_cell(v->val);
}

static void after_assign(struct var *v)
{
    if (v == v_NF)
        set_nf((int)to_num(v->val));
    else if (v == v_CONVFMT) {
        char *s = to_str(v->val);
        snprintf(convfmt, sizeof(convfmt), "%s", s);
        free(s);
    } else if (v == v_OFMT) {
        char *s = to_str(v->val);
        snprintf(ofmt, sizeof(ofmt), "%s", s);
        free(s);
    }
}

/* store a copy of V into the lvalue N */
static void assign_to(struct node *n, cell v)
{
    if (n->type == N_GROUP)
        n = n->a;
    if (n->type == N_VAR) {
        struct var *var = resolve(n);
        if (var->kind == K_ARRAY)
            fatal("can't assign to %s; it's an array name", var->name);
        free_cell(&var->val);
        var->val = copy_cell(v);
        var->kind = K_SCALAR;
        after_assign(var);
    } else if (n->type == N_INDEX) {
        char *key = subscript_key(n->b);
        struct elem *e = arr_find(var_array(resolve(n->a)), key, 1);
        free(key);
        free_cell(&e->val);
        e->val = copy_cell(v);
    } else if (n->type == N_FIELD) {
        cell i = eval(n->a);
        int idx = (int)to_num(i);
        free_cell(&i);
        set_field(idx, v);
    }
}

static cell lvalue_get(struct node *n)
{
    return eval(n);
}

/* format with printf rules; the arguments from the list L */
static char *format(const char *fmt, struct node *l)
{
    struct sb b = { 0 };
    sb_add(&b, "", 0);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            sb_addc(&b, *f);
            continue;
        }
        if (f[1] == '%') {
            sb_addc(&b, '%');
            f++;
            continue;
        }
        char spec[64];
        int k = 0;
        spec[k++] = '%';
        f++;
        while (*f && strchr("-+ #0", *f) && k < 20)
            spec[k++] = *f++;
        if (*f == '*') {
            cell c = l ? eval(l) : mknum(0);
            if (l)
                l = l->next;
            k += snprintf(spec + k, sizeof(spec) - k, "%d", (int)to_num(c));
            free_cell(&c);
            f++;
        } else {
            while (isdigit((unsigned char)*f) && k < 40)
                spec[k++] = *f++;
        }
        if (*f == '.') {
            spec[k++] = *f++;
            if (*f == '*') {
                cell c = l ? eval(l) : mknum(0);
                if (l)
                    l = l->next;
                k += snprintf(spec + k, sizeof(spec) - k, "%d", (int)to_num(c));
                free_cell(&c);
                f++;
            } else {
                while (isdigit((unsigned char)*f) && k < 50)
                    spec[k++] = *f++;
            }
        }
        while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'q')
            f++;
        char conv = *f;
        if (!conv) {
            spec[k] = 0;
            sb_adds(&b, spec);
            break;
        }
        cell arg = l ? eval(l) : mkstr("");
        if (l)
            l = l->next;
        char out[1024 + 512];
        char *big = 0;
        switch (conv) {
        case 'd': case 'i': {
            double d = to_num(arg);
            spec[k++] = 'l';
            spec[k++] = 'l';
            spec[k++] = 'd';
            spec[k] = 0;
            snprintf(out, sizeof(out), spec, (long long)d);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            double d = to_num(arg);
            spec[k++] = 'l';
            spec[k++] = 'l';
            spec[k++] = conv;
            spec[k] = 0;
            snprintf(out, sizeof(out), spec, (unsigned long long)(long long)d);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            spec[k++] = conv;
            spec[k] = 0;
            snprintf(out, sizeof(out), spec, to_num(arg));
            break;
        case 'c': {
            char ch[2] = { 0, 0 };
            if ((arg.flags & V_NUM) && !(arg.flags & V_STRNUM))
                ch[0] = (char)(int)arg.num;
            else {
                char *s = to_str(arg);
                ch[0] = s[0];
                free(s);
            }
            spec[k++] = 's';
            spec[k] = 0;
            if (!ch[0]) {
                /* a NUL character: width padding, then the byte */
                snprintf(out, sizeof(out), spec, "");
                sb_adds(&b, out);
                sb_add(&b, "", 1);
                free_cell(&arg);
                continue;
            }
            snprintf(out, sizeof(out), spec, ch);
            break;
        }
        case 's': {
            char *s = to_str(arg);
            spec[k++] = 's';
            spec[k] = 0;
            size_t need = strlen(s) + 64 + (size_t)atoi(spec + 1 + strspn(spec + 1, "-+ #0"));
            if (need > sizeof(out)) {
                big = xmalloc(need + 1024);
                snprintf(big, need + 1024, spec, s);
            } else {
                snprintf(out, sizeof(out), spec, s);
            }
            free(s);
            break;
        }
        default:
            spec[k++] = conv;
            spec[k] = 0;
            snprintf(out, sizeof(out), "%s", spec);
            break;
        }
        sb_adds(&b, big ? big : out);
        free(big);
        free_cell(&arg);
    }
    return sb_take(&b);
}

/* sub/gsub: replace in TARGET; returns the number of substitutions */
static int substitute(regex_t *re, const char *repl, struct node *target, int global)
{
    cell tv = target ? lvalue_get(target) : get_field(0);
    char *s = to_str(tv);
    free_cell(&tv);
    struct sb out = { 0 };
    sb_add(&out, "", 0);
    int count = 0;
    size_t pos = 0, len = strlen(s), prev_end = (size_t)-1;
    regmatch_t m;
    while (pos <= len) {
        if (!re_match(re, s + pos, pos > 0, &m))
            break;
        size_t so = pos + m.rm_so, eo = pos + m.rm_eo;
        if (so == eo && so == prev_end) {
            /* no empty match right after the previous one */
            if (so >= len)
                break;
            sb_addc(&out, s[so]);
            pos = so + 1;
            continue;
        }
        sb_add(&out, s + pos, so - pos);
        for (const char *r = repl; *r; r++) {
            if (*r == '\\' && (r[1] == '&' || r[1] == '\\')) {
                sb_addc(&out, r[1]);
                r++;
            } else if (*r == '&') {
                sb_add(&out, s + so, eo - so);
            } else {
                sb_addc(&out, *r);
            }
        }
        count++;
        prev_end = eo;
        if (eo == so) {
            if (so < len)
                sb_addc(&out, s[so]);
            pos = so + 1;
        } else {
            pos = eo;
        }
        if (!global)
            break;
    }
    if (count) {
        if (pos < len)
            sb_add(&out, s + pos, len - pos);
        cell nv = mkstr_own(sb_take(&out));
        if (target)
            assign_to(target, nv);
        else
            set_field(0, nv);
        free_cell(&nv);
    } else {
        free(out.s);
    }
    free(s);
    return count;
}

static regex_t *regex_of(struct node *n)
{
    if (n->type == N_ERE)
        return n->re;
    cell c = eval(n);
    char *s = to_str(c);
    free_cell(&c);
    regex_t *re = get_regex(s);
    free(s);
    return re;
}

static unsigned long rand_seed;
static double prev_seed;

static cell builtin(struct node *n)
{
    struct node *a = n->a, *b = a ? a->next : 0, *c = b ? b->next : 0;
    int nargs = 0;
    for (struct node *x = a; x; x = x->next)
        nargs++;
    switch (n->op) {
    case B_LENGTH: {
        if (!a) {
            cell v = get_field(0);
            double l = strlen(v.str);
            free_cell(&v);
            return mknum(l);
        }
        if (a->type == N_VAR) {
            struct var *v = resolve(a);
            struct var *t = v->ref ? v->ref : v;
            if (t->kind == K_ARRAY || v->kind == K_ARRAY)
                return mknum(var_array(v)->n);
        }
        cell v = eval(a);
        char *s = to_str(v);
        double l = strlen(s);
        free(s);
        free_cell(&v);
        return mknum(l);
    }
    case B_SUBSTR: {
        if (nargs < 2)
            fatal("substr needs 2 or 3 arguments");
        cell sv = eval(a), mv = eval(b);
        char *s = to_str(sv);
        double len = strlen(s);
        double m = to_num(mv);
        double start = round(m), end;
        if (c) {
            cell nv = eval(c);
            double nn = to_num(nv);
            free_cell(&nv);
            end = isnan(nn) ? start : start + round(nn);
        } else {
            end = len + 1;
        }
        if (start < 1)
            start = 1;
        if (end > len + 1)
            end = len + 1;
        cell r = end > start ? mkstr_own(xstrndup(s + (long)start - 1, (size_t)(end - start))) : mkstr("");
        free(s);
        free_cell(&sv);
        free_cell(&mv);
        return r;
    }
    case B_INDEX: {
        cell sv = eval(a), tv = eval(b);
        char *s = to_str(sv), *t = to_str(tv);
        char *p = strstr(s, t);
        double r = p ? p - s + 1 : 0;
        free(s);
        free(t);
        free_cell(&sv);
        free_cell(&tv);
        return mknum(r);
    }
    case B_SPLIT: {
        if (nargs < 2 || b->type != N_VAR)
            fatal("split needs a string and an array");
        cell sv = eval(a);
        char *s = to_str(sv);
        char *fs;
        if (c) {
            if (c->type == N_ERE) {
                fs = xstrdup(c->str);
                if (strlen(fs) == 1 && fs[0] == ' ') {
                    free(fs);
                    fs = xstrdup("[ ]");
                }
            } else {
                cell fv = eval(c);
                fs = to_str(fv);
                free_cell(&fv);
            }
        } else {
            fs = to_str(v_FS->val);
        }
        struct array *arr = var_array(resolve(b));
        arr_clear(arr);
        char **v;
        int cnt = split_string(s, fs, 0, &v);
        for (int i = 0; i < cnt; i++) {
            char key[32];
            snprintf(key, sizeof(key), "%d", i + 1);
            struct elem *e = arr_find(arr, key, 1);
            e->val = mkstrnum_own(v[i]);
        }
        free(v);
        free(fs);
        free(s);
        free_cell(&sv);
        return mknum(cnt);
    }
    case B_SUB:
    case B_GSUB: {
        if (nargs < 2)
            fatal("sub and gsub need 2 or 3 arguments");
        regex_t *re = regex_of(a);
        cell rv = eval(b);
        char *repl = to_str(rv);
        free_cell(&rv);
        if (c && !is_lvalue(c) && !(c->type == N_GROUP && is_lvalue(c->a)))
            fatal("sub and gsub need a variable as their third argument");
        int count = substitute(re, repl, c, n->op == B_GSUB);
        free(repl);
        return mknum(count);
    }
    case B_MATCH: {
        cell sv = eval(a);
        char *s = to_str(sv);
        regex_t *re = regex_of(b);
        regmatch_t m;
        double start = 0, length = -1;
        if (re_match(re, s, 0, &m)) {
            start = m.rm_so + 1;
            length = m.rm_eo - m.rm_so;
        }
        set_var_num(v_RSTART, start);
        set_var_num(v_RLENGTH, length);
        free(s);
        free_cell(&sv);
        return mknum(start);
    }
    case B_SPRINTF: {
        if (!a)
            fatal("sprintf needs a format");
        cell fv = eval(a);
        char *fmt = to_str(fv);
        free_cell(&fv);
        char *r = format(fmt, a->next);
        free(fmt);
        return mkstr_own(r);
    }
    case B_SIN: case B_COS: case B_EXP: case B_LOG: case B_SQRT: case B_INT: {
        cell v = a ? eval(a) : mknum(0);
        double x = to_num(v);
        free_cell(&v);
        switch (n->op) {
        case B_SIN: return mknum(sin(x));
        case B_COS: return mknum(cos(x));
        case B_EXP: return mknum(exp(x));
        case B_LOG: return mknum(log(x));
        case B_SQRT: return mknum(sqrt(x));
        default: return mknum(x < 0 ? ceil(x) : floor(x));
        }
    }
    case B_ATAN2: {
        cell y = eval(a), x = eval(b);
        double r = atan2(to_num(y), to_num(x));
        free_cell(&y);
        free_cell(&x);
        return mknum(r);
    }
    case B_RAND:
        rand_seed = rand_seed * 6364136223846793005UL + 1442695040888963407UL;
        return mknum((double)(rand_seed >> 11) / 9007199254740992.0);
    case B_SRAND: {
        double old = prev_seed;
        if (a) {
            cell v = eval(a);
            prev_seed = to_num(v);
            free_cell(&v);
        } else {
            prev_seed = time(0);
        }
        rand_seed = (unsigned long)(long long)prev_seed * 2654435761UL + 1;
        return mknum(old);
    }
    case B_TOLOWER:
    case B_TOUPPER: {
        cell v = eval(a);
        char *s = to_str(v);
        free_cell(&v);
        for (char *p = s; *p; p++)
            *p = n->op == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p);
        return mkstr_own(s);
    }
    case B_SYSTEM: {
        cell v = eval(a);
        char *cmd = to_str(v);
        free_cell(&v);
        fflush(0);
        pid_t pid = fork();
        int st = -1;
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)0);
            _exit(127);
        }
        if (pid > 0)
            st = wait_status(pid);
        free(cmd);
        return mknum(st);
    }
    case B_CLOSE: {
        cell v = eval(a);
        char *name = to_str(v);
        free_cell(&v);
        int r = close_stream(name);
        free(name);
        return mknum(r);
    }
    case B_FFLUSH: {
        if (!a) {
            fflush(0);
            return mknum(0);
        }
        cell v = eval(a);
        char *name = to_str(v);
        free_cell(&v);
        int r = -1;
        for (struct stream *s = streams; s; s = s->next)
            if (!strcmp(s->name, name)) {
                fflush(s->fp);
                r = 0;
            }
        if (!strcmp(name, "/dev/stdout")) {
            fflush(stdout);
            r = 0;
        }
        free(name);
        return mknum(r);
    }
    }
    return mknum(0);
}

static cell call(struct node *n)
{
    struct func *f = n->fn;
    int nargs = 0;
    for (struct node *x = n->a; x; x = x->next)
        nargs++;
    if (nargs > f->nparams)
        fatal("function %s called with %d arguments, accepts only %d", f->name, nargs, f->nparams);
    if (nframes >= 1024)
        fatal("function call nesting is too deep");
    struct var *locals = calloc(f->nparams + 1, sizeof(struct var));
    int i = 0;
    for (struct node *x = n->a; x; x = x->next, i++) {
        struct var *l = &locals[i];
        l->name = f->params[i];
        if (x->type == N_VAR) {
            struct var *src = resolve(x);
            struct var *t = src->ref && src->kind == K_UNTYPED ? src->ref : src;
            if (t->kind == K_ARRAY) {
                l->kind = K_ARRAY;
                l->arr = var_array(t);
                l->ref = t;             /* shared, by reference */
                continue;
            }
            if (t->kind == K_UNTYPED) {
                l->ref = t;             /* may become an array in the callee */
                continue;
            }
            l->val = copy_cell(t->val);
            l->kind = K_SCALAR;
            if (t == v_NF)
                split_record(), free_cell(&l->val), l->val = copy_cell(t->val);
            continue;
        }
        l->val = eval(x);
        l->kind = K_SCALAR;
    }
    for (; i < f->nparams; i++)
        locals[i].name = f->params[i];
    frames[nframes].locals = locals;
    frames[nframes].n = f->nparams;
    nframes++;
    ret_val = mkstr("");
    ret_val.flags = 0;
    exec(f->body);
    cell r = ret_val;
    ret_val.flags = 0;
    ret_val.str = 0;
    nframes--;
    for (i = 0; i < f->nparams; i++) {
        free_cell(&locals[i].val);
        if (locals[i].arr && !locals[i].ref) {
            arr_clear(locals[i].arr);
            free(locals[i].arr->b);
            free(locals[i].arr);
        }
    }
    free(locals);
    return r;
}

static cell do_getline(struct node *n)
{
    struct sb b = { 0 };
    int got;
    if (n->op == 0) {
        got = next_main_record();
        if (got) {
            sb_add(&b, rec_buf.s, rec_buf.n);
        }
    } else {
        cell src = eval(n->b);
        char *name = to_str(src);
        free_cell(&src);
        struct stream *s = get_stream(name, n->op == '<' ? '<' : 'r');
        free(name);
        if (!s)
            return mknum(-1);
        got = read_record(s->fp, &b);
        if (got && n->op == '|')
            set_var_num(v_NR, to_num(v_NR->val) + 1);
    }
    if (!got) {
        free(b.s);
        return mknum(0);
    }
    cell v = mkstrnum_own(sb_take(&b));
    if (n->a) {
        assign_to(n->a, v);
    } else {
        char *s = v.str;
        set_record(s);
    }
    free_cell(&v);
    return mknum(1);
}

static cell incdec(struct node *n, int pre)
{
    cell old = lvalue_get(n->a);
    double o = to_num(old);
    free_cell(&old);
    double nv = o + (n->op == T_INCR ? 1 : -1);
    cell c = mknum(nv);
    assign_to(n->a, c);
    return mknum(pre ? nv : o);
}

static double arith(int op, double x, double y)
{
    switch (op) {
    case T_PLUS: case T_ADD_ASSIGN: return x + y;
    case T_MINUS: case T_SUB_ASSIGN: return x - y;
    case T_STAR: case T_MUL_ASSIGN: return x * y;
    case T_SLASH: case T_DIV_ASSIGN:
        if (y == 0)
            fatal("division by zero");
        return x / y;
    case T_PERCENT: case T_MOD_ASSIGN:
        if (y == 0)
            fatal("division by zero in %%");
        return fmod(x, y);
    default:
        return pow(x, y);
    }
}

static cell eval(struct node *n)
{
    switch (n->type) {
    case N_NUM:
        return mknum(n->num);
    case N_STR:
        return mkstr(n->str);
    case N_ERE: {
        cell r = get_field(0);
        int m = re_match(n->re, r.str, 0, 0);
        free_cell(&r);
        return mknum(m);
    }
    case N_GROUP:
        return eval(n->a);
    case N_VAR:
        return var_value(resolve(n));
    case N_INDEX: {
        char *key = subscript_key(n->b);
        struct elem *e = arr_find(var_array(resolve(n->a)), key, 1);
        free(key);
        return copy_cell(e->val);
    }
    case N_FIELD: {
        cell i = eval(n->a);
        double d = to_num(i);
        free_cell(&i);
        return get_field((int)d);
    }
    case N_ASSIGN: {
        cell v = eval(n->b);
        if (n->op != T_ASSIGN) {
            cell old = lvalue_get(n->a);
            double r = arith(n->op, to_num(old), to_num(v));
            free_cell(&old);
            free_cell(&v);
            v = mknum(r);
        }
        assign_to(n->a, v);
        return v;
    }
    case N_COND: {
        cell c = eval(n->a);
        int t = to_bool(c);
        free_cell(&c);
        return eval(t ? n->b : n->c);
    }
    case N_OR:
    case N_AND: {
        cell x = eval(n->a);
        int t = to_bool(x);
        free_cell(&x);
        if (n->type == N_OR ? t : !t)
            return mknum(t);
        cell y = eval(n->b);
        int u = to_bool(y);
        free_cell(&y);
        return mknum(u);
    }
    case N_IN: {
        char *key = subscript_key(n->a);
        struct elem *e = arr_find(var_array(resolve(n->b)), key, 0);
        free(key);
        return mknum(e != 0);
    }
    case N_MATCH: {
        cell s = eval(n->a);
        char *str = to_str(s);
        free_cell(&s);
        regex_t *re = regex_of(n->b);
        int m = re_match(re, str, 0, 0);
        free(str);
        return mknum(n->op == T_MATCH ? m : !m);
    }
    case N_REL: {
        cell x = eval(n->a), y = eval(n->b);
        int c = compare(x, y), r;
        free_cell(&x);
        free_cell(&y);
        switch (n->op) {
        case T_LT: r = c < 0; break;
        case T_LE: r = c <= 0; break;
        case T_NE: r = c != 0; break;
        case T_EQ: r = c == 0; break;
        case T_GT: r = c > 0; break;
        default: r = c >= 0; break;
        }
        return mknum(r);
    }
    case N_CONCAT: {
        cell x = eval(n->a), y = eval(n->b);
        char *s = to_str(x), *t = to_str(y);
        size_t ls = strlen(s), lt = strlen(t);
        char *r = xmalloc(ls + lt + 1);
        memcpy(r, s, ls);
        memcpy(r + ls, t, lt + 1);
        free(s);
        free(t);
        free_cell(&x);
        free_cell(&y);
        return mkstr_own(r);
    }
    case N_BINOP: {
        cell x = eval(n->a), y = eval(n->b);
        double r = arith(n->op, to_num(x), to_num(y));
        free_cell(&x);
        free_cell(&y);
        return mknum(r);
    }
    case N_NEG:
    case N_PLUS: {
        cell x = eval(n->a);
        double d = to_num(x);
        free_cell(&x);
        return mknum(n->type == N_NEG ? -d : d);
    }
    case N_NOT: {
        cell x = eval(n->a);
        int t = to_bool(x);
        free_cell(&x);
        return mknum(!t);
    }
    case N_PREINC:
        return incdec(n, 1);
    case N_POSTINC:
        return incdec(n, 0);
    case N_CALL:
        return call(n);
    case N_BUILTIN:
        return builtin(n);
    case N_GETLINE:
        return do_getline(n);
    }
    fatal("internal error: node %d", n->type);
    return mknum(0);
}

static FILE *output_for(struct node *n)
{
    if (!n->op)
        return stdout;
    cell c = eval(n->b);
    char *name = to_str(c);
    free_cell(&c);
    struct stream *s = get_stream(name, n->op);
    if (!s)
        fatal("can't redirect to %s", name);
    free(name);
    return s->fp;
}

static int exec(struct node *n)
{
    for (; n; n = n->next) {
        int f = F_NONE;
        switch (n->type) {
        case S_NONE:
            break;
        case S_EXPR: {
            cell v = eval(n->a);
            free_cell(&v);
            break;
        }
        case S_PRINT: {
            struct sb b = { 0 };
            sb_add(&b, "", 0);
            if (!n->a) {
                cell r = get_field(0);
                sb_adds(&b, r.str);
                free_cell(&r);
            } else {
                char *ofs = to_str(v_OFS->val);
                for (struct node *e = n->a; e; e = e->next) {
                    cell v = eval(e);
                    char *s = to_str_fmt(v, (v.flags & V_STR) ? convfmt : ofmt);
                    if (e != n->a)
                        sb_adds(&b, ofs);
                    sb_adds(&b, s);
                    free(s);
                    free_cell(&v);
                }
                free(ofs);
            }
            char *ors = to_str(v_ORS->val);
            sb_adds(&b, ors);
            free(ors);
            FILE *out = output_for(n);
            fwrite(b.s, 1, b.n, out);
            if (out != stdout || isatty(1))
                fflush(out);
            free(b.s);
            break;
        }
        case S_PRINTF: {
            cell fv = eval(n->a);
            char *fmt = to_str(fv);
            free_cell(&fv);
            char *s = format(fmt, n->a->next);
            free(fmt);
            FILE *out = output_for(n);
            /* the formatted text may hold NULs (%c of 0): its length is in the buffer */
            fputs(s, out);
            if (out != stdout || isatty(1))
                fflush(out);
            free(s);
            break;
        }
        case S_BLOCK:
            f = exec(n->a);
            break;
        case S_IF: {
            cell c = eval(n->a);
            int t = to_bool(c);
            free_cell(&c);
            if (t)
                f = exec(n->b);
            else if (n->c)
                f = exec(n->c);
            break;
        }
        case S_WHILE:
            for (;;) {
                cell c = eval(n->a);
                int t = to_bool(c);
                free_cell(&c);
                if (!t)
                    break;
                int r = exec(n->b);
                if (r == F_BREAK)
                    break;
                if (r == F_RETURN)
                    return r;
            }
            break;
        case S_DO:
            for (;;) {
                int r = exec(n->b);
                if (r == F_BREAK)
                    break;
                if (r == F_RETURN)
                    return r;
                cell c = eval(n->a);
                int t = to_bool(c);
                free_cell(&c);
                if (!t)
                    break;
            }
            break;
        case S_FOR:
            if (n->a) {
                struct node *save = n->a->next;
                n->a->next = 0;
                exec(n->a);
                n->a->next = save;
            }
            for (;;) {
                if (n->b) {
                    cell c = eval(n->b);
                    int t = to_bool(c);
                    free_cell(&c);
                    if (!t)
                        break;
                }
                int r = exec(n->d);
                if (r == F_BREAK)
                    break;
                if (r == F_RETURN)
                    return r;
                if (n->c) {
                    struct node *save = n->c->next;
                    n->c->next = 0;
                    exec(n->c);
                    n->c->next = save;
                }
            }
            break;
        case S_FORIN: {
            struct array *a = var_array(resolve(n->b));
            /* a snapshot of the keys: the body may change the array */
            char **keys = xmalloc((a->n + 1) * sizeof(char *));
            int nk = 0;
            for (int i = 0; i < a->nb; i++)
                for (struct elem *e = a->b[i]; e; e = e->next)
                    keys[nk++] = xstrdup(e->key);
            int r = F_NONE;
            for (int i = 0; i < nk; i++) {
                if (!arr_find(a, keys[i], 0))
                    continue;
                cell k = mkstrnum_own(xstrdup(keys[i]));
                assign_to(n->a, k);
                free_cell(&k);
                r = exec(n->c);
                if (r == F_BREAK || r == F_RETURN)
                    break;
            }
            for (int i = 0; i < nk; i++)
                free(keys[i]);
            free(keys);
            if (r == F_RETURN)
                return r;
            break;
        }
        case S_BREAK:
            return F_BREAK;
        case S_CONTINUE:
            return F_CONTINUE;
        case S_NEXT:
            if (in_end)
                fatal("next used in END");
            longjmp(next_jmp, 1);
        case S_NEXTFILE:
            if (main_in && main_in != stdin)
                fclose(main_in);
            main_in = 0;
            longjmp(next_jmp, 1);
        case S_EXIT:
            if (n->a) {
                cell c = eval(n->a);
                exit_code = (int)to_num(c);
                free_cell(&c);
            }
            longjmp(exit_jmp, 1);
        case S_RETURN:
            free_cell(&ret_val);
            ret_val = n->a ? eval(n->a) : mkstr("");
            if (!n->a)
                ret_val.flags = 0;
            return F_RETURN;
        case S_DELETE: {
            struct array *a = var_array(resolve(n->a));
            if (n->b) {
                char *key = subscript_key(n->b);
                arr_delete(a, key);
                free(key);
            } else {
                arr_clear(a);
            }
            break;
        }
        default: {
            cell v = eval(n);
            free_cell(&v);
        }
        }
        if (f != F_NONE)
            return f;
    }
    return F_NONE;
}

/* ================================================================ main */

static char *unescape(const char *s)
{
    struct sb b = { 0 };
    sb_add(&b, "", 0);
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            const char *q = p + 1;
            int e = escape_char(*p, &q);
            if (e < 0) {
                sb_addc(&b, '\\');
                sb_addc(&b, *p);
            } else {
                sb_addc(&b, e);
                p = q - 1;
            }
        } else {
            sb_addc(&b, *p);
        }
    }
    return sb_take(&b);
}

static void assign_var(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    char *name = xstrndup(assignment, eq - assignment);
    struct var *v = global(name);
    if (v->kind == K_ARRAY)
        fatal("can't assign to %s; it's an array name", name);
    if (find_func(name, 0))
        fatal("can't assign to %s; it's a function", name);
    free_cell(&v->val);
    v->val = mkstrnum_own(unescape(eq + 1));
    v->kind = K_SCALAR;
    after_assign(v);
    free(name);
}

static int run_rules(struct rule *r)
{
    for (; r; r = r->next) {
        int hit;
        if (!r->pattern) {
            hit = 1;
        } else if (r->pattern2) {
            if (!r->in_range) {
                cell c = eval(r->pattern);
                hit = to_bool(c);
                free_cell(&c);
                if (hit) {
                    cell e = eval(r->pattern2);
                    r->in_range = !to_bool(e);
                    free_cell(&e);
                }
            } else {
                hit = 1;
                cell e = eval(r->pattern2);
                if (to_bool(e))
                    r->in_range = 0;
                free_cell(&e);
            }
        } else {
            cell c = eval(r->pattern);
            hit = to_bool(c);
            free_cell(&c);
        }
        if (!hit)
            continue;
        if (r->action) {
            exec(r->action);
        } else {
            cell rec = get_field(0);
            char *ors = to_str(v_ORS->val);
            fputs(rec.str, stdout);
            fputs(ors, stdout);
            free(ors);
            free_cell(&rec);
        }
    }
    return 0;
}

static void usage(void)
{
    eprintf("usage: awk [-F FS] [-v VAR=VALUE]... 'PROGRAM' [FILE | VAR=VALUE]...\n"
            "       awk [-F FS] [-v VAR=VALUE]... -f PROGFILE... [FILE | VAR=VALUE]...\n");
    exit(2);
}

int main(int argc, char **argv)
{
    v_NF = global("NF");
    v_NR = global("NR");
    v_FNR = global("FNR");
    v_FS = global("FS");
    v_OFS = global("OFS");
    v_ORS = global("ORS");
    v_RS = global("RS");
    v_SUBSEP = global("SUBSEP");
    v_FILENAME = global("FILENAME");
    v_RSTART = global("RSTART");
    v_RLENGTH = global("RLENGTH");
    v_ARGC = global("ARGC");
    v_ARGV = global("ARGV");
    v_ENVIRON = global("ENVIRON");
    v_CONVFMT = global("CONVFMT");
    v_OFMT = global("OFMT");
    set_var_num(v_NF, 0);
    set_var_num(v_NR, 0);
    set_var_num(v_FNR, 0);
    set_var_str(v_FS, " ");
    set_var_str(v_OFS, " ");
    set_var_str(v_ORS, "\n");
    set_var_str(v_RS, "\n");
    set_var_str(v_SUBSEP, "\034");
    set_var_str(v_CONVFMT, "%.6g");
    set_var_str(v_OFMT, "%.6g");
    set_var_num(v_RSTART, 0);
    set_var_num(v_RLENGTH, -1);
    v_FILENAME->kind = K_SCALAR;
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char *k = xstrndup(*e, eq - *e);
        struct elem *el = arr_find(var_array(v_ENVIRON), k, 1);
        el->val = mkstrnum_own(xstrdup(eq + 1));
        free(k);
    }
    rand_seed = 0;
    prev_seed = 0;
    struct sb progfiles = { 0 };
    const char *fs_opt = 0;
    const char **assigns = 0;
    int nassigns = 0, have_progfile = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--version")) {
            puts("awk (Eunuchs) 1.0");
            return 0;
        }
        if (a[1] == 'F') {
            fs_opt = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : 0;
            if (!fs_opt)
                usage();
        } else if (a[1] == 'v') {
            const char *v = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : 0;
            if (!v || !strchr(v, '='))
                usage();
            assigns = xrealloc(assigns, (nassigns + 1) * sizeof(char *));
            assigns[nassigns++] = v;
        } else if (a[1] == 'f') {
            const char *path = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : 0;
            if (!path)
                usage();
            FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "r");
            if (!f)
                fatal("can't open file %s", path);
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                sb_add(&progfiles, buf, n);
            sb_addc(&progfiles, '\n');
            if (f != stdin)
                fclose(f);
            have_progfile = 1;
        } else {
            usage();
        }
    }
    if (have_progfile) {
        prog_text = progfiles.s;
    } else {
        if (i >= argc)
            usage();
        prog_text = argv[i++];
    }
    if (fs_opt) {
        char *fs = !strcmp(fs_opt, "t") ? xstrdup("\t") : unescape(fs_opt);
        set_var_str(v_FS, fs);
        free(fs);
    }
    for (int k = 0; k < nassigns; k++)
        assign_var(assigns[k]);
    /* ARGV: the program's name, then the operands */
    struct array *av = var_array(v_ARGV);
    struct elem *e0 = arr_find(av, "0", 1);
    e0->val = mkstr("awk");
    int nargs = 1;
    for (; i < argc; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%d", nargs++);
        struct elem *e = arr_find(av, key, 1);
        e->val = mkstrnum_own(xstrdup(argv[i]));
    }
    set_var_num(v_ARGC, nargs);
    v_FILENAME->val.flags = 0;
    parse_program();
    record = xstrdup("");
    if (setjmp(exit_jmp)) {
        if (in_end)
            goto done;
        goto end_rules;
    }
    if (setjmp(next_jmp))
        fatal("next used in BEGIN or a function called from it");
    run_rules(begin_rules);
    if (main_rules || end_rules) {
        for (;;) {
            if (setjmp(next_jmp)) {
                nframes = 0;
                continue;
            }
            if (!next_main_record())
                break;
            set_record(rec_buf.s);
            run_rules(main_rules);
        }
    }
end_rules:
    in_end = 1;
    nframes = 0;
    if (setjmp(exit_jmp))
        goto done;
    if (setjmp(next_jmp))
        fatal("next used in END");
    run_rules(end_rules);
done:
    close_all();
    return exit_code;
}
