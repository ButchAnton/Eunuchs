/*
 * edit: the emacs personality.
 *
 * Movement: C-f C-b C-n C-p C-a C-e M-f M-b M-a M-e C-v M-v M-< M-> M-g g, arrows.
 * Editing: C-d DEL M-d M-DEL C-k C-w M-w C-y M-y C-o C-t M-t M-u M-l M-c C-q Tab C-j.
 * Mark and region: C-SPC (C-@), C-x C-x, C-x h; the region shows highlighted.
 * Search: C-s C-r (incremental), M-% query-replace, M-x commands.
 * Files and buffers: C-x C-f C-x C-s C-x C-w C-x s C-x i C-x b C-x k C-x C-b C-x C-c.
 * Other: C-u and M-digit arguments, C-_ C-/ C-x u undo, M-_ redo, C-g, C-l, C-z, M-!.
 */
#include "edit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int quit_now;
static int last_cmd, this_cmd;          /* for kill appending and M-y */
enum { C_OTHER, C_KILL, C_YANK, C_VMOVE };

/* ---- kill ring ---- */

#define NKILL 16
static char *kills[NKILL];
static int kill_len[NKILL];
static int nkills, kill_idx;
static int yank_y1, yank_x1, yank_y2, yank_x2;

static void kill_push(const char *t, int len, int prepend)
{
    if (last_cmd == C_KILL && nkills) {
        /* consecutive kills build one entry */
        char *s = xmalloc(kill_len[0] + len + 1);
        if (prepend) {
            memcpy(s, t, len);
            memcpy(s + len, kills[0], kill_len[0]);
        } else {
            memcpy(s, kills[0], kill_len[0]);
            memcpy(s + kill_len[0], t, len);
        }
        free(kills[0]);
        kills[0] = s;
        kill_len[0] += len;
    } else {
        if (nkills == NKILL)
            free(kills[--nkills]);
        memmove(kills + 1, kills, nkills * sizeof(char *));
        memmove(kill_len + 1, kill_len, nkills * sizeof(int));
        kills[0] = xstrndup(t, len);
        kill_len[0] = len;
        nkills++;
    }
    kill_idx = 0;
    this_cmd = C_KILL;
}

/* ---- helpers ---- */

static struct line *L(int y)
{
    return &cur->lines[y];
}

static void set_want(void)
{
    cur->want_col = screen_col(cur, cur->cy, cur->cx);
}

static int before(int y1, int x1, int y2, int x2)
{
    return y1 < y2 || (y1 == y2 && x1 < x2);
}

/* The region between mark and point, ordered. 0 if no mark. */
static int region(int *y1, int *x1, int *y2, int *x2)
{
    struct buffer *b = cur;
    if (!b->mark_set) {
        err("The mark is not set now, so there is no region");
        return 0;
    }
    if (b->mark_y >= b->nlines) {
        b->mark_y = b->nlines - 1;
        b->mark_x = L(b->mark_y)->len;
    }
    if (b->mark_x > L(b->mark_y)->len)
        b->mark_x = L(b->mark_y)->len;
    if (before(b->mark_y, b->mark_x, b->cy, b->cx)) {
        *y1 = b->mark_y, *x1 = b->mark_x, *y2 = b->cy, *x2 = b->cx;
    } else {
        *y1 = b->cy, *x1 = b->cx, *y2 = b->mark_y, *x2 = b->mark_x;
    }
    return 1;
}

static int readonly_check(void)
{
    if (cur->readonly) {
        err("Buffer is read-only: #<buffer %s>", cur->name);
        return 1;
    }
    return 0;
}

static void delete_range(int y1, int x1, int y2, int x2, int to_kill, int prepend)
{
    char *t = buf_delete(cur, y1, x1, y2, x2);
    if (to_kill) {
        int len;
        /* the deleted text's length: count it from the buffer coordinates */
        if (y1 == y2) {
            len = x2 - x1;
        } else {
            len = strlen(t);
        }
        kill_push(t, len, prepend);
    }
    free(t);
    cur->cy = y1;
    cur->cx = x1;
}

static void forward_char(void)
{
    struct buffer *b = cur;
    if (b->cx < L(b->cy)->len)
        b->cx = utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx);
    else if (b->cy < b->nlines - 1)
        b->cy++, b->cx = 0;
    else
        err("End of buffer");
}

static void backward_char(void)
{
    struct buffer *b = cur;
    if (b->cx > 0)
        b->cx = utf8_prev(L(b->cy)->s, b->cx);
    else if (b->cy > 0)
        b->cy--, b->cx = L(b->cy)->len;
    else
        err("Beginning of buffer");
}

static int char_at(int y, int x)
{
    return x < L(y)->len ? (unsigned char)L(y)->s[x] : '\n';
}

static void forward_word(void)
{
    struct buffer *b = cur;
    while (!is_word_char(char_at(b->cy, b->cx))) {
        if (b->cy == b->nlines - 1 && b->cx >= L(b->cy)->len)
            return;
        forward_char();
    }
    while (b->cx < L(b->cy)->len && is_word_char(char_at(b->cy, b->cx)))
        b->cx++;
}

static void backward_word(void)
{
    struct buffer *b = cur;
    for (;;) {
        if (b->cy == 0 && b->cx == 0)
            return;
        if (b->cx > 0 && is_word_char((unsigned char)L(b->cy)->s[b->cx - 1]))
            break;
        backward_char();
    }
    while (b->cx > 0 && is_word_char((unsigned char)L(b->cy)->s[b->cx - 1]))
        b->cx--;
}

static void line_move(int n)
{
    struct buffer *b = cur;
    if (last_cmd != C_VMOVE)
        set_want();
    int y = b->cy + n;
    if (y < 0) {
        y = 0;
        err("Beginning of buffer");
    }
    if (y >= b->nlines) {
        y = b->nlines - 1;
        err("End of buffer");
    }
    b->cy = y;
    b->cx = col_to_x(b, y, b->want_col);
    this_cmd = C_VMOVE;
}

static void insert_text(const char *t, int len)
{
    int ey, ex;
    buf_insert(cur, cur->cy, cur->cx, t, len, &ey, &ex);
    cur->cy = ey;
    cur->cx = ex;
}

/* An extended regular expression matching the literal string s. */
static void quote_regex(const char *s, char *out, int size)
{
    int n = 0;
    for (; *s && n < size - 3; s++) {
        if (strchr("\\.[]*^$", *s))
            out[n++] = '\\';
        out[n++] = *s;
    }
    out[n] = 0;
}

/* ---- incremental search ---- */

static char isearch_last[256];
static void emacs_command_key(int k);

static void isearch(int dir)
{
    struct buffer *b = cur;
    char str[256] = "";
    int n = 0;
    int oy = b->cy, ox = b->cx;
    int failing = 0, wrapped = 0;
    /* a stack of positions so DEL goes back */
    struct { int y, x, n; } stack[256];
    int sp = 0;
    char saved_pattern[256];
    snprintf(saved_pattern, sizeof(saved_pattern), "%s", last_pattern);
    int mlen = 0;
    for (;;) {
        char q[512];
        quote_regex(str, q, sizeof(q));
        snprintf(last_pattern, sizeof(last_pattern), "%s", n ? q : "");
        if (n && !failing) {
            vis_mode = 3;
            vis_y1 = vis_y2 = b->cy;
            vis_x1 = dir > 0 ? b->cx - mlen : b->cx;
            vis_x2 = dir > 0 ? b->cx : b->cx + mlen;
        } else {
            vis_mode = 0;
        }
        msg("%s%sI-search%s: %s", failing ? "Failing " : "", wrapped ? "Wrapped " : "",
            dir < 0 ? " backward" : "", str);
        draw();
        int k = term_getkey();
        if (k == K_RESIZE)
            continue;
        if (k == CTRL('G')) {
            if (failing && sp) {
                /* back to the last successful state */
                while (sp && failing) {
                    sp--;
                    b->cy = stack[sp].y, b->cx = stack[sp].x, n = stack[sp].n;
                    str[n] = 0;
                    failing = 0;
                }
                continue;
            }
            b->cy = oy, b->cx = ox;
            msg("Quit");
            break;
        }
        if (k == 127 || k == CTRL('H')) {
            if (sp) {
                sp--;
                b->cy = stack[sp].y, b->cx = stack[sp].x, n = stack[sp].n;
                str[n] = 0;
                failing = 0;
                mlen = n;
            }
            continue;
        }
        int again = k == CTRL('S') || k == CTRL('R');
        if (again) {
            int ndir = k == CTRL('S') ? 1 : -1;
            if (!n) {
                /* C-s C-s: the previous search string */
                snprintf(str, sizeof(str), "%s", isearch_last);
                n = strlen(str);
                dir = ndir;
                if (!n)
                    continue;
            } else if (ndir != dir) {
                dir = ndir;
                /* turning around: just flip which end of the match we're on */
                if (!failing) {
                    if (dir < 0)
                        b->cx -= mlen;
                    else
                        b->cx += mlen;
                    continue;
                }
            }
        } else if (k == CTRL('W')) {
            /* add the word after the match */
            int x = b->cx;
            while (x < L(b->cy)->len && is_word_char((unsigned char)L(b->cy)->s[x]) && n < 250)
                str[n++] = L(b->cy)->s[x++];
            if (x == b->cx && x < L(b->cy)->len && n < 250)
                str[n++] = L(b->cy)->s[x];
            str[n] = 0;
        } else if (k >= 32 && k < 256 && n < 250) {
            str[n++] = (char)k;
            str[n] = 0;
        } else if (k == CTRL('Q')) {
            int c = term_getkey();
            if (c < 256 && n < 250) {
                str[n++] = (char)c;
                str[n] = 0;
            }
        } else {
            /* anything else ends the search; Enter is just consumed */
            if (n)
                snprintf(isearch_last, sizeof(isearch_last), "%s", str);
            if (k != '\r' && k != K_ESC) {
                last_pattern[0] = 0;
                vis_mode = 0;
                message[0] = 0;
                if (!b->mark_set || 1) {
                    b->mark_y = oy, b->mark_x = ox;
                    b->mark_set = 0;
                }
                snprintf(last_pattern, sizeof(last_pattern), "%s", saved_pattern);
                /* re-run the key as a command */
                emacs_command_key(k);
                return;
            }
            msg("Mark saved where search started");
            b->mark_y = oy, b->mark_x = ox;
            break;
        }
        if (sp < 256) {
            stack[sp].y = b->cy, stack[sp].x = b->cx, stack[sp].n = again ? n : n - 1;
            if (k == CTRL('W'))
                stack[sp].n = n;
            sp++;
        }
        /* search from the start of the current match (forward) or its end (backward) */
        quote_regex(str, q, sizeof(q));
        int fy = b->cy, fx = b->cx, y, x, len;
        if (dir > 0)
            fx = again ? fx : fx - mlen;
        else
            fx = again ? fx : fx + mlen + 1;
        if (fx < 0) fx = 0;
        if (!again && dir > 0)
            fx--;                       /* search_find looks after fx */
        int icase = search_icase(str);
        int r;
        if (failing && again) {
            /* wrap around */
            r = search_find(b, q, dir, dir > 0 ? 0 : b->nlines - 1, dir > 0 ? -1 : L(b->nlines - 1)->len + 1,
                            &y, &x, &len, 0, icase);
            wrapped = 1;
        } else {
            r = search_find(b, q, dir, fy, fx, &y, &x, &len, 0, icase);
        }
        if (r > 0) {
            failing = 0;
            mlen = len;
            b->cy = y;
            b->cx = dir > 0 ? x + len : x;
        } else {
            failing = 1;
            out("\a");
        }
    }
    snprintf(last_pattern, sizeof(last_pattern), "%s", saved_pattern);
    vis_mode = 0;
}

/* ---- query replace ---- */

static void query_replace(int regex, int query)
{
    struct buffer *b = cur;
    if (readonly_check())
        return;
    static char from_def[256], to_def[256];
    char label[300];
    snprintf(label, sizeof(label), "%s%s%s: ", query ? "Query replace" : "Replace", regex ? " regexp" : "",
             from_def[0] ? "" : "");
    char *f = prompt(label, 0, 0);
    if (!f)
        return;
    if (!*f) {
        if (!from_def[0])
            return;
    } else {
        snprintf(from_def, sizeof(from_def), "%s", f);
    }
    char label2[600];
    snprintf(label2, sizeof(label2), "%s%s %s with: ", query ? "Query replace" : "Replace",
             regex ? " regexp" : "", from_def);
    char *t = prompt(label2, 0, 0);
    if (!t)
        return;
    snprintf(to_def, sizeof(to_def), "%s", t);
    char pat[512], rep[512];
    if (regex) {
        snprintf(pat, sizeof(pat), "%s", from_def);
        snprintf(rep, sizeof(rep), "%s", to_def);
    } else {
        quote_regex(from_def, pat, sizeof(pat));
        int n = 0;
        for (const char *p = to_def; *p && n < 500; p++) {
            if (*p == '&' || *p == '\\')
                rep[n++] = '\\';
            rep[n++] = *p;
        }
        rep[n] = 0;
    }
    int icase = search_icase(from_def);
    int count = 0, all = !query;
    int y = b->cy, x = b->cx - 1;
    int oy = b->cy, ox = b->cx;
    undo_begin(b);
    for (;;) {
        int ry, rx, len;
        int r = search_find(b, pat, 1, y, x, &ry, &rx, &len, 0, icase);
        if (r <= 0)
            break;
        b->cy = ry;
        b->cx = rx + len;
        int ans = 'y';
        if (!all) {
            vis_mode = 3;
            vis_y1 = vis_y2 = ry;
            vis_x1 = rx;
            vis_x2 = rx + len;
            for (;;) {
                msg("Query replacing %s with %s: (? for help) ", from_def, to_def);
                draw();
                ans = term_getkey();
                if (ans == '?') {
                    msg("y/SPC replace, n/DEL skip, ! all, . replace and stop, q/RET quit");
                    draw();
                    term_getkey();
                    continue;
                }
                break;
            }
            vis_mode = 0;
        }
        if (ans == ' ' || ans == 'y' || ans == '!' || ans == '.') {
            int ey, ex, ml;
            if (replace_at(b, ry, rx, pat, rep, icase, &ey, &ex, &ml) <= 0)
                break;
            b->cy = ey;
            b->cx = ex;
            count++;
            if (ans == '!')
                all = 1;
            if (ans == '.')
                break;
            /* go on after the replacement (one further after an empty match) */
            y = ey;
            x = ml ? ex - 1 : ex;
        } else if (ans == 'n' || ans == 127) {
            y = ry;
            x = rx + (len ? len - 1 : 0);
            if (!len)
                x = rx;
        } else {
            break;
        }
    }
    vis_mode = 0;
    b->mark_set = 0;
    b->mark_y = oy, b->mark_x = ox;
    msg("Replaced %d occurrence%s", count, count == 1 ? "" : "s");
}

/* ---- files and buffers ---- */

static void find_file(void)
{
    char init[300] = "";
    if (cur->path) {
        const char *sl = strrchr(cur->path, '/');
        if (sl)
            snprintf(init, sizeof(init), "%.*s/", (int)(sl - cur->path), cur->path);
    }
    char *path = prompt("Find file: ", init, 'f');
    if (!path || !*path)
        return;
    for (struct buffer *b = buffers; b; b = b->next)
        if (b->path && !strcmp(b->path, path)) {
            cur = b;
            return;
        }
    struct buffer *b = buf_new(path);
    if (buf_load(b, path) < 0) {
        buf_free(b);
        return;
    }
    cur = b;
    if (b->newfile)
        msg("(New file)");
}

static int save_buffer(struct buffer *b)
{
    if (!b->path) {
        char *path = prompt("File to save in: ", 0, 'f');
        if (!path || !*path)
            return -1;
        b->path = xstrdup(path);
        const char *sl = strrchr(path, '/');
        snprintf(b->name, sizeof(b->name), "%s", sl ? sl + 1 : path);
        buf_set_syntax(b);
    }
    if (!b->dirty && !b->newfile) {
        msg("(No changes need to be saved)");
        return 0;
    }
    if (buf_save(b, 0) < 0)
        return -1;
    msg("Wrote %s", b->path);
    return 0;
}

static void write_file(void)
{
    char *path = prompt("Write file: ", 0, 'f');
    if (!path || !*path)
        return;
    if (buf_save(cur, path) < 0)
        return;
    free(cur->path);
    cur->path = xstrdup(path);
    const char *sl = strrchr(path, '/');
    snprintf(cur->name, sizeof(cur->name), "%s", sl ? sl + 1 : path);
    buf_set_syntax(cur);
    msg("Wrote %s", path);
}

static void save_some_buffers(void)
{
    int n = 0;
    for (struct buffer *b = buffers; b; b = b->next) {
        if (!b->dirty || !b->path)
            continue;
        char q[200];
        snprintf(q, sizeof(q), "Save file %s? (y or n) ", b->path);
        int a = prompt_yes_no(q);
        if (a < 0)
            return;
        if (a && buf_save(b, 0) == 0)
            n++;
    }
    msg(n ? "Saved %d file(s)" : "(No files need saving)", n);
}

static void kill_emacs(void)
{
    for (struct buffer *b = buffers; b; b = b->next) {
        if (!b->dirty)
            continue;
        char q[200];
        snprintf(q, sizeof(q), "Save file %s? (y or n) ", b->path ? b->path : b->name);
        int a = prompt_yes_no(q);
        if (a < 0)
            return;
        if (a && save_buffer(b) < 0)
            return;
    }
    if (buf_count_dirty()) {
        int a = prompt_yes_no("Modified buffers exist; exit anyway? (y or n) ");
        if (a != 1)
            return;
    }
    quit_now = 1;
}

static void switch_buffer(void)
{
    struct buffer *other = cur->next ? cur->next : buffers;
    char label[120];
    snprintf(label, sizeof(label), "Switch to buffer (default %s): ", other->name);
    char *name = prompt(label, 0, 0);
    if (!name)
        return;
    if (!*name) {
        cur = other;
        return;
    }
    for (struct buffer *b = buffers; b; b = b->next)
        if (!strcmp(b->name, name) || (b->path && !strcmp(b->path, name))) {
            cur = b;
            return;
        }
    cur = buf_new(0);
    snprintf(cur->name, sizeof(cur->name), "%s", name);
}

static void kill_buffer(void)
{
    char label[120];
    snprintf(label, sizeof(label), "Kill buffer (default %s): ", cur->name);
    char *name = prompt(label, 0, 0);
    if (!name)
        return;
    struct buffer *victim = cur;
    if (*name) {
        victim = 0;
        for (struct buffer *b = buffers; b; b = b->next)
            if (!strcmp(b->name, name))
                victim = b;
        if (!victim) {
            err("No such buffer %s", name);
            return;
        }
    }
    if (victim->dirty) {
        char q[160];
        snprintf(q, sizeof(q), "Buffer %s modified; kill anyway? (y or n) ", victim->name);
        if (prompt_yes_no(q) != 1)
            return;
    }
    struct buffer *next = victim->next ? victim->next : buffers != victim ? buffers : 0;
    buf_free(victim);
    if (cur == victim)
        cur = next ? next : buf_new(0);
    if (!buffers) {
        cur = buf_new(0);
        snprintf(cur->name, sizeof(cur->name), "*scratch*");
    }
}

static void list_buffers(void)
{
    char list[256];
    int k = 0;
    for (struct buffer *t = buffers; t && k < 240; t = t->next)
        k += snprintf(list + k, sizeof(list) - k, "%s%s%s%s", k ? "  " : "", t == cur ? "." : "",
                      t->dirty ? "*" : "", t->name);
    msg("%s", list);
}

static void insert_file(void)
{
    char *path = prompt("Insert file: ", 0, 'f');
    if (!path || !*path || readonly_check())
        return;
    struct buffer *tmp = buf_new(path);
    if (buf_load(tmp, path) < 0 || tmp->newfile) {
        if (tmp->newfile)
            err("Opening input file: No such file or directory, %s", path);
        buf_free(tmp);
        return;
    }
    int len;
    char *text = buf_text(tmp, 0, 0, tmp->nlines - 1, tmp->lines[tmp->nlines - 1].len, &len);
    buf_free(tmp);
    int y = cur->cy, x = cur->cx;
    insert_text(text, len);
    free(text);
    cur->mark_y = cur->cy, cur->mark_x = cur->cx;
    cur->cy = y, cur->cx = x;
}

static void shell_command(void)
{
    char *cmd = prompt("Shell command: ", 0, 'f');
    if (!cmd || !*cmd)
        return;
    char *o;
    int n;
    int st = run_shell_command(cmd, &o, &n);
    while (n && o[n - 1] == '\n')
        o[--n] = 0;
    if (!n)
        msg("(Shell command %s with no output)", st ? "failed" : "succeeded");
    else if (!memchr(o, '\n', n))
        msg("%s", o);
    else {
        /* several lines: show them in a buffer */
        struct buffer *b = 0;
        for (struct buffer *t = buffers; t; t = t->next)
            if (!strcmp(t->name, "*Shell Command Output*"))
                b = t;
        if (!b) {
            b = buf_new(0);
            snprintf(b->name, sizeof(b->name), "*Shell Command Output*");
        } else {
            free(buf_delete(b, 0, 0, b->nlines - 1, b->lines[b->nlines - 1].len));
        }
        buf_insert(b, 0, 0, o, n, 0, 0);
        b->dirty = 0;
        b->cy = b->cx = b->top = 0;
        cur = b;
    }
    free(o);
}

static void revert_buffer(void)
{
    if (!cur->path)
        return;
    char q[300];
    snprintf(q, sizeof(q), "Revert buffer from file %s? (y or n) ", cur->path);
    if (prompt_yes_no(q) != 1)
        return;
    struct buffer *nb = buf_new(cur->path);
    if (buf_load(nb, cur->path) < 0) {
        buf_free(nb);
        return;
    }
    nb->cy = cur->cy < nb->nlines ? cur->cy : nb->nlines - 1;
    nb->top = cur->top;
    buf_free(cur);
    cur = nb;
}

static void goto_line(int arg)
{
    int n = arg;
    if (!n) {
        char *s = prompt("Goto line: ", 0, 0);
        if (!s || !*s)
            return;
        n = atoi(s);
    }
    cur->mark_y = cur->cy, cur->mark_x = cur->cx;
    cur->cy = n < 1 ? 0 : n > cur->nlines ? cur->nlines - 1 : n - 1;
    cur->cx = 0;
}

static void count_words(void)
{
    int y1 = 0, x1 = 0, y2 = cur->nlines - 1, x2 = L(y2)->len;
    int inreg = cur->mark_set;
    if (inreg)
        region(&y1, &x1, &y2, &x2);
    int len;
    char *t = buf_text(cur, y1, x1, y2, x2, &len);
    int words = 0, inword = 0;
    for (int i = 0; i < len; i++) {
        int w = !isspace((unsigned char)t[i]);
        if (w && !inword)
            words++;
        inword = w;
    }
    free(t);
    msg("%s has %d line%s, %d word%s, and %d character%s", inreg ? "Region" : "Buffer", y2 - y1 + 1,
        y2 == y1 ? "" : "s", words, words == 1 ? "" : "s", len, len == 1 ? "" : "s");
}

static void case_word(int how, int n)
{
    struct buffer *b = cur;
    if (readonly_check())
        return;
    for (int i = 0; i < n; i++) {
        int y = b->cy, x = b->cx;
        forward_word();
        if (b->cy != y) {
            /* the word starts on a later line */
            y = b->cy;
            x = 0;
        }
        int ey = b->cy, ex = b->cx;
        while (x < ex && !is_word_char((unsigned char)L(y)->s[x]))
            x++;
        if (x >= ex || ey != y)
            continue;
        char *t = buf_delete(b, y, x, y, ex);
        for (int k = 0; k < ex - x; k++)
            t[k] = how == 'u' ? toupper((unsigned char)t[k])
                 : how == 'l' || k ? tolower((unsigned char)t[k]) : toupper((unsigned char)t[k]);
        buf_insert(b, y, x, t, ex - x, 0, 0);
        free(t);
        b->cy = y, b->cx = ex;
    }
}

static void indent_line(void)
{
    /* Tab: indent like the previous non-blank line, or to the next tab stop */
    struct buffer *b = cur;
    int ind;
    line_indent_end(L(b->cy), &ind);
    int want = 0;
    for (int y = b->cy - 1; y >= 0; y--)
        if (L(y)->len) {
            int pi;
            line_indent_end(L(y), &pi);
            want = screen_col(b, y, pi);
            char last = L(y)->s[L(y)->len - 1];
            if (last == '{' || last == ':' || last == '(')
                want += 4;
            break;
        }
    int have = screen_col(b, b->cy, ind);
    if (L(b->cy)->len > ind && L(b->cy)->s[ind] == '}' && want >= 4)
        want -= 4;
    if (have == want || b->cx > ind) {
        if (b->cx >= ind && have == want && b->cx == ind) {
            /* already indented: insert a tab stop's worth of spaces */
            int n = 4 - screen_col(b, b->cy, b->cx) % 4;
            insert_text("    ", n);
            return;
        }
        if (b->cx > ind) {
            int n = 4 - screen_col(b, b->cy, b->cx) % 4;
            insert_text("    ", n);
            return;
        }
    }
    int off = b->cx - ind;
    free(buf_delete(b, b->cy, 0, b->cy, ind));
    char sp[256];
    if (want > 255)
        want = 255;
    memset(sp, ' ', want);
    buf_insert(b, b->cy, 0, sp, want, 0, 0);
    b->cx = want + (off > 0 ? off : 0);
}

/* ---- M-x ---- */

static void help(void);
static void undo_cmd(int redo);

static const char *const commands[] = {
    "goto-line", "query-replace", "query-replace-regexp", "replace-string", "replace-regexp",
    "save-buffer", "write-file", "find-file", "insert-file", "kill-buffer", "switch-to-buffer",
    "list-buffers", "save-some-buffers", "revert-buffer", "undo", "undo-redo", "set-mark-command",
    "exchange-point-and-mark", "mark-whole-buffer", "display-line-numbers-mode", "linum-mode",
    "whitespace-mode", "font-lock-mode", "toggle-case-fold-search", "count-words", "what-line",
    "shell-command", "isearch-forward", "isearch-backward", "beginning-of-buffer", "end-of-buffer",
    "upcase-region", "downcase-region", "kill-region", "copy-region-as-kill", "yank",
    "delete-trailing-whitespace", "set-tab-width", "indent-tabs-mode", "suspend-emacs",
    "save-buffers-kill-emacs", "kill-emacs", "help", "describe-bindings", "version",
    "hl-search-mode", 0,
};

static void case_region(int up)
{
    int y1, x1, y2, x2;
    if (!region(&y1, &x1, &y2, &x2) || readonly_check())
        return;
    int len;
    char *t = buf_delete(cur, y1, x1, y2, x2);
    len = strlen(t);
    for (int i = 0; i < len; i++)
        t[i] = up ? toupper((unsigned char)t[i]) : tolower((unsigned char)t[i]);
    int cy = cur->cy, cx = cur->cx;
    buf_insert(cur, y1, x1, t, len, 0, 0);
    free(t);
    cur->cy = cy, cur->cx = cx;
}

static void mx(int arg)
{
    char *c = prompt("M-x ", 0, 'c');
    if (!c || !*c)
        return;
    char name[64];
    snprintf(name, sizeof(name), "%s", c);
    struct buffer *b = cur;
    if (!strcmp(name, "goto-line")) goto_line(arg);
    else if (!strcmp(name, "query-replace")) query_replace(0, 1);
    else if (!strcmp(name, "query-replace-regexp")) query_replace(1, 1);
    else if (!strcmp(name, "replace-string")) query_replace(0, 0);
    else if (!strcmp(name, "replace-regexp")) query_replace(1, 0);
    else if (!strcmp(name, "save-buffer")) save_buffer(b);
    else if (!strcmp(name, "write-file")) write_file();
    else if (!strcmp(name, "find-file")) find_file();
    else if (!strcmp(name, "insert-file")) insert_file();
    else if (!strcmp(name, "kill-buffer")) kill_buffer();
    else if (!strcmp(name, "switch-to-buffer")) switch_buffer();
    else if (!strcmp(name, "list-buffers")) list_buffers();
    else if (!strcmp(name, "save-some-buffers")) save_some_buffers();
    else if (!strcmp(name, "revert-buffer")) revert_buffer();
    else if (!strcmp(name, "undo")) undo_cmd(0);
    else if (!strcmp(name, "undo-redo")) undo_cmd(1);
    else if (!strcmp(name, "set-mark-command")) {
        b->mark_set = 1, b->mark_y = b->cy, b->mark_x = b->cx;
        msg("Mark set");
    } else if (!strcmp(name, "exchange-point-and-mark")) {
        int y = b->mark_y, x = b->mark_x;
        b->mark_y = b->cy, b->mark_x = b->cx, b->cy = y, b->cx = x, b->mark_set = 1;
    } else if (!strcmp(name, "mark-whole-buffer")) {
        b->mark_set = 1, b->mark_y = b->nlines - 1, b->mark_x = L(b->nlines - 1)->len;
        b->cy = b->cx = 0;
    } else if (!strcmp(name, "display-line-numbers-mode") || !strcmp(name, "linum-mode")) {
        vopt.numbers = !vopt.numbers;
    } else if (!strcmp(name, "whitespace-mode")) {
        vopt.list = !vopt.list;
    } else if (!strcmp(name, "font-lock-mode")) {
        vopt.highlight = !vopt.highlight;
        b->hl_valid = 0;
    } else if (!strcmp(name, "hl-search-mode")) {
        vopt.hlsearch = !vopt.hlsearch;
    } else if (!strcmp(name, "toggle-case-fold-search")) {
        opt_ignorecase = !opt_ignorecase;
        msg("Case folding %s", opt_ignorecase ? "enabled" : "disabled");
    } else if (!strcmp(name, "count-words")) count_words();
    else if (!strcmp(name, "what-line")) msg("Line %d", b->cy + 1);
    else if (!strcmp(name, "shell-command")) shell_command();
    else if (!strcmp(name, "isearch-forward")) isearch(1);
    else if (!strcmp(name, "isearch-backward")) isearch(-1);
    else if (!strcmp(name, "beginning-of-buffer")) b->cy = b->cx = 0;
    else if (!strcmp(name, "end-of-buffer")) b->cy = b->nlines - 1, b->cx = L(b->cy)->len;
    else if (!strcmp(name, "upcase-region")) case_region(1);
    else if (!strcmp(name, "downcase-region")) case_region(0);
    else if (!strcmp(name, "kill-region") || !strcmp(name, "copy-region-as-kill")) {
        int y1, x1, y2, x2;
        if (region(&y1, &x1, &y2, &x2)) {
            if (name[0] == 'k') {
                if (!readonly_check())
                    delete_range(y1, x1, y2, x2, 1, 0);
            } else {
                int len;
                char *t = buf_text(b, y1, x1, y2, x2, &len);
                kill_push(t, len, 0);
                free(t);
            }
            b->mark_set = 0;
        }
    } else if (!strcmp(name, "yank")) {
        if (nkills)
            insert_text(kills[0], kill_len[0]);
    } else if (!strcmp(name, "delete-trailing-whitespace")) {
        for (int y = 0; y < b->nlines; y++) {
            int e = L(y)->len;
            while (e > 0 && (L(y)->s[e - 1] == ' ' || L(y)->s[e - 1] == '\t'))
                e--;
            if (e < L(y)->len)
                free(buf_delete(b, y, e, y, L(y)->len));
        }
        if (b->cx > L(b->cy)->len)
            b->cx = L(b->cy)->len;
    } else if (!strcmp(name, "set-tab-width")) {
        char *s = prompt("Tab width: ", 0, 0);
        if (s && atoi(s) > 0 && atoi(s) <= 16)
            vopt.tabstop = atoi(s);
    } else if (!strcmp(name, "indent-tabs-mode")) {
        msg("Indentation uses spaces");
    } else if (!strcmp(name, "suspend-emacs")) term_suspend();
    else if (!strcmp(name, "save-buffers-kill-emacs")) kill_emacs();
    else if (!strcmp(name, "kill-emacs")) quit_now = 1;
    else if (!strcmp(name, "help") || !strcmp(name, "describe-bindings")) help();
    else if (!strcmp(name, "version")) msg("edit (Eunuchs) emacs personality 1.0");
    else err("[No match] %s", name);
}

static void help(void)
{
    static const char *const lines[] = {
        "Movement:  C-f C-b C-n C-p  C-a C-e  M-f M-b  C-v M-v  M-< M->  M-g g line",
        "Editing:   C-d DEL  M-d M-DEL  C-k kill line  C-y yank  M-y cycle  C-o C-t M-u M-l M-c",
        "Region:    C-SPC mark  C-w kill  M-w copy  C-x C-x swap  C-x h all  C-x C-u/C-l case",
        "Search:    C-s C-r incremental  M-% query-replace  M-x replace-regexp",
        "Files:     C-x C-f open  C-x C-s save  C-x C-w save as  C-x i insert  C-x C-c quit",
        "Buffers:   C-x b switch  C-x k kill  C-x C-b list  C-x <left>/<right>",
        "Other:     C-_ undo  M-_ redo  C-u N arg  C-g cancel  C-l recenter  M-! shell  M-x cmd",
        "(press any key)",
    };
    int n = sizeof(lines) / sizeof(lines[0]);
    draw();
    for (int i = 0; i < n; i++)
        outf("\033[%d;1H\033[0;7m\033[K %s\033[0m", rows - n + i, lines[i]);
    out_flush();
    term_getkey();
}

static void undo_cmd(int redo)
{
    if (redo ? !undo_redo(cur) : !undo_undo(cur))
        err(redo ? "No further redo information" : "No further undo information");
    else
        msg(redo ? "Redo" : "Undo");
}

/* ---- the command loop ---- */

static int arg_val, arg_given;

static void cmd_cx(void)
{
    struct buffer *b = cur;
    msg("C-x-");
    draw();
    int k = term_getkey();
    message[0] = 0;
    switch (k) {
    case CTRL('C'): kill_emacs(); break;
    case CTRL('S'): save_buffer(b); break;
    case CTRL('W'): write_file(); break;
    case CTRL('F'): find_file(); break;
    case CTRL('V'): revert_buffer(); break;
    case CTRL('X'): {
        if (!b->mark_y && !b->mark_x && !b->mark_set) {
            err("No mark set in this buffer");
            break;
        }
        int y = b->mark_y, x = b->mark_x;
        b->mark_y = b->cy, b->mark_x = b->cx;
        b->cy = y < b->nlines ? y : b->nlines - 1;
        b->cx = x <= L(b->cy)->len ? x : L(b->cy)->len;
        b->mark_set = 1;
        break;
    }
    case CTRL('B'): list_buffers(); break;
    case CTRL('U'): case_region(1); break;
    case CTRL('L'): case_region(0); break;
    case CTRL('Z'): term_suspend(); break;
    case 's': save_some_buffers(); break;
    case 'i': insert_file(); break;
    case 'b': switch_buffer(); break;
    case 'k': kill_buffer(); break;
    case 'u': undo_cmd(0); break;
    case 'h':
        b->mark_set = 1;
        b->mark_y = b->nlines - 1;
        b->mark_x = L(b->nlines - 1)->len;
        b->cy = b->cx = 0;
        break;
    case K_RIGHT: cur = b->next ? b->next : buffers; break;
    case K_LEFT: {
        struct buffer *p = buffers;
        while (p->next && p->next != b)
            p = p->next;
        if (b == buffers)
            while (p->next)
                p = p->next;
        cur = p;
        break;
    }
    case '=': {
        int c = b->cx < L(b->cy)->len ? (unsigned char)L(b->cy)->s[b->cx] : '\n';
        msg("Char: %c (%d, #o%o, #x%x)  line %d  column %d", c == '\n' ? ' ' : c, c, c, c, b->cy + 1,
            screen_col(b, b->cy, b->cx));
        break;
    }
    case CTRL('G'): msg("Quit"); break;
    default: err("C-x %s%c is undefined", k < 32 ? "C-" : "", k < 32 ? k + 96 : k & 0xff); break;
    }
}

static void edit_key(int k, int n);

static void command(int k)
{
    struct buffer *b = cur;
    int n = arg_given ? arg_val : 1;
    int ro = b->readonly;
    this_cmd = C_OTHER;
    switch (k) {
    case K_RESIZE:
        this_cmd = last_cmd;
        return;
    case CTRL('U'):
    case K_META | '0': case K_META | '1': case K_META | '2': case K_META | '3': case K_META | '4':
    case K_META | '5': case K_META | '6': case K_META | '7': case K_META | '8': case K_META | '9':
    case K_META | '-': {
        /* C-u: 4, C-u C-u: 16, C-u digits, M-digits, a leading - negates */
        int v = 4, digits = 0, neg = 0;
        if (k == (K_META | '-'))
            neg = 1, v = 1;
        else if (k != CTRL('U'))
            v = (k & 0xff) - '0', digits = 1;
        for (;;) {
            msg("%s%s%d-", k == CTRL('U') ? "C-u " : "M-", neg ? "-" : "", v);
            draw();
            int c = term_getkey();
            if ((c & K_META) && (c & 0xff) >= '0' && (c & 0xff) <= '9' && c < (K_META | 0x100))
                c &= 0xff;                  /* M-1 M-2 continues the number */
            if (c == CTRL('U') && !digits) {
                v *= 4;
                continue;
            }
            if (c >= '0' && c <= '9') {
                v = digits ? v * 10 + c - '0' : c - '0';
                digits = 1;
                continue;
            }
            if (c == '-' && !digits && !neg) {
                neg = 1;
                v = 1;
                continue;
            }
            message[0] = 0;
            arg_val = neg ? -v : v;
            arg_given = 1;
            command(c);
            arg_given = 0;
            return;
        }
    }
    case CTRL('G'):
        b->mark_set = 0;
        msg("Quit");
        break;
    case CTRL('F'): case K_RIGHT:
        for (int i = 0; i < n; i++)
            forward_char();
        break;
    case CTRL('B'): case K_LEFT:
        for (int i = 0; i < n; i++)
            backward_char();
        break;
    case CTRL('N'): case K_DOWN:
        line_move(n);
        return_vmove:
        last_cmd = C_VMOVE;
        return;
    case CTRL('P'): case K_UP:
        line_move(-n);
        goto return_vmove;
    case CTRL('A'): case K_HOME:
        b->cx = 0;
        break;
    case CTRL('E'): case K_END:
        b->cx = L(b->cy)->len;
        break;
    case K_META | 'f': case K_CRIGHT: case K_META | K_RIGHT:
        for (int i = 0; i < n; i++)
            forward_word();
        break;
    case K_META | 'b': case K_CLEFT: case K_META | K_LEFT:
        for (int i = 0; i < n; i++)
            backward_word();
        break;
    case K_META | 'a': case K_META | '{':
        /* back to the start of the paragraph */
        while (b->cy > 0 && L(b->cy - 1)->len == 0)
            b->cy--;
        while (b->cy > 0 && L(b->cy - 1)->len)
            b->cy--;
        b->cx = 0;
        break;
    case K_META | 'e': case K_META | '}':
        while (b->cy < b->nlines - 1 && L(b->cy)->len == 0)
            b->cy++;
        while (b->cy < b->nlines - 1 && L(b->cy)->len)
            b->cy++;
        b->cx = L(b->cy)->len;
        break;
    case CTRL('V'): case K_PGDN: {
        int page = text_rows() - 2;
        if (b->top + text_rows() >= b->nlines) {
            err("End of buffer");
            b->cy = b->nlines - 1;
            break;
        }
        b->top += arg_given ? n : page;
        b->cy = b->cy < b->top ? b->top : b->cy + (arg_given ? n : page);
        if (b->cy >= b->nlines)
            b->cy = b->nlines - 1;
        b->cx = 0;
        break;
    }
    case K_META | 'v': case K_PGUP: {
        int page = text_rows() - 2;
        if (b->top == 0) {
            err("Beginning of buffer");
            b->cy = 0;
            b->cx = 0;
            break;
        }
        b->top -= arg_given ? n : page;
        if (b->top < 0)
            b->top = 0;
        b->cy -= arg_given ? n : page;
        if (b->cy < 0)
            b->cy = 0;
        b->cx = 0;
        break;
    }
    case K_META | '<':
        b->mark_y = b->cy, b->mark_x = b->cx;
        b->cy = b->cx = 0;
        msg("Mark set");
        break;
    case K_META | '>':
        b->mark_y = b->cy, b->mark_x = b->cx;
        b->cy = b->nlines - 1;
        b->cx = L(b->cy)->len;
        msg("Mark set");
        break;
    case K_META | 'g': {
        msg("M-g-");
        draw();
        int c = term_getkey();
        message[0] = 0;
        if (c == 'g' || c == (K_META | 'g'))
            goto_line(arg_given ? arg_val : 0);
        break;
    }
    case CTRL('L'): {
        /* recenter, then top, then bottom */
        static int phase;
        int tr = text_rows();
        phase = last_cmd == 'L' ? (phase + 1) % 3 : 0;
        b->top = phase == 0 ? b->cy - tr / 2 : phase == 1 ? b->cy : b->cy - tr + 1;
        if (b->top < 0)
            b->top = 0;
        out("\033[2J");
        this_cmd = 'L';
        break;
    }
    case CTRL('@'):
        b->mark_set = 1;
        b->mark_y = b->cy, b->mark_x = b->cx;
        msg("Mark set");
        break;
    case CTRL('S'):
        isearch(1);
        break;
    case CTRL('R'):
        isearch(-1);
        break;
    case K_META | '%':
        query_replace(0, 1);
        break;
    case K_META | 'x':
        mx(arg_given ? arg_val : 0);
        break;
    case K_META | '!':
        shell_command();
        break;
    case CTRL('X'):
        cmd_cx();
        break;
    case CTRL('Z'):
        term_suspend();
        break;
    case CTRL('_'): case K_META | CTRL('_'):
        undo_cmd(k != CTRL('_'));
        break;
    case K_META | '_':
        undo_cmd(1);
        break;
    case K_META | 'w': {
        int y1, x1, y2, x2;
        if (region(&y1, &x1, &y2, &x2)) {
            int len;
            char *t = buf_text(b, y1, x1, y2, x2, &len);
            kill_push(t, len, 0);
            this_cmd = C_OTHER;
            free(t);
            b->mark_set = 0;
            msg("Copied region");
        }
        break;
    }
    case CTRL('Y'):
        if (ro || readonly_check())
            break;
        if (!nkills) {
            err("Kill ring is empty");
            break;
        }
        undo_begin(b);
        b->mark_y = b->cy, b->mark_x = b->cx;
        yank_y1 = b->cy, yank_x1 = b->cx;
        kill_idx = 0;
        insert_text(kills[0], kill_len[0]);
        yank_y2 = b->cy, yank_x2 = b->cx;
        this_cmd = C_YANK;
        break;
    case K_META | 'y':
        if (last_cmd != C_YANK) {
            err("Previous command was not a yank");
            break;
        }
        undo_begin(b);
        free(buf_delete(b, yank_y1, yank_x1, yank_y2, yank_x2));
        kill_idx = (kill_idx + 1) % nkills;
        b->cy = yank_y1, b->cx = yank_x1;
        insert_text(kills[kill_idx], kill_len[kill_idx]);
        yank_y2 = b->cy, yank_x2 = b->cx;
        this_cmd = C_YANK;
        break;
    case K_META | 'u': case K_META | 'l': case K_META | 'c':
        undo_begin(b);
        case_word(k & 0xff, n);
        break;
    case K_META | 'q':
        msg("fill-paragraph is not supported");
        break;
    case K_META | K_ESC: case K_META | 27:
        b->mark_set = 0;
        break;
    default:
        if (readonly_check())
            break;
        edit_key(k, n);
        break;
    }
    last_cmd = this_cmd;
}

/* Commands that change the text. */
static void edit_key(int k, int n)
{
    struct buffer *b = cur;
    switch (k) {
    case CTRL('D'): case K_DEL:
        for (int i = 0; i < n; i++) {
            if (b->cx < L(b->cy)->len)
                free(buf_delete(b, b->cy, b->cx, b->cy, utf8_next(L(b->cy)->s, L(b->cy)->len, b->cx)));
            else if (b->cy < b->nlines - 1)
                free(buf_delete(b, b->cy, b->cx, b->cy + 1, 0));
        }
        break;
    case 127: case CTRL('H'):
        for (int i = 0; i < n; i++) {
            int y = b->cy, x = b->cx;
            if (y == 0 && x == 0)
                break;
            backward_char();
            free(buf_delete(b, b->cy, b->cx, y, x));
        }
        break;
    case K_META | 'd': {
        undo_begin(b);
        int y = b->cy, x = b->cx;
        for (int i = 0; i < n; i++)
            forward_word();
        int ey = b->cy, ex = b->cx;
        b->cy = y, b->cx = x;
        if (before(y, x, ey, ex))
            delete_range(y, x, ey, ex, 1, 0);
        break;
    }
    case K_META | 127: case K_META | CTRL('H'): {
        undo_begin(b);
        int y = b->cy, x = b->cx;
        for (int i = 0; i < n; i++)
            backward_word();
        if (before(b->cy, b->cx, y, x))
            delete_range(b->cy, b->cx, y, x, 1, 1);
        break;
    }
    case CTRL('K'): {
        undo_begin(b);
        int y = b->cy, x = b->cx, ey, ex;
        if (arg_given) {
            /* C-u N C-k: N whole lines */
            ey = y + n < b->nlines ? y + n : b->nlines - 1;
            ex = y + n < b->nlines ? 0 : L(ey)->len;
        } else {
            /* to the end of the line; through the newline if only blanks remain */
            int e = x;
            while (e < L(y)->len && (L(y)->s[e] == ' ' || L(y)->s[e] == '\t'))
                e++;
            if (e == L(y)->len) {
                if (y == b->nlines - 1) {
                    if (x == e) {
                        err("End of buffer");
                        break;
                    }
                    ey = y, ex = e;
                } else {
                    ey = y + 1, ex = 0;
                }
            } else {
                ey = y, ex = L(y)->len;
            }
        }
        delete_range(y, x, ey, ex, 1, 0);
        break;
    }
    case CTRL('W'): {
        int y1, x1, y2, x2;
        if (!region(&y1, &x1, &y2, &x2))
            break;
        undo_begin(b);
        delete_range(y1, x1, y2, x2, 1, 0);
        b->mark_set = 0;
        break;
    }
    case CTRL('O'):
        for (int i = 0; i < n; i++) {
            int y = b->cy, x = b->cx;
            insert_text("\n", 1);
            b->cy = y, b->cx = x;
        }
        break;
    case CTRL('T'): {
        /* swap the characters around point (at end of line: the two before it) */
        struct line *l = L(b->cy);
        if (l->len < 2)
            break;
        int x = b->cx;
        if (x == 0)
            x = 1;
        if (x >= l->len)
            x = l->len - 1;
        undo_begin(b);
        char *t = buf_delete(b, b->cy, x - 1, b->cy, x + 1);
        char sw[2] = { t[1], t[0] };
        free(t);
        buf_insert(b, b->cy, x - 1, sw, 2, 0, 0);
        b->cx = x + 1;
        break;
    }
    case K_META | 't': {
        /* transpose words */
        undo_begin(b);
        int y = b->cy;
        backward_word();
        int a1 = b->cx;
        forward_word();
        int a2 = b->cx;
        forward_word();
        int b2 = b->cx;
        backward_word();
        int b1 = b->cx;
        if (b->cy != y || a1 >= a2 || a2 > b1 || b1 >= b2) {
            b->cy = y;
            err("Don't have two things to transpose");
            break;
        }
        char *w1 = xstrndup(L(y)->s + a1, a2 - a1);
        char *mid = xstrndup(L(y)->s + a2, b1 - a2);
        char *w2 = xstrndup(L(y)->s + b1, b2 - b1);
        free(buf_delete(b, y, a1, y, b2));
        int len = 0;
        char *all = xmalloc(b2 - a1 + 1);
        memcpy(all + len, w2, b2 - b1), len += b2 - b1;
        memcpy(all + len, mid, b1 - a2), len += b1 - a2;
        memcpy(all + len, w1, a2 - a1), len += a2 - a1;
        buf_insert(b, y, a1, all, len, 0, 0);
        free(all), free(w1), free(w2), free(mid);
        b->cy = y, b->cx = b2;
        break;
    }
    case CTRL('Q'): {
        msg("C-q-");
        draw();
        int c = term_getkey();
        message[0] = 0;
        if (c < 256) {
            char ch = (char)c;
            for (int i = 0; i < n; i++)
                insert_text(&ch, 1);
        }
        break;
    }
    case '\t':
        indent_line();
        break;
    case '\r': case CTRL('J'): {
        for (int i = 0; i < n; i++) {
            /* C-j and Enter both keep the indentation */
            int ind;
            line_indent_end(L(b->cy), &ind);
            if (ind > b->cx)
                ind = b->cx;
            char *pre = xstrndup(L(b->cy)->s, ind);
            /* drop trailing blanks before the break */
            int e = b->cx;
            while (e > ind && (L(b->cy)->s[e - 1] == ' ' || L(b->cy)->s[e - 1] == '\t'))
                e--;
            if (e < b->cx) {
                free(buf_delete(b, b->cy, e, b->cy, b->cx));
                b->cx = e;
            }
            insert_text("\n", 1);
            insert_text(pre, ind);
            free(pre);
        }
        break;
    }
    case K_META | '\\': {
        int s = b->cx, e = b->cx;
        struct line *l = L(b->cy);
        while (s > 0 && (l->s[s - 1] == ' ' || l->s[s - 1] == '\t'))
            s--;
        while (e < l->len && (l->s[e] == ' ' || l->s[e] == '\t'))
            e++;
        if (e > s)
            free(buf_delete(b, b->cy, s, b->cy, e));
        b->cx = s;
        break;
    }
    case K_META | ' ': {
        int s = b->cx, e = b->cx;
        struct line *l = L(b->cy);
        while (s > 0 && (l->s[s - 1] == ' ' || l->s[s - 1] == '\t'))
            s--;
        while (e < l->len && (l->s[e] == ' ' || l->s[e] == '\t'))
            e++;
        if (e > s)
            free(buf_delete(b, b->cy, s, b->cy, e));
        b->cx = s;
        insert_text(" ", 1);
        break;
    }
    default:
        if (k >= 32 && k < 256) {
            /* self-insert; a UTF-8 sequence arrives as several bytes */
            char ch[4] = { (char)k };
            int len = 1;
            if (k >= 0xc0) {
                int need = k >= 0xf0 ? 3 : k >= 0xe0 ? 2 : 1;
                for (int i = 0; i < need; i++)
                    ch[len++] = (char)term_getkey();
            }
            for (int i = 0; i < n; i++)
                insert_text(ch, len);
            break;
        }
        if (k & K_META)
            err("M-%c is undefined", k & 0xff);
        else if (k < 32)
            err("C-%c is undefined", k + 96);
        break;
    }
}

static void emacs_command_key(int k)
{
    command(k);
}

/* A new undo group per command; runs of typed characters share one (up to 20, as in Emacs). */
static void undo_begin_if_new(int k)
{
    static int prev_insert, run;
    int ins = k >= 32 && k < 256;
    if (ins && prev_insert && run < 20) {
        run++;
    } else {
        undo_begin(cur);
        run = 0;
    }
    prev_insert = ins;
}

static const char *const modes[] = { "Fundamental", "C", "Shell-script", "Makefile", "Python" };

/* The mode line: -UUU:**-  name   Top L12  (C) */
static void status(char *buf, int size)
{
    struct buffer *b = cur;
    int tr = text_rows();
    char where[8];
    if (b->nlines <= tr)
        snprintf(where, sizeof(where), "All");
    else if (b->top == 0)
        snprintf(where, sizeof(where), "Top");
    else if (b->top + tr >= b->nlines)
        snprintf(where, sizeof(where), "Bot");
    else
        snprintf(where, sizeof(where), "%d%%", b->top * 100 / (b->nlines - tr));
    const char *flags = b->readonly ? "%%" : b->dirty ? "**" : "--";
    int col = screen_col(b, b->cy, b->cx);
    snprintf(buf, size, "-UUU:%s-  %-18s %-4s L%-5d C%-4d (%s)", flags, b->name, where, b->cy + 1, col,
             modes[b->syntax]);
}

/* Show the region (transient mark) while the mark is active. */
static void show_region(void)
{
    struct buffer *b = cur;
    if (b->mark_set && (b->mark_y != b->cy || b->mark_x != b->cx) && b->mark_y < b->nlines) {
        vis_mode = 3;
        vis_y1 = b->mark_y, vis_x1 = b->mark_x;
        vis_y2 = b->cy, vis_x2 = b->cx;
    } else {
        vis_mode = 0;
    }
}

int emacs_main(int argc, char **argv)
{
    int start_line = 0;
    struct buffer *first = 0;
    for (int i = argc - 1; i >= 1; i--) {
        const char *a = argv[i];
        if (a[0] == '+' && a[1]) {
            start_line = atoi(a + 1);
            continue;
        }
        if (!strcmp(a, "-nw") || !strcmp(a, "-q") || !strcmp(a, "-Q"))
            continue;
        if (a[0] == '-' && a[1]) {
            eprintf("emacs: unknown option %s (usage: emacs [+LINE] [file ...])\n", a);
            return 2;
        }
        struct buffer *b = buf_new(a);
        if (buf_load(b, a) < 0) {
            eprintf("emacs: %s\n", message);
            message[0] = 0;
            buf_free(b);
            continue;
        }
        first = b;
    }
    if (!first) {
        first = buf_new(0);
        snprintf(first->name, sizeof(first->name), "*scratch*");
    }
    cur = first;
    if (start_line > 0)
        cur->cy = start_line - 1 < cur->nlines ? start_line - 1 : cur->nlines - 1;
    if (term_init() < 0)
        return 1;
    opt_ignorecase = 1;                 /* case-fold-search, with smartcase */
    opt_smartcase = 1;
    vopt.hlsearch = 0;
    vopt.status_line = 1;
    show_tildes = 0;
    draw_status = status;
    mx_commands = commands;
    if (first->newfile)
        msg("(New file)");
    else if (!first->path)
        msg("For help, type M-x help (C-x C-c to quit)");
    while (!quit_now) {
        show_region();
        draw();
        int k = term_getkey();
        message[0] = 0;
        message_error = 0;
        if (k != K_RESIZE)
            undo_begin_if_new(k);
        command(k);
        if (cur->cx > L(cur->cy)->len)
            cur->cx = L(cur->cy)->len;
    }
    term_done();
    return 0;
}
