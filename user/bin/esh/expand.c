/*
 * esh: word expansion.
 *
 * Order, as in zsh: brace expansion, then (left to right) tilde, parameter,
 * command and arithmetic expansion; then splitting (only of unquoted
 * command substitution, unless shwordsplit), filename generation, and
 * quote removal. Unlike sh, an unquoted $var is not split or globbed, and
 * an unquoted expansion that comes out empty disappears.
 *
 * Each word is built as a field with two buffers: the value, and a pattern
 * where quoted metacharacters are escaped with a backslash, so a globbing
 * or matching step can tell "*" from '*'.
 */
#include "esh.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

int expand_error;

struct field {
    struct sb v, pat;
    int glob;       /* an unquoted glob character */
    int forced;     /* quoting seen: the word exists even if empty */
    int any;        /* a character was emitted */
};

struct ex {
    int flags;
    struct strv *out;
    struct field f;
    int at_zero;    /* "$@" expanded to nothing */
};

#define DQ 1        /* inside "..." */
#define HD 2        /* here-document body */

static void expand_part(struct ex *e, const char *s, int len, int mode);

/* ---- fields ---- */

static int is_glob_meta(int c)
{
    return c == '*' || c == '?' || c == '[' || c == ']' || c == '\\';
}

static void emit(struct ex *e, int c, int quoted)
{
    sb_putc(&e->f.v, c);
    if (quoted && is_glob_meta(c))
        sb_putc(&e->f.pat, '\\');
    else if (!quoted && (c == '*' || c == '?' || c == '['))
        e->f.glob = 1;
    sb_putc(&e->f.pat, c);
    e->f.any = 1;
}

static void emit_str(struct ex *e, const char *s, int quoted)
{
    while (*s)
        emit(e, (unsigned char)*s++, quoted);
}

static void field_reset(struct field *f)
{
    sb_reset(&f->v);
    sb_reset(&f->pat);
    f->glob = f->forced = f->any = 0;
}

static void end_field(struct ex *e)
{
    struct field *f = &e->f;
    if (!f->any && !f->forced) {
        field_reset(f);
        return;
    }
    if (e->flags & EX_PATTERN) {
        strv_push(e->out, sb_adup(&f->pat));
    } else if (f->glob && (e->flags & EX_GLOB) && opts[OPT_GLOB] && pattern_has_magic(sb_str(&f->pat))) {
        int before = e->out->n;
        char *pat = sb_str(&f->pat);
        int n = glob_expand(pat, e->out);
        if (n == 0) {
            if (opts[OPT_NOMATCH]) {
                if (!expand_error)
                    sh_error("no matches found: %s", sb_str(&f->v));
                expand_error = 1;
            } else {
                strv_push(e->out, sb_adup(&f->v));
            }
        }
        (void)before;
    } else {
        strv_push(e->out, sb_adup(&f->v));
    }
    field_reset(f);
}

/* Split a value on IFS into the current and following fields. */
static void emit_split(struct ex *e, const char *s)
{
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    for (; *s; s++) {
        if (*ifs && strchr(ifs, *s)) {
            if (is_space(*s)) {
                if (e->f.any)
                    end_field(e);
            } else {
                e->f.forced = 1;
                end_field(e);
            }
            continue;
        }
        emit(e, (unsigned char)*s, 1);
    }
}

/* ---- tilde ---- */

char *tilde_expand_name(const char *user)
{
    if (!*user) {
        const char *h = var_get("HOME");
        return astrdup(h ? h : "/");
    }
    if (!strcmp(user, "+")) {
        const char *p = var_get("PWD");
        return p ? astrdup(p) : 0;
    }
    if (!strcmp(user, "-")) {
        const char *p = var_get("OLDPWD");
        return p ? astrdup(p) : 0;
    }
    char buf[256];
    if (user_home(user, buf, sizeof(buf)) == 0)
        return astrdup(buf);
    return 0;
}

/* s[i] == '~' at a word start; returns the new index. */
static int expand_tilde(struct ex *e, const char *s, int i, int len)
{
    int j = i + 1;
    while (j < len && s[j] != '/' && s[j] != ':' && (is_name_char(s[j]) || s[j] == '+' || s[j] == '-' ||
                                                     s[j] == '.'))
        j++;
    if (j < len && s[j] != '/' && s[j] != ':') {
        emit(e, '~', 0);
        return i + 1;
    }
    char *name = astrndup(s + i + 1, j - i - 1);
    char *home = tilde_expand_name(name);
    if (!home) {
        if (*name) {
            if (!expand_error)
                sh_error("no such user or named directory: %s", name);
            expand_error = 1;
        }
        emit(e, '~', 0);
        return i + 1;
    }
    emit_str(e, home, 1);
    if (!*home)
        e->f.forced = 1;
    return j;
}

/* ---- command substitution ---- */

char *run_cmdsub(const char *src, int *status)
{
    int fds[2];
    if (pipe(fds) < 0) {
        sh_error("pipe: %s", strerror(errno));
        *status = 1;
        return astrdup("");
    }
    pid_t pid = sh_fork();
    if (pid < 0) {
        sh_error("fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        *status = 1;
        return astrdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        if (fds[1] != 1) {
            dup2(fds[1], 1);
            close(fds[1]);
        }
        sh.in_child = 1;
        sh.interactive = 0;
        opts[OPT_MONITOR] = 0;
        child_reset_signals();
        trap_set(0, 0);
        int st = run_string(src, 0);
        sh_exit(st);
    }
    close(fds[1]);
    struct sb b = { 0 };
    char buf[512];
    for (;;) {
        long n = read(fds[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        sb_putn(&b, buf, n);
    }
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    while (b.len && b.s[b.len - 1] == '\n')
        b.len--;
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

static void emit_cmdsub(struct ex *e, const char *src, int mode)
{
    int st;
    char *out = run_cmdsub(src, &st);
    sh.last_status = st;
    if (mode)
        emit_str(e, out, 1);
    else if (e->flags & EX_SPLIT)
        emit_split(e, out);
    else
        emit_str(e, out, 1);
}

/* ---- parameters ---- */

struct pval {
    char **v;       /* elements (arena) */
    int n;
    int array;
    int set;
};

static char *num(long long v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", v);
    return astrdup(buf);
}

static void pv_scalar(struct pval *pv, const char *s)
{
    pv->v = amalloc(2 * sizeof(char *));
    pv->v[0] = astrdup(s);
    pv->v[1] = 0;
    pv->n = 1;
    pv->array = 0;
    pv->set = 1;
}

static struct pval get_param(const char *name)
{
    struct pval pv = { 0 };
    if (!strcmp(name, "@") || !strcmp(name, "*")) {
        pv.v = amalloc((pos.n + 1) * sizeof(char *));
        for (int i = 0; i < pos.n; i++)
            pv.v[i] = pos.v[i];
        pv.v[pos.n] = 0;
        pv.n = pos.n;
        pv.array = 1;
        pv.set = 1;
        return pv;
    }
    if (!strcmp(name, "#")) {
        pv_scalar(&pv, num(pos.n));
        return pv;
    }
    if (!strcmp(name, "0")) {
        pv_scalar(&pv, sh.script_name ? sh.script_name : "esh");
        return pv;
    }
    if (is_digit(name[0])) {
        int k = atoi(name);
        if (k >= 1 && k <= pos.n)
            pv_scalar(&pv, pos.v[k - 1]);
        return pv;
    }
    if (!strcmp(name, "?")) {
        pv_scalar(&pv, num(sh.last_status));
        return pv;
    }
    if (!strcmp(name, "$")) {
        static pid_t top;
        if (!top)
            top = getpid();
        pv_scalar(&pv, num(top));
        return pv;
    }
    if (!strcmp(name, "!")) {
        pv_scalar(&pv, sh.last_bg ? num(sh.last_bg) : "");
        return pv;
    }
    if (!strcmp(name, "-")) {
        char f[16];
        int k = 0;
        if (opts[OPT_ERREXIT]) f[k++] = 'e';
        if (!opts[OPT_GLOB]) f[k++] = 'f';
        if (opts[OPT_INTERACTIVE]) f[k++] = 'i';
        if (opts[OPT_MONITOR]) f[k++] = 'm';
        if (opts[OPT_NOUNSET]) f[k++] = 'u';
        if (opts[OPT_VERBOSE]) f[k++] = 'v';
        if (opts[OPT_XTRACE]) f[k++] = 'x';
        f[k] = 0;
        pv_scalar(&pv, f);
        return pv;
    }
    struct var *v = var_lookup(name);
    if (!v || (v->flags & V_UNSET))
        return pv;
    if (v->flags & V_ARRAY) {
        pv.v = amalloc((v->narr + 1) * sizeof(char *));
        for (int i = 0; i < v->narr; i++)
            pv.v[i] = v->arr[i];
        pv.v[v->narr] = 0;
        pv.n = v->narr;
        pv.array = 1;
        pv.set = 1;
        return pv;
    }
    const char *val = var_get(name);
    if (val)
        pv_scalar(&pv, val);
    return pv;
}

static void emit_values(struct ex *e, struct pval *pv, int mode, int at_form)
{
    if (mode) {
        if (pv->array && at_form) {
            if (pv->n == 0)
                e->at_zero = 1;
            for (int i = 0; i < pv->n; i++) {
                if (i) {
                    e->f.forced = 1;
                    end_field(e);
                }
                emit_str(e, pv->v[i], 1);
                e->f.forced = 1;
            }
        } else {
            const char *ifs = var_get("IFS");
            char sep = ifs ? *ifs : ' ';
            for (int i = 0; i < pv->n; i++) {
                if (i && sep)
                    emit(e, sep, 1);
                emit_str(e, pv->v[i], 1);
            }
        }
        return;
    }
    for (int i = 0; i < pv->n; i++) {
        if (pv->array && i && !(e->flags & EX_PATTERN)) {
            if (e->flags & (EX_SPLIT | EX_GLOB))
                end_field(e);
            else
                emit(e, ' ', 1);
        }
        if (opts[OPT_SHWORDSPLIT] && (e->flags & EX_SPLIT))
            emit_split(e, pv->v[i]);
        else
            emit_str(e, pv->v[i], 1);
    }
}

/* Expand a word inside ${...} (default values, patterns, replacements). */
static char *sub_word(const char *raw, int flags)
{
    char *r = expand_str(raw, flags);
    return r ? r : astrdup("");
}

static void case_conv(char *s, int mode)
{
    int start = 1;
    for (; *s; s++) {
        if (mode == 'U')
            *s = to_upper(*s);
        else if (mode == 'L')
            *s = to_lower(*s);
        else if (mode == 'C') {
            *s = start ? to_upper(*s) : to_lower(*s);
            start = !is_alnum(*s);
        }
    }
}

static char *replace_pat(const char *s, const char *pat, const char *rep, int all, int anchor)
{
    struct sb b = { 0 };
    int n = strlen(s), i = 0, done = 0;
    char *buf = xmalloc(n + 1);
    while (i <= n) {
        int best = -1;
        if (!done && (anchor != '#' || i == 0)) {
            for (int k = n; k >= i; k--) {
                if (anchor == '%' && k != n)
                    break;
                memcpy(buf, s + i, k - i);
                buf[k - i] = 0;
                if (pmatch(pat, buf, 0)) {
                    best = k - i;
                    break;
                }
            }
        }
        if (best > 0 || (best == 0 && anchor)) {
            sb_puts(&b, rep);
            i += best;
            if (!all)
                done = 1;
            if (best == 0) {
                if (i < n)
                    sb_putc(&b, s[i]);
                i++;
            }
            continue;
        }
        if (i < n)
            sb_putc(&b, s[i]);
        i++;
    }
    free(buf);
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

/* ${...}: content is the text between the braces. */
static void expand_braced(struct ex *e, const char *c, int mode)
{
    const char *p = c;
    char flags[16] = { 0 };
    int nflags = 0;
    char *join = 0, *split = 0;
    if (*p == '(') {
        p++;
        while (*p && *p != ')') {
            char f = *p++;
            if ((f == 'j' || f == 's') && *p) {
                char d = *p++;
                const char *st = p;
                while (*p && *p != d)
                    p++;
                char *sep = astrndup(st, p - st);
                if (*p)
                    p++;
                if (f == 'j')
                    join = sep;
                else
                    split = sep;
                continue;
            }
            if (nflags < 15)
                flags[nflags++] = f;
        }
        if (*p == ')')
            p++;
    }
    int length = 0, test_set = 0;
    if (*p == '#' && p[1] && p[1] != '}' && !strchr(":-=+?%/", p[1])) {
        length = 1;
        p++;
    } else if (*p == '+' && p[1] && (is_name_start(p[1]) || is_digit(p[1]))) {
        test_set = 1;
        p++;
    }
    char name[64];
    int k = 0;
    struct pval pv;
    int nested = 0;
    if ((*p == '$' && p[1] && (p[1] == '(' || p[1] == '{' || is_name_start(p[1]))) || *p == '"') {
        /* zsh: a nested expansion stands in for the name: ${(s:,:)$(cmd)}, ${${x#a}%b} */
        int len = strlen(p), end;
        if (*p == '"') {
            end = 1;
            while (end < len && p[end] != '"') {
                if (p[end] == '\\')
                    end++;
                end++;
            }
            end = end < len ? end + 1 : len;
        } else if (p[1] == '(') {
            end = skip_cmdsub(p, 2, len);
        } else if (p[1] == '{') {
            end = skip_braces(p, 2, len);
        } else {
            end = 1;
            while (end < len && is_name_char(p[end]))
                end++;
        }
        if (end < 0)
            end = len;
        char *val = expand_str(astrndup(p, end), 0);
        if (!val) {
            expand_error = 1;
            return;
        }
        memset(&pv, 0, sizeof(pv));
        pv_scalar(&pv, val);
        p += end;
        strcpy(name, "");
        nested = 1;
    } else {
        if (is_name_start(*p)) {
            while (is_name_char(*p) && k < 63)
                name[k++] = *p++;
        } else if (is_digit(*p)) {
            while (is_digit(*p) && k < 63)
                name[k++] = *p++;
        } else if (*p && strchr("@*#?$!-", *p)) {
            name[k++] = *p++;
        }
        name[k] = 0;
        if (!k) {
            if (!expand_error)
                sh_error("bad substitution");
            expand_error = 1;
            return;
        }
        pv = get_param(name);
    }
    int at_form = !strcmp(name, "@");

    /* subscript */
    if (*p == '[') {
        int d = 0;
        const char *st = p + 1;
        for (; *p; p++) {
            if (*p == '[')
                d++;
            else if (*p == ']' && --d == 0)
                break;
        }
        char *sub = astrndup(st, p - st);
        if (*p == ']')
            p++;
        if (!strcmp(sub, "@") || !strcmp(sub, "*")) {
            at_form = sub[0] == '@';
            if (!pv.array && pv.set) {
                pv.array = 1;
            }
        } else {
            char *comma = strchr(sub, ',');
            long long a, b;
            char *sa = sub_word(comma ? astrndup(sub, comma - sub) : sub, 0);
            if (arith_eval(sa, &a) < 0) {
                expand_error = 1;
                return;
            }
            b = a;
            if (comma && arith_eval(sub_word(comma + 1, 0), &b) < 0) {
                expand_error = 1;
                return;
            }
            if (pv.array) {
                int n = pv.n;
                if (a < 0) a += n + 1;
                if (b < 0) b += n + 1;
                if (a < 1) a = 1;
                if (b > n) b = n;
                struct pval r = { 0 };
                r.v = amalloc((n + 1) * sizeof(char *));
                for (long long i = a; i <= b; i++)
                    r.v[r.n++] = pv.v[i - 1];
                r.v[r.n] = 0;
                r.array = comma != 0;
                r.set = r.n > 0;
                if (!comma && r.n == 0)
                    pv_scalar(&r, ""), r.set = 0;
                pv = r;
            } else if (pv.set) {
                const char *s = pv.v[0];
                int n = strlen(s);
                if (a < 0) a += n + 1;
                if (b < 0) b += n + 1;
                if (a < 1) a = 1;
                if (b > n) b = n;
                pv_scalar(&pv, b >= a ? astrndup(s + a - 1, b - a + 1) : "");
            }
        }
    }

    if (test_set) {
        pv_scalar(&pv, pv.set ? "1" : "0");
        emit_values(e, &pv, mode, 0);
        return;
    }
    if (length) {
        if (pv.array && !(mode && !at_form && 0)) {
            pv_scalar(&pv, num(pv.n));
        } else {
            pv_scalar(&pv, num(pv.set ? str_width(pv.v[0], 1 << 30) : 0));
        }
        emit_values(e, &pv, mode, 0);
        return;
    }

    /* operators */
    const char *op = p;
    int colon = 0;
    if (*op == ':' && op[1] && strchr("-=+?", op[1])) {
        colon = 1;
        op++;
    }
    int empty = !pv.set || (colon && (pv.n == 0 || (pv.n == 1 && !*pv.v[0])));
    if (*op == '-' || *op == '=' || *op == '+' || *op == '?') {
        const char *word = op + 1;
        switch (*op) {
        case '-':
            if (empty) {
                pv_scalar(&pv, sub_word(word, EX_TILDE));
                at_form = 0;
            }
            break;
        case '=':
            if (empty) {
                char *w = sub_word(word, EX_TILDE);
                if (nested) {
                    sh_error("not an identifier: can't assign in a nested expansion");
                    expand_error = 1;
                    return;
                }
                if (var_set(name, w, 0) < 0) {
                    expand_error = 1;
                    return;
                }
                pv_scalar(&pv, w);
            }
            break;
        case '+':
            if (empty) {
                pv.n = 0;
                pv.set = 0;
            } else {
                pv_scalar(&pv, sub_word(word, EX_TILDE));
            }
            break;
        case '?':
            if (empty) {
                char *w = sub_word(word, 0);
                sh_error("%s: %s", name, *w ? w : "parameter not set");
                expand_error = 1;
                if (!sh.interactive)
                    sh_exit(1);
                return;
            }
            break;
        }
    } else if (*op == '#' || *op == '%') {
        int longest = op[1] == *op;
        char *pat = sub_word(op + 1 + longest, EX_PATTERN);
        for (int i = 0; i < pv.n; i++) {
            const char *s = pv.v[i];
            if (*op == '#') {
                int m = pmatch_prefix(pat, s, longest);
                pv.v[i] = astrdup(m >= 0 ? s + m : s);
            } else {
                int m = pmatch_suffix(pat, s, longest);
                pv.v[i] = m >= 0 ? astrndup(s, strlen(s) - m) : astrdup(s);
            }
        }
    } else if (*op == '/') {
        int all = 0, anchor = 0;
        op++;
        if (*op == '/') { all = 1; op++; }
        else if (*op == '#' || *op == '%') anchor = *op++;
        const char *slash = op;
        int d = 0;
        for (; *slash; slash++) {
            if (*slash == '\\' && slash[1]) { slash++; continue; }
            if (*slash == '{') d++;
            if (*slash == '}') d--;
            if (*slash == '/' && !d) break;
        }
        char *pat = sub_word(astrndup(op, slash - op), EX_PATTERN);
        char *rep = *slash ? sub_word(slash + 1, 0) : "";
        for (int i = 0; i < pv.n; i++)
            pv.v[i] = replace_pat(pv.v[i], pat, rep, all, anchor);
    } else if (*op == ':' && op[1]) {
        /* ${x:offset[:length]} (zero-based, as in bash and zsh) */
        const char *st = op + 1;
        const char *c2 = strchr(st, ':');
        long long off, len = -1;
        if (arith_eval(sub_word(c2 ? astrndup(st, c2 - st) : st, 0), &off) < 0 ||
            (c2 && arith_eval(sub_word(c2 + 1, 0), &len) < 0)) {
            expand_error = 1;
            return;
        }
        if (pv.array) {
            int n = pv.n;
            if (off < 0) off += n;
            if (off < 0) off = 0;
            if (off > n) off = n;
            if (len < 0 || off + len > n) len = n - off;
            pv.v += off;
            pv.n = len;
        } else if (pv.set) {
            const char *s = pv.v[0];
            int n = strlen(s);
            if (off < 0) off += n;
            if (off < 0) off = 0;
            if (off > n) off = n;
            if (len < 0 || off + len > n) len = n - off;
            pv_scalar(&pv, astrndup(s + off, len));
        }
    } else if (*op) {
        if (!expand_error)
            sh_error("bad substitution");
        expand_error = 1;
        return;
    } else if (!pv.set && opts[OPT_NOUNSET] && strcmp(name, "@") && strcmp(name, "*")) {
        sh_error("%s: parameter not set", name);
        expand_error = 1;
        return;
    }

    /* flags */
    for (int i = 0; i < nflags; i++)
        if (flags[i] == 'f')
            split = "\n";
    if (split && pv.n == 1) {
        struct strv parts = { 0 };
        const char *s = pv.v[0];
        int sl = strlen(split);
        while (*s) {
            const char *f = sl ? strstr(s, split) : 0;
            if (!f) {
                strv_push(&parts, astrdup(s));
                break;
            }
            if (f > s)
                strv_push(&parts, astrndup(s, f - s));
            s = f + sl;
        }
        pv.v = parts.v ? parts.v : amalloc(sizeof(char *));
        pv.n = parts.n;
        pv.array = 1;
        at_form = 1;
    }
    for (int i = 0; i < nflags; i++) {
        char f = flags[i];
        if (f == 'U' || f == 'L' || f == 'C') {
            for (int k2 = 0; k2 < pv.n; k2++) {
                pv.v[k2] = astrdup(pv.v[k2]);
                case_conv(pv.v[k2], f);
            }
        } else if (f == 'o' || f == 'O') {
            char **copy = amalloc((pv.n + 1) * sizeof(char *));
            memcpy(copy, pv.v, pv.n * sizeof(char *));
            copy[pv.n] = 0;
            sort_strs(copy, pv.n);
            if (f == 'O')
                for (int a = 0, b = pv.n - 1; a < b; a++, b--) {
                    char *t = copy[a];
                    copy[a] = copy[b];
                    copy[b] = t;
                }
            pv.v = copy;
        } else if (f == 'u') {
            char **copy = amalloc((pv.n + 1) * sizeof(char *));
            int m = 0;
            for (int a = 0; a < pv.n; a++) {
                int dup = 0;
                for (int b = 0; b < m && !dup; b++)
                    dup = !strcmp(copy[b], pv.v[a]);
                if (!dup)
                    copy[m++] = pv.v[a];
            }
            copy[m] = 0;
            pv.v = copy;
            pv.n = m;
        } else if (f == '@') {
            at_form = 1;
        } else if (f == 'q') {
            for (int k2 = 0; k2 < pv.n; k2++)
                pv.v[k2] = quote_word(pv.v[k2]);
        }
    }
    if (join) {
        struct sb b = { 0 };
        for (int i = 0; i < pv.n; i++) {
            if (i)
                sb_puts(&b, join);
            sb_puts(&b, pv.v[i]);
        }
        pv_scalar(&pv, sb_str(&b));
        sb_free(&b);
        at_form = 0;
    }
    emit_values(e, &pv, mode, at_form);
}

/* s[i] == '$'; returns the index after the expansion. */
static int expand_dollar(struct ex *e, const char *s, int i, int len, int mode)
{
    if (i + 1 >= len) {
        emit(e, '$', mode);
        return i + 1;
    }
    char c = s[i + 1];
    if (c == '(') {
        int j = skip_cmdsub(s, i + 2, len);
        if (j < 0)
            j = len;
        if (i + 2 < len && s[i + 2] == '(' && j - 2 > i + 2 && s[j - 2] == ')') {
            char *ex = astrndup(s + i + 3, j - 2 - (i + 3));
            char *val = expand_str(ex, EX_NOBRACE);
            long long v = 0;
            if (!val || arith_eval(val, &v) < 0) {
                expand_error = 1;
                return j;
            }
            emit_str(e, num(v), 1);
            return j;
        }
        emit_cmdsub(e, astrndup(s + i + 2, j - 1 - (i + 2)), mode);
        return j;
    }
    if (c == '{') {
        int j = skip_braces(s, i + 2, len);
        if (j < 0)
            j = len;
        expand_braced(e, astrndup(s + i + 2, j - 1 - (i + 2)), mode);
        return j;
    }
    char name[64];
    int k = 0, j = i + 1;
    if (c == '#' && j + 1 < len && is_name_start(s[j + 1])) {
        /* $#name: length */
        j++;
        while (j < len && is_name_char(s[j]) && k < 63)
            name[k++] = s[j++];
        name[k] = 0;
        struct pval pv = get_param(name);
        long long n = pv.array ? pv.n : pv.set ? str_width(pv.v[0], 1 << 30) : 0;
        emit_str(e, num(n), 1);
        return j;
    }
    if (is_name_start(c)) {
        while (j < len && is_name_char(s[j]) && k < 63)
            name[k++] = s[j++];
    } else if (is_digit(c) || strchr("@*#?$!-", c)) {
        name[k++] = c;
        j++;
    } else {
        emit(e, '$', mode);
        return i + 1;
    }
    name[k] = 0;
    if (j < len && s[j] == '[' && is_name_start(name[0])) {
        /* zsh: $name[subscript] works without braces */
        int d = 0, q = j;
        for (; q < len; q++) {
            if (s[q] == '[')
                d++;
            else if (s[q] == ']' && --d == 0)
                break;
        }
        if (q < len) {
            struct sb b = { 0 };
            sb_puts(&b, name);
            sb_putn(&b, s + j, q + 1 - j);
            expand_braced(e, sb_str(&b), mode);
            sb_free(&b);
            return q + 1;
        }
    }
    struct pval pv = get_param(name);
    if (!pv.set && opts[OPT_NOUNSET] && strcmp(name, "@") && strcmp(name, "*")) {
        sh_error("%s: parameter not set", name);
        expand_error = 1;
        return j;
    }
    emit_values(e, &pv, mode, !strcmp(name, "@"));
    return j;
}

static int hexval(int c)
{
    if (is_digit(c))
        return c - '0';
    c = to_lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void put_utf8(struct ex *e, unsigned cp)
{
    if (cp < 0x80) {
        emit(e, cp, 1);
    } else if (cp < 0x800) {
        emit(e, 0xc0 | (cp >> 6), 1);
        emit(e, 0x80 | (cp & 0x3f), 1);
    } else if (cp < 0x10000) {
        emit(e, 0xe0 | (cp >> 12), 1);
        emit(e, 0x80 | ((cp >> 6) & 0x3f), 1);
        emit(e, 0x80 | (cp & 0x3f), 1);
    } else {
        emit(e, 0xf0 | (cp >> 18), 1);
        emit(e, 0x80 | ((cp >> 12) & 0x3f), 1);
        emit(e, 0x80 | ((cp >> 6) & 0x3f), 1);
        emit(e, 0x80 | (cp & 0x3f), 1);
    }
}

/* $'...' starting after the quote; returns the index after the closing quote. */
static int expand_csquote(struct ex *e, const char *s, int i, int len)
{
    e->f.forced = 1;
    while (i < len && s[i] != '\'') {
        if (s[i] != '\\' || i + 1 >= len) {
            emit(e, (unsigned char)s[i++], 1);
            continue;
        }
        char c = s[++i];
        i++;
        switch (c) {
        case 'a': emit(e, 7, 1); break;
        case 'b': emit(e, 8, 1); break;
        case 'e': case 'E': emit(e, 27, 1); break;
        case 'f': emit(e, 12, 1); break;
        case 'n': emit(e, '\n', 1); break;
        case 'r': emit(e, '\r', 1); break;
        case 't': emit(e, '\t', 1); break;
        case 'v': emit(e, 11, 1); break;
        case 'c':
            if (i < len)
                emit(e, s[i++] & 31, 1);
            break;
        case 'x': case 'u': case 'U': {
            int max = c == 'x' ? 2 : c == 'u' ? 4 : 8, v = 0, k = 0;
            while (k < max && i < len && hexval(s[i]) >= 0)
                v = v * 16 + hexval(s[i++]), k++;
            if (c == 'x')
                emit(e, v, 1);
            else
                put_utf8(e, v);
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int v = c - '0', k = 1;
            while (k < 3 && i < len && s[i] >= '0' && s[i] <= '7')
                v = v * 8 + s[i++] - '0', k++;
            emit(e, v & 0xff, 1);
            break;
        }
        default:
            emit(e, c, 1);
        }
    }
    return i < len ? i + 1 : len;
}

/* Text between backquotes: \$ \` \\ lose the backslash. */
static char *bquote_text(const char *s, int len)
{
    char *d = amalloc(len + 1), *o = d;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len && strchr("$`\\", s[i + 1]))
            i++;
        *o++ = s[i];
    }
    *o = 0;
    return d;
}

/* mode: 0 unquoted, DQ inside "...", HD here-document body */
static void expand_part(struct ex *e, const char *s, int len, int mode)
{
    int i = 0;
    while (i < len && !expand_error) {
        char c = s[i];
        if (!mode) {
            if (c == '\'') {
                int j = i + 1;
                while (j < len && s[j] != '\'')
                    j++;
                for (int k = i + 1; k < j; k++)
                    emit(e, (unsigned char)s[k], 1);
                e->f.forced = 1;
                i = j + 1;
                continue;
            }
            if (c == '"') {
                int j = i + 1, depth_ok = 1;
                /* find the closing quote (same rules as the lexer) */
                while (j < len && s[j] != '"') {
                    if (s[j] == '\\' && j + 1 < len)
                        j += 2;
                    else if (s[j] == '$' && j + 1 < len && s[j + 1] == '(') {
                        int k = skip_cmdsub(s, j + 2, len);
                        j = k < 0 ? len : k;
                    } else if (s[j] == '$' && j + 1 < len && s[j + 1] == '{') {
                        int k = skip_braces(s, j + 2, len);
                        j = k < 0 ? len : k;
                    } else if (s[j] == '`') {
                        int k = j + 1;
                        while (k < len && s[k] != '`') {
                            if (s[k] == '\\')
                                k++;
                            k++;
                        }
                        j = k + 1;
                    } else
                        j++;
                }
                (void)depth_ok;
                int had = e->f.any;
                e->at_zero = 0;
                expand_part(e, s + i + 1, (j < len ? j : len) - i - 1, DQ);
                if (!(e->at_zero && !e->f.any && !had))
                    e->f.forced = 1;
                e->at_zero = 0;
                i = j + 1;
                continue;
            }
            if (c == '\\') {
                if (i + 1 < len) {
                    if (s[i + 1] != '\n')
                        emit(e, (unsigned char)s[i + 1], 1);
                    e->f.forced |= s[i + 1] != '\n';
                    i += 2;
                } else {
                    i++;
                }
                continue;
            }
            if (c == '$' && i + 1 < len && s[i + 1] == '\'') {
                i = expand_csquote(e, s, i + 2, len);
                continue;
            }
            if (c == '~' && (e->flags & EX_TILDE) &&
                (i == 0 || ((e->flags & EX_ASSIGN) && (s[i - 1] == ':' || s[i - 1] == '=')))) {
                i = expand_tilde(e, s, i, len);
                continue;
            }
        } else if (c == '\\') {
            const char *special = mode == DQ ? "$`\"\\\n" : "$`\\\n";
            if (i + 1 < len && strchr(special, s[i + 1])) {
                if (s[i + 1] != '\n')
                    emit(e, (unsigned char)s[i + 1], 1);
                i += 2;
            } else {
                emit(e, '\\', 1);
                i++;
            }
            continue;
        }
        if (c == '$') {
            i = expand_dollar(e, s, i, len, mode);
            continue;
        }
        if (c == '`') {
            int j = i + 1;
            while (j < len && s[j] != '`') {
                if (s[j] == '\\')
                    j++;
                j++;
            }
            emit_cmdsub(e, bquote_text(s + i + 1, (j < len ? j : len) - i - 1), mode);
            i = j + 1;
            continue;
        }
        emit(e, (unsigned char)c, mode != 0);
        i++;
    }
}

/* ---- brace expansion ---- */

/* Find a top-level {..} starting search at i; returns 1 with the bounds. */
static int find_brace(const char *w, int *open, int *close)
{
    int len = strlen(w);
    for (int i = 0; i < len; i++) {
        char c = w[i];
        if (c == '\\') { i++; continue; }
        if (c == '\'') { while (++i < len && w[i] != '\''); continue; }
        if (c == '"') {
            while (++i < len && w[i] != '"')
                if (w[i] == '\\') i++;
            continue;
        }
        if (c == '$' && i + 1 < len && (w[i + 1] == '{' || w[i + 1] == '(')) {
            int k = w[i + 1] == '{' ? skip_braces(w, i + 2, len) : skip_cmdsub(w, i + 2, len);
            if (k < 0)
                return 0;
            i = k - 1;
            continue;
        }
        if (c != '{')
            continue;
        int depth = 0, comma = 0, j;
        for (j = i; j < len; j++) {
            char d = w[j];
            if (d == '\\') { j++; continue; }
            if (d == '\'') { while (++j < len && w[j] != '\''); continue; }
            if (d == '"') {
                while (++j < len && w[j] != '"')
                    if (w[j] == '\\') j++;
                continue;
            }
            if (d == '$' && j + 1 < len && (w[j + 1] == '{' || w[j + 1] == '(')) {
                int k = w[j + 1] == '{' ? skip_braces(w, j + 2, len) : skip_cmdsub(w, j + 2, len);
                if (k < 0)
                    break;
                j = k - 1;
                continue;
            }
            if (d == '{')
                depth++;
            else if (d == '}' && --depth == 0)
                break;
            else if (d == ',' && depth == 1)
                comma = 1;
        }
        if (j >= len)
            return 0;
        if (comma || strstr(astrndup(w + i, j - i), "..")) {
            *open = i;
            *close = j;
            return 1;
        }
    }
    return 0;
}

static int seq_parse(const char *s, long long *v, int *is_char, int *width)
{
    if (s[0] && !s[1] && !is_digit(s[0])) {
        *v = (unsigned char)s[0];
        *is_char = 1;
        return 1;
    }
    if (!is_number(s))
        return 0;
    *v = strtol(s, 0, 10);
    *is_char = 0;
    const char *d = s[0] == '-' ? s + 1 : s;
    if (d[0] == '0' && d[1])
        *width = strlen(s);
    return 1;
}

static int brace_rec(const char *w, struct strv *out, int depth)
{
    int o, c;
    if (depth > 20 || out->n > 10000 || !find_brace(w, &o, &c)) {
        strv_push(out, astrdup(w));
        return 0;
    }
    char *pre = astrndup(w, o), *body = astrndup(w + o + 1, c - o - 1), *post = astrdup(w + c + 1);
    /* sequence? */
    char *dots = strstr(body, "..");
    int has_comma = 0, d = 0;
    for (char *q = body; *q; q++) {
        if (*q == '{') d++;
        else if (*q == '}') d--;
        else if (*q == ',' && !d) has_comma = 1;
    }
    if (!has_comma && dots) {
        char *a = astrndup(body, dots - body), *rest = dots + 2, *b = rest, *st = 0;
        char *dots2 = strstr(rest, "..");
        if (dots2) {
            b = astrndup(rest, dots2 - rest);
            st = dots2 + 2;
        }
        long long x, y, step = 1;
        int ca, cb, wa = 0, wb = 0;
        if (seq_parse(a, &x, &ca, &wa) && seq_parse(b, &y, &cb, &wb) && ca == cb &&
            (!st || is_number(st))) {
            if (st)
                step = strtol(st, 0, 10);
            if (step < 0)
                step = -step;
            if (!step)
                step = 1;
            int width = wa > wb ? wa : wb;
            for (long long v = x; x <= y ? v <= y : v >= y; v += x <= y ? step : -step) {
                char buf[32];
                if (ca)
                    snprintf(buf, sizeof(buf), "%c", (char)v);
                else
                    snprintf(buf, sizeof(buf), "%0*lld", width, v);
                struct sb b2 = { 0 };
                sb_puts(&b2, pre);
                sb_puts(&b2, buf);
                sb_puts(&b2, post);
                brace_rec(sb_str(&b2), out, depth + 1);
                sb_free(&b2);
                if (out->n > 10000)
                    break;
            }
            return 0;
        }
        strv_push(out, astrdup(w));
        return 0;
    }
    /* alternatives */
    d = 0;
    char *st = body;
    for (char *q = body;; q++) {
        if (*q == '\\' && q[1]) {
            q++;
            continue;
        }
        if (*q == '{') d++;
        else if (*q == '}') d--;
        if (!*q || (*q == ',' && !d)) {
            struct sb b2 = { 0 };
            sb_puts(&b2, pre);
            sb_putn(&b2, st, q - st);
            sb_puts(&b2, post);
            brace_rec(sb_str(&b2), out, depth + 1);
            sb_free(&b2);
            if (!*q)
                break;
            st = q + 1;
        }
    }
    return 0;
}

int brace_expand(const char *w, struct strv *out)
{
    return brace_rec(w, out, 0);
}

/* ---- globbing ---- */

static int glob_hidden_ok(const char *comp)
{
    return comp[0] == '.' || opts[OPT_GLOBDOTS];
}

static char *join_path(const char *dir, const char *name)
{
    if (!*dir)
        return astrdup(name);
    struct sb b = { 0 };
    sb_puts(&b, dir);
    if (dir[strlen(dir) - 1] != '/')
        sb_putc(&b, '/');
    sb_puts(&b, name);
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void glob_rec(const char *dir, char **comps, int nc, int ci, struct strv *out, int depth)
{
    if (out->n > 20000 || depth > 32)
        return;
    if (ci == nc) {
        strv_push(out, astrdup(*dir ? dir : "."));
        return;
    }
    const char *comp = comps[ci];
    if (!*comp) {
        /* trailing slash: directories only */
        if (is_dir(*dir ? dir : "."))
            strv_push(out, join_path(dir, ""));
        return;
    }
    if (!strcmp(comp, "**") && ci + 1 < nc) {
        /* zero or more directories */
        glob_rec(dir, comps, nc, ci + 1, out, depth + 1);
        DIR *d = opendir(*dir ? dir : ".");
        if (!d)
            return;
        struct strv subs = { 0 };
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (de->d_name[0] == '.' && !opts[OPT_GLOBDOTS])
                continue;
            char *p = join_path(dir, de->d_name);
            if (is_dir(p))
                strv_push(&subs, p);
        }
        closedir(d);
        sort_strs(subs.v, subs.n);
        for (int i = 0; i < subs.n; i++)
            glob_rec(subs.v[i], comps, nc, ci, out, depth + 1);
        return;
    }
    if (!pattern_has_magic(comp)) {
        char *lit = unescape_pattern(comp);
        char *p = join_path(dir, lit);
        struct stat st;
        if (ci + 1 == nc) {
            if (stat(p, &st) == 0)
                strv_push(out, p);
        } else if (is_dir(p)) {
            glob_rec(p, comps, nc, ci + 1, out, depth);
        }
        return;
    }
    DIR *d = opendir(*dir ? dir : ".");
    if (!d)
        return;
    struct strv m = { 0 };
    struct dirent *de;
    int fold = opts[OPT_NOCASEGLOB] ? PM_CASEFOLD : 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && !glob_hidden_ok(comp))
            continue;
        if (pmatch(comp, de->d_name, fold))
            strv_push(&m, astrdup(de->d_name));
    }
    closedir(d);
    sort_strs(m.v, m.n);
    for (int i = 0; i < m.n; i++) {
        char *p = join_path(dir, m.v[i]);
        if (ci + 1 == nc)
            strv_push(out, p);
        else if (is_dir(p))
            glob_rec(p, comps, nc, ci + 1, out, depth);
    }
}

int glob_expand(const char *pattern, struct strv *out)
{
    int before = out->n;
    char *pat = astrdup(pattern);
    char *comps[64];
    int nc = 0;
    const char *dir = "";
    char *p = pat;
    if (*p == '/') {
        dir = "/";
        while (*p == '/')
            p++;
    }
    char *st = p;
    for (;; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '/' || !*p) {
            int end = !*p;
            *p = 0;
            if (nc < 64 && (*st || end))
                comps[nc++] = st;
            if (end)
                break;
            while (p[1] == '/')
                p++;
            st = p + 1;
            if (!p[1]) {
                if (nc < 64)
                    comps[nc++] = "";      /* trailing slash */
                break;
            }
        }
    }
    glob_rec(dir, comps, nc, 0, out, 0);
    return out->n - before;
}

/* ---- entry points ---- */

static void ex_init(struct ex *e, struct strv *out, int flags)
{
    memset(e, 0, sizeof(*e));
    e->flags = flags;
    e->out = out;
}

static void ex_free(struct ex *e)
{
    sb_free(&e->f.v);
    sb_free(&e->f.pat);
}

int expand_words(char **raw, int n, struct strv *out, int flags)
{
    expand_error = 0;
    struct ex e;
    ex_init(&e, out, flags);
    for (int i = 0; i < n && !expand_error; i++) {
        struct strv words = { 0 };
        if (flags & EX_NOBRACE)
            strv_push(&words, raw[i]);
        else
            brace_expand(raw[i], &words);
        for (int k = 0; k < words.n && !expand_error; k++) {
            expand_part(&e, words.v[k], strlen(words.v[k]), 0);
            end_field(&e);
        }
    }
    ex_free(&e);
    if (!out->v)
        strv_push(out, 0), out->n = 0;
    return expand_error ? -1 : 0;
}

char *expand_str(const char *raw, int flags)
{
    int saved = expand_error;
    expand_error = 0;
    struct strv out = { 0 };
    struct ex e;
    ex_init(&e, &out, flags & ~(EX_SPLIT | EX_GLOB));
    expand_part(&e, raw, strlen(raw), 0);
    e.f.forced = 1;
    end_field(&e);
    ex_free(&e);
    if (expand_error)
        return 0;
    expand_error = saved;
    if (out.n == 1)
        return out.v[0];
    struct sb b = { 0 };
    for (int i = 0; i < out.n; i++) {
        if (i)
            sb_putc(&b, ' ');
        sb_puts(&b, out.v[i]);
    }
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

char *expand_heredoc(const char *body)
{
    expand_error = 0;
    struct strv out = { 0 };
    struct ex e;
    ex_init(&e, &out, 0);
    expand_part(&e, body, strlen(body), HD);
    e.f.forced = 1;
    end_field(&e);
    ex_free(&e);
    if (expand_error)
        return 0;
    struct sb b = { 0 };
    for (int i = 0; i < out.n; i++) {
        if (i)
            sb_putc(&b, ' ');
        sb_puts(&b, out.v[i]);
    }
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

char *quote_word(const char *s)
{
    int plain = *s != 0;
    for (const char *p = s; *p; p++)
        if (!is_alnum(*p) && !strchr("_-./,:=+@%^", *p))
            plain = 0;
    if (plain)
        return astrdup(s);
    struct sb b = { 0 };
    sb_putc(&b, '\'');
    for (; *s; s++) {
        if (*s == '\'')
            sb_puts(&b, "'\\''");
        else
            sb_putc(&b, *s);
    }
    sb_putc(&b, '\'');
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}

char *escape_for_completion(const char *s)
{
    struct sb b = { 0 };
    for (const char *p = s; *p; p++) {
        if (strchr(" \t\n'\"\\$`&|;<>()*?[]{}!#", *p) || (*p == '~' && p == s))
            sb_putc(&b, '\\');
        sb_putc(&b, *p);
    }
    char *r = sb_adup(&b);
    sb_free(&b);
    return r;
}
