/*
 * Open files, per-process descriptor tables, and the file system calls.
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <debug.h>
#include <fs.h>
#include <fmt.h>
#include <kstring.h>
#include <riscv.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <uapi/errno.h>
#include <uapi/syscall.h>

#define WRITE_CHUNK (4 * EFS_BSIZE)     /* per transaction (see log.c MAXOPBLOCKS) */

/*
 * file_lock: file reference counts. Also lets other processes look at a
 * process's descriptor table (/proc/<pid>/fds): every path that drops a
 * table entry clears the slot before file_close() takes file_lock, so a
 * reader holding file_lock never sees a slot whose file is being freed.
 */
static spinlock_t file_lock = SPINLOCK_INIT("file");

/* ---- files ---- */

long file_read(struct file *f, void *buf, size_t n, bool user)
{
    if (!f->readable)
        return -EBADF;
    if (f->cops && f->cops->read)
        return f->cops->read(f, buf, n, user);
    struct inode *ip = f->ip;
    if (ip->type == I_DIR)
        return -EISDIR;
    ilock(ip);
    long r = ip->sb->ops->read(ip, buf, f->off, n, user);
    if (r > 0)
        f->off += r;
    iunlock(ip);
    return r;
}

long file_write(struct file *f, const void *buf, size_t n, bool user)
{
    if (!f->writable)
        return -EBADF;
    if (f->cops && f->cops->write)
        return f->cops->write(f, buf, n, user);
    struct inode *ip = f->ip;
    if (!ip->sb->ops->write)
        return -EPERM;
    size_t done = 0;
    while (done < n) {
        size_t chunk = MIN(n - done, (size_t)WRITE_CHUNK);
        fs_begin_op(ip->sb);
        ilock(ip);
        if (f->flags & O_APPEND)
            f->off = ip->size;
        long r = ip->sb->ops->write(ip, (const u8 *)buf + done, f->off, chunk, user);
        if (r > 0)
            f->off += r;
        iunlock(ip);
        fs_end_op(ip->sb);
        if (r < 0)
            return done ? (long)done : r;
        done += r;
        if ((size_t)r < chunk)
            break;
    }
    return done;
}

struct file *file_dup(struct file *f)
{
    unsigned long flags = spin_lock_irqsave(&file_lock);
    f->refcnt++;
    spin_unlock_irqrestore(&file_lock, flags);
    return f;
}

void file_close(struct file *f)
{
    unsigned long flags = spin_lock_irqsave(&file_lock);
    if (f->refcnt < 1) {
        spin_unlock_irqrestore(&file_lock, flags);
        panic("file_close of '%s' with refcnt %d (double close?)", f->path, f->refcnt);
    }
    bool last = --f->refcnt == 0;
    spin_unlock_irqrestore(&file_lock, flags);
    if (!last)
        return;
    if (f->cops && f->cops->close)
        f->cops->close(f);
    iput(f->ip);
    kfree(f);
}

/* ---- descriptor tables ---- */

static int fd_install_from(struct proc *p, struct file *f, int start, u8 flags)
{
    for (int fd = start; fd < NOFILE; fd++) {
        if (!p->files[fd]) {
            p->files[fd] = f;
            p->fd_flags[fd] = flags;
            return fd;
        }
    }
    return -EMFILE;
}

int fd_install(struct proc *p, struct file *f)
{
    return fd_install_from(p, f, 0, 0);
}

/* The calling process's file for fd (its own table: no reference needed). */
struct file *fd_get(long fd)
{
    struct proc *p = current_proc();
    return fd >= 0 && fd < NOFILE ? p->files[fd] : NULL;
}

void files_fork(struct proc *child, struct proc *parent)
{
    for (int fd = 0; fd < NOFILE; fd++) {
        if (parent->files[fd])
            child->files[fd] = file_dup(parent->files[fd]);
        child->fd_flags[fd] = parent->fd_flags[fd];
    }
    child->cwd = parent->cwd ? idup(parent->cwd) : NULL;
}

void files_exec(struct proc *p)
{
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->files[fd] && (p->fd_flags[fd] & FD_CLOEXEC)) {
            struct file *f = p->files[fd];
            p->files[fd] = NULL;
            file_close(f);
        }
        p->fd_flags[fd] = 0;
    }
}

void files_close_all(struct proc *p)
{
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->files[fd]) {
            struct file *f = p->files[fd];
            p->files[fd] = NULL;
            file_close(f);
        }
    }
    unsigned long flags = spin_lock_irqsave(&proc_lock);   /* procfs reads p->cwd */
    struct inode *cwd = p->cwd;
    p->cwd = NULL;
    spin_unlock_irqrestore(&proc_lock, flags);
    if (cwd)
        iput(cwd);
}

/* From the monitor (world stopped): no sleeping, so the cwd is an inode number. */
void files_print(print_fn print, struct proc *p)
{
    if (p->cwd)
        print("  cwd inode %u (path: cat /proc/%d/fds)\n", p->cwd->ino, p->pid);
    files_print_fds(print, p);
}

/* Descriptor table of any process (see file_lock). */
void files_print_fds(print_fn print, struct proc *p)
{
    static const char *const types[] = { "free", "dir", "file", "chr" };
    unsigned long flags;
    bool locked = spin_lock_debug(&file_lock, &flags);
    for (int fd = 0; fd < NOFILE; fd++) {
        struct file *f = p->files[fd];
        if (!f)
            continue;
        print("  %2d %c%c%s %-4s off %-8lu refs %d  ino %u  %s\n", fd, f->readable ? 'r' : '-',
              f->writable ? 'w' : '-', p->fd_flags[fd] & FD_CLOEXEC ? "e" : " ",
              f->ip ? types[f->ip->type & 3] : "pipe", (unsigned long)f->off, f->refcnt,
              f->ip ? f->ip->ino : 0, f->path);
    }
    spin_unlock_debug(&file_lock, locked, flags);
}

/* ---- system calls ---- */

static long get_path(char *buf, uintptr_t upath, long dirfd)
{
    long r = strncpy_from_user(buf, upath, PATH_MAX);
    if (r < 0)
        return r;
    if (dirfd != AT_FDCWD && buf[0] != '/')
        return -EINVAL;                 /* only AT_FDCWD-relative paths for now */
    return 0;
}

long do_openat(const u64 *a)
{
    char path[PATH_MAX];
    long r = get_path(path, a[1], (long)a[0]);
    if (r)
        return r;
    int err;
    struct file *f = vfs_open(path, (int)a[2], (int)a[3], &err);
    if (!f)
        return err;
    int fd = fd_install_from(current_proc(), f, 0, (a[2] & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0)
        file_close(f);
    return fd;
}

long do_close(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    current_proc()->files[a[0]] = NULL;
    file_close(f);
    return 0;
}

long do_read(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    if (!user_range_ok(a[1], a[2]))
        return -EFAULT;
    return file_read(f, (void *)a[1], a[2], true);
}

long do_write(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    if (!user_range_ok(a[1], a[2]))
        return -EFAULT;
    return file_write(f, (const void *)a[1], a[2], true);
}

long do_lseek(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    if (!f->ip || (f->cops && !f->priv) || file_is_pipe(f))
        return -ESPIPE;                 /* pipes, devices (procfs snapshots are seekable) */
    i64 off = (i64)a[1], base;
    switch (a[2]) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (i64)f->off; break;
    case SEEK_END: base = f->priv ? (i64)f->priv_len : (i64)f->ip->size; break;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->off = base + off;
    return f->off;
}

long do_fstat(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    struct stat st;
    if (!f)
        return -EBADF;
    if (!f->ip) {                       /* pipe */
        memset(&st, 0, sizeof(st));
        st.st_mode = S_IFIFO | 0600;
        return copy_to_user(a[1], &st, sizeof(st));
    }
    ilock(f->ip);
    vfs_stat(f->ip, &st);
    iunlock(f->ip);
    if (f->priv)
        st.st_size = f->priv_len;
    return copy_to_user(a[1], &st, sizeof(st));
}

long do_fstatat(const u64 *a)
{
    char path[PATH_MAX];
    long r = get_path(path, a[1], (long)a[0]);
    if (r)
        return r;
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err;
    struct stat st;
    ilock(ip);
    vfs_stat(ip, &st);
    iunlockput(ip);
    return copy_to_user(a[2], &st, sizeof(st));
}

long do_getdents64(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    struct inode *dp = f->ip;
    if (!dp || dp->type != I_DIR)
        return -ENOTDIR;
    uintptr_t ubuf = a[1];
    size_t max = a[2] / sizeof(struct dirent), n = 0;
    while (n < max) {
        struct kdirent kd;
        u64 off = f->off;
        ilock(dp);
        int more = dp->sb->ops->readdir(dp, &off, &kd);
        iunlock(dp);
        if (!more)
            break;
        if (kd.type == DT_UNKNOWN) {
            struct inode *ip = iget(dp->sb, kd.ino);
            ilock(ip);
            kd.type = ip->type == I_DIR ? DT_DIR : ip->type == I_CHR ? DT_CHR : DT_REG;
            iunlockput(ip);
        }
        struct dirent ud = { .d_ino = kd.ino, .d_type = kd.type };
        strlcpy(ud.d_name, kd.name, sizeof(ud.d_name));
        if (copy_to_user(ubuf + n * sizeof(ud), &ud, sizeof(ud)))
            return n ? (long)(n * sizeof(ud)) : -EFAULT;
        f->off = off;
        n++;
    }
    return n * sizeof(struct dirent);
}

long do_mkdirat(const u64 *a)
{
    char path[PATH_MAX];
    long r = get_path(path, a[1], (long)a[0]);
    return r ? r : vfs_mkdir(path, (int)a[2]);
}

long do_unlinkat(const u64 *a)
{
    char path[PATH_MAX];
    long r = get_path(path, a[1], (long)a[0]);
    return r ? r : vfs_unlink(path, a[2] & AT_REMOVEDIR);
}

long do_linkat(const u64 *a)
{
    char from[PATH_MAX], to[PATH_MAX];
    long r = get_path(from, a[1], (long)a[0]);
    if (!r)
        r = get_path(to, a[3], (long)a[2]);
    return r ? r : vfs_link(from, to);
}

long do_renameat(const u64 *a)
{
    char from[PATH_MAX], to[PATH_MAX];
    long r = get_path(from, a[1], (long)a[0]);
    if (!r)
        r = get_path(to, a[3], (long)a[2]);
    return r ? r : vfs_rename(from, to);
}

long do_chdir(const u64 *a)
{
    char path[PATH_MAX];
    long r = get_path(path, a[0], AT_FDCWD);
    if (r)
        return r;
    int err;
    struct inode *ip = namei(path, &err);
    if (!ip)
        return err;
    ilock(ip);
    bool dir = ip->type == I_DIR;
    iunlock(ip);
    if (!dir) {
        iput(ip);
        return -ENOTDIR;
    }
    struct proc *p = current_proc();
    unsigned long flags = spin_lock_irqsave(&proc_lock);   /* procfs reads p->cwd */
    struct inode *old = p->cwd;
    p->cwd = ip;
    spin_unlock_irqrestore(&proc_lock, flags);
    iput(old);
    return 0;
}

long do_getcwd(const u64 *a)
{
    char path[PATH_MAX];
    struct proc *p = current_proc();
    int r = vfs_getcwd(p->cwd ? p->cwd : root_inode, path, MIN((size_t)a[1], sizeof(path)));
    if (r)
        return r;
    size_t len = strlen(path) + 1;
    return copy_to_user(a[0], path, len) ? -EFAULT : (long)len;
}

long do_dup(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    int fd = fd_install(current_proc(), file_dup(f));
    if (fd < 0)
        file_close(f);
    return fd;
}

long do_pipe2(const u64 *a)
{
    struct file *rf, *wf;
    struct proc *p = current_proc();
    u8 fl = (a[1] & O_CLOEXEC) ? FD_CLOEXEC : 0;
    int r = pipe_create(&rf, &wf);
    if (r)
        return r;
    int fds[2] = { fd_install_from(p, rf, 0, fl), -1 };
    if (fds[0] >= 0)
        fds[1] = fd_install_from(p, wf, 0, fl);
    if (fds[1] < 0 || copy_to_user(a[0], fds, sizeof(fds))) {
        if (fds[0] >= 0)
            p->files[fds[0]] = NULL;
        if (fds[1] >= 0)
            p->files[fds[1]] = NULL;
        file_close(rf);
        file_close(wf);
        return fds[1] < 0 ? -EMFILE : -EFAULT;
    }
    return 0;
}

long do_fcntl(const u64 *a)
{
    struct proc *p = current_proc();
    long fd = (long)a[0];
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    switch (a[1]) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if ((long)a[2] < 0 || a[2] >= NOFILE)
            return -EINVAL;
        int nfd = fd_install_from(p, file_dup(f), (int)a[2],
                                  a[1] == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0);
        if (nfd < 0)
            file_close(f);
        return nfd;
    }
    case F_GETFD:
        return p->fd_flags[fd];
    case F_SETFD:
        p->fd_flags[fd] = a[2] & FD_CLOEXEC;
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (a[2] & (O_APPEND | O_NONBLOCK));
        return 0;
    }
    return -EINVAL;
}

long do_dup3(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    long nfd = (long)a[1];
    if (!f || nfd < 0 || nfd >= NOFILE)
        return -EBADF;
    if (nfd == (long)a[0])
        return -EINVAL;
    struct proc *p = current_proc();
    struct file *old = p->files[nfd];
    p->files[nfd] = file_dup(f);
    p->fd_flags[nfd] = (a[2] & O_CLOEXEC) ? FD_CLOEXEC : 0;
    if (old)
        file_close(old);
    return nfd;
}

long do_ioctl(const u64 *a)
{
    struct file *f = fd_get((long)a[0]);
    if (!f)
        return -EBADF;
    if (!f->cops || !f->cops->ioctl)
        return -ENOTTY;
    return f->cops->ioctl(f, a[1], a[2]);
}

long do_sync(const u64 *a)
{
    (void)a;
    return 0;                           /* every transaction is already on disk */
}

long do_fsync(const u64 *a)
{
    return fd_get((long)a[0]) ? 0 : -EBADF;
}
