/*
 * edit: the vi personality.
 *
 * Normal mode: counts, registers ("a), motions (h j k l w b e W B E 0 ^ $ gg G
 * H M L f F t T ; , % { } n N * # ' ` + - Enter |), operators (d c y > < gu gU g~)
 * with a motion, doubled (dd) or a text object (iw aw i" a" i( a( i{ a{ i[ a[ ip),
 * x X D C s S r R J ~ p P u ^R . i a I A o O v V : / ? m q @ ZZ ZQ ^G ^L
 * ^F ^B ^D ^U ^E ^Y zz zt zb &.
 * Insert mode: text, Enter (keeps indentation), BS ^W ^U ^R{reg} Tab, arrows.
 * Ex: :w :q :wq :x :e :r :s :g :v :d :y :m :t :j :> :< :set :noh :!cmd :bn ...
 */
#include "edit.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { NORMAL, INSERT, REPLACE, VISUAL, VLINE };

static int mode = NORMAL;
static int quit_now;

/* options */
static int opt_ai = 1, opt_sw = 4, opt_et = 1;

/* ---- registers ---- */

struct reg {
    char *text;
    int len;
    int linewise;
};
static struct reg regs[28];             /* 0-25 a-z, 26 unnamed, 27 "0 (last yank) */
static int cur_reg = -1;                /* "x given for this command */

static struct reg *reg_get(int r)
{
    return &regs[r < 0 ? 26 : r];
}

static void reg_set(int r, const char *t, int len, int linewise, int is_yank)
{
    struct reg *dst[3];
    int n = 0;
    dst[n++] = &regs[26];
    if (r >= 0 && r < 26)
        dst[n++] = &regs[r];
    if (is_yank)
        dst[n++] = &regs[27];
    for (int i = 0; i < n; i++) {
        free(dst[i]->text);
        dst[i]->text = xstrndup(t, len);
        dst[i]->len = len;
        dst[i]->linewise = linewise;
    }
}

/* ---- input: replays (., @) and recordings (., q) ---- */

static int queue[4096];
static int qhead, qlen;
static int cmd_keys[1024], ncmd;        /* the current command, for . */
static int dot_keys[1024], ndot;
static int changed;                     /* the current command modified the buffer */
static int macro_reg = -1;              /* q recording into this register */
static struct reg macro_buf;
static int last_macro = -1;

static void queue_keys(const int *k, int n)
{
    /* replayed keys go before anything already queued */
    if (qlen + n > 4096)
        n = 4096 - qlen;
    memmove(queue + n, queue + qhead, qlen * sizeof(int));
    memcpy(queue, k, n * sizeof(int));
    qhead = 0;
    qlen += n;
}

static int vk(void)
{
    int k;
    if (qlen) {
        k = queue[qhead++];
        qlen--;
        if (!qlen)
            qhead = 0;
    } else {
        static int pushback = -1;
        if (pushback >= 0) {
            k = pushback;
            pushback = -1;
        } else {
            k = term_getkey();
        }
        if (k & K_META) {
            /* vi has no Meta keys: ESC typed quickly before a key */
            pushback = k & ~K_META;
            k = K_ESC;
        }
        if (macro_reg >= 0 && k != K_RESIZE) {
            macro_buf.text = xrealloc(macro_buf.text, (macro_buf.len + 2) * sizeof(int));
            ((int *)macro_buf.text)[macro_buf.len++] = k;
        }
    }
    if (ncmd < 1024 && k != K_RESIZE)
        cmd_keys[ncmd++] = k;
    return k;
}

/* ---- cursor helpers ---- */

static struct line *L(int y)
{
    return &cur->lines[y];
}

static void clamp(void)
{
    struct buffer *b = cur;
    if (b->cy >= b->nlines)
        b->cy = b->nlines - 1;
    if (b->cy < 0)
        b->cy = 0;
    int len = L(b->cy)->len;
    int max = mode == INSERT || mode == REPLACE ? len : (len ? utf8_prev(L(b->cy)->s, len) : 0);
    if (mode == VISUAL || mode == VLINE)
        max = len ? utf8_prev(L(b->cy)->s, len) : 0;
    if (b->cx > max)
        b->cx = max;
    if (b->cx < 0)
        b->cx = 0;
}

static void set_want(void)
{
    cur->want_col = screen_col(cur, cur->cy, cur->cx);
}

static int first_nonblank(int y)
{
    int n;
    line_indent_end(L(y), &n);
    return n;
}

static int char_class(int c)
{
    if (c == ' ' || c == '\t' || c == 0)
        return 0;
    return is_word_char(c) ? 1 : 2;
}

/* ---- motions ---- */

struct pos {
    int y, x;
};

static int at(struct pos p)
{
    struct line *l = L(p.y);
    return p.x < l->len ? (unsigned char)l->s[p.x] : (p.y < cur->nlines - 1 ? '\n' : 0);
}

static int step_fwd(struct pos *p)
{
    if (p->x < L(p->y)->len) {
        p->x = utf8_next(L(p->y)->s, L(p->y)->len, p->x);
        if (p->x < L(p->y)->len || p->y == cur->nlines - 1)
            return 1;
    }
    if (p->y >= cur->nlines - 1)
        return 0;
    p->y++;
    p->x = 0;
    return 1;
}

static int step_back(struct pos *p)
{
    if (p->x > 0) {
        p->x = utf8_prev(L(p->y)->s, p->x);
        return 1;
    }
    if (p->y == 0)
        return 0;
    p->y--;
    p->x = L(p->y)->len;
    return 1;
}

static int cls(struct pos p, int big)
{
    int c = at(p);
    if (c == '\n' || c == 0)
        return 0;
    int k = char_class(c);
    return big && k ? 1 : k;
}

static struct pos word_fwd(struct pos p, int big, int stop_at_eol)
{
    int c0 = cls(p, big);
    /* skip the rest of this word */
    if (c0) {
        while (cls(p, big) == c0 && at(p) != '\n')
            if (!step_fwd(&p))
                return p;
    }
    /* then blanks (an empty line counts as a word) */
    while (cls(p, big) == 0) {
        if (stop_at_eol && at(p) == '\n')
            return p;
        int was_eol = at(p) == '\n';
        if (!step_fwd(&p))
            return p;
        if (was_eol && L(p.y)->len == 0)
            return p;
    }
    return p;
}

static struct pos word_end(struct pos p, int big)
{
    if (!step_fwd(&p))
        return p;
    while (cls(p, big) == 0)
        if (!step_fwd(&p))
            return p;
    int c = cls(p, big);
    for (;;) {
        struct pos q = p;
        if (!step_fwd(&q) || cls(q, big) != c || q.y != p.y)
            return p;
        p = q;
    }
}

static struct pos word_back(struct pos p, int big)
{
    if (!step_back(&p))
        return p;
    while (cls(p, big) == 0) {
        if (p.x == 0 && L(p.y)->len == 0)
            return p;
        if (!step_back(&p))
            return p;
    }
    int c = cls(p, big);
    for (;;) {
        struct pos q = p;
        if (!step_back(&q) || cls(q, big) != c || q.y != p.y)
            return p;
        p = q;
    }
}

static int last_f_char, last_f_kind;    /* f F t T for ; and , */

static int find_char(int kind, int ch, int count, struct pos *p)
{
    struct line *l = L(p->y);
    int x = p->x;
    for (int n = 0; n < count; n++) {
        if (kind == 'f' || kind == 't') {
            int start = x + 1 + (kind == 't' && n == 0 && x + 1 < l->len && l->s[x + 1] == ch ? 1 : 0);
            int i = start;
            while (i < l->len && l->s[i] != ch)
                i++;
            if (i >= l->len)
                return 0;
            x = i;
        } else {
            int start = x - 1 - (kind == 'T' && n == 0 && x - 1 >= 0 && l->s[x - 1] == ch ? 1 : 0);
            int i = start;
            while (i >= 0 && l->s[i] != ch)
                i--;
            if (i < 0)
                return 0;
            x = i;
        }
    }
    if (kind == 't')
        x--;
    else if (kind == 'T')
        x++;
    p->x = x;
    return 1;
}

static int match_pair(struct pos *p)
{
    static const char pairs[] = "()[]{}";
    struct line *l = L(p->y);
    int x = p->x;
    while (x < l->len && !strchr(pairs, l->s[x]))
        x++;
    if (x >= l->len)
        return 0;
    char c = l->s[x];
    int idx = strchr(pairs, c) - pairs;
    char open = pairs[idx & ~1], close = pairs[idx | 1];
    int dir = idx & 1 ? -1 : 1, depth = 0;
    struct pos q = { p->y, x };
    for (;;) {
        int ch = at(q);
        if (ch == open)
            depth += dir;
        else if (ch == close)
            depth -= dir;
        if (depth == 0) {
            *p = q;
            return 1;
        }
        if (dir > 0 ? !step_fwd(&q) : !step_back(&q))
            return 0;
    }
}

static void do_search(const char *pat, int dir, int count)
{
    struct buffer *b = cur;
    search_hl_off = 0;
    int y = b->cy, x = b->cx, len;
    for (int i = 0; i < count; i++) {
        int r = search_find(b, pat, dir, y, x, &y, &x, &len, 1, search_icase(pat));
        if (r <= 0) {
            if (r == 0)
                err("E486: Pattern not found: %s", pat);
            return;
        }
    }
    if ((dir > 0 && (y < b->cy || (y == b->cy && x <= b->cx))) ||
        (dir < 0 && (y > b->cy || (y == b->cy && x >= b->cx))))
        msg("search hit %s, continuing at %s", dir > 0 ? "BOTTOM" : "TOP", dir > 0 ? "TOP" : "BOTTOM");
    b->cy = y;
    b->cx = x;
}

/* Text under the cursor for * and #: a \<word\> pattern. */
static int word_under_cursor(char *out, int size)
{
    struct line *l = L(cur->cy);
    int s = cur->cx;
    while (s < l->len && !is_word_char((unsigned char)l->s[s]))
        s++;
    if (s >= l->len)
        return 0;
    int e = s;
    while (s > 0 && is_word_char((unsigned char)l->s[s - 1]))
        s--;
    while (e < l->len && is_word_char((unsigned char)l->s[e]))
        e++;
    snprintf(out, size, "\\<%.*s\\>", e - s, l->s + s);
    return 1;
}

/* A motion from the cursor. Returns 0 if it fails; sets the target and
 * whether the range is linewise / inclusive. */
static int motion(int k, int count, int has_count, struct pos *to, int *linewise, int *inclusive, int for_op)
{
    struct buffer *b = cur;
    struct pos p = { b->cy, b->cx };
    *linewise = 0;
    *inclusive = 0;
    int n = count ? count : 1;
    switch (k) {
    case 'h': case K_LEFT: case 127: case CTRL('H'):
        if (p.x == 0)
            return 0;
        for (int i = 0; i < n && p.x > 0; i++)
            p.x = utf8_prev(L(p.y)->s, p.x);
        break;
    case 'l': case K_RIGHT: case ' ': {
        int len = L(p.y)->len;
        if (p.x >= len - (for_op ? 0 : 1) && !(for_op && p.x < len))
            return 0;
        for (int i = 0; i < n && p.x < len; i++)
            p.x = utf8_next(L(p.y)->s, len, p.x);
        if (!for_op && p.x >= len)
            p.x = len ? utf8_prev(L(p.y)->s, len) : 0;
        break;
    }
    case 'j': case K_DOWN: case CTRL('N'):
        if (p.y + n >= b->nlines && p.y == b->nlines - 1)
            return 0;
        p.y = p.y + n < b->nlines ? p.y + n : b->nlines - 1;
        p.x = col_to_x(b, p.y, b->want_col);
        *linewise = 1;
        break;
    case 'k': case K_UP: case CTRL('P'):
        if (p.y == 0)
            return 0;
        p.y = p.y - n > 0 ? p.y - n : 0;
        p.x = col_to_x(b, p.y, b->want_col);
        *linewise = 1;
        break;
    case '+': case '\r': case '\n':
        if (p.y + n >= b->nlines)
            return 0;
        p.y += n;
        p.x = first_nonblank(p.y);
        *linewise = 1;
        break;
    case '-':
        if (p.y - n < 0)
            return 0;
        p.y -= n;
        p.x = first_nonblank(p.y);
        *linewise = 1;
        break;
    case 'w': case 'W':
        for (int i = 0; i < n; i++) {
            struct pos q = word_fwd(p, k == 'W', for_op && i == n - 1);
            if (q.y == p.y && q.x == p.x)
                break;
            p = q;
        }
        if (for_op && p.y > b->cy && p.x == 0 && L(b->cy)->len) {
            /* cw/dw on the last word of a line stops at its end */
            p.y = b->cy;
            p.x = L(p.y)->len;
        }
        break;
    case 'b': case 'B':
        for (int i = 0; i < n; i++)
            p = word_back(p, k == 'B');
        break;
    case 'e': case 'E':
        for (int i = 0; i < n; i++)
            p = word_end(p, k == 'E');
        *inclusive = 1;
        break;
    case '0': case K_HOME:
        p.x = 0;
        break;
    case '^':
        p.x = first_nonblank(p.y);
        break;
    case '$': case K_END:
        p.y = p.y + n - 1 < b->nlines ? p.y + n - 1 : b->nlines - 1;
        p.x = L(p.y)->len ? (for_op ? L(p.y)->len : utf8_prev(L(p.y)->s, L(p.y)->len)) : 0;
        b->want_col = 1 << 30;
        *inclusive = 0;
        break;
    case '|':
        p.x = col_to_x(b, p.y, n - 1);
        break;
    case 'G':
        p.y = has_count ? (count - 1 < b->nlines ? count - 1 : b->nlines - 1) : b->nlines - 1;
        p.x = first_nonblank(p.y);
        *linewise = 1;
        break;
    case 'H': case 'L': case 'M': {
        int tr = text_rows();
        int bottom = b->top + tr - 1 < b->nlines - 1 ? b->top + tr - 1 : b->nlines - 1;
        p.y = k == 'H' ? b->top + n - 1 : k == 'L' ? bottom - (n - 1) : (b->top + bottom) / 2;
        if (p.y > bottom) p.y = bottom;
        if (p.y < b->top) p.y = b->top;
        p.x = first_nonblank(p.y);
        *linewise = 1;
        break;
    }
    case '{': case '}': {
        int dir = k == '}' ? 1 : -1;
        for (int i = 0; i < n; i++) {
            int y = p.y + dir;
            while (y >= 0 && y < b->nlines && L(y)->len == 0)
                y += dir;
            while (y >= 0 && y < b->nlines && L(y)->len != 0)
                y += dir;
            p.y = y < 0 ? 0 : y >= b->nlines ? b->nlines - 1 : y;
        }
        p.x = k == '}' && p.y == b->nlines - 1 && L(p.y)->len ? L(p.y)->len - (for_op ? 0 : 1) : 0;
        break;
    }
    case '%':
        if (!match_pair(&p))
            return 0;
        *inclusive = 1;
        break;
    case 'f': case 'F': case 't': case 'T': {
        int ch = vk();
        if (ch == K_ESC || ch > 255)
            return 0;
        last_f_char = ch;
        last_f_kind = k;
        if (!find_char(k, ch, n, &p))
            return 0;
        *inclusive = k == 'f' || k == 't';
        break;
    }
    case ';': case ',': {
        if (!last_f_kind)
            return 0;
        int kind = last_f_kind;
        if (k == ',')
            kind = kind == 'f' ? 'F' : kind == 'F' ? 'f' : kind == 't' ? 'T' : 't';
        if (!find_char(kind, last_f_char, n, &p))
            return 0;
        *inclusive = kind == 'f' || kind == 't';
        break;
    }
    case 'n': case 'N': {
        if (!last_pattern[0]) {
            err("E35: No previous regular expression");
            return 0;
        }
        int sy = b->cy, sx = b->cx;
        do_search(last_pattern, k == 'n' ? last_dir : -last_dir, n);
        p.y = b->cy, p.x = b->cx;
        b->cy = sy, b->cx = sx;
        break;
    }
    case '*': case '#': {
        char pat[200];
        if (!word_under_cursor(pat, sizeof(pat)))
            return 0;
        snprintf(last_pattern, sizeof(last_pattern), "%s", pat);
        last_dir = k == '*' ? 1 : -1;
        int sy = b->cy, sx = b->cx;
        do_search(pat, last_dir, n);
        p.y = b->cy, p.x = b->cx;
        b->cy = sy, b->cx = sx;
        break;
    }
    case '\'': case '`': {
        int m = vk();
        if (m == '\'' || m == '`') {
            p.y = b->mark_y;        /* the previous context */
            p.x = b->mark_x;
        } else if (m >= 'a' && m <= 'z' && b->vmarks[m - 'a'][0] >= 0) {
            p.y = b->vmarks[m - 'a'][0];
            p.x = b->vmarks[m - 'a'][1];
        } else {
            err("E20: Mark not set");
            return 0;
        }
        if (p.y >= b->nlines)
            p.y = b->nlines - 1;
        if (k == '\'') {
            p.x = first_nonblank(p.y);
            *linewise = 1;
        }
        break;
    }
    case 'g': {
        int k2 = vk();
        if (k2 == 'g') {
            p.y = has_count ? (count - 1 < b->nlines ? count - 1 : b->nlines - 1) : 0;
            p.x = first_nonblank(p.y);
            *linewise = 1;
        } else if (k2 == 'e' || k2 == 'E') {
            /* back to the end of the previous word */
            for (int i = 0; i < n; i++) {
                p = word_back(p, k2 == 'E');
                struct pos q = p;
                if (step_back(&q))
                    p = q;
                while (cls(p, k2 == 'E') == 0 && step_back(&p))
                    ;
            }
            *inclusive = 1;
        } else if (k2 == '_') {
            p.x = L(p.y)->len;
            while (p.x > 0 && (L(p.y)->s[p.x - 1] == ' ' || L(p.y)->s[p.x - 1] == '\t'))
                p.x--;
            if (p.x)
                p.x--;
            *inclusive = 1;
        } else {
            return 0;
        }
        break;
    }
    default:
        return 0;
    }
    *to = p;
    return 1;
}

/* ---- text objects (after an operator or in visual mode) ---- */

static int text_object(int kind, int obj, struct pos *s, struct pos *e, int *linewise)
{
    struct buffer *b = cur;
    struct line *l = L(b->cy);
    *linewise = 0;
    int x = b->cx;
    if (obj == 'w' || obj == 'W') {
        if (!l->len)
            return 0;
        int big = obj == 'W';
        int c = x < l->len ? (big ? (char_class((unsigned char)l->s[x]) ? 1 : 0) : char_class((unsigned char)l->s[x])) : 0;
        int a = x, z = x;
        while (a > 0) {
            int cc = big ? (char_class((unsigned char)l->s[a - 1]) ? 1 : 0) : char_class((unsigned char)l->s[a - 1]);
            if (cc != c)
                break;
            a--;
        }
        while (z < l->len) {
            int cc = big ? (char_class((unsigned char)l->s[z]) ? 1 : 0) : char_class((unsigned char)l->s[z]);
            if (cc != c)
                break;
            z++;
        }
        if (kind == 'a') {
            int z2 = z;
            while (z2 < l->len && (l->s[z2] == ' ' || l->s[z2] == '\t'))
                z2++;
            if (z2 > z)
                z = z2;
            else
                while (a > 0 && (l->s[a - 1] == ' ' || l->s[a - 1] == '\t'))
                    a--;
        }
        s->y = e->y = b->cy;
        s->x = a;
        e->x = z;               /* exclusive */
        return 1;
    }
    if (obj == '"' || obj == '\'' || obj == '`') {
        int a = x, z;
        /* the quotes around (or the first pair after) the cursor */
        int left = -1;
        for (int i = 0; i <= x && i < l->len; i++)
            if (l->s[i] == obj && (i == 0 || l->s[i - 1] != '\\'))
                left = left < 0 ? i : (i == x ? left : -1 == left ? i : (left = -2 == left ? i : left), left);
        /* simpler: scan pairs from the line start */
        int open = -1, cl = -1;
        for (int i = 0; i < l->len; i++) {
            if (l->s[i] != obj || (i && l->s[i - 1] == '\\'))
                continue;
            if (open < 0) {
                open = i;
            } else {
                if (open <= x && x <= i) {
                    cl = i;
                    break;
                }
                if (open > x) {
                    cl = i;
                    break;
                }
                open = -1;
            }
        }
        (void)a;
        (void)left;
        if (open < 0 || cl < 0)
            return 0;
        z = cl;
        s->y = e->y = b->cy;
        if (kind == 'i') {
            s->x = open + 1;
            e->x = z;
        } else {
            s->x = open;
            e->x = z + 1;
        }
        return 1;
    }
    const char *pairs = "()b[]{}B<>";
    const char *pc = strchr(pairs, obj);
    if (pc) {
        char open, close;
        if (obj == 'b' || obj == '(' || obj == ')') open = '(', close = ')';
        else if (obj == 'B' || obj == '{' || obj == '}') open = '{', close = '}';
        else if (obj == '[' || obj == ']') open = '[', close = ']';
        else open = '<', close = '>';
        /* search backward for the unmatched opener */
        struct pos p = { b->cy, b->cx };
        int depth = 0;
        if (at(p) == close)
            step_back(&p);
        for (;;) {
            int c = at(p);
            if (c == close)
                depth++;
            else if (c == open) {
                if (depth == 0)
                    break;
                depth--;
            }
            if (!step_back(&p))
                return 0;
        }
        struct pos q = p;
        if (!match_pair(&q))
            return 0;
        if (kind == 'i') {
            step_fwd(&p);
            *s = p;
            *e = q;
            /* a block that spans lines: whole lines inside */
            if (s->x >= L(s->y)->len && s->y < e->y) {
                s->y++;
                s->x = 0;
                int n;
                line_indent_end(L(e->y), &n);
                if (n == e->x && e->y > s->y) {
                    e->y--;
                    e->x = L(e->y)->len;
                    *linewise = 1;
                }
            }
        } else {
            *s = p;
            step_fwd(&q);
            *e = q;
        }
        return 1;
    }
    if (obj == 'p') {
        int y1 = b->cy, y2 = b->cy;
        int blank = L(y1)->len == 0;
        while (y1 > 0 && (L(y1 - 1)->len == 0) == blank)
            y1--;
        while (y2 < b->nlines - 1 && (L(y2 + 1)->len == 0) == blank)
            y2++;
        if (kind == 'a')
            while (y2 < b->nlines - 1 && L(y2 + 1)->len == 0)
                y2++;
        s->y = y1, s->x = 0;
        e->y = y2, e->x = 0;
        *linewise = 1;
        return 1;
    }
    return 0;
}

/* ---- operators ---- */

static void shift_lines(int y1, int y2, int dir)
{
    struct buffer *b = cur;
    for (int y = y1; y <= y2; y++) {
        if (!L(y)->len)
            continue;
        if (dir > 0) {
            char pad[32];
            int n = opt_sw < 32 ? opt_sw : 31;
            if (opt_et)
                memset(pad, ' ', n);
            else
                pad[0] = '\t', n = 1;
            buf_insert(b, y, 0, pad, n, 0, 0);
        } else {
            int n = 0, col = 0;
            while (n < L(y)->len && col < opt_sw && (L(y)->s[n] == ' ' || L(y)->s[n] == '\t')) {
                col += L(y)->s[n] == '\t' ? vopt.tabstop : 1;
                n++;
            }
            if (n)
                free(buf_delete(b, y, 0, y, n));
        }
    }
    b->cy = y1;
    b->cx = first_nonblank(y1);
    changed = 1;
}

static void change_case(int y1, int x1, int y2, int x2, int how)
{
    struct buffer *b = cur;
    for (int y = y1; y <= y2; y++) {
        int a = y == y1 ? x1 : 0, z = y == y2 ? x2 : L(y)->len;
        if (z > L(y)->len)
            z = L(y)->len;
        if (a >= z)
            continue;
        char *t = buf_delete(b, y, a, y, z);
        for (int i = 0; i < z - a; i++) {
            unsigned char c = t[i];
            t[i] = how == 'u' ? tolower(c) : how == 'U' ? toupper(c) : islower(c) ? toupper(c) : tolower(c);
        }
        buf_insert(b, y, a, t, z - a, 0, 0);
        free(t);
    }
    b->cy = y1;
    b->cx = x1;
    changed = 1;
}

static void insert_mode(int replace);

/* Apply operator op to the range from (y1,x1) to (y2,x2), exclusive. */
static void apply_op(int op, struct pos s, struct pos e, int linewise)
{
    struct buffer *b = cur;
    if (s.y > e.y || (s.y == e.y && s.x > e.x)) {
        struct pos t = s;
        s = e;
        e = t;
    }
    if (op == '>' || op == '<') {
        shift_lines(s.y, e.y, op == '>' ? 1 : -1);
        return;
    }
    if (op == 'u' || op == 'U' || op == '~') {
        if (linewise)
            s.x = 0, e.x = L(e.y)->len;
        change_case(s.y, s.x, e.y, e.x, op);
        return;
    }
    if (linewise) {
        s.x = 0;
        if (e.y < b->nlines - 1) {
            e.y++;
            e.x = 0;
        } else {
            e.x = L(e.y)->len;
            if (s.y > 0 && op != 'y' && op != 'c') {
                /* deleting the last lines: take the newline before them */
                s.y--;
                s.x = L(s.y)->len;
            }
        }
    }
    int len;
    char *t = buf_text(b, s.y, s.x, e.y, e.x, &len);
    if (linewise && (len == 0 || t[len - 1] != '\n')) {
        /* registers always hold whole lines with their newlines */
        char *u = xmalloc(len + 2);
        int off = t[0] == '\n' && s.x > 0 ? 1 : 0;
        memcpy(u, t + off, len - off);
        u[len - off] = '\n';
        reg_set(cur_reg, u, len - off + 1, 1, op == 'y');
        free(u);
    } else {
        reg_set(cur_reg, t, len, linewise, op == 'y');
    }
    free(t);
    if (op == 'y') {
        b->cy = s.y + (linewise && s.x > 0 ? 1 : 0);
        b->cx = linewise ? b->cx : s.x;
        if (linewise && b->cy < s.y)
            b->cy = s.y;
        return;
    }
    if (op == 'c' && linewise) {
        /* cc: keep the indentation, replace the rest */
        int ind;
        line_indent_end(L(s.y), &ind);
        int ey = e.y > s.y && e.x == 0 ? e.y - 1 : e.y;
        free(buf_delete(b, s.y, opt_ai ? ind : 0, ey, L(ey)->len));
        b->cy = s.y;
        b->cx = opt_ai ? ind : 0;
        changed = 1;
        insert_mode(0);
        return;
    }
    free(buf_delete(b, s.y, s.x, e.y, e.x));
    changed = 1;
    b->cy = s.y < b->nlines ? s.y : b->nlines - 1;
    if (linewise) {
        if (s.x > 0 && b->cy + 1 < b->nlines)
            b->cy++;
        b->cx = first_nonblank(b->cy);
    } else {
        b->cx = s.x;
    }
    if (op == 'c')
        insert_mode(0);
}

/* ---- put ---- */

static void put(int after, int count)
{
    struct buffer *b = cur;
    struct reg *r = reg_get(cur_reg);
    if (!r->text) {
        err("E353: Nothing in register");
        return;
    }
    struct reg keys_as_text = { 0, 0, 0 };
    if (r->linewise == -1) {
        /* a recorded macro holds keys: put the ones that are characters */
        keys_as_text.text = xmalloc(r->len + 1);
        for (int i = 0; i < r->len; i++) {
            int k = ((int *)r->text)[i];
            if (k < 256)
                keys_as_text.text[keys_as_text.len++] = (char)k;
        }
        if (!keys_as_text.len) {
            free(keys_as_text.text);
            return;
        }
        r = &keys_as_text;
    }
    for (int i = 0; i < (count ? count : 1); i++) {
        if (r->linewise) {
            int y = after ? b->cy + 1 : b->cy;
            if (y >= b->nlines) {
                /* after the last line: add a newline first */
                int ey, ex;
                buf_insert(b, b->nlines - 1, L(b->nlines - 1)->len, "\n", 1, &ey, &ex);
                buf_insert(b, ey, 0, r->text, r->len - 1, 0, 0);
            } else {
                buf_insert(b, y, 0, r->text, r->len, 0, 0);
            }
            b->cy = y;
            b->cx = first_nonblank(y);
        } else {
            int x = after && L(b->cy)->len ? utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx) : b->cx;
            int ey, ex;
            buf_insert(b, b->cy, x, r->text, r->len, &ey, &ex);
            b->cy = ey;
            b->cx = ex > 0 ? ex - 1 : 0;
        }
    }
    free(keys_as_text.text);
    changed = 1;
}

/* ---- insert mode ---- */

static void insert_newline(void)
{
    struct buffer *b = cur;
    int ind = 0;
    char indent[128];
    if (opt_ai) {
        line_indent_end(L(b->cy), &ind);
        if (ind > b->cx)
            ind = b->cx;
        if (ind > 127)
            ind = 127;
        memcpy(indent, L(b->cy)->s, ind);
    }
    int ey, ex;
    buf_insert(b, b->cy, b->cx, "\n", 1, &ey, &ex);
    b->cy = ey;
    b->cx = 0;
    if (ind) {
        buf_insert(b, b->cy, 0, indent, ind, &ey, &ex);
        b->cx = ex;
    }
}

static void insert_mode(int replace)
{
    struct buffer *b = cur;
    mode = replace ? REPLACE : INSERT;
    changed = 1;
    char *replaced = 0;             /* R: what was overwritten, for backspace */
    int nreplaced = 0;
    for (;;) {
        clamp();
        draw();
        int k = vk();
        if (k == K_ESC || k == CTRL('C') || k == CTRL('[')) {
            if (b->cx > 0)
                b->cx = utf8_prev(L(b->cy)->s, b->cx);
            break;
        }
        if (k == K_RESIZE)
            continue;
        if (k == '\r' || k == '\n') {
            insert_newline();
        } else if (k == 127 || k == CTRL('H')) {
            if (replace && nreplaced) {
                /* put back what R overwrote */
                b->cx = utf8_prev(L(b->cy)->s, b->cx);
                free(buf_delete(b, b->cy, b->cx, b->cy, b->cx + 1));
                char c = replaced[--nreplaced];
                if (c)
                    buf_insert(b, b->cy, b->cx, &c, 1, 0, 0);
            } else if (b->cx > 0) {
                int p = utf8_prev(L(b->cy)->s, b->cx);
                free(buf_delete(b, b->cy, p, b->cy, b->cx));
                b->cx = p;
            } else if (b->cy > 0) {
                int px = L(b->cy - 1)->len;
                free(buf_delete(b, b->cy - 1, px, b->cy, 0));
                b->cy--;
                b->cx = px;
            }
        } else if (k == K_DEL) {
            if (b->cx < L(b->cy)->len)
                free(buf_delete(b, b->cy, b->cx, b->cy, utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx)));
        } else if (k == CTRL('W')) {
            int p = b->cx;
            while (p > 0 && (L(b->cy)->s[p - 1] == ' ' || L(b->cy)->s[p - 1] == '\t'))
                p--;
            if (p > 0 && is_word_char((unsigned char)L(b->cy)->s[p - 1]))
                while (p > 0 && is_word_char((unsigned char)L(b->cy)->s[p - 1]))
                    p--;
            else if (p > 0)
                p--;
            free(buf_delete(b, b->cy, p, b->cy, b->cx));
            b->cx = p;
        } else if (k == CTRL('U')) {
            int ind;
            line_indent_end(L(b->cy), &ind);
            int p = b->cx > ind ? ind : 0;
            free(buf_delete(b, b->cy, p, b->cy, b->cx));
            b->cx = p;
        } else if (k == CTRL('R')) {
            int r = vk();
            int idx = r == '"' ? 26 : r >= 'a' && r <= 'z' ? r - 'a' : r == '0' ? 27 : -2;
            if (idx >= 0 && regs[idx].text) {
                int ey, ex;
                buf_insert(b, b->cy, b->cx, regs[idx].text, regs[idx].len, &ey, &ex);
                b->cy = ey, b->cx = ex;
            }
        } else if (k == '\t') {
            if (opt_et) {
                int col = screen_col(b, b->cy, b->cx);
                int n = opt_sw - col % opt_sw;
                char sp[32];
                memset(sp, ' ', n);
                buf_insert(b, b->cy, b->cx, sp, n, 0, 0);
                b->cx += n;
            } else {
                buf_insert(b, b->cy, b->cx, "\t", 1, 0, 0);
                b->cx++;
            }
        } else if (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN || k == K_HOME || k == K_END) {
            undo_begin(b);                      /* moving starts a new undo step */
            if (k == K_LEFT && b->cx > 0) b->cx = utf8_prev(L(b->cy)->s, b->cx);
            if (k == K_RIGHT && b->cx < L(b->cy)->len) b->cx = utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx);
            if (k == K_UP && b->cy > 0) b->cy--;
            if (k == K_DOWN && b->cy < b->nlines - 1) b->cy++;
            if (k == K_HOME) b->cx = 0;
            if (k == K_END) b->cx = L(b->cy)->len;
        } else if (k == CTRL('V')) {
            int c = vk();
            if (c < 256) {
                char ch = (char)c;
                buf_insert(b, b->cy, b->cx, &ch, 1, 0, 0);
                b->cx++;
            }
        } else if (k >= 32 && k < 256) {
            /* collect a UTF-8 sequence */
            char ch[4] = { (char)k };
            int n = 1;
            if (k >= 0xc0) {
                int need = k >= 0xf0 ? 3 : k >= 0xe0 ? 2 : 1;
                for (int i = 0; i < need; i++)
                    ch[n++] = (char)vk();
            }
            if (replace && b->cx < L(b->cy)->len) {
                int e = utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx);
                char *old = buf_delete(b, b->cy, b->cx, b->cy, e);
                replaced = xrealloc(replaced, nreplaced + 1);
                replaced[nreplaced++] = old[0];
                free(old);
            } else if (replace) {
                replaced = xrealloc(replaced, nreplaced + 1);
                replaced[nreplaced++] = 0;
            }
            buf_insert(b, b->cy, b->cx, ch, n, 0, 0);
            b->cx += n;
        }
    }
    free(replaced);
    mode = NORMAL;
}

/* ---- ex commands ---- */

static int parse_addr(const char **pp, int *line)
{
    const char *p = *pp;
    struct buffer *b = cur;
    int have = 1;
    if (isdigit((unsigned char)*p)) {
        *line = (int)strtol(p, (char **)&p, 10) - 1;
    } else if (*p == '.') {
        *line = b->cy;
        p++;
    } else if (*p == '$') {
        *line = b->nlines - 1;
        p++;
    } else if (*p == '\'' && p[1]) {
        char m = p[1];
        p += 2;
        if (m == '<' || m == '>')
            *line = m == '<' ? (vis_y1 < vis_y2 ? vis_y1 : vis_y2) : (vis_y1 > vis_y2 ? vis_y1 : vis_y2);
        else if (m >= 'a' && m <= 'z' && b->vmarks[m - 'a'][0] >= 0)
            *line = b->vmarks[m - 'a'][0];
        else
            return -1;
    } else if (*p == '/' || *p == '?') {
        char d = *p++;
        char pat[200];
        int n = 0;
        while (*p && *p != d && n < 199) {
            if (*p == '\\' && p[1] == d)
                p++;
            pat[n++] = *p++;
        }
        pat[n] = 0;
        if (*p == d)
            p++;
        if (n)
            snprintf(last_pattern, sizeof(last_pattern), "%s", pat);
        int y, x, len;
        if (search_find(b, last_pattern, d == '/' ? 1 : -1, b->cy, d == '/' ? L(b->cy)->len : 0, &y, &x,
                        &len, 1, search_icase(last_pattern)) <= 0)
            return -1;
        *line = y;
    } else if (*p == '+' || *p == '-') {
        *line = b->cy;
    } else {
        have = 0;
    }
    while (*p == '+' || *p == '-') {
        int sign = *p++ == '+' ? 1 : -1;
        int n = isdigit((unsigned char)*p) ? (int)strtol(p, (char **)&p, 10) : 1;
        *line += sign * n;
    }
    *pp = p;
    return have;
}

static void ex_set(const char *arg)
{
    if (!*arg) {
        msg("ts=%d sw=%d %s %s %s %s %s %s %s", vopt.tabstop, opt_sw, opt_et ? "et" : "noet",
            opt_ai ? "ai" : "noai", vopt.numbers ? "nu" : "nonu", opt_ignorecase ? "ic" : "noic",
            opt_smartcase ? "scs" : "noscs", vopt.hlsearch ? "hls" : "nohls", vopt.list ? "list" : "nolist");
        return;
    }
    char word[64];
    while (*arg) {
        int n = 0;
        while (*arg == ' ')
            arg++;
        while (*arg && *arg != ' ' && n < 63)
            word[n++] = *arg++;
        word[n] = 0;
        if (!n)
            break;
        int on = 1;
        char *w = word;
        char *eq = strchr(w, '=');
        if (!strncmp(w, "no", 2) && !eq) {
            on = 0;
            w += 2;
        }
        int val = eq ? atoi(eq + 1) : 0;
        if (eq)
            *eq = 0;
        if (!strcmp(w, "nu") || !strcmp(w, "number")) vopt.numbers = on;
        else if (!strcmp(w, "ai") || !strcmp(w, "autoindent")) opt_ai = on;
        else if (!strcmp(w, "ic") || !strcmp(w, "ignorecase")) opt_ignorecase = on;
        else if (!strcmp(w, "scs") || !strcmp(w, "smartcase")) opt_smartcase = on;
        else if (!strcmp(w, "hls") || !strcmp(w, "hlsearch")) vopt.hlsearch = on;
        else if (!strcmp(w, "list")) vopt.list = on;
        else if (!strcmp(w, "et") || !strcmp(w, "expandtab")) opt_et = on;
        else if ((!strcmp(w, "ts") || !strcmp(w, "tabstop")) && val > 0 && val <= 16) vopt.tabstop = val;
        else if ((!strcmp(w, "sw") || !strcmp(w, "shiftwidth")) && val > 0 && val <= 16) opt_sw = val;
        else if (!strcmp(w, "syntax") || !strcmp(w, "syn")) vopt.highlight = on;
        else {
            err("E518: Unknown option: %s", word);
            return;
        }
    }
    cur->hl_valid = 0;
}

static struct buffer *open_file(const char *path)
{
    for (struct buffer *b = buffers; b; b = b->next)
        if (b->path && !strcmp(b->path, path))
            return b;
    struct buffer *b = buf_new(path);
    if (buf_load(b, path) < 0) {
        buf_free(b);
        return 0;
    }
    return b;
}

static void file_info(void)
{
    struct buffer *b = cur;
    msg("\"%s\"%s%s %d line%s --%d%%--", b->path ? b->path : "[No Name]", b->dirty ? " [Modified]" : "",
        b->readonly ? " [readonly]" : "", b->nlines, b->nlines == 1 ? "" : "s",
        b->nlines ? (b->cy + 1) * 100 / b->nlines : 0);
}

static void announce_loaded(struct buffer *b)
{
    if (b->newfile) {
        msg("\"%s\" [New]", b->path);
        return;
    }
    long bytes = 0;
    for (int i = 0; i < b->nlines; i++)
        bytes += b->lines[i].len + 1;
    msg("\"%s\"%s %dL, %ldB", b->path, b->readonly ? " [readonly]" : "", b->nlines, bytes);
}

static void ex_shell(const char *cmd)
{
    term_done();
    fflush_stdout();
    int st = system(cmd);
    printf("\n[%s; press ENTER]", st == 0 ? "done" : "command failed");
    fflush_stdout();
    term_init();
    term_getkey();
}

static void ex_command(char *line)
{
    struct buffer *b = cur;
    const char *p = line;
    while (*p == ' ' || *p == ':')
        p++;
    if (!*p)
        return;
    /* range */
    int y1 = b->cy, y2 = b->cy, nranges = 0;
    if (*p == '%') {
        y1 = 0;
        y2 = b->nlines - 1;
        nranges = 2;
        p++;
    } else {
        int a;
        int r = parse_addr(&p, &a);
        if (r < 0) {
            err("E16: Invalid range");
            return;
        }
        if (r) {
            y1 = y2 = a;
            nranges = 1;
        }
        if (*p == ',' || *p == ';') {
            p++;
            r = parse_addr(&p, &a);
            if (r < 0) {
                err("E16: Invalid range");
                return;
            }
            if (r) {
                y2 = a;
                nranges = 2;
            }
        }
    }
    if (y1 > y2) {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    /* an address past either end: fine for a bare ":N" (it goes as far as it can), an error otherwise */
    int out_of_range = y1 < 0 || y2 >= b->nlines || y1 >= b->nlines || y2 < 0;
    y1 = y1 < 0 ? 0 : y1 >= b->nlines ? b->nlines - 1 : y1;
    y2 = y2 < 0 ? 0 : y2 >= b->nlines ? b->nlines - 1 : y2;
    char cmd[32];
    int n = 0;
    if (*p == '!' || *p == '&' || *p == '<' || *p == '>' || *p == '=') {
        cmd[n++] = *p++;
        while (n < 31 && (*p == '<' || *p == '>') && cmd[0] != '!')
            cmd[n++] = *p++;
    } else {
        while (isalpha((unsigned char)*p) && n < 31)
            cmd[n++] = *p++;
    }
    cmd[n] = 0;
    int bang = 0;
    if (*p == '!' && cmd[0] != '!') {
        bang = 1;
        p++;
    }
    while (*p == ' ')
        p++;
    const char *arg = p;

    if (!n) {
        /* just a range: go to the line */
        b->cy = y2;
        b->cx = first_nonblank(y2);
        return;
    }
    if (out_of_range && nranges) {
        err("E16: Invalid range");
        return;
    }
#define IS(full, min) (!strncmp(cmd, full, n) && n >= (min))
    if (IS("write", 1) || IS("wq", 2) || IS("xit", 1) || IS("wall", 2) || IS("wqall", 3) || IS("xall", 2)) {
        int quit = cmd[0] == 'x' || !strcmp(cmd, "wq") || IS("wqall", 3) || IS("xall", 2);
        int all = IS("wall", 2) || IS("wqall", 3) || IS("xall", 2);
        if (cmd[0] == 'x' && !b->dirty && !all && (bang || !buf_count_dirty())) {
            quit_now = 1;
            return;
        }
        for (struct buffer *t = all ? buffers : b; t; t = all ? t->next : 0) {
            if (t->readonly && !bang && !*arg) {
                err("E45: 'readonly' option is set (add ! to override)");
                return;
            }
            if (*arg && !all) {
                if (buf_save(t, arg) < 0)
                    return;
            } else if (t->dirty || !all) {
                if (buf_save(t, 0) < 0)
                    return;
            }
            if (!all)
                break;
        }
        if (quit) {
            if (!bang && !all && buf_count_dirty()) {
                for (struct buffer *t = buffers; t; t = t->next)
                    if (t->dirty) {
                        err("E162: No write since last change for buffer \"%s\"", t->name);
                        break;
                    }
                return;
            }
            quit_now = 1;
        }
        return;
    }
    if (IS("quit", 1) || IS("qall", 2) || !strcmp(cmd, "close")) {
        if (!bang && (IS("qall", 2) ? buf_count_dirty() : b->dirty)) {
            err("E37: No write since last change (add ! to override)");
            return;
        }
        /* :q with other buffers: drop this one */
        if (!IS("qall", 2) && buffers && buffers->next) {
            struct buffer *next = b->next ? b->next : buffers == b ? b->next : buffers;
            buf_free(b);
            cur = next ? next : buffers;
            return;
        }
        quit_now = 1;
        return;
    }
    if (IS("edit", 1)) {
        if (!*arg) {
            if (bang && b->path) {
                char *path = xstrdup(b->path);
                struct buffer *nb = buf_new(path);
                if (buf_load(nb, path) == 0) {
                    nb->cy = b->cy < nb->nlines ? b->cy : nb->nlines - 1;
                    buf_free(b);
                    cur = nb;
                    announce_loaded(nb);
                } else {
                    buf_free(nb);
                }
                free(path);
            }
            return;
        }
        if (b->dirty && !bang && !(b->next || buffers != b)) {
            /* the buffer stays, so switching is fine */
        }
        struct buffer *nb = open_file(arg);
        if (nb) {
            cur = nb;
            announce_loaded(nb);
        }
        return;
    }
    if (IS("read", 1)) {
        char *text = 0;
        int len = 0;
        if (*arg == '!') {
            run_shell_command(arg + 1, &text, &len);
        } else {
            struct buffer *tmp = buf_new(arg);
            if (buf_load(tmp, arg) < 0 || tmp->newfile) {
                if (tmp->newfile)
                    err("E484: Can't open file %s", arg);
                buf_free(tmp);
                return;
            }
            text = buf_text(tmp, 0, 0, tmp->nlines - 1, tmp->lines[tmp->nlines - 1].len, &len);
            buf_free(tmp);
            text = xrealloc(text, len + 2);
            text[len++] = '\n';
        }
        if (!len) {
            free(text);
            return;
        }
        if (text[len - 1] != '\n') {
            text = xrealloc(text, len + 2);
            text[len++] = '\n';
        }
        int at = y2 + 1;
        if (at >= b->nlines) {
            int ey, ex;
            buf_insert(b, b->nlines - 1, L(b->nlines - 1)->len, "\n", 1, &ey, &ex);
            buf_insert(b, ey, 0, text, len - 1, 0, 0);
        } else {
            buf_insert(b, at, 0, text, len, 0, 0);
        }
        b->cy = at < b->nlines ? at : b->nlines - 1;
        b->cx = 0;
        changed = 1;
        free(text);
        return;
    }
    if (IS("substitute", 1) || cmd[0] == '&' || IS("global", 1) || IS("vglobal", 1)) {
        static char last_rep[256], last_flags[8];
        char pat[256] = "", rep[256] = "";
        int global_cmd = IS("global", 1) || IS("vglobal", 1);
        int invert = IS("vglobal", 1) || (global_cmd && bang);
        const char *q = arg;
        if (cmd[0] != '&' && *q && !isalnum((unsigned char)*q) && *q != ' ') {
            char d = *q++;
            int k = 0;
            while (*q && *q != d && k < 255) {
                if (*q == '\\' && q[1] == d)
                    q++;
                else if (*q == '\\' && q[1])
                    pat[k++] = *q++;
                pat[k++] = *q++;
            }
            pat[k] = 0;
            if (*q == d)
                q++;
            if (!global_cmd) {
                k = 0;
                while (*q && *q != d && k < 255) {
                    if (*q == '\\' && q[1] == d)
                        q++;
                    else if (*q == '\\' && q[1])
                        rep[k++] = *q++;
                    rep[k++] = *q++;
                }
                rep[k] = 0;
                if (*q == d)
                    q++;
                snprintf(last_rep, sizeof(last_rep), "%s", rep);
                snprintf(last_flags, sizeof(last_flags), "%.7s", q);
            }
            if (pat[0])
                snprintf(last_pattern, sizeof(last_pattern), "%s", pat);
        } else {
            snprintf(rep, sizeof(rep), "%s", last_rep);
            q = last_flags;
        }
        if (!last_pattern[0]) {
            err("E35: No previous regular expression");
            return;
        }
        if (global_cmd) {
            /* :g/pat/cmd with cmd d (default p is just a count) or s/// */
            while (*q == ' ')
                q++;
            if (nranges == 0)
                y1 = 0, y2 = b->nlines - 1;
            int count = 0;
            for (int y = y1; y <= y2 && y < b->nlines;) {
                char *s = xstrndup(L(y)->s, L(y)->len);
                int m = line_matches(last_pattern, s, search_icase(last_pattern)) != invert;
                free(s);
                if (!m) {
                    y++;
                    continue;
                }
                count++;
                if (!*q || *q == 'p') {
                    b->cy = y;
                    y++;
                    continue;
                }
                if (*q == 'd') {
                    if (b->nlines == 1) {
                        free(buf_delete(b, 0, 0, 0, L(0)->len));
                        break;
                    }
                    if (y < b->nlines - 1)
                        free(buf_delete(b, y, 0, y + 1, 0));
                    else
                        free(buf_delete(b, y - 1, L(y - 1)->len, y, L(y)->len));
                    y2--;
                    changed = 1;
                    continue;
                }
                if (*q == 's') {
                    char sub[300];
                    snprintf(sub, sizeof(sub), "%d%s", y + 1, q);
                    ex_command(sub);
                }
                y++;
            }
            msg("%d line%s %s", count, count == 1 ? "" : "s", *q == 'd' ? "deleted" : "matched");
            b->cy = b->cy < b->nlines ? b->cy : b->nlines - 1;
            return;
        }
        int g = strchr(q, 'g') != 0, icase = strchr(q, 'i') ? 1 : strchr(q, 'I') ? 0 : search_icase(last_pattern);
        int nsubs;
        int lines = substitute(b, y1, y2, last_pattern, rep, g, icase, 0, &nsubs);
        if (lines < 0)
            return;
        if (!nsubs) {
            err("E486: Pattern not found: %s", last_pattern);
            return;
        }
        changed = 1;
        if (nsubs > 1)
            msg("%d substitution%s on %d line%s", nsubs, nsubs == 1 ? "" : "s", lines, lines == 1 ? "" : "s");
        b->cx = first_nonblank(b->cy);
        return;
    }
    if (IS("delete", 1) || IS("yank", 1)) {
        int r = *arg >= 'a' && *arg <= 'z' ? *arg - 'a' : -1;
        int save = cur_reg;
        cur_reg = r;
        struct pos s = { y1, 0 }, e = { y2, 0 };
        apply_op(cmd[0] == 'd' ? 'd' : 'y', s, e, 1);
        cur_reg = save;
        return;
    }
    if (IS("join", 1)) {
        if (y2 == y1)
            y2++;
        for (int y = y1; y < y2 && y1 + 1 < b->nlines; y++) {
            int len = L(y1)->len, ind;
            line_indent_end(L(y1 + 1), &ind);
            free(buf_delete(b, y1, len, y1 + 1, ind));
            if (len && L(y1)->len > len)
                buf_insert(b, y1, len, " ", 1, 0, 0);
        }
        b->cy = y1;
        changed = 1;
        return;
    }
    if (IS("move", 1) || IS("copy", 2) || !strcmp(cmd, "t")) {
        const char *q = arg;
        int dest;
        if (parse_addr(&q, &dest) <= 0 && strcmp(arg, "0")) {
            err("E14: Invalid address");
            return;
        }
        if (!strcmp(arg, "0"))
            dest = -1;
        int len;
        char *t = buf_text(b, y1, 0, y2, L(y2)->len, &len);
        t = xrealloc(t, len + 2);
        t[len++] = '\n';
        if (cmd[0] == 'm') {
            if (dest >= y1 - 1 && dest <= y2) {
                free(t);                    /* moving lines to where they already are */
                return;
            }
            if (y2 < b->nlines - 1)
                free(buf_delete(b, y1, 0, y2 + 1, 0));
            else if (y1 > 0)
                free(buf_delete(b, y1 - 1, L(y1 - 1)->len, y2, L(y2)->len));
            if (dest > y2)
                dest -= y2 - y1 + 1;
        }
        int at = dest + 1;
        if (at >= b->nlines) {
            int ey, ex;
            buf_insert(b, b->nlines - 1, L(b->nlines - 1)->len, "\n", 1, &ey, &ex);
            buf_insert(b, ey, 0, t, len - 1, 0, 0);
        } else {
            buf_insert(b, at, 0, t, len, 0, 0);
        }
        free(t);
        b->cy = at + (y2 - y1);
        if (b->cy >= b->nlines)
            b->cy = b->nlines - 1;
        changed = 1;
        return;
    }
    if (cmd[0] == '>' || cmd[0] == '<') {
        for (int i = 0; i < n; i++)
            shift_lines(y1, y2, cmd[0] == '>' ? 1 : -1);
        return;
    }
    if (cmd[0] == '=') {
        msg("%d", nranges ? y2 + 1 : b->nlines);
        return;
    }
    if (cmd[0] == '!') {
        if (nranges) {
            /* filter lines through a command */
            int len;
            char *t = buf_text(b, y1, 0, y2, L(y2)->len, &len);
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "/tmp/.vi-filter-%d", getpid());
            FILE *f = fopen(tmp, "w");
            if (f) {
                fwrite(t, 1, len, f);
                fputc('\n', f);
                fclose(f);
            }
            free(t);
            char full[600];
            snprintf(full, sizeof(full), "(%s) < %s", arg, tmp);
            char *outp;
            int olen;
            run_shell_command(full, &outp, &olen);
            unlink(tmp);
            if (olen && outp[olen - 1] == '\n')
                olen--;
            free(buf_delete(b, y1, 0, y2, L(y2)->len));
            buf_insert(b, y1, 0, outp, olen, 0, 0);
            free(outp);
            b->cy = y1;
            changed = 1;
            return;
        }
        ex_shell(arg);
        return;
    }
    if (IS("set", 2)) {
        ex_set(arg);
        return;
    }
    if (IS("nohlsearch", 3)) {
        search_hl_off = 1;
        return;
    }
    if (IS("syntax", 2)) {
        vopt.highlight = strcmp(arg, "off") != 0;
        b->hl_valid = 0;
        return;
    }
    if (IS("bnext", 2) || IS("bprevious", 2) || IS("bNext", 2)) {
        if (cmd[1] == 'n') {
            cur = b->next ? b->next : buffers;
        } else {
            struct buffer *prev = buffers;
            while (prev->next && prev->next != b)
                prev = prev->next;
            cur = b == buffers ? ({ struct buffer *t = buffers; while (t->next) t = t->next; t; }) : prev;
        }
        file_info();
        return;
    }
    if (IS("buffers", 7) || IS("ls", 2) || IS("files", 5)) {
        char list[256];
        int k = 0, i = 1;
        for (struct buffer *t = buffers; t && k < 240; t = t->next, i++)
            k += snprintf(list + k, sizeof(list) - k, "%s%d%s %s%s", i > 1 ? " | " : "", i,
                          t == b ? "%" : "", t->name, t->dirty ? " +" : "");
        msg("%s", list);
        return;
    }
    if (IS("buffer", 1) || IS("bdelete", 2)) {
        if (cmd[1] == 'd') {
            if (b->dirty && !bang) {
                err("E89: No write since last change for buffer (add ! to override)");
                return;
            }
            if (!buffers->next) {
                quit_now = 1;
                return;
            }
            struct buffer *next = b->next ? b->next : buffers;
            buf_free(b);
            cur = next == b ? buffers : next;
            return;
        }
        int want = atoi(arg), i = 1;
        for (struct buffer *t = buffers; t; t = t->next, i++)
            if (i == want || (!isdigit((unsigned char)*arg) && strstr(t->name, arg))) {
                cur = t;
                return;
            }
        err("E86: Buffer %s does not exist", arg);
        return;
    }
    if (IS("file", 1) || IS("f", 1)) {
        if (*arg) {
            free(b->path);
            b->path = xstrdup(arg);
            snprintf(b->name, sizeof(b->name), "%s", arg);
            buf_set_syntax(b);
        }
        file_info();
        return;
    }
    if (IS("help", 1)) {
        msg("vi: i a o insert, ESC normal, :w save, :q quit, u undo, ^R redo, /pat search, :s/a/b/g replace");
        return;
    }
    if (IS("version", 2)) {
        msg("edit (Eunuchs) vi personality 1.0");
        return;
    }
    if (IS("normal", 4)) {
        int keys[256], k = 0;
        for (const char *q = arg; *q && k < 256; q++)
            keys[k++] = (unsigned char)*q;
        queue_keys(keys, k);
        return;
    }
    if (IS("stop", 2) || IS("suspend", 3)) {
        term_suspend();
        return;
    }
    err("E492: Not an editor command: %s", line);
#undef IS
}

/* ---- visual mode ---- */

static void visual_mode(int linewise)
{
    struct buffer *b = cur;
    mode = linewise ? VLINE : VISUAL;
    vis_y1 = b->cy;
    vis_x1 = b->cx;
    for (;;) {
        vis_mode = mode == VLINE ? 2 : 1;
        vis_y2 = b->cy;
        vis_x2 = b->cx;
        clamp();
        draw();
        int k = vk();
        if (k == K_ESC || k == CTRL('C') || (k == 'v' && mode == VISUAL) || (k == 'V' && mode == VLINE))
            break;
        if (k == 'v' || k == 'V') {
            mode = k == 'V' ? VLINE : VISUAL;
            continue;
        }
        if (k == K_RESIZE)
            continue;
        if (k == 'o') {
            int ty = vis_y1, tx = vis_x1;
            vis_y1 = b->cy;
            vis_x1 = b->cx;
            b->cy = ty;
            b->cx = tx;
            continue;
        }
        if (k == ':') {
            vis_mode = 0;
            mode = NORMAL;
            char *line = prompt(":", "'<,'>", 0);
            if (line)
                ex_command(line);
            return;
        }
        if (k == '"') {
            int r = vk();
            cur_reg = r >= 'a' && r <= 'z' ? r - 'a' : -1;
            continue;
        }
        if (k == 'i' || k == 'a') {
            int obj = vk();
            struct pos s, e;
            int lw;
            if (text_object(k, obj, &s, &e, &lw)) {
                vis_y1 = s.y, vis_x1 = s.x;
                b->cy = e.y;
                b->cx = e.x > 0 ? e.x - 1 : 0;
                if (lw)
                    mode = VLINE;
            }
            continue;
        }
        int count = 0;
        while (k >= '1' && k <= '9') {
            count = count * 10 + k - '0';
            k = vk();
            while (k >= '0' && k <= '9') {
                count = count * 10 + k - '0';
                k = vk();
            }
        }
        struct pos to;
        int lw, inc;
        if (strchr("hjklwbeWBE0^$GHLM{}%fFtTnN;,|g'`+-", k) || k == K_LEFT || k == K_RIGHT || k == K_UP ||
            k == K_DOWN || k == K_HOME || k == K_END) {
            if (motion(k, count, count > 0, &to, &lw, &inc, 0)) {
                b->cy = to.y;
                b->cx = to.x;
                if (k != 'j' && k != 'k' && k != K_UP && k != K_DOWN)
                    set_want();
            }
            continue;
        }
        if (k == CTRL('D') || k == CTRL('U') || k == CTRL('F') || k == CTRL('B')) {
            int n = k == CTRL('D') || k == CTRL('U') ? text_rows() / 2 : text_rows() - 2;
            b->cy += k == CTRL('D') || k == CTRL('F') ? n : -n;
            if (b->cy < 0) b->cy = 0;
            if (b->cy >= b->nlines) b->cy = b->nlines - 1;
            continue;
        }
        /* operators on the selection */
        struct pos s = { vis_y1, vis_x1 }, e = { b->cy, b->cx };
        if (s.y > e.y || (s.y == e.y && s.x > e.x)) {
            struct pos t = s;
            s = e;
            e = t;
        }
        int line = mode == VLINE;
        if (!line && e.x < L(e.y)->len)
            e.x = utf8_next(L(e.y)->s, L(e.y)->len, e.x);       /* inclusive */
        vis_mode = 0;
        int op = k == 'x' || k == 'd' || k == K_DEL ? 'd' : k == 'y' ? 'y' : k == 'c' || k == 's' ? 'c'
               : k == '>' ? '>' : k == '<' ? '<' : k == '~' ? '~' : k == 'u' ? 'u' : k == 'U' ? 'U'
               : k == 'D' || k == 'X' ? 'D' : k == 'J' ? 'J' : k == 'r' ? 'r' : k == 'p' || k == 'P' ? 'p'
               : k == 'C' || k == 'S' || k == 'R' ? 'C' : 0;
        if (!op)
            continue;
        mode = NORMAL;
        if (op == 'D' || op == 'C') {
            line = 1;
            op = op == 'D' ? 'd' : 'c';
        }
        if (op == 'J') {
            char cmdline[32];
            snprintf(cmdline, sizeof(cmdline), "%d,%dj", s.y + 1, e.y + 1);
            ex_command(cmdline);
        } else if (op == 'r') {
            int ch = vk();
            if (ch >= 32 && ch < 127) {
                for (int y = s.y; y <= e.y; y++) {
                    int a = line || y > s.y ? 0 : s.x, z = line || y < e.y ? L(y)->len : e.x;
                    for (int x = a; x < z; x++) {
                        free(buf_delete(b, y, x, y, x + 1));
                        char c = (char)ch;
                        buf_insert(b, y, x, &c, 1, 0, 0);
                    }
                }
                changed = 1;
            }
            b->cy = s.y, b->cx = s.x;
        } else if (op == 'p') {
            struct reg saved = *reg_get(cur_reg);
            saved.text = saved.text ? xstrndup(saved.text, saved.len) : 0;
            apply_op('d', s, e, line);
            struct reg *r = reg_get(cur_reg);
            free(r->text);
            *r = saved;
            if (r->text)
                put(line && r->linewise ? (b->cy < s.y ? 1 : 0) : 0, 1);
        } else {
            apply_op(op, s, e, line);
        }
        break;
    }
    vis_mode = 0;
    mode = NORMAL;
}

/* ---- normal mode ---- */

static int pending_count;

static void scroll_lines(int n)
{
    struct buffer *b = cur;
    b->top += n;
    if (b->top < 0)
        b->top = 0;
    if (b->top > b->nlines - 1)
        b->top = b->nlines - 1;
    if (b->cy < b->top)
        b->cy = b->top;
    if (b->cy > b->top + text_rows() - 1)
        b->cy = b->top + text_rows() - 1;
}

static void normal_command(void)
{
    struct buffer *b = cur;
    ncmd = 0;
    changed = 0;
    cur_reg = -1;
    int start_dirty = b->dirty;
    (void)start_dirty;
    undo_begin(b);
    int k = vk();
    int count = 0;
    if (k == '"') {
        int r = vk();
        cur_reg = r >= 'a' && r <= 'z' ? r - 'a' : r >= 'A' && r <= 'Z' ? r - 'A' : r == '0' ? 27 : -1;
        k = vk();
    }
    if (k >= '1' && k <= '9') {
        while (k >= '0' && k <= '9') {
            count = count * 10 + k - '0';
            k = vk();
        }
    }
    pending_count = count;
    int n = count ? count : 1;
    struct pos to;
    int lw, inc;
    switch (k) {
    case K_RESIZE:
        return;
    case ':': {
        char *line = prompt(":", 0, 'f');
        if (line)
            ex_command(line);
        return;
    }
    case '/': case '?': {
        char *pat = prompt(k == '/' ? "/" : "?", 0, 0);
        if (!pat)
            return;
        if (*pat)
            snprintf(last_pattern, sizeof(last_pattern), "%s", pat);
        last_dir = k == '/' ? 1 : -1;
        if (!last_pattern[0]) {
            err("E35: No previous regular expression");
            return;
        }
        b->mark_y = b->cy, b->mark_x = b->cx;
        do_search(last_pattern, last_dir, n);
        set_want();
        return;
    }
    case 'i': case 'a': case 'I': case 'A': case 'o': case 'O': {
        if (b->readonly) {
            err("W10: Warning: Changing a readonly file");
        }
        struct line *l = L(b->cy);
        if (k == 'a' && l->len)
            b->cx = utf8_next(l->s, l->len, b->cx);
        else if (k == 'I')
            b->cx = first_nonblank(b->cy);
        else if (k == 'A')
            b->cx = l->len;
        else if (k == 'o' || k == 'O') {
            int ind = 0;
            char indent[128];
            if (opt_ai) {
                line_indent_end(l, &ind);
                if (ind > 127) ind = 127;
                memcpy(indent, l->s, ind);
            }
            if (k == 'o') {
                int ey, ex;
                buf_insert(b, b->cy, l->len, "\n", 1, &ey, &ex);
                b->cy = ey;
            } else {
                buf_insert(b, b->cy, 0, "\n", 1, 0, 0);
            }
            b->cx = 0;
            if (ind) {
                buf_insert(b, b->cy, 0, indent, ind, 0, 0);
                b->cx = ind;
            }
        }
        insert_mode(0);
        /* a count repeats the inserted text (simplified: for i a only) */
        break;
    }
    case 'R':
        insert_mode(1);
        break;
    case 'v': case 'V':
        visual_mode(k == 'V');
        break;
    case 'x': case K_DEL: case 'X': case 's': {
        struct line *l = L(b->cy);
        if (!l->len) {
            if (k == 's')
                insert_mode(0);
            return;
        }
        struct pos s = { b->cy, b->cx }, e = s;
        if (k == 'X') {
            if (b->cx == 0)
                return;
            for (int i = 0; i < n && s.x > 0; i++)
                s.x = utf8_prev(l->s, s.x);
        } else {
            for (int i = 0; i < n && e.x < l->len; i++)
                e.x = utf8_next(l->s, l->len, e.x);
        }
        apply_op(k == 's' ? 'c' : 'd', s, e, 0);
        break;
    }
    case 'D': case 'C': {
        struct pos s = { b->cy, b->cx }, e = { b->cy + n - 1, 0 };
        if (e.y >= b->nlines)
            e.y = b->nlines - 1;
        e.x = L(e.y)->len;
        apply_op(k == 'D' ? 'd' : 'c', s, e, 0);
        break;
    }
    case 'S': {
        struct pos s = { b->cy, 0 }, e = { b->cy + n - 1, 0 };
        apply_op('c', s, e, 1);
        break;
    }
    case 'Y': {
        struct pos s = { b->cy, 0 }, e = { b->cy + n - 1 < b->nlines ? b->cy + n - 1 : b->nlines - 1, 0 };
        int cy = b->cy, cx = b->cx;
        apply_op('y', s, e, 1);
        b->cy = cy, b->cx = cx;
        break;
    }
    case 'p': case 'P':
        put(k == 'p', count);
        break;
    case 'J': {
        char cmdline[32];
        snprintf(cmdline, sizeof(cmdline), "%d,%dj", b->cy + 1, b->cy + (n > 1 ? n : 2));
        ex_command(cmdline);
        break;
    }
    case 'r': {
        int ch = vk();
        struct line *l = L(b->cy);
        if (ch == K_ESC || ch > 255 || b->cx + n > l->len)
            return;
        if (ch == '\r' || ch == '\n') {
            free(buf_delete(b, b->cy, b->cx, b->cy, b->cx + n));
            insert_newline();
            changed = 1;
            break;
        }
        for (int i = 0; i < n; i++) {
            free(buf_delete(b, b->cy, b->cx + i, b->cy, b->cx + i + 1));
            char c = (char)ch;
            buf_insert(b, b->cy, b->cx + i, &c, 1, 0, 0);
        }
        b->cx += n - 1;
        changed = 1;
        break;
    }
    case '~': {
        struct line *l = L(b->cy);
        if (!l->len)
            return;
        int e = b->cx + n < l->len ? b->cx + n : l->len;
        change_case(b->cy, b->cx, b->cy, e, '~');
        b->cx = e < l->len ? e : l->len - 1;
        break;
    }
    case 'u':
        for (int i = 0; i < n; i++)
            if (!undo_undo(b)) {
                msg("Already at oldest change");
                break;
            }
        return;
    case CTRL('R'):
        for (int i = 0; i < n; i++)
            if (!undo_redo(b)) {
                msg("Already at newest change");
                break;
            }
        return;
    case '.':
        if (!ndot)
            return;
        {
            int keys[1024];
            memcpy(keys, dot_keys, ndot * sizeof(int));
            int nk = ndot;
            for (int i = 0; i < n; i++)
                queue_keys(keys, nk);
        }
        return;
    case 'm': {
        int c = vk();
        if (c >= 'a' && c <= 'z') {
            b->vmarks[c - 'a'][0] = b->cy;
            b->vmarks[c - 'a'][1] = b->cx;
        }
        return;
    }
    case 'q': {
        if (macro_reg >= 0) {
            /* stop recording (drop the q itself) */
            int nk = macro_buf.len - 1;
            struct reg *r = &regs[macro_reg];
            free(r->text);
            r->text = xmalloc(nk * sizeof(int) + 1);
            memcpy(r->text, macro_buf.text, nk * sizeof(int));
            r->len = nk;
            r->linewise = -1;               /* a key list, not text */
            free(macro_buf.text);
            macro_buf.text = 0;
            macro_buf.len = 0;
            macro_reg = -1;
            return;
        }
        int r = vk();
        if (r >= 'a' && r <= 'z') {
            macro_reg = r - 'a';
            macro_buf.len = 0;
        }
        return;
    }
    case '@': {
        int r = vk();
        if (r == '@')
            r = last_macro;
        else if (r >= 'a' && r <= 'z')
            r = r - 'a';
        else
            return;
        if (r < 0 || !regs[r].text)
            return;
        last_macro = r;
        for (int i = 0; i < n; i++) {
            if (regs[r].linewise == -1) {
                queue_keys((int *)regs[r].text, regs[r].len);
            } else {
                int keys[1024], nk = 0;
                for (int j = 0; j < regs[r].len && nk < 1024; j++)
                    keys[nk++] = (unsigned char)regs[r].text[j];
                queue_keys(keys, nk);
            }
        }
        return;
    }
    case 'Z': {
        int c = vk();
        if (c == 'Z')
            ex_command("x");
        else if (c == 'Q')
            ex_command("q!");
        return;
    }
    case CTRL('G'):
        file_info();
        return;
    case CTRL('L'):
        out("\033[2J");
        return;
    case CTRL('Z'):
        term_suspend();
        return;
    case CTRL('F'): case K_PGDN:
        scroll_lines(n * (text_rows() - 2));
        b->cy = b->top;
        return;
    case CTRL('B'): case K_PGUP:
        scroll_lines(-n * (text_rows() - 2));
        b->cy = b->top + text_rows() - 1 < b->nlines ? b->top + text_rows() - 1 : b->nlines - 1;
        return;
    case CTRL('D'):
        b->cy += count ? count : text_rows() / 2;
        b->top += count ? count : text_rows() / 2;
        if (b->cy >= b->nlines) b->cy = b->nlines - 1;
        if (b->top > b->nlines - 1) b->top = b->nlines - 1;
        b->cx = first_nonblank(b->cy);
        return;
    case CTRL('U'):
        b->cy -= count ? count : text_rows() / 2;
        b->top -= count ? count : text_rows() / 2;
        if (b->cy < 0) b->cy = 0;
        if (b->top < 0) b->top = 0;
        b->cx = first_nonblank(b->cy);
        return;
    case CTRL('E'):
        scroll_lines(n);
        return;
    case CTRL('Y'):
        scroll_lines(-n);
        return;
    case 'z': {
        int c = vk();
        int tr = text_rows();
        if (c == 'z' || c == '.')
            b->top = b->cy - tr / 2;
        else if (c == 't' || c == '\r')
            b->top = b->cy;
        else if (c == 'b' || c == '-')
            b->top = b->cy - tr + 1;
        if (b->top < 0)
            b->top = 0;
        return;
    }
    case '&':
        ex_command("s");
        break;
    case 'd': case 'c': case 'y': case '>': case '<': case 'g': {
        int op = k;
        if (k == 'g') {
            int k2 = vk();
            if (k2 == 'u' || k2 == 'U' || k2 == '~') {
                op = k2;
            } else if (k2 == 'J') {
                char cmdline[32];
                snprintf(cmdline, sizeof(cmdline), "%d,%dj", b->cy + 1, b->cy + (n > 1 ? n : 2));
                ex_command(cmdline);
                break;
            } else if (k2 == 'v') {
                b->cy = vis_y2, b->cx = vis_x2;
                visual_mode(0);
                break;
            } else {
                /* gg ge gE g_ are motions */
                int keys[1] = { k2 };
                queue_keys(keys, 1);
                ncmd--;                     /* it will be recorded again */
                if (motion('g', count, count > 0, &to, &lw, &inc, 0)) {
                    b->mark_y = b->cy, b->mark_x = b->cx;
                    b->cy = to.y, b->cx = to.x;
                    set_want();
                }
                return;
            }
        }
        int k2 = vk();
        int count2 = 0;
        while (k2 >= '0' && k2 <= '9' && (count2 || k2 != '0')) {
            count2 = count2 * 10 + k2 - '0';
            k2 = vk();
        }
        int total = (count ? count : 1) * (count2 ? count2 : 1);
        int has = count || count2;
        struct pos s = { b->cy, b->cx };
        if (k2 == op || (op == 'u' && k2 == 'u') || (op == 'U' && k2 == 'U') || (op == '~' && k2 == '~') ||
            (k == 'g' && k2 == 'g' && 0)) {
            /* dd cc yy >> << guu gUU g~~: whole lines */
            struct pos e = { b->cy + total - 1, 0 };
            if (e.y >= b->nlines)
                e.y = b->nlines - 1;
            int cy = b->cy;
            apply_op(op, s, e, 1);
            if (op == 'y')
                b->cy = cy;
            break;
        }
        if (k2 == 'i' || k2 == 'a') {
            int obj = vk();
            struct pos ts, te;
            int tlw;
            if (!text_object(k2, obj, &ts, &te, &tlw))
                return;
            apply_op(op, ts, te, tlw);
            break;
        }
        if (k2 == K_ESC)
            return;
        if (op == 'c' && (k2 == 'w' || k2 == 'W') && s.x < L(s.y)->len &&
            char_class((unsigned char)L(s.y)->s[s.x])) {
            k2 = k2 == 'w' ? 'e' : 'E';     /* cw acts like ce on a word */
        }
        if (!motion(k2, total, has, &to, &lw, &inc, 1))
            return;
        if (inc && !lw) {
            struct line *l = L(to.y);
            if (to.x < l->len)
                to.x = utf8_next(l->s, l->len, to.x);
        }
        apply_op(op, s, to, lw);
        break;
    }
    default:
        if (motion(k, count, count > 0, &to, &lw, &inc, 0)) {
            if (k == 'G' || k == 'n' || k == 'N' || k == '*' || k == '#' || k == '%' || k == '\'' ||
                k == '`' || (k == 'g'))
                b->mark_y = b->cy, b->mark_x = b->cx;
            b->cy = to.y;
            b->cx = to.x;
            if (k != 'j' && k != 'k' && k != K_UP && k != K_DOWN && k != CTRL('N') && k != CTRL('P'))
                set_want();
        } else if (k != K_ESC && !message[0]) {
            out("\a");
        }
        return;
    }
    if (changed && ncmd) {
        memcpy(dot_keys, cmd_keys, ncmd * sizeof(int));
        ndot = ncmd;
    }
}

/* ---- the screen's bottom line ---- */

static const char *bottom(void)
{
    static char buf[256];
    struct buffer *b = cur;
    const char *m = mode == INSERT ? "-- INSERT --" : mode == REPLACE ? "-- REPLACE --"
                  : mode == VISUAL ? "-- VISUAL --" : mode == VLINE ? "-- VISUAL LINE --" : "";
    char rec[32] = "";
    if (macro_reg >= 0)
        snprintf(rec, sizeof(rec), "%srecording @%c", *m ? " " : "", 'a' + macro_reg);
    char ruler[64];
    int col = screen_col(b, b->cy, b->cx) + 1;
    int tr = text_rows();
    const char *where = b->nlines <= tr ? "All" : b->top == 0 ? "Top"
                      : b->top + tr >= b->nlines ? "Bot" : 0;
    char pct[8];
    if (!where) {
        snprintf(pct, sizeof(pct), "%d%%", b->top * 100 / (b->nlines - tr));
        where = pct;
    }
    if (b->cx + 1 != col)
        snprintf(ruler, sizeof(ruler), "%d,%d-%d", b->cy + 1, b->cx + 1, col);
    else
        snprintf(ruler, sizeof(ruler), "%d,%d", b->cy + 1, col);
    int left = snprintf(buf, sizeof(buf), "%s%s", m, rec);
    int pad = cols - 18 - left;
    if (pad < 1)
        pad = 1;
    snprintf(buf + left, sizeof(buf) - left, "%*s%-13s %s", pad, "", ruler, where);
    return buf;
}

int vi_main(int argc, char **argv, int readonly)
{
    int start_line = -1;
    const char *start_pat = 0;
    struct buffer *first = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '+') {
            if (a[1] == '/')
                start_pat = a + 2;
            else
                start_line = a[1] ? atoi(a + 1) : 1 << 30;
        } else if (!strcmp(a, "-R")) {
            readonly = 1;
        } else if (a[0] == '-' && a[1]) {
            eprintf("vi: unknown option %s (usage: vi [-R] [+N | +/pat] [file ...])\n", a);
            return 2;
        }
    }
    for (int i = argc - 1; i >= 1; i--) {      /* buf_new puts each buffer first */
        const char *a = argv[i];
        if (a[0] == '+' || (a[0] == '-' && a[1]))
            continue;
        struct buffer *b = buf_new(a);
        if (buf_load(b, a) < 0) {
            eprintf("vi: %s\n", message);
            message[0] = 0;
            buf_free(b);
            continue;
        }
        if (readonly)
            b->readonly = 1;
        first = b;
    }
    if (!first)
        first = buf_new(0);
    cur = first;
    if (term_init() < 0)
        return 1;
    show_tildes = 1;
    vopt.status_line = 0;
    draw_bottom = bottom;
    if (cur->path)
        announce_loaded(cur);
    if (start_line > 0) {
        cur->cy = start_line - 1 < cur->nlines ? start_line - 1 : cur->nlines - 1;
        cur->cx = first_nonblank(cur->cy);
    }
    if (start_pat) {
        snprintf(last_pattern, sizeof(last_pattern), "%s", start_pat);
        do_search(start_pat, 1, 1);
    }
    while (!quit_now) {
        clamp();
        draw();
        message[0] = 0;
        normal_command();
    }
    term_done();
    return 0;
}
