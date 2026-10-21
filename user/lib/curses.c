/*
 * curses (curses.h): a virtual screen of cells, drawn into by windows; a
 * second copy holds what the terminal shows, and refresh sends the
 * difference (cursor moves, SGR attribute changes, UTF-8 characters) in one
 * write. Input is decoded from ANSI/xterm key sequences.
 */
#include <curses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

WINDOW *stdscr;
int LINES, COLS, COLORS = 16, COLOR_PAIRS = 64;
chtype acs_map[14];

struct cell {
    unsigned ch;                        /* code point; 0: nothing known (phys) */
    attr_t attr;
};

static struct cell *virt, *phys;
static int rows, cols;
static int active;                      /* between initscr and endwin */
static int full_redraw;
static int cursor_vis = 1;
static int colors_on;
static short pair_fg[64], pair_bg[64];
static struct termios saved_tio, cur_tio;
static int tty = 0;
static int pushed = ERR;                /* ungetch */
static attr_t bkgd;

/* ---- output ---- */

static char obuf[16384];
static int olen;

static void oflush(void)
{
    for (int done = 0; done < olen;) {
        long n = write(tty == 0 ? 1 : tty, obuf + done, olen - done);
        if (n <= 0)
            break;
        done += n;
    }
    olen = 0;
}

static void oput(const char *s, int n)
{
    if (olen + n > (int)sizeof(obuf))
        oflush();
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void ostr(const char *s) { oput(s, (int)strlen(s)); }

static void outf(const char *fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    oput(b, n);
}

static void put_utf8(unsigned c)
{
    char b[4];
    int n;
    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xc0 | c >> 6);
        b[1] = (char)(0x80 | (c & 0x3f));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xe0 | c >> 12);
        b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[2] = (char)(0x80 | (c & 0x3f));
        n = 3;
    } else {
        b[0] = (char)(0xf0 | c >> 18);
        b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        b[3] = (char)(0x80 | (c & 0x3f));
        n = 4;
    }
    oput(b, n);
}

static void sgr(attr_t a)
{
    char b[64];
    int n = snprintf(b, sizeof(b), "\033[0");
    if (a & A_BOLD) n += snprintf(b + n, sizeof(b) - n, ";1");
    if (a & A_DIM) n += snprintf(b + n, sizeof(b) - n, ";2");
    if (a & A_ITALIC) n += snprintf(b + n, sizeof(b) - n, ";3");
    if (a & A_UNDERLINE) n += snprintf(b + n, sizeof(b) - n, ";4");
    if (a & A_BLINK) n += snprintf(b + n, sizeof(b) - n, ";5");
    if (a & A_REVERSE) n += snprintf(b + n, sizeof(b) - n, ";7");
    int p = PAIR_NUMBER(a);
    if (colors_on && p) {
        short fg = pair_fg[p], bg = pair_bg[p];
        if (fg >= 0)
            n += snprintf(b + n, sizeof(b) - n, ";%d", fg < 8 ? 30 + fg : 90 + fg - 8);
        if (bg >= 0)
            n += snprintf(b + n, sizeof(b) - n, ";%d", bg < 8 ? 40 + bg : 100 + bg - 8);
    }
    b[n++] = 'm';
    oput(b, n);
}

/* ---- terminal modes and size ---- */

static void set_tio(void)
{
    tcsetattr(tty, TCSANOW, &cur_tio);
}

static void measure(int *r, int *c)
{
    struct winsize ws;
    *r = 24;
    *c = 80;
    if (ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1 && ws.ws_col > 1) {
        *r = ws.ws_row;
        *c = ws.ws_col;
    }
    const char *e = getenv("LINES");
    if (e && atoi(e) > 1)
        *r = atoi(e);
    e = getenv("COLUMNS");
    if (e && atoi(e) > 1)
        *c = atoi(e);
    if (*r > 300)
        *r = 300;
    if (*c > 500)
        *c = 500;
}

static void alloc_screen(int r, int c)
{
    free(virt);
    free(phys);
    rows = r;
    cols = c;
    virt = calloc((size_t)r * c, sizeof(*virt));
    phys = calloc((size_t)r * c, sizeof(*phys));
    for (int i = 0; i < r * c; i++)
        virt[i].ch = ' ';
    LINES = r;
    COLS = c;
    if (stdscr) {
        stdscr->rows = r;
        stdscr->cols = c;
        if (stdscr->cy >= r)
            stdscr->cy = r - 1;
        if (stdscr->cx >= c)
            stdscr->cx = c - 1;
    }
    full_redraw = 1;
}

/* The size changed since we last looked? Then a new (blank) screen. */
static int resized(void)
{
    int r, c;
    measure(&r, &c);
    if (r == rows && c == cols)
        return 0;
    alloc_screen(r, c);
    return 1;
}

static void enter_screen(void)
{
    ostr("\033[?1049h\033[H\033[2J");   /* the alternate screen keeps the shell's text */
    if (!cursor_vis)
        ostr("\033[?25l");
    oflush();
    full_redraw = 1;
}

static void on_tstp(int sig);

static void on_cont(int sig)
{
    (void)sig;
    signal(SIGTSTP, on_tstp);
    set_tio();
    enter_screen();
    pushed = KEY_RESIZE;                /* the program draws everything again */
}

static void on_tstp(int sig)
{
    (void)sig;
    sgr(A_NORMAL);
    ostr("\033[?25h\033[?1049l");
    oflush();
    tcsetattr(tty, TCSANOW, &saved_tio);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCONT, on_cont);
    raise(SIGTSTP);
}

static void at_exit(void)
{
    if (active)
        endwin();
}

WINDOW *initscr(void)
{
    if (active)
        return stdscr;
    tty = isatty(0) ? 0 : -1;
    if (tty < 0) {
        fprintf(stderr, "curses: standard input is not a terminal\n");
        exit(1);
    }
    tcgetattr(tty, &saved_tio);
    cur_tio = saved_tio;
    const char *ascii = getenv("CURSES_ASCII");
    static const unsigned uni[] = { 0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, 0x251c,
                                    0x2524, 0x252c, 0x2534, 0x253c, 0x2588, 0x2592, 0x00b7 };
    static const char asc[] = "-|++++++++++#:o";
    for (int i = 0; i < 14; i++)
        acs_map[i] = ascii ? (chtype)(unsigned char)asc[i] : uni[i];
    if (!stdscr)
        stdscr = calloc(1, sizeof(*stdscr));
    stdscr->delay_ms = -1;
    int r, c;
    measure(&r, &c);
    alloc_screen(r, c);
    active = 1;
    enter_screen();
    signal(SIGTSTP, on_tstp);
    static int registered;
    if (!registered++)
        atexit(at_exit);
    return stdscr;
}

int endwin(void)
{
    if (!active)
        return ERR;
    sgr(A_NORMAL);
    outf("\033[%d;1H", rows);
    ostr("\033[?25h\033[?1049l");
    oflush();
    tcsetattr(tty, TCSAFLUSH, &saved_tio);  /* (keys typed for us go too) */
    signal(SIGTSTP, SIG_DFL);
    active = 0;
    return OK;
}

bool isendwin(void) { return !active; }

WINDOW *newwin(int r, int c, int y, int x)
{
    WINDOW *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->y0 = y;
    w->x0 = x;
    w->rows = r ? r : LINES - y;
    w->cols = c ? c : COLS - x;
    w->delay_ms = -1;
    return w;
}

int delwin(WINDOW *w)
{
    if (w && w != stdscr)
        free(w);
    return OK;
}

int cbreak(void) { cur_tio.c_lflag &= ~ICANON; cur_tio.c_cc[VMIN] = 1; cur_tio.c_cc[VTIME] = 0; set_tio(); return OK; }
int nocbreak(void) { cur_tio.c_lflag |= ICANON; set_tio(); return OK; }
int raw(void) { cur_tio.c_lflag &= ~(ICANON | ISIG | IEXTEN); cur_tio.c_iflag &= ~(IXON | ICRNL);
                cur_tio.c_cc[VMIN] = 1; cur_tio.c_cc[VTIME] = 0; set_tio(); return OK; }
int noraw(void) { cur_tio.c_lflag |= ICANON | ISIG | IEXTEN; set_tio(); return OK; }
int echo(void) { cur_tio.c_lflag |= ECHO; set_tio(); return OK; }
int noecho(void) { cur_tio.c_lflag &= ~ECHO; set_tio(); return OK; }
int keypad(WINDOW *w, bool on) { w->keypad = on; return OK; }
int nodelay(WINDOW *w, bool on) { w->delay_ms = on ? 0 : -1; return OK; }
void wtimeout(WINDOW *w, int ms) { w->delay_ms = ms; }
void timeout(int ms) { wtimeout(stdscr, ms); }

int curs_set(int v)
{
    int old = cursor_vis;
    cursor_vis = v != 0;
    ostr(cursor_vis ? "\033[?25h" : "\033[?25l");
    oflush();
    return old;
}

int napms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
    return OK;
}

int beep(void) { ostr("\a"); oflush(); return OK; }

int flushinp(void)
{
    char b[64];
    struct pollfd p = { tty, POLLIN, 0 };
    while (poll(&p, 1, 0) > 0 && read(tty, b, sizeof(b)) > 0)
        ;
    pushed = ERR;
    return OK;
}

int start_color(void) { colors_on = 1; return OK; }
bool has_colors(void) { const char *t = getenv("TERM"); return !t || strcmp(t, "dumb"); }
int use_default_colors(void) { return OK; }

int init_pair(short p, short fg, short bg)
{
    if (p < 1 || p >= 64)
        return ERR;
    pair_fg[p] = fg;
    pair_bg[p] = bg;
    full_redraw = 1;                    /* cells with this pair may look different now */
    return OK;
}

/* ---- drawing into the virtual screen ---- */

static struct cell *at(WINDOW *w, int y, int x)
{
    int sy = w->y0 + y, sx = w->x0 + x;
    if (y < 0 || x < 0 || y >= w->rows || x >= w->cols || sy >= rows || sx >= cols || sy < 0 || sx < 0)
        return NULL;
    return &virt[sy * cols + sx];
}

int wmove(WINDOW *w, int y, int x)
{
    if (y < 0 || x < 0 || y >= w->rows || x >= w->cols)
        return ERR;
    w->cy = y;
    w->cx = x;
    return OK;
}

int move(int y, int x) { return wmove(stdscr, y, x); }

static int put_cell(WINDOW *w, unsigned ch, attr_t a)
{
    if (ch == '\n') {
        wclrtoeol(w);
        if (w->cy + 1 >= w->rows)
            return ERR;
        w->cy++;
        w->cx = 0;
        return OK;
    }
    if (ch == '\t') {
        do
            put_cell(w, ' ', a);
        while (w->cx % 8 && w->cx);
        return OK;
    }
    if (ch < 32 || ch == 127)
        ch = '?';
    struct cell *c = at(w, w->cy, w->cx);
    if (!c)
        return ERR;
    c->ch = ch;
    c->attr = a;
    if (++w->cx >= w->cols) {           /* wrap (no scrolling) */
        w->cx = 0;
        if (w->cy + 1 < w->rows)
            w->cy++;
        else
            w->cx = w->cols - 1;
    }
    return OK;
}

int waddch(WINDOW *w, chtype c)
{
    attr_t a = (c & A_ATTRIBUTES) | w->attrs;
    if ((c & A_COLOR) && (w->attrs & A_COLOR))
        a = (a & ~A_COLOR) | (c & A_COLOR);
    return put_cell(w, (unsigned)(c & A_CHARTEXT), a);
}

int addch(chtype c) { return waddch(stdscr, c); }
int mvaddch(int y, int x, chtype c) { return move(y, x) == OK ? addch(c) : ERR; }
int mvwaddch(WINDOW *w, int y, int x, chtype c) { return wmove(w, y, x) == OK ? waddch(w, c) : ERR; }

int waddnstr(WINDOW *w, const char *s, int n)
{
    const unsigned char *p = (const unsigned char *)s;
    int count = 0;
    while (*p && (n < 0 || count < n)) {
        unsigned c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more)
            c &= 0x3f >> more;
        while (more-- && (*p & 0xc0) == 0x80)
            c = c << 6 | (*p++ & 0x3f);
        put_cell(w, c, w->attrs);
        count++;
    }
    return OK;
}

int waddstr(WINDOW *w, const char *s) { return waddnstr(w, s, -1); }
int addstr(const char *s) { return waddstr(stdscr, s); }
int addnstr(const char *s, int n) { return waddnstr(stdscr, s, n); }
int mvaddstr(int y, int x, const char *s) { return move(y, x) == OK ? addstr(s) : ERR; }
int mvaddnstr(int y, int x, const char *s, int n) { return move(y, x) == OK ? addnstr(s, n) : ERR; }
int mvwaddstr(WINDOW *w, int y, int x, const char *s) { return wmove(w, y, x) == OK ? waddstr(w, s) : ERR; }

int vw_printw(WINDOW *w, const char *fmt, va_list ap)
{
    char b[1024];
    vsnprintf(b, sizeof(b), fmt, ap);
    return waddstr(w, b);
}

int wprintw(WINDOW *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(w, fmt, ap);
    va_end(ap);
    return r;
}

int printw(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

int mvprintw(int y, int x, const char *fmt, ...)
{
    if (move(y, x) != OK)
        return ERR;
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...)
{
    if (wmove(w, y, x) != OK)
        return ERR;
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(w, fmt, ap);
    va_end(ap);
    return r;
}

int whline(WINDOW *w, chtype c, int n)
{
    if (!c)
        c = ACS_HLINE;
    for (int i = 0; i < n && w->cx + i < w->cols; i++) {
        struct cell *cl = at(w, w->cy, w->cx + i);
        if (cl) {
            cl->ch = (unsigned)(c & A_CHARTEXT);
            cl->attr = (c & A_ATTRIBUTES) | w->attrs;
        }
    }
    return OK;
}

int hline(chtype c, int n) { return whline(stdscr, c, n); }
int mvhline(int y, int x, chtype c, int n) { return move(y, x) == OK ? hline(c, n) : ERR; }

int wvline(WINDOW *w, chtype c, int n)
{
    if (!c)
        c = ACS_VLINE;
    for (int i = 0; i < n && w->cy + i < w->rows; i++) {
        struct cell *cl = at(w, w->cy + i, w->cx);
        if (cl) {
            cl->ch = (unsigned)(c & A_CHARTEXT);
            cl->attr = (c & A_ATTRIBUTES) | w->attrs;
        }
    }
    return OK;
}

int mvvline(int y, int x, chtype c, int n) { return move(y, x) == OK ? wvline(stdscr, c, n) : ERR; }

int box(WINDOW *w, chtype v, chtype h)
{
    if (!v)
        v = ACS_VLINE;
    if (!h)
        h = ACS_HLINE;
    int y = w->cy, x = w->cx;
    wmove(w, 0, 1);
    whline(w, h, w->cols - 2);
    wmove(w, w->rows - 1, 1);
    whline(w, h, w->cols - 2);
    wmove(w, 1, 0);
    wvline(w, v, w->rows - 2);
    wmove(w, 1, w->cols - 1);
    wvline(w, v, w->rows - 2);
    mvwaddch(w, 0, 0, ACS_ULCORNER);
    mvwaddch(w, 0, w->cols - 1, ACS_URCORNER);
    mvwaddch(w, w->rows - 1, 0, ACS_LLCORNER);
    struct cell *c = at(w, w->rows - 1, w->cols - 1);   /* (no wrap past the corner) */
    if (c) {
        c->ch = (unsigned)(ACS_LRCORNER & A_CHARTEXT);
        c->attr = w->attrs;
    }
    w->cy = y;
    w->cx = x;
    return OK;
}

int wattron(WINDOW *w, attr_t a)
{
    if (a & A_COLOR)
        w->attrs &= ~A_COLOR;
    w->attrs |= a;
    return OK;
}

int wattroff(WINDOW *w, attr_t a) { w->attrs &= ~a; return OK; }
int wattrset(WINDOW *w, attr_t a) { w->attrs = a; return OK; }
int attron(attr_t a) { return wattron(stdscr, a); }
int attroff(attr_t a) { return wattroff(stdscr, a); }
int attrset(attr_t a) { return wattrset(stdscr, a); }
int standout(void) { return attron(A_STANDOUT); }
int standend(void) { return attrset(A_NORMAL); }
int wbkgdset_attr(WINDOW *w, attr_t a) { (void)w; bkgd = a; return OK; }

int wclrtoeol(WINDOW *w)
{
    for (int x = w->cx; x < w->cols; x++) {
        struct cell *c = at(w, w->cy, x);
        if (c) {
            c->ch = ' ';
            c->attr = bkgd;
        }
    }
    return OK;
}

int clrtoeol(void) { return wclrtoeol(stdscr); }

int wclrtobot(WINDOW *w)
{
    int y = w->cy, x = w->cx;
    wclrtoeol(w);
    for (int r = y + 1; r < w->rows; r++) {
        w->cy = r;
        w->cx = 0;
        wclrtoeol(w);
    }
    w->cy = y;
    w->cx = x;
    return OK;
}

int clrtobot(void) { return wclrtobot(stdscr); }

int werase(WINDOW *w)
{
    w->cy = w->cx = 0;
    wclrtobot(w);
    return OK;
}

int erase(void) { return werase(stdscr); }
int wclear(WINDOW *w) { werase(w); full_redraw = 1; return OK; }
int clear(void) { return wclear(stdscr); }
int clearok(WINDOW *w, bool on) { (void)w; if (on) full_redraw = 1; return OK; }

/* ---- refresh: send what changed ---- */

int doupdate(void)
{
    if (!active)
        return ERR;
    attr_t cur = (attr_t)-1;
    if (full_redraw) {
        sgr(A_NORMAL);
        cur = A_NORMAL;
        ostr("\033[H\033[2J");
        for (int i = 0; i < rows * cols; i++) {
            phys[i].ch = ' ';
            phys[i].attr = A_NORMAL;
        }
        full_redraw = 0;
    }
    for (int y = 0; y < rows; y++) {
        struct cell *v = &virt[y * cols], *p = &phys[y * cols];
        int x = 0;
        while (x < cols) {
            if (v[x].ch == p[x].ch && v[x].attr == p[x].attr) {
                x++;
                continue;
            }
            /* a run of changes (small gaps included: cheaper than moving) */
            int end = x, gap = 0;
            for (int k = x; k < cols && gap < 6; k++) {
                if (v[k].ch != p[k].ch || v[k].attr != p[k].attr) {
                    end = k;
                    gap = 0;
                } else {
                    gap++;
                }
            }
            outf("\033[%d;%dH", y + 1, x + 1);
            for (int k = x; k <= end; k++) {
                if (v[k].attr != cur) {
                    sgr(v[k].attr);
                    cur = v[k].attr;
                }
                put_utf8(v[k].ch ? v[k].ch : ' ');
                p[k] = v[k];
            }
            x = end + 1;
        }
    }
    if (cur != A_NORMAL)
        sgr(A_NORMAL);
    outf("\033[%d;%dH", stdscr->y0 + stdscr->cy + 1, stdscr->x0 + stdscr->cx + 1);
    oflush();
    return OK;
}

int wrefresh(WINDOW *w) { (void)w; return doupdate(); }
int refresh(void) { return doupdate(); }

/* ---- input ---- */

static int read_byte(int ms)
{
    struct pollfd p = { tty, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0)
        return ERR;
    unsigned char c;
    return read(tty, &c, 1) == 1 ? c : ERR;
}

/* What follows ESC: a key sequence, or just ESC. */
static int escape(void)
{
    int c = read_byte(60);
    if (c == ERR)
        return 27;
    if (c == 'O') {                     /* SS3: F1-F4, and keypad arrows */
        c = read_byte(60);
        switch (c) {
        case 'P': return KEY_F(1);
        case 'Q': return KEY_F(2);
        case 'R': return KEY_F(3);
        case 'S': return KEY_F(4);
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
        return 27;
    }
    if (c != '[')
        return c | 0x200;               /* (Alt-c: rarely useful; distinct from c) */
    int num = 0;
    for (;;) {
        c = read_byte(60);
        if (c >= '0' && c <= '9')
            num = num * 10 + (c - '0');
        else if (c != ';')
            break;
    }
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch (num) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 2: return KEY_IC;
        case 3: return KEY_DC;
        case 5: return KEY_PPAGE;
        case 6: return KEY_NPAGE;
        case 11: case 12: case 13: case 14: case 15: return KEY_F(num - 10);
        case 17: case 18: case 19: case 20: case 21: return KEY_F(num - 11);
        case 23: case 24: return KEY_F(num - 12);
        }
    }
    return ERR;
}

int wgetch(WINDOW *w)
{
    if (pushed != ERR) {
        int c = pushed;
        pushed = ERR;
        return c;
    }
    if (resized())
        return KEY_RESIZE;
    int ms = w->delay_ms;
    /* (wait in slices, so a size change shows up while we wait) */
    for (;;) {
        int slice = ms < 0 || ms > 500 ? 500 : ms;
        int c = read_byte(slice);
        if (c != ERR) {
            if (c == 27 && w->keypad) {
                c = escape();
                if (c == ERR)
                    continue;
            } else if (w->keypad && (c == 127 || c == 8)) {
                c = KEY_BACKSPACE;
            } else if (w->keypad && c == '\r' && !(cur_tio.c_iflag & ICRNL)) {
                c = '\n';
            }
            return c;
        }
        if (resized())
            return KEY_RESIZE;
        if (ms >= 0) {
            ms -= slice;
            if (ms <= 0)
                return ERR;
        }
    }
}

int getch(void) { return wgetch(stdscr); }

int ungetch(int c)
{
    pushed = c;
    return OK;
}
