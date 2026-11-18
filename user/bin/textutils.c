/*
 * textutils: text and binary filters in one binary, chosen by argv[0] (the
 * disk image has /bin/<tool> as hard links to it):
 *   sort uniq cut tr nl tac rev paste comm cmp fold expand
 *   od xxd strings cksum md5sum sha256sum
 * Each reads the files named (or standard input, also for "-").
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *prog;

static int fail(const char *what)
{
    eprintf("%s: %s: %s\n", prog, what, strerror(errno));
    return 1;
}

static void die_usage(const char *usage)
{
    eprintf("usage: %s %s\n", prog, usage);
    exit(2);
}

static FILE *open_in(const char *name)
{
    if (!strcmp(name, "-"))
        return stdin;
    FILE *f = fopen(name, "r");
    if (!f)
        fail(name);
    return f;
}

static void close_in(FILE *f)
{
    if (f && f != stdin)
        fclose(f);
}

/* ---- lines ---- */

struct lines {
    char **v;
    size_t *len;
    size_t n, cap;
};

static void lines_add(struct lines *l, char *s, size_t len)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = realloc(l->v, l->cap * sizeof(*l->v));
        l->len = realloc(l->len, l->cap * sizeof(*l->len));
        if (!l->v || !l->len) {
            eprintf("%s: out of memory\n", prog);
            exit(2);
        }
    }
    l->v[l->n] = s;
    l->len[l->n++] = len;
}

/* Every line of F, without its newline. */
static void read_lines(FILE *f, struct lines *l)
{
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = 0;
        lines_add(l, line, n);
        line = 0;
        cap = 0;
    }
    free(line);
}

/* Run FN on each file in argv[i..] (standard input if none). */
static int each_file(int argc, char **argv, int i, int (*fn)(FILE *, const char *))
{
    int rc = 0;
    if (i >= argc)
        return fn(stdin, "-");
    for (; i < argc; i++) {
        FILE *f = open_in(argv[i]);
        if (!f) {
            rc = 1;
            continue;
        }
        rc |= fn(f, argv[i]);
        close_in(f);
    }
    return rc;
}

static long num_arg(const char *s, const char *what)
{
    char *end;
    long v = strtol(s, &end, 0);
    if (end == s || *end) {
        eprintf("%s: bad %s: '%s'\n", prog, what, s);
        exit(2);
    }
    return v;
}

/* ================================================================ sort */

#define MAXKEYS 8

struct key {
    int f1, c1, f2, c2;                 /* fields and chars, 1-based; f2 0 = end of line */
    int numeric, reverse, fold, blanks;
};

static struct key keys[MAXKEYS];
static int nkeys;
static int sort_sep = -1;               /* -t; -1: runs of blanks */
static int s_numeric, s_reverse, s_fold, s_blanks, s_unique;

/* Start of field F (1-based) in S, or the end of S. */
static const char *field_start(const char *s, const char *end, int f)
{
    for (int i = 1; i < f && s < end; i++) {
        if (sort_sep >= 0) {
            while (s < end && *s != sort_sep)
                s++;
            if (s < end)
                s++;
        } else {
            while (s < end && isblank(*s))
                s++;
            while (s < end && !isblank(*s))
                s++;
        }
    }
    return s;
}

static const char *field_end(const char *s, const char *end)
{
    if (sort_sep >= 0) {
        while (s < end && *s != sort_sep)
            s++;
    } else {
        while (s < end && isblank(*s))
            s++;
        while (s < end && !isblank(*s))
            s++;
    }
    return s;
}

static void key_span(const struct key *k, const char *line, size_t len, const char **a, const char **b)
{
    const char *end = line + len;
    if (!k->f1) {                       /* no -k: the whole line */
        *a = line, *b = end;
        return;
    }
    const char *s = field_start(line, end, k->f1);
    if (k->blanks || sort_sep < 0)
        while (s < end && isblank(*s))
            s++;
    if (k->c1 > 1)
        s = s + k->c1 - 1 < end ? s + k->c1 - 1 : end;
    const char *e = end;
    if (k->f2) {
        const char *fs = field_start(line, end, k->f2);
        if (k->c2) {
            if (sort_sep < 0)
                while (fs < end && isblank(*fs))
                    fs++;
            e = fs + k->c2 < end ? fs + k->c2 : end;
        } else {
            e = field_end(fs, end);
        }
    }
    *a = s;
    *b = e < s ? s : e;
}

static int cmp_span(const char *a, const char *ae, const char *b, const char *be, int numeric, int fold)
{
    if (numeric) {
        while (a < ae && isblank(*a))
            a++;
        while (b < be && isblank(*b))
            b++;
        char ba[64], bb[64];
        size_t la = ae - a < 63 ? (size_t)(ae - a) : 63, lb = be - b < 63 ? (size_t)(be - b) : 63;
        memcpy(ba, a, la), ba[la] = 0;
        memcpy(bb, b, lb), bb[lb] = 0;
        double x = strtod(ba, 0), y = strtod(bb, 0);
        return x < y ? -1 : x > y;
    }
    for (; a < ae && b < be; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (fold)
            x = toupper(x), y = toupper(y);
        if (x != y)
            return x - y;
    }
    return (a < ae) - (b < be);
}

struct sline {
    const char *s;
    size_t len, idx;
};

static int sort_keys_cmp(const struct sline *x, const struct sline *y)
{
    for (int i = 0; i < nkeys; i++) {
        const struct key *k = &keys[i];
        const char *a, *ae, *b, *be;
        key_span(k, x->s, x->len, &a, &ae);
        key_span(k, y->s, y->len, &b, &be);
        int r = cmp_span(a, ae, b, be, k->numeric, k->fold);
        if (r)
            return k->reverse ? -r : r;
    }
    return 0;
}

static int sort_cmp(const void *pa, const void *pb)
{
    const struct sline *x = pa, *y = pb;
    int r = sort_keys_cmp(x, y);
    if (r || s_unique)
        return r;
    /* last resort: the whole line, bytewise (reversed with -r), then input order */
    r = cmp_span(x->s, x->s + x->len, y->s, y->s + y->len, 0, 0);
    if (r)
        return s_reverse ? -r : r;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static void parse_key(const char *spec)
{
    if (nkeys == MAXKEYS)
        die_usage("[-nrufb] [-t SEP] [-k F[.C][,F[.C]]]... [-o FILE] [FILE...]");
    struct key *k = &keys[nkeys++];
    char *p;
    k->f1 = strtol(spec, &p, 10);
    if (k->f1 < 1)
        die_usage("[-nrufb] [-t SEP] [-k F[.C][,F[.C]]]... [-o FILE] [FILE...]");
    if (*p == '.')
        k->c1 = strtol(p + 1, &p, 10);
    for (; *p && *p != ','; p++) {
        if (*p == 'n') k->numeric = 1;
        else if (*p == 'r') k->reverse = 1;
        else if (*p == 'f') k->fold = 1;
        else if (*p == 'b') k->blanks = 1;
    }
    if (*p == ',') {
        k->f2 = strtol(p + 1, &p, 10);
        if (*p == '.')
            k->c2 = strtol(p + 1, &p, 10);
        for (; *p; p++) {
            if (*p == 'n') k->numeric = 1;
            else if (*p == 'r') k->reverse = 1;
            else if (*p == 'f') k->fold = 1;
            else if (*p == 'b') k->blanks = 1;
        }
    }
}

static int sort_main(int argc, char **argv)
{
    const char *out = 0;
    int c, check = 0;
    while ((c = getopt(argc, argv, "nrufbt:k:o:c")) != -1) {
        switch (c) {
        case 'n': s_numeric = 1; break;
        case 'r': s_reverse = 1; break;
        case 'u': s_unique = 1; break;
        case 'f': s_fold = 1; break;
        case 'b': s_blanks = 1; break;
        case 'c': check = 1; break;
        case 't':
            if (strlen(optarg) != 1 && strcmp(optarg, "\\t"))
                die_usage("[-nrufbc] [-t SEP] [-k F[.C][,F[.C]]]... [-o FILE] [FILE...]");
            sort_sep = !strcmp(optarg, "\\t") ? '\t' : (unsigned char)optarg[0];
            break;
        case 'k': parse_key(optarg); break;
        case 'o': out = optarg; break;
        default: die_usage("[-nrufbc] [-t SEP] [-k F[.C][,F[.C]]]... [-o FILE] [FILE...]");
        }
    }
    /* options apply to keys that have none of their own; no -k: the whole line */
    if (!nkeys)
        keys[nkeys++] = (struct key){ 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < nkeys; i++) {
        struct key *k = &keys[i];
        if (!k->numeric && !k->reverse && !k->fold && !k->blanks) {
            k->numeric = s_numeric, k->reverse = s_reverse, k->fold = s_fold;
            k->blanks = s_blanks;
        }
    }

    struct lines l = { 0 };
    int rc = 0;
    if (optind >= argc)
        read_lines(stdin, &l);
    for (int i = optind; i < argc; i++) {
        FILE *f = open_in(argv[i]);
        if (!f) {
            rc = 2;
            continue;
        }
        read_lines(f, &l);
        close_in(f);
    }
    struct sline *v = malloc((l.n ? l.n : 1) * sizeof(*v));
    for (size_t i = 0; i < l.n; i++)
        v[i] = (struct sline){ l.v[i], l.len[i], i };
    if (check) {
        for (size_t i = 1; i < l.n; i++) {
            int r = sort_cmp(&v[i - 1], &v[i]);
            if (r > 0 || (s_unique && r == 0)) {
                eprintf("sort: disorder at line %lu: %s\n", (unsigned long)i + 1, v[i].s);
                return 1;
            }
        }
        return rc;
    }
    qsort(v, l.n, sizeof(*v), sort_cmp);
    FILE *o = stdout;
    if (out && !(o = fopen(out, "w")))
        return fail(out) + 1;
    for (size_t i = 0; i < l.n; i++) {
        if (s_unique && i && sort_keys_cmp(&v[i - 1], &v[i]) == 0)
            continue;
        fwrite(v[i].s, 1, v[i].len, o);
        fputc('\n', o);
    }
    if (o != stdout)
        fclose(o);
    return rc;
}

/* ================================================================ uniq */

static int u_skip_fields, u_skip_chars, u_icase;

static const char *uniq_key(const char *s)
{
    for (int i = 0; i < u_skip_fields; i++) {
        while (isblank(*s))
            s++;
        while (*s && !isblank(*s))
            s++;
    }
    for (int i = 0; i < u_skip_chars && *s; i++)
        s++;
    return s;
}

static int uniq_same(const char *a, const char *b)
{
    a = uniq_key(a), b = uniq_key(b);
    return u_icase ? !strcasecmp(a, b) : !strcmp(a, b);
}

static int uniq_main(int argc, char **argv)
{
    int c, count = 0, dups = 0, singles = 0;
    while ((c = getopt(argc, argv, "cdduif:s:")) != -1) {
        switch (c) {
        case 'c': count = 1; break;
        case 'd': dups = 1; break;
        case 'u': singles = 1; break;
        case 'i': u_icase = 1; break;
        case 'f': u_skip_fields = num_arg(optarg, "field count"); break;
        case 's': u_skip_chars = num_arg(optarg, "character count"); break;
        default: die_usage("[-cdui] [-f N] [-s N] [INPUT [OUTPUT]]");
        }
    }
    FILE *in = stdin, *out = stdout;
    if (optind < argc && !(in = open_in(argv[optind])))
        return 1;
    if (optind + 1 < argc && !(out = fopen(argv[optind + 1], "w")))
        return fail(argv[optind + 1]);
    char *prev = 0, *line = 0;
    size_t pcap = 0, cap = 0;
    long n = 0;
    ssize_t len;
    for (;;) {
        len = getline(&line, &cap, in);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = 0;
        if (len >= 0 && prev && uniq_same(prev, line)) {
            n++;
            continue;
        }
        if (prev && ((!dups && !singles) || (dups && n > 1) || (singles && n == 1))) {
            if (count)
                fprintf(out, "%7ld %s\n", n, prev);
            else
                fprintf(out, "%s\n", prev);
        }
        if (len < 0)
            break;
        char *t = prev;
        size_t tc = pcap;
        prev = line, pcap = cap;
        line = t, cap = tc;
        n = 1;
    }
    close_in(in);
    if (out != stdout)
        fclose(out);
    return 0;
}

/* ================================================================ cut */

#define MAXRANGES 64

static struct { long lo, hi; } ranges[MAXRANGES];
static int nranges;

static void parse_list(const char *list)
{
    char *copy = strdup(list), *save, *tok;
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(0, ",", &save)) {
        if (nranges == MAXRANGES)
            break;
        char *dash = strchr(tok, '-');
        long lo, hi;
        if (!dash)
            lo = hi = atol(tok);
        else {
            lo = dash == tok ? 1 : atol(tok);
            hi = dash[1] ? atol(dash + 1) : 0x7fffffff;
        }
        if (lo < 1 || hi < lo) {
            eprintf("%s: bad list '%s'\n", prog, list);
            exit(2);
        }
        ranges[nranges].lo = lo;
        ranges[nranges++].hi = hi;
    }
    free(copy);
}

static int in_list(long i)
{
    for (int r = 0; r < nranges; r++)
        if (i >= ranges[r].lo && i <= ranges[r].hi)
            return 1;
    return 0;
}

static int cut_mode, cut_delim = '\t', cut_only_delimited;

static int cut_file(FILE *f, const char *name)
{
    (void)name;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = 0;
        if (cut_mode != 'f') {
            for (ssize_t i = 0; i < n; i++)
                if (in_list(i + 1))
                    putchar(line[i]);
            putchar('\n');
            continue;
        }
        if (!memchr(line, cut_delim, n)) {
            if (!cut_only_delimited)
                printf("%s\n", line);
            continue;
        }
        long fno = 1;
        int first = 1;
        char *s = line;
        for (;;) {
            char *d = strchr(s, cut_delim);
            if (d)
                *d = 0;
            if (in_list(fno)) {
                if (!first)
                    putchar(cut_delim);
                fputs(s, stdout);
                first = 0;
            }
            if (!d)
                break;
            s = d + 1;
            fno++;
        }
        putchar('\n');
    }
    free(line);
    return 0;
}

static int cut_main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "b:c:f:d:s")) != -1) {
        switch (c) {
        case 'b':
        case 'c':
        case 'f':
            cut_mode = c;
            parse_list(optarg);
            break;
        case 'd':
            if (strlen(optarg) != 1)
                die_usage("-b LIST | -c LIST | -f LIST [-d C] [-s] [FILE...]");
            cut_delim = (unsigned char)optarg[0];
            break;
        case 's': cut_only_delimited = 1; break;
        default: die_usage("-b LIST | -c LIST | -f LIST [-d C] [-s] [FILE...]");
        }
    }
    if (!cut_mode)
        die_usage("-b LIST | -c LIST | -f LIST [-d C] [-s] [FILE...]");
    return each_file(argc, argv, optind, cut_file);
}

/* ================================================================ tr */

/* Unescape one character of a tr set; returns it and advances *p. */
static int unescape(const char **p)
{
    const char *s = *p;
    int c = (unsigned char)*s++;
    if (c == '\\' && *s) {
        c = (unsigned char)*s++;
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'v': c = '\v'; break;
        case '\\': c = '\\'; break;
        default:
            if (c >= '0' && c <= '7') {
                c -= '0';
                for (int i = 0; i < 2 && *s >= '0' && *s <= '7'; i++)
                    c = c * 8 + (*s++ - '0');
            }
        }
    }
    *p = s;
    return c & 0xff;
}

static int class_has(const char *name, size_t len, int c)
{
#define IS(n) (len == strlen(n) && !strncmp(name, n, len))
    if (IS("alpha")) return isalpha(c);
    if (IS("digit")) return isdigit(c);
    if (IS("alnum")) return isalnum(c);
    if (IS("upper")) return isupper(c);
    if (IS("lower")) return islower(c);
    if (IS("space")) return isspace(c);
    if (IS("blank")) return isblank(c);
    if (IS("punct")) return ispunct(c);
    if (IS("print")) return isprint(c);
    if (IS("graph")) return isgraph(c);
    if (IS("cntrl")) return iscntrl(c);
    if (IS("xdigit")) return isxdigit(c);
#undef IS
    eprintf("tr: unknown class '[:%.*s:]'\n", (int)len, name);
    exit(2);
}

/* Expand a tr set into OUT (at most 256... repeats allowed); returns the length. */
static int expand_set(const char *s, unsigned char *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (e) {
                for (int c = 0; c < 256 && n < max; c++)
                    if (class_has(s + 2, e - s - 2, c))
                        out[n++] = c;
                s = e + 2;
                continue;
            }
        }
        if (s[0] == '[' && s[1] && s[2] == '*') {
            /* [c*n] or [c*]: repeat c (n times; 0 or none: to fill SET2) */
            const char *p = s + 1;
            int c = unescape(&p);
            if (*p == '*') {
                char *e;
                long r = strtol(p + 1, &e, 10);
                if (*e == ']') {
                    if (r <= 0)
                        r = max - n;        /* fill: trimmed by the caller */
                    for (long i = 0; i < r && n < max; i++)
                        out[n++] = c;
                    s = e + 1;
                    continue;
                }
            }
        }
        int c = unescape(&s);
        if (*s == '-' && s[1]) {
            s++;
            int hi = unescape(&s);
            if (hi < c) {
                eprintf("tr: range %c-%c is backwards\n", c, hi);
                exit(2);
            }
            for (int x = c; x <= hi && n < max; x++)
                out[n++] = x;
        } else {
            out[n++] = c;
        }
    }
    return n;
}

static int tr_main(int argc, char **argv)
{
    int c, comp = 0, del = 0, squeeze = 0;
    while ((c = getopt(argc, argv, "cCds")) != -1) {
        switch (c) {
        case 'c':
        case 'C': comp = 1; break;
        case 'd': del = 1; break;
        case 's': squeeze = 1; break;
        default: die_usage("[-cds] SET1 [SET2]");
        }
    }
    int nsets = argc - optind;
    if (nsets < 1 || nsets > 2 || (del && !squeeze && nsets != 1) || (!del && !squeeze && nsets != 2))
        die_usage("[-cds] SET1 [SET2]");
    static unsigned char s1[4096], s2[4096];
    int n1 = expand_set(argv[optind], s1, sizeof(s1));
    int n2 = nsets > 1 ? expand_set(argv[optind + 1], s2, sizeof(s2)) : 0;
    unsigned char in1[256] = { 0 };
    for (int i = 0; i < n1; i++)
        in1[s1[i]] = 1;
    if (comp) {
        n1 = 0;
        for (int x = 0; x < 256; x++)
            if (!in1[x])
                s1[n1++] = x;
        memset(in1, 0, sizeof(in1));
        for (int i = 0; i < n1; i++)
            in1[s1[i]] = 1;
    }
    int map[256];
    for (int x = 0; x < 256; x++)
        map[x] = x;
    int translate = !del && n2 > 0;
    if (translate) {
        for (int i = 0; i < n1; i++)
            map[s1[i]] = s2[i < n2 ? i : n2 - 1];
    }
    /* the set squeezed: SET2 when translating or deleting, else SET1 */
    unsigned char sq[256] = { 0 };
    if (squeeze) {
        if (nsets == 2)
            for (int i = 0; i < n2; i++)
                sq[s2[i]] = 1;
        else
            memcpy(sq, in1, sizeof(sq));
    }
    int last = -1;
    while ((c = getchar()) != EOF) {
        if (del && in1[c])
            continue;
        int o = map[c];
        if (squeeze && sq[o] && o == last)
            continue;
        putchar(o);
        last = o;
    }
    return 0;
}

/* ================================================================ nl */

static int nl_main(int argc, char **argv)
{
    int c, width = 6, incr = 1;
    long start = 1;
    char style = 't';
    const char *fmt = "rn", *sep = "\t";
    while ((c = getopt(argc, argv, "b:n:w:s:v:i:")) != -1) {
        switch (c) {
        case 'b': style = optarg[0]; break;
        case 'n': fmt = optarg; break;
        case 'w': width = num_arg(optarg, "width"); break;
        case 's': sep = optarg; break;
        case 'v': start = num_arg(optarg, "start"); break;
        case 'i': incr = num_arg(optarg, "increment"); break;
        default: die_usage("[-b a|t|n] [-n ln|rn|rz] [-w N] [-s SEP] [-v N] [-i N] [FILE]");
        }
    }
    FILE *f = optind < argc ? open_in(argv[optind]) : stdin;
    if (!f)
        return 1;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    long num = start;
    while ((n = getline(&line, &cap, f)) > 0) {
        int empty = line[0] == '\n';
        if (style == 'a' || (style == 't' && !empty)) {
            if (!strcmp(fmt, "ln"))
                printf("%-*ld%s", width, num, sep);
            else if (!strcmp(fmt, "rz"))
                printf("%0*ld%s", width, num, sep);
            else
                printf("%*ld%s", width, num, sep);
            num += incr;
        } else {
            printf("%*s", width + (int)strlen(sep), "");
        }
        fwrite(line, 1, n, stdout);
        if (line[n - 1] != '\n')
            putchar('\n');
    }
    free(line);
    close_in(f);
    return 0;
}

/* ================================================================ tac, rev */

static int tac_file(FILE *f, const char *name)
{
    (void)name;
    struct lines l = { 0 };
    read_lines(f, &l);
    for (size_t i = l.n; i-- > 0;) {
        fwrite(l.v[i], 1, l.len[i], stdout);
        putchar('\n');
        free(l.v[i]);
    }
    free(l.v);
    free(l.len);
    return 0;
}

/* Reverse each line by character (UTF-8 sequences stay whole). */
static int rev_file(FILE *f, const char *name)
{
    (void)name;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        int nl = line[n - 1] == '\n';
        if (nl)
            n--;
        ssize_t e = n;
        while (e > 0) {
            ssize_t s = e - 1;
            while (s > 0 && (line[s] & 0xc0) == 0x80)
                s--;
            fwrite(line + s, 1, e - s, stdout);
            e = s;
        }
        if (nl)
            putchar('\n');
    }
    free(line);
    return 0;
}

/* ================================================================ paste */

static int paste_main(int argc, char **argv)
{
    int c, serial = 0;
    const char *delims = "\t";
    while ((c = getopt(argc, argv, "d:s")) != -1) {
        switch (c) {
        case 'd': delims = optarg; break;
        case 's': serial = 1; break;
        default: die_usage("[-s] [-d LIST] FILE...");
        }
    }
    /* the delimiter list, unescaped (\n \t \\ \0 = none) */
    char dl[64];
    int nd = 0;
    for (const char *p = delims; *p && nd < 64;) {
        if (*p == '\\' && p[1] == '0') {
            dl[nd++] = 0;
            p += 2;
        } else {
            dl[nd++] = unescape(&p);
        }
    }
    if (!nd)
        dl[nd++] = 0;
    int nf = argc - optind;
    char *dflt[] = { "-" };
    char **names = nf ? argv + optind : dflt;
    if (!nf)
        nf = 1;
    FILE **fs = calloc(nf, sizeof(*fs));
    for (int i = 0; i < nf; i++)
        if (!(fs[i] = open_in(names[i])))
            return 1;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    if (serial) {
        for (int i = 0; i < nf; i++) {
            int k = 0;
            while ((n = getline(&line, &cap, fs[i])) > 0) {
                if (line[n - 1] == '\n')
                    line[--n] = 0;
                if (k && dl[(k - 1) % nd])
                    putchar(dl[(k - 1) % nd]);
                fputs(line, stdout);
                k++;
            }
            putchar('\n');
        }
    } else {
        char **row = calloc(nf, sizeof(*row));
        for (;;) {
            int got = 0;
            for (int i = 0; i < nf; i++) {
                free(row[i]);
                row[i] = 0;
                n = getline(&line, &cap, fs[i]);
                if (n > 0) {
                    got = 1;
                    if (line[n - 1] == '\n')
                        line[--n] = 0;
                    row[i] = strdup(line);
                }
            }
            if (!got)
                break;
            for (int i = 0; i < nf; i++) {
                if (i && dl[(i - 1) % nd])
                    putchar(dl[(i - 1) % nd]);
                if (row[i])
                    fputs(row[i], stdout);
            }
            putchar('\n');
        }
    }
    free(line);
    return 0;
}

/* ================================================================ comm */

static int comm_main(int argc, char **argv)
{
    int hide[4] = { 0 };
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f < '1' || *f > '3')
                die_usage("[-123] FILE1 FILE2");
            hide[*f - '0'] = 1;
        }
    if (argc - i != 2)
        die_usage("[-123] FILE1 FILE2");
    FILE *a = open_in(argv[i]), *b = open_in(argv[i + 1]);
    if (!a || !b)
        return 2;
    char *la = 0, *lb = 0;
    size_t ca = 0, cb = 0;
    ssize_t na = getline(&la, &ca, a), nb = getline(&lb, &cb, b);
    while (na > 0 || nb > 0) {
        if (na > 0 && la[na - 1] == '\n')
            la[na - 1] = 0;
        if (nb > 0 && lb[nb - 1] == '\n')
            lb[nb - 1] = 0;
        int r = na <= 0 ? 1 : nb <= 0 ? -1 : strcmp(la, lb);
        int col = r < 0 ? 1 : r > 0 ? 2 : 3;
        if (!hide[col]) {
            for (int k = 1; k < col; k++)
                if (!hide[k])
                    putchar('\t');
            puts(col == 2 ? lb : la);
        }
        if (r <= 0)
            na = getline(&la, &ca, a);
        if (r >= 0)
            nb = getline(&lb, &cb, b);
    }
    return 0;
}

/* ================================================================ cmp */

static int cmp_main(int argc, char **argv)
{
    int c, list = 0, silent = 0;
    while ((c = getopt(argc, argv, "ls")) != -1) {
        if (c == 'l') list = 1;
        else if (c == 's') silent = 1;
        else die_usage("[-l | -s] FILE1 FILE2 [SKIP1 [SKIP2]]");
    }
    if (argc - optind < 2 || argc - optind > 4)
        die_usage("[-l | -s] FILE1 FILE2 [SKIP1 [SKIP2]]");
    const char *n1 = argv[optind], *n2 = argv[optind + 1];
    FILE *a = open_in(n1), *b = open_in(n2);
    if (!a || !b)
        return 2;
    long skip1 = argc - optind > 2 ? num_arg(argv[optind + 2], "skip") : 0;
    long skip2 = argc - optind > 3 ? num_arg(argv[optind + 3], "skip") : 0;
    while (skip1-- > 0 && getc(a) != EOF)
        ;
    while (skip2-- > 0 && getc(b) != EOF)
        ;
    long byte = 1, line = 1;
    int rc = 0;
    for (;; byte++) {
        int x = getc(a), y = getc(b);
        if (x == EOF && y == EOF)
            break;
        if (x == EOF || y == EOF) {
            if (!silent)
                eprintf("cmp: EOF on %s after byte %ld\n", x == EOF ? n1 : n2, byte - 1);
            return 1;
        }
        if (x != y) {
            rc = 1;
            if (silent)
                break;
            if (!list) {
                printf("%s %s differ: byte %ld, line %ld\n", n1, n2, byte, line);
                break;
            }
            printf("%ld %3o %3o\n", byte, x, y);
        }
        if (x == '\n')
            line++;
    }
    return rc;
}

/* ================================================================ fold, expand */

static int fold_width = 80, fold_bytes, fold_spaces;

static int col_after(int col, int c)
{
    if (fold_bytes)
        return col + 1;
    if (c == '\t')
        return (col / 8 + 1) * 8;
    if (c == '\b')
        return col > 0 ? col - 1 : 0;
    if (c == '\r')
        return 0;
    if ((c & 0xc0) == 0x80)
        return col;                     /* UTF-8 continuation */
    return col + 1;
}

static int fold_file(FILE *f, const char *name)
{
    (void)name;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        int nl = line[n - 1] == '\n';
        if (nl)
            n--;
        ssize_t start = 0;
        while (start < n) {
            int col = 0;
            ssize_t i = start, last_space = -1;
            while (i < n) {
                int nc = col_after(col, (unsigned char)line[i]);
                if (nc > fold_width && i > start)
                    break;
                col = nc;
                if (line[i] == ' ' || line[i] == '\t')
                    last_space = i;
                i++;
            }
            if (i < n && fold_spaces && last_space >= start)
                i = last_space + 1;
            fwrite(line + start, 1, i - start, stdout);
            if (i < n)
                putchar('\n');
            start = i;
        }
        if (nl)
            putchar('\n');
    }
    free(line);
    return 0;
}

static int fold_main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "w:bs")) != -1) {
        switch (c) {
        case 'w': fold_width = num_arg(optarg, "width"); break;
        case 'b': fold_bytes = 1; break;
        case 's': fold_spaces = 1; break;
        default: die_usage("[-bs] [-w WIDTH] [FILE...]");
        }
    }
    if (fold_width < 1)
        die_usage("[-bs] [-w WIDTH] [FILE...]");
    return each_file(argc, argv, optind, fold_file);
}

static int tabstops[64], ntabs = 0, initial_only;

static int next_stop(int col)
{
    if (ntabs == 1)
        return (col / tabstops[0] + 1) * tabstops[0];
    for (int i = 0; i < ntabs; i++)
        if (tabstops[i] > col)
            return tabstops[i];
    return col + 1;
}

static int expand_file(FILE *f, const char *name)
{
    (void)name;
    int c, col = 0, leading = 1;
    while ((c = getc(f)) != EOF) {
        if (c == '\t' && (!initial_only || leading)) {
            int to = next_stop(col);
            while (col < to)
                putchar(' '), col++;
            continue;
        }
        if (c != ' ' && c != '\t')
            leading = 0;
        putchar(c);
        if (c == '\n')
            col = 0, leading = 1;
        else if (c == '\b')
            col = col ? col - 1 : 0;
        else if ((c & 0xc0) != 0x80)
            col++;
    }
    return 0;
}

static int expand_main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "it:")) != -1) {
        if (c == 'i') {
            initial_only = 1;
        } else if (c == 't') {
            char *p = optarg;
            ntabs = 0;
            while (*p && ntabs < 64) {
                tabstops[ntabs++] = strtol(p, &p, 10);
                if (*p == ',' || *p == ' ')
                    p++;
            }
        } else {
            die_usage("[-i] [-t N | -t LIST] [FILE...]");
        }
    }
    if (!ntabs)
        tabstops[ntabs++] = 8;
    if (tabstops[0] < 1)
        die_usage("[-i] [-t N | -t LIST] [FILE...]");
    return each_file(argc, argv, optind, expand_file);
}

/* ================================================================ od */

struct odtype {
    char kind;                          /* o x d u c a */
    int size;
};

static struct odtype odtypes[8];
static int nodtypes;

static void od_add(char kind, int size)
{
    if (nodtypes < 8)
        odtypes[nodtypes++] = (struct odtype){ kind, size };
}

static int od_width(const struct odtype *t)
{
    static const int o[] = { 0, 3, 6, 0, 11, 0, 0, 0, 22 }, x[] = { 0, 2, 4, 0, 8, 0, 0, 0, 16 },
                     d[] = { 0, 4, 6, 0, 11, 0, 0, 0, 20 }, u[] = { 0, 3, 5, 0, 10, 0, 0, 0, 20 };
    switch (t->kind) {
    case 'o': return o[t->size];
    case 'x': return x[t->size];
    case 'd': return d[t->size];
    case 'u': return u[t->size];
    default: return 3;
    }
}

static void parse_odtype(const char *s)
{
    while (*s) {
        char kind = *s++;
        int size = kind == 'c' || kind == 'a' ? 1 : 4;
        if (strchr("oxdu", kind)) {
            if (*s == 'C') size = 1, s++;
            else if (*s == 'S') size = 2, s++;
            else if (*s == 'I') size = 4, s++;
            else if (*s == 'L') size = 8, s++;
            else if (isdigit(*s)) size = strtol(s, (char **)&s, 10);
        } else if (kind != 'c' && kind != 'a') {
            eprintf("od: bad type '%c'\n", kind);
            exit(2);
        }
        if (size != 1 && size != 2 && size != 4 && size != 8) {
            eprintf("od: bad size %d\n", size);
            exit(2);
        }
        od_add(kind, size);
    }
}

static void od_item(const struct odtype *t, const unsigned char *p, int width)
{
    static const char *const names[] = { "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
                                         "bs",  "ht",  "nl",  "vt",  "ff",  "cr",  "so",  "si",
                                         "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
                                         "can", "em",  "sub", "esc", "fs",  "gs",  "rs",  "us" };
    if (t->kind == 'c' || t->kind == 'a') {
        int c = *p;
        char b[8];
        if (t->kind == 'a') {
            c &= 0x7f;
            if (c < 32)
                snprintf(b, sizeof(b), "%s", names[c]);
            else if (c == 32)
                snprintf(b, sizeof(b), "sp");
            else if (c == 127)
                snprintf(b, sizeof(b), "del");
            else
                snprintf(b, sizeof(b), "%c", c);
        } else {
            const char *esc = c == 0 ? "\\0" : c == '\a' ? "\\a" : c == '\b' ? "\\b" : c == '\f' ? "\\f"
                            : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : c == '\v' ? "\\v" : 0;
            if (esc)
                snprintf(b, sizeof(b), "%s", esc);
            else if (isprint(c))
                snprintf(b, sizeof(b), "%c", c);
            else
                snprintf(b, sizeof(b), "%03o", c);
        }
        printf("%*s", width, b);
        return;
    }
    uint64_t v = 0;
    memcpy(&v, p, t->size);             /* little-endian */
    int64_t sv = t->size == 1 ? (int8_t)v : t->size == 2 ? (int16_t)v : t->size == 4 ? (int32_t)v : (int64_t)v;
    int w = od_width(t);
    switch (t->kind) {
    case 'o': printf("%*s%0*llo", width - w, "", w, (unsigned long long)v); break;
    case 'x': printf("%*s%0*llx", width - w, "", w, (unsigned long long)v); break;
    case 'd': printf("%*lld", width, (long long)sv); break;
    default: printf("%*llu", width, (unsigned long long)v); break;
    }
}

static int od_main(int argc, char **argv)
{
    int c, verbose = 0;
    char radix = 'o';
    long skip = 0, limit = -1;
    while ((c = getopt(argc, argv, "A:t:j:N:vbcdox")) != -1) {
        switch (c) {
        case 'A':
            radix = optarg[0];
            if (!strchr("odxn", radix))
                die_usage("[-A o|d|x|n] [-t TYPE] [-bcdox] [-j SKIP] [-N COUNT] [-v] [FILE...]");
            break;
        case 't': parse_odtype(optarg); break;
        case 'j': skip = num_arg(optarg, "skip"); break;
        case 'N': limit = num_arg(optarg, "count"); break;
        case 'v': verbose = 1; break;
        case 'b': od_add('o', 1); break;
        case 'c': od_add('c', 1); break;
        case 'd': od_add('u', 2); break;
        case 'o': od_add('o', 2); break;
        case 'x': od_add('x', 2); break;
        default: die_usage("[-A o|d|x|n] [-t TYPE] [-bcdox] [-j SKIP] [-N COUNT] [-v] [FILE...]");
        }
    }
    if (!nodtypes)
        od_add('o', 2);
    /* columns: every type's items line up, each byte gets the same width */
    int perbyte = 1;
    for (int i = 0; i < nodtypes; i++) {
        int w = (od_width(&odtypes[i]) + 1 + odtypes[i].size - 1) / odtypes[i].size;
        if (w > perbyte)
            perbyte = w;
    }
    /* the input: every file, one after the other */
    int nf = argc - optind, fi = 0;
    FILE *f = nf ? 0 : stdin;
    unsigned char buf[16], prev[16];
    long addr = 0;
    int have_prev = 0, starred = 0;
    for (;;) {
        int n = 0;
        while (n < 16 && (limit < 0 || addr + n < skip + limit)) {
            if (!f) {
                if (fi >= nf)
                    break;
                if (!(f = open_in(argv[optind + fi++])))
                    return 1;
            }
            int ch = getc(f);
            if (ch == EOF) {
                close_in(f);
                f = 0;
                if (!nf)
                    break;
                continue;
            }
            if (addr < skip) {
                addr++;
                continue;
            }
            buf[n++] = ch;
        }
        if (!n)
            break;
        if (!verbose && have_prev && n == 16 && !memcmp(buf, prev, 16)) {
            if (!starred)
                puts("*");
            starred = 1;
            addr += n;
            continue;
        }
        starred = 0;
        for (int t = 0; t < nodtypes; t++) {
            const struct odtype *ty = &odtypes[t];
            if (radix == 'n')
                ;
            else if (t)
                printf("%*s", radix == 'x' ? 6 : 7, "");
            else
                printf(radix == 'o' ? "%07lo" : radix == 'd' ? "%07ld" : "%06lx", addr);
            unsigned char padded[16 + 8] = { 0 };
            memcpy(padded, buf, n);
            for (int i = 0; i < n; i += ty->size)
                od_item(ty, padded + i, perbyte * ty->size);
            putchar('\n');
        }
        memcpy(prev, buf, n);
        have_prev = n == 16;
        addr += n;
        if (n < 16)
            break;
    }
    if (radix != 'n')
        printf(radix == 'o' ? "%07lo\n" : radix == 'd' ? "%07ld\n" : "%06lx\n", addr);
    return 0;
}

/* ================================================================ xxd */

static int unhex(int c)
{
    return isdigit(c) ? c - '0' : isxdigit(c) ? tolower(c) - 'a' + 10 : -1;
}

static int xxd_reverse(FILE *in, FILE *out, int plain)
{
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, in)) > 0) {
        char *p = line;
        if (!plain) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            long off = strtol(line, 0, 16);
            fseek(out, off, SEEK_SET);
            p = colon + 1;
        }
        /* hex digit pairs until two spaces (the text column) or the end */
        int hi = -1;
        for (; *p && *p != '\n'; p++) {
            if (!plain && p[0] == ' ' && p[1] == ' ')
                break;
            int v = unhex((unsigned char)*p);
            if (v < 0)
                continue;
            if (hi < 0)
                hi = v;
            else {
                fputc(hi << 4 | v, out);
                hi = -1;
            }
        }
    }
    free(line);
    return 0;
}

static int xxd_main(int argc, char **argv)
{
    int c, cols = 16, group = 2, plain = 0, rev = 0, upper = 0, include = 0;
    long len = -1, seek = 0;
    int cols_set = 0, group_set = 0;
    while ((c = getopt(argc, argv, "c:g:l:s:pruib")) != -1) {
        switch (c) {
        case 'c': cols = num_arg(optarg, "columns"); cols_set = 1; break;
        case 'g': group = num_arg(optarg, "group size"); group_set = 1; break;
        case 'l': len = num_arg(optarg, "length"); break;
        case 's': seek = num_arg(optarg, "seek"); break;
        case 'p': plain = 1; break;
        case 'r': rev = 1; break;
        case 'u': upper = 1; break;
        case 'i': include = 1; break;
        default: die_usage("[-p] [-i] [-r] [-u] [-c COLS] [-g BYTES] [-l LEN] [-s SEEK] [IN [OUT]]");
        }
    }
    FILE *in = stdin, *out = stdout;
    const char *iname = optind < argc ? argv[optind] : 0;
    if (iname && strcmp(iname, "-") && !(in = fopen(iname, "r")))
        return fail(iname);
    if (optind + 1 < argc && !(out = fopen(argv[optind + 1], rev ? "w" : "w")))
        return fail(argv[optind + 1]);
    if (rev)
        return xxd_reverse(in, out, plain);
    if (plain && !cols_set)
        cols = 30;
    if (include && !cols_set)
        cols = 12;
    if (cols < 1 || cols > 256)
        die_usage("[-p] [-i] [-r] [-u] [-c COLS] [-g BYTES] [-l LEN] [-s SEEK] [IN [OUT]]");
    if (!group_set && group > cols)
        group = cols;
    if (seek > 0)
        for (long i = 0; i < seek && getc(in) != EOF; i++)
            ;
    const char *hx = upper ? "%02X" : "%02x";
    unsigned char buf[256];
    long addr = seek, total = 0;
    if (include && iname) {
        fputs("unsigned char ", out);
        for (const char *p = iname; *p; p++)
            fputc(isalnum(*p) ? *p : '_', out);
        fputs("[] = {\n", out);
    }
    for (;;) {
        int n = 0, ch;
        while (n < cols && (len < 0 || total + n < len) && (ch = getc(in)) != EOF)
            buf[n++] = ch;
        if (!n)
            break;
        if (include) {
            /* is there more after this line? (no comma after the last byte) */
            int more = 0;
            if (n == cols && (len < 0 || total + n < len) && (ch = getc(in)) != EOF) {
                ungetc(ch, in);
                more = 1;
            }
            fputs(" ", out);
            for (int i = 0; i < n; i++)
                fprintf(out, upper ? " 0X%02X%s" : " 0x%02x%s", buf[i], i + 1 < n || more ? "," : "");
            fputc('\n', out);
        } else if (plain) {
            for (int i = 0; i < n; i++)
                fprintf(out, hx, buf[i]);
            fputc('\n', out);
        } else {
            fprintf(out, "%08lx:", addr);
            for (int i = 0; i < cols; i++) {
                if (group && i % group == 0)
                    fputc(' ', out);
                if (i < n)
                    fprintf(out, hx, buf[i]);
                else
                    fputs("  ", out);
            }
            fputs("  ", out);
            for (int i = 0; i < n; i++)
                fputc(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.', out);
            fputc('\n', out);
        }
        addr += n;
        total += n;
        if (n < cols)
            break;
    }
    if (include && iname) {
        fputs("};\nunsigned int ", out);
        for (const char *p = iname; *p; p++)
            fputc(isalnum(*p) ? *p : '_', out);
        fprintf(out, "_len = %ld;\n", total);
    }
    return 0;
}

/* ================================================================ strings */

static int str_min = 4;
static char str_radix;

static int strings_file(FILE *f, const char *name)
{
    (void)name;
    char *run = malloc(4096);
    size_t n = 0, cap = 4096;
    long off = 0, start = 0;
    int c;
    for (;; off++) {
        c = getc(f);
        if (c != EOF && (isprint(c) || c == '\t')) {
            if (!n)
                start = off;
            if (n + 1 >= cap)
                run = realloc(run, cap *= 2);
            run[n++] = c;
            continue;
        }
        if ((int)n >= str_min) {
            if (str_radix)
                printf(str_radix == 'o' ? "%7lo " : str_radix == 'x' ? "%7lx " : "%7ld ", start);
            fwrite(run, 1, n, stdout);
            putchar('\n');
        }
        n = 0;
        if (c == EOF)
            break;
    }
    free(run);
    return 0;
}

static int strings_main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "an:t:")) != -1) {
        switch (c) {
        case 'a': break;                /* the whole file is always scanned */
        case 'n': str_min = num_arg(optarg, "length"); break;
        case 't':
            str_radix = optarg[0];
            if (!strchr("dox", str_radix))
                die_usage("[-a] [-n MIN] [-t d|o|x] [FILE...]");
            break;
        default: die_usage("[-a] [-n MIN] [-t d|o|x] [FILE...]");
        }
    }
    if (str_min < 1)
        str_min = 1;
    return each_file(argc, argv, optind, strings_file);
}

/* ================================================================ cksum */

static uint32_t crc_table[256];

static int cksum_file(FILE *f, const char *name)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++)
                c = c & 0x80000000 ? (c << 1) ^ 0x04C11DB7 : c << 1;
            crc_table[i] = c;
        }
    uint32_t crc = 0;
    unsigned long len = 0;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            crc = (crc << 8) ^ crc_table[(crc >> 24) ^ buf[i]];
        len += n;
    }
    for (unsigned long l = len; l; l >>= 8)
        crc = (crc << 8) ^ crc_table[(crc >> 24) ^ (l & 0xff)];
    crc = ~crc;
    if (strcmp(name, "-"))
        printf("%u %lu %s\n", crc, len, name);
    else
        printf("%u %lu\n", crc, len);
    return 0;
}

/* ================================================================ md5, sha256 */

struct hash {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    int n;
    int sha;                            /* 1: SHA-256, 0: MD5 */
};

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void md5_block(uint32_t *h, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

static void sha256_block(uint32_t *h, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void hash_init(struct hash *s, int sha)
{
    static const uint32_t md5_iv[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    static const uint32_t sha_iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memset(s, 0, sizeof(*s));
    s->sha = sha;
    memcpy(s->h, sha ? sha_iv : md5_iv, sha ? sizeof(sha_iv) : sizeof(md5_iv));
}

static void hash_block(struct hash *s, const unsigned char *p)
{
    if (s->sha)
        sha256_block(s->h, p);
    else
        md5_block(s->h, p);
}

static void hash_update(struct hash *s, const unsigned char *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t room = 64 - (size_t)s->n, k = room < n ? room : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k, p += k, n -= k;
        if (s->n == 64) {
            hash_block(s, s->buf);
            s->n = 0;
        }
    }
}

/* The digest as lowercase hex in OUT (65 bytes). */
static void hash_final(struct hash *s, char *out)
{
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    hash_update(s, &pad, 1);
    while (s->n != 56)
        hash_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = s->sha ? bits >> (56 - 8 * i) : bits >> (8 * i);
    hash_update(s, lenb, 8);
    int words = s->sha ? 8 : 4;
    for (int i = 0; i < words; i++)
        for (int b = 0; b < 4; b++) {
            unsigned v = s->sha ? s->h[i] >> (24 - 8 * b) : s->h[i] >> (8 * b);
            sprintf(out + (i * 4 + b) * 2, "%02x", v & 0xff);
        }
}

static int is_sha;

static int digest(FILE *f, char *out)
{
    struct hash s;
    hash_init(&s, is_sha);
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        hash_update(&s, buf, n);
    if (ferror(f))
        return -1;
    hash_final(&s, out);
    return 0;
}

static int sum_file(FILE *f, const char *name)
{
    char out[65];
    if (digest(f, out) < 0)
        return fail(name);
    printf("%s  %s\n", out, name);
    return 0;
}

/* -c: check "HASH  NAME" lines. */
static int sum_check(FILE *list, const char *lname)
{
    (void)lname;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    int bad = 0, hexlen = is_sha ? 64 : 32;
    while ((n = getline(&line, &cap, list)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = 0;
        if (n < hexlen + 2 || line[hexlen] != ' ')
            continue;
        const char *name = line + hexlen + 2;
        if (line[hexlen + 1] != ' ' && line[hexlen + 1] != '*')
            continue;
        FILE *f = fopen(name, "r");
        char out[65];
        if (!f || digest(f, out) < 0) {
            printf("%s: FAILED open or read\n", name);
            bad++;
        } else if (strncasecmp(out, line, hexlen)) {
            printf("%s: FAILED\n", name);
            bad++;
        } else {
            printf("%s: OK\n", name);
        }
        if (f)
            fclose(f);
    }
    free(line);
    if (bad)
        eprintf("%s: WARNING: %d computed checksum%s did NOT match\n", prog, bad, bad == 1 ? "" : "s");
    return bad ? 1 : 0;
}

static int sum_main(int argc, char **argv)
{
    is_sha = !strcmp(prog, "sha256sum");
    int c, check = 0;
    while ((c = getopt(argc, argv, "cb")) != -1) {
        if (c == 'c') check = 1;
        else if (c != 'b') die_usage("[-c] [FILE...]");
    }
    return each_file(argc, argv, optind, check ? sum_check : sum_file);
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(prog, "sort")) return sort_main(argc, argv);
    if (!strcmp(prog, "uniq")) return uniq_main(argc, argv);
    if (!strcmp(prog, "cut")) return cut_main(argc, argv);
    if (!strcmp(prog, "tr")) return tr_main(argc, argv);
    if (!strcmp(prog, "nl")) return nl_main(argc, argv);
    if (!strcmp(prog, "tac")) return each_file(argc, argv, 1, tac_file);
    if (!strcmp(prog, "rev")) return each_file(argc, argv, 1, rev_file);
    if (!strcmp(prog, "paste")) return paste_main(argc, argv);
    if (!strcmp(prog, "comm")) return comm_main(argc, argv);
    if (!strcmp(prog, "cmp")) return cmp_main(argc, argv);
    if (!strcmp(prog, "fold")) return fold_main(argc, argv);
    if (!strcmp(prog, "expand")) return expand_main(argc, argv);
    if (!strcmp(prog, "od")) return od_main(argc, argv);
    if (!strcmp(prog, "xxd")) return xxd_main(argc, argv);
    if (!strcmp(prog, "strings")) return strings_main(argc, argv);
    if (!strcmp(prog, "cksum")) return each_file(argc, argv, 1, cksum_file);
    if (!strcmp(prog, "md5sum") || !strcmp(prog, "sha256sum")) return sum_main(argc, argv);
    eprintf("textutils: run me as one of: sort uniq cut tr nl tac rev paste comm cmp fold expand "
            "od xxd strings cksum md5sum sha256sum\n");
    return 2;
}
