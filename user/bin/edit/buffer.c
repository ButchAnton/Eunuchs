/* edit: buffers of lines, files, and undo/redo. */
#include "edit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct buffer *buffers, *cur;

/* ---- memory ---- */

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        term_done();
        eprintf("edit: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        term_done();
        eprintf("edit: out of memory\n");
        exit(1);
    }
    return q;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

int is_word_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c >= 0x80;
}

int utf8_next(const char *s, int len, int pos)
{
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos++;
    return pos;
}

int utf8_prev(const char *s, int pos)
{
    if (pos <= 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}

/* ---- lines ---- */

static void line_reserve(struct line *l, int need)
{
    if (need + 1 <= l->cap)
        return;
    int cap = l->cap ? l->cap : 16;
    while (cap < need + 1)
        cap *= 2;
    l->s = xrealloc(l->s, cap);
    l->cap = cap;
}

static void lines_insert(struct buffer *b, int at, int n)
{
    if (b->nlines + n > b->cap) {
        int cap = b->cap ? b->cap : 64;
        while (cap < b->nlines + n)
            cap *= 2;
        b->lines = xrealloc(b->lines, cap * sizeof(struct line));
        b->cap = cap;
    }
    memmove(b->lines + at + n, b->lines + at, (b->nlines - at) * sizeof(struct line));
    memset(b->lines + at, 0, n * sizeof(struct line));
    for (int i = 0; i < n; i++)
        line_reserve(&b->lines[at + i], 0), b->lines[at + i].s[0] = 0;
    b->nlines += n;
}

static void lines_remove(struct buffer *b, int at, int n)
{
    for (int i = 0; i < n; i++)
        free(b->lines[at + i].s);
    memmove(b->lines + at, b->lines + at + n, (b->nlines - at - n) * sizeof(struct line));
    b->nlines -= n;
}

const char *line_indent_end(struct line *l, int *n)
{
    int i = 0;
    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
        i++;
    *n = i;
    return l->s + i;
}

void buf_touch(struct buffer *b, int from_line)
{
    if (b->hl_valid > from_line)
        b->hl_valid = from_line;
    b->dirty = 1;
}

/* ---- raw edits (no undo) ---- */

static void raw_insert(struct buffer *b, int y, int x, const char *t, int n, int *ey, int *ex)
{
    if (y >= b->nlines)
        lines_insert(b, b->nlines, 1);
    struct line *l = &b->lines[y];
    if (x > l->len)
        x = l->len;
    /* the text after the insertion point moves to the end of the last new line */
    int tail_len = l->len - x;
    char *tail = xstrndup(l->s + x, tail_len);
    l->len = x;
    int cy = y;
    for (int i = 0; i < n;) {
        const char *nl = memchr(t + i, '\n', n - i);
        int seg = nl ? (int)(nl - (t + i)) : n - i;
        struct line *c = &b->lines[cy];
        line_reserve(c, c->len + seg);
        memcpy(c->s + c->len, t + i, seg);
        c->len += seg;
        c->s[c->len] = 0;
        i += seg;
        if (nl) {
            i++;
            lines_insert(b, cy + 1, 1);
            cy++;
        }
    }
    struct line *c = &b->lines[cy];
    *ey = cy;
    *ex = c->len;
    line_reserve(c, c->len + tail_len);
    memcpy(c->s + c->len, tail, tail_len);
    c->len += tail_len;
    c->s[c->len] = 0;
    free(tail);
    buf_touch(b, y);
}

char *buf_text(struct buffer *b, int y1, int x1, int y2, int x2, int *lenp)
{
    int cap = 64, n = 0;
    char *s = xmalloc(cap);
    for (int y = y1; y <= y2 && y < b->nlines; y++) {
        struct line *l = &b->lines[y];
        int from = y == y1 ? x1 : 0, to = y == y2 ? x2 : l->len;
        if (from > l->len)
            from = l->len;
        if (to > l->len)
            to = l->len;
        int need = n + (to - from) + 2;
        if (need > cap) {
            while (cap < need)
                cap *= 2;
            s = xrealloc(s, cap);
        }
        if (to > from)
            memcpy(s + n, l->s + from, to - from), n += to - from;
        if (y < y2)
            s[n++] = '\n';
    }
    s[n] = 0;
    if (lenp)
        *lenp = n;
    return s;
}

static char *raw_delete(struct buffer *b, int y1, int x1, int y2, int x2, int *lenp)
{
    if (y2 >= b->nlines) {
        y2 = b->nlines - 1;
        x2 = b->lines[y2].len;
    }
    char *t = buf_text(b, y1, x1, y2, x2, lenp);
    struct line *a = &b->lines[y1], *z = &b->lines[y2];
    if (x2 > z->len)
        x2 = z->len;
    int keep = z->len - x2;
    line_reserve(a, x1 + keep);
    memmove(a->s + x1, z->s + x2, keep);
    a->len = x1 + keep;
    a->s[a->len] = 0;
    if (y2 > y1)
        lines_remove(b, y1 + 1, y2 - y1);
    if (!b->nlines)
        lines_insert(b, 0, 1);
    buf_touch(b, y1);
    return t;
}

/* ---- undo ---- */

static int undo_cy, undo_cx;        /* the cursor when the current group began */

void undo_begin(struct buffer *b)
{
    b->group++;
    undo_cy = b->cy;
    undo_cx = b->cx;
}

static void push(struct undo **v, int *n, int *cap, struct undo u)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = xrealloc(*v, *cap * sizeof(struct undo));
    }
    (*v)[(*n)++] = u;
}

static void record(struct buffer *b, int kind, int y, int x, const char *t, int n)
{
    /* coalesce consecutive typing into one record */
    if (b->nundo && kind == U_INSERT) {
        struct undo *u = &b->undo[b->nundo - 1];
        if (u->kind == U_INSERT && u->group == b->group && !memchr(t, '\n', n)) {
            /* does this insertion continue the previous one? */
            int ey = u->y, ex = u->x;
            for (int i = 0; i < u->len; i++) {
                if (u->text[i] == '\n')
                    ey++, ex = 0;
                else
                    ex++;
            }
            if (ey == y && ex == x) {
                u->text = xrealloc(u->text, u->len + n + 1);
                memcpy(u->text + u->len, t, n);
                u->len += n;
                u->text[u->len] = 0;
                goto done;
            }
        }
    }
    struct undo u = { kind, y, x, xstrndup(t, n), n, b->group, undo_cy, undo_cx };
    push(&b->undo, &b->nundo, &b->capundo, u);
done:
    for (int i = 0; i < b->nredo; i++)
        free(b->redo[i].text);
    b->nredo = 0;
}

void buf_insert(struct buffer *b, int y, int x, const char *text, int len, int *ey, int *ex)
{
    int dy, dx;
    if (y < b->nlines && x > b->lines[y].len)
        x = b->lines[y].len;
    raw_insert(b, y, x, text, len, ey ? ey : &dy, ex ? ex : &dx);
    record(b, U_INSERT, y, x, text, len);
}

char *buf_delete(struct buffer *b, int y1, int x1, int y2, int x2)
{
    if (y1 > y2 || (y1 == y2 && x1 >= x2)) {
        if (y1 == y2 && x1 == x2)
            return xstrdup("");
        int ty = y1, tx = x1;
        y1 = y2, x1 = x2, y2 = ty, x2 = tx;
    }
    int n;
    char *t = raw_delete(b, y1, x1, y2, x2, &n);
    record(b, U_DELETE, y1, x1, t, n);
    return t;
}

/* the end of `text` inserted at (y, x) */
static void text_end(int y, int x, const char *t, int n, int *ey, int *ex)
{
    for (int i = 0; i < n; i++) {
        if (t[i] == '\n')
            y++, x = 0;
        else
            x++;
    }
    *ey = y;
    *ex = x;
}

int undo_undo(struct buffer *b)
{
    if (!b->nundo)
        return 0;
    int g = b->undo[b->nundo - 1].group;
    int cy = 0, cx = 0;
    while (b->nundo && b->undo[b->nundo - 1].group == g) {
        struct undo u = b->undo[--b->nundo];
        int ey, ex, n;
        if (u.kind == U_INSERT) {
            text_end(u.y, u.x, u.text, u.len, &ey, &ex);
            free(raw_delete(b, u.y, u.x, ey, ex, &n));
        } else {
            raw_insert(b, u.y, u.x, u.text, u.len, &ey, &ex);
        }
        cy = u.cy;
        cx = u.cx;
        push(&b->redo, &b->nredo, &b->capredo, u);
    }
    b->cy = cy < b->nlines ? cy : b->nlines - 1;
    b->cx = cx;
    if (b->cx > b->lines[b->cy].len)
        b->cx = b->lines[b->cy].len;
    b->group++;
    return 1;
}

int undo_redo(struct buffer *b)
{
    if (!b->nredo)
        return 0;
    int g = b->redo[b->nredo - 1].group;
    int ly = 0, lx = 0;
    while (b->nredo && b->redo[b->nredo - 1].group == g) {
        struct undo u = b->redo[--b->nredo];
        int ey, ex, n;
        if (u.kind == U_INSERT) {
            raw_insert(b, u.y, u.x, u.text, u.len, &ey, &ex);
            ly = u.y, lx = u.x;
        } else {
            text_end(u.y, u.x, u.text, u.len, &ey, &ex);
            free(raw_delete(b, u.y, u.x, ey, ex, &n));
            ly = u.y, lx = u.x;
        }
        push(&b->undo, &b->nundo, &b->capundo, u);
    }
    b->cy = ly < b->nlines ? ly : b->nlines - 1;
    b->cx = lx;
    if (b->cx > b->lines[b->cy].len)
        b->cx = b->lines[b->cy].len;
    b->group++;
    return 1;
}

/* ---- files ---- */

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

void buf_set_syntax(struct buffer *b)
{
    const char *n = b->path ? basename_of(b->path) : "";
    const char *dot = strrchr(n, '.');
    b->syntax = SYN_NONE;
    if (dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".cc") || !strcmp(dot, ".cpp") ||
                !strcmp(dot, ".S") || !strcmp(dot, ".js") || !strcmp(dot, ".java")))
        b->syntax = SYN_C;
    else if (!strcmp(n, "Makefile") || !strcmp(n, "makefile") || (dot && !strcmp(dot, ".mk")))
        b->syntax = SYN_MAKE;
    else if (dot && !strcmp(dot, ".py"))
        b->syntax = SYN_PYTHON;
    else if ((dot && (!strcmp(dot, ".sh") || !strcmp(dot, ".esh"))) || !strcmp(n, "eshrc") ||
             !strcmp(n, ".eshrc") || !strcmp(n, ".esh_profile") || !strcmp(n, "esh_profile"))
        b->syntax = SYN_SHELL;
    else if (b->nlines && b->lines[0].len > 2 && !strncmp(b->lines[0].s, "#!", 2))
        b->syntax = strstr(b->lines[0].s, "python") ? SYN_PYTHON : SYN_SHELL;
    b->hl_valid = 0;
}

struct buffer *buf_new(const char *path)
{
    struct buffer *b = xmalloc(sizeof(*b));
    memset(b, 0, sizeof(*b));
    lines_insert(b, 0, 1);
    for (int i = 0; i < 26; i++)
        b->vmarks[i][0] = -1;
    if (path) {
        b->path = xstrdup(path);
        snprintf(b->name, sizeof(b->name), "%s", basename_of(path));
    } else {
        snprintf(b->name, sizeof(b->name), "[No Name]");
    }
    b->next = buffers;
    buffers = b;
    return b;
}

int buf_load(struct buffer *b, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            b->newfile = 1;
            buf_set_syntax(b);
            return 0;
        }
        err("%s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        err("%s is a directory", path);
        return -1;
    }
    if (access(path, W_OK) < 0)
        b->readonly = 1;
    lines_remove(b, 0, b->nlines);
    char chunk[4096];
    long n;
    struct line *l = 0;
    int at_line_start = 1;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n;) {
            if (at_line_start) {
                lines_insert(b, b->nlines, 1);
                l = &b->lines[b->nlines - 1];
                at_line_start = 0;
            }
            char *nl = memchr(chunk + i, '\n', n - i);
            long seg = nl ? nl - (chunk + i) : n - i;
            line_reserve(l, l->len + (int)seg);
            memcpy(l->s + l->len, chunk + i, seg);
            l->len += (int)seg;
            l->s[l->len] = 0;
            i += seg;
            if (nl) {
                i++;
                at_line_start = 1;
            }
        }
    }
    close(fd);
    if (!b->nlines)
        lines_insert(b, 0, 1);
    b->dirty = 0;
    buf_set_syntax(b);
    return 0;
}

int buf_save(struct buffer *b, const char *path)
{
    if (!path)
        path = b->path;
    if (!path) {
        err("No file name");
        return -1;
    }
    /* write a temporary file next to it, then rename: never a half-written file */
    char tmp[300];
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(tmp, sizeof(tmp), "%.*s/.%s.edit%d", (int)(slash - path), path, slash + 1, getpid());
    else
        snprintf(tmp, sizeof(tmp), ".%s.edit%d", path, getpid());
    int mode = 0644;
    struct stat st;
    if (stat(path, &st) == 0)
        mode = st.st_mode & 07777;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        /* no permission in the directory: write in place */
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
        tmp[0] = 0;
        if (fd < 0) {
            err("\"%s\": %s", path, strerror(errno));
            return -1;
        }
    }
    long total = 0;
    char *buf = xmalloc(8192);
    int used = 0;
    for (int y = 0; y < b->nlines; y++) {
        struct line *l = &b->lines[y];
        for (int i = 0; i <= l->len; i++) {
            if (used == 8192) {
                if (write(fd, buf, used) != used)
                    goto fail;
                used = 0;
            }
            buf[used++] = i < l->len ? l->s[i] : '\n';
            total++;
        }
    }
    if (used && write(fd, buf, used) != used)
        goto fail;
    free(buf);
    close(fd);
    if (tmp[0]) {
        if (rename(tmp, path) < 0) {
            err("\"%s\": %s", path, strerror(errno));
            unlink(tmp);
            return -1;
        }
        chmod(path, mode);
    }
    if (!b->path || strcmp(b->path, path)) {
        free(b->path);
        b->path = xstrdup(path);
        snprintf(b->name, sizeof(b->name), "%s", basename_of(path));
        buf_set_syntax(b);
    }
    b->dirty = 0;
    b->newfile = 0;
    msg("\"%s\" %dL, %ldB written", path, b->nlines, total);
    return 0;
fail:
    err("\"%s\": write failed: %s", path, strerror(errno));
    free(buf);
    close(fd);
    if (tmp[0])
        unlink(tmp);
    return -1;
}

void buf_free(struct buffer *b)
{
    for (struct buffer **pp = &buffers; *pp; pp = &(*pp)->next)
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    for (int i = 0; i < b->nlines; i++)
        free(b->lines[i].s);
    free(b->lines);
    for (int i = 0; i < b->nundo; i++)
        free(b->undo[i].text);
    for (int i = 0; i < b->nredo; i++)
        free(b->redo[i].text);
    free(b->undo);
    free(b->redo);
    free(b->path);
    free(b);
}

int buf_count_dirty(void)
{
    int n = 0;
    for (struct buffer *b = buffers; b; b = b->next)
        n += b->dirty;
    return n;
}
