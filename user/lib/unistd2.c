/* unistd odds and ends. */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
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
    (void)fd;
    (void)length;
    errno = ENOSYS;                     /* efs can't shorten files yet */
    return -1;
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
    (void)target;
    (void)path;
    errno = ENOSYS;                     /* efs has no symbolic links */
    return -1;
}

long readlink(const char *path, char *buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;                     /* nothing is a symbolic link */
    return -1;
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

