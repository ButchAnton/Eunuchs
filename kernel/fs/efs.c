/*
 * efs: the on-disk filesystem (format: efs_format.h). Implements struct
 * fs_ops for the VFS. All modifications go through the log; callers bracket
 * them with fs_begin_op/fs_end_op. Inode contents are protected by the
 * inode lock held by the VFS caller.
 *
 * Consistency problems noticed at run time (freeing a free block, pointers
 * outside the data area, ...) are reported with fs_error() and counted in
 * the superblock; `make test` also runs the host fsck on the image.
 */
#define LOG_SUBSYS SUBSYS_FS
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <uapi/errno.h>

struct efs_info {
    struct efs_super s;
    struct efs_log *log;
    spinlock_t alloc_lock;
    u32 bhint;                  /* where to start looking for free blocks */
    u32 nfree_blocks;           /* kept in memory so stats need no I/O */
    u32 nfree_inodes;
};

static struct efs_info *info(struct super_block *sb) { return sb->priv; }
#define BREAD(...) bread(sb->disk, __VA_ARGS__)   /* every caller has its sb */

u64 rtc_time_ns(void);

/* ---- blocks ---- */

static void bzero_block(struct super_block *sb, u32 bno)
{
    struct buf *b = BREAD(bno);
    memset(b->data, 0, EFS_BSIZE);
    log_write(info(sb)->log, b);
    brelse(b);
}

static u32 balloc(struct super_block *sb)
{
    struct efs_info *fi = info(sb);
    u32 size = fi->s.size, per = EFS_BSIZE * 8;
    for (u32 n = 0; n < size; n++) {
        u32 bno = fi->s.datastart + (fi->bhint - fi->s.datastart + n) % (size - fi->s.datastart);
        if (n && bno == fi->bhint)
            break;
        struct buf *b = BREAD(fi->s.bmapstart + bno / per);
        u8 *byte = &b->data[(bno % per) / 8];
        u8 bit = 1 << (bno % 8);
        if (!(*byte & bit)) {
            *byte |= bit;
            log_write(fi->log, b);
            brelse(b);
            fi->bhint = bno + 1 < size ? bno + 1 : fi->s.datastart;
            fi->nfree_blocks--;
            bzero_block(sb, bno);
            return bno;
        }
        brelse(b);
    }
    pr_warn("efs: out of disk blocks\n");
    return 0;
}

static void bfree(struct super_block *sb, u32 bno)
{
    struct efs_info *fi = info(sb);
    if (bno < fi->s.datastart || bno >= fi->s.size) {
        fs_error(sb, "freeing block %u outside the data area [%u, %u)", bno, fi->s.datastart,
                 fi->s.size);
        return;
    }
    u32 per = EFS_BSIZE * 8;
    struct buf *b = BREAD(fi->s.bmapstart + bno / per);
    u8 *byte = &b->data[(bno % per) / 8];
    u8 bit = 1 << (bno % 8);
    if (!(*byte & bit))
        fs_error(sb, "freeing block %u which is already free (double free)", bno);
    else
        fi->nfree_blocks++;
    *byte &= ~bit;
    log_write(fi->log, b);
    brelse(b);
}

static bool block_ok(struct super_block *sb, u32 bno, struct inode *ip)
{
    if (bno >= info(sb)->s.datastart && bno < info(sb)->s.size)
        return true;
    fs_error(sb, "inode %u: block pointer %u outside the data area", ip->ino, bno);
    return false;
}

/* ---- inodes ---- */

static struct buf *inode_block(struct inode *ip, struct efs_dinode **d)
{
    struct super_block *sb = ip->sb;
    struct efs_info *fi = info(ip->sb);
    struct buf *b = BREAD(fi->s.inodestart + ip->ino / EFS_IPB);
    *d = (struct efs_dinode *)b->data + ip->ino % EFS_IPB;
    return b;
}

static int efs_read_inode(struct inode *ip)
{
    struct efs_dinode *d;
    if (ip->ino == 0 || ip->ino >= info(ip->sb)->s.ninodes) {
        fs_error(ip->sb, "inode number %u out of range", ip->ino);
        return -EIO;
    }
    struct buf *b = inode_block(ip, &d);
    ip->u.efs = *d;
    brelse(b);
    ip->type = d->type == EFS_T_DIR ? I_DIR : d->type == EFS_T_FILE ? I_FILE : I_FREE;
    ip->nlink = d->nlink;
    ip->mode = d->mode;
    ip->uid = d->uid;
    ip->gid = d->gid;
    ip->size = d->size;
    ip->mtime = d->mtime;
    ip->ctime = d->ctime;
    if (ip->type == I_FREE) {
        fs_error(ip->sb, "inode %u is free but referenced (a stale directory entry?)", ip->ino);
        return -EIO;
    }
    return 0;
}

static int efs_write_inode(struct inode *ip)
{
    struct efs_dinode *d;
    struct buf *b = inode_block(ip, &d);
    ip->u.efs.type = ip->type == I_DIR ? EFS_T_DIR : ip->type == I_FILE ? EFS_T_FILE : EFS_T_FREE;
    ip->u.efs.nlink = ip->nlink;
    ip->u.efs.mode = ip->mode;
    ip->u.efs.uid = ip->uid;
    ip->u.efs.gid = ip->gid;
    ip->u.efs.size = ip->size;
    ip->u.efs.mtime = ip->mtime;
    ip->u.efs.ctime = ip->ctime;
    *d = ip->u.efs;
    log_write(info(ip->sb)->log, b);
    brelse(b);
    return 0;
}

static struct inode *efs_alloc_inode(struct super_block *sb, enum itype type)
{
    struct efs_info *fi = info(sb);
    for (u32 ino = 1; ino < fi->s.ninodes; ino++) {
        struct buf *b = BREAD(fi->s.inodestart + ino / EFS_IPB);
        struct efs_dinode *d = (struct efs_dinode *)b->data + ino % EFS_IPB;
        if (d->type == EFS_T_FREE) {
            memset(d, 0, sizeof(*d));
            d->type = type == I_DIR ? EFS_T_DIR : EFS_T_FILE;
            d->mtime = d->ctime = rtc_time_ns();
            log_write(fi->log, b);
            brelse(b);
            fi->nfree_inodes--;
            return iget(sb, ino);
        }
        brelse(b);
    }
    pr_warn("efs: out of inodes\n");
    return NULL;
}

/* Entry `idx` of indirect block `ind`, allocating a block for it if asked. */
static u32 ind_get(struct inode *ip, u32 ind, u64 idx, bool alloc)
{
    struct super_block *sb = ip->sb;
    if (!block_ok(ip->sb, ind, ip))
        return 0;
    struct buf *b = BREAD(ind);
    u32 *a = (u32 *)b->data;
    u32 bno = a[idx];
    if (!bno && alloc && (bno = balloc(ip->sb))) {
        a[idx] = bno;
        log_write(info(ip->sb)->log, b);
    }
    brelse(b);
    return bno;
}

/* Make sure *slot (an inode address field) holds a block; 0 on failure. */
static u32 ensure(struct inode *ip, u32 *slot, bool alloc)
{
    if (!*slot && alloc)
        *slot = balloc(ip->sb);
    return *slot;
}

/* Disk block for file block `bn`; 0 if unallocated and !alloc (or no space). */
static u32 bmap(struct inode *ip, u64 bn, bool alloc)
{
    u32 *addrs = ip->u.efs.addrs;

    if (bn < EFS_NDIRECT)
        return ensure(ip, &addrs[bn], alloc);
    bn -= EFS_NDIRECT;
    if (bn < EFS_NINDIRECT) {
        u32 ind = ensure(ip, &addrs[EFS_NDIRECT], alloc);
        return ind ? ind_get(ip, ind, bn, alloc) : 0;
    }
    bn -= EFS_NINDIRECT;
    if (bn >= (u64)EFS_NINDIRECT * EFS_NINDIRECT)
        return 0;
    u32 dbl = ensure(ip, &addrs[EFS_NDIRECT + 1], alloc);
    u32 ind = dbl ? ind_get(ip, dbl, bn / EFS_NINDIRECT, alloc) : 0;
    return ind ? ind_get(ip, ind, bn % EFS_NINDIRECT, alloc) : 0;
}

/* Free the blocks listed in an indirect block from index `from` on. */
static void free_ind(struct super_block *sb, u32 bno, u64 from, int depth, struct inode *ip)
{
    if (!block_ok(sb, bno, ip))
        return;
    struct buf *b = BREAD(bno);
    u32 *a = (u32 *)b->data;
    u64 per = depth == 2 ? EFS_NINDIRECT : 1;
    for (u64 i = 0; i < EFS_NINDIRECT; i++) {
        if (!a[i])
            continue;
        if (depth == 2) {
            u64 first = i * per;
            if (first + per <= from)
                continue;
            free_ind(sb, a[i], from > first ? from - first : 0, 1, ip);
            if (from <= first) {
                a[i] = 0;
                log_write(info(sb)->log, b);
            }
        } else if (i >= from) {
            bfree(sb, a[i]);
            a[i] = 0;
            log_write(info(sb)->log, b);
        }
    }
    brelse(b);
    if (from == 0)
        bfree(sb, bno);
}

static int efs_truncate(struct inode *ip, u64 size)
{
    struct super_block *sb = ip->sb;
    u32 *addrs = ip->u.efs.addrs;
    u64 keep = (size + EFS_BSIZE - 1) / EFS_BSIZE;      /* blocks to keep */

    for (u64 i = keep; i < EFS_NDIRECT; i++) {
        if (addrs[i]) {
            bfree(sb, addrs[i]);
            addrs[i] = 0;
        }
    }
    u64 from = keep > EFS_NDIRECT ? keep - EFS_NDIRECT : 0;
    if (addrs[EFS_NDIRECT] && from < EFS_NINDIRECT) {
        free_ind(sb, addrs[EFS_NDIRECT], from, 1, ip);
        if (from == 0)
            addrs[EFS_NDIRECT] = 0;
    }
    from = keep > EFS_NDIRECT + EFS_NINDIRECT ? keep - EFS_NDIRECT - EFS_NINDIRECT : 0;
    if (addrs[EFS_NDIRECT + 1]) {
        free_ind(sb, addrs[EFS_NDIRECT + 1], from, 2, ip);
        if (from == 0)
            addrs[EFS_NDIRECT + 1] = 0;
    }
    ip->size = size;
    ip->mtime = rtc_time_ns();
    return efs_write_inode(ip);
}

static void efs_free_inode(struct inode *ip)
{
    efs_truncate(ip, 0);
    ip->type = I_FREE;
    ip->nlink = 0;
    efs_write_inode(ip);
    info(ip->sb)->nfree_inodes++;
}

static long efs_read(struct inode *ip, void *dst, u64 off, size_t n, bool user)
{
    struct super_block *sb = ip->sb;
    if (off >= ip->size)
        return 0;
    if (n > ip->size - off)
        n = ip->size - off;
    size_t done = 0;
    while (done < n) {
        u64 bn = (off + done) / EFS_BSIZE;
        size_t boff = (off + done) % EFS_BSIZE;
        size_t chunk = MIN(n - done, (size_t)EFS_BSIZE - boff);
        u32 bno = bmap(ip, bn, false);
        const void *src;
        struct buf *b = NULL;
        static const u8 zeros[EFS_BSIZE];
        if (bno) {
            b = BREAD(bno);
            src = b->data + boff;
        } else {
            src = zeros;                        /* hole */
        }
        long r = user ? copy_to_user((uintptr_t)dst + done, src, chunk)
                      : (memcpy((u8 *)dst + done, src, chunk), 0);
        if (b)
            brelse(b);
        if (r)
            return done ? (long)done : -EFAULT;
        done += chunk;
    }
    return done;
}

static long efs_write(struct inode *ip, const void *src, u64 off, size_t n, bool user)
{
    struct super_block *sb = ip->sb;
    if (off + n > EFS_MAXFILE)
        return -EFBIG;
    size_t done = 0;
    while (done < n) {
        u64 bn = (off + done) / EFS_BSIZE;
        size_t boff = (off + done) % EFS_BSIZE;
        size_t chunk = MIN(n - done, (size_t)EFS_BSIZE - boff);
        u32 bno = bmap(ip, bn, true);
        if (!bno)
            break;                              /* disk full */
        struct buf *b = BREAD(bno);
        long r = user ? copy_from_user(b->data + boff, (uintptr_t)src + done, chunk)
                      : (memcpy(b->data + boff, (const u8 *)src + done, chunk), 0);
        if (r) {
            brelse(b);
            break;
        }
        log_write(info(ip->sb)->log, b);
        brelse(b);
        done += chunk;
    }
    if (off + done > ip->size)
        ip->size = off + done;
    ip->mtime = rtc_time_ns();
    efs_write_inode(ip);
    if (!done && n)
        return user ? -EFAULT : -ENOSPC;
    return done;
}

/* ---- directories ---- */

static bool name_ok(const char *name)
{
    size_t n = strlen(name);
    return n > 0 && n <= EFS_NAMELEN && !strchr(name, '/');
}

/* Find `name`; returns its inode number and the entry offset. */
static u32 dir_find(struct inode *dir, const char *name, u64 *offp)
{
    struct efs_dirent de;
    for (u64 off = 0; off < dir->size; off += sizeof(de)) {
        if (efs_read(dir, &de, off, sizeof(de), false) != sizeof(de)) {
            fs_error(dir->sb, "directory inode %u: short read at %lu", dir->ino,
                     (unsigned long)off);
            return 0;
        }
        if (de.ino && !strncmp(de.name, name, sizeof(de.name))) {
            if (offp)
                *offp = off;
            return de.ino;
        }
    }
    return 0;
}

static u32 efs_lookup(struct inode *dir, const char *name)
{
    return dir_find(dir, name, NULL);
}

static int efs_link(struct inode *dir, const char *name, u32 ino)
{
    struct efs_dirent de;
    u64 off;
    if (!name_ok(name))
        return strlen(name) > EFS_NAMELEN ? -ENAMETOOLONG : -EINVAL;
    if (dir_find(dir, name, NULL))
        return -EEXIST;
    for (off = 0; off < dir->size; off += sizeof(de)) {
        efs_read(dir, &de, off, sizeof(de), false);
        if (!de.ino)
            break;                              /* reuse a free slot */
    }
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    strlcpy(de.name, name, sizeof(de.name));
    return efs_write(dir, &de, off, sizeof(de), false) == sizeof(de) ? 0 : -ENOSPC;
}

static int efs_unlink(struct inode *dir, const char *name)
{
    u64 off;
    if (!dir_find(dir, name, &off))
        return -ENOENT;
    struct efs_dirent de;
    memset(&de, 0, sizeof(de));
    return efs_write(dir, &de, off, sizeof(de), false) == sizeof(de) ? 0 : -EIO;
}

static int efs_readdir(struct inode *dir, u64 *off, struct kdirent *out)
{
    struct efs_dirent de;
    while (*off < dir->size) {
        if (efs_read(dir, &de, *off, sizeof(de), false) != sizeof(de))
            return 0;
        *off += sizeof(de);
        if (de.ino) {
            out->ino = de.ino;
            out->type = DT_UNKNOWN;
            strlcpy(out->name, de.name, sizeof(out->name));
            return 1;
        }
    }
    return 0;
}

static void efs_begin_op(struct super_block *sb) { log_begin(info(sb)->log); }
static void efs_end_op(struct super_block *sb) { log_end(info(sb)->log); }

/* Free blocks counted from the on-disk bitmap (tests compare with the counter). */
u32 efs_free_blocks(struct super_block *sb)
{
    struct efs_info *fi = info(sb);
    u32 used = 0, per = EFS_BSIZE * 8;
    struct buf *b = NULL;
    for (u32 bno = fi->s.datastart; bno < fi->s.size; bno++) {
        if (!b || bno % per == 0 || bno == fi->s.datastart) {
            if (b)
                brelse(b);
            b = BREAD(fi->s.bmapstart + bno / per);
        }
        used += (b->data[(bno % per) / 8] >> (bno % 8)) & 1;
    }
    if (b)
        brelse(b);
    return fi->s.size - fi->s.datastart - used;
}

u32 efs_free_blocks_counter(struct super_block *sb)
{
    return info(sb)->nfree_blocks;
}

/* No I/O and no sleeping locks: safe from the debug monitor. */
static void efs_stat_fs(struct super_block *sb, print_fn print)
{
    struct efs_info *fi = info(sb);
    u32 data = fi->s.size - fi->s.datastart;
    print("  efs: %u blocks of %d bytes; data %u used, %u free; inodes %u used, %u free\n",
          fi->s.size, EFS_BSIZE, data - fi->nfree_blocks, fi->nfree_blocks,
          fi->s.ninodes - 1 - fi->nfree_inodes, fi->nfree_inodes);
    log_print(fi->log, print);
}

static const struct fs_ops efs_ops = {
    .read_inode = efs_read_inode,
    .write_inode = efs_write_inode,
    .alloc_inode = efs_alloc_inode,
    .free_inode = efs_free_inode,
    .read = efs_read,
    .write = efs_write,
    .truncate = efs_truncate,
    .lookup = efs_lookup,
    .link = efs_link,
    .unlink = efs_unlink,
    .readdir = efs_readdir,
    .begin_op = efs_begin_op,
    .end_op = efs_end_op,
    .stat_fs = efs_stat_fs,
};

static int efs_mount(struct super_block *sb)
{
    struct efs_info *fi = kzalloc(sizeof(*fi));
    struct buf *b = BREAD(1);
    memcpy(&fi->s, b->data, sizeof(fi->s));
    brelse(b);
    if (fi->s.magic != EFS_MAGIC) {
        pr_err("efs: not an efs filesystem (magic %#x, want %#x)\n", fi->s.magic, EFS_MAGIC);
        kfree(fi);
        return -EINVAL;
    }
    if (fi->s.bsize != EFS_BSIZE || fi->s.version != EFS_VERSION) {
        pr_err("efs: unsupported block size %u / version %u\n", fi->s.bsize, fi->s.version);
        kfree(fi);
        return -EINVAL;
    }
    spin_init(&fi->alloc_lock, "efs-alloc");
    fi->bhint = fi->s.datastart;
    sb->priv = fi;
    fi->log = log_create(sb, &fi->s);          /* replays a committed log */
    sb->root = iget(sb, EFS_ROOT_INO);
    fi->nfree_blocks = efs_free_blocks(sb);
    for (u32 blk = 0; blk < (fi->s.ninodes + EFS_IPB - 1) / EFS_IPB; blk++) {
        struct buf *b = BREAD(fi->s.inodestart + blk);
        for (u32 i = 0; i < EFS_IPB; i++) {
            u32 ino = blk * EFS_IPB + i;
            if (ino >= 1 && ino < fi->s.ninodes)
                fi->nfree_inodes += ((struct efs_dinode *)b->data)[i].type == EFS_T_FREE;
        }
        brelse(b);
    }
    pr_info("efs: %u blocks, %u inodes, log %u blocks at %u\n", fi->s.size, fi->s.ninodes,
            fi->s.nlog, fi->s.logstart);
    return 0;
}

/* The volume label of the efs filesystem on a disk (before mounting it). */
int efs_label(int disk, char out[17])
{
    struct buf *b = bread(disk, 1);
    const struct efs_super *s = (const void *)b->data;
    int r = -1;
    if (s->magic == EFS_MAGIC) {
        memcpy(out, s->label, 16);
        out[16] = 0;
        r = 0;
    }
    brelse(b);
    return r;
}

const struct fs_type efs_type = { .name = "efs", .mount = efs_mount, .ops = &efs_ops };
