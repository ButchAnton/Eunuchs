/*
 * esh: command history.
 *
 * Kept in memory (HISTSIZE entries) and appended to $HISTFILE
 * (~/.esh_history) as each command is entered, so it survives crashes and
 * is shared by later shells. Multi-line commands are stored with
 * backslash-newline, as zsh does. Also: csh-style !-expansion.
 */
#include "esh.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct hist_ent *ents;
static int nents, cap, base = 1;

static int histsize(void)
{
    const char *s = var_get("HISTSIZE");
    int n = s ? atoi(s) : 0;
    return n > 0 ? n : 1000;
}

static int savehist(void)
{
    const char *s = var_get("SAVEHIST");
    int n = s ? atoi(s) : 0;
    return n > 0 ? n : 1000;
}

static const char *histfile(void)
{
    return var_get("HISTFILE");
}

static void push(const char *line, long long when)
{
    if (nents == cap) {
        cap = cap ? cap * 2 : 256;
        ents = xrealloc(ents, cap * sizeof(*ents));
    }
    ents[nents].line = xstrdup(line);
    ents[nents].when = when;
    nents++;
    int max = histsize();
    if (nents > max) {
        int drop = nents - max;
        for (int i = 0; i < drop; i++)
            free(ents[i].line);
        memmove(ents, ents + drop, (nents - drop) * sizeof(*ents));
        nents -= drop;
        base += drop;
    }
}

/* One stored line -> "\\\n" for embedded newlines. */
static void encode(struct sb *b, const char *line, long long when)
{
    if (opts[OPT_EXTENDEDHIST])
        sb_printf(b, ": %lld:0;", when);
    for (const char *p = line; *p; p++) {
        if (*p == '\n')
            sb_puts(b, "\\\n");
        else
            sb_putc(b, *p);
    }
    sb_putc(b, '\n');
}

static void load(const char *path)
{
    int len;
    char *text = read_file(path, &len);
    if (!text)
        return;
    struct sb cur = { 0 };
    int lines = 0;
    char *p = text;
    while (*p) {
        char *e = strchr(p, '\n');
        int n = e ? e - p : (int)strlen(p);
        if (n > 0 && p[n - 1] == '\\') {
            sb_putn(&cur, p, n - 1);
            sb_putc(&cur, '\n');
        } else {
            sb_putn(&cur, p, n);
            const char *line = sb_str(&cur);
            long long when = 0;
            if (line[0] == ':' && line[1] == ' ') {
                /* ": <time>:<duration>;command" */
                const char *semi = strchr(line, ';');
                if (semi) {
                    when = strtol(line + 2, 0, 10);
                    line = semi + 1;
                }
            }
            if (*line)
                push(line, when);
            sb_reset(&cur);
            lines++;
        }
        if (!e)
            break;
        p = e + 1;
    }
    sb_free(&cur);
    free(text);
    /* Keep the file from growing forever. */
    if (lines > savehist() + savehist() / 2) {
        int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
        if (fd >= 0) {
            struct sb b = { 0 };
            int start = nents > savehist() ? nents - savehist() : 0;
            for (int i = start; i < nents; i++)
                encode(&b, ents[i].line, ents[i].when);
            write(fd, b.s ? b.s : "", b.len);
            sb_free(&b);
            close(fd);
        }
    }
}

void hist_init(void)
{
    if (!var_get("HISTFILE")) {
        const char *home = var_get("HOME");
        struct sb b = { 0 };
        sb_puts(&b, home && strcmp(home, "/") ? home : "");
        sb_puts(&b, "/.esh_history");
        var_set("HISTFILE", sb_str(&b), 0);
        sb_free(&b);
    }
    if (!var_get("HISTSIZE"))
        var_set("HISTSIZE", "1000", 0);
    if (!var_get("SAVEHIST"))
        var_set("SAVEHIST", "1000", 0);
    load(histfile());
}

void hist_add(const char *line)
{
    if (!*line)
        return;
    int blank = 1;
    for (const char *p = line; *p; p++)
        if (!is_space(*p))
            blank = 0;
    if (blank)
        return;
    if (opts[OPT_HISTIGNORESPACE] && line[0] == ' ')
        return;
    if (opts[OPT_HISTIGNOREDUPS] && nents && !strcmp(ents[nents - 1].line, line))
        return;
    long long when = time(0);
    push(line, when);
    const char *f = histfile();
    if (!f || !*f)
        return;
    int fd = open(f, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0)
        return;
    struct sb b = { 0 };
    encode(&b, line, when);
    write(fd, b.s, b.len);
    sb_free(&b);
    close(fd);
}

void hist_save(void)
{
    /* lines are appended as they are entered: nothing to do */
}

void hist_clear(void)
{
    for (int i = 0; i < nents; i++)
        free(ents[i].line);
    base += nents;
    nents = 0;
}

int hist_count(void)
{
    return nents;
}

int hist_base(void)
{
    return base;
}

const char *hist_get(int i)
{
    return i >= 0 && i < nents ? ents[i].line : 0;
}

/* ---- words of a history line ---- */

static int split_words(const char *line, struct strv *out)
{
    const char *p = line;
    while (*p) {
        while (*p && is_space(*p))
            p++;
        if (!*p)
            break;
        const char *st = p;
        char q = 0;
        if (strchr("|&;<>()", *p)) {
            p++;
            while (*p && strchr("|&;<>", *p) && *p == p[-1])
                p++;
        } else {
            while (*p && (q || (!is_space(*p) && !strchr("|&;<>()", *p)))) {
                if (q) {
                    if (*p == q)
                        q = 0;
                    else if (*p == '\\' && q == '"' && p[1])
                        p++;
                } else if (*p == '\'' || *p == '"') {
                    q = *p;
                } else if (*p == '\\' && p[1]) {
                    p++;
                }
                p++;
            }
        }
        strv_push(out, astrndup(st, p - st));
    }
    return out->n;
}

const char *hist_last_word(int back, int nth)
{
    (void)nth;
    int i = nents - 1 - back;
    if (i < 0)
        return 0;
    struct strv w = { 0 };
    split_words(ents[i].line, &w);
    return w.n ? w.v[w.n - 1] : 0;
}

/* ---- !-expansion ---- */

static const char *find_event(const char *p, int *len, const char *cur_line)
{
    (void)cur_line;
    const char *st = p;
    int idx = -1;
    if (*p == '!') {
        idx = nents - 1;
        p++;
    } else if (*p == '-' && is_digit(p[1])) {
        p++;
        int n = 0;
        while (is_digit(*p))
            n = n * 10 + *p++ - '0';
        idx = nents - n;
    } else if (is_digit(*p)) {
        int n = 0;
        while (is_digit(*p))
            n = n * 10 + *p++ - '0';
        idx = n - base;
    } else if (*p == '?') {
        p++;
        const char *e = strchr(p, '?');
        int n = e ? e - p : (int)strlen(p);
        char *s = astrndup(p, n);
        p += n + (e ? 1 : 0);
        for (int i = nents - 1; i >= 0; i--)
            if (strstr(ents[i].line, s)) {
                idx = i;
                break;
            }
        if (idx < 0) {
            sh_error("no such event: %s", s);
            return 0;
        }
    } else if (*p == '$' || *p == '^' || *p == '*' || *p == ':') {
        idx = nents - 1;        /* !$ !^ !* !:n refer to the previous command */
    } else {
        const char *e = p;
        while (*e && !is_space(*e) && !strchr(":;&|<>()\"'", *e))
            e++;
        char *s = astrndup(p, e - p);
        p = e;
        for (int i = nents - 1; i >= 0; i--)
            if (!strncmp(ents[i].line, s, strlen(s))) {
                idx = i;
                break;
            }
        if (idx < 0) {
            sh_error("event not found: %s", s);
            return 0;
        }
    }
    if (idx < 0 || idx >= nents) {
        sh_error("no such event: %.*s", (int)(p - st), st);
        return 0;
    }
    *len = p - st;
    return ents[idx].line;
}

static char *apply_modifier(char *w, char m)
{
    char *slash = strrchr(w, '/');
    switch (m) {
    case 'h':
        if (slash)
            return astrndup(w, slash == w ? 1 : slash - w);
        return astrdup(".");
    case 't':
        return astrdup(slash ? slash + 1 : w);
    case 'r': {
        char *dot = strrchr(w, '.');
        if (dot && (!slash || dot > slash))
            return astrndup(w, dot - w);
        return w;
    }
    case 'e': {
        char *dot = strrchr(w, '.');
        if (dot && (!slash || dot > slash))
            return astrdup(dot + 1);
        return astrdup("");
    }
    case 'u':
        for (char *p = w = astrdup(w); *p; p++) *p = to_upper(*p);
        return w;
    case 'l':
        for (char *p = w = astrdup(w); *p; p++) *p = to_lower(*p);
        return w;
    }
    return w;
}

int hist_expand(const char *line, char **out)
{
    if (!opts[OPT_BANGHIST])
        return 0;
    struct sb b = { 0 };
    int changed = 0;
    const char *p = line;

    /* ^old^new[^] : quick substitution in the previous command */
    if (*p == '^' && nents) {
        const char *o = p + 1, *m = strchr(o, '^');
        if (m) {
            char *old = astrndup(o, m - o);
            const char *r = m + 1, *e = strchr(r, '^');
            char *rep = e ? astrndup(r, e - r) : astrdup(r);
            const char *prev = ents[nents - 1].line;
            const char *at = strstr(prev, old);
            if (!at || !*old) {
                sh_error("substitution failed");
                return -1;
            }
            sb_putn(&b, prev, at - prev);
            sb_puts(&b, rep);
            sb_puts(&b, at + strlen(old));
            if (e)
                sb_puts(&b, e + 1);
            *out = xstrdup(sb_str(&b));
            sb_free(&b);
            return 1;
        }
    }

    char q = 0;
    for (; *p; p++) {
        char c = *p;
        if (c == '\\' && p[1] && q != '\'') {
            if (p[1] == '!') {
                sb_putc(&b, '!');           /* \! is a literal ! */
                changed = 1;
                p++;
                continue;
            }
            sb_putc(&b, c);
            sb_putc(&b, *++p);
            continue;
        }
        if (q) {
            if (c == q)
                q = 0;
            if (q == '\'' || c != '!') {
                sb_putc(&b, c);
                continue;
            }
        } else if (c == '\'') {
            q = c;
            sb_putc(&b, c);
            continue;
        } else if (c == '"') {
            q = c;
            sb_putc(&b, c);
            continue;
        }
        if (c != '!' || !p[1] || is_space(p[1]) || p[1] == '=' || p[1] == '(' || p[1] == '"') {
            sb_putc(&b, c);
            continue;
        }
        int elen;
        const char *ev = find_event(p + 1, &elen, line);
        if (!ev) {
            sb_free(&b);
            return -1;
        }
        p += 1 + elen;
        /* word designators */
        struct strv words = { 0 };
        split_words(ev, &words);
        int from = 0, to = words.n - 1, whole = 1;
        if (*p == '$' || *p == '^' || *p == '*') {
            whole = 0;
            if (*p == '$') from = to = words.n - 1;
            else if (*p == '^') from = to = 1;
            else from = 1;
            p++;
        } else if (*p == ':' && (is_digit(p[1]) || strchr("$^*-", p[1]))) {
            whole = 0;
            p++;
            if (*p == '$') { from = to = words.n - 1; p++; }
            else if (*p == '^') { from = to = 1; p++; }
            else if (*p == '*') { from = 1; p++; }
            else {
                from = 0;
                if (is_digit(*p)) {
                    from = 0;
                    while (is_digit(*p))
                        from = from * 10 + *p++ - '0';
                }
                to = from;
                if (*p == '-') {
                    p++;
                    if (is_digit(*p)) {
                        to = 0;
                        while (is_digit(*p))
                            to = to * 10 + *p++ - '0';
                    } else if (*p == '$') {
                        to = words.n - 1;
                        p++;
                    } else {
                        to = words.n - 2;
                    }
                } else if (*p == '*') {
                    to = words.n - 1;
                    p++;
                }
            }
        }
        if (!whole && (from < 0 || from >= words.n || to >= words.n || (to < from && to != from - 1))) {
            sh_error("bad word specifier");
            sb_free(&b);
            return -1;
        }
        struct sb sel = { 0 };
        if (whole) {
            sb_puts(&sel, ev);
        } else {
            for (int i = from; i <= to; i++) {
                if (i > from)
                    sb_putc(&sel, ' ');
                sb_puts(&sel, words.v[i]);
            }
        }
        /* modifiers :h :t :r :e :u :l */
        char *w = sb_adup(&sel);
        sb_free(&sel);
        while (*p == ':' && p[1] && strchr("htreul", p[1])) {
            w = apply_modifier(w, p[1]);
            p += 2;
        }
        sb_puts(&b, w);
        changed = 1;
        p--;
    }
    if (!changed) {
        sb_free(&b);
        return 0;
    }
    *out = xstrdup(sb_str(&b));
    sb_free(&b);
    return 1;
}
