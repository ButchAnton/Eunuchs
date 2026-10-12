/*
 * regex: POSIX regcomp/regexec.
 *
 * The pattern is parsed into a small tree (alternation, concatenation,
 * repetition, groups, character sets, anchors, backreferences) and matched
 * by backtracking with explicit continuations, trying each start position
 * in turn: the first match found wins (leftmost, not leftmost-longest).
 */
#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

enum kind {
    N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WBEG, N_WEND,
    N_GROUP, N_ALT, N_CAT, N_REPEAT, N_BACKREF, N_EMPTY,
};

struct node {
    enum kind kind;
    int c;                      /* N_CHAR; group/backref number */
    unsigned char *set;         /* N_SET: 256-entry membership */
    struct node **kids;         /* N_ALT, N_CAT */
    int nkids;
    struct node *kid;           /* N_GROUP, N_REPEAT */
    int min, max;               /* N_REPEAT (max -1: unbounded) */
};

struct prog {
    struct node *root;
    int ngroups;
    int flags;
    /* allocation: everything comes from one list freed by regfree */
    void **allocs;
    int nallocs, capallocs;
};

/* ---- allocation ---- */

static void *ralloc(struct prog *p, size_t n)
{
    void *m = calloc(1, n);
    if (!m)
        return 0;
    if (p->nallocs == p->capallocs) {
        int cap = p->capallocs ? p->capallocs * 2 : 32;
        void **v = realloc(p->allocs, cap * sizeof(void *));
        if (!v) {
            free(m);
            return 0;
        }
        p->allocs = v;
        p->capallocs = cap;
    }
    p->allocs[p->nallocs++] = m;
    return m;
}

static struct node *mk(struct prog *p, enum kind k)
{
    struct node *n = ralloc(p, sizeof(*n));
    if (n)
        n->kind = k;
    return n;
}

static int add_kid(struct prog *p, struct node *parent, struct node *kid)
{
    struct node **v = ralloc(p, (parent->nkids + 1) * sizeof(*v));
    if (!v)
        return -1;
    if (parent->nkids)
        memcpy(v, parent->kids, parent->nkids * sizeof(*v));
    v[parent->nkids++] = kid;
    parent->kids = v;
    return 0;
}

/* ---- parsing ---- */

struct parser {
    struct prog *prog;
    const char *s;
    int ere;
    int icase;
    int err;
    int depth;                  /* open groups */
};

static struct node *parse_alt(struct parser *ps);

static int at_alt_end(struct parser *ps)
{
    const char *s = ps->s;
    if (!*s)
        return 1;
    if (ps->ere)
        return *s == '|' || (*s == ')' && ps->depth > 0);
    return s[0] == '\\' && (s[1] == '|' || (s[1] == ')' && ps->depth > 0));
}

/* [:name:] inside a bracket expression; 0 if the name is not a class. */
static int set_class(unsigned char *set, const char *name, int len)
{
    static const struct {
        const char *name;
        int (*test)(int);
    } classes[] = {
        { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "upper", isupper },
        { "lower", islower }, { "space", isspace }, { "blank", isblank }, { "punct", ispunct },
        { "print", isprint }, { "graph", isgraph }, { "cntrl", iscntrl }, { "xdigit", isxdigit },
    };
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
        if ((int)strlen(classes[i].name) != len || strncmp(classes[i].name, name, len))
            continue;
        for (int c = 0; c < 256; c++)
            if (classes[i].test(c))
                set[c] = 1;
        return 1;
    }
    return 0;
}

static struct node *parse_bracket(struct parser *ps)
{
    struct node *n = mk(ps->prog, N_SET);
    unsigned char *set = n ? ralloc(ps->prog, 256) : 0;
    if (!set) {
        ps->err = REG_ESPACE;
        return 0;
    }
    n->set = set;
    const char *s = ps->s;     /* after '[' */
    int neg = 0;
    if (*s == '^') {
        neg = 1;
        s++;
    }
    int first = 1;
    while (*s && (*s != ']' || first)) {
        first = 0;
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (!e) {
                ps->err = REG_ECTYPE;
                return 0;
            }
            if (!set_class(set, s + 2, (int)(e - s - 2))) {
                ps->err = REG_ECTYPE;
                return 0;
            }
            s = e + 2;
            continue;
        }
        int lo = (unsigned char)*s++;
        int hi = lo;
        if (s[0] == '-' && s[1] && s[1] != ']') {
            hi = (unsigned char)s[1];
            s += 2;
            if (hi < lo) {
                ps->err = REG_ERANGE;
                return 0;
            }
        }
        for (int c = lo; c <= hi; c++)
            set[c] = 1;
    }
    if (*s != ']') {
        ps->err = REG_EBRACK;
        return 0;
    }
    ps->s = s + 1;
    if (ps->icase)
        for (int c = 0; c < 256; c++)
            if (set[c])
                set[tolower(c)] = set[toupper(c)] = 1;
    if (neg) {
        for (int c = 0; c < 256; c++)
            set[c] = !set[c];
        if (ps->prog->flags & REG_NEWLINE)
            set['\n'] = 0;
    }
    return n;
}

static struct node *class_escape(struct parser *ps, char e)
{
    struct node *n = mk(ps->prog, N_SET);
    unsigned char *set = n ? ralloc(ps->prog, 256) : 0;
    if (!set) {
        ps->err = REG_ESPACE;
        return 0;
    }
    n->set = set;
    for (int c = 0; c < 256; c++) {
        int in = e == 'w' || e == 'W' ? isalnum(c) || c == '_' : e == 's' || e == 'S' ? isspace(c) : isdigit(c);
        set[c] = (e == 'W' || e == 'S' || e == 'D') ? !in : in;
    }
    return n;
}

static struct node *parse_atom(struct parser *ps, int at_start)
{
    const char *s = ps->s;
    struct node *n = 0;
    if (*s == '\\') {
        char e = s[1];
        if (!e) {
            ps->err = REG_EESCAPE;
            return 0;
        }
        ps->s = s + 2;
        if (!ps->ere && e == '(') {
            ps->depth++;
            struct node *g = mk(ps->prog, N_GROUP);
            if (!g) {
                ps->err = REG_ESPACE;
                return 0;
            }
            g->c = ++ps->prog->ngroups;
            g->kid = parse_alt(ps);
            if (ps->err)
                return 0;
            if (ps->s[0] != '\\' || ps->s[1] != ')') {
                ps->err = REG_EPAREN;
                return 0;
            }
            ps->s += 2;
            ps->depth--;
            return g;
        }
        if (e >= '1' && e <= '9') {
            n = mk(ps->prog, N_BACKREF);
            if (n)
                n->c = e - '0';
            if (n && n->c > ps->prog->ngroups) {
                ps->err = REG_ESUBREG;
                return 0;
            }
        } else if (e == 'w' || e == 'W' || e == 's' || e == 'S' || e == 'd' || e == 'D') {
            return class_escape(ps, e);
        } else if (e == '<') {
            n = mk(ps->prog, N_WBEG);
        } else if (e == '>') {
            n = mk(ps->prog, N_WEND);
        } else if (e == 'b') {
            n = mk(ps->prog, N_WORDB);
        } else if (e == 'B') {
            n = mk(ps->prog, N_NWORDB);
        } else if (e == 'n') {
            n = mk(ps->prog, N_CHAR);
            if (n) n->c = '\n';
        } else if (e == 't') {
            n = mk(ps->prog, N_CHAR);
            if (n) n->c = '\t';
        } else {
            n = mk(ps->prog, N_CHAR);
            if (n) n->c = (unsigned char)e;
        }
        if (!n)
            ps->err = REG_ESPACE;
        return n;
    }
    ps->s = s + 1;
    switch (*s) {
    case '.':
        n = mk(ps->prog, N_ANY);
        break;
    case '[':
        return parse_bracket(ps);
    case '^':
        if (ps->ere || at_start) {
            n = mk(ps->prog, N_BOL);
            break;
        }
        n = mk(ps->prog, N_CHAR);
        if (n) n->c = '^';
        break;
    case '$':
        /* BRE: an anchor only at the end of the pattern or before \) \| */
        if (ps->ere || !ps->s[0] || (ps->s[0] == '\\' && (ps->s[1] == ')' || ps->s[1] == '|'))) {
            n = mk(ps->prog, N_EOL);
            break;
        }
        n = mk(ps->prog, N_CHAR);
        if (n) n->c = '$';
        break;
    case '(':
        if (ps->ere) {
            ps->depth++;
            struct node *g = mk(ps->prog, N_GROUP);
            if (!g) {
                ps->err = REG_ESPACE;
                return 0;
            }
            g->c = ++ps->prog->ngroups;
            g->kid = parse_alt(ps);
            if (ps->err)
                return 0;
            if (*ps->s != ')') {
                ps->err = REG_EPAREN;
                return 0;
            }
            ps->s++;
            ps->depth--;
            return g;
        }
        __attribute__((fallthrough));     /* a literal ( in BRE */
    default:
        if (ps->ere && (*s == '*' || *s == '+' || *s == '?' || *s == '{') && at_start) {
            ps->err = REG_BADRPT;
            return 0;
        }
        n = mk(ps->prog, N_CHAR);
        if (n) n->c = (unsigned char)*s;
    }
    if (!n)
        ps->err = REG_ESPACE;
    return n;
}

static int parse_interval(struct parser *ps, int *min, int *max)
{
    const char *s = ps->s;
    if (!isdigit((unsigned char)*s)) {
        ps->err = REG_BADBR;
        return -1;
    }
    *min = 0;
    while (isdigit((unsigned char)*s))
        *min = *min * 10 + *s++ - '0';
    *max = *min;
    if (*s == ',') {
        s++;
        if (isdigit((unsigned char)*s)) {
            *max = 0;
            while (isdigit((unsigned char)*s))
                *max = *max * 10 + *s++ - '0';
        } else {
            *max = -1;
        }
    }
    if (ps->ere ? *s != '}' : (s[0] != '\\' || s[1] != '}')) {
        ps->err = REG_EBRACE;
        return -1;
    }
    ps->s = s + (ps->ere ? 1 : 2);
    if (*max >= 0 && *max < *min) {
        ps->err = REG_BADBR;
        return -1;
    }
    return 0;
}

static struct node *parse_cat(struct parser *ps)
{
    struct node *cat = mk(ps->prog, N_CAT);
    if (!cat) {
        ps->err = REG_ESPACE;
        return 0;
    }
    int start = 1;
    while (!at_alt_end(ps) && !ps->err) {
        /* BRE: a leading * is literal */
        struct node *a;
        if (!ps->ere && start && ps->s[0] == '*') {
            ps->s++;
            a = mk(ps->prog, N_CHAR);
            if (a) a->c = '*';
        } else {
            a = parse_atom(ps, start);
        }
        if (!a)
            return 0;
        start = 0;
        for (;;) {
            int min, max;
            const char *s = ps->s;
            if (*s == '*') {
                ps->s++;
                min = 0, max = -1;
            } else if (ps->ere && *s == '+') {
                ps->s++;
                min = 1, max = -1;
            } else if (ps->ere && *s == '?') {
                ps->s++;
                min = 0, max = 1;
            } else if (!ps->ere && s[0] == '\\' && s[1] == '+') {
                ps->s += 2;
                min = 1, max = -1;
            } else if (!ps->ere && s[0] == '\\' && s[1] == '?') {
                ps->s += 2;
                min = 0, max = 1;
            } else if (ps->ere && *s == '{' && isdigit((unsigned char)s[1])) {
                ps->s++;
                if (parse_interval(ps, &min, &max))
                    return 0;
            } else if (!ps->ere && s[0] == '\\' && s[1] == '{') {
                ps->s += 2;
                if (parse_interval(ps, &min, &max))
                    return 0;
            } else {
                break;
            }
            struct node *r = mk(ps->prog, N_REPEAT);
            if (!r) {
                ps->err = REG_ESPACE;
                return 0;
            }
            r->kid = a;
            r->min = min;
            r->max = max;
            a = r;
        }
        if (add_kid(ps->prog, cat, a)) {
            ps->err = REG_ESPACE;
            return 0;
        }
    }
    return cat;
}

static struct node *parse_alt(struct parser *ps)
{
    struct node *alt = mk(ps->prog, N_ALT);
    if (!alt) {
        ps->err = REG_ESPACE;
        return 0;
    }
    for (;;) {
        struct node *c = parse_cat(ps);
        if (!c || add_kid(ps->prog, alt, c)) {
            if (!ps->err)
                ps->err = REG_ESPACE;
            return 0;
        }
        if (ps->ere && *ps->s == '|') {
            ps->s++;
            continue;
        }
        if (!ps->ere && ps->s[0] == '\\' && ps->s[1] == '|') {
            ps->s += 2;
            continue;
        }
        break;
    }
    return alt->nkids == 1 ? alt->kids[0] : alt;
}

int regcomp(regex_t *re, const char *pattern, int cflags)
{
    struct prog *p = calloc(1, sizeof(*p));
    if (!p)
        return REG_ESPACE;
    p->flags = cflags;
    struct parser ps = { p, pattern, cflags & REG_EXTENDED, cflags & REG_ICASE, 0, 0 };
    p->root = parse_alt(&ps);
    if (!ps.err && *ps.s)
        ps.err = REG_EPAREN;    /* an unmatched ) */
    re->re_prog = p;
    if (ps.err) {
        regfree(re);
        return ps.err;
    }
    re->re_nsub = p->ngroups;
    re->re_flags = cflags;
    return 0;
}

void regfree(regex_t *re)
{
    struct prog *p = re->re_prog;
    if (!p)
        return;
    for (int i = 0; i < p->nallocs; i++)
        free(p->allocs[i]);
    free(p->allocs);
    free(p);
    re->re_prog = 0;
}

/* ---- matching ---- */

enum ck { K_SEQ, K_REP, K_GROUP };

struct cont {
    enum ck kind;
    const struct node *node;    /* K_SEQ: the N_CAT; K_REP: the N_REPEAT */
    int index;                  /* K_SEQ: next child; K_REP: iterations done */
    int start;                  /* K_REP: where this iteration began; K_GROUP: group start */
    const struct cont *next;
};

struct mstate {
    const char *s;              /* the whole subject */
    int len;
    int base;                   /* start offset (REG_STARTEND) */
    int flags, eflags;
    regmatch_t g[10];
    int steps;
};

static int is_word(int c)
{
    return isalnum(c) || c == '_';
}

static int fold(int c, int icase)
{
    return icase ? tolower(c) : c;
}

static int mnode(struct mstate *m, const struct node *n, int pos, const struct cont *k);

static int mcont(struct mstate *m, int pos, const struct cont *k)
{
    if (!k)
        return pos;
    if (++m->steps > 2000000)   /* runaway backtracking */
        return -1;
    switch (k->kind) {
    case K_SEQ:
        if (k->index >= k->node->nkids)
            return mcont(m, pos, k->next);
        {
            struct cont c = { K_SEQ, k->node, k->index + 1, 0, k->next };
            return mnode(m, k->node->kids[k->index], pos, &c);
        }
    case K_REP: {
        const struct node *r = k->node;
        int count = k->index;
        /* a pass that matched nothing can't make progress */
        if (pos == k->start && count > r->min)
            return -1;
        /* greedy: one more if allowed, else (or then) what follows */
        if (r->max < 0 || count < r->max) {
            struct cont c = { K_REP, r, count + 1, pos, k->next };
            int e = mnode(m, r->kid, pos, &c);
            if (e >= 0)
                return e;
        }
        if (count >= r->min)
            return mcont(m, pos, k->next);
        return -1;
    }
    case K_GROUP: {
        int gi = k->node->c;
        regmatch_t saved = gi < 10 ? m->g[gi] : (regmatch_t){ 0, 0 };
        if (gi < 10) {
            m->g[gi].rm_so = k->start;
            m->g[gi].rm_eo = pos;
        }
        int e = mcont(m, pos, k->next);
        if (e < 0 && gi < 10)
            m->g[gi] = saved;
        return e;
    }
    }
    return -1;
}

static int mnode(struct mstate *m, const struct node *n, int pos, const struct cont *k)
{
    int icase = m->flags & REG_ICASE;
    int nl = m->flags & REG_NEWLINE;
    const char *s = m->s;
    switch (n->kind) {
    case N_EMPTY:
        return mcont(m, pos, k);
    case N_CHAR:
        if (pos < m->len && fold((unsigned char)s[pos], icase) == fold(n->c, icase))
            return mcont(m, pos + 1, k);
        return -1;
    case N_ANY:
        if (pos < m->len && !(nl && s[pos] == '\n'))
            return mcont(m, pos + 1, k);
        return -1;
    case N_SET:
        if (pos < m->len && n->set[(unsigned char)s[pos]])
            return mcont(m, pos + 1, k);
        return -1;
    case N_BOL:
        if ((pos == m->base && !(m->eflags & REG_NOTBOL)) || (nl && pos > 0 && s[pos - 1] == '\n'))
            return mcont(m, pos, k);
        return -1;
    case N_EOL:
        if ((pos == m->len && !(m->eflags & REG_NOTEOL)) || (nl && pos < m->len && s[pos] == '\n'))
            return mcont(m, pos, k);
        return -1;
    case N_WORDB:
    case N_NWORDB:
    case N_WBEG:
    case N_WEND: {
        int before = pos > 0 && is_word((unsigned char)s[pos - 1]);
        int after = pos < m->len && is_word((unsigned char)s[pos]);
        int ok = n->kind == N_WORDB ? before != after
               : n->kind == N_NWORDB ? before == after
               : n->kind == N_WBEG ? !before && after : before && !after;
        return ok ? mcont(m, pos, k) : -1;
    }
    case N_BACKREF: {
        regmatch_t g = m->g[n->c];
        if (g.rm_so < 0)
            return -1;
        int l = (int)(g.rm_eo - g.rm_so);
        if (pos + l > m->len)
            return -1;
        for (int i = 0; i < l; i++)
            if (fold((unsigned char)s[g.rm_so + i], icase) != fold((unsigned char)s[pos + i], icase))
                return -1;
        return mcont(m, pos + l, k);
    }
    case N_GROUP: {
        struct cont c = { K_GROUP, n, 0, pos, k };
        return mnode(m, n->kid, pos, &c);
    }
    case N_CAT: {
        struct cont c = { K_SEQ, n, 0, 0, k };
        return mcont(m, pos, &c);
    }
    case N_ALT:
        for (int i = 0; i < n->nkids; i++) {
            int e = mnode(m, n->kids[i], pos, k);
            if (e >= 0)
                return e;
        }
        return -1;
    case N_REPEAT: {
        /* count 0: start the first iteration (or skip it) */
        if (n->max != 0) {
            struct cont first = { K_REP, n, 1, pos, k };
            int e = mnode(m, n->kid, pos, &first);
            if (e >= 0)
                return e;
        }
        return n->min == 0 ? mcont(m, pos, k) : -1;
    }
    }
    return -1;
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t pmatch[], int eflags)
{
    struct prog *p = re->re_prog;
    if (!p)
        return REG_BADPAT;
    struct mstate m;
    memset(&m, 0, sizeof(m));
    m.s = s;
    m.flags = p->flags;
    m.eflags = eflags;
    int start = 0;
    if ((eflags & REG_STARTEND) && pmatch) {
        start = (int)pmatch[0].rm_so;
        m.len = (int)pmatch[0].rm_eo;
    } else {
        m.len = (int)strlen(s);
    }
    m.base = start;
    for (int pos = start; pos <= m.len; pos++) {
        for (int i = 0; i < 10; i++)
            m.g[i].rm_so = m.g[i].rm_eo = -1;
        m.steps = 0;
        int e = mnode(&m, p->root, pos, 0);
        if (e >= 0) {
            if (!(p->flags & REG_NOSUB) && pmatch) {
                for (size_t i = 0; i < nmatch; i++) {
                    if (i == 0) {
                        pmatch[0].rm_so = pos;
                        pmatch[0].rm_eo = e;
                    } else if (i < 10) {
                        pmatch[i] = m.g[i];
                    } else {
                        pmatch[i].rm_so = pmatch[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }
        /* anchored at the start: no point trying later positions */
        const struct node *first = p->root->kind == N_CAT && p->root->nkids ? p->root->kids[0] : p->root;
        if (first->kind == N_BOL && !(p->flags & REG_NEWLINE))
            break;
    }
    return REG_NOMATCH;
}

size_t regerror(int err, const regex_t *re, char *buf, size_t size)
{
    (void)re;
    static const char *const msgs[] = {
        "success", "no match", "invalid regular expression", "invalid collating element",
        "invalid character class", "trailing backslash", "invalid backreference",
        "unmatched [", "unmatched ( or \\(", "unmatched \\{", "invalid contents of \\{\\}",
        "invalid range end", "out of memory", "invalid preceding regular expression",
    };
    const char *m = err >= 0 && err < (int)(sizeof(msgs) / sizeof(msgs[0])) ? msgs[err] : "unknown error";
    size_t n = strlen(m) + 1;
    if (size) {
        size_t c = n < size ? n : size;
        memcpy(buf, m, c - 1);
        buf[c - 1] = 0;
    }
    return n;
}
