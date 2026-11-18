/*
 * sed: the stream editor, GNU flavoured.
 *
 *   sed [-nEsuz] [-i[SUFFIX]] [-e SCRIPT]... [-f FILE]... [SCRIPT] [FILE...]
 *
 * Addresses: N, $, /RE/ (and \cREc), with I for case-blind; FIRST~STEP;
 * ADDR1,ADDR2, ADDR1,+N, ADDR1,~N, 0,/RE/; ! negates.
 * Commands: { } = a i c (both `a text` and `a\` + lines) b t T : d D g G h
 * H l n N p P q Q r w s y z #.
 * s///FLAGS: g, p, N (the Nth match), i/I, w FILE; the replacement knows
 * & \1..\9 \n and GNU's \U \L \u \l \E.
 * -i edits each file in place (with SUFFIX: keeps a backup); -i and -s make
 * each file its own stream (line numbers and $).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- growable strings ---- */

struct buf {
    char *s;
    size_t n, cap;
};

static void b_reserve(struct buf *b, size_t n)
{
    if (b->n + n + 1 <= b->cap)
        return;
    while (b->n + n + 1 > b->cap)
        b->cap = b->cap ? b->cap * 2 : 128;
    if (!(b->s = realloc(b->s, b->cap))) {
        eprintf("sed: out of memory\n");
        exit(4);
    }
}

static void b_add(struct buf *b, const char *s, size_t n)
{
    b_reserve(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void b_addc(struct buf *b, int c)
{
    char ch = c;
    b_add(b, &ch, 1);
}

static void b_set(struct buf *b, const char *s, size_t n)
{
    b->n = 0;
    b_add(b, s, n);
}

/* ---- the script ---- */

enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_ZERO };

struct addr {
    int type;
    long n, step;                       /* A_LINE: n; A_STEP: n~step */
    regex_t *re;                        /* NULL with A_RE: the last regex used */
};

struct wfile {
    char *name;
    FILE *f;
    struct wfile *next;
};

struct cmd {
    struct addr a1, a2;
    int a2_rel;                         /* 0, '+' (ADDR1,+N) or '~' (ADDR1,~N) */
    int negate;
    char c;
    int active;                         /* inside the range */
    long end_line;                      /* ADDR1,+N: last line of the range */
    /* arguments */
    char *text;                         /* a i c: text; b t T :: label; r: file */
    long target;                        /* b t T: command index ({: matching }) */
    regex_t *re;                        /* s */
    char *repl;
    int global, occurrence, print, icase;
    struct wfile *wf;                   /* s///w, w */
    unsigned char map[256];             /* y */
    int qcode;                          /* q Q */
};

static struct cmd *cmds;
static int ncmds, capcmds;
static int extended, quiet, separate, inplace, nulldata;
static const char *suffix = "";
static struct wfile *wfiles;
static regex_t *last_re;

static void die(const char *fmt, const char *arg)
{
    eprintf("sed: ");
    eprintf(fmt, arg);
    eprintf("\n");
    exit(1);
}

static struct cmd *new_cmd(void)
{
    if (ncmds == capcmds) {
        capcmds = capcmds ? capcmds * 2 : 32;
        cmds = realloc(cmds, capcmds * sizeof(*cmds));
    }
    struct cmd *c = &cmds[ncmds++];
    memset(c, 0, sizeof(*c));
    c->target = -1;
    return c;
}

static struct wfile *open_wfile(const char *name)
{
    for (struct wfile *w = wfiles; w; w = w->next)
        if (!strcmp(w->name, name))
            return w;
    struct wfile *w = calloc(1, sizeof(*w));
    w->name = strdup(name);
    if (!strcmp(name, "/dev/stdout"))
        w->f = stdout;
    else if (!strcmp(name, "/dev/stderr"))
        w->f = stderr;
    else if (!(w->f = fopen(name, "w")))
        die("cannot open %s for writing", name);
    w->next = wfiles;
    wfiles = w;
    return w;
}

static const char *p;                   /* parse position */

static void skip_ws(void)
{
    while (*p == ' ' || *p == '\t')
        p++;
}

/* Text up to DELIM (unescaping \DELIM and \n); for regexes, keeps other
   escapes. Advances past the closing delimiter. */
static char *delimited(int delim, int is_re)
{
    struct buf b = { 0 };
    b_reserve(&b, 0);
    b.s[0] = 0;
    int in_bracket = 0;
    for (;;) {
        if (!*p)
            die("unterminated `%s' command", delim == '/' ? "s" : "address");
        if (*p == '\\' && p[1]) {
            if (p[1] == delim && !in_bracket)
                b_addc(&b, delim);
            else if (p[1] == 'n')
                b_addc(&b, '\n');
            else if (p[1] == 't' && is_re)
                b_addc(&b, '\t');
            else if (p[1] == '\n')
                b_addc(&b, '\n');
            else {
                b_addc(&b, '\\');
                b_addc(&b, p[1]);
            }
            p += 2;
            continue;
        }
        if (is_re && *p == '[' && !in_bracket) {
            in_bracket = 1;
            b_addc(&b, *p++);
            if (*p == '^')
                b_addc(&b, *p++);
            if (*p == ']')
                b_addc(&b, *p++);
            continue;
        }
        if (in_bracket && *p == ']')
            in_bracket = 0;
        if (*p == delim && !in_bracket) {
            p++;
            return b.s;
        }
        b_addc(&b, *p++);
    }
}

static regex_t *compile(const char *pat, int icase)
{
    if (!*pat)
        return 0;                       /* the empty regex: the last one used */
    regex_t *re = malloc(sizeof(*re));
    int r = regcomp(re, pat, (extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
    if (r) {
        char msg[128];
        regerror(r, re, msg, sizeof(msg));
        eprintf("sed: -e expression: %s: %s\n", pat, msg);
        exit(1);
    }
    return re;
}

static int parse_addr(struct addr *a)
{
    if (isdigit(*p)) {
        a->n = strtol(p, (char **)&p, 10);
        a->type = A_LINE;
        if (*p == '~') {
            p++;
            a->step = strtol(p, (char **)&p, 10);
            a->type = A_STEP;
        } else if (a->n == 0) {
            a->type = A_ZERO;
        }
        return 1;
    }
    if (*p == '$') {
        p++;
        a->type = A_LAST;
        return 1;
    }
    if (*p == '/' || *p == '\\') {
        int delim = *p == '\\' ? p[1] : '/';
        p += *p == '\\' ? 2 : 1;
        char *pat = delimited(delim, 1);
        int icase = 0;
        while (*p == 'I' || *p == 'M') {
            icase |= *p == 'I';
            p++;
        }
        a->type = A_RE;
        a->re = compile(pat, icase);
        free(pat);
        return 1;
    }
    return 0;
}

/* The rest of a line (for labels, file names): up to newline or ';' for labels. */
static char *rest_of_line(int stop_at_semicolon)
{
    skip_ws();
    const char *s = p;
    while (*p && *p != '\n' && !(stop_at_semicolon && (*p == ';' || *p == '}')))
        p++;
    const char *e = p;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    return strndup(s, e - s);
}

/* a i c text: GNU one-liner (`a text`) or POSIX (`a\` newline text, lines
   continued with a trailing backslash). */
static char *parse_text(void)
{
    struct buf b = { 0 };
    b_reserve(&b, 0);
    b.s[0] = 0;
    skip_ws();
    if (*p == '\\') {
        p++;
        skip_ws();
        if (*p == '\n')
            p++;
    }
    while (*p) {
        if (*p == '\\' && p[1]) {
            b_addc(&b, p[1] == 't' ? '\t' : p[1]);
            p += 2;
            continue;
        }
        if (*p == '\n') {
            p++;
            break;
        }
        b_addc(&b, *p++);
    }
    return b.s;
}

static void parse_replacement_flags(struct cmd *c)
{
    for (;;) {
        if (*p == 'g')
            c->global = 1, p++;
        else if (*p == 'p')
            c->print = 1, p++;
        else if (*p == 'i' || *p == 'I')
            c->icase = 1, p++;
        else if (*p == 'm' || *p == 'M' || *p == 'e')
            p++;
        else if (isdigit(*p))
            c->occurrence = strtol(p, (char **)&p, 10);
        else if (*p == 'w') {
            p++;
            char *name = rest_of_line(0);
            c->wf = open_wfile(name);
            free(name);
            return;
        } else
            return;
    }
}

static void parse(const char *script)
{
    p = script;
    int depth = 0;
    int stack[64];
    for (;;) {
        while (isspace(*p) || *p == ';')
            p++;
        if (!*p)
            break;
        if (*p == '#') {
            if (p == script && p[1] == 'n' && (p[2] == '\n' || !p[2]))
                quiet = 1;
            while (*p && *p != '\n')
                p++;
            continue;
        }
        struct cmd *c = new_cmd();
        if (parse_addr(&c->a1)) {
            skip_ws();
            if (*p == ',') {
                p++;
                skip_ws();
                if (*p == '+' || *p == '~') {
                    c->a2_rel = *p++;
                    c->a2.type = A_LINE;
                    c->a2.n = strtol(p, (char **)&p, 10);
                } else if (!parse_addr(&c->a2)) {
                    die("unexpected `,'%s", "");
                }
                if (c->a2.type == A_ZERO)
                    die("invalid usage of line address 0%s", "");
            }
            if (c->a1.type == A_ZERO && (c->a2.type != A_RE || c->a2_rel))
                die("invalid usage of line address 0%s", "");
        }
        skip_ws();
        while (*p == '!') {
            c->negate = 1;
            p++;
            skip_ws();
        }
        c->c = *p ? *p++ : 0;
        switch (c->c) {
        case '{':
            if (depth == 64)
                die("too many {%s", "");
            stack[depth++] = ncmds - 1;
            break;
        case '}':
            if (!depth)
                die("unexpected `}'%s", "");
            if (c->a1.type)
                die("} doesn't want any addresses%s", "");
            cmds[stack[--depth]].target = ncmds - 1;
            break;
        case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'l':
        case 'n': case 'N': case 'p': case 'P': case 'x': case 'z':
            break;
        case 'a': case 'i': case 'c':
            c->text = parse_text();
            break;
        case ':':
            if (c->a1.type)
                die(": doesn't want any addresses%s", "");
            c->text = rest_of_line(1);
            if (!*c->text)
                die("\":\" lacks a label%s", "");
            break;
        case 'b': case 't': case 'T':
            c->text = rest_of_line(1);
            break;
        case 'r':
            c->text = rest_of_line(0);
            break;
        case 'w': {
            char *name = rest_of_line(0);
            c->wf = open_wfile(name);
            free(name);
            break;
        }
        case 'q': case 'Q':
            skip_ws();
            c->qcode = isdigit(*p) ? strtol(p, (char **)&p, 10) : 0;
            break;
        case 's': {
            int delim = *p++;
            if (!delim || delim == '\n' || delim == '\\')
                die("unterminated `s' command%s", "");
            char *pat = delimited(delim, 1);
            c->repl = delimited(delim, 0);
            parse_replacement_flags(c);
            c->re = compile(pat, c->icase);
            free(pat);
            break;
        }
        case 'y': {
            int delim = *p++;
            char *from = delimited(delim, 0), *to = delimited(delim, 0);
            /* delimited() left escapes other than \delim and \n: undo \\ */
            char *f = from, *t = to;
            for (int i = 0; i < 256; i++)
                c->map[i] = i;
            while (*f && *t) {
                int fc = *f == '\\' && f[1] ? (f++, *f == '\\' ? '\\' : *f) : *f;
                int tc = *t == '\\' && t[1] ? (t++, *t == '\\' ? '\\' : *t) : *t;
                c->map[(unsigned char)fc] = tc;
                f++, t++;
            }
            if (*f || *t)
                die("strings for `y' command are different lengths%s", "");
            free(from);
            free(to);
            break;
        }
        case 'v': case 'F': case 'e':
            rest_of_line(1);
            break;
        default:
            eprintf("sed: unknown command: `%c'\n", c->c);
            exit(1);
        }
        skip_ws();
        if (c->c != '{' && *p && *p != ';' && *p != '\n' && *p != '}' && *p != '#')
            die("extra characters after command%s", "");
    }
    if (depth)
        die("unmatched `{'%s", "");
    /* resolve labels */
    for (int i = 0; i < ncmds; i++) {
        struct cmd *c = &cmds[i];
        if (c->c != 'b' && c->c != 't' && c->c != 'T')
            continue;
        if (!*c->text) {
            c->target = ncmds;          /* to the end of the script */
            continue;
        }
        for (int j = 0; j < ncmds; j++)
            if (cmds[j].c == ':' && !strcmp(cmds[j].text, c->text))
                c->target = j;
        if (c->target < 0)
            die("can't find label for jump to `%s'", c->text);
    }
}

/* ---- input ---- */

static char **files;
static int nfiles, fileno_;
static FILE *in;
static const char *in_name;
static FILE *out;                       /* stdout, or the temporary file with -i */
static char *tmp_name;
static struct buf next;                 /* the line after the current one */
static int have_next, next_nl;
static long lineno;
static int exit_code;

static int open_next_file(void)
{
    while (fileno_ < nfiles) {
        const char *name = files[fileno_++];
        if (!strcmp(name, "-") && !inplace) {
            in = stdin;
            in_name = "-";
            return 1;
        }
        struct stat st;
        if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
            eprintf("sed: couldn't edit %s: not a regular file\n", name);
            exit_code = 2;
            continue;
        }
        if (!(in = fopen(name, "r"))) {
            eprintf("sed: can't read %s: %s\n", name, strerror(errno));
            exit_code = 2;
            continue;
        }
        in_name = name;
        return 1;
    }
    return 0;
}

static void finish_inplace(void)
{
    if (!inplace || !tmp_name)
        return;
    fclose(out);
    if (*suffix) {
        char bak[1024];
        snprintf(bak, sizeof(bak), "%s%s", in_name, suffix);
        if (rename(in_name, bak) < 0)
            eprintf("sed: cannot rename %s: %s\n", in_name, strerror(errno));
    }
    if (rename(tmp_name, in_name) < 0)
        eprintf("sed: cannot rename %s: %s\n", tmp_name, strerror(errno));
    free(tmp_name);
    tmp_name = 0;
    out = stdout;
}

static void start_inplace(void)
{
    char t[1024];
    snprintf(t, sizeof(t), "%s.sedXXXXXX", in_name);
    int fd = mkstemp(t);
    if (fd < 0) {
        eprintf("sed: couldn't open temporary file %s: %s\n", t, strerror(errno));
        exit(4);
    }
    struct stat st;
    if (stat(in_name, &st) == 0)
        fchmod(fd, st.st_mode & 07777);
    out = fdopen(fd, "w");
    tmp_name = strdup(t);
}

/* Read one line into B from the current input; 1 if got one. */
static int read_raw(struct buf *b, int *nl)
{
    int delim = nulldata ? 0 : '\n';
    for (;;) {
        if (!in && (separate || !open_next_file()))
            return 0;                   /* separate files: process() opens them */
        char *line = 0;
        size_t cap = 0;
        ssize_t n = getdelim(&line, &cap, delim, in);
        if (n > 0) {
            *nl = line[n - 1] == delim;
            b_set(b, line, n - *nl);
            free(line);
            return 1;
        }
        free(line);
        if (in != stdin)
            fclose(in);
        in = 0;
        if (separate)
            return 0;
    }
}

/* The next line of the stream into PS; 1 if there was one. */
static int ps_nl;
static int read_line(struct buf *ps)
{
    if (!have_next && !read_raw(&next, &next_nl))
        return 0;
    b_set(ps, next.s, next.n);
    ps_nl = next_nl;
    have_next = read_raw(&next, &next_nl);
    lineno++;
    return 1;
}

static int is_last_line(void)
{
    return !have_next;
}

/* ---- execution ---- */

static struct buf ps, hs, append_q;
static int quit_now, tflag;

static void emit(FILE *f, const char *s, size_t n, int nl)
{
    fwrite(s, 1, n, f);
    if (nl)
        fputc(nulldata ? 0 : '\n', f);
}

static int match_re(regex_t *re, const char *s)
{
    if (!re)
        re = last_re;
    if (!re)
        die("no previous regular expression%s", "");
    last_re = re;
    return regexec(re, s, 0, 0, 0) == 0;
}

static int addr_match(struct addr *a)
{
    switch (a->type) {
    case A_LINE: return lineno == a->n;
    case A_LAST: return is_last_line();
    case A_RE: return match_re(a->re, ps.s ? ps.s : "");
    case A_STEP: return a->step <= 0 ? lineno == a->n : lineno >= a->n && (lineno - a->n) % a->step == 0;
    case A_ZERO: return 0;
    }
    return 1;
}

static int selected(struct cmd *c)
{
    int r;
    if (!c->a1.type) {
        r = 1;
    } else if (!c->a2.type) {
        r = addr_match(&c->a1);
    } else if (c->active) {
        /* inside a range: does this line end it? */
        r = 1;
        if (c->a2_rel == '+')
            c->active = lineno < c->end_line;
        else if (c->a2_rel == '~')
            c->active = !(c->a2.n <= 0 || lineno % c->a2.n == 0);
        else if (c->a2.type == A_LINE)
            c->active = lineno < c->a2.n;
        else
            c->active = !addr_match(&c->a2);
        if (c->a2.type == A_LINE && !c->a2_rel && lineno > c->a2.n)
            c->active = 0;
    } else if (c->a1.type == A_ZERO ? lineno == 1 || 0 : addr_match(&c->a1)) {
        r = 1;
        if (c->a1.type == A_ZERO) {
            c->active = !addr_match(&c->a2);
        } else if (c->a2_rel == '+') {
            c->end_line = lineno + c->a2.n;
            c->active = c->a2.n > 0;
        } else if (c->a2_rel == '~') {
            c->active = !(c->a2.n <= 0 || lineno % c->a2.n == 0);
        } else if (c->a2.type == A_LINE) {
            c->active = lineno < c->a2.n;
        } else {
            c->active = 1;              /* a regex end is checked from the next line */
        }
    } else if (c->a1.type == A_ZERO && c->active) {
        r = 1;
    } else {
        r = 0;
    }
    return r != c->negate;
}

/* The replacement for one match, case conversions included. */
static void substitute_one(struct buf *o, const char *repl, const char *s, regmatch_t *m)
{
    int one = 0, mode = 0;              /* one: 'u'/'l' for the next char; mode: 'U'/'L' */
    for (const char *r = repl; *r; r++) {
        const char *src = 0;
        size_t len = 0;
        char lit;
        if (*r == '\\' && r[1]) {
            r++;
            if (isdigit(*r)) {
                int g = *r - '0';
                if (m[g].rm_so >= 0)
                    src = s + m[g].rm_so, len = m[g].rm_eo - m[g].rm_so;
            } else if (*r == 'U' || *r == 'L') {
                mode = *r, one = 0;
                continue;
            } else if (*r == 'E') {
                mode = one = 0;
                continue;
            } else if (*r == 'u' || *r == 'l') {
                one = *r;
                continue;
            } else {
                lit = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
                src = &lit, len = 1;
            }
        } else if (*r == '&') {
            src = s + m[0].rm_so, len = m[0].rm_eo - m[0].rm_so;
        } else {
            lit = *r;
            src = &lit, len = 1;
        }
        for (size_t i = 0; i < len; i++) {
            int ch = (unsigned char)src[i];
            if (one) {
                ch = one == 'u' ? toupper(ch) : tolower(ch);
                one = 0;
            } else if (mode) {
                ch = mode == 'U' ? toupper(ch) : tolower(ch);
            }
            b_addc(o, ch);
        }
    }
}

static int do_subst(struct cmd *c)
{
    regex_t *re = c->re ? c->re : last_re;
    if (!re)
        die("no previous regular expression%s", "");
    last_re = re;
    regmatch_t m[10];
    struct buf o = { 0 };
    b_reserve(&o, ps.n);
    o.s[0] = 0;
    size_t pos = 0, prev_end = (size_t)-1;
    int count = 0, done = 0;
    while (pos <= ps.n) {
        int flags = pos ? REG_NOTBOL : 0;
        if (regexec(re, ps.s + pos, 10, m, flags))
            break;
        size_t so = pos + m[0].rm_so, eo = pos + m[0].rm_eo;
        if (so == eo && so == prev_end) {
            /* no empty match right after a match: step over a character */
            if (so >= ps.n)
                break;
            b_addc(&o, ps.s[so]);
            pos = so + 1;
            continue;
        }
        prev_end = eo;
        count++;
        if (count >= (c->occurrence ? c->occurrence : 1)) {
            b_add(&o, ps.s + pos, so - pos);
            for (int g = 0; g < 10; g++)
                if (m[g].rm_so >= 0)
                    m[g].rm_so += pos, m[g].rm_eo += pos;
            substitute_one(&o, c->repl, ps.s, m);
            done = 1;
        } else {
            b_add(&o, ps.s + pos, eo - pos);
        }
        if (eo == so) {                 /* an empty match: step over a character */
            if (so < ps.n)
                b_addc(&o, ps.s[so]);
            pos = so + 1;
        } else {
            pos = eo;
        }
        if (done && !c->global)
            break;
    }
    if (!done) {
        free(o.s);
        return 0;
    }
    if (pos < ps.n)
        b_add(&o, ps.s + pos, ps.n - pos);
    free(ps.s);
    ps = o;
    return 1;
}

static void list_line(void)
{
    int col = 0;
    for (size_t i = 0; i < ps.n; i++) {
        unsigned char ch = ps.s[i];
        char e[8];
        const char *esc = ch == '\\' ? "\\\\" : ch == '\a' ? "\\a" : ch == '\b' ? "\\b" : ch == '\f' ? "\\f"
                        : ch == '\n' ? "\\n" : ch == '\r' ? "\\r" : ch == '\t' ? "\\t" : ch == '\v' ? "\\v" : 0;
        if (!esc) {
            if (isprint(ch))
                e[0] = ch, e[1] = 0;
            else
                snprintf(e, sizeof(e), "\\%03o", ch);
            esc = e;
        }
        int w = strlen(esc);
        if (col + w > 69) {
            fputs("\\\n", out);
            col = 0;
        }
        fputs(esc, out);
        col += w;
    }
    fputs("$\n", out);
}

static void flush_appends(void)
{
    if (append_q.n)
        fwrite(append_q.s, 1, append_q.n, out);
    append_q.n = 0;
}

static void queue_file(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f)
        return;                         /* a missing file is silently ignored */
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0)
        b_add(&append_q, b, n);
    fclose(f);
}

/* End of cycle: print the pattern space, then whatever was appended. */
static void end_cycle(int autoprint)
{
    if (autoprint && !quiet)
        emit(out, ps.s, ps.n, ps_nl);
    flush_appends();
}

/* Run the script over the pattern space. Returns 0 to continue with the
   next cycle, 1 to restart the cycle without reading (D). */
static int run(void)
{
    tflag = 0;
    int restart = 0;
    for (int pc = 0; pc < ncmds; pc++) {
        struct cmd *c = &cmds[pc];
        if (c->c == ':' || c->c == '}')
            continue;
        if (!selected(c)) {
            if (c->c == '{')
                pc = c->target;
            continue;
        }
        switch (c->c) {
        case '{':
            break;
        case '=':
            fprintf(out, "%ld\n", lineno);
            break;
        case 'a':
            b_add(&append_q, c->text, strlen(c->text));
            b_addc(&append_q, '\n');
            break;
        case 'i':
            fputs(c->text, out);
            fputc('\n', out);
            break;
        case 'c':
            /* in a range: print the text at its end only */
            if (!c->a2.type || !c->active || c->negate) {
                fputs(c->text, out);
                fputc('\n', out);
            }
            flush_appends();
            return 0;
        case 'b':
            pc = c->target - 1;
            break;
        case 't':
            if (tflag) {
                tflag = 0;
                pc = c->target - 1;
            }
            break;
        case 'T':
            if (!tflag)
                pc = c->target - 1;
            else
                tflag = 0;
            break;
        case 'd':
            flush_appends();
            return 0;
        case 'D': {
            char *nl = memchr(ps.s, '\n', ps.n);
            if (!nl) {
                flush_appends();
                return 0;
            }
            size_t k = nl - ps.s + 1;
            memmove(ps.s, ps.s + k, ps.n - k + 1);
            ps.n -= k;
            flush_appends();
            restart = 1;
            return restart;
        }
        case 'g':
            b_set(&ps, hs.s ? hs.s : "", hs.n);
            break;
        case 'G':
            b_addc(&ps, '\n');
            b_add(&ps, hs.s ? hs.s : "", hs.n);
            break;
        case 'h':
            b_set(&hs, ps.s, ps.n);
            break;
        case 'H':
            b_addc(&hs, '\n');
            b_add(&hs, ps.s, ps.n);
            break;
        case 'l':
            list_line();
            break;
        case 'n':
            if (is_last_line()) {
                end_cycle(1);           /* GNU: print, then quit */
                quit_now = 1;
                return 2;
            }
            if (!quiet)
                emit(out, ps.s, ps.n, ps_nl);
            flush_appends();
            read_line(&ps);
            break;
        case 'N': {
            if (is_last_line()) {
                end_cycle(1);
                quit_now = 1;
                return 2;
            }
            flush_appends();
            struct buf line = { 0 };
            read_line(&line);
            b_addc(&ps, '\n');
            b_add(&ps, line.s, line.n);
            free(line.s);
            break;
        }
        case 'p':
            emit(out, ps.s, ps.n, 1);
            break;
        case 'P': {
            char *nl = memchr(ps.s, '\n', ps.n);
            emit(out, ps.s, nl ? (size_t)(nl - ps.s) : ps.n, 1);
            break;
        }
        case 'q':
            end_cycle(1);
            exit_code = c->qcode;
            quit_now = 1;
            return 2;
        case 'Q':
            exit_code = c->qcode;
            quit_now = 1;
            return 2;
        case 'r':
            queue_file(c->text);
            break;
        case 'w':
            emit(c->wf->f, ps.s, ps.n, 1);
            break;
        case 's':
            if (do_subst(c)) {
                tflag = 1;
                if (c->print)
                    emit(out, ps.s, ps.n, 1);
                if (c->wf)
                    emit(c->wf->f, ps.s, ps.n, 1);
            }
            break;
        case 'x': {
            struct buf t = ps;
            ps = hs;
            hs = t;
            if (!ps.s)
                b_set(&ps, "", 0);
            break;
        }
        case 'y':
            for (size_t i = 0; i < ps.n; i++)
                ps.s[i] = c->map[(unsigned char)ps.s[i]];
            break;
        case 'z':
            ps.n = 0;
            ps.s[0] = 0;
            break;
        }
    }
    end_cycle(1);
    return 0;
}

/* One stream: cycles until the input ends or q. */
static void cycles(void)
{
    lineno = separate ? 0 : lineno;
    for (int i = 0; i < ncmds; i++)
        cmds[i].active = 0;
    have_next = read_raw(&next, &next_nl);
    int restart = 0;
    while (!quit_now) {
        if (!restart && !read_line(&ps))
            break;
        restart = run() == 1;
    }
}

static void process(void)
{
    if (!separate) {
        cycles();
        return;
    }
    while (!quit_now && open_next_file()) {
        if (inplace)
            start_inplace();
        cycles();
        if (in && in != stdin)
            fclose(in);
        in = 0;
        finish_inplace();
    }
}

int main(int argc, char **argv)
{
    struct buf script = { 0 };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strncmp(argv[i], "--", 2)) {
            if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--silent")) quiet = 1;
            else if (!strcmp(argv[i], "--regexp-extended")) extended = 1;
            else if (!strcmp(argv[i], "--separate")) separate = 1;
            else if (!strncmp(argv[i], "--in-place", 10)) {
                inplace = separate = 1;
                if (argv[i][10] == '=')
                    suffix = argv[i] + 11;
            } else {
                eprintf("sed: unknown option %s\n", argv[i]);
                return 1;
            }
            continue;
        }
        for (char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'n': quiet = 1; break;
            case 'E':
            case 'r': extended = 1; break;
            case 's': separate = 1; break;
            case 'u': break;
            case 'z': nulldata = 1; break;
            case 'i':
                inplace = separate = 1;
                suffix = f + 1;
                f += strlen(f) - 1;
                break;
            case 'e':
            case 'f': {
                const char *arg = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!arg) {
                    eprintf("sed: option requires an argument -- '%c'\n", *f);
                    return 1;
                }
                if (script.n)
                    b_addc(&script, '\n');
                if (*f == 'e') {
                    b_add(&script, arg, strlen(arg));
                } else {
                    FILE *sf = !strcmp(arg, "-") ? stdin : fopen(arg, "r");
                    if (!sf) {
                        eprintf("sed: couldn't open file %s: %s\n", arg, strerror(errno));
                        return 1;
                    }
                    char b[4096];
                    size_t n;
                    while ((n = fread(b, 1, sizeof(b), sf)) > 0)
                        b_add(&script, b, n);
                    if (sf != stdin)
                        fclose(sf);
                }
                f += strlen(f) - 1;
                break;
            }
            default:
                eprintf("usage: sed [-nEsuz] [-i[SUFFIX]] [-e SCRIPT]... [-f FILE]... [SCRIPT] [FILE...]\n");
                return 1;
            }
        }
    }
    if (!script.s) {
        if (i >= argc) {
            eprintf("usage: sed [-nEsuz] [-i[SUFFIX]] [-e SCRIPT]... [-f FILE]... [SCRIPT] [FILE...]\n");
            return 1;
        }
        b_add(&script, argv[i], strlen(argv[i]));
        i++;
    }
    parse(script.s);
    static char *dash[] = { "-" };
    files = i < argc ? argv + i : dash;
    nfiles = i < argc ? argc - i : 1;
    if (inplace && i >= argc) {
        eprintf("sed: no input files\n");
        return 1;
    }
    out = stdout;
    process();
    for (struct wfile *w = wfiles; w; w = w->next)
        if (w->f != stdout && w->f != stderr)
            fclose(w->f);
    fflush(stdout);
    return exit_code;
}
