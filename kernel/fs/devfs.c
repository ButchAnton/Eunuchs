/*
 * devfs, mounted on /dev: console, null, zero, kmsg (kernel log snapshot),
 * disk0, disk1 (whole disks) and disk0p1... (their partitions), read-only.
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <console.h>
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <uapi/errno.h>
#include <virtio.h>

enum { DEV_ROOT = 1, DEV_CONSOLE, DEV_NULL, DEV_ZERO, DEV_KMSG, DEV_DISK0, DEV_TTY, DEV_TTY0,
       DEV_DISK1, DEV_MAX, DEV_VOL = DEV_MAX, DEV_END = DEV_VOL + MAX_VOLUMES };

/* disk0, disk1: which whole disk, or -1 */
static int disk_of(u32 ino)
{
    return ino == DEV_DISK0 ? 0 : ino == DEV_DISK1 ? 1 : -1;
}

/* a partition's node: which volume, or -1 (a whole-disk volume has none:
   it is the disk's node) */
static int vol_of(u32 ino)
{
    if (ino < DEV_VOL || ino >= DEV_END || (int)(ino - DEV_VOL) >= blk_count())
        return -1;
    int v = (int)(ino - DEV_VOL);
    return strchr(blk_name(v), 'p') ? v : -1;
}

static bool is_disk(u32 ino) { return disk_of(ino) >= 0 || ino >= DEV_VOL; }

/* Devices that exist (a disk device only if that disk is attached). */
static bool dev_exists(u32 ino)
{
    if (ino >= DEV_VOL)
        return vol_of(ino) >= 0;
    int d = disk_of(ino);
    return d < 0 || d < blk_disk_count();
}

static u64 disk_size(u32 ino)
{
    return (ino >= DEV_VOL ? blk_capacity(vol_of(ino)) : blk_disk_capacity(disk_of(ino))) * 512;
}

static int disk_read(u32 ino, u64 sector, void *buf, u32 len)
{
    return ino >= DEV_VOL ? blk_rw(vol_of(ino), sector, buf, len, false)
                          : blk_disk_rw(disk_of(ino), sector, buf, len, false);
}

static long out(void *dst, const void *src, size_t n, bool user)
{
    if (user)
        return copy_to_user((uintptr_t)dst, src, n);
    memcpy(dst, src, n);
    return 0;
}

/* /dev/tty: the caller's controlling terminal. */
static int ctty_open(struct file *f)
{
    struct proc *p = current_proc();
    if (p && !p->ctty)
        return -ENXIO;
    f->cops = &tty_ops;
    return 0;
}

static long null_rd(struct file *f, void *b, size_t n, bool u) { (void)f, (void)b, (void)n, (void)u; return 0; }
static long sink_wr(struct file *f, const void *b, size_t n, bool u) { (void)f, (void)b, (void)u; return n; }

static long zero_rd(struct file *f, void *buf, size_t n, bool user)
{
    (void)f;
    static const u8 zeros[256];
    for (size_t done = 0; done < n;) {
        size_t c = MIN(n - done, sizeof(zeros));
        if (out((u8 *)buf + done, zeros, c, user))
            return done ? (long)done : -EFAULT;
        done += c;
    }
    return n;
}

/* kmsg: a snapshot of the log ring at open time. */
static int kmsg_open(struct file *f)
{
    size_t cap = LOG_BUF_SIZE;
    char *buf = kmalloc(cap);
    if (!buf)
        return -ENOMEM;
    f->priv_len = log_ring_copy(buf, cap);
    f->priv = buf;
    return 0;
}

static long snapshot_rd(struct file *f, void *buf, size_t n, bool user)
{
    if (f->off >= f->priv_len)
        return 0;
    n = MIN(n, f->priv_len - f->off);
    if (out(buf, (char *)f->priv + f->off, n, user))
        return -EFAULT;
    f->off += n;
    return n;
}

static void snapshot_close(struct file *f)
{
    kfree(f->priv);
}

static long disk_rd(struct file *f, void *buf, size_t n, bool user)
{
    u32 ino = f->ip->ino;
    u64 size = disk_size(ino);
    if (f->off >= size)
        return 0;
    n = MIN(n, size - f->off);
    u8 *page = page_alloc();
    if (!page)
        return -ENOMEM;
    size_t done = 0;
    while (done < n) {
        u64 pos = f->off + done, base = ALIGN_DOWN(pos, PAGE_SIZE);
        if (disk_read(ino, base / 512, page, MIN((u64)PAGE_SIZE, size - base)))
            break;
        size_t c = MIN(n - done, (size_t)(base + PAGE_SIZE - pos));
        if (out((u8 *)buf + done, page + (pos - base), c, user))
            break;
        done += c;
    }
    page_free(page);
    f->off += done;
    return done ? (long)done : -EIO;
}

static long ctty_never(struct file *f, void *b, size_t n, bool u) { (void)f, (void)b, (void)n, (void)u; return -ENXIO; }
static long ctty_never_w(struct file *f, const void *b, size_t n, bool u) { (void)f, (void)b, (void)n, (void)u; return -ENXIO; }
/* read/write are replaced by the tty's at open (they only make vfs_open's access checks pass). */
static const struct chr_ops ctty_ops = { .open = ctty_open, .read = ctty_never, .write = ctty_never_w };
static const struct chr_ops null_ops = { .read = null_rd, .write = sink_wr };
static const struct chr_ops zero_ops = { .read = zero_rd, .write = sink_wr };
static const struct chr_ops kmsg_ops = { .open = kmsg_open, .read = snapshot_rd, .close = snapshot_close };
static const struct chr_ops disk_ops = { .read = disk_rd };

static const struct {
    const char *name;
    const struct chr_ops *ops;
    u16 mode;
} devs[DEV_MAX] = {
    [DEV_CONSOLE] = { "console", &tty_ops, 0620 },
    [DEV_TTY]     = { "tty", &ctty_ops, 0666 },
    [DEV_TTY0]    = { "tty0", &tty_ops, 0620 },
    [DEV_NULL]    = { "null", &null_ops, 0666 },
    [DEV_ZERO]    = { "zero", &zero_ops, 0666 },
    [DEV_KMSG]    = { "kmsg", &kmsg_ops, 0444 },
    [DEV_DISK0]   = { "disk0", &disk_ops, 0440 },
    [DEV_DISK1]   = { "disk1", &disk_ops, 0440 },
};

static const char *dev_name(u32 ino)
{
    return ino >= DEV_VOL ? blk_name(vol_of(ino)) : devs[ino].name;
}

static int dev_read_inode(struct inode *ip)
{
    if (ip->ino == DEV_ROOT) {
        ip->type = I_DIR;
        ip->nlink = 2;
        ip->mode = 0755;
        return 0;
    }
    if (ip->ino >= DEV_END || (ip->ino < DEV_MAX && !devs[ip->ino].name) || !dev_exists(ip->ino))
        return -ENOENT;
    ip->type = I_CHR;
    ip->nlink = 1;
    ip->mode = ip->ino >= DEV_VOL ? 0440 : devs[ip->ino].mode;
    ip->rdev = ip->ino;
    ip->cops = ip->ino >= DEV_VOL ? &disk_ops : devs[ip->ino].ops;
    if (is_disk(ip->ino))
        ip->size = disk_size(ip->ino);
    return 0;
}

static u32 dev_lookup(struct inode *dir, const char *name)
{
    (void)dir;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return DEV_ROOT;
    for (u32 i = DEV_CONSOLE; i < DEV_END; i++)
        if (dev_exists(i) && (i >= DEV_VOL || devs[i].name) && !strcmp(name, dev_name(i)))
            return i;
    return 0;
}

static int dev_readdir(struct inode *dir, u64 *off, struct kdirent *de)
{
    (void)dir;
    u64 i = (*off)++;
    if (i < 2) {
        de->ino = DEV_ROOT;
        de->type = DT_DIR;
        strlcpy(de->name, i ? ".." : ".", sizeof(de->name));
        return 1;
    }
    u32 ino = DEV_CONSOLE + (u32)(i - 2);
    while (ino < DEV_END && !dev_exists(ino)) {     /* skip missing disks */
        ino++;
        (*off)++;
    }
    if (ino >= DEV_END)
        return 0;
    de->ino = ino;
    de->type = DT_CHR;
    strlcpy(de->name, dev_name(ino), sizeof(de->name));
    return 1;
}

static const struct fs_ops dev_ops = {
    .read_inode = dev_read_inode,
    .lookup = dev_lookup,
    .readdir = dev_readdir,
};

static int dev_mount(struct super_block *sb)
{
    sb->root = iget(sb, DEV_ROOT);
    return 0;
}

const struct fs_type devfs_type = { .name = "devfs", .mount = dev_mount, .ops = &dev_ops };
