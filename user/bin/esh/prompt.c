/*
 * esh: prompt expansion (zsh's % escapes) and user/host names.
 *
 *   %n user  %m host  %~ cwd (with ~)  %/ cwd  %c last component  %# # or %
 *   %? status  %j jobs  %h history number  %L SHLVL  %T %* %t %D %D{fmt} time
 *   %F{c} %f colors  %K{c} %k background  %B %b bold  %U %u underline
 *   %S %s standout  %E clear to end of line  %(x.true.false)  %{ %} raw
 *
 * The width of what is displayed (on the last line) is returned so the
 * line editor can place the cursor; escape sequences take no columns.
 */
#include "esh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- users and hosts ---- */

struct pwent {
    char name[32];
    int uid;
    char home[128];
};

static int read_passwd(int (*fn)(struct pwent *, void *), void *arg)
{
    char *text = read_file("/etc/passwd", 0);
    if (!text)
        return 0;
    int r = 0;
    for (char *line = text; line && *line && !r;) {
        char *e = strchr(line, '\n');
        if (e)
            *e = 0;
        char *f[7] = { 0 };
        int n = 0;
        for (char *p = line; n < 7;) {
            f[n++] = p;
            p = strchr(p, ':');
            if (!p)
                break;
            *p++ = 0;
        }
        if (n >= 6) {
            struct pwent pw;
            snprintf(pw.name, sizeof(pw.name), "%s", f[0]);
            pw.uid = atoi(f[2]);
            snprintf(pw.home, sizeof(pw.home), "%s", f[5]);
            r = fn(&pw, arg);
        }
        line = e ? e + 1 : 0;
    }
    free(text);
    return r;
}

struct uid_q {
    int uid;
    char *out;
};

static int by_uid(struct pwent *pw, void *arg)
{
    struct uid_q *q = arg;
    if (pw->uid != q->uid)
        return 0;
    snprintf(q->out, 32, "%s", pw->name);
    return 1;
}

const char *user_name(void)
{
    static char name[32];
    static int cached_uid = -1;
    int uid = geteuid();
    if (uid != cached_uid) {
        struct uid_q q = { uid, name };
        if (!read_passwd(by_uid, &q))
            snprintf(name, sizeof(name), "uid%d", uid);
        cached_uid = uid;
    }
    return name;
}

const char *host_name(void)
{
    static char host[64];
    if (!host[0]) {
        char *t = read_file("/etc/hostname", 0);
        if (t) {
            int n = 0;
            while (t[n] && t[n] != '\n' && n < 63)
                n++;
            memcpy(host, t, n);
            host[n] = 0;
            free(t);
        }
        if (!host[0])
            strcpy(host, "eunuchs");
    }
    return host;
}

struct home_q {
    const char *user;
    char *buf;
    int len;
};

static int by_name(struct pwent *pw, void *arg)
{
    struct home_q *q = arg;
    if (strcmp(pw->name, q->user))
        return 0;
    snprintf(q->buf, q->len, "%s", pw->home);
    return 1;
}

int user_home(const char *user, char *buf, int len)
{
    struct home_q q = { user, buf, len };
    return read_passwd(by_name, &q) ? 0 : -1;
}

struct list_q {
    const char *prefix;
    struct strv *out;
};

static int by_prefix(struct pwent *pw, void *arg)
{
    struct list_q *q = arg;
    if (!strncmp(pw->name, q->prefix, strlen(q->prefix)))
        strv_push(q->out, astrdup(pw->name));
    return 0;
}

int passwd_users(const char *prefix, struct strv *out)
{
    int before = out->n;
    struct list_q q = { prefix, out };
    read_passwd(by_prefix, &q);
    return out->n - before;
}

/* ---- colors ---- */

static const char *const color_names[] = { "black", "red", "green", "yellow", "blue",
                                           "magenta", "cyan", "white", 0 };

static void put_color(struct sb *b, const char *spec, int bg)
{
    if (!spec || !*spec || !strcmp(spec, "default")) {
        sb_printf(b, "\033[%dm", bg ? 49 : 39);
        return;
    }
    for (int i = 0; color_names[i]; i++)
        if (!strcmp(spec, color_names[i])) {
            sb_printf(b, "\033[%dm", (bg ? 40 : 30) + i);
            return;
        }
    if (spec[0] == '#' && strlen(spec) == 7) {
        unsigned v = strtol(spec + 1, 0, 16);
        sb_printf(b, "\033[%d;2;%u;%u;%um", bg ? 48 : 38, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        return;
    }
    if (is_number(spec)) {
        int n = atoi(spec);
        if (n < 8)
            sb_printf(b, "\033[%dm", (bg ? 40 : 30) + n);
        else
            sb_printf(b, "\033[%d;5;%dm", bg ? 48 : 38, n);
    }
}

/* ---- the expander ---- */

struct pctx {
    struct sb out;
    int width;
    int color;
};

static void put(struct pctx *c, const char *s)
{
    for (; *s; s++) {
        sb_putc(&c->out, *s);
        if (*s == '\n')
            c->width = 0;
        else if (((unsigned char)*s & 0xc0) != 0x80)
            c->width++;
    }
}

static void esc(struct pctx *c, const char *s)
{
    if (c->color)
        sb_puts(&c->out, s);
}

static const char *trailing(const char *path, int n)
{
    if (n <= 0)
        return path;
    const char *p = path + strlen(path);
    while (p > path) {
        p--;
        if (*p == '/' && --n == 0)
            return p + 1;
    }
    return path;
}

static void put_time(struct pctx *c, const char *fmt)
{
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
                                      "Sep", "Oct", "Nov", "Dec" };
    char buf[64];
    for (const char *f = fmt; *f; f++) {
        if (*f != '%' || !f[1]) {
            char one[2] = { *f, 0 };
            put(c, one);
            continue;
        }
        f++;
        int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        switch (*f) {
        case 'H': snprintf(buf, sizeof(buf), "%02d", tm.tm_hour); break;
        case 'k': snprintf(buf, sizeof(buf), "%2d", tm.tm_hour); break;
        case 'I': snprintf(buf, sizeof(buf), "%02d", h12); break;
        case 'l': snprintf(buf, sizeof(buf), "%2d", h12); break;
        case 'M': snprintf(buf, sizeof(buf), "%02d", tm.tm_min); break;
        case 'S': snprintf(buf, sizeof(buf), "%02d", tm.tm_sec); break;
        case 'p': snprintf(buf, sizeof(buf), "%s", tm.tm_hour < 12 ? "AM" : "PM"); break;
        case 'Y': snprintf(buf, sizeof(buf), "%d", tm.tm_year + 1900); break;
        case 'y': snprintf(buf, sizeof(buf), "%02d", tm.tm_year % 100); break;
        case 'm': snprintf(buf, sizeof(buf), "%02d", tm.tm_mon + 1); break;
        case 'd': snprintf(buf, sizeof(buf), "%02d", tm.tm_mday); break;
        case 'e': snprintf(buf, sizeof(buf), "%2d", tm.tm_mday); break;
        case 'a': snprintf(buf, sizeof(buf), "%s", wd[tm.tm_wday % 7]); break;
        case 'b': snprintf(buf, sizeof(buf), "%s", mo[tm.tm_mon % 12]); break;
        case 'Z': snprintf(buf, sizeof(buf), "UTC"); break;
        case '%': snprintf(buf, sizeof(buf), "%%"); break;
        default: snprintf(buf, sizeof(buf), "%%%c", *f); break;
        }
        put(c, buf);
    }
}

static const char *expand_into(struct pctx *c, const char *p, char stop);

/* %(x.true.false): p points after "%(" */
static const char *conditional(struct pctx *c, const char *p, int arg, int has_arg)
{
    while (is_digit(*p)) {
        arg = arg * 10 + *p++ - '0';
        has_arg = 1;
    }
    char test = *p++;
    if (!test)
        return p - 1;
    char sep = *p++;
    if (!sep)
        return p - 1;
    int truth = 0;
    switch (test) {
    case '?': truth = sh.last_status == (has_arg ? arg : 0); break;
    case '#': case '!': truth = (int)geteuid() == (has_arg ? arg : 0); break;
    case 'j': truth = jobs_count() >= (has_arg ? arg : 1); break;
    case 'L': {
        const char *l = var_get("SHLVL");
        truth = (l ? atoi(l) : 0) >= (has_arg ? arg : 1);
        break;
    }
    case 'C': case '/': case 'c': case '.': case '~': {
        char cwd[512];
        int depth = 0;
        if (getcwd(cwd, sizeof(cwd)))
            for (char *q = cwd; *q; q++)
                depth += *q == '/';
        truth = depth >= (has_arg ? arg : 1);
        break;
    }
    case 'T': case 't': {
        time_t t = time(0);
        struct tm tm;
        gmtime_r(&t, &tm);
        truth = test == 'T' ? tm.tm_hour == arg : tm.tm_min == arg;
        break;
    }
    }
    /* true part, then false part, each up to sep / ')' with nesting */
    struct pctx skip = { { 0 }, 0, 0 };
    struct pctx *t = truth ? c : &skip, *f = truth ? &skip : c;
    p = expand_into(t, p, sep);
    if (*p == sep)
        p++;
    p = expand_into(f, p, ')');
    sb_free(&skip.out);
    return p;
}

/* Expand until stop (unescaped) or end; returns the position of stop. */
static const char *expand_into(struct pctx *c, const char *p, char stop)
{
    char buf[512];
    while (*p) {
        if (stop && *p == stop)
            return p;
        if (*p != '%') {
            char one[2] = { *p++, 0 };
            put(c, one);
            continue;
        }
        p++;
        int arg = 0, has_arg = 0, neg = 0;
        if (*p == '-') {
            neg = 1;
            p++;
        }
        while (is_digit(*p)) {
            arg = arg * 10 + *p++ - '0';
            has_arg = 1;
        }
        if (neg)
            arg = -arg;
        char e = *p;
        if (!e)
            break;
        p++;
        switch (e) {
        case '%': put(c, "%"); break;
        case ')': put(c, ")"); break;
        case 'n': put(c, user_name()); break;
        case 'm': {
            const char *h = host_name();
            const char *dot = strchr(h, '.');
            snprintf(buf, sizeof(buf), "%.*s", dot ? (int)(dot - h) : (int)strlen(h), h);
            put(c, buf);
            break;
        }
        case 'M': put(c, host_name()); break;
        case '~': case '/': case 'd': case 'c': case '.': case 'C': {
            char cwd[512];
            if (!getcwd(cwd, sizeof(cwd)))
                strcpy(cwd, "?");
            const char *s = (e == '~' || e == 'c' || e == '.') ? pwd_tilde(cwd) : cwd;
            if (e == 'c' || e == '.' || e == 'C')
                s = !strcmp(s, "/") ? s : trailing(s, has_arg ? arg : 1);
            else if (has_arg && arg > 0 && strcmp(s, "/"))
                s = trailing(s, arg);
            put(c, s);
            break;
        }
        case '#': put(c, geteuid() == 0 ? "#" : "%"); break;
        case '?': snprintf(buf, sizeof(buf), "%d", sh.last_status); put(c, buf); break;
        case 'j': snprintf(buf, sizeof(buf), "%d", jobs_count()); put(c, buf); break;
        case 'L': {
            const char *l = var_get("SHLVL");
            put(c, l ? l : "1");
            break;
        }
        case 'h': case '!':
            snprintf(buf, sizeof(buf), "%d", hist_base() + hist_count());
            put(c, buf);
            break;
        case 'i': snprintf(buf, sizeof(buf), "%d", sh.lineno); put(c, buf); break;
        case 'N': put(c, sh.script_name ? sh.script_name : "esh"); break;
        case 'y': case 'l': put(c, "console"); break;
        case 'T': put_time(c, "%H:%M"); break;
        case '*': put_time(c, "%H:%M:%S"); break;
        case 't': case '@': put_time(c, "%l:%M%p"); break;
        case 'w': put_time(c, "%a %e"); break;
        case 'W': put_time(c, "%m/%d/%y"); break;
        case 'D':
            if (*p == '{') {
                const char *end = strchr(p, '}');
                if (end) {
                    put_time(c, astrndup(p + 1, end - p - 1));
                    p = end + 1;
                    break;
                }
            }
            put_time(c, "%y-%m-%d");
            break;
        case 'F': case 'K': {
            char spec[32] = "";
            if (*p == '{') {
                const char *end = strchr(p, '}');
                if (end) {
                    snprintf(spec, sizeof(spec), "%.*s", (int)(end - p - 1), p + 1);
                    p = end + 1;
                }
            } else if (has_arg) {
                snprintf(spec, sizeof(spec), "%d", arg);
            }
            if (c->color)
                put_color(&c->out, spec, e == 'K');
            break;
        }
        case 'f': esc(c, "\033[39m"); break;
        case 'k': esc(c, "\033[49m"); break;
        case 'B': esc(c, "\033[1m"); break;
        case 'b': esc(c, "\033[22m"); break;
        case 'U': esc(c, "\033[4m"); break;
        case 'u': esc(c, "\033[24m"); break;
        case 'S': esc(c, "\033[7m"); break;
        case 's': esc(c, "\033[27m"); break;
        case 'E': esc(c, "\033[K"); break;
        case '{': {
            const char *end = strstr(p, "%}");
            int n = end ? end - p : (int)strlen(p);
            if (c->color)
                sb_putn(&c->out, p, n);
            p += n + (end ? 2 : 0);
            break;
        }
        case '(':
            p = conditional(c, p, arg, has_arg);
            if (*p == ')')
                p++;
            break;
        default:
            snprintf(buf, sizeof(buf), "%%%c", e);
            put(c, buf);
        }
    }
    return p;
}

char *prompt_expand(const char *fmt, int *width, int color)
{
    struct pctx c = { { 0 }, 0, color };
    expand_into(&c, fmt ? fmt : "", 0);
    if (color && c.out.len)
        sb_puts(&c.out, "\033[0m");
    if (width)
        *width = c.width;
    char *r = sb_adup(&c.out);
    sb_free(&c.out);
    return r;
}
