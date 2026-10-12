/* edit: the terminal (raw mode, keys, output). */
#include "edit.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int rows = 24, cols = 80;
static int tty = -1;
static struct termios saved;
static int raw_on;
static volatile int resumed;

/* ---- output ---- */

static char *obuf;
static int olen, ocap;

void outn(const char *s, int n)
{
    if (olen + n > ocap) {
        ocap = ocap ? ocap * 2 : 16384;
        while (ocap < olen + n)
            ocap *= 2;
        obuf = xrealloc(obuf, ocap);
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

void out(const char *s)
{
    outn(s, strlen(s));
}

void outf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    outn(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

void out_flush(void)
{
    int done = 0;
    while (done < olen) {
        long n = write(tty >= 0 ? tty : 1, obuf + done, olen - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += n;
    }
    olen = 0;
}

/* ---- modes ---- */

static void get_size(void)
{
    struct winsize ws;
    if (ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 3 && ws.ws_col > 10) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    const char *e = getenv("LINES");
    if (e && atoi(e) > 3)
        rows = atoi(e);
    e = getenv("COLUMNS");
    if (e && atoi(e) > 10)
        cols = atoi(e);
}

static void raw(void)
{
    struct termios t = saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(ICRNL | IXON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSANOW, &t);
    raw_on = 1;
    out("\033[?1049h\033[H\033[2J");    /* the alternate screen keeps the shell's output */
    out_flush();
}

static void on_cont(int sig)
{
    (void)sig;
    resumed = 1;
}

int term_init(void)
{
    tty = open("/dev/tty", O_RDWR);
    if (tty < 0 || !isatty(tty)) {
        if (tty >= 0)
            close(tty);
        tty = isatty(0) ? 0 : -1;
    }
    if (tty < 0) {
        eprintf("edit: needs a terminal\n");
        return -1;
    }
    if (tcgetattr(tty, &saved) < 0)
        return -1;
    get_size();
    raw();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_cont;
    sigaction(SIGCONT, &sa, 0);
    return 0;
}

void term_done(void)
{
    if (!raw_on)
        return;
    out("\033[0m\033[?25h\033[?1049l");
    out_flush();
    tcsetattr(tty, TCSANOW, &saved);
    raw_on = 0;
}

void term_suspend(void)
{
    term_done();
    signal(SIGTSTP, SIG_DFL);
    kill(0, SIGTSTP);                   /* our process group: the shell takes over */
    /* resumed */
    tcgetattr(tty, &saved);
    get_size();
    raw();
}

/* ---- input ---- */

static unsigned char pending[64];
static int npending;

static int readbyte(int timeout)
{
    if (npending) {
        int c = pending[0];
        memmove(pending, pending + 1, --npending);
        return c;
    }
    for (;;) {
        if (timeout >= 0) {
            struct pollfd p = { .fd = tty, .events = POLLIN };
            int r = poll(&p, 1, timeout);
            if (r < 0 && errno == EINTR) {
                if (resumed)
                    return -2;
                continue;
            }
            if (r <= 0)
                return -1;
        }
        unsigned char c;
        long n = read(tty, &c, 1);
        if (n == 1)
            return c;
        if (n < 0 && errno == EINTR) {
            if (resumed)
                return -2;
            continue;
        }
        return -3;                      /* end of input: the terminal went away */
    }
}

int term_peekkey(int ms)
{
    if (npending)
        return 1;
    struct pollfd p = { .fd = tty, .events = POLLIN };
    return poll(&p, 1, ms) > 0;
}

int term_getkey(void)
{
    for (;;) {
        int c = readbyte(-1);
        if (c == -2 || resumed) {
            resumed = 0;
            get_size();
            return K_RESIZE;
        }
        if (c == -3) {
            term_done();
            exit(1);
        }
        if (c != 27)
            return c;
        int d = readbyte(30);
        if (d < 0)
            return K_ESC;
        if (d == '[' || d == 'O') {
            char params[16];
            int n = 0, f;
            for (;;) {
                f = readbyte(50);
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
            int meta = mod == 3 ? K_META : 0;
            switch (f) {
            case 'A': return K_UP | meta;
            case 'B': return K_DOWN | meta;
            case 'C': return mod == 5 ? K_CRIGHT : K_RIGHT | meta;
            case 'D': return mod == 5 ? K_CLEFT : K_LEFT | meta;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case 'Z': return K_STAB;
            case '~':
                switch (p1) {
                case 1: case 7: return K_HOME;
                case 4: case 8: return K_END;
                case 2: return K_INS;
                case 3: return K_DEL;
                case 5: return K_PGUP;
                case 6: return K_PGDN;
                }
            }
            continue;                   /* unknown sequence: ignore */
        }
        return K_META | d;
    }
}
