/* esh: memory, strings, character classes, sorting and pattern matching. */
#include "esh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- heap ---- */

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        eprintf("esh: out of memory\n");
        _exit(127);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        eprintf("esh: out of memory\n");
        _exit(127);
    }
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* ---- arena: a stack of chunks; a mark is (chunk, used) ---- */

struct chunk {
    struct chunk *prev;
    size_t size, used;
    char data[];
};

static struct chunk *arena;
/* Released chunks are kept for reuse: libc's malloc doesn't merge free
 * blocks, so handing 32 KiB chunks back and forth would fragment the heap. */
static struct chunk *spare;
static int nspare;

#define CHUNK 32768

void *amalloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!arena || arena->used + n > arena->size) {
        struct chunk *c = 0;
        if (n <= CHUNK && spare) {
            c = spare;
            spare = c->prev;
            nspare--;
        } else {
            size_t size = n > CHUNK ? n : CHUNK;
            c = xmalloc(sizeof(*c) + size);
            c->size = size;
        }
        c->prev = arena;
        c->used = 0;
        arena = c;
    }
    void *p = arena->data + arena->used;
    arena->used += n;
    return p;
}

char *astrndup(const char *s, size_t n)
{
    char *d = amalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *astrdup(const char *s)
{
    return astrndup(s, strlen(s));
}

struct amark amark(void)
{
    return (struct amark){ arena, arena ? arena->used : 0 };
}

void arelease(struct amark m)
{
    while (arena && arena != m.chunk) {
        struct chunk *c = arena;
        arena = c->prev;
        if (c->size == CHUNK && nspare < 8) {
            c->prev = spare;
            spare = c;
            nspare++;
        } else {
            free(c);
        }
    }
    if (arena)
        arena->used = m.used;
}

/* ---- string buffers and vectors ---- */

static void sb_grow(struct sb *b, int need)
{
    if (b->len + need + 1 <= b->cap)
        return;
    int cap = b->cap ? b->cap : 64;
    while (cap < b->len + need + 1)
        cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void sb_putc(struct sb *b, int c)
{
    sb_grow(b, 1);
    b->s[b->len++] = (char)c;
    b->s[b->len] = 0;
}

void sb_putn(struct sb *b, const char *s, int n)
{
    if (n <= 0)
        return;
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(struct sb *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

static void sb_out(char c, void *arg)
{
    sb_putc(arg, c);
}

void sb_printf(struct sb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvcbprintf(sb_out, b, fmt, ap);
    va_end(ap);
}

void sb_reset(struct sb *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = 0;
}

void sb_free(struct sb *b)
{
    free(b->s);
    b->s = 0;
    b->len = b->cap = 0;
}

char *sb_str(struct sb *b)
{
    if (!b->s)
        sb_grow(b, 0);
    b->s[b->len] = 0;
    return b->s;
}

char *sb_adup(struct sb *b)
{
    return astrndup(b->s ? b->s : "", b->len);
}

void strv_push(struct strv *sv, char *s)
{
    if (sv->n + 2 > sv->cap) {
        int cap = sv->cap ? sv->cap * 2 : 8;
        char **v = amalloc(cap * sizeof(char *));
        if (sv->n)
            memcpy(v, sv->v, sv->n * sizeof(char *));
        sv->v = v;
        sv->cap = cap;
    }
    sv->v[sv->n++] = s;
    sv->v[sv->n] = 0;
}

/* ---- small predicates ---- */

int is_name(const char *s)
{
    if (!is_name_start(*s))
        return 0;
    while (*++s)
        if (!is_name_char(*s))
            return 0;
    return 1;
}

int is_number(const char *s)
{
    if (*s == '-')
        s++;
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!is_digit(*s))
            return 0;
    return 1;
}

int strcasecmp_(const char *a, const char *b)
{
    while (*a && to_lower(*a) == to_lower(*b))
        a++, b++;
    return to_lower((unsigned char)*a) - to_lower((unsigned char)*b);
}

int strncasecmp_(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = to_lower((unsigned char)*a) - to_lower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

/* ---- sorting (merge sort; no qsort in libc) ---- */

static void msort(char **v, char **tmp, int n, int (*cmp)(const char *, const char *))
{
    if (n < 2)
        return;
    int h = n / 2;
    msort(v, tmp, h, cmp);
    msort(v + h, tmp, n - h, cmp);
    int i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(char *));
}

void sort_strs(char **v, int n)
{
    if (n < 2)
        return;
    char **tmp = xmalloc(n * sizeof(char *));
    msort(v, tmp, n, strcmp);
    free(tmp);
}


/* ---- UTF-8 ---- */

int str_width(const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n && s[i]; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            w++;
    return w;
}

int utf8_prev(const char *s, int pos)
{
    if (pos <= 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}

int utf8_next(const char *s, int len, int pos)
{
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos++;
    return pos;
}

/* Damerau-Levenshtein (optimal string alignment) distance. */
int edit_distance(const char *a, const char *b)
{
    int la = strlen(a), lb = strlen(b);
    if (la > 30 || lb > 30)
        return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++)
        d[i][0] = i;
    for (int j = 0; j <= lb; j++)
        d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] != b[j - 1];
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v)
                v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < v)
                v = d[i - 1][j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                d[i - 2][j - 2] + 1 < v)
                v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
        }
    return d[la][lb];
}

char *read_file(const char *path, int *lenp)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct sb b = { 0 };
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        sb_putn(&b, buf, n);
    close(fd);
    if (lenp)
        *lenp = b.len;
    return sb_str(&b);
}

/* ---- signals ---- */

static const struct {
    int num;
    const char *name;
} signames[] = {
#define X(name, num, act, desc) { num, #name },
    SIGNALS(X)
#undef X
};

const char *signame(int sig)
{
    for (size_t i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
        if (signames[i].num == sig)
            return signames[i].name + 3;
    return "?";
}

int sig_by_name(const char *s)
{
    if (is_number(s))
        return atoi(s);
    if (!strncasecmp_(s, "SIG", 3))
        s += 3;
    if (!strcasecmp_(s, "EXIT"))
        return 0;
    for (size_t i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
        if (!strcasecmp_(signames[i].name + 3, s))
            return signames[i].num;
    return -1;
}

const char *sigdesc(int sig)
{
    static char buf[48];
    const char *d = strsignal(sig);
    size_t i;
    for (i = 0; d[i] && i < sizeof(buf) - 1; i++)
        buf[i] = to_lower(d[i]);
    buf[i] = 0;
    if (sig == SIGTSTP || sig == SIGSTOP)
        return "suspended";
    if (sig == SIGTTIN)
        return "suspended (tty input)";
    if (sig == SIGTTOU)
        return "suspended (tty output)";
    if (sig == SIGINT)
        return "interrupt";
    return buf;
}

long long now_ms(void)
{
    return (long long)uptime_ms();
}

/* ---- patterns ---- */

/* Match a bracket expression at p (just after '['); returns the position
 * after ']' and sets *ok, or NULL if the bracket is unterminated. */
static const char *bracket(const char *p, int c, int fold, int *ok)
{
    int neg = 0, hit = 0;
    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        int lo = (unsigned char)*p;
        if (*p == '\\' && p[1])
            lo = (unsigned char)*++p;
        p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = (unsigned char)*p;
            if (*p == '\\' && p[1])
                hi = (unsigned char)*++p;
            p++;
        }
        if ((c >= lo && c <= hi) || (fold && to_lower(c) >= to_lower(lo) && to_lower(c) <= to_lower(hi)))
            hit = 1;
    }
    if (*p != ']')
        return 0;
    *ok = hit != neg;
    return p + 1;
}

int pmatch(const char *p, const char *s, int flags)
{
    int fold = flags & PM_CASEFOLD;
    const char *star_p = 0, *star_s = 0;
    for (;;) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return 1;
            star_p = p;
            star_s = s;
            continue;
        }
        if (!*s) {
            if (!*p)
                return 1;
            goto backtrack;
        }
        if (*p == '?') {
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            int ok;
            const char *q = bracket(p + 1, (unsigned char)*s, fold, &ok);
            if (q) {
                if (!ok)
                    goto backtrack;
                p = q;
                s++;
                continue;
            }
            /* unterminated: literal '[' */
        }
        {
            int pc = *p;
            if (pc == '\\' && p[1])
                pc = *++p;
            if (!pc)
                goto backtrack;
            if (pc == *s || (fold && to_lower(pc) == to_lower(*s))) {
                p++;
                s++;
                continue;
            }
        }
    backtrack:
        if (!star_p || !*star_s)
            return 0;
        p = star_p;
        s = ++star_s;
    }
}

int pattern_has_magic(const char *p)
{
    for (; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '*' || *p == '?')
            return 1;
        if (*p == '[') {
            /* only a bracket expression with its closing ] */
            const char *q = p + 1;
            if (*q == '!' || *q == '^')
                q++;
            if (*q == ']')
                q++;
            while (*q && *q != ']')
                q++;
            if (*q == ']')
                return 1;
        }
    }
    return 0;
}

int pmatch_prefix(const char *pat, const char *s, int longest)
{
    int n = strlen(s);
    char *buf = xmalloc(n + 1);
    int found = -1;
    for (int i = longest ? n : 0; longest ? i >= 0 : i <= n; i += longest ? -1 : 1) {
        memcpy(buf, s, i);
        buf[i] = 0;
        if (pmatch(pat, buf, 0)) {
            found = i;
            break;
        }
    }
    free(buf);
    return found;
}

int pmatch_suffix(const char *pat, const char *s, int longest)
{
    int n = strlen(s);
    for (int i = longest ? 0 : n; longest ? i <= n : i >= 0; i += longest ? 1 : -1)
        if (pmatch(pat, s + i, 0))
            return n - i;
    return -1;
}

char *unescape_pattern(const char *pat)
{
    char *d = amalloc(strlen(pat) + 1), *o = d;
    for (; *pat; pat++) {
        if (*pat == '\\' && pat[1])
            pat++;
        *o++ = *pat;
    }
    *o = 0;
    return d;
}
