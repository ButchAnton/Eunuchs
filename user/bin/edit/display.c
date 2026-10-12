/* edit: drawing the screen. */
#include "edit.h"

#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct view_opts vopt = { 0, 0, 1, 1, 8, 0 };
char message[256];
int message_error;
void (*draw_status)(char *buf, int size);
const char *(*draw_bottom)(void);
int vis_y1, vis_x1, vis_y2, vis_x2, vis_mode;
int show_tildes = 1;

void msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_error = 0;
}

void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_error = 1;
}

int text_rows(void)
{
    return rows - 1 - (vopt.status_line ? 1 : 0);
}

static int gutter(struct buffer *b)
{
    if (!vopt.numbers)
        return 0;
    int d = 1, n = b->nlines;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return (d < 3 ? 3 : d) + 1;
}

static int width_of(struct buffer *b)
{
    int w = cols - gutter(b);
    return w > 10 ? w : 10;
}

/* Display width of one character starting at byte i, at column col. */
static int cell_width(const char *s, int len, int i, int col, int *bytes)
{
    unsigned char c = s[i];
    *bytes = 1;
    if (c == '\t')
        return vopt.list ? 2 : vopt.tabstop - col % vopt.tabstop;
    if (c < 32 || c == 127)
        return 2;
    if (c >= 0x80) {
        *bytes = utf8_next(s, len, i) - i;
        return 1;
    }
    return 1;
}

int screen_col(struct buffer *b, int y, int x)
{
    struct line *l = &b->lines[y];
    int col = 0;
    for (int i = 0; i < x && i < l->len;) {
        int n;
        col += cell_width(l->s, l->len, i, col, &n);
        i += n;
    }
    return col;
}

int col_to_x(struct buffer *b, int y, int want)
{
    struct line *l = &b->lines[y];
    int col = 0, i = 0;
    while (i < l->len) {
        int n, w = cell_width(l->s, l->len, i, col, &n);
        if (col + w > want)
            break;
        col += w;
        i += n;
    }
    return i;
}

static int line_rows(struct buffer *b, int y)
{
    int w = screen_col(b, y, b->lines[y].len) + (vopt.list ? 1 : 0);
    int W = width_of(b);
    return w <= 0 ? 1 : (w + W - 1) / W;
}

/* ---- syntax highlighting ---- */

enum { HL_NORMAL, HL_COMMENT, HL_STRING, HL_NUMBER, HL_KEYWORD, HL_TYPE, HL_PREPROC, HL_VAR,
       HL_SEARCH, HL_VISUAL, HL_SPECIAL };

static const char *const hl_sgr[] = {
    [HL_NORMAL] = "\033[0m",
    [HL_COMMENT] = "\033[0;36m",
    [HL_STRING] = "\033[0;35m",
    [HL_NUMBER] = "\033[0;31m",
    [HL_KEYWORD] = "\033[0;33m",
    [HL_TYPE] = "\033[0;32m",
    [HL_PREPROC] = "\033[0;1;35m",
    [HL_VAR] = "\033[0;36m",
    [HL_SEARCH] = "\033[0;30;43m",
    [HL_VISUAL] = "\033[0;7m",
    [HL_SPECIAL] = "\033[0;34m",
};

static const char *const c_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
    "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern",
    "const", "volatile", "inline", "register", "auto", "restrict", "_Static_assert",
    "_Alignof", "_Generic", "asm", "__asm__", "__attribute__", "class", "public", "private",
    "new", "this", "true", "false", "NULL", 0,
};
static const char *const c_types[] = {
    "int", "char", "short", "long", "unsigned", "signed", "float", "double", "void", "bool",
    "_Bool", "size_t", "ssize_t", "off_t", "pid_t", "uid_t", "gid_t", "mode_t", "time_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "uintptr_t", "intptr_t", "FILE", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
    "va_list", "regex_t", 0,
};
static const char *const sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac",
    "in", "function", "return", "local", "export", "readonly", "typeset", "alias", "source",
    "exit", "break", "continue", "setopt", "unsetopt", "repeat", "time", "eval", "exec", "set",
    "unset", "shift", 0,
};
static const char *const py_keywords[] = {
    "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from", "as",
    "pass", "break", "continue", "try", "except", "finally", "with", "lambda", "yield", "None",
    "True", "False", "and", "or", "not", "in", "is", "raise", "global", "nonlocal", "assert",
    "del", "async", "await", "self", 0,
};

static int in_list(const char *s, int n, const char *const *l)
{
    for (int i = 0; l[i]; i++)
        if ((int)strlen(l[i]) == n && !strncmp(l[i], s, n))
            return 1;
    return 0;
}

/* Attributes for line y given the state at its start; returns the state at its end
 * (1: inside a C block comment). */
static int hl_line(struct buffer *b, int y, unsigned char *attr, int state)
{
    struct line *l = &b->lines[y];
    const char *s = l->s;
    int n = l->len;
    memset(attr, HL_NORMAL, n + 1);
    if (!vopt.highlight || b->syntax == SYN_NONE)
        return 0;
    int i = 0;
    if (b->syntax == SYN_C) {
        int k = 0;
        while (k < n && (s[k] == ' ' || s[k] == '\t'))
            k++;
        if (!state && k < n && s[k] == '#') {
            /* preprocessor: the directive, then normal highlighting for the rest */
            int e = k + 1;
            while (e < n && (s[e] == ' ' || is_word_char((unsigned char)s[e])))
                e++;
            memset(attr + k, HL_PREPROC, e - k);
            if (e - k >= 8 && !strncmp(s + k, "#include", 8)) {
                memset(attr + e, HL_STRING, n - e);
                return 0;
            }
            i = e;
        }
    }
    int hash_comments = b->syntax != SYN_C;
    while (i < n) {
        char c = s[i];
        if (state == 1) {               /* inside a C block comment */
            attr[i] = HL_COMMENT;
            if (c == '*' && i + 1 < n && s[i + 1] == '/') {
                attr[i + 1] = HL_COMMENT;
                i += 2;
                state = 0;
                continue;
            }
            i++;
            continue;
        }
        if (b->syntax == SYN_C && c == '/' && i + 1 < n && s[i + 1] == '/') {
            memset(attr + i, HL_COMMENT, n - i);
            break;
        }
        if (b->syntax == SYN_C && c == '/' && i + 1 < n && s[i + 1] == '*') {
            attr[i] = attr[i + 1] = HL_COMMENT;
            i += 2;
            state = 1;
            continue;
        }
        if (hash_comments && c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            memset(attr + i, HL_COMMENT, n - i);
            break;
        }
        if (c == '"' || c == '\'' || (c == '`' && b->syntax != SYN_C)) {
            int j = i + 1;
            while (j < n && s[j] != c) {
                if (s[j] == '\\' && c != '\'' && j + 1 < n)
                    j++;
                else if (s[j] == '\\' && b->syntax == SYN_C && j + 1 < n)
                    j++;
                j++;
            }
            int e = j < n ? j + 1 : n;
            memset(attr + i, HL_STRING, e - i);
            i = e;
            continue;
        }
        if ((b->syntax == SYN_SHELL || b->syntax == SYN_MAKE) && c == '$') {
            int j = i + 1;
            if (j < n && (s[j] == '(' || s[j] == '{')) {
                char close = s[j] == '(' ? ')' : '}';
                while (j < n && s[j] != close)
                    j++;
                j = j < n ? j + 1 : n;
            } else {
                while (j < n && (is_word_char((unsigned char)s[j]) || (j == i + 1 && strchr("?$!#@*-", s[j]))))
                    j++;
            }
            memset(attr + i, HL_VAR, j - i);
            i = j;
            continue;
        }
        if ((c >= '0' && c <= '9') && (i == 0 || !is_word_char((unsigned char)s[i - 1]))) {
            int j = i;
            while (j < n && (is_word_char((unsigned char)s[j]) || s[j] == '.'))
                j++;
            memset(attr + i, HL_NUMBER, j - i);
            i = j;
            continue;
        }
        if (is_word_char((unsigned char)c)) {
            int j = i;
            while (j < n && is_word_char((unsigned char)s[j]))
                j++;
            int h = HL_NORMAL;
            if (b->syntax == SYN_C)
                h = in_list(s + i, j - i, c_keywords) ? HL_KEYWORD : in_list(s + i, j - i, c_types) ? HL_TYPE : 0;
            else if (b->syntax == SYN_SHELL)
                h = in_list(s + i, j - i, sh_keywords) ? HL_KEYWORD : 0;
            else if (b->syntax == SYN_PYTHON)
                h = in_list(s + i, j - i, py_keywords) ? HL_KEYWORD : 0;
            else if (b->syntax == SYN_MAKE && j < n && s[j] == ':' && i == 0)
                h = HL_TYPE;            /* a target */
            memset(attr + i, h, j - i);
            i = j;
            continue;
        }
        i++;
    }
    return state;
}

/* The syntax state at the start of line y: lines[k].hl_in is valid for k < hl_valid. */
static int hl_state_at(struct buffer *b, int y)
{
    if (!vopt.highlight || b->syntax != SYN_C)
        return 0;
    static unsigned char *scratch;
    static int scap;
    if (b->hl_valid == 0) {
        b->lines[0].hl_in = 0;
        b->hl_valid = 1;
    }
    while (b->hl_valid <= y && b->hl_valid < b->nlines) {
        int k = b->hl_valid - 1;
        if (b->lines[k].len + 1 > scap) {
            scap = b->lines[k].len + 64;
            scratch = xrealloc(scratch, scap);
        }
        b->lines[k + 1].hl_in = hl_line(b, k, scratch, b->lines[k].hl_in);
        b->hl_valid++;
    }
    return y < b->nlines ? b->lines[y].hl_in : 0;
}

/* ---- scrolling ---- */

void scroll_to_cursor(void)
{
    struct buffer *b = cur;
    if (b->cy >= b->nlines)
        b->cy = b->nlines - 1;
    if (b->cy < 0)
        b->cy = 0;
    if (b->cy < b->top)
        b->top = b->cy;
    int tr = text_rows();
    for (;;) {
        int used = 0;
        for (int y = b->top; y <= b->cy; y++)
            used += line_rows(b, y);
        if (used <= tr || b->top >= b->cy)
            break;
        b->top++;
    }
}

/* ---- drawing ---- */

static int in_visual(int y, int x)
{
    if (!vis_mode)
        return 0;
    int y1 = vis_y1, x1 = vis_x1, y2 = vis_y2, x2 = vis_x2;
    if (y1 > y2 || (y1 == y2 && x1 > x2)) {
        int t = y1; y1 = y2; y2 = t;
        t = x1; x1 = x2; x2 = t;
    }
    if (y < y1 || y > y2)
        return 0;
    if (vis_mode == 2)
        return 1;
    if (vis_mode == 3) {                /* exclusive end (emacs region) */
        if (y == y1 && x < x1)
            return 0;
        if (y == y2 && x >= x2)
            return 0;
        return 1;
    }
    if (y == y1 && x < x1)
        return 0;
    if (y == y2 && x > x2)
        return 0;
    return 1;
}

void draw(void)
{
    struct buffer *b = cur;
    scroll_to_cursor();
    int tr = text_rows(), G = gutter(b), W = width_of(b);
    int crow = 0, ccol = 0;
    out("\033[?25l\033[H");

    regex_t re;
    int have_re = 0;
    if (vopt.hlsearch && !search_hl_off && last_pattern[0])
        have_re = regcomp(&re, last_pattern, search_icase(last_pattern) ? REG_ICASE : 0) == 0;

    static unsigned char *attr;
    static int acap;
    int row = 0, y = b->top;
    int state = hl_state_at(b, y);
    while (row < tr) {
        if (y >= b->nlines) {
            out(show_tildes ? "\033[0;34m~\033[0m\033[K\r\n" : "\033[K\r\n");
            row++;
            continue;
        }
        struct line *l = &b->lines[y];
        if (l->len + 2 > acap) {
            acap = l->len + 64;
            attr = xrealloc(attr, acap);
        }
        b->lines[y].hl_in = state;
        int next_state = hl_line(b, y, attr, state);
        if (y + 1 < b->nlines && b->hl_valid == y + 1) {
            b->lines[y + 1].hl_in = next_state;
            b->hl_valid = y + 2;
        }
        state = next_state;
        if (have_re) {
            regmatch_t m;
            int pos = 0;
            char *line = xstrndup(l->s, l->len);
            while (pos <= l->len) {
                m.rm_so = pos;
                m.rm_eo = l->len;
                if (regexec(&re, line, 1, &m, REG_STARTEND | (pos ? REG_NOTBOL : 0)))
                    break;
                for (long k = m.rm_so; k < m.rm_eo; k++)
                    attr[k] = HL_SEARCH;
                pos = m.rm_eo > m.rm_so ? (int)m.rm_eo : (int)m.rm_eo + 1;
            }
            free(line);
        }
        /* gutter */
        if (G)
            outf("\033[0;33m%*d \033[0m", G - 1, y + 1);
        int col = 0, cur_attr = -1;
        int vis_line = vis_mode == 2 && in_visual(y, 0);
        for (int i = 0; i <= l->len;) {
            if (y == b->cy && i == b->cx) {
                crow = row + col / W;
                ccol = G + col % W;
                if (col && col % W == 0 && i == l->len) {
                    /* the cursor sits at the margin: show it at the next row's start */
                }
            }
            if (i == l->len) {
                if (vopt.list) {
                    if (cur_attr != HL_SPECIAL)
                        out(hl_sgr[HL_SPECIAL]);
                    out("$");
                    col++;
                    cur_attr = HL_SPECIAL;
                }
                if (vis_line && col == 0) {
                    out("\033[0;7m \033[0m");
                    col++;
                }
                break;
            }
            int nb, w = cell_width(l->s, l->len, i, col, &nb);
            int a = attr[i];
            if (in_visual(y, i))
                a = HL_VISUAL;
            if (a != cur_attr) {
                out(hl_sgr[a]);
                cur_attr = a;
            }
            for (int k = 0; k < w; k++) {
                if (col && col % W == 0) {
                    /* wrap: continue on the next row */
                    out("\033[0m\033[K\r\n");
                    row++;
                    if (row >= tr)
                        break;
                    if (G)
                        outf("%*s", G, "");
                    out(hl_sgr[a]);
                }
                unsigned char c = l->s[i];
                if (c == '\t')
                    out(vopt.list ? (k ? "I" : "^") : " ");
                else if (c < 32 || c == 127) {
                    char t[2] = { k ? (c == 127 ? '?' : c + 64) : '^', 0 };
                    if (cur_attr != HL_SPECIAL && !k)
                        out(hl_sgr[HL_SPECIAL]);
                    out(t);
                } else
                    outn(l->s + i, nb);
                col++;
            }
            i += nb;
            if (row >= tr)
                break;
        }
        out("\033[0m\033[K\r\n");
        row++;
        y++;
    }
    if (have_re)
        regfree(&re);
    /* mode line */
    if (vopt.status_line) {
        char st[256];
        st[0] = 0;
        if (draw_status)
            draw_status(st, sizeof(st));
        int n = strlen(st);
        out("\033[0;7m");
        outn(st, n < cols ? n : cols);
        for (int k = n; k < cols; k++)
            out(" ");
        out("\033[0m\r\n");
    }
    /* bottom line: a message, or the personality's text */
    out("\033[K");
    if (message[0]) {
        if (message_error)
            out("\033[0;1;37;41m");
        int n = strlen(message);
        outn(message, n < cols - 1 ? n : cols - 1);
        out("\033[0m");
    } else if (draw_bottom) {
        const char *t = draw_bottom();
        int n = strlen(t);
        outn(t, n < cols - 1 ? n : cols - 1);
    }
    if (crow >= tr)
        crow = tr - 1;
    outf("\033[%d;%dH\033[?25h", crow + 1, ccol + 1);
    out_flush();
}
