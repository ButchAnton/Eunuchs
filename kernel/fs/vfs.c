/*
 * VFS: inode cache, mounts, path lookup and namespace operations, generic
 * over struct fs_ops. See fs.h for the locking order.
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <debug.h>
#include <fmt.h>
#include <fs.h>
#include <riscv.h>
#include <platform.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <trace.h>
#include <uapi/errno.h>
#include <virtio.h>

struct inode *root_inode;
static LIST_HEAD(icache);
static spinlock_t icache_lock = SPINLOCK_INIT("icache");
static LIST_HEAD(mounts);

u64 rtc_time_ns(void);

void fs_error(struct super_block *sb, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sb->errors++;
    klog(LOG_ERR, SUBSYS_FS, "%s filesystem error on %s: %s\n", sb->type->name, sb->path, msg);
    backtrace_here(panic_print);
}

/* ---- inode cache ---- */

struct inode *iget(struct super_block *sb, u32 ino)
{
    struct inode *ip;
    unsigned long flags = spin_lock_irqsave(&icache_lock);
    list_for_each_entry(ip, &icache, cache_node) {
        if (ip->sb == sb && ip->ino == ino) {
            ip->refcnt++;
            spin_unlock_irqrestore(&icache_lock, flags);
            return ip;
        }
    }
    spin_unlock_irqrestore(&icache_lock, flags);

    struct inode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    n->sb = sb;
    n->ino = ino;
    n->refcnt = 1;
    mutex_init(&n->lock, "inode");

    flags = spin_lock_irqsave(&icache_lock);
    list_for_each_entry(ip, &icache, cache_node) {      /* lost a race? */
        if (ip->sb == sb && ip->ino == ino) {
            ip->refcnt++;
            spin_unlock_irqrestore(&icache_lock, flags);
            kfree(n);
            return ip;
        }
    }
    list_add(&n->cache_node, &icache);
    spin_unlock_irqrestore(&icache_lock, flags);
    return n;
}

struct inode *idup(struct inode *ip)
{
    unsigned long flags = spin_lock_irqsave(&icache_lock);
    ip->refcnt++;
    spin_unlock_irqrestore(&icache_lock, flags);
    return ip;
}

void ilock(struct inode *ip)
{
    if (!ip || ip->refcnt < 1)
        panic("ilock of %s inode %p", ip ? "unreferenced" : "NULL", ip);
    mutex_lock(&ip->lock);
    if (!ip->valid) {
        if (ip->sb->ops->read_inode(ip) == 0)
            ip->valid = true;
    }
}

/* ilock without waiting (writeback from reclaim); false if it is held. */
bool ilock_try(struct inode *ip)
{
    if (!mutex_trylock(&ip->lock))
        return false;
    if (!ip->valid && ip->sb->ops->read_inode(ip) == 0)
        ip->valid = true;
    return true;
}

void iunlock(struct inode *ip)
{
    mutex_unlock(&ip->lock);
}

void iput(struct inode *ip)
{
    if (!ip)
        return;
    unsigned long flags = spin_lock_irqsave(&icache_lock);
    if (ip->refcnt < 1) {
        spin_unlock_irqrestore(&icache_lock, flags);
        panic("iput of inode %u with refcnt %d (double iput?)", ip->ino, ip->refcnt);
    }
    if (ip->refcnt == 1 && ip->valid && ip->nlink == 0 && ip->type != I_FREE &&
        ip->sb->ops->free_inode) {
        /* Last reference to an unlinked file: free it on disk. */
        spin_unlock_irqrestore(&icache_lock, flags);
        fs_begin_op(ip->sb);
        mutex_lock(&ip->lock);
        pc_drop_inode(ip->sb, ip->ino);         /* its cached pages, dirty or not */
        ip->sb->ops->free_inode(ip);
        mutex_unlock(&ip->lock);
        fs_end_op(ip->sb);
        flags = spin_lock_irqsave(&icache_lock);
    }
    if (--ip->refcnt == 0) {
        list_del(&ip->cache_node);
        spin_unlock_irqrestore(&icache_lock, flags);
        kfree(ip);
        return;
    }
    spin_unlock_irqrestore(&icache_lock, flags);
}

void iunlockput(struct inode *ip)
{
    iunlock(ip);
    iput(ip);
}

int icache_busy(void)
{
    int n = 0;
    struct inode *ip;
    unsigned long flags = spin_lock_irqsave(&icache_lock);
    list_for_each_entry(ip, &icache, cache_node)
        n++;
    spin_unlock_irqrestore(&icache_lock, flags);
    return n;
}

void icache_print(print_fn print)
{
    struct inode *ip;
    static const char *const types[] = { "free", "dir", "file", "chr" };
    unsigned long flags;
    bool locked = spin_lock_debug(&icache_lock, &flags);
    print("  %-6s %7s %4s %-4s %5s %10s  %s\n", "fs", "ino", "refs", "type", "nlink", "size",
          "lock");
    list_for_each_entry(ip, &icache, cache_node) {
        struct thread *owner = ip->lock.owner;
        print("  %-6s %7u %4d %-4s %5u %10lu  %s%s\n", ip->sb->type->name, ip->ino, ip->refcnt,
              ip->valid ? types[ip->type & 3] : "?", ip->nlink, (unsigned long)ip->size,
              owner ? "held by " : "", owner ? owner->name : "");
    }
    spin_unlock_debug(&icache_lock, locked, flags);
}

void fs_begin_op(struct super_block *sb)
{
    if (sb->ops->begin_op)
        sb->ops->begin_op(sb);
}

void fs_end_op(struct super_block *sb)
{
    if (sb->ops->end_op)
        sb->ops->end_op(sb);
}

/* ---- mounts ---- */

int vfs_mount(const struct fs_type *type, const char *path)
{
    return vfs_mount_disk(type, path, 0);
}

int vfs_mount_disk(const struct fs_type *type, const char *path, int disk)
{
    static u32 next_dev = 1;
    struct super_block *sb = kzalloc(sizeof(*sb));
    if (!sb)
        return -ENOMEM;
    sb->dev = __atomic_fetch_add(&next_dev, 1, __ATOMIC_RELAXED);
    sb->disk = disk;
    sb->type = type;
    sb->ops = type->ops;
    mutex_init(&sb->ns_lock, "namespace");
    strlcpy(sb->path, path, sizeof(sb->path));

    struct inode *covered = NULL;
    if (strcmp(path, "/")) {
        int err;
        covered = namei(path, &err);
        if (!covered) {
            pr_err("mount %s on %s: mount point: %s\n", type->name, path, errno_name(-err));
            kfree(sb);
            return err;
        }
        ilock(covered);
        bool dir = covered->type == I_DIR;
        iunlock(covered);
        if (!dir || covered->mounted) {
            iput(covered);
            kfree(sb);
            return dir ? -EBUSY : -ENOTDIR;
        }
    }
    int r = type->mount(sb);
    if (r) {
        iput(covered);
        kfree(sb);
        return r;
    }
    sb->covered = covered;
    if (covered)
        covered->mounted = sb;          /* the mount keeps its reference */
    else
        root_inode = sb->root;
    list_add_tail(&sb->node, &mounts);
    if (type == &efs_type)
        pr_info("mounted %s (%s) on %s\n", type->name, blk_name(disk), path);
    else
        pr_info("mounted %s on %s\n", type->name, path);
    return 0;
}

void vfs_sync(struct super_block *sb)
{
    if (sb->ops->sync)
        sb->ops->sync(sb);
}

/* Every mounted filesystem (mounts are only ever added, at boot), with the
   file pages written through shared mappings first. */
void vfs_sync_all(void)
{
    struct super_block *sb;
    pc_writeback(NULL, 0, 0, ~0ULL, true);
    list_for_each_entry(sb, &mounts, node)
        vfs_sync(sb);
}

void vfs_mounts_print(print_fn print)
{
    struct super_block *sb;
    list_for_each_entry(sb, &mounts, node) {
        print("%s on %s (%lu errors)\n", sb->type->name, sb->path, (unsigned long)sb->errors);
        if (sb->ops->stat_fs)
            sb->ops->stat_fs(sb, print);
    }
}

/* ---- permissions ---- */

static void creds(u16 *uid, u16 *gid)
{
    struct proc *p = current_proc();
    *uid = p ? p->euid : 0;             /* kernel threads act as root */
    *gid = p ? p->egid : 0;
}

int vfs_permission(struct inode *ip, int mask)
{
    u16 uid, gid;
    creds(&uid, &gid);
    if (uid == 0) {
        /* root may do anything, but only executes files with some x bit */
        if ((mask & X_OK) && ip->type == I_FILE && !(ip->mode & 0111))
            return -EACCES;
        return 0;
    }
    int bits = uid == ip->uid ? (ip->mode >> 6) : gid == ip->gid ? (ip->mode >> 3) : ip->mode;
    return (bits & mask & 7) == (mask & 7) ? 0 : -EACCES;
}

/* ---- path lookup ---- */

static const char *next_component(const char *path, char *name, int *err)
{
    while (*path == '/')
        path++;
    if (!*path)
        return NULL;
    const char *start = path;
    while (*path && *path != '/')
        path++;
    size_t len = path - start;
    if (len > EFS_NAMELEN) {
        *err = -ENAMETOOLONG;
        return NULL;
    }
    memcpy(name, start, len);
    name[len] = '\0';
    return path;
}

static struct inode *start_dir(const char *path)
{
    struct proc *p = current_proc();
    if (path[0] != '/' && p && p->cwd)
        return idup(p->cwd);
    return idup(root_inode);
}

/*
 * Walk `path`. With `parent`, stop at the last component's directory and
 * copy the last component into `name`.
 */
static struct inode *walk(const char *path, bool parent, char *name, int *err)
{
    char comp[EFS_NAMELEN + 1];
    int e = 0;

    if (!root_inode) {
        *err = -ENOENT;
        return NULL;
    }
    if (!*path) {
        *err = -ENOENT;
        return NULL;
    }
    struct inode *ip = start_dir(path);
    const char *p = path;

    while ((p = next_component(p, comp, &e)) != NULL) {
        ilock(ip);
        if (ip->type != I_DIR) {
            iunlockput(ip);
            *err = -ENOTDIR;
            return NULL;
        }
        if (vfs_permission(ip, X_OK)) {          /* search permission */
            iunlockput(ip);
            *err = -EACCES;
            return NULL;
        }
        if (parent && !next_component(p, name, &e) && !e) {
            strlcpy(name, comp, EFS_NAMELEN + 1);
            iunlock(ip);
            return ip;
        }
        if (!strcmp(comp, ".")) {
            iunlock(ip);
            continue;
        }
        if (!strcmp(comp, "..") && ip == ip->sb->root) {
            /* Leave a mounted filesystem through its mount point. */
            struct inode *covered = ip->sb->covered;
            iunlockput(ip);
            if (!covered) {             /* ".." of / is / */
                ip = idup(root_inode);
                continue;
            }
            ip = idup(covered);
            ilock(ip);
        }
        u32 ino = ip->sb->ops->lookup ? ip->sb->ops->lookup(ip, comp) : 0;
        if (!ino) {
            iunlockput(ip);
            *err = -ENOENT;
            return NULL;
        }
        struct inode *next = iget(ip->sb, ino);
        iunlockput(ip);
        if (next && next->mounted) {    /* enter a mounted filesystem */
            struct inode *r = idup(next->mounted->root);
            iput(next);
            next = r;
        }
        ip = next;
    }
    if (e) {
        iput(ip);
        *err = e;
        return NULL;
    }
    if (parent) {                       /* path was "/" or only "." components */
        iput(ip);
        *err = -EEXIST;
        return NULL;
    }
    return ip;
}

struct inode *namei(const char *path, int *err)
{
    return walk(path, false, NULL, err);
}

struct inode *nameiparent(const char *path, char *name, int *err)
{
    return walk(path, true, name, err);
}

/* Wall-clock time of boot, for files of filesystems that keep no times. */
static u64 boot_time_ns(void)
{
    static u64 boot;
    if (!boot) {
        u64 tb = platform.timebase ? platform.timebase : 1;
        u64 up = rdtime();
        boot = rtc_time_ns() - (up / tb) * 1000000000ull - (up % tb) * 1000000000ull / tb;
    }
    return boot;
}

void vfs_stat(struct inode *ip, struct stat *st)
{
    if (ip->sb->ops->refresh)           /* attributes another host may have changed (ip is locked) */
        ip->sb->ops->refresh(ip);
    memset(st, 0, sizeof(*st));
    st->st_dev = ip->sb->dev;
    st->st_ino = ip->ino;
    st->st_mode = ip->mode | (ip->type == I_DIR ? S_IFDIR : ip->type == I_CHR ? S_IFCHR : S_IFREG);
    st->st_nlink = ip->nlink;
    st->st_uid = ip->uid;
    st->st_gid = ip->gid;
    st->st_size = ip->size;
    st->st_blocks = ip->type == I_CHR ? 0 : (ip->size + 511) / 512;
    u64 mtime = ip->mtime, ctime = ip->ctime;
    if (!mtime && !ctime)                       /* devfs, procfs */
        mtime = ctime = boot_time_ns();
    st->st_mtim.tv_sec = mtime / 1000000000ULL;
    st->st_mtim.tv_nsec = mtime % 1000000000ULL;
    st->st_ctim.tv_sec = ctime / 1000000000ULL;
    st->st_ctim.tv_nsec = ctime % 1000000000ULL;
    st->st_rdev = ip->rdev;
}

/* ---- namespace operations ---- */

/* Create `name` in locked dir `dp` (caller holds ns_lock and a transaction). */
static struct inode *create_in(struct inode *dp, const char *name, enum itype type, int mode,
                               int *err)
{
    struct super_block *sb = dp->sb;
    if (sb->ops->create) {
        struct proc *cp = current_proc();
        return sb->ops->create(dp, name, type, mode & 07777 & ~(cp ? cp->umask : 0), err);
    }
    if (!sb->ops->alloc_inode) {
        *err = -EPERM;
        return NULL;
    }
    struct inode *ip = sb->ops->alloc_inode(sb, type);
    if (!ip) {
        *err = -ENOSPC;
        return NULL;
    }
    ilock(ip);
    ip->nlink = 1;
    struct proc *p = current_proc();
    ip->mode = mode & 07777 & ~(p ? p->umask : 0);
    creds(&ip->uid, &ip->gid);
    if (type == I_DIR) {
        ip->nlink = 2;                  /* "." and the parent's entry */
        int r = sb->ops->link(ip, ".", ip->ino);
        if (!r)
            r = sb->ops->link(ip, "..", dp->ino);
        if (r) {
            *err = r;
            goto undo;
        }
        dp->nlink++;                    /* the new ".." */
        sb->ops->write_inode(dp);
    }
    sb->ops->write_inode(ip);
    int r = sb->ops->link(dp, name, ip->ino);
    if (r) {
        *err = r;
        if (type == I_DIR) {
            dp->nlink--;
            sb->ops->write_inode(dp);
        }
        goto undo;
    }
    return ip;                          /* locked */
undo:
    ip->nlink = 0;
    sb->ops->write_inode(ip);
    iunlockput(ip);                     /* frees it on disk */
    return NULL;
}

struct file *vfs_open(const char *path, int flags, int mode, int *err)
{
    struct inode *ip;
    char name[EFS_NAMELEN + 1];

    if (flags & O_CREAT) {
        struct inode *dp = nameiparent(path, name, err);
        if (!dp)
            return NULL;
        struct super_block *sb = dp->sb;
        mutex_lock(&sb->ns_lock);
        fs_begin_op(sb);
        ilock(dp);
        u32 ino = sb->ops->lookup(dp, name);
        if (!ino && vfs_permission(dp, W_OK | X_OK)) {
            iunlockput(dp);
            fs_end_op(sb);
            mutex_unlock(&sb->ns_lock);
            *err = -EACCES;
            return NULL;
        }
        if (ino) {
            iunlockput(dp);
            fs_end_op(sb);
            mutex_unlock(&sb->ns_lock);
            if (flags & O_EXCL) {
                *err = -EEXIST;
                return NULL;
            }
            ip = namei(path, err);      /* may cross into a mount */
            if (!ip)
                return NULL;
            ilock(ip);
        } else {
            ip = create_in(dp, name, I_FILE, mode, err);
            iunlockput(dp);
            fs_end_op(sb);
            mutex_unlock(&sb->ns_lock);
            if (!ip)
                return NULL;
        }
    } else {
        ip = namei(path, err);
        if (!ip)
            return NULL;
        ilock(ip);
    }

    int acc = flags & O_ACCMODE;
    if (ip->valid && ip->sb->ops->refresh) {
        int rr = ip->sb->ops->refresh(ip);
        if (rr == -ENOENT || rr == -ESTALE) {
            *err = rr;                  /* gone on the other side */
            goto fail;
        }
    }
    if (!ip->valid) {
        *err = -EIO;
        goto fail;
    }
    if (ip->type == I_DIR && acc != O_RDONLY) {
        *err = -EISDIR;
        goto fail;
    }
    if ((flags & O_DIRECTORY) && ip->type != I_DIR) {
        *err = -ENOTDIR;
        goto fail;
    }
    if (((acc == O_RDONLY || acc == O_RDWR) && vfs_permission(ip, R_OK)) ||
        ((acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC)) && vfs_permission(ip, W_OK))) {
        *err = -EACCES;
        goto fail;
    }
    if (ip->cops && acc != O_RDONLY && !ip->cops->write) {
        *err = -EACCES;                 /* read-only device or /proc file */
        goto fail;
    }
    if (ip->cops && acc != O_WRONLY && !ip->cops->read) {
        *err = -EACCES;
        goto fail;
    }
    if ((flags & O_TRUNC) && ip->type == I_FILE && acc != O_RDONLY && ip->size) {
        if (!ip->sb->ops->truncate) {
            *err = -EPERM;
            goto fail;
        }
        /* one lock order everywhere: the transaction first, then the inode */
        iunlock(ip);
        fs_begin_op(ip->sb);
        ilock(ip);
        if (ip->size) {
            ip->sb->ops->truncate(ip, 0);
            pc_truncate(ip, 0);
        }
        iunlock(ip);
        fs_end_op(ip->sb);
        ilock(ip);
    }

    struct file *f = kzalloc(sizeof(*f));
    if (!f) {
        *err = -ENOMEM;
        goto fail;
    }
    f->refcnt = 1;
    f->ip = ip;
    f->flags = flags;
    f->readable = acc == O_RDONLY || acc == O_RDWR;
    f->writable = acc == O_WRONLY || acc == O_RDWR;
    f->cops = ip->cops;
    strlcpy(f->path, path, sizeof(f->path));
    iunlock(ip);

    size_t len = strlen(path);
    u64 w[3] = { 0, 0, 0 };
    memcpy(w, path + (len > 24 ? len - 24 : 0), MIN(len, (size_t)24));
    trace(file_open, w[0], w[1], w[2]);

    if (f->cops && f->cops->open) {
        int r = f->cops->open(f);
        if (r) {
            iput(ip);
            kfree(f);
            *err = r;
            return NULL;
        }
    }
    return f;
fail:
    iunlockput(ip);
    return NULL;
}

int vfs_mkdir(const char *path, int mode)
{
    char name[EFS_NAMELEN + 1];
    int err;
    struct inode *dp = nameiparent(path, name, &err);
    if (!dp)
        return err;
    struct super_block *sb = dp->sb;
    mutex_lock(&sb->ns_lock);
    fs_begin_op(sb);
    ilock(dp);
    if (sb->ops->lookup(dp, name)) {
        err = -EEXIST;
    } else if (vfs_permission(dp, W_OK | X_OK)) {
        err = -EACCES;
    } else {
        struct inode *ip = create_in(dp, name, I_DIR, mode, &err);
        if (ip) {
            iunlockput(ip);
            err = 0;
        }
    }
    iunlockput(dp);
    fs_end_op(sb);
    mutex_unlock(&sb->ns_lock);
    return err;
}

static bool dir_empty(struct inode *dp)
{
    struct kdirent de;
    u64 off = 0;
    while (dp->sb->ops->readdir(dp, &off, &de))
        if (strcmp(de.name, ".") && strcmp(de.name, ".."))
            return false;
    return true;
}

/* Remove `name` from dir `dp`. Caller holds sb->ns_lock and a transaction. */
static int unlink_locked(struct inode *dp, const char *name, bool want_dir)
{
    struct super_block *sb = dp->sb;
    int err;
    ilock(dp);
    u32 ino = sb->ops->lookup(dp, name);
    struct inode *ip = ino ? iget(sb, ino) : NULL;
    if (!ip) {
        iunlock(dp);
        return -ENOENT;
    }
    if (vfs_permission(dp, W_OK | X_OK)) {
        iunlock(dp);
        iput(ip);
        return -EACCES;
    }
    ilock(ip);
    if (ip->mounted) {
        err = -EBUSY;
    } else if (want_dir && ip->type != I_DIR) {
        err = -ENOTDIR;
    } else if (!want_dir && ip->type == I_DIR) {
        err = -EISDIR;
    } else if (sb->ops->remove) {
        err = sb->ops->remove(dp, name, want_dir);  /* the server checks emptiness */
        if (!err)
            ip->nlink = ip->nlink ? ip->nlink - 1 : 0;
    } else if (want_dir && !dir_empty(ip)) {
        err = -ENOTEMPTY;
    } else {
        err = sb->ops->unlink(dp, name);
        if (!err) {
            if (ip->type == I_DIR) {
                dp->nlink--;            /* its ".." */
                sb->ops->write_inode(dp);
                ip->nlink = 0;          /* "." and the entry */
            } else {
                ip->nlink--;
            }
            sb->ops->write_inode(ip);
        }
    }
    iunlockput(ip);                     /* frees the inode if that was the last link */
    iunlock(dp);
    return err;
}

int vfs_unlink(const char *path, bool want_dir)
{
    char name[EFS_NAMELEN + 1];
    int err;
    struct inode *dp = nameiparent(path, name, &err);
    if (!dp)
        return err;
    struct super_block *sb = dp->sb;
    bool can = sb->ops->unlink || sb->ops->remove;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !can) {
        iput(dp);
        return can ? -EINVAL : -EPERM;
    }
    mutex_lock(&sb->ns_lock);
    fs_begin_op(sb);
    err = unlink_locked(dp, name, want_dir);
    fs_end_op(sb);
    mutex_unlock(&sb->ns_lock);
    iput(dp);
    return err;
}

int vfs_link(const char *oldpath, const char *newpath)
{
    char name[EFS_NAMELEN + 1];
    int err;
    struct inode *ip = namei(oldpath, &err);
    if (!ip)
        return err;
    struct inode *dp = nameiparent(newpath, name, &err);
    if (!dp) {
        iput(ip);
        return err;
    }
    struct super_block *sb = dp->sb;
    if (ip->sb != sb) {
        iput(ip);
        iput(dp);
        return -EXDEV;
    }
    mutex_lock(&sb->ns_lock);
    fs_begin_op(sb);
    ilock(dp);
    int perm = vfs_permission(dp, W_OK | X_OK);
    iunlock(dp);
    ilock(ip);
    if (perm) {
        err = perm;
        iunlock(ip);
    } else if (ip->type == I_DIR) {
        err = -EPERM;                   /* no hard links to directories */
        iunlock(ip);
    } else {
        ip->nlink++;
        sb->ops->write_inode(ip);
        iunlock(ip);
        ilock(dp);
        err = sb->ops->link(dp, name, ip->ino);
        iunlock(dp);
        if (err) {
            ilock(ip);
            ip->nlink--;
            sb->ops->write_inode(ip);
            iunlock(ip);
        }
    }
    iput(ip);
    iput(dp);
    fs_end_op(sb);
    mutex_unlock(&sb->ns_lock);
    return err;
}

/* Is `anc` an ancestor of (or equal to) directory `ip`? (ns_lock held) */
static bool is_ancestor(struct inode *anc, struct inode *ip)
{
    struct inode *cur = idup(ip);
    for (int depth = 0; depth < 256; depth++) {
        if (cur == anc) {
            iput(cur);
            return true;
        }
        if (cur == cur->sb->root) {
            iput(cur);
            return false;
        }
        ilock(cur);
        u32 up = cur->sb->ops->lookup(cur, "..");
        struct super_block *sb = cur->sb;
        iunlockput(cur);
        cur = iget(sb, up);
    }
    iput(cur);
    return false;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    char oname[EFS_NAMELEN + 1], nname[EFS_NAMELEN + 1];
    int err;
    struct inode *odp = nameiparent(oldpath, oname, &err);
    if (!odp)
        return err;
    struct inode *ndp = nameiparent(newpath, nname, &err);
    if (!ndp) {
        iput(odp);
        return err;
    }
    struct super_block *sb = odp->sb;
    if (ndp->sb != sb) {
        iput(odp);
        iput(ndp);
        return -EXDEV;
    }
    if (!strcmp(oname, ".") || !strcmp(oname, "..") || !strcmp(nname, ".") ||
        !strcmp(nname, "..")) {
        iput(odp);
        iput(ndp);
        return -EINVAL;
    }

    mutex_lock(&sb->ns_lock);
    fs_begin_op(sb);
    ilock(odp);
    u32 ino = sb->ops->lookup(odp, oname);
    int perm = vfs_permission(odp, W_OK | X_OK);
    iunlock(odp);
    if (!perm) {
        ilock(ndp);
        perm = vfs_permission(ndp, W_OK | X_OK);
        iunlock(ndp);
    }
    if (perm) {
        err = perm;
        goto out;
    }
    struct inode *ip = ino ? iget(sb, ino) : NULL;
    if (!ip) {
        err = -ENOENT;
        goto out;
    }
    ilock(ip);
    bool isdir = ip->type == I_DIR;
    iunlock(ip);
    if (isdir && is_ancestor(ip, ndp)) {
        err = -EINVAL;                  /* moving a directory into itself */
        goto out_ip;
    }
    if (sb->ops->rename) {
        err = sb->ops->rename(odp, oname, ndp, nname);   /* replaces a target, as rename(2) */
        goto out_ip;
    }

    ilock(ndp);
    u32 tino = sb->ops->lookup(ndp, nname);
    iunlock(ndp);
    if (tino == ino) {
        err = 0;                        /* same file: nothing to do */
        goto out_ip;
    }
    if (tino) {
        struct inode *t = iget(sb, tino);
        ilock(t);
        bool tdir = t->type == I_DIR;
        iunlockput(t);
        if (tdir || isdir) {
            err = tdir ? -EISDIR : -ENOTDIR;
            goto out_ip;
        }
        err = unlink_locked(ndp, nname, false);    /* replace the target file */
        if (err)
            goto out_ip;
    }

    ilock(ndp);
    err = sb->ops->link(ndp, nname, ino);
    if (!err && isdir && ndp != odp) {
        ndp->nlink++;
        sb->ops->write_inode(ndp);
    }
    iunlock(ndp);
    if (err)
        goto out_ip;
    ilock(odp);
    sb->ops->unlink(odp, oname);
    if (isdir && ndp != odp) {
        odp->nlink--;
        sb->ops->write_inode(odp);
    }
    iunlock(odp);
    if (isdir && ndp != odp) {          /* fix the moved directory's ".." */
        ilock(ip);
        sb->ops->unlink(ip, "..");
        sb->ops->link(ip, "..", ndp->ino);
        iunlock(ip);
    }
out_ip:
    iput(ip);
out:
    fs_end_op(sb);
    mutex_unlock(&sb->ns_lock);
    iput(odp);
    iput(ndp);
    return err;
}

/* Build the absolute path of directory `cwd` by walking "..". */
int vfs_getcwd(struct inode *cwd, char *buf, size_t size)
{
    char tmp[PATH_MAX];
    size_t pos = sizeof(tmp) - 1;
    tmp[pos] = '\0';
    struct inode *cur = idup(cwd);

    for (int depth = 0; depth < 64; depth++) {
        if (cur == root_inode)
            break;
        if (cur == cur->sb->root) {     /* climb out of a mount */
            struct inode *c = idup(cur->sb->covered);
            iput(cur);
            cur = c;
            continue;
        }
        ilock(cur);
        u32 up = cur->sb->ops->lookup(cur, "..");
        iunlock(cur);
        struct inode *parent = iget(cur->sb, up);
        ilock(parent);
        struct kdirent de;
        u64 off = 0;
        bool found = false;
        while (parent->sb->ops->readdir(parent, &off, &de)) {
            if (de.ino == cur->ino && strcmp(de.name, ".") && strcmp(de.name, "..")) {
                found = true;
                break;
            }
        }
        iunlock(parent);
        iput(cur);
        cur = parent;
        size_t len = strlen(de.name);
        if (!found || len + 1 > pos) {
            iput(cur);
            return found ? -ERANGE : -ENOENT;
        }
        pos -= len;
        memcpy(tmp + pos, de.name, len);
        tmp[--pos] = '/';
    }
    iput(cur);
    const char *res = tmp[pos] ? tmp + pos : "/";
    if (strlen(res) + 1 > size)
        return -ERANGE;
    strlcpy(buf, res, size);
    return 0;
}

void *vfs_read_all(const char *path, size_t *size, int *err)
{
    struct inode *ip = namei(path, err);
    if (!ip)
        return NULL;
    ilock(ip);
    if (ip->type != I_FILE) {
        iunlockput(ip);
        *err = ip->type == I_DIR ? -EISDIR : -EACCES;
        return NULL;
    }
    if (ip->size > (16UL << 20)) {
        iunlockput(ip);
        *err = -EFBIG;
        return NULL;
    }
    size_t n = ip->size;
    void *data = kmalloc(n ? n : 1);
    long r = data ? ip->sb->ops->read(ip, data, 0, n, false) : -ENOMEM;
    iunlockput(ip);
    if (r != (long)n) {
        kfree(data);
        *err = r < 0 ? (int)r : -EIO;
        return NULL;
    }
    *size = n;
    return data;
}

/* The volume named `label`: by its GPT partition name, else by its efs
   volume label; -1 if there is none. */
static int volume_labeled(const char *label)
{
    for (int v = 0; v < blk_count(); v++)
        if (!strcmp(blk_label(v), label))
            return v;
    for (int v = 0; v < blk_count(); v++) {
        char l[17];
        if (efs_label(v, l) == 0 && !strcmp(l, label))
            return v;
    }
    return -1;
}

/* A boot slot (by GPT name or efs label): never the root filesystem. */
static bool is_boot_volume(int v)
{
    char l[17];
    const char *g = blk_label(v);
    if (!strcmp(g, "boot") || !strcmp(g, "boot.old"))
        return true;
    return efs_label(v, l) == 0 && (!strcmp(l, "boot") || !strcmp(l, "boot.old"));
}

/* The root filesystem: the volume labelled "root", else the first efs volume
   that is not a boot slot, else volume 0. */
static int root_volume(void)
{
    int root = volume_labeled("root");
    if (root >= 0)
        return root;
    for (int v = 0; v < blk_count(); v++) {
        char l[17];
        if (efs_label(v, l) == 0 && !is_boot_volume(v))
            return v;
    }
    return 0;
}

void vfs_init(void)
{
    bcache_init();
    blk_scan_volumes();
    /* / is the volume labelled "root" (see root_volume); /home, /boot and
       /boot.old the ones labelled so, when there are */
    int root = root_volume();
    if (vfs_mount_disk(&efs_type, "/", root))
        panic("cannot mount the root filesystem");
    static const char *const others[][2] = {
        { "home", "/home" }, { "boot", "/boot" }, { "boot.old", "/boot.old" },
    };
    for (size_t i = 0; i < ARRAY_SIZE(others); i++) {
        int v = volume_labeled(others[i][0]);
        if (v < 0 || v == root)
            continue;
        int r = vfs_mount_disk(&efs_type, others[i][1], v);
        if (r)
            pr_warn("%s (%s) not mounted: %s\n", others[i][1], blk_name(v), errno_name(-r));
    }
    int r = vfs_mount(&devfs_type, "/dev");
    if (r)
        pr_warn("/dev not mounted (%s); create /dev in the image\n", errno_name(-r));
    r = vfs_mount(&procfs_type, "/proc");
    if (r)
        pr_warn("/proc not mounted (%s); create /proc in the image\n", errno_name(-r));
    /* host directories shared through 9P: /mnt/<tag> */
    for (int i = 0; i < p9_count(); i++) {
        char path[48];
        ksnprintf(path, sizeof(path), "/mnt/%s", p9_tag(i));
        r = vfs_mount_disk(&ninep_type, path, i);
        if (r)
            pr_warn("9p share \"%s\" not mounted on %s: %s\n", p9_tag(i), path, errno_name(-r));
    }
}
