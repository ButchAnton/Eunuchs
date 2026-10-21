#ifndef _CURSES_H
#define _CURSES_H

/*
 * curses: full-screen terminal programs (top). A subset of the usual API
 * over ANSI escape sequences: one virtual screen, redrawn by sending only
 * what changed; windows are rectangles of it. Characters are Unicode code
 * points (addstr takes UTF-8); attributes and a color pair ride above them
 * in a chtype. The terminal's size comes from TIOCGWINSZ (the shell measures
 * the serial console), else $LINES/$COLUMNS, else 24x80; getch() checks it
 * again when called and every half second while it waits, and returns
 * KEY_RESIZE when it changed.
 */

#include <stdarg.h>
#include <stdbool.h>

typedef unsigned long chtype;           /* code point | attributes | color pair */
typedef chtype attr_t;

#define A_CHARTEXT   0xffffffffUL
#define A_NORMAL     0UL
#define A_BOLD       (1UL << 32)
#define A_DIM        (1UL << 33)
#define A_UNDERLINE  (1UL << 34)
#define A_REVERSE    (1UL << 35)
#define A_BLINK      (1UL << 36)
#define A_ITALIC     (1UL << 37)
#define A_STANDOUT   (A_REVERSE | A_BOLD)
#define A_ATTRIBUTES (0xffffffffUL << 32)
#define A_COLOR      (0xffUL << 40)
#define COLOR_PAIR(n)   ((chtype)((n) & 0xff) << 40)
#define PAIR_NUMBER(a)  ((int)(((a) >> 40) & 0xff))

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7                 /* 8..15: the bright versions */

/* Line drawing (Unicode box drawing; ASCII with $CURSES_ASCII set). */
extern chtype acs_map[];
#define ACS_HLINE    (acs_map[0])
#define ACS_VLINE    (acs_map[1])
#define ACS_ULCORNER (acs_map[2])
#define ACS_URCORNER (acs_map[3])
#define ACS_LLCORNER (acs_map[4])
#define ACS_LRCORNER (acs_map[5])
#define ACS_LTEE     (acs_map[6])
#define ACS_RTEE     (acs_map[7])
#define ACS_TTEE     (acs_map[8])
#define ACS_BTEE     (acs_map[9])
#define ACS_PLUS     (acs_map[10])
#define ACS_BLOCK    (acs_map[11])      /* a full block (bars) */
#define ACS_CKBOARD  (acs_map[12])      /* a shaded block */
#define ACS_BULLET   (acs_map[13])

/* Keys from getch() with keypad() on. */
#define KEY_DOWN      0402
#define KEY_UP        0403
#define KEY_LEFT      0404
#define KEY_RIGHT     0405
#define KEY_HOME      0406
#define KEY_BACKSPACE 0407
#define KEY_F0        0410
#define KEY_F(n)      (KEY_F0 + (n))
#define KEY_DC        0512
#define KEY_IC        0513
#define KEY_NPAGE     0522
#define KEY_PPAGE     0523
#define KEY_ENTER     0527
#define KEY_END       0550
#define KEY_RESIZE    0632
#define ERR (-1)
#define OK  0

typedef struct curses_window {
    int y0, x0, rows, cols;             /* on the screen */
    int cy, cx;                         /* the cursor, in the window */
    attr_t attrs;
    bool keypad, nodelay;
    int delay_ms;                       /* getch: -1 wait, 0 don't, else this long */
} WINDOW;

extern WINDOW *stdscr;
extern int LINES, COLS, COLORS, COLOR_PAIRS;

WINDOW *initscr(void);
int endwin(void);
bool isendwin(void);
WINDOW *newwin(int rows, int cols, int y, int x);
int delwin(WINDOW *w);

int cbreak(void);
int nocbreak(void);
int raw(void);
int noraw(void);
int echo(void);
int noecho(void);
int keypad(WINDOW *w, bool on);
int nodelay(WINDOW *w, bool on);
void wtimeout(WINDOW *w, int ms);
void timeout(int ms);
int curs_set(int visibility);           /* 0 hidden, 1 normal; returns the old one */
int napms(int ms);
int beep(void);
int flushinp(void);

int start_color(void);
bool has_colors(void);
int use_default_colors(void);
int init_pair(short pair, short fg, short bg);     /* -1: the terminal's default */

int wrefresh(WINDOW *w);
int refresh(void);
int doupdate(void);
int werase(WINDOW *w);
int erase(void);
int wclear(WINDOW *w);
int clear(void);                        /* erase, and repaint everything next refresh */
int clearok(WINDOW *w, bool on);
int wclrtoeol(WINDOW *w);
int clrtoeol(void);
int wclrtobot(WINDOW *w);
int clrtobot(void);

int wmove(WINDOW *w, int y, int x);
int move(int y, int x);
int waddch(WINDOW *w, chtype c);
int addch(chtype c);
int mvaddch(int y, int x, chtype c);
int mvwaddch(WINDOW *w, int y, int x, chtype c);
int waddnstr(WINDOW *w, const char *s, int n);    /* UTF-8; n < 0: all */
int waddstr(WINDOW *w, const char *s);
int addstr(const char *s);
int addnstr(const char *s, int n);
int mvaddstr(int y, int x, const char *s);
int mvaddnstr(int y, int x, const char *s, int n);
int mvwaddstr(WINDOW *w, int y, int x, const char *s);
int vw_printw(WINDOW *w, const char *fmt, va_list ap);
int wprintw(WINDOW *w, const char *fmt, ...);
int printw(const char *fmt, ...);
int mvprintw(int y, int x, const char *fmt, ...);
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...);
int whline(WINDOW *w, chtype c, int n);
int hline(chtype c, int n);
int mvhline(int y, int x, chtype c, int n);
int wvline(WINDOW *w, chtype c, int n);
int mvvline(int y, int x, chtype c, int n);
int box(WINDOW *w, chtype v, chtype h);  /* 0: the line-drawing characters */

int wattron(WINDOW *w, attr_t a);
int wattroff(WINDOW *w, attr_t a);
int wattrset(WINDOW *w, attr_t a);
int attron(attr_t a);
int attroff(attr_t a);
int attrset(attr_t a);
int standout(void);
int standend(void);
int wbkgdset_attr(WINDOW *w, attr_t a);  /* (attributes for erasing) */

int wgetch(WINDOW *w);
int getch(void);
int ungetch(int c);

#define getyx(w, y, x)    ((y) = (w)->cy, (x) = (w)->cx)
#define getmaxyx(w, y, x) ((y) = (w)->rows, (x) = (w)->cols)
#define getbegyx(w, y, x) ((y) = (w)->y0, (x) = (w)->x0)

#endif
