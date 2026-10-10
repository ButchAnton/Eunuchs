/* Credentials, file modes and ownership, time. */
#include <fs.h>
#include <kstring.h>
#include <platform.h>
#include <proc.h>
#include <riscv.h>
#include <uapi/errno.h>

u64 rtc_time_ns(void);

long do_getuid(const u64 *a) { (void)a; return current_proc()->uid; }
long do_geteuid(const u64 *a) { (void)a; return current_proc()->euid; }
long do_getgid(const u64 *a) { (void)a; return current_proc()->gid; }
long do_getegid(const u64 *a) { (void)a; return current_proc()->egid; }

long do_setuid(const u64 *a)
{
    struct proc *p = current_proc();
    u16 uid = (u16)a[0];
    if (p->euid == 0)
        p->uid = p->euid = p->suid = uid;
    else if (uid == p->uid || uid == p->suid)
        p->euid = uid;
    else
        return -EPERM;
    return 0;
}

long do_setgid(const u64 *a)
{
    struct proc *p = current_proc();
    u16 gid = (u16)a[0];
    if (p->euid == 0)
        p->gid = p->egid = p->sgid = gid;
    else if (gid == p->gid || gid == p->sgid)
        p->egid = gid;
    else
        return -EPERM;
    return 0;
}

long do_umask(const u64 *a)
{
    struct proc *p = current_proc();
    u16 old = p->umask;
    p->umask = a[0] & 0777;
    return old;
}

static long get_inode(uintptr_t upath, struct inode **ipp)
{
    char path[PATH_MAX];
    long r = strncpy_from_user(path, upath, sizeof(path));
    if (r < 0)
        return r;
    int err;
    *ipp = namei(path, &err);
    return *ipp ? 0 : err;
}

static long chmod_inode(struct inode *ip, u64 mode)
{
    struct proc *p = current_proc();
    ilock(ip);
    long r = 0;
    if (p->euid != 0 && p->euid != ip->uid) {
        r = -EPERM;
    } else if (!ip->sb->ops->write_inode) {
        r = -EPERM;
    } else {
        fs_begin_op(ip->sb);
        ip->mode = mode & 07777;
        ip->sb->ops->write_inode(ip);
        fs_end_op(ip->sb);
    }
    iunlockput(ip);
    return r;
}

long do_fchmodat(const u64 *a)
{
    struct inode *ip;
    if ((long)a[0] != AT_FDCWD)
        return -EINVAL;
    long r = get_inode(a[1], &ip);
    return r ? r : chmod_inode(ip, a[2]);
}

long do_fchmod(const u64 *a)
{
    struct proc *p = current_proc();
    if (a[0] >= NOFILE || !p->files[a[0]] || !p->files[a[0]]->ip)
        return -EBADF;
    return chmod_inode(idup(p->files[a[0]]->ip), a[1]);
}

long do_fchownat(const u64 *a)
{
    struct inode *ip;
    if ((long)a[0] != AT_FDCWD)
        return -EINVAL;
    if (current_proc()->euid != 0)
        return -EPERM;                  /* only root gives files away */
    long r = get_inode(a[1], &ip);
    if (r)
        return r;
    ilock(ip);
    if (ip->sb->ops->write_inode) {
        fs_begin_op(ip->sb);
        if ((long)a[2] != -1)
            ip->uid = (u16)a[2];
        if ((long)a[3] != -1)
            ip->gid = (u16)a[3];
        ip->sb->ops->write_inode(ip);
        fs_end_op(ip->sb);
    } else {
        r = -EPERM;
    }
    iunlockput(ip);
    return r;
}

long do_faccessat(const u64 *a)
{
    struct inode *ip;
    if ((long)a[0] != AT_FDCWD)
        return -EINVAL;
    long r = get_inode(a[1], &ip);
    if (r)
        return r;
    ilock(ip);
    r = (a[2] & 7) ? vfs_permission(ip, (int)(a[2] & 7)) : 0;
    iunlockput(ip);
    return r;
}

/* ---- time ---- */

long do_clock_gettime(const u64 *a)
{
    struct timespec ts;
    u64 tb = platform.timebase;
    if (a[0] == CLOCK_REALTIME) {
        u64 ns = rtc_time_ns();
        ts.tv_sec = ns / 1000000000;
        ts.tv_nsec = ns % 1000000000;
    } else if (a[0] == CLOCK_MONOTONIC) {
        u64 t = rdtime();
        ts.tv_sec = t / tb;
        ts.tv_nsec = (t % tb) * 1000000000 / tb;
    } else {
        return -EINVAL;
    }
    return copy_to_user(a[1], &ts, sizeof(ts));
}

long do_gettimeofday(const u64 *a)
{
    u64 ns = rtc_time_ns();
    struct timeval tv = { ns / 1000000000, (ns % 1000000000) / 1000 };
    return a[0] ? copy_to_user(a[0], &tv, sizeof(tv)) : 0;
}

long do_nanosleep(const u64 *a)
{
    struct timespec req;
    if (copy_from_user(&req, a[0], sizeof(req)))
        return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000)
        return -EINVAL;
    struct proc *p = current_proc();
    u64 tb = platform.timebase;
    u64 now = rdtime();
    /*
     * A restarted call (after a stop/continue) sleeps only until the original
     * deadline, like Linux's restart_block.
     */
    u64 deadline = p->nanosleep_restarting ? p->nanosleep_deadline
                 : now + (u64)req.tv_sec * tb + (u64)req.tv_nsec * tb / 1000000000;
    p->nanosleep_restarting = false;
    if (deadline > now)
        sleep_ms(((deadline - now) * 1000 + tb - 1) / tb);
    if (!signal_pending(p))
        return 0;
    if (a[1]) {
        u64 left = deadline > rdtime() ? (deadline - rdtime()) * 1000000000 / tb : 0;
        struct timespec rem = { left / 1000000000, left % 1000000000 };
        copy_to_user(a[1], &rem, sizeof(rem));
    }
    p->nanosleep_deadline = deadline;
    return -ERESTARTSYS;                /* EINTR if a handler runs, else resume */
}
