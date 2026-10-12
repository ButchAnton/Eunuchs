/* edit: searching and substituting with regular expressions. */
#include "edit.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char last_pattern[256];
int search_hl_off;
int last_dir = 1;
int opt_ignorecase, opt_smartcase = 1;

int search_icase(const char *pat)
{
    if (!opt_ignorecase)
        return 0;
    if (opt_smartcase)
        for (const char *p = pat; *p; p++)
            if (isupper((unsigned char)*p))
                return 0;
    return 1;
}

static int compile(regex_t *re, const char *pat, int icase)
{
    int r = regcomp(re, pat, icase ? REG_ICASE : 0);
    if (r) {
        char e[80];
        regerror(r, re, e, sizeof(e));
        err("E: bad pattern: %s", e);
        return -1;
    }
    return 0;
}

/* All matches on a line, in order: starts and ends (up to max). */
static int line_find_all(regex_t *re, struct line *l, int *so, int *eo, int max)
{
    char *s = xstrndup(l->s, l->len);
    int n = 0, pos = 0;
    while (n < max && pos <= l->len) {
        regmatch_t m;
        m.rm_so = pos;
        m.rm_eo = l->len;
        if (regexec(re, s, 1, &m, REG_STARTEND | (pos ? REG_NOTBOL : 0)))
            break;
        so[n] = (int)m.rm_so;
        eo[n] = (int)m.rm_eo;
        n++;
        pos = m.rm_eo > m.rm_so ? (int)m.rm_eo : (int)m.rm_eo + 1;
    }
    free(s);
    return n;
}

int search_find(struct buffer *b, const char *pat, int dir, int fy, int fx, int *ry, int *rx,
                int *rlen, int wrap, int icase)
{
    regex_t re;
    if (compile(&re, pat, icase))
        return -1;
    int so[256], eo[256], found = 0;
    /* step 0 is the rest of the start line; step nlines (with wrap) its other part */
    for (int step = 0; step <= b->nlines && !found; step++) {
        int y = fy + dir * step;
        if (y < 0 || y >= b->nlines) {
            if (!wrap)
                break;
            y = (y % b->nlines + b->nlines) % b->nlines;
        }
        int n = line_find_all(&re, &b->lines[y], so, eo, 256);
        int lo = 0, hi = n - 1;
        for (int k = 0; k < n; k++) {
            int i = dir > 0 ? lo + k : hi - k;
            int ok = step == 0 ? (dir > 0 ? so[i] > fx : so[i] < fx)
                   : step == b->nlines ? (dir > 0 ? so[i] <= fx : so[i] >= fx) : 1;
            if (ok) {
                *ry = y, *rx = so[i], *rlen = eo[i] - so[i];
                found = 1;
                break;
            }
        }
    }
    regfree(&re);
    return found;
}

int line_matches(const char *pat, const char *line, int icase)
{
    regex_t re;
    if (regcomp(&re, pat, icase ? REG_ICASE : 0))
        return 0;
    int r = regexec(&re, line, 0, 0, 0) == 0;
    regfree(&re);
    return r;
}

/* The replacement for one match: & is the match, \1-\9 groups, \& and \\ literal. */
static char *expand_rep(const char *rep, const char *s, regmatch_t *m, int *len)
{
    int cap = 64, n = 0;
    char *o = xmalloc(cap);
    for (const char *p = rep; *p; p++) {
        const char *src = 0;
        int sl = 0;
        if (*p == '&') {
            src = s + m[0].rm_so;
            sl = (int)(m[0].rm_eo - m[0].rm_so);
        } else if (*p == '\\' && p[1] >= '0' && p[1] <= '9') {
            int g = *++p - '0';
            if (m[g].rm_so >= 0) {
                src = s + m[g].rm_so;
                sl = (int)(m[g].rm_eo - m[g].rm_so);
            }
        } else if (*p == '\\' && p[1] == 'n') {
            p++;
            src = "\n";
            sl = 1;
        } else if (*p == '\\' && p[1]) {
            src = ++p;
            sl = 1;
        } else {
            src = p;
            sl = 1;
        }
        if (n + sl + 1 > cap) {
            while (n + sl + 1 > cap)
                cap *= 2;
            o = xrealloc(o, cap);
        }
        if (sl)
            memcpy(o + n, src, sl), n += sl;
    }
    o[n] = 0;
    *len = n;
    return o;
}

int replace_at(struct buffer *b, int y, int x, const char *pat, const char *rep, int icase, int *ey,
               int *ex, int *mlen)
{
    regex_t re;
    if (compile(&re, pat, icase))
        return -1;
    struct line *l = &b->lines[y];
    char *s = xstrndup(l->s, l->len);
    regmatch_t m[10];
    m[0].rm_so = x;
    m[0].rm_eo = l->len;
    int r = regexec(&re, s, 10, m, REG_STARTEND | (x ? REG_NOTBOL : 0));
    regfree(&re);
    if (r || m[0].rm_so != x) {
        free(s);
        return 0;
    }
    int rl;
    char *t = expand_rep(rep, s, m, &rl);
    free(s);
    *mlen = (int)(m[0].rm_eo - m[0].rm_so);
    free(buf_delete(b, y, x, y, (int)m[0].rm_eo));
    buf_insert(b, y, x, t, rl, ey, ex);
    free(t);
    return 1;
}

int substitute(struct buffer *b, int y1, int y2, const char *pat, const char *rep, int global,
               int icase, int confirm, int *nsubs)
{
    (void)confirm;
    regex_t re;
    if (compile(&re, pat, icase))
        return -1;
    *nsubs = 0;
    int lines_changed = 0;
    for (int y = y1; y <= y2 && y < b->nlines; y++) {
        int pos = 0, changed = 0;
        for (;;) {
            struct line *l = &b->lines[y];
            if (pos > l->len)
                break;
            char *s = xstrndup(l->s, l->len);
            regmatch_t m[10];
            m[0].rm_so = pos;
            m[0].rm_eo = l->len;
            if (regexec(&re, s, 10, m, REG_STARTEND | (pos ? REG_NOTBOL : 0))) {
                free(s);
                break;
            }
            int rl;
            char *r = expand_rep(rep, s, m, &rl);
            free(s);
            free(buf_delete(b, y, (int)m[0].rm_so, y, (int)m[0].rm_eo));
            int ey, ex;
            buf_insert(b, y, (int)m[0].rm_so, r, rl, &ey, &ex);
            /* a replacement with newlines moves us down */
            y2 += ey - y;
            y = ey;
            pos = ex + (m[0].rm_eo == m[0].rm_so ? 1 : 0);
            free(r);
            (*nsubs)++;
            changed = 1;
            b->cy = y;
            b->cx = ex > 0 ? ex - 1 : 0;
            if (!global)
                break;
        }
        lines_changed += changed;
    }
    regfree(&re);
    return lines_changed;
}
