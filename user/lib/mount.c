/* statfs, statvfs, mount, umount */
#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <uapi/syscall.h>

static long ret(long r)
{
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

int statfs(const char *path, struct statfs *buf)
{
    return ret(__syscall(SYS_statfs, (long)path, (long)buf, 0, 0, 0, 0));
}

int fstatfs(int fd, struct statfs *buf)
{
    return ret(__syscall(SYS_fstatfs, fd, (long)buf, 0, 0, 0, 0));
}

static void to_vfs(const struct statfs *s, struct statvfs *v)
{
    memset(v, 0, sizeof(*v));
    v->f_bsize = s->f_bsize;
    v->f_frsize = s->f_frsize ? s->f_frsize : s->f_bsize;
    v->f_blocks = s->f_blocks;
    v->f_bfree = s->f_bfree;
    v->f_bavail = s->f_bavail;
    v->f_files = s->f_files;
    v->f_ffree = v->f_favail = s->f_ffree;
    v->f_fsid = (unsigned)s->f_fsid.val[0];
    v->f_flag = s->f_flags;
    v->f_namemax = s->f_namelen;
}

int statvfs(const char *path, struct statvfs *buf)
{
    struct statfs s;
    if (statfs(path, &s) < 0)
        return -1;
    to_vfs(&s, buf);
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    struct statfs s;
    if (fstatfs(fd, &s) < 0)
        return -1;
    to_vfs(&s, buf);
    return 0;
}

int mount(const char *source, const char *target, const char *fstype, unsigned long flags,
          const void *data)
{
    return ret(__syscall(SYS_mount, (long)source, (long)target, (long)fstype, flags, (long)data, 0));
}

int umount2(const char *target, int flags)
{
    return ret(__syscall(SYS_umount2, (long)target, flags, 0, 0, 0, 0));
}

int umount(const char *target)
{
    return umount2(target, 0);
}
