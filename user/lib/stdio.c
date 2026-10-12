/*
 * stdio: buffered FILE streams.
 *
 * stdout is line-buffered (flushed at every newline, as Eunuchs programs
 * have always expected), stderr unbuffered, other streams fully buffered.
 * Reading from standard input flushes stdout first. exit() flushes
 * every open stream.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct FILE {
    int fd;
    int flags;              /* F_READ F_WRITE F_EOF F_ERR F_APPEND */
    int mode;               /* _IOFBF _IOLBF _IONBF */
    char *buf;
    size_t size;
    size_t pos, len;        /* read: buf[pos..len) unread; write: buf[0..pos) pending */
    int writing;            /* the buffer holds output */
    int ungot;              /* ungetc character or EOF */
    int own_buf;
    struct FILE *next;      /* list of open streams */
};

#define F_READ   1
#define F_WRITE  2
#define F_EOF    4
#define F_ERR    8
#define F_APPEND 16

static char stdout_buf[BUFSIZ], stdin_buf[BUFSIZ];
static FILE f_stdin = { 0, F_READ, _IOFBF, stdin_buf, BUFSIZ, 0, 0, 0, EOF, 0, 0 };
static FILE f_stdout = { 1, F_WRITE, _IOLBF, stdout_buf, BUFSIZ, 0, 0, 0, EOF, 0, 0 };
static FILE f_stderr = { 2, F_WRITE, _IONBF, 0, 0, 0, 0, 0, EOF, 0, 0 };
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;
static FILE *open_files;

/* ---- low level ---- */

static int flush_out(FILE *f)
{
    size_t done = 0;
    while (done < f->pos) {
        long n = write(f->fd, f->buf + done, f->pos - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            f->flags |= F_ERR;
            f->pos = 0;
            return EOF;
        }
        done += n;
    }
    f->pos = 0;
    return 0;
}

int fflush(FILE *f)
{
    if (!f) {
        int r = fflush(stdout);
        fflush(stderr);
        for (FILE *g = open_files; g; g = g->next)
            if (fflush(g))
                r = EOF;
        return r;
    }
    if (f->writing) {
        int r = flush_out(f);
        f->writing = 0;
        return r;
    }
    if (f->pos < f->len && (f->flags & F_READ)) {
        /* drop read-ahead: put the file position back where the reader is */
        lseek(f->fd, -(long)(f->len - f->pos), SEEK_CUR);
        f->pos = f->len = 0;
    }
    return 0;
}

void fflush_stdout(void)
{
    fflush(stdout);
}

static int ensure_buf(FILE *f)
{
    if (f->buf || f->mode == _IONBF)
        return 0;
    f->buf = malloc(BUFSIZ);
    if (!f->buf) {
        f->mode = _IONBF;
        return 0;
    }
    f->size = BUFSIZ;
    f->own_buf = 1;
    return 0;
}

static int put_bytes(FILE *f, const char *p, size_t n)
{
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERR;
        errno = EBADF;
        return EOF;
    }
    if (!f->writing && f->len) {
        /* switching from reading: give back the read-ahead */
        fflush(f);
    }
    f->writing = 1;
    ensure_buf(f);
    if (f->mode == _IONBF || !f->buf) {
        while (n) {
            long w = write(f->fd, p, n);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                f->flags |= F_ERR;
                return EOF;
            }
            p += w;
            n -= w;
        }
        return 0;
    }
    while (n) {
        size_t room = f->size - f->pos;
        size_t k = n < room ? n : room;
        memcpy(f->buf + f->pos, p, k);
        f->pos += k;
        p += k;
        n -= k;
        if (f->pos == f->size && flush_out(f))
            return EOF;
    }
    if (f->mode == _IOLBF && f->pos && memchr(f->buf, '\n', f->pos))
        return flush_out(f);
    return 0;
}

static int fill(FILE *f)
{
    if (f->writing)
        fflush(f);
    if (f->flags & F_EOF)
        return EOF;
    if (f == stdin || f->fd == 0)
        fflush(stdout);                 /* prompts appear before input is read */
    ensure_buf(f);
    char one;
    char *b = f->buf ? f->buf : &one;
    size_t size = f->buf ? f->size : 1;
    for (;;) {
        long n = read(f->fd, b, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            f->flags |= F_ERR;
            return EOF;
        }
        if (n == 0) {
            f->flags |= F_EOF;
            return EOF;
        }
        if (!f->buf) {
            f->ungot = (unsigned char)one;
            return 0;
        }
        f->pos = 0;
        f->len = n;
        return 0;
    }
}

/* ---- opening and closing ---- */

static int parse_mode(const char *mode, int *oflags)
{
    int fl;
    switch (mode[0]) {
    case 'r': fl = F_READ; *oflags = O_RDONLY; break;
    case 'w': fl = F_WRITE; *oflags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': fl = F_WRITE | F_APPEND; *oflags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    if (strchr(mode, '+')) {
        fl |= F_READ | F_WRITE;
        *oflags = (*oflags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
    }
    if (strchr(mode, 'e'))
        *oflags |= O_CLOEXEC;
    if (strchr(mode, 'x'))
        *oflags |= O_EXCL;
    return fl;
}

static FILE *new_file(int fd, int fl)
{
    FILE *f = calloc(1, sizeof(*f));
    if (!f)
        return 0;
    f->fd = fd;
    f->flags = fl;
    f->mode = isatty(fd) ? _IOLBF : _IOFBF;
    f->ungot = EOF;
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int of, fl = parse_mode(mode, &of);
    if (fl < 0)
        return 0;
    int fd = open(path, of, 0666);
    if (fd < 0)
        return 0;
    FILE *f = new_file(fd, fl);
    if (!f)
        close(fd);
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int of, fl = parse_mode(mode, &of);
    if (fl < 0)
        return 0;
    return new_file(fd, fl);
}

static void unlink_file(FILE *f)
{
    for (FILE **pp = &open_files; *pp; pp = &(*pp)->next)
        if (*pp == f) {
            *pp = f->next;
            return;
        }
}

int fclose(FILE *f)
{
    int r = fflush(f);
    if (close(f->fd) < 0)
        r = EOF;
    if (f == stdin || f == stdout || f == stderr) {
        f->flags = 0;
        return r;
    }
    unlink_file(f);
    if (f->own_buf)
        free(f->buf);
    free(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    fflush(f);
    int of, fl = parse_mode(mode, &of);
    if (fl < 0)
        return 0;
    int fd = open(path, of, 0666);
    if (fd < 0)
        return 0;
    if (dup2(fd, f->fd) < 0) {
        close(fd);
        return 0;
    }
    close(fd);
    f->flags = fl;
    f->pos = f->len = 0;
    f->writing = 0;
    f->ungot = EOF;
    return f;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    fflush(f);
    if (f->own_buf)
        free(f->buf);
    f->own_buf = 0;
    f->mode = mode;
    f->buf = mode == _IONBF ? 0 : buf;
    f->size = buf ? size : 0;
    f->pos = f->len = 0;
    return 0;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int fileno(FILE *f) { return f->fd; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

/* ---- characters and blocks ---- */

int fgetc(FILE *f)
{
    if (f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }
    if (f->pos >= f->len || f->writing) {
        if (fill(f))
            return EOF;
        if (f->ungot != EOF) {          /* unbuffered: fill() left it here */
            int c = f->ungot;
            f->ungot = EOF;
            return c;
        }
    }
    return (unsigned char)f->buf[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF)
        return EOF;
    /* The character just read is still in the buffer: back up over it. This
       gives more than one character of pushback (scanf needs it, e.g. "%d"
       on "12abc" reads ahead and returns "abc"). */
    if (f->ungot == EOF && !f->writing && f->buf && f->pos > 0 && f->pos <= f->len &&
        (unsigned char)f->buf[f->pos - 1] == (unsigned char)c) {
        f->pos--;
        f->flags &= ~F_EOF;
        return (unsigned char)c;
    }
    if (f->ungot != EOF)
        return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return put_bytes(f, &ch, 1) ? EOF : (unsigned char)ch;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    return put_bytes(f, s, strlen(s)) ? EOF : 0;
}

int puts(const char *s)
{
    if (put_bytes(stdout, s, strlen(s)) || put_bytes(stdout, "\n", 1))
        return EOF;
    return 0;
}

char *fgets(char *s, int n, FILE *f)
{
    int k = 0;
    while (k < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (!k)
                return 0;
            break;
        }
        s[k++] = (char)c;
        if (c == '\n')
            break;
    }
    if (n > 0)
        s[k] = 0;
    return s;
}

ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f)
{
    size_t n = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF && !n)
            return -1;
        if (n + 2 > *cap || !*line) {
            size_t nc = *cap ? *cap * 2 : 128;
            char *p = realloc(*line, nc);
            if (!p)
                return -1;
            *line = p;
            *cap = nc;
        }
        if (c == EOF)
            break;
        (*line)[n++] = (char)c;
        if (c == delim)
            break;
    }
    (*line)[n] = 0;
    return (ssize_t)n;
}

ssize_t getline(char **line, size_t *cap, FILE *f)
{
    return getdelim(line, cap, '\n', f);
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    size_t want = size * n, got = 0;
    char *d = p;
    if (!want)
        return 0;
    if (f->ungot != EOF) {
        d[got++] = (char)f->ungot;
        f->ungot = EOF;
    }
    while (got < want) {
        if (f->pos < f->len && !f->writing) {
            size_t k = f->len - f->pos;
            if (k > want - got)
                k = want - got;
            memcpy(d + got, f->buf + f->pos, k);
            f->pos += k;
            got += k;
            continue;
        }
        if (want - got >= BUFSIZ && !f->writing) {
            /* big reads go straight to the destination */
            long r = read(f->fd, d + got, want - got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                f->flags |= r ? F_ERR : F_EOF;
                break;
            }
            got += r;
            continue;
        }
        int c = fgetc(f);
        if (c == EOF)
            break;
        d[got++] = (char)c;
    }
    return got / size;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    if (!size || !n)
        return 0;
    return put_bytes(f, p, size * n) ? 0 : n;
}

/* ---- positioning ---- */

int fseek(FILE *f, long off, int whence)
{
    if (whence == SEEK_CUR && !f->writing)
        off -= (long)(f->len - f->pos) + (f->ungot != EOF);
    if (f->writing && flush_out(f))
        return -1;
    f->writing = 0;
    f->pos = f->len = 0;
    f->ungot = EOF;
    if (lseek(f->fd, off, whence) < 0)
        return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    long p = lseek(f->fd, 0, SEEK_CUR);
    if (p < 0)
        return -1;
    if (f->writing)
        return p + (long)f->pos;
    return p - (long)(f->len - f->pos) - (f->ungot != EOF);
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

int fgetpos(FILE *f, fpos_t *pos)
{
    long p = ftell(f);
    if (p < 0)
        return -1;
    *pos = p;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos)
{
    return fseek(f, *pos, SEEK_SET);
}

/* ---- files ---- */

int remove(const char *path)
{
    if (unlink(path) == 0)
        return 0;
    return rmdir(path);
}

static unsigned tmp_seq;

char *tmpnam(char *s)
{
    static char buf[L_tmpnam];
    char *r = s ? s : buf;
    snprintf(r, L_tmpnam, "/tmp/tmp%d.%u", getpid(), tmp_seq++);
    return r;
}

FILE *tmpfile(void)
{
    for (int tries = 0; tries < 100; tries++) {
        char name[L_tmpnam];
        tmpnam(name);
        int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            continue;
        unlink(name);
        return new_file(fd, F_READ | F_WRITE);
    }
    return 0;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

/* ---- formatted output ---- */

struct sink {
    FILE *f;
    char buf[256];
    int n;
};

static void sink_char(char c, void *arg)
{
    struct sink *s = arg;
    s->buf[s->n++] = c;
    if (s->n == (int)sizeof(s->buf)) {
        put_bytes(s->f, s->buf, s->n);
        s->n = 0;
    }
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct sink s;
    s.f = f;
    s.n = 0;
    int n = kvcbprintf(sink_char, &s, fmt, ap);
    if (s.n)
        put_bytes(f, s.buf, s.n);
    return ferror(f) ? -1 : n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    FILE f;
    memset(&f, 0, sizeof(f));
    f.fd = fd;
    f.flags = F_WRITE;
    f.mode = _IONBF;
    f.ungot = EOF;
    if (fd == 1)
        fflush(stdout);
    return vfprintf(&f, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

/* The system's programs: like dprintf, but stdout is flushed first so
 * messages stay in order with normal output. */
int fprintf_fd(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return kvsnprintf(buf, (size_t)1 << 30, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = kvsnprintf(0, 0, fmt, ap2);
    va_end(ap2);
    *out = malloc(n + 1);
    if (!*out)
        return -1;
    return kvsnprintf(*out, n + 1, fmt, ap);
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- exit ---- */

static void (*exit_fns[32])(void);
static int nexit_fns;

int atexit(void (*fn)(void))
{
    if (nexit_fns == 32)
        return -1;
    exit_fns[nexit_fns++] = fn;
    return 0;
}

void __libc_exit_hooks(void)
{
    while (nexit_fns > 0)
        exit_fns[--nexit_fns]();
    fflush(0);
}
