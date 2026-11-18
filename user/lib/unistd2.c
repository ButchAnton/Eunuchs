/* unistd odds and ends. */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <uapi/syscall.h>

int fsync(int fd)
{
    long r = __syscall(SYS_fsync, fd, 0, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int ftruncate(int fd, long length)
{
    long r = __syscall(SYS_ftruncate, fd, length, 0, 0, 0, 0);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return 0;
}

int truncate(const char *path, long length)
{
    long r = __syscall(SYS_truncate, (long)path, length, 0, 0, 0, 0);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return 0;
}

int usleep(unsigned long usec)
{
    struct timespec ts = { (long)(usec / 1000000), (long)(usec % 1000000) * 1000 };
    return nanosleep(&ts, 0);
}

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_CLK_TCK: return 100;
    }
    errno = EINVAL;
    return -1;
}

int gethostname(char *name, size_t len)
{
    int fd = open("/etc/hostname", O_RDONLY);
    long n = fd >= 0 ? read(fd, name, len - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0) {
        strlcpy(name, "eunuchs", len);
        return 0;
    }
    name[n] = 0;
    char *nl = strchr(name, '\n');
    if (nl)
        *nl = 0;
    return 0;
}

char *ttyname(int fd)
{
    static char name[] = "/dev/console";
    return isatty(fd) ? name : 0;
}

int symlink(const char *target, const char *path)
{
    long r = __syscall(SYS_symlinkat, (long)target, AT_FDCWD, (long)path, 0, 0, 0);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return 0;
}

long readlink(const char *path, char *buf, size_t size)
{
    long r = __syscall(SYS_readlinkat, AT_FDCWD, (long)path, (long)buf, size, 0, 0);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

/* ---- execl and friends: the argument list becomes a vector ---- */

#define EXECL_MAX 256

static int collect_args(const char *arg, va_list ap, char **argv)
{
    int n = 0;
    argv[n++] = (char *)arg;
    while (arg && n < EXECL_MAX) {
        arg = va_arg(ap, const char *);
        argv[n++] = (char *)arg;
    }
    if (arg) {
        errno = E2BIG;
        return -1;
    }
    return n;
}

int execl(const char *path, const char *arg, ...)
{
    char *argv[EXECL_MAX + 1];
    va_list ap;
    va_start(ap, arg);
    int n = collect_args(arg, ap, argv);
    va_end(ap);
    return n < 0 ? -1 : execv(path, argv);
}

int execlp(const char *file, const char *arg, ...)
{
    char *argv[EXECL_MAX + 1];
    va_list ap;
    va_start(ap, arg);
    int n = collect_args(arg, ap, argv);
    va_end(ap);
    return n < 0 ? -1 : execvp(file, argv);
}

int execle(const char *path, const char *arg, ...)
{
    char *argv[EXECL_MAX + 1];
    va_list ap;
    va_start(ap, arg);
    int n = collect_args(arg, ap, argv);
    char *const *envp = n < 0 ? 0 : va_arg(ap, char *const *);
    va_end(ap);
    return n < 0 ? -1 : execve(path, argv, envp);
}

/* ---- file times ---- */

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    long r = __syscall(SYS_utimensat, dirfd, (long)path, (long)times, flags, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int futimens(int fd, const struct timespec times[2])
{
    return utimensat(fd, 0, times, 0);
}

/* ---- ptrace ---- */

long ptrace(int request, pid_t pid, void *addr, void *data)
{
    long word = 0;
    int peek = request == PTRACE_PEEKTEXT || request == PTRACE_PEEKDATA;
    long r = __syscall(SYS_ptrace, request, pid, (long)addr, peek ? (long)&word : (long)data, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return peek ? word : r;
}

int getloadavg(double loadavg[], int nelem)
{
    char buf[128];
    int fd = open("/proc/loadavg", O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    double v[3];
    int got = sscanf(buf, "%lf %lf %lf", &v[0], &v[1], &v[2]);
    if (got < 3)
        return -1;
    if (nelem > 3)
        nelem = 3;
    for (int i = 0; i < nelem; i++)
        loadavg[i] = v[i];
    return nelem < 0 ? -1 : nelem;
}
