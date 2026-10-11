/*
 * Minimal libc for Eunuchs user programs: syscall wrappers, stdio with
 * buffered stdout, a debugging-friendly malloc, and odds and ends.
 */
#include <dirent.h>
#include <errno.h>
#include <eunuchs.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int errno;

static long ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define SC0(n)          __syscall((n), 0, 0, 0, 0, 0, 0)
#define SC1(n, a)       __syscall((n), (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b)    __syscall((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) __syscall((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) __syscall((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SC5(n, a, b, c, d, e) __syscall((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

ssize_t read(int fd, void *buf, size_t n) { return ret(SC3(SYS_read, fd, buf, n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(SC3(SYS_write, fd, buf, n)); }
pid_t fork(void) { return ret(SC0(SYS_fork)); }
int execve(const char *p, char *const argv[], char *const envp[]) { return ret(SC3(SYS_execve, p, argv, envp)); }
int execv(const char *p, char *const argv[]) { return execve(p, argv, environ); }

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/'))
        return execv(file, argv);
    const char *path = getenv("PATH");
    char buf[256];
    for (const char *p = path ? path : "/bin"; *p;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)n, p, file);
        execv(buf, argv);
        if (errno != ENOENT)
            return -1;
        p += n + (colon ? 1 : 0);
    }
    errno = ENOENT;
    return -1;
}

int pipe2(int fds[2], int flags) { return ret(SC2(SYS_pipe2, fds, flags)); }
int pipe(int fds[2]) { return pipe2(fds, 0); }

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return ret(SC3(SYS_fcntl, fd, cmd, arg));
}

/* ---- terminal ---- */

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    long arg = va_arg(ap, long);
    va_end(ap);
    return ret(SC3(SYS_ioctl, fd, req, arg));
}

int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TCGETS, t); }

int tcsetattr(int fd, int when, const struct termios *t)
{
    return ioctl(fd, when == TCSAFLUSH ? TCSETSF : when == TCSADRAIN ? TCSETSW : TCSETS, t);
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

pid_t tcgetpgrp(int fd)
{
    int pg;
    return ioctl(fd, TIOCGPGRP, &pg) < 0 ? -1 : pg;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    int pg = pgrp;
    return ioctl(fd, TIOCSPGRP, &pg);
}

int isatty(int fd)
{
    struct termios t;
    return tcgetattr(fd, &t) == 0;
}

/* ---- credentials and modes ---- */

uid_t getuid(void) { return SC0(SYS_getuid); }
uid_t geteuid(void) { return SC0(SYS_geteuid); }
gid_t getgid(void) { return SC0(SYS_getgid); }
gid_t getegid(void) { return SC0(SYS_getegid); }
int setuid(uid_t uid) { return ret(SC1(SYS_setuid, uid)); }
int setgid(gid_t gid) { return ret(SC1(SYS_setgid, gid)); }
int chmod(const char *path, int mode) { return ret(SC3(SYS_fchmodat, AT_FDCWD, path, mode)); }
int fchmod(int fd, int mode) { return ret(SC2(SYS_fchmod, fd, mode)); }
int umask(int mask) { return SC1(SYS_umask, mask); }
int chown(const char *path, int uid, int gid)
{
    return ret(SC5(SYS_fchownat, AT_FDCWD, path, uid, gid, 0));
}
int access(const char *path, int mode) { return ret(SC3(SYS_faccessat, AT_FDCWD, path, mode)); }

/* ---- time ---- */

int clock_gettime(int clock, struct timespec *ts) { return ret(SC2(SYS_clock_gettime, clock, ts)); }
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; return ret(SC2(SYS_gettimeofday, tv, 0)); }
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return ret(SC2(SYS_nanosleep, req, rem));
}

time_t time(time_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

unsigned sleep(unsigned seconds)
{
    struct timespec req = { seconds, 0 }, rem = { 0, 0 };
    return nanosleep(&req, &rem) < 0 ? (unsigned)rem.tv_sec : 0;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    time_t s = *t;
    long days = (long)(s / 86400);
    long rem = (long)(s % 86400);
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7);            /* 1970-01-01 was a Thursday */
    int year = 1970;
    for (;;) {
        int leap = (year % 4 == 0 && year % 100) || year % 400 == 0;
        if (days < 365 + leap)
            break;
        days -= 365 + leap;
        year++;
    }
    tm->tm_year = year - 1900;
    tm->tm_yday = (int)days;
    int leap = (year % 4 == 0 && year % 100) || year % 400 == 0;
    int mon = 0;
    while (days >= mdays[mon] + (mon == 1 && leap)) {
        days -= mdays[mon] + (mon == 1 && leap);
        mon++;
    }
    tm->tm_mon = mon;
    tm->tm_mday = (int)days + 1;
    return tm;
}

/* ---- environment ---- */

char **environ;
static char *env_store[64];

char *getenv(const char *name)
{
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=')
            return *e + n + 1;
    return 0;
}

/* Our copy of the environment (the kernel's is read-only to grow). */
static int own_environ(void)
{
    if (environ == env_store)
        return 0;
    int n = 0;
    for (char **e = environ; e && *e && n < 63; e++)
        env_store[n++] = *e;
    env_store[n] = 0;
    environ = env_store;
    return 0;
}

int unsetenv(const char *name)
{
    own_environ();
    size_t n = strlen(name);
    for (char **e = environ; *e; e++) {
        if (!strncmp(*e, name, n) && (*e)[n] == '=') {
            char **d = e;
            do
                d[0] = d[1];
            while (*d++);
            return 0;
        }
    }
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (getenv(name) && !overwrite)
        return 0;
    unsetenv(name);
    int n = 0;
    while (environ[n])
        n++;
    if (n >= 63) {
        errno = ENOMEM;
        return -1;
    }
    char *s = malloc(strlen(name) + strlen(value) + 2);
    snprintf(s, strlen(name) + strlen(value) + 2, "%s=%s", name, value);
    environ[n] = s;
    environ[n + 1] = 0;
    return 0;
}
pid_t getpid(void) { return SC0(SYS_getpid); }
pid_t getppid(void) { return SC0(SYS_getppid); }
int sched_yield(void) { return ret(SC0(SYS_sched_yield)); }
int kill(pid_t pid, int sig) { return ret(SC2(SYS_kill, pid, sig)); }
unsigned sleep_ms(unsigned long ms) { return ret(SC1(SYS_sleep_ms, ms)); }
unsigned long uptime_ms(void) { return SC0(SYS_uptime_ms); }
pid_t waitpid(pid_t pid, int *st, int opt) { return ret(__syscall(SYS_wait4, pid, (long)st, opt, 0, 0, 0)); }
pid_t wait(int *st) { return waitpid(-1, st, 0); }
int munmap(void *a, size_t n) { return ret(SC2(SYS_munmap, a, n)); }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    long r = __syscall(SYS_mmap, (long)addr, len, prot, flags, fd, off);
    return ret(r) == -1 ? MAP_FAILED : (void *)r;
}

void _exit(int status)
{
    SC1(SYS_exit, status);
    for (;;)
        ;
}

static char *cur_brk;

int brk(void *addr)
{
    char *r = (char *)SC1(SYS_brk, addr);
    cur_brk = r;
    if (r != addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void *sbrk(long inc)
{
    if (!cur_brk)
        cur_brk = (char *)SC1(SYS_brk, 0);
    char *old = cur_brk;
    if (inc && brk(old + inc) < 0)
        return (void *)-1;
    return old;
}

void dbg_mark(const char *m) { SC2(SYS_debug, DBG_MARK, m); }
int dbg_profile(int op, long arg) { return (int)ret(SC3(SYS_debug, DBG_PROFILE, op, arg)); }
void dbg_break(void) { SC1(SYS_debug, DBG_BREAK); }
void dbg_strace(int mode) { SC2(SYS_debug, DBG_STRACE, mode); }
void dbg_ps(void) { SC1(SYS_debug, DBG_PS); }
void dbg_files(void) { SC1(SYS_debug, DBG_FILES); }
int dbg_fscrash(const char *point) { return ret(SC2(SYS_debug, DBG_FSCRASH, point)); }

/* ---- files ---- */


int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return ret(SC4(SYS_openat, AT_FDCWD, path, flags, mode));
}

int creat(const char *path, int mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
int close(int fd) { return ret(SC1(SYS_close, fd)); }
long lseek(int fd, long off, int whence) { return ret(SC3(SYS_lseek, fd, off, whence)); }
int stat(const char *path, struct stat *st) { return ret(SC4(SYS_fstatat, AT_FDCWD, path, st, 0)); }
int fstat(int fd, struct stat *st) { return ret(SC2(SYS_fstat, fd, st)); }
int mkdir(const char *path, int mode) { return ret(SC3(SYS_mkdirat, AT_FDCWD, path, mode)); }
int unlink(const char *path) { return ret(SC3(SYS_unlinkat, AT_FDCWD, path, 0)); }
int rmdir(const char *path) { return ret(SC3(SYS_unlinkat, AT_FDCWD, path, AT_REMOVEDIR)); }
int link(const char *a, const char *b) { return ret(SC5(SYS_linkat, AT_FDCWD, a, AT_FDCWD, b, 0)); }
int rename(const char *a, const char *b) { return ret(SC4(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b)); }
int chdir(const char *path) { return ret(SC1(SYS_chdir, path)); }
int dup(int fd) { return ret(SC1(SYS_dup, fd)); }
int dup2(int a, int b) { return a == b ? b : ret(SC3(SYS_dup3, a, b, 0)); }
void sync(void) { SC0(SYS_sync); }

char *getcwd(char *buf, size_t size)
{
    return ret(SC2(SYS_getcwd, buf, size)) < 0 ? 0 : buf;
}

struct DIR {
    int fd;
    int pos, n;
    struct dirent ents[16];
};

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return 0;
    DIR *d = malloc(sizeof(*d));
    if (!d) {
        close(fd);
        return 0;
    }
    d->fd = fd;
    d->pos = d->n = 0;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->pos == d->n) {
        long r = ret(SC3(SYS_getdents64, d->fd, d->ents, sizeof(d->ents)));
        if (r <= 0)
            return 0;
        d->n = (int)(r / sizeof(struct dirent));
        d->pos = 0;
    }
    return &d->ents[d->pos++];
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}
void dbg_vm(void) { SC1(SYS_debug, DBG_VM); }

/* ---- signals and job control ---- */

void __restore_rt(void);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct sigaction k;
    if (act) {
        k = *act;
        k.sa_restorer = __restore_rt;
    }
    return ret(SC3(SYS_rt_sigaction, sig, act ? &k : 0, old));
}

sighandler_t signal(int sig, sighandler_t h)
{
    struct sigaction act = { .sa_handler = h }, old;
    return sigaction(sig, &act, &old) < 0 ? SIG_ERR : old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return ret(SC3(SYS_rt_sigprocmask, how, set, old));
}

int sigsuspend(const sigset_t *mask) { return ret(SC1(SYS_rt_sigsuspend, mask)); }

int pause(void)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, 0, &cur);
    return sigsuspend(&cur);
}

int raise(int sig) { return kill(getpid(), sig); }

const char *strsignal(int sig)
{
    switch (sig) {
#define X(name, num, act, desc) case num: return desc;
        SIGNALS(X)
#undef X
    }
    return "Unknown signal";
}

int setpgid(pid_t pid, pid_t pgid) { return ret(SC2(SYS_setpgid, pid, pgid)); }
pid_t getpgid(pid_t pid) { return ret(SC1(SYS_getpgid, pid)); }
pid_t getpgrp(void) { return getpgid(0); }
pid_t setsid(void) { return ret(SC0(SYS_setsid)); }
pid_t getsid(pid_t pid) { return ret(SC1(SYS_getsid, pid)); }

const char *strerror(int err)
{
    switch (err) {
#define X(name, num, desc) case num: return desc;
        ERRNOS(X)
#undef X
    }
    return "Unknown error";
}

/* ---- stdio ---- */

static char outbuf[512];
static size_t outlen;

void fflush_stdout(void)
{
    if (outlen)
        write(1, outbuf, outlen);
    outlen = 0;
}

static void out_char(char c, void *arg)
{
    (void)arg;
    outbuf[outlen++] = c;
    if (c == '\n' || outlen == sizeof(outbuf))
        fflush_stdout();
}

int vprintf(const char *fmt, va_list ap)
{
    return kvcbprintf(out_char, NULL, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fprintf_fd(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fd == 1)
        fflush_stdout();
    write(fd, buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

int putchar(int c)
{
    out_char((char)c, NULL);
    return c;
}

int puts(const char *s)
{
    while (*s)
        out_char(*s++, NULL);
    out_char('\n', NULL);
    return 0;
}

int getchar(void)
{
    char c;
    fflush_stdout();
    return read(0, &c, 1) == 1 ? (unsigned char)c : EOF;
}

/* ---- string extras ---- */

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

int atoi(const char *s)
{
    return (int)strtol(s, 0, 10);
}

long strtol(const char *s, char **end, int base)
{
    int neg = 0;
    while (kisspace(*s))
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    const char *e;
    long v = (long)kstrtoul(s, &e, base);
    if (end)
        *end = (char *)e;
    return neg ? -v : v;
}

/* ---- malloc ----
 * First-fit over an address-ordered free list on top of sbrk. free() merges
 * a block with free neighbours, so memory freed in a long-running program
 * (the shell) can be reused for bigger requests instead of fragmenting.
 * Each block has a header with a magic number; free() checks it (catches
 * frees of bad pointers, double frees and header overwrites) and poisons the
 * memory with 0x6b like the kernel. */

#define MAGIC_USED 0xa110c8edUL
#define MAGIC_FREE 0xf4eeb10cUL

struct block {
    unsigned long magic;
    size_t size;            /* usable bytes */
    struct block *next;     /* free list, in address order */
    unsigned long pad;
};

static struct block *free_list;
static size_t heap_used, heap_total;

static void malloc_die(const char *what, void *p)
{
    fflush_stdout();
    fprintf_fd(2, "malloc: %s (%p) -- heap corruption or bad pointer\n", what, p);
    dbg_mark("malloc abort");
    _exit(134);
}

static char *block_end(struct block *b)
{
    return (char *)(b + 1) + b->size;
}

/* Put a free block in the list (address order), merging with neighbours. */
static void free_insert(struct block *b)
{
    struct block *prev = 0, *cur = free_list;
    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    b->magic = MAGIC_FREE;
    b->next = cur;
    if (prev)
        prev->next = b;
    else
        free_list = b;
    if (cur && block_end(b) == (char *)cur) {
        b->size += sizeof(struct block) + cur->size;
        b->next = cur->next;
        cur->magic = 0;
    }
    if (prev && block_end(prev) == (char *)b) {
        prev->size += sizeof(struct block) + b->size;
        prev->next = b->next;
        b->magic = 0;
    }
}

void *malloc(size_t n)
{
    if (n > ((size_t)1 << 40))
        return 0;
    n = (n + 15) & ~15UL;
    struct block **pp = &free_list;
    for (struct block *b = free_list; b; pp = &b->next, b = b->next) {
        if (b->magic != MAGIC_FREE)
            malloc_die("free list corrupted", b);
        if (b->size >= n) {
            if (b->size - n >= sizeof(struct block) + 16) {    /* split: the rest keeps b's place */
                struct block *rest = (struct block *)((char *)(b + 1) + n);
                rest->magic = MAGIC_FREE;
                rest->size = b->size - n - sizeof(struct block);
                rest->next = b->next;
                *pp = rest;
                b->size = n;
            } else {
                *pp = b->next;
            }
            b->magic = MAGIC_USED;
            heap_used += b->size;
            return b + 1;
        }
    }
    /* Grow the heap by at least 4 KiB; the surplus becomes a free block. */
    size_t need = n + sizeof(struct block);
    size_t chunk = need < 4096 ? 4096 : need;
    struct block *b = sbrk((long)chunk);
    if (b == (void *)-1)
        return 0;
    heap_total += chunk;
    b->magic = MAGIC_USED;
    b->size = n;
    if (chunk - need >= sizeof(struct block) + 16) {
        struct block *rest = (struct block *)((char *)(b + 1) + n);
        rest->size = chunk - need - sizeof(struct block);
        free_insert(rest);
    } else {
        b->size = chunk - sizeof(struct block);
    }
    heap_used += b->size;
    return b + 1;
}

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    if (b->magic == MAGIC_FREE)
        malloc_die("double free", p);
    if (b->magic != MAGIC_USED)
        malloc_die("free of a pointer malloc did not return", p);
    memset(p, 0x6b, b->size);
    heap_used -= b->size;
    free_insert(b);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > ((size_t)-1) / size)
        return 0;
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    struct block *b = (struct block *)p - 1;
    if (b->magic != MAGIC_USED)
        malloc_die("realloc of a bad pointer", p);
    if (b->size >= n)
        return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, b->size);
        free(p);
    }
    return q;
}

void malloc_stats(void)
{
    printf("malloc: %lu bytes in use, %lu bytes of heap\n",
           (unsigned long)heap_used, (unsigned long)heap_total);
}

/* ---- startup ---- */

int main(int argc, char **argv);

void exit(int status)
{
    fflush_stdout();
    _exit(status);
}

void __libc_start(int argc, char **argv, char **envp)
{
    environ = envp;
    exit(main(argc, argv));
}
