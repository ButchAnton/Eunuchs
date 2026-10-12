/* unistd odds and ends. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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

