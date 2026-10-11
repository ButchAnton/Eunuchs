/*
 * more, less: page through text.
 *
 * One program, two personalities chosen by argv[0]:
 *   more  prints the first page without clearing the screen, says
 *         --More--(42%), and exits after the end of the last file;
 *   less  starts on a clear screen, says ":" and waits at (END).
 * Both scroll backward as well as forward (input from a pipe is kept in
 * memory), search with / and ?, and read keys from /dev/tty, so
 * `ps aux | more` works. When the output isn't a terminal they just copy.
 *
 * Options: -N line numbers, -i ignore case in searches, -R pass colors
 * through, -s squeeze blank lines, -F quit if it fits on one screen,
 * -E quit at the end, -d friendlier prompt, -NUM lines per screen,
 * +NUM start at line NUM, +/PATTERN start at the first match.
 * Press h inside for the keys.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ---- input files ---- */

struct file {
    const char *name;           /* "-" or NULL for standard input */
    int fd;
    int eof;
    char *data;
    size_t len, cap;
    size_t *lines;              /* start offsets */
    int nlines, lcap;
    long long size;             /* regular files: st_size; else -1 */
    int top;                    /* where we were, for :n/:p */
    int last_blank;             /* -s */
};

static struct file *files;
static int nfiles, cur;

/* ---- options and state ---- */

static int is_less, opt_numbers, opt_icase, opt_raw, opt_squeeze, opt_quit_one, opt_quit_eof;
static int opt_dumb_prompt, page_lines;
static long start_line = -1;
static char start_pattern[256];
static int tty = -1;
static struct termios saved_tio;
static int tio_saved;
static int rows = 24, cols = 80;
static int top, bottom;         /* first line shown; first line not shown */
static char pattern[256];
static int search_back;
static char message[160];
static int marks[26];
static volatile int resized;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        eprintf("more: out of memory\n");
        exit(2);
    }
    return q;
}

static void add_line(struct file *f, size_t off)
{
    if (f->nlines == f->lcap) {
        f->lcap = f->lcap ? f->lcap * 2 : 256;
        f->lines = xrealloc(f->lines, f->lcap * sizeof(size_t));
    }
    f->lines[f->nlines++] = off;
}

static size_t line_end(struct file *f, int i)
{
    return i + 1 < f->nlines ? f->lines[i + 1] : f->len;
}

/* Length of line i without its newline. */
static size_t line_len(struct file *f, int i)
{
    size_t s = f->lines[i], e = line_end(f, i);
    if (e > s && f->data[e - 1] == '\n')
        e--;
    return e - s;
}

static int is_blank_line(struct file *f, size_t s, size_t e)
{
    for (size_t k = s; k < e; k++)
        if (f->data[k] != '\n' && f->data[k] != '\r' && f->data[k] != ' ' && f->data[k] != '\t')
            return 0;
    return 1;
}

/* Read until at least `want` lines are known (or the input ends). */
static void load(struct file *f, int want)
{
    while (!f->eof && (want < 0 || f->nlines <= want)) {
        if (f->len + 4096 > f->cap) {
            f->cap = f->cap ? f->cap * 2 : 16384;
            f->data = xrealloc(f->data, f->cap);
        }
        long n = read(f->fd, f->data + f->len, 4096);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            f->eof = 1;
            if (f->fd > 0)
                close(f->fd);
            break;
        }
        size_t old = f->len;
        f->len += n;
        if (f->nlines == 0)
            add_line(f, 0);
        for (size_t k = old; k < f->len; k++) {
            if (f->data[k] != '\n')
                continue;
            if (opt_squeeze) {
                /* drop a blank line that follows a blank line */
                size_t st = f->lines[f->nlines - 1];
                int blank = is_blank_line(f, st, k + 1);
                if (blank && f->last_blank) {
                    memmove(f->data + st, f->data + k + 1, f->len - (k + 1));
                    f->len -= k + 1 - st;
                    k = st - 1;                 /* rescan from st (wraps to st with k++) */
                    continue;
                }
                f->last_blank = blank;
            }
            add_line(f, k + 1);
        }
    }
    /* a final "line" that starts at the very end is not a line */
    if (f->eof && f->nlines && f->lines[f->nlines - 1] == f->len)
        f->nlines--;
}

static int open_file(int i)
{
    struct file *f = &files[i];
    if (f->fd >= 0 || f->eof)
        return 0;
    if (!f->name || !strcmp(f->name, "-")) {
        f->fd = 0;
    } else {
        f->fd = open(f->name, O_RDONLY);
        if (f->fd < 0) {
            snprintf(message, sizeof(message), "%s: %s", f->name, strerror(errno));
            f->eof = 1;
            return -1;
        }
        struct stat st;
        if (fstat(f->fd, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                snprintf(message, sizeof(message), "%s is a directory", f->name);
                close(f->fd);
                f->fd = -1;
                f->eof = 1;
                return -1;
            }
            if (S_ISREG(st.st_mode))
                f->size = st.st_size;
        }
    }
    load(f, 0);
    return 0;
}

/* ---- terminal ---- */

static void out(const char *s)
{
    printf("%s", s);
}

static void tty_restore(void)
{
    fflush_stdout();
    if (tio_saved)
        tcsetattr(tty, TCSANOW, &saved_tio);
}

static void tty_setup(void)
{
    if (tcgetattr(tty, &saved_tio) == 0)
        tio_saved = 1;
    struct termios t = saved_tio;
    t.c_lflag &= ~(ICANON | ECHO);      /* keep ISIG: ^C and ^Z work */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSANOW, &t);
}

static void get_size(void)
{
    struct winsize ws;
    if (ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    const char *e = getenv("LINES");
    if (e && atoi(e) > 2)
        rows = atoi(e);
    e = getenv("COLUMNS");
    if (e && atoi(e) > 10)
        cols = atoi(e);
    if (page_lines > 0 && page_lines < rows)
        rows = page_lines + 1;
}

static int text_rows(void)
{
    return rows - 1;
}

static void on_signal(int sig)
{
    tty_restore();
    write(1, "\r\033[K", 4);
    signal(sig, SIG_DFL);
    kill(getpid(), sig);
}

static void on_tstp(int sig)
{
    (void)sig;
    tty_restore();
    write(1, "\r\033[K", 4);
    signal(SIGTSTP, SIG_DFL);
    kill(getpid(), SIGTSTP);            /* stops here until SIGCONT */
}

static void on_cont(int sig)
{
    (void)sig;
    signal(SIGTSTP, on_tstp);
    tty_setup();
    resized = 1;                        /* redraw */
}

static int readkey(void)
{
    unsigned char c;
    for (;;) {
        long n = read(tty, &c, 1);
        if (n == 1)
            return c;
        if (n < 0 && errno == EINTR) {
            if (resized)
                return 0x0c;            /* ^L: redraw after a resume */
            continue;
        }
        return 'q';
    }
}

/* ---- matching (K&P regular expressions: c . ^ $ * and \c) ---- */

static int ceq(int a, int b)
{
    if (opt_icase) {
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
    }
    return a == b;
}

static const char *m_here(const char *re, const char *s, const char *end);

static int m_atom(const char **re, int c)
{
    const char *r = *re;
    if (*r == '\\' && r[1]) {
        *re = r + 2;
        return ceq(r[1], c);
    }
    *re = r + 1;
    return *r == '.' || ceq(*r, c);
}

static const char *m_star(const char *atom, const char *rest, const char *s, const char *end)
{
    const char *t = s;
    for (;;) {
        const char *a = atom;
        if (t >= end || !m_atom(&a, (unsigned char)*t))
            break;
        t++;
    }
    for (; t >= s; t--) {
        const char *r = m_here(rest, t, end);
        if (r)
            return r;
    }
    return 0;
}

static const char *m_here(const char *re, const char *s, const char *end)
{
    if (!*re)
        return s;
    const char *after = re + (re[0] == '\\' && re[1] ? 2 : 1);
    if (*after == '*')
        return m_star(re, after + 1, s, end);
    if (re[0] == '$' && !re[1])
        return s == end ? s : 0;
    if (s >= end)
        return 0;
    const char *r = re;
    if (!m_atom(&r, (unsigned char)*s))
        return 0;
    return m_here(r, s + 1, end);
}

/* First match in [s, end): start in *ms, end in *me. */
static int find(const char *re, const char *s, const char *end, const char **ms, const char **me)
{
    if (!*re)
        return 0;
    if (re[0] == '^') {
        const char *e = m_here(re + 1, s, end);
        if (e) {
            *ms = s;
            *me = e;
            return 1;
        }
        return 0;
    }
    for (const char *t = s; t <= end; t++) {
        const char *e = m_here(re, t, end);
        if (e && (e > t || t < end)) {
            *ms = t;
            *me = e > t ? e : t + 1;
            return 1;
        }
    }
    return 0;
}

static int line_matches(struct file *f, int i)
{
    const char *s = f->data + f->lines[i], *ms, *me;
    return find(pattern, s, s + line_len(f, i), &ms, &me);
}

/* ---- drawing ---- */

/* Rows a line takes on the screen. */
static int line_rows(struct file *f, int i)
{
    const char *s = f->data + f->lines[i];
    size_t n = line_len(f, i);
    int col = opt_numbers ? 8 : 0;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = s[k];
        if (c == '\t')
            col = (col / 8 + 1) * 8;
        else if (c == 27 && opt_raw && k + 1 < n && s[k + 1] == '[') {
            k += 2;
            while (k < n && !(s[k] >= '@' && s[k] <= '~'))
                k++;
        } else if (c < 32 || c == 127)
            col += 2;
        else if ((c & 0xc0) != 0x80)
            col++;
    }
    if (col == 0)
        return 1;
    return (col + cols - 1) / cols;
}

static void draw_line(struct file *f, int i)
{
    const char *s = f->data + f->lines[i];
    size_t n = line_len(f, i);
    /* matches to highlight */
    size_t ms_off[32], me_off[32];
    int nm = 0;
    if (*pattern) {
        const char *p = s, *ms, *me;
        while (nm < 32 && p <= s + n && find(pattern, p, s + n, &ms, &me)) {
            ms_off[nm] = ms - s;
            me_off[nm] = me - s;
            nm++;
            p = me > p ? me : p + 1;
            if (pattern[0] == '^')
                break;
        }
    }
    char buf[64];
    if (opt_numbers) {
        snprintf(buf, sizeof(buf), "%7d ", i + 1);
        out(buf);
    }
    int col = opt_numbers ? 8 : 0, inv = 0;
    for (size_t k = 0; k < n; k++) {
        int want = 0;
        for (int m = 0; m < nm; m++)
            if (k >= ms_off[m] && k < me_off[m])
                want = 1;
        if (want != inv) {
            out(want ? "\033[7m" : "\033[27m");
            inv = want;
        }
        unsigned char c = s[k];
        if (c == '\t') {
            int to = (col / 8 + 1) * 8;
            while (col < to) {
                putchar(' ');
                col++;
            }
        } else if (c == 27 && opt_raw && k + 1 < n && s[k + 1] == '[') {
            size_t e = k + 2;
            while (e < n && !(s[e] >= '@' && s[e] <= '~'))
                e++;
            if (e < n && s[e] == 'm')               /* colors only */
                for (size_t q = k; q <= e; q++)
                    putchar(s[q]);
            k = e;
        } else if (c < 32 || c == 127) {
            putchar('^');
            putchar(c == 127 ? '?' : c + 64);
            col += 2;
        } else {
            putchar(c);
            if ((c & 0xc0) != 0x80)
                col++;
        }
    }
    if (inv || opt_raw)
        out("\033[0m");
    out("\033[K\n");
}

/* The top line that ends the screen at `bot` (exclusive). */
static int top_for_bottom(struct file *f, int bot)
{
    int used = 0, t = bot;
    while (t > 0) {
        int r = line_rows(f, t - 1);
        if (used + r > text_rows())
            break;
        used += r;
        t--;
    }
    return t;
}

/* Lines from `from` that fit on a screen; loads as needed. */
static int bottom_for_top(struct file *f, int from)
{
    int used = 0, b = from;
    for (;;) {
        load(f, b);
        if (b >= f->nlines)
            break;
        int r = line_rows(f, b);
        if (used + r > text_rows() && used > 0)
            break;
        used += r;
        b++;
    }
    return b;
}

static int at_end(struct file *f)
{
    load(f, bottom);
    return f->eof && bottom >= f->nlines;
}

static void prompt(void)
{
    struct file *f = &files[cur];
    char buf[256];
    out("\r\033[K\033[7m");
    if (*message) {
        out(message);
        message[0] = 0;
    } else if (is_less) {
        if (at_end(f)) {
            if (cur + 1 < nfiles)
                snprintf(buf, sizeof(buf), "(END) - Next: %s", files[cur + 1].name);
            else
                snprintf(buf, sizeof(buf), "(END)");
            out(buf);
        } else if (top == 0 && f->name && strcmp(f->name, "-")) {
            snprintf(buf, sizeof(buf), "%s", f->name);
            out(buf);
        } else {
            out("\033[27m:");
        }
    } else {
        if (at_end(f) && cur + 1 < nfiles) {
            snprintf(buf, sizeof(buf), "--More--(Next file: %s)", files[cur + 1].name);
        } else {
            size_t pos = bottom < f->nlines ? f->lines[bottom] : f->len;
            long long total = f->size >= 0 ? f->size : f->eof ? (long long)f->len : -1;
            if (total > 0)
                snprintf(buf, sizeof(buf), "--More--(%lld%%)", (long long)pos * 100 / total);
            else
                snprintf(buf, sizeof(buf), "--More--");
        }
        out(buf);
        if (opt_dumb_prompt)
            out("\033[27m[Press space to continue, 'q' to quit.]");
    }
    out("\033[0m");
    fflush_stdout();
}

static void redraw(void)
{
    struct file *f = &files[cur];
    out("\033[H\033[2J");
    bottom = bottom_for_top(f, top);
    int used = 0;
    for (int i = top; i < bottom; i++) {
        draw_line(f, i);
        used += line_rows(f, i);
    }
    if (is_less)                        /* less marks the rows past the end */
        for (; used < text_rows(); used++)
            out("~\n");
}

/* Show the screen starting at line t (scrolling forward when we can). */
static void show_from(int t)
{
    struct file *f = &files[cur];
    load(f, t);
    if (t < 0)
        t = 0;
    if (t > 0 && t >= f->nlines)
        t = f->nlines ? f->nlines - 1 : 0;
    if (t > top && t <= bottom) {
        /* forward: print the new lines at the bottom and let the screen scroll */
        int nb = bottom_for_top(f, t);
        out("\r\033[K");
        for (int i = bottom; i < nb; i++)
            draw_line(f, i);
        top = t;
        bottom = nb;
        fflush_stdout();
        return;
    }
    top = t;
    redraw();
}

static void forward(int n)
{
    struct file *f = &files[cur];
    load(f, bottom + n);
    int nb = bottom + n;
    if (nb > f->nlines)
        nb = f->nlines;
    if (nb <= bottom)
        return;
    out("\r\033[K");
    for (int i = bottom; i < nb; i++)
        draw_line(f, i);
    bottom = nb;
    top = top_for_bottom(f, bottom);
    fflush_stdout();
}

static void backward(int n)
{
    show_from(top - n > 0 ? top - n : 0);
}

static void goto_end(void)
{
    struct file *f = &files[cur];
    load(f, -1);
    int t = top_for_bottom(f, f->nlines);
    if (t != top || bottom != f->nlines) {
        top = t;
        redraw();
    }
}

/* ---- commands ---- */

static void search(int dir, int skip_current)
{
    struct file *f = &files[cur];
    if (!*pattern) {
        snprintf(message, sizeof(message), "No previous regular expression");
        return;
    }
    int i = top + (skip_current ? dir : 0);
    for (;;) {
        if (i < 0)
            break;
        load(f, i);
        if (i >= f->nlines)
            break;
        if (line_matches(f, i)) {
            top = i;
            redraw();
            return;
        }
        i += dir;
    }
    snprintf(message, sizeof(message), "Pattern not found");
}

/* Read a line at the bottom after a prompt character; returns 0 if cancelled. */
static int read_line(const char *lead, char *buf, int size)
{
    int n = 0;
    buf[0] = 0;
    out("\r\033[K");
    out(lead);
    fflush_stdout();
    for (;;) {
        int c = readkey();
        if (c == '\r' || c == '\n')
            break;
        if (c == 27 || c == 3 || c == 7) {
            out("\r\033[K");
            return 0;
        }
        if (c == 127 || c == 8) {
            if (!n) {
                out("\r\033[K");
                return 0;
            }
            n--;
            out("\b \b");
        } else if (c == 21) {
            while (n) {
                n--;
                out("\b \b");
            }
        } else if (c >= 32 && n < size - 1) {
            buf[n++] = c;
            putchar(c);
        }
        fflush_stdout();
    }
    buf[n] = 0;
    return 1;
}

static void help(void)
{
    static const char *const lines[] = {
        "  Keys (N is an optional count typed first)",
        "",
        "  SPACE f ^F PgDn   forward a screen (or N lines)",
        "  RETURN j ^N Down  forward N lines (default 1)",
        "  d ^D / u ^U       forward / back half a screen",
        "  b ^B PgUp         back a screen     k y ^P Up   back N lines",
        "  g < Home          line N (default 1)  G > End  the end, or line N",
        "  p %               N percent of the way     s   skip N lines",
        "  /pat  ?pat        search forward / back    n N  next / other way",
        "                    (pat: c . ^ $ * and \\c; -i ignores case)",
        "  m<a-z>  '<a-z>    mark this place / go back to it",
        "  :n :p             next / previous file    :f = ^G  where am I",
        "  !command          run a shell command     r ^L     redraw",
        "  -N -i -R          toggle line numbers, ignoring case, colors",
        "  q Q :q ZZ         quit                    h        this help",
        "",
        "  Press any key to go back.",
        0,
    };
    out("\033[H\033[2J");
    for (int i = 0; lines[i]; i++) {
        out(lines[i]);
        out("\n");
    }
    fflush_stdout();
    readkey();
    redraw();
}

static void where(void)
{
    struct file *f = &files[cur];
    char total[32] = "", size_s[32] = "";
    if (f->eof)
        snprintf(total, sizeof(total), " of %d", f->nlines);
    long long size = f->size >= 0 ? f->size : f->eof ? (long long)f->len : -1;
    if (size >= 0)
        snprintf(size_s, sizeof(size_s), "/%lld", size);
    size_t pos = bottom < f->nlines ? f->lines[bottom] : f->len;
    snprintf(message, sizeof(message), "%s%s lines %d-%d%s byte %lu%s",
             f->name && strcmp(f->name, "-") ? f->name : "standard input",
             nfiles > 1 ? (cur + 1 < nfiles ? " (file not last)" : " (last file)") : "",
             top + 1, bottom, total, (unsigned long)pos, size_s);
}

static void shell_escape(void)
{
    char cmd[256];
    if (!read_line("!", cmd, sizeof(cmd)))
        return;
    out("\n");
    tty_restore();
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *argv[] = { "sh", "-c", cmd, 0 };
        execv("/bin/sh", argv);
        _exit(127);
    }
    signal(SIGINT, SIG_IGN);
    int st;
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    signal(SIGINT, on_signal);
    tty_setup();
    out("\033[7m!done (press RETURN)\033[0m");
    fflush_stdout();
    readkey();
    redraw();
}

static int switch_file(int to)
{
    if (to < 0 || to >= nfiles) {
        snprintf(message, sizeof(message), to < 0 ? "No previous file" : "No next file");
        return -1;
    }
    files[cur].top = top;
    cur = to;
    open_file(cur);
    top = files[cur].top;
    bottom = top;
    if (!is_less && files[cur].top == 0) {
        /* more: a header, then the new file scrolls in */
        char buf[300];
        snprintf(buf, sizeof(buf), "\r\033[K::::::::::::::\n%s\n::::::::::::::\n", files[cur].name);
        out(buf);
        load(&files[cur], text_rows());
        int used = 3;
        int b = 0;
        while (b < files[cur].nlines && used + line_rows(&files[cur], b) <= text_rows()) {
            used += line_rows(&files[cur], b);
            b++;
        }
        for (int i = 0; i < b; i++)
            draw_line(&files[cur], i);
        bottom = b;
        top = 0;
        return 0;
    }
    redraw();
    return 0;
}

/* ---- copying (output isn't a terminal) ---- */

static int copy_all(void)
{
    int rc = 0;
    char buf[4096];
    for (int i = 0; i < nfiles; i++) {
        int fd = 0;
        if (files[i].name && strcmp(files[i].name, "-")) {
            fd = open(files[i].name, O_RDONLY);
            if (fd < 0) {
                eprintf("%s: %s: %s\n", is_less ? "less" : "more", files[i].name, strerror(errno));
                rc = 1;
                continue;
            }
        }
        if (nfiles > 1 && !is_less) {
            printf("::::::::::::::\n%s\n::::::::::::::\n", files[i].name);
            fflush_stdout();
        }
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        if (fd > 0)
            close(fd);
    }
    return rc;
}

/* ---- main ---- */

static void usage(void)
{
    eprintf("usage: %s [-NiRsFEd] [-NUM] [+NUM | +/PATTERN] [file ...]\n", is_less ? "less" : "more");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    is_less = !strcmp(base, "less");
    files = xrealloc(0, (argc + 1) * sizeof(*files));
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '+' && a[1]) {
            if (a[1] == '/')
                snprintf(start_pattern, sizeof(start_pattern), "%s", a + 2);
            else
                start_line = atoi(a + 1);
            continue;
        }
        if (a[0] == '-' && a[1] && strcmp(a, "-")) {
            if (!strcmp(a, "--"))
                continue;
            if (!strcmp(a, "--help"))
                usage();
            if (a[1] >= '0' && a[1] <= '9') {
                page_lines = atoi(a + 1);
                continue;
            }
            for (const char *c = a + 1; *c; c++) {
                switch (*c) {
                case 'N': opt_numbers = 1; break;
                case 'i': case 'I': opt_icase = 1; break;
                case 'R': case 'r': opt_raw = 1; break;
                case 's': opt_squeeze = 1; break;
                case 'F': opt_quit_one = 1; break;
                case 'E': case 'e': opt_quit_eof = 1; break;
                case 'd': opt_dumb_prompt = 1; break;
                case 'X': case 'c': case 'p': case 'f': case 'u': case 'l': case 'S': case 'K':
                    break;                      /* accepted for compatibility */
                default:
                    eprintf("%s: unknown option -%c\n", is_less ? "less" : "more", *c);
                    usage();
                }
            }
            continue;
        }
        struct file *f = &files[nfiles++];
        memset(f, 0, sizeof(*f));
        f->name = a;
        f->fd = -1;
        f->size = -1;
    }
    if (!nfiles) {
        if (isatty(0)) {
            eprintf("%s: no input (usage: %s file ..., or cmd | %s)\n", base, base, base);
            return 2;
        }
        memset(&files[0], 0, sizeof(files[0]));
        files[0].name = "-";
        files[0].fd = -1;
        files[0].size = -1;
        nfiles = 1;
    }
    if (!isatty(1))
        return copy_all();

    tty = open("/dev/tty", O_RDWR);
    if (tty < 0 || !isatty(tty))
        tty = isatty(2) ? 2 : -1;
    if (tty < 0)
        return copy_all();
    get_size();
    tty_setup();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGTSTP, on_tstp);
    signal(SIGCONT, on_cont);
    for (int i = 0; i < 26; i++)
        marks[i] = -1;

    /* first readable file */
    while (cur < nfiles && open_file(cur) < 0 && files[cur].eof && !files[cur].len) {
        eprintf("%s: %s\n", base, message);
        message[0] = 0;
        cur++;
    }
    if (cur >= nfiles) {
        tty_restore();
        return 1;
    }
    struct file *f = &files[cur];
    if (*start_pattern) {
        snprintf(pattern, sizeof(pattern), "%s", start_pattern);
        int i = 0;
        for (;; i++) {
            load(f, i);
            if (i >= f->nlines || line_matches(f, i))
                break;
        }
        top = i < f->nlines ? i : 0;
    } else if (start_line > 0) {
        load(f, start_line - 1);
        top = start_line - 1 < f->nlines ? start_line - 1 : 0;
    }

    /* first screen */
    if (nfiles > 1 && !is_less) {
        char buf[300];
        snprintf(buf, sizeof(buf), "::::::::::::::\n%s\n::::::::::::::\n", f->name);
        out(buf);
    }
    bottom = bottom_for_top(f, top);
    if ((opt_quit_one || !is_less) && nfiles == 1 && top == 0 && at_end(f)) {
        /* it all fits: just show it */
        for (int i = top; i < bottom; i++)
            draw_line(f, i);
        tty_restore();
        return 0;
    }
    if (is_less) {
        redraw();
    } else {
        for (int i = top; i < bottom; i++)
            draw_line(f, i);
    }

    int count = -1;
    char pending_colon = 0;
    for (;;) {
        f = &files[cur];
        if (resized) {
            resized = 0;
            get_size();
            redraw();
        }
        if (at_end(f) && (opt_quit_eof || !is_less) && cur + 1 >= nfiles && !*message)
            break;
        prompt();
        int c = readkey();
        if (c >= '0' && c <= '9') {
            count = (count < 0 ? 0 : count * 10) + c - '0';
            char buf[32];
            snprintf(buf, sizeof(buf), "\r\033[K:%d", count);
            out(buf);
            fflush_stdout();
            continue;
        }
        int n = count;
        count = -1;
        if (c == 27) {                          /* escape sequences: arrows and friends */
            int d = readkey();
            if (d == '[' || d == 'O') {
                int e = readkey(), p = 0;
                while (e >= '0' && e <= '9') {
                    p = p * 10 + e - '0';
                    e = readkey();
                }
                c = e == 'A' ? 'k' : e == 'B' ? 'j' : e == 'H' ? 'g' : e == 'F' ? 'G'
                  : e == '~' && (p == 5) ? 'b' : e == '~' && p == 6 ? ' '
                  : e == '~' && (p == 1 || p == 7) ? 'g' : e == '~' && (p == 4 || p == 8) ? 'G' : 0;
            } else {
                c = d == '<' ? 'g' : d == '>' ? 'G' : d == 'v' ? 'b' : 0;
            }
            if (!c)
                continue;
        }
        if (pending_colon) {
            pending_colon = 0;
            switch (c) {
            case 'n': switch_file(cur + (n > 0 ? n : 1)); break;
            case 'p': switch_file(cur - (n > 0 ? n : 1)); break;
            case 'f': where(); break;
            case 'q': case 'Q': goto quit;
            default: break;
            }
            continue;
        }
        int half = text_rows() / 2;
        switch (c) {
        case ' ': case 'f': case 6: case 'z':
            if (at_end(f) && cur + 1 < nfiles) {
                switch_file(cur + 1);
                break;
            }
            forward(n > 0 ? n : text_rows());
            break;
        case '\r': case '\n': case 'j': case 14: case 'e': case 5:
            if (at_end(f) && cur + 1 < nfiles && !is_less) {
                switch_file(cur + 1);
                break;
            }
            forward(n > 0 ? n : 1);
            break;
        case 'd': case 4:
            if (n > 0)
                half = n;
            forward(half);
            break;
        case 's':
            show_from(bottom + (n > 0 ? n : 1));
            break;
        case 'b': case 2: case 'w':
            backward(n > 0 ? n : text_rows());
            break;
        case 'k': case 'y': case 16: case 25: case 11:
            backward(n > 0 ? n : 1);
            break;
        case 'u': case 21:
            if (n > 0)
                half = n;
            backward(half);
            break;
        case 'g': case '<':
            show_from(n > 0 ? n - 1 : 0);
            break;
        case 'G': case '>':
            if (n > 0)
                show_from(n - 1);
            else
                goto_end();
            break;
        case 'p': case '%': {
            load(f, -1);
            int pct = n > 0 ? (n > 100 ? 100 : n) : 0;
            show_from((int)((long)f->nlines * pct / 100));
            break;
        }
        case '/': case '?': {
            char buf[256];
            if (!read_line(c == '/' ? "/" : "?", buf, sizeof(buf)))
                break;
            if (buf[0])
                snprintf(pattern, sizeof(pattern), "%s", buf);
            search_back = c == '?';
            for (int k = 0; k < (n > 0 ? n : 1); k++)
                search(search_back ? -1 : 1, 1);
            break;
        }
        case 'n': case 'N': {
            int back = (c == 'N') != search_back;
            for (int k = 0; k < (n > 0 ? n : 1); k++)
                search(back ? -1 : 1, 1);
            break;
        }
        case 'm': {
            int l = readkey();
            if (l >= 'a' && l <= 'z')
                marks[l - 'a'] = top;
            break;
        }
        case '\'': {
            int l = readkey();
            if (l == '\'')
                show_from(0);
            else if (l >= 'a' && l <= 'z' && marks[l - 'a'] >= 0)
                show_from(marks[l - 'a']);
            else
                snprintf(message, sizeof(message), "Mark not set");
            break;
        }
        case ':':
            pending_colon = 1;
            out("\r\033[K:");
            fflush_stdout();
            break;
        case '=': case 7:
            where();
            break;
        case '!':
            shell_escape();
            break;
        case 'r': case 12: case 18:
            get_size();
            redraw();
            break;
        case 'h': case 'H':
            help();
            break;
        case '-': {
            int o = readkey();
            int *flag = o == 'N' ? &opt_numbers : o == 'i' || o == 'I' ? &opt_icase
                      : o == 'R' ? &opt_raw : 0;
            if (flag) {
                *flag = !*flag;
                snprintf(message, sizeof(message), "%s %s",
                         o == 'N' ? "Line numbers" : o == 'R' ? "Colors" : "Ignore case",
                         *flag ? "on" : "off");
                redraw();
            }
            break;
        }
        case 'v':
            snprintf(message, sizeof(message), "There is no editor (try !cmd)");
            break;
        case 'Z':
            if (readkey() == 'Z')
                goto quit;
            break;
        case 'q': case 'Q': case 3:
            goto quit;
        default:
            if (!is_less)
                snprintf(message, sizeof(message), "Press 'h' for instructions.");
            break;
        }
    }
quit:
    out("\r\033[K");
    tty_restore();
    return 0;
}
