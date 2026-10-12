/*
 * edit: a text editor with two personalities, chosen by argv[0]:
 *   vi, view          modal, vi-like (vi.c)
 *   emacs, em, edit   modeless, emacs-like (emacs.c)
 *
 * The shared core: buffers of lines with undo (buffer.c), the terminal
 * (term.c), drawing with soft wrap, line numbers and syntax highlighting
 * (display.c), the prompt line (prompt.c) and searching (search.c).
 */
#ifndef EDIT_H
#define EDIT_H

#include <stddef.h>
#include <sys/types.h>

/* ---- keys ---- */

enum {
    K_UP = 0x1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS,
    K_STAB, K_CLEFT, K_CRIGHT, K_ESC, K_RESIZE, K_NONE,
    K_META = 0x2000,                /* ORed with a key: Alt/Meta (ESC prefix) */
};
#define CTRL(c) ((c) & 31)

/* ---- buffers ---- */

struct line {
    char *s;
    int len, cap;
    unsigned char hl_in;            /* syntax state at the start of the line */
};

enum { U_INSERT, U_DELETE };

struct undo {
    int kind;                       /* what was done (undo does the opposite) */
    int y, x;                       /* where */
    char *text;                     /* inserted or deleted text */
    int len;
    int group;                      /* records of one command share a group */
    int cy, cx;                     /* cursor before the command */
};

enum { SYN_NONE, SYN_C, SYN_SHELL, SYN_MAKE, SYN_PYTHON };

struct buffer {
    char *path;                     /* NULL: no file yet */
    char name[64];                  /* display name */
    struct line *lines;
    int nlines, cap;
    int dirty, readonly, newfile;
    int cy, cx;                     /* cursor: line, byte offset */
    int want_col;                   /* column to aim for on up/down */
    int top;                        /* first line on the screen */
    int mark_y, mark_x, mark_set;   /* emacs mark; vi visual start */
    struct undo *undo, *redo;
    int nundo, capundo, nredo, capredo;
    int group, last_group;
    int syntax;
    int hl_valid;                   /* lines whose hl_in is up to date */
    int vmarks[26][2];              /* vi marks a-z: line, column (-1: unset) */
    struct buffer *next;
};

extern struct buffer *buffers, *cur;

struct buffer *buf_new(const char *path);
int buf_load(struct buffer *b, const char *path);   /* 0, or -1 with message set */
int buf_save(struct buffer *b, const char *path);
void buf_free(struct buffer *b);
void buf_insert(struct buffer *b, int y, int x, const char *text, int len, int *ey, int *ex);
char *buf_delete(struct buffer *b, int y1, int x1, int y2, int x2);  /* heap, the deleted text */
char *buf_text(struct buffer *b, int y1, int x1, int y2, int x2, int *len);
void undo_begin(struct buffer *b);  /* start a new undo group (a new command) */
int undo_undo(struct buffer *b);    /* 0: nothing to undo */
int undo_redo(struct buffer *b);
void buf_touch(struct buffer *b, int from_line);   /* edits invalidate highlighting */
const char *line_indent_end(struct line *l, int *n);
int buf_count_dirty(void);
void buf_set_syntax(struct buffer *b);

/* ---- terminal and drawing ---- */

extern int rows, cols;
int term_init(void);                /* raw mode, alternate screen */
void term_done(void);
void term_suspend(void);            /* ^Z */
int term_getkey(void);              /* decoded key */
int term_peekkey(int ms);           /* is input pending within ms? */
void out(const char *s);
void outn(const char *s, int n);
void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void out_flush(void);

struct view_opts {
    int numbers;                    /* line numbers */
    int status_line;                /* emacs mode line */
    int highlight;                  /* syntax highlighting */
    int hlsearch;                   /* highlight matches of the last search */
    int tabstop;
    int list;                       /* show tabs and ends of lines */
};
extern struct view_opts vopt;
extern char message[256];
extern int message_error;
void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void draw(void);                    /* the whole screen */
void scroll_to_cursor(void);
int text_rows(void);
int screen_col(struct buffer *b, int y, int x);    /* display column of a byte offset */
int col_to_x(struct buffer *b, int y, int col);    /* byte offset nearest a display column */
extern void (*draw_status)(char *buf, int size);   /* the personality's mode line text */
extern const char *(*draw_bottom)(void);           /* text of the last line (vi mode etc.) */
extern int vis_y1, vis_x1, vis_y2, vis_x2, vis_mode; /* region to show in reverse: 0 none,
                                       1 characters (inclusive), 2 lines, 3 characters (exclusive) */
extern int show_tildes;             /* ~ on the rows past the end (vi) */

/* ---- the prompt line ---- */

/* Read a line at the bottom of the screen; NULL if cancelled. completion:
 * 'f' file names, 'c' M-x commands, 0 none. Result is static. */
char *prompt(const char *label, const char *initial, int completion);
int prompt_yes_no(const char *question);    /* 1 yes, 0 no, -1 cancelled */
extern const char *const *mx_commands;

/* ---- search ---- */

extern char last_pattern[256];
extern int search_hl_off;         /* :noh: hide matches until the next search */
extern int last_dir;                /* 1 forward, -1 backward */
int search_find(struct buffer *b, const char *pat, int dir, int fy, int fx, int *ry, int *rx,
                int *rlen, int wrap, int icase);
int substitute(struct buffer *b, int y1, int y2, const char *pat, const char *rep, int global,
               int icase, int confirm, int *nsubs);
int line_matches(const char *pat, const char *line, int icase);
/* Replace the match starting exactly at (y, x): 1 (end of the replacement in
   *ey, *ex; matched length in *mlen), 0 if it doesn't match there, -1 bad pattern. */
int replace_at(struct buffer *b, int y, int x, const char *pat, const char *rep, int icase, int *ey,
               int *ex, int *mlen);
int search_icase(const char *pat);  /* ignorecase/smartcase */
extern int opt_ignorecase, opt_smartcase;

/* ---- utilities ---- */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int is_word_char(int c);
int utf8_next(const char *s, int len, int pos);
int utf8_prev(const char *s, int pos);
int run_shell_command(const char *cmd, char **output, int *outlen);  /* exit status */

/* ---- personalities ---- */

int vi_main(int argc, char **argv, int readonly);
int emacs_main(int argc, char **argv);

#endif
