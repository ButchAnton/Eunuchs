/*
 * esh: the line editor (in the spirit of zsh's ZLE).
 *
 * Emacs key bindings, multi-line buffers, syntax highlighting, gray
 * autosuggestions from history, prefix history search on Up/Down,
 * incremental search, a kill ring, undo, and completion with listings and
 * a selectable menu.
 *
 * Rendering: the prompt is printed once; every refresh moves the cursor to
 * where the buffer starts, rewrites the buffer (and anything shown below
 * it, like completion listings), clears the rest of the screen, and puts
 * the cursor back. Positions are computed the way the terminal wraps:
 * one column per character, wrapping when a character is placed past the
 * last column.
 */
#include "esh.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

int zle_active;
int zle_cmd_ran;

enum {
    K_UP = 0x1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_INS, K_PGUP, K_PGDN,
    K_CLEFT, K_CRIGHT, K_STAB, K_PASTE, K_ESC, K_INTR, K_EOF,
    K_ALT = 0x2000,
};
#define CTRL(c) ((c) & 31)

static struct termios orig_modes;
static int have_orig;

/* ---- terminal modes ---- */

void tty_raw(void)
{
    struct termios t;
    if (tcgetattr(0, &t) < 0)
        return;
    orig_modes = t;
    have_orig = 1;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(ICRNL | IXON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSADRAIN, &t);
}

void tty_cooked(void)
{
    if (have_orig)
        tcsetattr(0, TCSADRAIN, &orig_modes);
}

/* ---- input ---- */

static unsigned char ahead[512];
static int nahead;

static void push_ahead(const unsigned char *s, int n)
{
    if (nahead + n > (int)sizeof(ahead))
        n = sizeof(ahead) - nahead;
    memcpy(ahead + nahead, s, n);
    nahead += n;
}

static int input_pending(void)
{
    if (nahead)
        return 1;
    struct pollfd p = { .fd = 0, .events = POLLIN };
    return poll(&p, 1, 0) > 0;
}

/* A byte, -1 at EOF, -2 if a signal interrupted, -3 on timeout. */
static int readbyte(int timeout_ms)
{
    if (nahead) {
        int c = ahead[0];
        memmove(ahead, ahead + 1, --nahead);
        return c;
    }
    if (timeout_ms >= 0) {
        struct pollfd p = { .fd = 0, .events = POLLIN };
        int r = poll(&p, 1, timeout_ms);
        if (r < 0)
            return errno == EINTR ? -2 : -1;
        if (r == 0)
            return -3;
    }
    unsigned char buf[256];
    long n = read(0, buf, sizeof(buf));
    if (n < 0)
        return errno == EINTR ? -2 : -1;
    if (n == 0)
        return -1;
    if (n > 1)
        push_ahead(buf + 1, n - 1);
    return buf[0];
}

/* Read a key, decoding escape sequences. */
static int readkey(void)
{
    int c = readbyte(-1);
    if (c < 0)
        return c == -2 ? K_INTR : K_EOF;
    if (c != 27)
        return c;
    int d = readbyte(60);
    if (d == -3 || d < 0)
        return K_ESC;
    if (d == '[' || d == 'O') {
        char params[16];
        int n = 0, f;
        for (;;) {
            f = readbyte(60);
            if (f < 0)
                return K_ESC;
            if (f >= 0x40 && f <= 0x7e)
                break;
            if (n < 15)
                params[n++] = f;
        }
        params[n] = 0;
        int p1 = atoi(params), mod = 0;
        char *semi = strchr(params, ';');
        if (semi)
            mod = atoi(semi + 1);
        int ctrl = mod == 5, alt = mod == 3;
        switch (f) {
        case 'A': return alt ? K_ALT | K_UP : K_UP;
        case 'B': return alt ? K_ALT | K_DOWN : K_DOWN;
        case 'C': return ctrl || alt ? K_CRIGHT : K_RIGHT;
        case 'D': return ctrl || alt ? K_CLEFT : K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'Z': return K_STAB;
        case 'R': return -4;            /* a stray cursor position report */
        case '~':
            switch (p1) {
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 2: return K_INS;
            case 3: return K_DEL;
            case 5: return K_PGUP;
            case 6: return K_PGDN;
            case 200: return K_PASTE;
            }
        }
        return -4;
    }
    if (d == 127 || d == 8)
        return K_ALT | 127;
    return K_ALT | d;
}

/* ---- terminal queries ---- */

/* ESC[6n -> ESC[row;colR; other input is kept for later. */
static int query_pos(int *row, int *col, int timeout)
{
    write(1, "\033[6n", 4);
    long long end = now_ms() + timeout;
    unsigned char got[64];
    int n = 0;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0)
            break;
        int c = -3;
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (poll(&p, 1, left) > 0) {
            unsigned char b;
            if (read(0, &b, 1) == 1)
                c = b;
        }
        if (c < 0)
            continue;
        if (n < (int)sizeof(got))
            got[n++] = c;
        if (c == 'R') {
            /* find ESC [ r ; c R at the end */
            int i = n - 1;
            while (i > 0 && got[i] != 27)
                i--;
            if (got[i] == 27 && i + 1 < n && got[i + 1] == '[') {
                int r = 0, k = 0, j = i + 2;
                while (j < n && is_digit(got[j]))
                    r = r * 10 + got[j++] - '0';
                if (j < n && got[j] == ';') {
                    j++;
                    while (j < n && is_digit(got[j]))
                        k = k * 10 + got[j++] - '0';
                    push_ahead(got, i);
                    *row = r;
                    *col = k;
                    return 0;
                }
            }
        }
    }
    push_ahead(got, n);
    return -1;
}

void term_query_size(void)
{
    static int tried;
    if (tried && !sh.term_ok)
        return;
    int r, c;
    const char *save_and_go = "\0337\033[999;999H";
    write(1, save_and_go, strlen(save_and_go));
    int ok = query_pos(&r, &c, tried ? 250 : 400);
    write(1, "\0338", 2);
    tried = 1;
    if (ok < 0 || r < 2 || c < 10) {
        if (!sh.term_ok) {
            struct winsize ws;
            if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
                sh.cols = ws.ws_col;
                sh.rows = ws.ws_row;
            }
        }
        return;
    }
    sh.term_ok = 1;
    if (r != sh.rows || c != sh.cols) {
        sh.rows = r;
        sh.cols = c;
        struct winsize ws = { (uint16_t)r, (uint16_t)c, 0, 0 };
        ioctl(0, TIOCSWINSZ, &ws);
    }
}

/* ---- state ---- */

#define MAXUNDO 100

static struct {
    struct sb buf;
    int pos;
    const char *prompt;
    int pw, prompt_rows;
    const char *rprompt;
    int rpw;
    int cur_row;                /* cursor row, relative to the prompt's last row */
    int done, eof;
    /* history */
    int hidx;
    char *saved;                /* the line being edited before moving into history */
    char *prefix;               /* Up/Down prefix search */
    /* suggestion */
    char *suggest;
    /* the area below the buffer: completion listings, search status */
    struct sb below;
    int below_rows;
    /* completion */
    struct cands cands;
    int menu, menu_idx, menu_top;
    int listed;                 /* a listing is showing for the current word */
    char *menu_orig;
    int menu_start, menu_len;
    /* incremental search */
    int isearch, isearch_dir, isearch_idx, isearch_fail;
    struct sb query;
    char *isearch_saved;
    int isearch_saved_pos;
    int match_start, match_len;
    /* undo */
    struct {
        char *s;
        int pos;
    } undo[MAXUNDO];
    int nundo;
    /* insert-last-word */
    int lw_back, lw_start, lw_len;
    /* what the last command was, for coalescing and repeats */
    int last, cur;
} z;

enum { C_OTHER, C_INSERT, C_KILL, C_YANK, C_HIST, C_LASTWORD, C_COMPLETE, C_UNDO };

static char *kill_ring[8];
static int nkill, yank_idx, yank_start, yank_len;
static char *pushed_line;       /* Alt-Q */

static int cols(void)
{
    return sh.cols > 10 ? sh.cols : 80;
}

static void beep(void)
{
    if (opts[OPT_BEEP])
        write(1, "\a", 1);
}

/* ---- rendering ---- */

struct draw {
    struct sb out;
    int row, col;
    int attr;
};

static void d_char(struct draw *d, const char *s, int n)
{
    if (d->col >= cols()) {
        d->row++;
        d->col = 0;
    }
    sb_putn(&d->out, s, n);
    d->col++;
}

static void d_sgr(struct draw *d, int attr)
{
    if (attr == d->attr)
        return;
    d->attr = attr;
    if (attr == 255)
        sb_puts(&d->out, "\033[0;7m");
    else if (attr == 254)
        sb_puts(&d->out, "\033[0;90m");
    else
        sb_puts(&d->out, highlight_sgr(attr));
}

/* Draw text (one column per character; control characters as ^X). */
static void d_text(struct draw *d, const char *s, int len, const unsigned char *attr,
                   int cursor, int *crow, int *ccol)
{
    for (int i = 0; i < len;) {
        if (i == cursor) {
            *crow = d->row + (d->col >= cols());
            *ccol = d->col >= cols() ? 0 : d->col;
        }
        if (attr)
            d_sgr(d, attr[i]);
        unsigned char c = s[i];
        if (c == '\n') {
            sb_puts(&d->out, "\033[K\n");
            d->row++;
            d->col = 0;
            i++;
            continue;
        }
        if (c < 32 || c == 127) {
            char t[2] = { '^', c == 127 ? '?' : c + 64 };
            d_char(d, t, 1);
            d_char(d, t + 1, 1);
            i++;
            continue;
        }
        int n = utf8_next(s, len, i) - i;
        d_char(d, s + i, n);
        i += n;
    }
    if (cursor == len) {
        *crow = d->row + (d->col >= cols());
        *ccol = d->col >= cols() ? 0 : d->col;
    }
}

static void refresh_full(int show_extras)
{
    struct amark mark = amark();    /* everything below is temporary */
    struct draw d = { { 0 }, 0, z.pw % cols(), -1 };
    out_flush();
    /* back to where the buffer starts */
    if (z.cur_row > 0)
        sb_printf(&d.out, "\033[%dA", z.cur_row);
    sb_putc(&d.out, '\r');
    if (d.col > 0)
        sb_printf(&d.out, "\033[%dC", d.col);

    int len = z.buf.len;
    unsigned char *attr = amalloc(len + 1);
    memset(attr, 0, len + 1);
    if (opts[OPT_HIGHLIGHT] && len < 4000)
        highlight(z.buf.s ? z.buf.s : "", len, attr);
    if (z.isearch && !z.isearch_fail && z.match_len)
        for (int i = z.match_start; i < z.match_start + z.match_len && i < len; i++)
            attr[i] = 255;
    int crow = 0, ccol = 0;
    d_text(&d, z.buf.s ? z.buf.s : "", len, attr, z.pos, &crow, &ccol);
    d_sgr(&d, 0);
    if (d.col >= cols() && z.pos == len) {
        /* exactly at the margin: make the terminal move to the next row too */
        sb_putc(&d.out, '\n');
        d.row++;
        d.col = 0;
    }
    if (show_extras && z.suggest && *z.suggest && z.pos == len) {
        int r2, c2;
        d_sgr(&d, 254);
        d_text(&d, z.suggest, strlen(z.suggest), 0, -1, &r2, &c2);
        d_sgr(&d, 0);
    }
    sb_puts(&d.out, "\033[0m\033[K");
    d.attr = 0;
    /* right prompt, if everything is on one row with room to spare */
    if (show_extras && sh.term_ok && z.rprompt && z.rpw && d.row == 0 && d.col + z.rpw + 2 < cols()) {
        int at = cols() - z.rpw - 1;
        sb_printf(&d.out, "\r\033[%dC%s", at, z.rprompt);
        d.col = at + z.rpw;
    }
    int end_row = d.row;
    if (show_extras && z.below.len) {
        sb_puts(&d.out, "\n");
        sb_puts(&d.out, z.below.s);
        sb_puts(&d.out, "\033[0m\033[K");
        end_row += z.below_rows;
    }
    sb_puts(&d.out, "\033[J");
    /* cursor */
    if (end_row > crow)
        sb_printf(&d.out, "\033[%dA", end_row - crow);
    sb_putc(&d.out, '\r');
    if (ccol > 0)
        sb_printf(&d.out, "\033[%dC", ccol);
    write(1, d.out.s, d.out.len);
    sb_free(&d.out);
    z.cur_row = crow;
    arelease(mark);
}

static void refresh(void)
{
    refresh_full(1);
}

static void osc(const char *code)
{
    if (sh.term_ok && opts[OPT_PROMPTMARKS]) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "\033]133;%s\a", code);
        write(1, buf, n);
    }
}

static void print_prompt(void)
{
    out_flush();
    osc("A");
    write(1, z.prompt, strlen(z.prompt));
    osc("B");
    z.cur_row = 0;
}

/* Erase the prompt and buffer, run fn (which prints), redraw below it. */
void zle_print_above(const char *s)
{
    if (!zle_active) {
        eprintf("%s", s);
        return;
    }
    struct sb b = { 0 };
    int up = z.cur_row + z.prompt_rows;
    if (up > 0)
        sb_printf(&b, "\033[%dA", up);
    sb_puts(&b, "\r\033[J");
    write(1, b.s, b.len);
    sb_free(&b);
    write(2, s, strlen(s));
    print_prompt();
    refresh();
}

static void notify_above(void)
{
    jobs_reap(0);
    if (!jobs_pending_report())
        return;
    int up = z.cur_row + z.prompt_rows;
    struct sb b = { 0 };
    if (up > 0)
        sb_printf(&b, "\033[%dA", up);
    sb_puts(&b, "\r\033[J");
    write(1, b.s, b.len);
    sb_free(&b);
    tty_cooked();
    jobs_reap(1);
    tty_raw();
    print_prompt();
    refresh();
}

/* ---- buffer editing ---- */

static void save_undo(void)
{
    if (z.nundo == MAXUNDO) {
        free(z.undo[0].s);
        memmove(z.undo, z.undo + 1, (MAXUNDO - 1) * sizeof(z.undo[0]));
        z.nundo--;
    }
    z.undo[z.nundo].s = xstrdup(sb_str(&z.buf));
    z.undo[z.nundo].pos = z.pos;
    z.nundo++;
}

static void set_buf(const char *s, int pos)
{
    sb_reset(&z.buf);
    sb_puts(&z.buf, s);
    z.pos = pos < 0 || pos > z.buf.len ? z.buf.len : pos;
}

static void insert(const char *s, int n)
{
    int len = z.buf.len;
    sb_putn(&z.buf, s, n);          /* grow */
    memmove(z.buf.s + z.pos + n, z.buf.s + z.pos, len - z.pos);
    memcpy(z.buf.s + z.pos, s, n);
    z.pos += n;
    z.buf.s[z.buf.len] = 0;
}

static void delete_range(int a, int b)
{
    if (a < 0)
        a = 0;
    if (b > z.buf.len)
        b = z.buf.len;
    if (a >= b)
        return;
    memmove(z.buf.s + a, z.buf.s + b, z.buf.len - b);
    z.buf.len -= b - a;
    z.buf.s[z.buf.len] = 0;
    if (z.pos > b)
        z.pos -= b - a;
    else if (z.pos > a)
        z.pos = a;
}

static void kill_text(int a, int b, int backward)
{
    if (a >= b)
        return;
    char *t = xstrndup(z.buf.s + a, b - a);
    if (z.last == C_KILL && nkill) {
        struct sb s = { 0 };
        if (backward) {
            sb_puts(&s, t);
            sb_puts(&s, kill_ring[0]);
        } else {
            sb_puts(&s, kill_ring[0]);
            sb_puts(&s, t);
        }
        free(kill_ring[0]);
        kill_ring[0] = xstrdup(sb_str(&s));
        sb_free(&s);
        free(t);
    } else {
        free(kill_ring[7]);
        memmove(kill_ring + 1, kill_ring, 7 * sizeof(char *));
        kill_ring[0] = t;
        if (nkill < 8)
            nkill++;
    }
    delete_range(a, b);
    z.cur = C_KILL;
}

static int is_word_char(int c)
{
    if (is_alnum(c) || c >= 0x80)
        return 1;
    const char *wc = var_get("WORDCHARS");
    if (!wc)
        wc = "*?_-.[]~=&;!#$%^(){}<>";
    return c && strchr(wc, c) != 0;
}

static int word_left(int p)
{
    while (p > 0 && !is_word_char((unsigned char)z.buf.s[p - 1]))
        p--;
    while (p > 0 && is_word_char((unsigned char)z.buf.s[p - 1]))
        p--;
    return p;
}

static int word_right(int p)
{
    while (p < z.buf.len && !is_word_char((unsigned char)z.buf.s[p]))
        p++;
    while (p < z.buf.len && is_word_char((unsigned char)z.buf.s[p]))
        p++;
    return p;
}

static int line_start(int p)
{
    while (p > 0 && z.buf.s[p - 1] != '\n')
        p--;
    return p;
}

static int line_end(int p)
{
    while (p < z.buf.len && z.buf.s[p] != '\n')
        p++;
    return p;
}

/* ---- suggestions ---- */

static void update_suggestion(void)
{
    free(z.suggest);
    z.suggest = 0;
    if (!opts[OPT_AUTOSUGGEST] || z.isearch || z.menu || !z.buf.len || z.pos != z.buf.len)
        return;
    const char *b = sb_str(&z.buf);
    for (int i = hist_count() - 1; i >= 0; i--) {
        const char *h = hist_get(i);
        if (!strncmp(h, b, z.buf.len) && h[z.buf.len]) {
            z.suggest = xstrdup(h + z.buf.len);
            return;
        }
    }
}

static int accept_suggestion(int word_only)
{
    if (!z.suggest || z.pos != z.buf.len)
        return 0;
    const char *s = z.suggest;
    int n = strlen(s);
    if (word_only) {
        int k = 0;
        while (k < n && !is_word_char((unsigned char)s[k]))
            k++;
        while (k < n && is_word_char((unsigned char)s[k]))
            k++;
        n = k;
    }
    save_undo();
    insert(s, n);
    return 1;
}

/* ---- history movement ---- */

static void hist_goto(int idx)
{
    int n = hist_count();
    if (z.hidx == n) {
        free(z.saved);
        z.saved = xstrdup(sb_str(&z.buf));
    }
    z.hidx = idx;
    if (idx >= n)
        set_buf(z.saved ? z.saved : "", -1);
    else
        set_buf(hist_get(idx), -1);
}

static void history_prefix(int dir)
{
    if (z.last != C_HIST) {
        free(z.prefix);
        z.prefix = xstrndup(sb_str(&z.buf), z.pos);
    }
    z.cur = C_HIST;
    int n = hist_count();
    int plen = strlen(z.prefix);
    const char *current = sb_str(&z.buf);
    for (int i = z.hidx + dir; i >= 0 && i <= n; i += dir) {
        if (i == n) {
            hist_goto(n);
            return;
        }
        const char *h = hist_get(i);
        if (!strncmp(h, z.prefix, plen) && strcmp(h, current)) {
            hist_goto(i);
            return;
        }
    }
    beep();
}

static void history_step(int dir)
{
    int n = hist_count();
    int idx = z.hidx + dir;
    if (idx < 0 || idx > n) {
        beep();
        return;
    }
    z.cur = C_HIST;
    hist_goto(idx);
}

/* ---- incremental search ---- */

static void isearch_status(void)
{
    sb_reset(&z.below);
    sb_printf(&z.below, "%s%s-i-search: %s_", z.isearch_fail ? "failing " : "",
              z.isearch_dir < 0 ? "bck" : "fwd", sb_str(&z.query));
    z.below_rows = 1;
}

static void isearch_find(int from, int dir)
{
    const char *q = sb_str(&z.query);
    int n = hist_count();
    if (!*q) {
        z.isearch_fail = 0;
        z.match_len = 0;
        isearch_status();
        return;
    }
    for (int i = from; i >= 0 && i < n; i += dir) {
        const char *h = hist_get(i);
        const char *at = strstr(h, q);
        if (at) {
            z.isearch_idx = i;
            z.hidx = i;
            set_buf(h, at - h);
            z.match_start = at - h;
            z.match_len = strlen(q);
            z.isearch_fail = 0;
            isearch_status();
            return;
        }
    }
    z.isearch_fail = 1;
    isearch_status();
    beep();
}

static void isearch_start(int dir)
{
    z.isearch = 1;
    z.isearch_dir = dir;
    sb_reset(&z.query);
    free(z.isearch_saved);
    z.isearch_saved = xstrdup(sb_str(&z.buf));
    z.isearch_saved_pos = z.pos;
    z.isearch_idx = hist_count();
    z.isearch_fail = 0;
    z.match_len = 0;
    if (z.hidx == hist_count()) {
        free(z.saved);
        z.saved = xstrdup(sb_str(&z.buf));
    }
    isearch_status();
}

static void isearch_end(void)
{
    z.isearch = 0;
    z.match_len = 0;
    sb_reset(&z.below);
    z.below_rows = 0;
}

/* Returns 1 if the key was consumed by the search. */
static int isearch_key(int k)
{
    if (k == CTRL('R') || k == CTRL('S')) {
        int dir = k == CTRL('R') ? -1 : 1;
        z.isearch_dir = dir;
        isearch_find(z.isearch_idx + dir, dir);
        return 1;
    }
    if (k == 127 || k == CTRL('H')) {
        if (z.query.len) {
            z.query.len = utf8_prev(z.query.s, z.query.len);
            z.query.s[z.query.len] = 0;
        }
        isearch_find(hist_count() - 1, -1);
        return 1;
    }
    if (k == CTRL('G') || k == CTRL('C') || k == K_ESC) {
        set_buf(z.isearch_saved ? z.isearch_saved : "", z.isearch_saved_pos);
        z.hidx = hist_count();
        isearch_end();
        return 1;
    }
    if (k >= 32 && k < 256 && k != 127) {
        char c = k;
        sb_putc(&z.query, c);
        isearch_find(z.isearch_fail ? z.isearch_idx : z.isearch_idx, z.isearch_dir);
        if (z.isearch_fail)
            isearch_find(hist_count() - 1, -1);
        return 1;
    }
    /* anything else ends the search and is processed normally */
    isearch_end();
    return 0;
}

/* ---- completion ---- */

static void cands_free(void)
{
    /* the candidates live in the line's arena: forget them entirely */
    z.cands.v = 0;
    z.cands.cap = 0;
    z.cands.n = 0;
    z.menu = 0;
    z.listed = 0;
    sb_reset(&z.below);
    z.below_rows = 0;
}

static const char *kind_color(int k)
{
    switch (k) {
    case K_DIR: return "\033[1;34m";
    case K_EXEC: return "\033[32m";
    case K_BUILTIN: case K_FUNC: case K_ALIAS: return "\033[32m";
    case K_KEYWORD: return "\033[33m";
    case K_VAR: return "\033[36m";
    case K_JOB: return "\033[35m";
    }
    return "";
}

static void build_listing(void)
{
    sb_reset(&z.below);
    z.below_rows = 0;
    int n = z.cands.n;
    if (n == 0)
        return;
    int maxw = 0, has_desc = 0;
    for (int i = 0; i < n; i++) {
        int w = str_width(z.cands.v[i].disp, 1 << 30);
        if (w > maxw)
            maxw = w;
        if (z.cands.v[i].desc)
            has_desc = 1;
    }
    int width = cols() - 1;
    int colw = maxw + 2;
    int ncol = has_desc ? 1 : width / colw;
    if (ncol < 1)
        ncol = 1;
    int nrow = (n + ncol - 1) / ncol;
    int maxrows = (sh.rows > 8 ? sh.rows : 24) - 4;
    if (maxrows < 3)
        maxrows = 3;
    int top = 0;
    if (nrow > maxrows) {
        if (z.menu) {
            int selrow = z.menu_idx % nrow;
            if (selrow < z.menu_top)
                z.menu_top = selrow;
            if (selrow >= z.menu_top + maxrows - 1)
                z.menu_top = selrow - maxrows + 2;
            top = z.menu_top;
        }
    }
    int shown = nrow > maxrows ? maxrows - 1 : nrow;
    for (int r = top; r < top + shown && r < nrow; r++) {
        if (r > top)
            sb_puts(&z.below, "\033[K\n");
        int used = 0;
        for (int c = 0; c < ncol; c++) {
            int i = c * nrow + r;
            if (i >= n)
                break;
            struct cand *cd = &z.cands.v[i];
            int w = str_width(cd->disp, 1 << 30);
            int sel = z.menu && i == z.menu_idx;
            if (used + w > width)
                break;
            sb_puts(&z.below, sel ? "\033[7m" : kind_color(cd->kind));
            sb_puts(&z.below, cd->disp);
            if (has_desc && cd->desc) {
                int pad = maxw - w;
                while (pad-- > 0)
                    sb_putc(&z.below, ' ');
                sb_puts(&z.below, "\033[0m\033[90m  -- ");
                int room = width - maxw - 5;
                const char *d = cd->desc;
                int dl = strlen(d);
                sb_putn(&z.below, d, dl < room ? dl : room > 0 ? room : 0);
            }
            sb_puts(&z.below, "\033[0m");
            used += w;
            if (c + 1 < ncol && (c + 1) * nrow + r < n) {
                int pad = colw - w;
                for (int k = 0; k < pad && used < width; k++, used++)
                    sb_putc(&z.below, ' ');
            }
        }
        z.below_rows++;
    }
    if (nrow > shown) {
        sb_printf(&z.below, "\033[K\n\033[90mrows %d to %d of %d%s\033[0m", top + 1, top + shown, nrow,
                  z.menu ? "" : " (Tab again for a menu)");
        z.below_rows++;
    }
}

static void replace_word(int start, int end, const char *with)
{
    int save_pos = z.pos;
    (void)save_pos;
    delete_range(start, end);
    z.pos = start;
    insert(with, strlen(with));
}

static void menu_select(int idx)
{
    int n = z.cands.n;
    if (n == 0)
        return;
    z.menu_idx = (idx % n + n) % n;
    struct cand *c = &z.cands.v[z.menu_idx];
    struct sb w = { 0 };
    sb_puts(&w, c->word);
    if (c->suffix == '/')
        sb_putc(&w, '/');
    replace_word(z.menu_start, z.menu_start + z.menu_len, sb_str(&w));
    z.menu_len = w.len;
    sb_free(&w);
    build_listing();
}

static void menu_end(int restore)
{
    if (restore && z.menu_orig) {
        replace_word(z.menu_start, z.menu_start + z.menu_len, z.menu_orig);
    }
    free(z.menu_orig);
    z.menu_orig = 0;
    cands_free();
}

static void do_complete(int list_only)
{
    if (z.menu) {
        menu_select(z.menu_idx + 1);
        z.cur = C_COMPLETE;
        return;
    }
    int again = z.last == C_COMPLETE && z.listed;
    cands_free();
    z.cands.n = 0;
    complete_collect(sb_str(&z.buf), z.buf.len, z.pos, &z.cands);
    z.cur = C_COMPLETE;
    int n = z.cands.n;
    if (n == 0) {
        beep();
        return;
    }
    int start = z.cands.start, end = z.cands.end;
    if (list_only) {
        build_listing();
        z.listed = 1;
        return;
    }
    if (n == 1) {
        struct cand *c = &z.cands.v[0];
        struct sb w = { 0 };
        sb_puts(&w, c->word);
        if (c->suffix)
            sb_putc(&w, c->suffix);
        save_undo();
        replace_word(start, end, sb_str(&w));
        sb_free(&w);
        cands_free();
        return;
    }
    /* longest common prefix of the insertions */
    const char *first = z.cands.v[0].word;
    int lcp = strlen(first);
    int fold = 0;
    for (int i = 1; i < n; i++) {
        const char *w = z.cands.v[i].word;
        int k = 0;
        while (k < lcp && w[k] && (w[k] == first[k] || to_lower(w[k]) == to_lower(first[k]))) {
            if (w[k] != first[k])
                fold = 1;
            k++;
        }
        lcp = k;
    }
    (void)fold;
    int typed = end - start;
    if (lcp > typed || (lcp == typed && strncmp(first, z.buf.s + start, lcp))) {
        save_undo();
        replace_word(start, end, astrndup(first, lcp));
        end = start + lcp;
        z.cands.end = end;
        if (!opts[OPT_AUTOLIST]) {
            cands_free();
            return;
        }
    }
    if (again && opts[OPT_AUTOMENU]) {
        /* second Tab: menu selection */
        z.menu = 1;
        z.menu_top = 0;
        z.menu_start = start;
        z.menu_len = end - start;
        free(z.menu_orig);
        z.menu_orig = xstrndup(z.buf.s + start, end - start);
        save_undo();
        menu_select(0);
        z.listed = 1;
        return;
    }
    if (opts[OPT_AUTOLIST] || again) {
        build_listing();
        z.listed = 1;
    } else {
        beep();
    }
}

/* Keys while the menu is active; returns 1 if consumed. */
static int menu_key(int k)
{
    int n = z.cands.n;
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = str_width(z.cands.v[i].disp, 1 << 30);
        if (w > maxw)
            maxw = w;
    }
    int ncol = (cols() - 1) / (maxw + 2);
    if (ncol < 1)
        ncol = 1;
    for (int i = 0; i < n; i++)
        if (z.cands.v[i].desc)
            ncol = 1;
    int nrow = (n + ncol - 1) / ncol;
    switch (k) {
    case '\t':
        menu_select(z.menu_idx + 1);
        return 1;
    case K_STAB:
        menu_select(z.menu_idx - 1);
        return 1;
    case K_DOWN: case CTRL('N'):
        menu_select(z.menu_idx + 1);
        return 1;
    case K_UP: case CTRL('P'):
        menu_select(z.menu_idx - 1);
        return 1;
    case K_RIGHT: case CTRL('F'):
        menu_select(z.menu_idx + nrow < n ? z.menu_idx + nrow : z.menu_idx % nrow + (z.menu_idx % nrow + 1 < nrow ? 1 : 0));
        return 1;
    case K_LEFT: case CTRL('B'):
        menu_select(z.menu_idx - nrow >= 0 ? z.menu_idx - nrow : z.menu_idx);
        return 1;
    case '\r': case '\n':
        menu_end(0);
        return 1;
    case CTRL('G'): case CTRL('C'): case K_ESC:
        menu_end(1);
        return 1;
    }
    menu_end(0);
    return 0;
}

/* ---- the key loop ---- */

static void upcase_word(int mode)
{
    int e = word_right(z.pos);
    int s = z.pos;
    while (s < e && !is_word_char((unsigned char)z.buf.s[s]))
        s++;
    for (int i = s; i < e; i++) {
        char c = z.buf.s[i];
        if (mode == 'u')
            z.buf.s[i] = to_upper(c);
        else if (mode == 'l')
            z.buf.s[i] = to_lower(c);
        else
            z.buf.s[i] = i == s ? to_upper(c) : to_lower(c);
    }
    z.pos = e;
}

static void insert_last_word(void)
{
    if (z.last == C_LASTWORD) {
        z.lw_back++;
        delete_range(z.lw_start, z.lw_start + z.lw_len);
        z.pos = z.lw_start;
    } else {
        z.lw_back = 0;
    }
    const char *w = hist_last_word(z.lw_back, -1);
    z.cur = C_LASTWORD;
    if (!w) {
        beep();
        z.lw_len = 0;
        z.lw_start = z.pos;
        return;
    }
    z.lw_start = z.pos;
    insert(w, strlen(w));
    z.lw_len = strlen(w);
}

static void yank(void)
{
    if (!nkill) {
        beep();
        return;
    }
    yank_idx = 0;
    yank_start = z.pos;
    insert(kill_ring[0], strlen(kill_ring[0]));
    yank_len = strlen(kill_ring[0]);
    z.cur = C_YANK;
}

static void yank_pop(void)
{
    if (z.last != C_YANK || !nkill) {
        beep();
        return;
    }
    delete_range(yank_start, yank_start + yank_len);
    z.pos = yank_start;
    yank_idx = (yank_idx + 1) % nkill;
    insert(kill_ring[yank_idx], strlen(kill_ring[yank_idx]));
    yank_len = strlen(kill_ring[yank_idx]);
    z.cur = C_YANK;
}

static void paste(void)
{
    /* bracketed paste: everything up to ESC[201~ is inserted literally */
    struct sb p = { 0 };
    for (;;) {
        int c = readbyte(500);
        if (c < 0 && c != -2)
            break;
        if (c == -2)
            continue;
        sb_putc(&p, c == '\r' ? '\n' : c);
        if (p.len >= 6 && !memcmp(p.s + p.len - 6, "\033[201~", 6)) {
            p.len -= 6;
            break;
        }
    }
    save_undo();
    insert(p.s ? p.s : "", p.len);
    sb_free(&p);
}

static void clear_screen(void)
{
    write(1, "\033[H\033[2J", 7);
    print_prompt();
}

static void abort_line(void)
{
    /* zsh: keep what was typed visible, start over on a new line */
    free(z.suggest);
    z.suggest = 0;
    cands_free();
    refresh_full(0);
    write(1, "\n", 1);
    set_buf("", 0);
    z.done = 1;
    z.eof = 2;
}

static void handle_key(int k)
{
    z.cur = C_OTHER;
    if (z.isearch && isearch_key(k)) {
        z.cur = C_OTHER;
        return;
    }
    if (z.menu && menu_key(k))
        return;
    if (z.listed && k != '\t' && k != CTRL('D')) {
        z.listed = 0;
        sb_reset(&z.below);
        z.below_rows = 0;
    }
    int len = z.buf.len;
    switch (k) {
    case '\r': case '\n':
        z.done = 1;
        return;
    case K_ALT | '\r': case K_ALT | '\n':
        save_undo();
        insert("\n", 1);
        break;
    case CTRL('A'): case K_HOME:
        z.pos = line_start(z.pos);
        break;
    case CTRL('E'): case K_END:
        if (z.pos == len && accept_suggestion(0))
            break;
        z.pos = line_end(z.pos);
        break;
    case CTRL('B'): case K_LEFT:
        z.pos = utf8_prev(z.buf.s, z.pos);
        break;
    case CTRL('F'): case K_RIGHT:
        if (z.pos == len && accept_suggestion(0))
            break;
        z.pos = utf8_next(z.buf.s, len, z.pos);
        break;
    case K_ALT | 'b': case K_CLEFT:
        z.pos = word_left(z.pos);
        break;
    case K_ALT | 'f': case K_CRIGHT:
        if (z.pos == len && accept_suggestion(1))
            break;
        z.pos = word_right(z.pos);
        break;
    case CTRL('D'):
        if (len == 0) {
            if (opts[OPT_IGNOREEOF]) {
                zle_print_above("esh: use 'exit' to exit.\n");
                break;
            }
            z.done = 1;
            z.eof = 1;
            return;
        }
        if (z.pos == len) {
            do_complete(1);     /* delete-char-or-list */
            return;
        }
        /* fall through */
    case K_DEL:
        if (z.pos < len) {
            if (z.last != C_INSERT)
                save_undo();
            delete_range(z.pos, utf8_next(z.buf.s, len, z.pos));
        }
        break;
    case 127: case CTRL('H'):
        if (z.pos > 0) {
            save_undo();
            delete_range(utf8_prev(z.buf.s, z.pos), z.pos);
        }
        break;
    case CTRL('K'):
        save_undo();
        kill_text(z.pos, z.pos == line_end(z.pos) && z.pos < len ? z.pos + 1 : line_end(z.pos), 0);
        break;
    case CTRL('U'):
        save_undo();
        kill_text(0, len, 1);
        break;
    case CTRL('W'): case K_ALT | 127: case K_ALT | CTRL('H'):
        save_undo();
        kill_text(word_left(z.pos), z.pos, 1);
        break;
    case K_ALT | 'd':
        save_undo();
        kill_text(z.pos, word_right(z.pos), 0);
        break;
    case CTRL('Y'):
        save_undo();
        yank();
        break;
    case K_ALT | 'y':
        yank_pop();
        break;
    case CTRL('T'):
        if (len >= 2 && z.pos > 0) {
            save_undo();
            int p = z.pos == len ? z.pos - 1 : z.pos;
            char t = z.buf.s[p - 1];
            z.buf.s[p - 1] = z.buf.s[p];
            z.buf.s[p] = t;
            z.pos = p + 1;
        }
        break;
    case K_ALT | 'u': case K_ALT | 'l': case K_ALT | 'c':
        save_undo();
        upcase_word(k & 0xff);
        break;
    case CTRL('_'):
        if (z.nundo) {
            z.nundo--;
            set_buf(z.undo[z.nundo].s, z.undo[z.nundo].pos);
            free(z.undo[z.nundo].s);
        } else {
            beep();
        }
        z.cur = C_UNDO;
        break;
    case CTRL('X'): {
        int k2 = readkey();
        if (k2 == CTRL('U') || k2 == 'u')
            handle_key(CTRL('_'));
        else if (k2 == CTRL('X')) {
            /* exchange point and mark: here, toggle between start and end */
            z.pos = z.pos ? 0 : len;
        } else
            beep();
        return;
    }
    case CTRL('L'):
        clear_screen();
        break;
    case CTRL('C'): case CTRL('G'):
        abort_line();
        return;
    case K_UP:
        if (strchr(sb_str(&z.buf), '\n') && line_start(z.pos) > 0) {
            int col = z.pos - line_start(z.pos);
            int prev = line_start(line_start(z.pos) - 1);
            int pe = line_end(prev);
            z.pos = prev + col < pe ? prev + col : pe;
            break;
        }
        history_prefix(-1);
        break;
    case K_DOWN:
        if (strchr(sb_str(&z.buf), '\n') && line_end(z.pos) < len) {
            int col = z.pos - line_start(z.pos);
            int next = line_end(z.pos) + 1;
            int ne = line_end(next);
            z.pos = next + col < ne ? next + col : ne;
            break;
        }
        history_prefix(1);
        break;
    case CTRL('P'):
        history_step(-1);
        break;
    case CTRL('N'):
        history_step(1);
        break;
    case K_ALT | '<': case K_PGUP:
        if (hist_count())
            hist_goto(0);
        break;
    case K_ALT | '>': case K_PGDN:
        hist_goto(hist_count());
        break;
    case CTRL('R'):
        isearch_start(-1);
        break;
    case CTRL('S'):
        isearch_start(1);
        break;
    case '\t':
        do_complete(0);
        return;
    case K_STAB:
        beep();
        break;
    case CTRL('V'): {
        int c = readbyte(-1);
        if (c >= 0) {
            char ch = c;
            save_undo();
            insert(&ch, 1);
        }
        break;
    }
    case CTRL('Q'): case K_ALT | 'q':
        /* push-line: put this line aside until the next prompt */
        free(pushed_line);
        pushed_line = xstrdup(sb_str(&z.buf));
        set_buf("", 0);
        break;
    case K_ALT | '.': case K_ALT | '_':
        save_undo();
        insert_last_word();
        break;
    case K_PASTE:
        paste();
        break;
    case K_ESC: case K_INS: case -4:
        break;
    default:
        if (k >= 32 && k < 256) {
            char c = k;
            if (z.last != C_INSERT || c == ' ')
                save_undo();
            insert(&c, 1);
            z.cur = C_INSERT;
        } else {
            beep();
        }
    }
}

int zle_init(void)
{
    if (!isatty(0) || !isatty(1))
        return 0;
    tty_raw();
    term_query_size();
    tty_cooked();
    return 1;
}

char *zle_read(const char *ps, int pw, const char *rps, int rpw)
{
    sb_reset(&z.buf);
    z.pos = 0;
    z.done = z.eof = 0;
    z.prompt = ps;
    z.pw = pw;
    z.rprompt = rps;
    z.rpw = rpw;
    z.prompt_rows = 0;
    for (const char *p = ps; *p; p++)
        z.prompt_rows += *p == '\n';
    z.hidx = hist_count();
    free(z.saved);
    z.saved = 0;
    free(z.prefix);
    z.prefix = 0;
    free(z.suggest);
    z.suggest = 0;
    cands_free();
    isearch_end();
    for (int i = 0; i < z.nundo; i++)
        free(z.undo[i].s);
    z.nundo = 0;
    z.last = z.cur = C_OTHER;
    zle_active = 1;

    tty_raw();
    if (sh.term_ok)
        term_query_size();
    if (zle_cmd_ran) {
        char code[16];
        snprintf(code, sizeof(code), "D;%d", sh.last_status);
        osc(code);
        zle_cmd_ran = 0;
    }
    if (sh.term_ok && opts[OPT_PROMPTSP]) {
        /* zsh's PROMPT_SP: mark output that didn't end with a newline */
        struct sb b = { 0 };
        sb_puts(&b, "\033[1;7m%\033[0m");
        for (int i = 0; i < cols() - 1; i++)
            sb_putc(&b, ' ');
        sb_puts(&b, "\r\033[K");
        write(1, b.s, b.len);
        sb_free(&b);
    }
    if (sh.term_ok)
        write(1, "\033[?2004h", 8);
    print_prompt();
    if (pushed_line) {
        set_buf(pushed_line, -1);
        free(pushed_line);
        pushed_line = 0;
    }
    while (!z.done) {
        if (!input_pending()) {
            update_suggestion();
            refresh();
        }
        int k = readkey();
        if (k == K_INTR) {
            if (got_sigchld && opts[OPT_NOTIFY])
                notify_above();
            if (pending_traps) {
                tty_cooked();
                run_traps();
                tty_raw();
                refresh();
            }
            got_sigint = 0;
            continue;
        }
        if (k == K_EOF) {
            z.eof = 1;
            break;
        }
        handle_key(k);
        z.last = z.cur;
    }
    free(z.suggest);
    z.suggest = 0;
    if (z.menu)
        menu_end(0);
    cands_free();
    if (z.isearch)
        isearch_end();
    if (z.eof != 2) {
        if (z.eof == 1)
            set_buf("", 0);
        z.pos = z.buf.len;
        refresh_full(0);
        write(1, "\n", 1);
    }
    if (sh.term_ok)
        write(1, "\033[?2004l", 8);
    tty_cooked();
    zle_active = 0;
    if (z.eof == 1)
        return 0;
    if (z.eof == 2)
        return xstrdup("");
    osc("C");
    return xstrdup(sb_str(&z.buf));
}
