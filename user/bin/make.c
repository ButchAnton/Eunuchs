/*
 * make: a small make for building C programs on Eunuchs with tcc.
 *
 * Supported: rules with several targets and prerequisites, order-only
 * prerequisites (after |), recipes (tab-indented; @ silent, - ignore
 * errors, + always run), variables (= := ::= += ?=, $(V) ${V} $V, command
 * line VAR=value, the environment), automatic variables $@ $< $^ $+ $? $*
 * $(@D) $(@F) $(<D) $(<F), pattern rules (%.o: %.c), suffix rules
 * (.c.o:), built-in rules for C (%.o from %.c, a program from %.c or %.o),
 * .PHONY, .DEFAULT, .SILENT, .IGNORE, include / -include, conditionals
 * (ifeq ifneq ifdef ifndef else endif), the functions wildcard patsubst
 * subst strip findstring filter filter-out sort dir notdir suffix basename
 * addprefix addsuffix join word words firstword lastword foreach if or and
 * shell origin error warning info, and substitution references
 * $(V:.c=.o). Options: -f file -C dir -n -s -i -k -B -p -q -j (ignored)
 * -d (debug: why targets are remade).
 *
 * Recipes run one line at a time through /bin/sh -c, as POSIX make does.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- small utilities ---- */

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

static char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* growable strings */
struct str {
    char *s;
    size_t n, cap;
};

static void sadd(struct str *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sadds(struct str *b, const char *s) { sadd(b, s, strlen(s)); }
static void saddc(struct str *b, char c) { sadd(b, &c, 1); }

static char *sfinish(struct str *b)
{
    if (!b->s)
        return xstrdup("");
    return b->s;
}

static const char *cur_file = "";
static int cur_line;
static const char *progname = "make";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    if (cur_line)
        fprintf(stderr, "%s:%d: *** ", cur_file, cur_line);
    else
        fprintf(stderr, "%s: *** ", progname);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ".  Stop.\n");
    va_end(ap);
    exit(2);
}

/* Split on blanks into a NULL-terminated vector (strings point into a copy). */
static char **split(const char *s, int *count)
{
    while (*s && isspace((unsigned char)*s))
        s++;                    /* so the first word is the start of the copy (split_free) */
    char *copy = xstrdup(s);
    int n = 0, cap = 8;
    char **v = xmalloc(cap * sizeof(char *));
    char *p = copy;
    for (;;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (n + 2 > cap) {
            cap *= 2;
            v = xrealloc(v, cap * sizeof(char *));
        }
        v[n++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = 0;
    }
    v[n] = 0;
    if (count)
        *count = n;
    if (!n)
        free(copy);
    return v;
}

static void split_free(char **v)
{
    if (v[0])
        free(v[0]);             /* the copy starts at the first word */
    free(v);
}

/* ---- variables ---- */

enum origin { O_DEFAULT, O_ENV, O_FILE, O_CMDLINE, O_AUTO };
static const char *const origin_names[] = { "default", "environment", "file", "command line", "automatic" };

struct var {
    char *name, *value;
    int recursive;              /* = (expand when used) or := (already expanded) */
    enum origin origin;
    struct var *next;
};

#define NHASH 211
static struct var *vars[NHASH];

static unsigned hash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % NHASH;
}

static struct var *var_find(const char *name)
{
    for (struct var *v = vars[hash(name)]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return 0;
}

static struct var *var_set(const char *name, const char *value, int recursive, enum origin o)
{
    struct var *v = var_find(name);
    if (v) {
        if (v->origin == O_CMDLINE && o != O_CMDLINE)
            return v;           /* the command line wins */
        free(v->value);
    } else {
        v = xmalloc(sizeof(*v));
        v->name = xstrdup(name);
        unsigned h = hash(name);
        v->next = vars[h];
        vars[h] = v;
    }
    v->value = xstrdup(value);
    v->recursive = recursive;
    v->origin = o;
    return v;
}

/* automatic variables of the recipe being run */
static struct {
    const char *at, *lt, *hat, *plus, *q, *star;
} autov;

static char *expand(const char *s);

/* ---- files and rules ---- */

struct rule;

struct file {
    char *name;
    struct dep *deps;           /* from all rules naming this target */
    char **cmds;                /* the recipe (one rule only) */
    int ncmds;
    const char *cmd_file;       /* where the recipe was defined */
    int cmd_line;
    int phony, precious;
    int state;                  /* 0 not visited, 1 in progress, 2 done */
    int remade;                 /* was rebuilt this run (for parents) */
    int failed;
    long long mtime;            /* ns; -1: does not exist */
    int mtime_known;
    char *stem;                 /* from the pattern rule that matched */
    int is_double_colon;
    struct file *next;
};

struct dep {
    struct file *f;
    int order_only;
    struct dep *next;
};

struct prule {                  /* pattern rule: targets with %, prerequisites with % */
    char **targets;
    char **deps;
    char **odeps;               /* order-only (after |), or NULL */
    char **cmds;
    int ncmds;
    const char *file;
    int line;
    int builtin;
    struct prule *next;
};

static struct file *files[NHASH];
static struct prule *prules, **prules_tail = &prules;
static struct file *default_goal;
static char *suffixes;          /* .SUFFIXES, space separated */

static int opt_n, opt_s, opt_i, opt_k, opt_B, opt_p, opt_q, opt_d;
static int errors;
static int recipes_run;         /* targets whose recipes ran (for "Nothing to be done") */

static struct file *file_get(const char *name)
{
    unsigned h = hash(name);
    for (struct file *f = files[h]; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    struct file *f = xmalloc(sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->name = xstrdup(name);
    f->next = files[h];
    files[h] = f;
    return f;
}

static struct file *file_lookup(const char *name)
{
    for (struct file *f = files[hash(name)]; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    return 0;
}

#ifdef __APPLE__
#define st_mtim st_mtimespec    /* so make can be tried out on the Mac */
#endif

static long long stat_mtime(const struct stat *st)
{
    return st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
}

static long long file_mtime(struct file *f)
{
    if (!f->mtime_known) {
        struct stat st;
        f->mtime = stat(f->name, &st) == 0 ? stat_mtime(&st) : -1;
        f->mtime_known = 1;
    }
    return f->mtime;
}

static void add_dep(struct file *f, struct file *d, int order_only)
{
    for (struct dep *e = f->deps; e; e = e->next)
        if (e->f == d)
            return;
    struct dep *n = xmalloc(sizeof(*n));
    n->f = d;
    n->order_only = order_only;
    n->next = 0;
    struct dep **pp = &f->deps;
    while (*pp)
        pp = &(*pp)->next;
    *pp = n;
}

/* ---- functions ---- */

/* Does word match pattern (one %)? Sets the stem. */
static int pat_match(const char *pat, const char *word, char *stem, size_t stemsize)
{
    const char *pc = strchr(pat, '%');
    if (!pc)
        return !strcmp(pat, word);
    size_t pre = pc - pat, post = strlen(pc + 1), wl = strlen(word);
    if (wl < pre + post || strncmp(pat, word, pre) || strcmp(pc + 1, word + wl - post))
        return 0;
    if (stem)
        snprintf(stem, stemsize, "%.*s", (int)(wl - pre - post), word + pre);
    return 1;
}

static void pat_subst(struct str *out, const char *pat, const char *stem)
{
    const char *pc = strchr(pat, '%');
    if (!pc) {
        sadds(out, pat);
        return;
    }
    sadd(out, pat, pc - pat);
    sadds(out, stem);
    sadds(out, pc + 1);
}

static void glob_dir(struct str *out, int *first, const char *pat);

static int glob_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) {
                if (glob_match(p + 1, t))
                    return 1;
                if (!*t)
                    return 0;
            }
        }
        if (*p == '?') {
            if (!*s)
                return 0;
            continue;
        }
        if (*p == '[') {
            int neg = p[1] == '!' || p[1] == '^', ok = 0;
            const char *q = p + 1 + neg;
            for (; *q && *q != ']'; q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= *q && *s <= q[2])
                        ok = 1;
                    q += 2;
                } else if (*q == *s) {
                    ok = 1;
                }
            }
            if (!*s || ok == neg)
                return 0;
            p = *q ? q : q - 1;
            continue;
        }
        if (*p != *s)
            return 0;
    }
    return !*s;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void glob_dir(struct str *out, int *first, const char *pat)
{
    if (!strpbrk(pat, "*?[")) {
        struct stat st;
        if (stat(pat, &st) == 0) {
            if (!*first)
                saddc(out, ' ');
            sadds(out, pat);
            *first = 0;
        }
        return;
    }
    const char *slash = strrchr(pat, '/');
    char dir[512];
    const char *base = pat;
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - pat) ? (int)(slash - pat) : 1, pat);
        base = slash + 1;
    } else {
        strcpy(dir, ".");
    }
    DIR *d = opendir(dir);
    if (!d)
        return;
    char **names = 0;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && base[0] != '.')
            continue;
        if (!glob_match(base, de->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = xrealloc(names, cap * sizeof(char *));
        }
        char full[600];
        if (slash)
            snprintf(full, sizeof(full), "%.*s/%s", (int)(slash - pat), pat, de->d_name);
        else
            snprintf(full, sizeof(full), "%s", de->d_name);
        names[n++] = xstrdup(full);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), cmp_str);
    for (int i = 0; i < n; i++) {
        if (!*first)
            saddc(out, ' ');
        sadds(out, names[i]);
        *first = 0;
        free(names[i]);
    }
    free(names);
}

static char *run_shell_capture(const char *cmd, int *status)
{
    int fds[2];
    if (pipe(fds) < 0)
        die("pipe: %s", strerror(errno));
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    close(fds[1]);
    struct str b = { 0 };
    char tmp[512];
    long r;
    while ((r = read(fds[0], tmp, sizeof(tmp))) > 0)
        sadd(&b, tmp, r);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (status)
        *status = st;
    char *s = sfinish(&b);
    /* newlines become spaces; the last one goes */
    size_t n = strlen(s);
    while (n && s[n - 1] == '\n')
        s[--n] = 0;
    for (char *p = s; *p; p++)
        if (*p == '\n')
            *p = ' ';
    return s;
}

/* Find the end of a $( ... ) or ${ ... } starting after the opener. */
static const char *match_paren(const char *p, char open, char close)
{
    int depth = 1;
    for (; *p; p++) {
        if (*p == open)
            depth++;
        else if (*p == close && --depth == 0)
            return p;
    }
    return 0;
}

/* Split function arguments at top-level commas. */
static int split_args(const char *s, char **args, int max)
{
    int n = 0, depth = 0;
    const char *start = s;
    for (const char *p = s;; p++) {
        if (*p == '(' || *p == '{')
            depth++;
        else if (*p == ')' || *p == '}')
            depth--;
        if ((*p == ',' && depth == 0 && n < max - 1) || !*p) {
            args[n++] = xstrndup(start, p - start);
            if (!*p)
                break;
            start = p + 1;
        }
    }
    return n;
}

static void add_word(struct str *out, int *first, const char *w, size_t n)
{
    if (!*first)
        saddc(out, ' ');
    sadd(out, w, n);
    *first = 0;
}

static char *call_function(const char *name, const char *rawargs)
{
    char *a[16];
    int na;
    struct str out = { 0 };
    int first = 1;
    /* foreach, if, or, and expand lazily; the others get expanded arguments */
    if (!strcmp(name, "foreach")) {
        na = split_args(rawargs, a, 3);
        if (na != 3)
            die("insufficient number of arguments to function 'foreach'");
        char *vname = expand(a[0]);
        char *list = expand(a[1]);
        int nw;
        char **w = split(list, &nw);
        struct var *old = var_find(vname);
        char *saved = old ? xstrdup(old->value) : 0;
        int saved_rec = old ? old->recursive : 0;
        enum origin saved_o = old ? old->origin : O_FILE;
        for (int i = 0; i < nw; i++) {
            struct var *v = var_set(vname, w[i], 0, O_AUTO);
            v->origin = O_AUTO;
            char *r = expand(a[2]);
            if (*r)
                add_word(&out, &first, r, strlen(r));
            free(r);
        }
        if (saved) {
            struct var *v = var_set(vname, saved, saved_rec, O_FILE);
            v->origin = saved_o;
            free(saved);
        } else if (nw) {
            var_set(vname, "", 0, O_AUTO);
        }
        split_free(w);
        free(vname);
        free(list);
        for (int i = 0; i < na; i++)
            free(a[i]);
        return sfinish(&out);
    }
    if (!strcmp(name, "if") || !strcmp(name, "or") || !strcmp(name, "and")) {
        na = split_args(rawargs, a, 16);
        char *result = 0;
        if (!strcmp(name, "if")) {
            char *c = expand(a[0]);
            char **w = split(c, 0);
            int t = w[0] != 0;
            split_free(w);
            free(c);
            result = t ? (na > 1 ? expand(a[1]) : xstrdup("")) : (na > 2 ? expand(a[2]) : xstrdup(""));
        } else if (!strcmp(name, "or")) {
            for (int i = 0; i < na && !result; i++) {
                char *c = expand(a[i]);
                char **w = split(c, 0);
                if (w[0])
                    result = c;
                else
                    free(c);
                split_free(w);
            }
            if (!result)
                result = xstrdup("");
        } else {
            for (int i = 0; i < na; i++) {
                free(result);
                result = expand(a[i]);
                char **w = split(result, 0);
                int empty = !w[0];
                split_free(w);
                if (empty) {
                    free(result);
                    result = xstrdup("");
                    break;
                }
            }
            if (!result)
                result = xstrdup("");
        }
        for (int i = 0; i < na; i++)
            free(a[i]);
        return result;
    }
    char *ex = expand(rawargs);
    if (!strcmp(name, "shell")) {
        char *r = run_shell_capture(ex, 0);
        free(ex);
        return r;
    }
    if (!strcmp(name, "error") || !strcmp(name, "warning") || !strcmp(name, "info")) {
        if (name[0] == 'e')
            die("%s", ex);
        if (name[0] == 'w')
            fprintf(stderr, "%s:%d: %s\n", cur_file, cur_line, ex);
        else
            printf("%s\n", ex);
        free(ex);
        return xstrdup("");
    }
    if (!strcmp(name, "origin")) {
        struct var *v = var_find(ex);
        free(ex);
        return xstrdup(v ? origin_names[v->origin] : "undefined");
    }
    na = split_args(ex, a, 16);
    free(ex);
#define WANT(n)                                                                                 \
    do {                                                                                        \
        if (na < (n))                                                                           \
            die("insufficient number of arguments (%d) to function '%s'", na, name);           \
    } while (0)
    if (!strcmp(name, "wildcard")) {
        char **w = split(a[0], 0);
        for (int i = 0; w[i]; i++)
            glob_dir(&out, &first, w[i]);
        split_free(w);
    } else if (!strcmp(name, "patsubst")) {
        WANT(3);
        char *pat = a[0], *rep = a[1];
        while (isspace((unsigned char)*pat))
            pat++;
        char **w = split(a[2], 0);
        char stem[512];
        for (int i = 0; w[i]; i++) {
            struct str t = { 0 };
            if (pat_match(pat, w[i], stem, sizeof(stem)))
                pat_subst(&t, rep, stem);
            else
                sadds(&t, w[i]);
            char *ts = sfinish(&t);
            add_word(&out, &first, ts, strlen(ts));
            free(ts);
        }
        split_free(w);
    } else if (!strcmp(name, "subst")) {
        WANT(3);
        const char *s = a[2];
        size_t fl = strlen(a[0]);
        if (!fl) {
            sadds(&out, s);
        } else {
            for (const char *p; (p = strstr(s, a[0])); s = p + fl) {
                sadd(&out, s, p - s);
                sadds(&out, a[1]);
            }
            sadds(&out, s);
        }
    } else if (!strcmp(name, "strip")) {
        char **w = split(a[0], 0);
        for (int i = 0; w[i]; i++)
            add_word(&out, &first, w[i], strlen(w[i]));
        split_free(w);
    } else if (!strcmp(name, "findstring")) {
        WANT(2);
        if (strstr(a[1], a[0]))
            sadds(&out, a[0]);
    } else if (!strcmp(name, "filter") || !strcmp(name, "filter-out")) {
        WANT(2);
        char **pats = split(a[0], 0), **w = split(a[1], 0);
        int keep_matching = !strcmp(name, "filter");
        for (int i = 0; w[i]; i++) {
            int m = 0;
            for (int j = 0; pats[j] && !m; j++)
                m = pat_match(pats[j], w[i], 0, 0);
            if (m == keep_matching)
                add_word(&out, &first, w[i], strlen(w[i]));
        }
        split_free(pats);
        split_free(w);
    } else if (!strcmp(name, "sort")) {
        int n;
        char **w = split(a[0], &n);
        qsort(w, n, sizeof(char *), cmp_str);
        for (int i = 0; i < n; i++)
            if (!i || strcmp(w[i], w[i - 1]))
                add_word(&out, &first, w[i], strlen(w[i]));
        /* the vector was reordered: free the block it was carved from */
        char *base = w[0];
        for (int i = 1; i < n; i++)
            if (w[i] < base)
                base = w[i];
        if (n)
            free(base);
        free(w);
    } else if (!strcmp(name, "dir") || !strcmp(name, "notdir")) {
        char **w = split(a[0], 0);
        for (int i = 0; w[i]; i++) {
            const char *sl = strrchr(w[i], '/');
            if (name[0] == 'd') {
                if (sl)
                    add_word(&out, &first, w[i], sl - w[i] + 1);
                else
                    add_word(&out, &first, "./", 2);
            } else {
                const char *b = sl ? sl + 1 : w[i];
                add_word(&out, &first, b, strlen(b));
            }
        }
        split_free(w);
    } else if (!strcmp(name, "suffix") || !strcmp(name, "basename")) {
        char **w = split(a[0], 0);
        for (int i = 0; w[i]; i++) {
            const char *dot = strrchr(w[i], '.'), *sl = strrchr(w[i], '/');
            if (dot && sl && dot < sl)
                dot = 0;
            if (name[0] == 's') {
                if (dot)
                    add_word(&out, &first, dot, strlen(dot));
            } else {
                add_word(&out, &first, w[i], dot ? (size_t)(dot - w[i]) : strlen(w[i]));
            }
        }
        split_free(w);
    } else if (!strcmp(name, "addprefix") || !strcmp(name, "addsuffix")) {
        WANT(2);
        char **w = split(a[1], 0);
        for (int i = 0; w[i]; i++) {
            if (!first)
                saddc(&out, ' ');
            first = 0;
            if (name[3] == 'p') {
                sadds(&out, a[0]);
                sadds(&out, w[i]);
            } else {
                sadds(&out, w[i]);
                sadds(&out, a[0]);
            }
        }
        split_free(w);
    } else if (!strcmp(name, "join")) {
        WANT(2);
        char **x = split(a[0], 0), **y = split(a[1], 0);
        for (int i = 0; x[i] || y[i]; i++) {
            if (!first)
                saddc(&out, ' ');
            first = 0;
            if (x[i])
                sadds(&out, x[i]);
            if (y[i])
                sadds(&out, y[i]);
            if (!x[i] || !y[i]) {
                /* one list ran out: copy the rest of the other */
                char **rest = x[i] ? x + i + 1 : y + i + 1;
                for (; *rest; rest++) {
                    saddc(&out, ' ');
                    sadds(&out, *rest);
                }
                break;
            }
        }
        split_free(x);
        split_free(y);
    } else if (!strcmp(name, "word")) {
        WANT(2);
        int n = atoi(a[0]), nw;
        char **w = split(a[1], &nw);
        if (n < 1)
            die("first argument to 'word' function must be greater than 0");
        if (n <= nw)
            sadds(&out, w[n - 1]);
        split_free(w);
    } else if (!strcmp(name, "words") || !strcmp(name, "firstword") || !strcmp(name, "lastword")) {
        int nw;
        char **w = split(a[0], &nw);
        char num[16];
        if (name[0] == 'w') {
            snprintf(num, sizeof(num), "%d", nw);
            sadds(&out, num);
        } else if (nw) {
            sadds(&out, name[0] == 'f' ? w[0] : w[nw - 1]);
        }
        split_free(w);
    } else {
        die("unknown function '%s'", name);
    }
#undef WANT
    for (int i = 0; i < na; i++)
        free(a[i]);
    return sfinish(&out);
}

static int depth_guard;

static char *var_value(const char *name)
{
    /* automatic variables, and their D and F forms */
    if ((name[0] == '@' || name[0] == '<' || name[0] == '^' || name[0] == '+' || name[0] == '?' ||
         name[0] == '*') && (!name[1] || ((name[1] == 'D' || name[1] == 'F') && !name[2]))) {
        const char *v = name[0] == '@' ? autov.at : name[0] == '<' ? autov.lt : name[0] == '^' ? autov.hat
                      : name[0] == '+' ? autov.plus : name[0] == '?' ? autov.q : autov.star;
        if (!v)
            v = "";
        if (!name[1])
            return xstrdup(v);
        struct str out = { 0 };
        int first = 1;
        char **w = split(v, 0);
        for (int i = 0; w[i]; i++) {
            const char *sl = strrchr(w[i], '/');
            if (name[1] == 'D') {
                if (sl)
                    add_word(&out, &first, w[i], sl == w[i] ? 1 : (size_t)(sl - w[i]));
                else
                    add_word(&out, &first, ".", 1);
            } else {
                const char *b = sl ? sl + 1 : w[i];
                add_word(&out, &first, b, strlen(b));
            }
        }
        split_free(w);
        return sfinish(&out);
    }
    struct var *v = var_find(name);
    if (!v)
        return xstrdup("");
    if (!v->recursive)
        return xstrdup(v->value);
    if (++depth_guard > 100)
        die("Recursive variable '%s' references itself (eventually)", name);
    char *r = expand(v->value);
    depth_guard--;
    return r;
}

static char *expand(const char *s)
{
    struct str out = { 0 };
    sadds(&out, "");
    for (const char *p = s; *p; p++) {
        if (*p != '$') {
            saddc(&out, *p);
            continue;
        }
        p++;
        if (!*p)
            break;
        if (*p == '$') {
            saddc(&out, '$');
            continue;
        }
        if (*p == '(' || *p == '{') {
            char close = *p == '(' ? ')' : '}';
            const char *e = match_paren(p + 1, *p, close);
            if (!e)
                die("unterminated variable reference");
            char *inner = xstrndup(p + 1, e - p - 1);
            p = e;
            /* a function call: name followed by a blank */
            char *sp = inner;
            while (*sp && !isspace((unsigned char)*sp) && *sp != ':' && *sp != '$')
                sp++;
            if (*sp && isspace((unsigned char)*sp)) {
                char fname[32];
                snprintf(fname, sizeof(fname), "%.*s", (int)(sp - inner), inner);
                static const char *const fns[] = {
                    "wildcard", "patsubst", "subst", "strip", "findstring", "filter", "filter-out",
                    "sort", "dir", "notdir", "suffix", "basename", "addprefix", "addsuffix", "join",
                    "word", "words", "firstword", "lastword", "foreach", "if", "or", "and", "shell",
                    "origin", "error", "warning", "info", 0,
                };
                int known = 0;
                for (int i = 0; fns[i]; i++)
                    if (!strcmp(fns[i], fname))
                        known = 1;
                if (known) {
                    while (isspace((unsigned char)*sp))
                        sp++;
                    char *r = call_function(fname, sp);
                    sadds(&out, r);
                    free(r);
                    free(inner);
                    continue;
                }
            }
            /* $(VAR) or $(VAR:a=b) */
            char *name_ex = expand(inner);
            free(inner);
            char *colon = 0;
            for (char *q = name_ex; *q; q++)
                if (*q == ':') {
                    colon = q;
                    break;
                }
            if (colon && strchr(colon, '=')) {
                *colon = 0;
                char *eq = strchr(colon + 1, '=');
                *eq = 0;
                const char *from = colon + 1, *to = eq + 1;
                char *val = var_value(name_ex);
                char **w = split(val, 0);
                int first = 1;
                char stem[512];
                for (int i = 0; w[i]; i++) {
                    struct str t = { 0 };
                    if (strchr(from, '%')) {
                        if (pat_match(from, w[i], stem, sizeof(stem)))
                            pat_subst(&t, to, stem);
                        else
                            sadds(&t, w[i]);
                    } else {
                        size_t wl = strlen(w[i]), fl = strlen(from);
                        if (wl >= fl && !strcmp(w[i] + wl - fl, from)) {
                            sadd(&t, w[i], wl - fl);
                            sadds(&t, to);
                        } else {
                            sadds(&t, w[i]);
                        }
                    }
                    char *ts = sfinish(&t);
                    if (!first)
                        saddc(&out, ' ');
                    sadds(&out, ts);
                    first = 0;
                    free(ts);
                }
                split_free(w);
                free(val);
            } else {
                char *val = var_value(name_ex);
                sadds(&out, val);
                free(val);
            }
            free(name_ex);
            continue;
        }
        char name[2] = { *p, 0 };
        char *val = var_value(name);
        sadds(&out, val);
        free(val);
    }
    return sfinish(&out);
}

/* ---- reading makefiles ---- */

struct reader {
    FILE *f;
    const char *name;
    int line;
};

/* A logical line: backslash-newline joins lines. Recipe lines keep the
 * backslash-newline (the shell sees it); other lines get a blank. */
static char *read_line(struct reader *r, int *is_recipe)
{
    struct str b = { 0 };
    int c, any = 0;
    int first = 1;
    *is_recipe = 0;
    for (;;) {
        c = fgetc(r->f);
        if (c == EOF) {
            if (!any)
                return 0;
            break;
        }
        any = 1;
        if (first) {
            first = 0;
            if (c == '\t')
                *is_recipe = 1;
        }
        if (c == '\n') {
            r->line++;
            if (b.n && b.s[b.n - 1] == '\\') {
                /* an even number of backslashes is not a continuation */
                size_t k = b.n;
                int nb = 0;
                while (k && b.s[k - 1] == '\\')
                    k--, nb++;
                if (nb % 2) {
                    if (*is_recipe) {
                        saddc(&b, '\n');
                        c = fgetc(r->f);
                        if (c != '\t' && c != EOF)
                            ungetc(c, r->f);
                    } else {
                        b.n--;
                        while (b.n && isspace((unsigned char)b.s[b.n - 1]))
                            b.n--;
                        b.s[b.n] = 0;
                        saddc(&b, ' ');
                        while ((c = fgetc(r->f)) == ' ' || c == '\t')
                            ;
                        if (c != EOF)
                            ungetc(c, r->f);
                    }
                    continue;
                }
            }
            break;
        }
        saddc(&b, (char)c);
    }
    return sfinish(&b);
}

static void strip_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == '#') {
            memmove(p, p + 1, strlen(p));
            continue;
        }
        if (*p == '#') {
            *p = 0;
            return;
        }
    }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    return s;
}

static void read_makefile(const char *path, int must_exist);

/* conditionals */
#define MAXCOND 32
static struct {
    int active;                 /* this branch is being read */
    int taken;                  /* some branch was taken */
    int parent_active;
} cond[MAXCOND];
static int ncond;

static int reading_active(void)
{
    return ncond == 0 || cond[ncond - 1].active;
}

/* ifeq (a,b) / ifeq "a" "b" */
static int eval_ifeq(char *args)
{
    char *a, *b;
    args = trim(args);
    if (*args == '(') {
        char *e = strrchr(args, ')');
        if (!e)
            die("invalid syntax in conditional");
        *e = 0;
        char *inner = args + 1;
        /* the comma at depth 0 */
        int depth = 0;
        char *comma = 0;
        for (char *p = inner; *p; p++) {
            if (*p == '(')
                depth++;
            else if (*p == ')')
                depth--;
            else if (*p == ',' && depth == 0) {
                comma = p;
                break;
            }
        }
        if (!comma)
            die("invalid syntax in conditional");
        *comma = 0;
        a = inner;
        b = comma + 1;
    } else {
        char q1 = *args;
        if (q1 != '"' && q1 != '\'')
            die("invalid syntax in conditional");
        a = args + 1;
        char *e = strchr(a, q1);
        if (!e)
            die("invalid syntax in conditional");
        *e = 0;
        b = trim(e + 1);
        char q2 = *b;
        if (q2 != '"' && q2 != '\'')
            die("invalid syntax in conditional");
        b++;
        e = strchr(b, q2);
        if (!e)
            die("invalid syntax in conditional");
        *e = 0;
    }
    char *ea = expand(a), *eb = expand(b);
    int r = !strcmp(trim(ea), trim(eb));
    free(ea);
    free(eb);
    return r;
}

/* Returns 1 if the line was a conditional directive. */
static int conditional(char *line)
{
    char *s = trim(line);
    int neg = 0;
    if (!strncmp(s, "ifeq", 4) && (isspace((unsigned char)s[4]) || s[4] == '(')) {
        int active = reading_active() && eval_ifeq(s + 4);
        goto push_cond;
    push_cond:
        if (ncond == MAXCOND)
            die("too many nested conditionals");
        cond[ncond].parent_active = reading_active();
        cond[ncond].active = cond[ncond].parent_active && active;
        cond[ncond].taken = cond[ncond].active;
        ncond++;
        return 1;
    }
    if (!strncmp(s, "ifneq", 5) && (isspace((unsigned char)s[5]) || s[5] == '(')) {
        int active = reading_active() && !eval_ifeq(s + 5);
        if (ncond == MAXCOND)
            die("too many nested conditionals");
        cond[ncond].parent_active = reading_active();
        cond[ncond].active = cond[ncond].parent_active && active;
        cond[ncond].taken = cond[ncond].active;
        ncond++;
        return 1;
    }
    if ((!strncmp(s, "ifdef", 5) && isspace((unsigned char)s[5])) ||
        (!strncmp(s, "ifndef", 6) && isspace((unsigned char)s[6]) && (neg = 1))) {
        int active = 0;
        if (reading_active()) {
            char *name = expand(trim(s + (neg ? 6 : 5)));
            struct var *v = var_find(trim(name));
            active = v && v->value[0];
            if (neg)
                active = !active;
            free(name);
        }
        if (ncond == MAXCOND)
            die("too many nested conditionals");
        cond[ncond].parent_active = reading_active();
        cond[ncond].active = cond[ncond].parent_active && active;
        cond[ncond].taken = cond[ncond].active;
        ncond++;
        return 1;
    }
    if (!strncmp(s, "else", 4) && (!s[4] || isspace((unsigned char)s[4]))) {
        if (!ncond)
            die("extraneous 'else'");
        struct { int active, taken, parent_active; } *c = (void *)&cond[ncond - 1];
        char *rest = trim(s + 4);
        if (*rest) {
            /* else ifeq ... : a chained conditional */
            ncond--;
            int was_taken = c->taken, parent = c->parent_active;
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", rest);
            if (!was_taken && parent) {
                conditional(tmp);
                cond[ncond - 1].parent_active = parent;
            } else {
                /* already taken: push an inactive one that stays taken */
                cond[ncond].parent_active = parent;
                cond[ncond].active = 0;
                cond[ncond].taken = 1;
                ncond++;
            }
            return 1;
        }
        c->active = c->parent_active && !c->taken;
        if (c->active)
            c->taken = 1;
        return 1;
    }
    if (!strncmp(s, "endif", 5) && (!s[5] || isspace((unsigned char)s[5]))) {
        if (!ncond)
            die("extraneous 'endif'");
        ncond--;
        return 1;
    }
    return 0;
}

/* The rule whose recipe lines are being read. */
static struct file **rule_targets;
static int nrule_targets;
static struct prule *rule_pattern;

static void add_recipe_line(const char *cmd)
{
    if (rule_pattern) {
        rule_pattern->cmds = xrealloc(rule_pattern->cmds, (rule_pattern->ncmds + 1) * sizeof(char *));
        rule_pattern->cmds[rule_pattern->ncmds++] = xstrdup(cmd);
        return;
    }
    for (int i = 0; i < nrule_targets; i++) {
        struct file *f = rule_targets[i];
        f->cmds = xrealloc(f->cmds, (f->ncmds + 1) * sizeof(char *));
        f->cmds[f->ncmds++] = xstrdup(cmd);
    }
}

static void end_rule(void)
{
    free(rule_targets);
    rule_targets = 0;
    nrule_targets = 0;
    rule_pattern = 0;
}

static int is_suffix_rule(const char *t)
{
    /* .c.o or .c (single suffix), both known suffixes */
    if (t[0] != '.' || !suffixes)
        return 0;
    const char *second = strchr(t + 1, '.');
    char a[64], b[64];
    snprintf(a, sizeof(a), "%.*s", second ? (int)(second - t) : (int)strlen(t), t);
    snprintf(b, sizeof(b), "%s", second ? second : "");
    char **w = split(suffixes, 0);
    int fa = 0, fb = !second;
    for (int i = 0; w[i]; i++) {
        if (!strcmp(w[i], a))
            fa = 1;
        if (second && !strcmp(w[i], b))
            fb = 1;
    }
    split_free(w);
    return fa && fb;
}

static struct prule *add_prule(char **targets, char **deps, int builtin)
{
    struct prule *p = xmalloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    p->targets = targets;
    p->deps = deps;
    p->file = cur_file;
    p->line = cur_line;
    p->builtin = builtin;
    /* later rules override earlier ones with the same target and prerequisites */
    *prules_tail = p;
    prules_tail = &p->next;
    return p;
}

static void parse_rule(char *line, char *colon, int is_double)
{
    char *tpart = xstrndup(line, colon - line);
    char *rest = colon + (is_double ? 2 : 1);
    /* an inline recipe after ; */
    char *semi = 0;
    for (char *p = rest; *p; p++)
        if (*p == ';') {
            semi = p;
            break;
        }
    char *inline_cmd = 0;
    if (semi) {
        *semi = 0;
        inline_cmd = semi + 1;
        while (*inline_cmd == ' ' || *inline_cmd == '\t')
            inline_cmd++;
    }
    char *texp = expand(tpart);
    char *dexp = expand(rest);
    free(tpart);
    char **targets = split(texp, 0);
    end_rule();
    if (!targets[0]) {
        split_free(targets);
        free(texp);
        free(dexp);
        return;
    }
    /* order-only prerequisites after | */
    char *bar = strchr(dexp, '|');
    char *oo = 0;
    if (bar) {
        *bar = 0;
        oo = bar + 1;
    }
    int is_pattern = strchr(targets[0], '%') != 0;
    if (is_pattern) {
        char **deps = split(dexp, 0);
        rule_pattern = add_prule(targets, deps, 0);
        if (oo)
            rule_pattern->odeps = split(oo, 0);
    } else if (targets[0][0] == '.' && is_suffix_rule(targets[0]) && !targets[1]) {
        /* .c.o: -> %.o: %.c ; .c: -> %: %.c */
        const char *second = strchr(targets[0] + 1, '.');
        char t[80], d[80];
        if (second) {
            snprintf(t, sizeof(t), "%%%s", second);
            snprintf(d, sizeof(d), "%%%.*s", (int)(second - targets[0]), targets[0]);
        } else {
            snprintf(t, sizeof(t), "%%");
            snprintf(d, sizeof(d), "%%%s", targets[0]);
        }
        char **tv = split(t, 0), **dv = split(d, 0);
        rule_pattern = add_prule(tv, dv, 0);
        split_free(targets);
    } else {
        char **deps = split(dexp, 0);
        char **odeps = oo ? split(oo, 0) : 0;
        int nt = 0;
        while (targets[nt])
            nt++;
        rule_targets = xmalloc(nt * sizeof(struct file *));
        for (int i = 0; i < nt; i++) {
            const char *t = targets[i];
            if (!strcmp(t, ".PHONY") || !strcmp(t, ".PRECIOUS") || !strcmp(t, ".SILENT") ||
                !strcmp(t, ".IGNORE") || !strcmp(t, ".SUFFIXES")) {
                for (int j = 0; deps[j]; j++) {
                    struct file *d = file_get(deps[j]);
                    if (t[1] == 'P' && t[2] == 'H')
                        d->phony = 1;
                    else if (t[1] == 'P')
                        d->precious = 1;
                }
                if (!strcmp(t, ".SILENT") && !deps[0])
                    opt_s = 1;
                if (!strcmp(t, ".IGNORE") && !deps[0])
                    opt_i = 1;
                if (!strcmp(t, ".SUFFIXES")) {
                    if (!deps[0]) {
                        free(suffixes);
                        suffixes = 0;
                    } else {
                        struct str s = { 0 };
                        if (suffixes)
                            sadds(&s, suffixes);
                        for (int j = 0; deps[j]; j++) {
                            saddc(&s, ' ');
                            sadds(&s, deps[j]);
                        }
                        free(suffixes);
                        suffixes = sfinish(&s);
                    }
                }
                continue;
            }
            struct file *f = file_get(t);
            if (!default_goal && t[0] != '.' && !strchr(t, '%'))
                default_goal = f;
            else if (!default_goal && t[0] == '.' && strchr(t, '/'))
                default_goal = f;
            for (int j = 0; deps[j]; j++)
                add_dep(f, file_get(deps[j]), 0);
            for (int j = 0; odeps && odeps[j]; j++)
                add_dep(f, file_get(odeps[j]), 1);
            if (is_double)
                f->is_double_colon = 1;
            rule_targets[nrule_targets++] = f;
        }
        /* only targets that get a recipe here: a new recipe replaces an old one */
        split_free(deps);
        if (odeps)
            split_free(odeps);
        split_free(targets);
    }
    if (rule_targets || rule_pattern) {
        /* remember where the recipe starts; warn about overrides when it does */
        for (int i = 0; i < nrule_targets; i++)
            rule_targets[i]->cmd_file = cur_file, rule_targets[i]->cmd_line = cur_line;
        if (inline_cmd) {
            for (int i = 0; i < nrule_targets; i++) {
                struct file *f = rule_targets[i];
                for (int k = 0; k < f->ncmds; k++)
                    free(f->cmds[k]);
                f->ncmds = 0;
            }
            add_recipe_line(inline_cmd);
        }
    }
    free(texp);
    free(dexp);
}

/* Assignment? Returns 1 and handles it. */
static int assignment(char *line, enum origin o)
{
    /* find the operator before any ':' that would make it a rule */
    char *p = line;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '(' || *p == '{')
            depth++;
        else if (*p == ')' || *p == '}')
            depth--;
        else if (depth == 0 && (*p == '=' || *p == ':' || *p == '+' || *p == '?' || *p == '!'))
            break;
    }
    if (!*p)
        return 0;
    int kind;                   /* '=' ':' '+' '?' '!' */
    char *op = p, *val;
    if (*p == '=') {
        kind = '=';
        val = p + 1;
    } else if (p[0] == ':' && p[1] == '=') {
        kind = ':';
        val = p + 2;
    } else if (p[0] == ':' && p[1] == ':' && p[2] == '=') {
        kind = ':';
        val = p + 3;
    } else if ((p[0] == '+' || p[0] == '?' || p[0] == '!') && p[1] == '=') {
        kind = p[0];
        val = p + 2;
    } else {
        return 0;
    }
    char *name_raw = xstrndup(line, op - line);
    char *name = expand(trim(name_raw));
    free(name_raw);
    char *tname = trim(name);
    if (!*tname || strchr(tname, ' '))
        die("invalid variable name '%s'", tname);
    while (*val == ' ' || *val == '\t')
        val++;
    /* trailing blanks stay, as in make */
    switch (kind) {
    case '=':
        var_set(tname, val, 1, o);
        break;
    case ':': {
        char *e = expand(val);
        var_set(tname, e, 0, o);
        free(e);
        break;
    }
    case '?':
        if (!var_find(tname))
            var_set(tname, val, 1, o);
        break;
    case '!': {
        char *e = expand(val);
        char *r = run_shell_capture(e, 0);
        var_set(tname, r, 0, o);
        free(e);
        free(r);
        break;
    }
    case '+': {
        struct var *v = var_find(tname);
        if (!v) {
            var_set(tname, val, 1, o);
        } else {
            struct str s = { 0 };
            sadds(&s, v->value);
            if (v->value[0])
                saddc(&s, ' ');
            if (v->recursive) {
                sadds(&s, val);
            } else {
                char *e = expand(val);
                sadds(&s, e);
                free(e);
            }
            char *nv = sfinish(&s);
            var_set(tname, nv, v->recursive, o == O_CMDLINE ? O_CMDLINE : v->origin == O_CMDLINE ? O_CMDLINE : o);
            free(nv);
        }
        break;
    }
    }
    free(name);
    return 1;
}

static void parse_line(char *line, int is_recipe)
{
    if (is_recipe && (rule_targets || rule_pattern)) {
        if (!reading_active())
            return;
        /* a recipe line: everything after the tab, comments included */
        add_recipe_line(line + 1);
        return;
    }
    char *copy = xstrdup(line);
    strip_comment(copy);
    char *s = trim(copy);
    if (!*s) {
        free(copy);
        return;
    }
    if (conditional(s)) {
        free(copy);
        return;
    }
    if (!reading_active()) {
        free(copy);
        return;
    }
    if (!strncmp(s, "include ", 8) || !strncmp(s, "-include ", 9) || !strncmp(s, "sinclude ", 9)) {
        int optional = s[0] != 'i';
        char *names = expand(s + (s[0] == 'i' ? 8 : 9));
        char **w = split(names, 0);
        end_rule();
        for (int i = 0; w[i]; i++) {
            const char *sf = cur_file;
            int sl = cur_line;
            read_makefile(w[i], !optional);
            cur_file = sf;
            cur_line = sl;
        }
        split_free(w);
        free(names);
        free(copy);
        return;
    }
    if (!strncmp(s, "export", 6) && (!s[6] || isspace((unsigned char)s[6]))) {
        /* export VAR or export VAR = value: put it in the environment for recipes */
        char *rest = trim(s + 6);
        if (*rest && !assignment(rest, O_FILE)) {
            char **w = split(rest, 0);
            for (int i = 0; w[i]; i++) {
                char *v = var_value(w[i]);
                setenv(w[i], v, 1);
                free(v);
            }
            split_free(w);
        } else if (*rest) {
            char name[128];
            int n = 0;
            while (rest[n] && !strchr(" \t:+?=!", rest[n]) && n < 127)
                n++;
            snprintf(name, sizeof(name), "%.*s", n, rest);
            char *v = var_value(name);
            setenv(name, v, 1);
            free(v);
        }
        free(copy);
        return;
    }
    if (!strncmp(s, "unexport", 8) || !strncmp(s, "vpath", 5) || !strncmp(s, "override ", 9)) {
        if (!strncmp(s, "override ", 9))
            assignment(s + 9, O_CMDLINE);
        free(copy);
        return;
    }
    if (assignment(s, O_FILE)) {
        end_rule();
        free(copy);
        return;
    }
    /* a rule: the first ':' outside $( ) */
    int depth = 0;
    char *colon = 0;
    for (char *p = s; *p; p++) {
        if (*p == '(' || *p == '{')
            depth++;
        else if (*p == ')' || *p == '}')
            depth--;
        else if (*p == ':' && depth == 0) {
            colon = p;
            break;
        }
    }
    if (!colon) {
        if (is_recipe)
            die("recipe commences before first target");
        die("missing separator");
    }
    /* the recipe part after ';' must keep its comment characters: use the raw line */
    char *raw = xstrdup(line);
    char *rs = raw;
    while (isspace((unsigned char)*rs))
        rs++;
    char *rcolon = rs + (colon - s);
    char *semi = strchr(rcolon, ';');
    if (!semi) {
        free(raw);
        raw = 0;
    }
    if (raw)
        parse_rule(rs, rcolon, rcolon[1] == ':');
    else
        parse_rule(s, colon, colon[1] == ':');
    free(raw);
    free(copy);
}

static void read_makefile(const char *path, int must_exist)
{
    FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
    if (!f) {
        if (must_exist)
            die("%s: %s", path, strerror(errno));
        return;
    }
    struct reader r = { f, xstrdup(path), 0 };
    cur_file = r.name;
    int saved_ncond = ncond;
    char *line;
    int is_recipe;
    for (;;) {
        cur_line = r.line + 1;
        line = read_line(&r, &is_recipe);
        if (!line)
            break;
        cur_line = r.line;
        parse_line(line, is_recipe);
        free(line);
    }
    if (ncond != saved_ncond)
        die("missing 'endif'");
    end_rule();
    if (f != stdin)
        fclose(f);
    cur_line = 0;
}

/* ---- built-in rules and variables ---- */

static void builtins(void)
{
    var_set("CC", "tcc", 1, O_DEFAULT);
    var_set("CFLAGS", "", 1, O_DEFAULT);
    var_set("CPPFLAGS", "", 1, O_DEFAULT);
    var_set("LDFLAGS", "", 1, O_DEFAULT);
    var_set("LDLIBS", "", 1, O_DEFAULT);
    var_set("AR", "tcc -ar", 1, O_DEFAULT);
    var_set("ARFLAGS", "rcs", 1, O_DEFAULT);
    var_set("RM", "rm -f", 1, O_DEFAULT);
    var_set("SHELL", "/bin/sh", 1, O_DEFAULT);
    var_set("MAKE", "make", 1, O_DEFAULT);
    var_set("COMPILE.c", "$(CC) $(CFLAGS) $(CPPFLAGS) -c", 1, O_DEFAULT);
    var_set("LINK.c", "$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)", 1, O_DEFAULT);
    var_set("LINK.o", "$(CC) $(LDFLAGS)", 1, O_DEFAULT);
    suffixes = xstrdup(".out .a .o .c .h .s .S");

    struct prule *p;
    p = add_prule(split("%.o", 0), split("%.c", 0), 1);
    p->cmds = xmalloc(sizeof(char *));
    p->cmds[p->ncmds++] = xstrdup("$(COMPILE.c) -o $@ $<");
    p = add_prule(split("%", 0), split("%.c", 0), 1);
    p->cmds = xmalloc(sizeof(char *));
    p->cmds[p->ncmds++] = xstrdup("$(LINK.c) -o $@ $^ $(LDLIBS)");
    p = add_prule(split("%", 0), split("%.o", 0), 1);
    p->cmds = xmalloc(sizeof(char *));
    p->cmds[p->ncmds++] = xstrdup("$(LINK.o) -o $@ $^ $(LDLIBS)");
    p = add_prule(split("%.o", 0), split("%.S", 0), 1);
    p->cmds = xmalloc(sizeof(char *));
    p->cmds[p->ncmds++] = xstrdup("$(CC) $(CPPFLAGS) -c -o $@ $<");
    p = add_prule(split("%.o", 0), split("%.s", 0), 1);
    p->cmds = xmalloc(sizeof(char *));
    p->cmds[p->ncmds++] = xstrdup("$(CC) -c -o $@ $<");
}

/* ---- building ---- */

static int build(struct file *f, int depth, const char *parent);

/* Can the file be made: it exists, or some rule makes it? */
static int can_make(const char *name, int depth);

/* Try the pattern rules for f; on success add the rule's prerequisites and recipe. */
static int find_pattern_rule(struct file *f, int depth)
{
    if (depth > 8)
        return 0;
    char stem[512];
    /* two passes: rules whose prerequisites exist or have rules, then (only
       for non-terminal targets) nothing more: no chaining beyond that check */
    for (int pass = 0; pass < 2; pass++) {
        for (struct prule *p = prules; p; p = p->next) {
            if (!p->ncmds)
                continue;
            if (pass == 0 && p->builtin)
                continue;           /* the makefile's own rules first */
            if (pass == 1 && !p->builtin)
                continue;
            for (int t = 0; p->targets[t]; t++) {
                /* match-anything rules (%) only for names without a dot suffix we know */
                if (!pat_match(p->targets[t], f->name, stem, sizeof(stem)))
                    continue;
                if (!strcmp(p->targets[t], "%") && strchr(f->name, '.') &&
                    strrchr(f->name, '.') > strrchr(f->name, '/'))
                    continue;       /* "%: %.c" doesn't make foo.o from foo.o.c */
                int ok = 1;
                for (int d = 0; p->deps[d] && ok; d++) {
                    struct str dn = { 0 };
                    pat_subst(&dn, p->deps[d], stem);
                    char *n = sfinish(&dn);
                    ok = can_make(n, depth + 1);
                    free(n);
                }
                if (!ok)
                    continue;
                for (int d = 0; p->deps[d]; d++) {
                    struct str dn = { 0 };
                    pat_subst(&dn, p->deps[d], stem);
                    char *n = sfinish(&dn);
                    /* the pattern's prerequisites come first ($< is the first) */
                    struct file *df = file_get(n);
                    struct dep *e = xmalloc(sizeof(*e));
                    e->f = df;
                    e->order_only = 0;
                    e->next = 0;
                    int dup = 0;
                    for (struct dep *x = f->deps; x; x = x->next)
                        if (x->f == df)
                            dup = 1;
                    if (dup) {
                        free(e);
                    } else if (d == 0) {
                        e->next = f->deps;
                        f->deps = e;
                    } else {
                        struct dep **pp = &f->deps;
                        while (*pp)
                            pp = &(*pp)->next;
                        *pp = e;
                    }
                    free(n);
                }
                for (int d = 0; p->odeps && p->odeps[d]; d++) {
                    struct str dn = { 0 };
                    pat_subst(&dn, p->odeps[d], stem);
                    char *n = sfinish(&dn);
                    add_dep(f, file_get(n), 1);
                    free(n);
                }
                f->cmds = p->cmds;
                f->ncmds = p->ncmds;
                f->cmd_file = p->file;
                f->cmd_line = p->line;
                free(f->stem);
                f->stem = xstrdup(stem);
                return 1;
            }
        }
    }
    return 0;
}

static int can_make(const char *name, int depth)
{
    struct file *f = file_lookup(name);
    if (f && (f->ncmds || f->deps || f->phony))
        return 1;
    struct stat st;
    if (stat(name, &st) == 0)
        return 1;
    if (depth > 4)
        return 0;
    char stem[512];
    for (struct prule *p = prules; p; p = p->next) {
        if (!p->ncmds)
            continue;
        for (int t = 0; p->targets[t]; t++) {
            if (!pat_match(p->targets[t], name, stem, sizeof(stem)))
                continue;
            if (!strcmp(p->targets[t], "%"))
                continue;           /* don't chain through match-anything rules */
            int ok = 1;
            for (int d = 0; p->deps[d] && ok; d++) {
                struct str dn = { 0 };
                pat_subst(&dn, p->deps[d], stem);
                char *n = sfinish(&dn);
                ok = can_make(n, depth + 1);
                free(n);
            }
            if (ok)
                return 1;
        }
    }
    return 0;
}

static void collect_auto(struct file *f, long long my_mtime)
{
    struct str hat = { 0 }, plus = { 0 }, q = { 0 };
    sadds(&hat, "");
    sadds(&plus, "");
    sadds(&q, "");
    int fh = 1, fp = 1, fq = 1;
    for (struct dep *d = f->deps; d; d = d->next) {
        if (d->order_only)
            continue;
        add_word(&plus, &fp, d->f->name, strlen(d->f->name));
        int seen = 0;
        for (struct dep *e = f->deps; e != d; e = e->next)
            if (!e->order_only && e->f == d->f)
                seen = 1;
        if (!seen)
            add_word(&hat, &fh, d->f->name, strlen(d->f->name));
        if (my_mtime < 0 || d->f->remade || file_mtime(d->f) > my_mtime)
            add_word(&q, &fq, d->f->name, strlen(d->f->name));
    }
    autov.at = f->name;
    autov.lt = "";
    for (struct dep *d = f->deps; d; d = d->next)
        if (!d->order_only) {
            autov.lt = d->f->name;
            break;
        }
    autov.hat = sfinish(&hat);
    autov.plus = sfinish(&plus);
    autov.q = sfinish(&q);
    if (f->stem) {
        autov.star = f->stem;
    } else {
        /* $* for explicit rules: the target without a known suffix */
        static char star[512];
        snprintf(star, sizeof(star), "%s", f->name);
        char *dot = strrchr(star, '.');
        if (dot && suffixes) {
            char **w = split(suffixes, 0);
            int known = 0;
            for (int i = 0; w[i]; i++)
                if (!strcmp(w[i], dot))
                    known = 1;
            split_free(w);
            if (known)
                *dot = 0;
            else
                star[0] = 0;
        } else {
            star[0] = 0;
        }
        autov.star = star;
    }
}

static void free_auto(void)
{
    free((char *)autov.hat);
    free((char *)autov.plus);
    free((char *)autov.q);
    memset(&autov, 0, sizeof(autov));
}

static int run_command(const char *cmd)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        struct var *sh = var_find("SHELL");
        const char *shell = sh && sh->value[0] ? sh->value : "/bin/sh";
        execl(shell, "sh", "-c", cmd, (char *)0);
        fprintf(stderr, "%s: %s: %s\n", progname, shell, strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return st;
}

static int run_recipe(struct file *f)
{
    for (int i = 0; i < f->ncmds; i++) {
        char *cmd = expand(f->cmds[i]);
        char *c = cmd;
        int silent = opt_s, ignore = opt_i, force = 0;
        for (;; c++) {
            if (*c == '@')
                silent = 1;
            else if (*c == '-')
                ignore = 1;
            else if (*c == '+')
                force = 1;
            else if (*c != ' ' && *c != '\t')
                break;
        }
        if (!*c) {
            free(cmd);
            continue;
        }
        if (!silent || opt_n)
            printf("%s\n", c);
        if (opt_n && !force) {
            free(cmd);
            continue;
        }
        int st = run_command(c);
        free(cmd);
        if (st != 0) {
            int code = st < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            if (ignore) {
                fprintf(stderr, "%s: [%s:%d: %s] Error %d (ignored)\n", progname, f->cmd_file ? f->cmd_file : "",
                        f->cmd_line + i + 1, f->name, code);
                continue;
            }
            fprintf(stderr, "%s: *** [%s:%d: %s] Error %d\n", progname, f->cmd_file ? f->cmd_file : "",
                    f->cmd_line + i + 1, f->name, code);
            if (!f->phony && !f->precious) {
                struct stat st2;
                if (stat(f->name, &st2) == 0 && S_ISREG(st2.st_mode) && file_mtime(f) != stat_mtime(&st2)) {
                    fprintf(stderr, "%s: *** Deleting file '%s'\n", progname, f->name);
                    unlink(f->name);
                }
            }
            return -1;
        }
    }
    return 0;
}

/* Bring f up to date. Returns 0 ok, -1 failed. */
static int build(struct file *f, int depth, const char *parent)
{
    if (f->state == 2)
        return f->failed ? -1 : 0;
    if (f->state == 1) {
        fprintf(stderr, "%s: Circular %s <- %s dependency dropped.\n", progname, parent ? parent : "?",
                f->name);
        return 0;
    }
    f->state = 1;
    if (!f->ncmds && !f->phony)
        find_pattern_rule(f, 0);
    long long my = f->phony ? -1 : file_mtime(f);
    int need = opt_B || my < 0 || f->phony;
    const char *why = need ? (f->phony ? "it is phony" : my < 0 ? "it does not exist" : "-B was given") : 0;
    char whybuf[600];
    int failed = 0;
    for (struct dep *d = f->deps; d; d = d->next) {
        if (build(d->f, depth + 1, f->name) < 0) {
            failed = 1;
            if (!opt_k)
                break;
            continue;
        }
        if (d->order_only)
            continue;
        long long dm = file_mtime(d->f);
        if (d->f->remade || (dm >= 0 && my >= 0 && dm > my)) {
            if (!need) {
                snprintf(whybuf, sizeof(whybuf), "'%s' is newer", d->f->name);
                why = whybuf;
            }
            need = 1;
        }
    }
    if (failed) {
        fprintf(stderr, "%s: Target '%s' not remade because of errors.\n", progname, f->name);
        f->state = 2;
        f->failed = 1;
        return -1;
    }
    if (!f->ncmds && !f->deps && !f->phony && my < 0) {
        f->state = 2;
        f->failed = 1;
        if (parent)
            fprintf(stderr, "%s: *** No rule to make target '%s', needed by '%s'.  Stop.\n", progname, f->name,
                    parent);
        else
            fprintf(stderr, "%s: *** No rule to make target '%s'.  Stop.\n", progname, f->name);
        if (!opt_k)
            exit(2);
        return -1;
    }
    if (need && opt_q) {
        exit(1);
    }
    if (need && f->ncmds) {
        if (opt_d)
            fprintf(stderr, "%s: remaking '%s': %s\n", progname, f->name, why ? why : "");
        collect_auto(f, my);
        recipes_run++;
        int r = run_recipe(f);
        free_auto();
        f->mtime_known = 0;
        f->remade = 1;
        if (r < 0) {
            errors++;
            f->state = 2;
            f->failed = 1;
            if (!opt_k)
                exit(2);
            return -1;
        }
    } else if (need && (f->phony || my < 0)) {
        /* nothing to do, but parents must be remade */
        f->remade = 1;
    }
    f->state = 2;
    return 0;
}

static void print_database(void)
{
    printf("# Variables\n");
    for (int h = 0; h < NHASH; h++)
        for (struct var *v = vars[h]; v; v = v->next)
            if (v->origin != O_ENV && v->origin != O_AUTO)
                printf("%s %s %s\n", v->name, v->recursive ? "=" : ":=", v->value);
    printf("\n# Files\n");
    for (int h = 0; h < NHASH; h++)
        for (struct file *f = files[h]; f; f = f->next) {
            if (!f->deps && !f->ncmds)
                continue;
            printf("%s:", f->name);
            for (struct dep *d = f->deps; d; d = d->next)
                printf(" %s%s", d->order_only ? "| " : "", d->f->name);
            printf("\n");
            for (int i = 0; i < f->ncmds; i++)
                printf("\t%s\n", f->cmds[i]);
        }
    printf("\n# Pattern rules\n");
    for (struct prule *p = prules; p; p = p->next) {
        for (int i = 0; p->targets[i]; i++)
            printf("%s%s", i ? " " : "", p->targets[i]);
        printf(":");
        for (int i = 0; p->deps[i]; i++)
            printf(" %s", p->deps[i]);
        printf("\n");
        for (int i = 0; i < p->ncmds; i++)
            printf("\t%s\n", p->cmds[i]);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: make [-f makefile] [-C dir] [-nsikBpqd] [VAR=value ...] [target ...]\n");
    exit(2);
}

extern char **environ;

int main(int argc, char **argv)
{
    const char *makefiles[8];
    int nmf = 0;
    const char *goals[64];
    int ngoals = 0;
    char cmdline_vars[2048] = "";

    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(eq - *e), *e);
        if (strcmp(name, "SHELL") && strcmp(name, "MAKEFLAGS"))
            var_set(name, eq + 1, 0, O_ENV);
    }
    builtins();

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '-' && a[1] && strchr(a, '=') == 0) {
            if (!strcmp(a, "--version")) {
                printf("make (Eunuchs) 1.0\n");
                return 0;
            }
            if (!strcmp(a, "--help"))
                usage();
            for (char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'f':
                case 'C': {
                    char opt = *p;
                    const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
                    if (!val)
                        usage();
                    if (opt == 'f') {
                        if (nmf < 8)
                            makefiles[nmf++] = val;
                    } else if (chdir(val) < 0) {
                        die("%s: %s", val, strerror(errno));
                    } else {
                        char cwd[512];
                        if (getcwd(cwd, sizeof(cwd)))
                            printf("%s: Entering directory '%s'\n", progname, cwd);
                    }
                    p = (char *)" " + 1;    /* the rest of this argument was the value */
                    p--;
                    goto next_arg;
                }
                case 'n': opt_n = 1; break;
                case 's': opt_s = 1; break;
                case 'i': opt_i = 1; break;
                case 'k': opt_k = 1; break;
                case 'B': opt_B = 1; break;
                case 'p': opt_p = 1; break;
                case 'q': opt_q = 1; break;
                case 'd': opt_d = 1; break;
                case 'j':
                    /* parallel jobs: accepted, builds stay sequential */
                    if (!p[1] && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]))
                        i++;
                    goto next_arg;
                default:
                    usage();
                }
            }
        next_arg:
            continue;
        }
        if (strchr(a, '=')) {
            /* VAR=value on the command line */
            char *copy = xstrdup(a);
            if (!assignment(copy, O_CMDLINE))
                die("bad assignment '%s'", a);
            free(copy);
            size_t n = strlen(cmdline_vars);
            snprintf(cmdline_vars + n, sizeof(cmdline_vars) - n, "%s%s", n ? " " : "", a);
            continue;
        }
        if (ngoals < 64)
            goals[ngoals++] = a;
    }
    char mflags[64];
    snprintf(mflags, sizeof(mflags), "%s%s%s%s%s", opt_n ? "n" : "", opt_s ? "s" : "", opt_i ? "i" : "",
             opt_k ? "k" : "", opt_B ? "B" : "");
    setenv("MAKEFLAGS", mflags, 1);
    var_set("MAKEFLAGS", mflags, 0, O_DEFAULT);
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)))
            var_set("CURDIR", cwd, 0, O_DEFAULT);
    }

    if (!nmf) {
        static const char *const defaults[] = { "GNUmakefile", "makefile", "Makefile", 0 };
        for (int i = 0; defaults[i]; i++) {
            struct stat st;
            if (stat(defaults[i], &st) == 0) {
                makefiles[nmf++] = defaults[i];
                break;
            }
        }
    }
    struct str mfl = { 0 };
    sadds(&mfl, "");
    for (int i = 0; i < nmf; i++) {
        if (i)
            saddc(&mfl, ' ');
        sadds(&mfl, makefiles[i]);
    }
    var_set("MAKEFILE_LIST", mfl.s, 0, O_DEFAULT);
    free(mfl.s);
    for (int i = 0; i < nmf; i++)
        read_makefile(makefiles[i], 1);

    if (opt_p) {
        print_database();
        if (!ngoals && !nmf)
            return 0;
    }
    struct var *dg = var_find(".DEFAULT_GOAL");
    if (!ngoals) {
        if (dg && dg->value[0]) {
            goals[ngoals++] = dg->value;
        } else if (default_goal) {
            goals[ngoals++] = default_goal->name;
        } else {
            if (!nmf)
                die("No targets specified and no makefile found");
            die("No targets");
        }
    }
    int status = 0;
    for (int i = 0; i < ngoals; i++) {
        struct file *f = file_get(goals[i]);
        int before = recipes_run;
        if (build(f, 0, 0) < 0) {
            status = 2;
            if (!opt_k)
                break;
            continue;
        }
        if (recipes_run == before && !opt_q) {
            if (!f->phony && (f->ncmds || f->deps) && file_mtime(f) >= 0)
                printf("%s: '%s' is up to date.\n", progname, f->name);
            else
                printf("%s: Nothing to be done for '%s'.\n", progname, f->name);
        }
    }
    return status;
}
