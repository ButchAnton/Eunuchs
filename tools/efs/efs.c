/*
 * efs on the host (see efs.h).
 */
#include "efs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DIRENT_SIZE ((uint32_t)sizeof(struct efs_dirent))

uint8_t *efs_block(struct efs *fs, uint32_t b)
{
    return fs->base + (size_t)b * EFS_BSIZE;
}

struct efs_dinode *efs_inode(struct efs *fs, uint32_t ino)
{
    return (struct efs_dinode *)efs_block(fs, fs->sb->inodestart + ino / EFS_IPB) + ino % EFS_IPB;
}

const char *efs_strerror(int err)
{
    switch (-err) {
    case ENOENT: return "no such file or directory";
    case EEXIST: return "already exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case ENOTEMPTY: return "directory not empty";
    case ENOSPC: return "no space left (blocks or inodes)";
    case ENAMETOOLONG: return "name too long (59 bytes at most)";
    case EFBIG: return "file too large";
    case EINVAL: return "invalid argument (or not an efs volume)";
    case EPERM: return "not allowed";
    case EMLINK: return "too many links";
    default: return strerror(-err);
    }
}

static uint64_t now_ns(void)
{
    return (uint64_t)time(NULL) * 1000000000ull;
}

/* ---- the free-block bitmap ---- */

static bool bit_get(struct efs *fs, uint32_t b)
{
    return efs_block(fs, fs->sb->bmapstart + b / (EFS_BSIZE * 8))[(b % (EFS_BSIZE * 8)) / 8] >> (b % 8) & 1;
}

static void bit_set(struct efs *fs, uint32_t b, bool on)
{
    uint8_t *p = &efs_block(fs, fs->sb->bmapstart + b / (EFS_BSIZE * 8))[(b % (EFS_BSIZE * 8)) / 8];
    if (on)
        *p |= 1 << (b % 8);
    else
        *p &= ~(1 << (b % 8));
}

/* A zeroed data block, or 0 if the volume is full. */
static uint32_t alloc_block(struct efs *fs)
{
    for (uint32_t b = fs->sb->datastart; b < fs->sb->size; b++) {
        if (!bit_get(fs, b)) {
            bit_set(fs, b, true);
            memset(efs_block(fs, b), 0, EFS_BSIZE);
            return b;
        }
    }
    return 0;
}

static void free_block(struct efs *fs, uint32_t b)
{
    if (b >= fs->sb->datastart && b < fs->sb->size)
        bit_set(fs, b, false);
}

/* ---- inodes ---- */

static int alloc_inode(struct efs *fs, uint16_t type, uint16_t mode, uint16_t uid, uint16_t gid)
{
    for (uint32_t ino = EFS_ROOT_INO; ino < fs->sb->ninodes; ino++) {
        struct efs_dinode *d = efs_inode(fs, ino);
        if (d->type != EFS_T_FREE)
            continue;
        memset(d, 0, sizeof(*d));
        d->type = type;
        d->mode = mode;
        d->uid = uid;
        d->gid = gid;
        d->mtime = d->ctime = fs->now;
        return (int)ino;
    }
    return -ENOSPC;
}

/* The block holding file block fbn; with alloc, allocated if missing (0: a
   hole, past the maximum size, or the volume is full). */
static uint32_t bmap(struct efs *fs, struct efs_dinode *d, uint64_t fbn, bool alloc)
{
    uint32_t *slot;
    if (fbn < EFS_NDIRECT) {
        slot = &d->addrs[fbn];
    } else if ((fbn -= EFS_NDIRECT) < EFS_NINDIRECT) {
        if (!d->addrs[EFS_NDIRECT] && (!alloc || !(d->addrs[EFS_NDIRECT] = alloc_block(fs))))
            return 0;
        slot = (uint32_t *)efs_block(fs, d->addrs[EFS_NDIRECT]) + fbn;
    } else if ((fbn -= EFS_NINDIRECT) < (uint64_t)EFS_NINDIRECT * EFS_NINDIRECT) {
        if (!d->addrs[EFS_NDIRECT + 1] && (!alloc || !(d->addrs[EFS_NDIRECT + 1] = alloc_block(fs))))
            return 0;
        uint32_t *dbl = (uint32_t *)efs_block(fs, d->addrs[EFS_NDIRECT + 1]) + fbn / EFS_NINDIRECT;
        if (!*dbl && (!alloc || !(*dbl = alloc_block(fs))))
            return 0;
        slot = (uint32_t *)efs_block(fs, *dbl) + fbn % EFS_NINDIRECT;
    } else {
        return 0;
    }
    if (!*slot && alloc)
        *slot = alloc_block(fs);
    return *slot;
}

long efs_read(struct efs *fs, uint32_t ino, uint64_t off, void *buf, size_t n)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    if (off >= d->size)
        return 0;
    if (n > d->size - off)
        n = d->size - off;
    uint8_t *out = buf;
    for (size_t done = 0; done < n;) {
        uint64_t pos = off + done;
        size_t in = pos % EFS_BSIZE, chunk = EFS_BSIZE - in < n - done ? EFS_BSIZE - in : n - done;
        uint32_t b = bmap(fs, d, pos / EFS_BSIZE, false);
        if (b)
            memcpy(out + done, efs_block(fs, b) + in, chunk);
        else
            memset(out + done, 0, chunk);
        done += chunk;
    }
    return (long)n;
}

int efs_write(struct efs *fs, uint32_t ino, uint64_t off, const void *buf, size_t n)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    if (off + n > EFS_MAXFILE)
        return -EFBIG;
    const uint8_t *in = buf;
    for (size_t done = 0; done < n;) {
        uint64_t pos = off + done;
        size_t at = pos % EFS_BSIZE, chunk = EFS_BSIZE - at < n - done ? EFS_BSIZE - at : n - done;
        uint32_t b = bmap(fs, d, pos / EFS_BSIZE, true);
        if (!b)
            return -ENOSPC;
        memcpy(efs_block(fs, b) + at, in + done, chunk);
        done += chunk;
        if (pos + chunk > d->size)
            d->size = pos + chunk;
    }
    d->mtime = fs->now;
    return 0;
}

/* Free the data blocks at file block `from` and beyond, and indirect blocks
   left with nothing to point to. */
static void free_from(struct efs *fs, struct efs_dinode *d, uint64_t from)
{
    for (uint64_t i = from; i < EFS_NDIRECT; i++) {
        free_block(fs, d->addrs[i]);
        d->addrs[i] = 0;
    }
    uint64_t base = EFS_NDIRECT;
    if (d->addrs[EFS_NDIRECT]) {
        uint32_t *ind = (uint32_t *)efs_block(fs, d->addrs[EFS_NDIRECT]);
        for (uint64_t i = from > base ? from - base : 0; i < EFS_NINDIRECT; i++) {
            free_block(fs, ind[i]);
            ind[i] = 0;
        }
        if (from <= base) {
            free_block(fs, d->addrs[EFS_NDIRECT]);
            d->addrs[EFS_NDIRECT] = 0;
        }
    }
    base += EFS_NINDIRECT;
    if (d->addrs[EFS_NDIRECT + 1]) {
        uint32_t *dbl = (uint32_t *)efs_block(fs, d->addrs[EFS_NDIRECT + 1]);
        for (uint64_t j = 0; j < EFS_NINDIRECT; j++) {
            uint64_t first = base + j * EFS_NINDIRECT;
            if (!dbl[j] || first + EFS_NINDIRECT <= from)
                continue;
            uint32_t *ind = (uint32_t *)efs_block(fs, dbl[j]);
            for (uint64_t i = from > first ? from - first : 0; i < EFS_NINDIRECT; i++) {
                free_block(fs, ind[i]);
                ind[i] = 0;
            }
            if (from <= first) {
                free_block(fs, dbl[j]);
                dbl[j] = 0;
            }
        }
        if (from <= base) {
            free_block(fs, d->addrs[EFS_NDIRECT + 1]);
            d->addrs[EFS_NDIRECT + 1] = 0;
        }
    }
}

int efs_truncate(struct efs *fs, uint32_t ino, uint64_t size)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    if (size > EFS_MAXFILE)
        return -EFBIG;
    if (size < d->size) {
        free_from(fs, d, (size + EFS_BSIZE - 1) / EFS_BSIZE);
        if (size % EFS_BSIZE) {                 /* the rest of the last block reads as zeros */
            uint32_t b = bmap(fs, d, size / EFS_BSIZE, false);
            if (b)
                memset(efs_block(fs, b) + size % EFS_BSIZE, 0, EFS_BSIZE - size % EFS_BSIZE);
        }
    }
    d->size = size;
    d->mtime = fs->now;
    return 0;
}

static void free_inode(struct efs *fs, uint32_t ino)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    free_from(fs, d, 0);
    memset(d, 0, sizeof(*d));
}

/* ---- directories ---- */

int efs_dirent(struct efs *fs, uint32_t dir, uint32_t i, struct efs_dirent *out)
{
    struct efs_dinode *d = efs_inode(fs, dir);
    if ((uint64_t)(i + 1) * DIRENT_SIZE > d->size)
        return 0;
    efs_read(fs, dir, (uint64_t)i * DIRENT_SIZE, out, sizeof(*out));
    out->name[EFS_NAMELEN] = 0;
    return 1;
}

int efs_lookup_in(struct efs *fs, uint32_t dir, const char *name)
{
    if (efs_inode(fs, dir)->type != EFS_T_DIR)
        return -ENOTDIR;
    struct efs_dirent de;
    for (uint32_t i = 0; efs_dirent(fs, dir, i, &de); i++)
        if (de.ino && !strcmp(de.name, name))
            return (int)de.ino;
    return -ENOENT;
}

static int dir_add(struct efs *fs, uint32_t dir, const char *name, uint32_t ino)
{
    struct efs_dirent de, cur;
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    strcpy(de.name, name);
    uint32_t i = 0;
    while (efs_dirent(fs, dir, i, &cur) && cur.ino)
        i++;                                    /* the first free slot, else the end */
    int r = efs_write(fs, dir, (uint64_t)i * DIRENT_SIZE, &de, sizeof(de));
    if (r)
        return r;
    efs_inode(fs, ino)->nlink++;
    return 0;
}

static void dir_remove(struct efs *fs, uint32_t dir, const char *name)
{
    struct efs_dirent de;
    for (uint32_t i = 0; efs_dirent(fs, dir, i, &de); i++) {
        if (de.ino && !strcmp(de.name, name)) {
            uint32_t zero = 0;
            efs_write(fs, dir, (uint64_t)i * DIRENT_SIZE, &zero, sizeof(zero));
            efs_inode(fs, dir)->mtime = fs->now;
            return;
        }
    }
}

static bool dir_empty(struct efs *fs, uint32_t dir)
{
    struct efs_dirent de;
    for (uint32_t i = 0; efs_dirent(fs, dir, i, &de); i++)
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, ".."))
            return false;
    return true;
}

static int check_name(const char *name)
{
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;
    return strlen(name) > EFS_NAMELEN ? -ENAMETOOLONG : 0;
}

/* The directory holding path's last component, and that component. */
static int parent_of(struct efs *fs, const char *path, char name[EFS_NAMELEN + 1])
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    char *slash = strrchr(buf, '/');
    const char *last = slash ? slash + 1 : buf;
    if (strlen(last) > EFS_NAMELEN)
        return -ENAMETOOLONG;
    strcpy(name, last);
    if (!slash)
        return EFS_ROOT_INO;
    *slash = 0;
    int dir = efs_lookup(fs, buf);
    if (dir < 0)
        return dir;
    return efs_inode(fs, dir)->type == EFS_T_DIR ? dir : -ENOTDIR;
}

int efs_lookup(struct efs *fs, const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    uint32_t ino = EFS_ROOT_INO;
    char *save, *tok = strtok_r(buf, "/", &save);
    for (; tok; tok = strtok_r(NULL, "/", &save)) {
        if (strlen(tok) > EFS_NAMELEN)
            return -ENAMETOOLONG;
        int r = efs_lookup_in(fs, ino, tok);
        if (r < 0)
            return r;
        ino = (uint32_t)r;
    }
    return (int)ino;
}

int efs_create(struct efs *fs, const char *path, uint16_t type, uint16_t mode, uint16_t uid,
               uint16_t gid)
{
    char name[EFS_NAMELEN + 1];
    int dir = parent_of(fs, path, name);
    if (dir < 0)
        return dir;
    int r = check_name(name);
    if (r)
        return r;
    if (efs_lookup_in(fs, (uint32_t)dir, name) >= 0)
        return -EEXIST;
    int ino = alloc_inode(fs, type, mode, uid, gid);
    if (ino < 0)
        return ino;
    if (type == EFS_T_DIR &&
        ((r = dir_add(fs, (uint32_t)ino, ".", (uint32_t)ino)) || (r = dir_add(fs, (uint32_t)ino, "..", (uint32_t)dir)))) {
        free_inode(fs, (uint32_t)ino);
        return r;
    }
    if ((r = dir_add(fs, (uint32_t)dir, name, (uint32_t)ino))) {
        if (type == EFS_T_DIR)
            efs_inode(fs, (uint32_t)dir)->nlink--;     /* the ".." we added */
        free_inode(fs, (uint32_t)ino);
        return r;
    }
    return ino;
}

int efs_mkdirs(struct efs *fs, const char *path)
{
    char buf[1024], sofar[1024] = "";
    snprintf(buf, sizeof(buf), "%s", path);
    char *save, *tok = strtok_r(buf, "/", &save);
    int ino = EFS_ROOT_INO;
    for (; tok; tok = strtok_r(NULL, "/", &save)) {
        strcat(sofar, "/");
        strcat(sofar, tok);
        int r = efs_lookup_in(fs, (uint32_t)ino, tok);
        if (r == -ENOENT)
            r = efs_create(fs, sofar, EFS_T_DIR, 0755, 0, 0);
        else if (r >= 0 && efs_inode(fs, (uint32_t)r)->type != EFS_T_DIR)
            r = -ENOTDIR;
        if (r < 0)
            return r;
        ino = r;
    }
    return ino;
}

int efs_link(struct efs *fs, const char *target, const char *path)
{
    int t = efs_lookup(fs, target);
    if (t < 0)
        return t;
    if (efs_inode(fs, (uint32_t)t)->type == EFS_T_DIR)
        return -EPERM;                          /* no hard links to directories */
    char name[EFS_NAMELEN + 1];
    int dir = parent_of(fs, path, name);
    if (dir < 0)
        return dir;
    int r = check_name(name);
    if (r)
        return r;
    if (efs_lookup_in(fs, (uint32_t)dir, name) >= 0)
        return -EEXIST;
    if (efs_inode(fs, (uint32_t)t)->nlink == UINT16_MAX)
        return -EMLINK;
    return dir_add(fs, (uint32_t)dir, name, (uint32_t)t);
}

int efs_unlink(struct efs *fs, const char *path)
{
    char name[EFS_NAMELEN + 1];
    int dir = parent_of(fs, path, name);
    if (dir < 0)
        return dir;
    if (check_name(name))
        return -EINVAL;                         /* /, . and .. */
    int ino = efs_lookup_in(fs, (uint32_t)dir, name);
    if (ino < 0)
        return ino;
    struct efs_dinode *d = efs_inode(fs, (uint32_t)ino);
    if (d->type == EFS_T_DIR) {
        if (!dir_empty(fs, (uint32_t)ino))
            return -ENOTEMPTY;
        dir_remove(fs, (uint32_t)dir, name);
        efs_inode(fs, (uint32_t)dir)->nlink--;  /* its ".." */
        free_inode(fs, (uint32_t)ino);
        return 0;
    }
    dir_remove(fs, (uint32_t)dir, name);
    if (--d->nlink == 0)
        free_inode(fs, (uint32_t)ino);
    else
        d->ctime = fs->now;
    return 0;
}

uint8_t *efs_slurp(struct efs *fs, uint32_t ino, size_t *len)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    uint8_t *buf = malloc(d->size ? (size_t)d->size : 1);
    if (!buf)
        return NULL;
    *len = (size_t)d->size;
    efs_read(fs, ino, 0, buf, *len);
    return buf;
}

int efs_replace(struct efs *fs, uint32_t ino, const void *data, size_t len)
{
    int r = efs_truncate(fs, ino, 0);
    if (!r)
        r = efs_write(fs, ino, 0, data, len);
    if (!r)
        efs_inode(fs, ino)->ctime = fs->now;
    return r;
}

/* ---- volumes ---- */

int efs_format(struct efs *fs, uint8_t *base, uint32_t nblocks, const char *label)
{
    uint32_t ninodes = nblocks / 8 > 1024 ? nblocks / 8 : 1024;   /* one per 32 KiB, >= 1024 */
    uint32_t inode_blocks = (ninodes + EFS_IPB - 1) / EFS_IPB;
    uint32_t bmap_blocks = (nblocks + EFS_BSIZE * 8 - 1) / (EFS_BSIZE * 8);
    if (nblocks < 2 + EFS_LOGSIZE + 1 + inode_blocks + bmap_blocks + 16)
        return -EINVAL;
    memset(base, 0, (size_t)nblocks * EFS_BSIZE);
    fs->base = base;
    fs->nblocks = nblocks;
    fs->now = now_ns();
    fs->replayed = 0;
    struct efs_super *sb = fs->sb = (struct efs_super *)efs_block(fs, 1);
    sb->magic = EFS_MAGIC;
    sb->version = EFS_VERSION;
    sb->bsize = EFS_BSIZE;
    sb->size = nblocks;
    sb->ninodes = ninodes;
    sb->nlog = EFS_LOGSIZE + 1;
    sb->logstart = 2;
    sb->inodestart = sb->logstart + sb->nlog;
    sb->bmapstart = sb->inodestart + inode_blocks;
    sb->datastart = sb->bmapstart + bmap_blocks;
    sb->mkfs_time = fs->now;
    strncpy(sb->label, label ? label : "", sizeof(sb->label));
    for (uint32_t b = 0; b < sb->datastart; b++)
        bit_set(fs, b, true);
    ((struct efs_logheader *)efs_block(fs, sb->logstart))->magic = EFS_LOG_MAGIC;
    int root = alloc_inode(fs, EFS_T_DIR, 0755, 0, 0);
    if (root != EFS_ROOT_INO)
        return -EINVAL;
    dir_add(fs, EFS_ROOT_INO, ".", EFS_ROOT_INO);
    dir_add(fs, EFS_ROOT_INO, "..", EFS_ROOT_INO);
    return 0;
}

int efs_open(struct efs *fs, uint8_t *base, uint32_t nblocks, bool replay)
{
    fs->base = base;
    fs->nblocks = nblocks;
    fs->now = now_ns();
    fs->replayed = 0;
    if (nblocks < 3)
        return -EINVAL;
    struct efs_super *sb = fs->sb = (struct efs_super *)efs_block(fs, 1);
    if (sb->magic != EFS_MAGIC || sb->bsize != EFS_BSIZE || sb->size > nblocks || sb->size < 3 ||
        sb->logstart != 2 || sb->inodestart != sb->logstart + sb->nlog || sb->bmapstart <= sb->inodestart ||
        sb->datastart <= sb->bmapstart || sb->datastart >= sb->size || sb->nlog != EFS_LOGSIZE + 1)
        return -EINVAL;
    struct efs_logheader *lh = (struct efs_logheader *)efs_block(fs, sb->logstart);
    if (replay && lh->magic == EFS_LOG_MAGIC && lh->n > 0 && lh->n <= EFS_LOGSIZE) {
        for (uint32_t i = 0; i < lh->n; i++)    /* recovery: install the committed blocks */
            if (lh->blocks[i] < sb->size)
                memcpy(efs_block(fs, lh->blocks[i]), efs_block(fs, sb->logstart + 1 + i), EFS_BSIZE);
        fs->replayed = lh->n;
        lh->n = 0;
    }
    return 0;
}

void efs_usage(struct efs *fs, struct efs_usage *u)
{
    memset(u, 0, sizeof(*u));
    for (uint32_t ino = EFS_ROOT_INO; ino < fs->sb->ninodes; ino++) {
        uint16_t t = efs_inode(fs, ino)->type;
        if (t == EFS_T_DIR)
            u->dirs++;
        else if (t == EFS_T_FILE)
            u->files++;
        else
            u->free_inodes++;
    }
    u->data_blocks = fs->sb->size - fs->sb->datastart;
    for (uint32_t b = fs->sb->datastart; b < fs->sb->size; b++)
        bit_get(fs, b) ? u->used_blocks++ : u->free_blocks++;
}

/* ---- checking ---- */

struct check {
    struct efs *fs;
    FILE *out;
    int errors;
    uint16_t *refs;             /* directory entries naming each inode */
    uint32_t *owner;            /* the inode owning each block */
};

static void cerr(struct check *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(c->out, "  ERROR: ");
    vfprintf(c->out, fmt, ap);
    fprintf(c->out, "\n");
    va_end(ap);
    c->errors++;
}

static bool claim(struct check *c, uint32_t b, uint32_t ino, const char *what)
{
    struct efs_super *sb = c->fs->sb;
    if (b < sb->datastart || b >= sb->size) {
        cerr(c, "inode %u: %s block %u outside the data area [%u, %u)", ino, what, b, sb->datastart, sb->size);
        return false;
    }
    if (c->owner[b]) {
        cerr(c, "inode %u: %s block %u is also used by inode %u", ino, what, b, c->owner[b]);
        return false;
    }
    c->owner[b] = ino;
    if (!bit_get(c->fs, b))
        cerr(c, "inode %u: block %u in use but free in the bitmap", ino, b);
    return true;
}

static uint64_t check_blocks(struct check *c, uint32_t ino, struct efs_dinode *d)
{
    uint64_t n = 0;
    for (int i = 0; i < EFS_NDIRECT; i++)
        if (d->addrs[i] && claim(c, d->addrs[i], ino, "direct"))
            n++;
    if (d->addrs[EFS_NDIRECT] && claim(c, d->addrs[EFS_NDIRECT], ino, "indirect")) {
        n++;
        uint32_t *ind = (uint32_t *)efs_block(c->fs, d->addrs[EFS_NDIRECT]);
        for (int i = 0; i < EFS_NINDIRECT; i++)
            if (ind[i] && claim(c, ind[i], ino, "data"))
                n++;
    }
    if (d->addrs[EFS_NDIRECT + 1] && claim(c, d->addrs[EFS_NDIRECT + 1], ino, "double-indirect")) {
        n++;
        uint32_t *dbl = (uint32_t *)efs_block(c->fs, d->addrs[EFS_NDIRECT + 1]);
        for (int i = 0; i < EFS_NINDIRECT; i++) {
            if (!dbl[i] || !claim(c, dbl[i], ino, "indirect"))
                continue;
            n++;
            uint32_t *ind = (uint32_t *)efs_block(c->fs, dbl[i]);
            for (int j = 0; j < EFS_NINDIRECT; j++)
                if (ind[j] && claim(c, ind[j], ino, "data"))
                    n++;
        }
    }
    return n;
}

static void check_dir(struct check *c, uint32_t ino, uint32_t parent, const char *path, int depth)
{
    struct efs *fs = c->fs;
    struct efs_dinode *d = efs_inode(fs, ino);
    bool dot = false, dotdot = false;
    if (depth > 64) {
        cerr(c, "%s: directory nesting too deep (a cycle?)", path);
        return;
    }
    if (d->size % DIRENT_SIZE)
        cerr(c, "%s: directory size %llu not a multiple of %u", path, (unsigned long long)d->size,
             DIRENT_SIZE);
    for (uint64_t off = 0; off + DIRENT_SIZE <= d->size; off += DIRENT_SIZE) {
        uint32_t b = bmap(fs, d, off / EFS_BSIZE, false);
        if (!b)
            continue;                           /* a hole reads as empty entries */
        if (b >= fs->sb->size) {
            cerr(c, "%s: directory block %u out of range", path, b);
            return;
        }
        struct efs_dirent *de = (struct efs_dirent *)(efs_block(fs, b) + off % EFS_BSIZE);
        if (!de->ino)
            continue;
        if (memchr(de->name, 0, sizeof(de->name)) == NULL || !de->name[0]) {
            cerr(c, "%s: entry at offset %llu has a bad name", path, (unsigned long long)off);
            continue;
        }
        if (de->ino >= fs->sb->ninodes || efs_inode(fs, de->ino)->type == EFS_T_FREE) {
            cerr(c, "%s/%s: points to %s inode %u", path, de->name,
                 de->ino >= fs->sb->ninodes ? "out-of-range" : "free", de->ino);
            continue;
        }
        c->refs[de->ino]++;
        if (!strcmp(de->name, ".")) {
            dot = true;
            if (de->ino != ino)
                cerr(c, "%s: '.' points to %u, not itself (%u)", path, de->ino, ino);
            continue;
        }
        if (!strcmp(de->name, "..")) {
            dotdot = true;
            if (de->ino != parent)
                cerr(c, "%s: '..' points to %u, not the parent %u", path, de->ino, parent);
            continue;
        }
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", de->name);
        struct efs_dinode *ch = efs_inode(fs, de->ino);
        if (ch->type == EFS_T_DIR) {
            if (c->refs[de->ino] > 1)
                cerr(c, "%s: directory has more than one parent (hard link to a directory?)", sub);
            else
                check_dir(c, de->ino, ino, sub, depth + 1);
        } else if (ch->type != EFS_T_FILE) {
            cerr(c, "%s: inode %u has unknown type %u", sub, de->ino, ch->type);
        }
    }
    if (!dot)
        cerr(c, "%s: missing '.'", path);
    if (!dotdot)
        cerr(c, "%s: missing '..'", path);
}

int efs_check(struct efs *fs, FILE *out)
{
    struct check c = { fs, out, 0, NULL, NULL };
    struct efs_super *sb = fs->sb;
    if (sb->label[0])
        fprintf(out, "  label %.16s\n", sb->label);
    if (fs->replayed)
        fprintf(out, "  note: replayed a committed transaction from the log (%u blocks)\n", fs->replayed);
    struct efs_logheader *lh = (struct efs_logheader *)efs_block(fs, sb->logstart);
    if (lh->magic != EFS_LOG_MAGIC)
        cerr(&c, "log header has bad magic %#x", lh->magic);
    else if (lh->n > EFS_LOGSIZE)
        cerr(&c, "log header claims %u blocks (max %d)", lh->n, EFS_LOGSIZE);
    else if (lh->n)
        fprintf(out, "  note: log holds a committed transaction (seq %llu, %u blocks); "
                     "the kernel replays it at mount\n", (unsigned long long)lh->seq, lh->n);
    c.refs = calloc(sb->ninodes, sizeof(*c.refs));
    c.owner = calloc(sb->size, sizeof(*c.owner));
    if (!c.refs || !c.owner) {
        free(c.refs);
        free(c.owner);
        cerr(&c, "out of memory");
        return c.errors;
    }
    for (uint32_t b = 0; b < sb->datastart; b++)
        if (!bit_get(fs, b))
            cerr(&c, "metadata block %u free in the bitmap", b);
    if (efs_inode(fs, EFS_ROOT_INO)->type != EFS_T_DIR) {
        cerr(&c, "root inode is not a directory");
        goto done;
    }
    c.refs[EFS_ROOT_INO]++;                     /* the root's ".." counts for itself */
    check_dir(&c, EFS_ROOT_INO, EFS_ROOT_INO, "/", 0);
    c.refs[EFS_ROOT_INO]--;
    uint32_t files = 0, dirs = 0;
    uint64_t used = 0;
    for (uint32_t ino = 1; ino < sb->ninodes; ino++) {
        struct efs_dinode *d = efs_inode(fs, ino);
        if (d->type == EFS_T_FREE)
            continue;
        d->type == EFS_T_DIR ? dirs++ : files++;
        used += check_blocks(&c, ino, d);
        if (d->size > EFS_MAXFILE)
            cerr(&c, "inode %u: size %llu exceeds the maximum file size", ino, (unsigned long long)d->size);
        if (!c.refs[ino])
            cerr(&c, "inode %u (%s, %llu bytes, nlink %u) is allocated but not in any directory (orphan)",
                 ino, d->type == EFS_T_DIR ? "dir" : "file", (unsigned long long)d->size, d->nlink);
        else if (d->nlink != c.refs[ino])
            cerr(&c, "inode %u: nlink is %u but %u directory entries refer to it", ino, d->nlink,
                 c.refs[ino]);
    }
    uint32_t leaked = 0;
    for (uint32_t b = sb->datastart; b < sb->size; b++)
        if (bit_get(fs, b) && !c.owner[b])
            leaked++;
    if (leaked)
        cerr(&c, "%u blocks marked used in the bitmap but not owned by any inode (leaked)", leaked);
    fprintf(out, "  %u files, %u directories, %llu data blocks used of %u\n", files, dirs,
            (unsigned long long)used, sb->size - sb->datastart);
done:
    fprintf(out, "  %s: %d error%s\n", c.errors ? "FAILED" : "clean", c.errors, c.errors == 1 ? "" : "s");
    free(c.refs);
    free(c.owner);
    return c.errors;
}

/* ---- mkfs-style specs ---- */

static int spec_fail(const char *spec, const char *what, int err)
{
    fprintf(stderr, "%s: %s: %s\n", spec, what, efs_strerror(err));
    return -1;
}

int efs_apply_spec(struct efs *fs, const char *spec)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *eq = strchr(buf, '=');
    if (!eq) {                                  /* "/dir/[:mode[:uid[:gid]]]" */
        char *colon = strchr(buf, ':');
        if (colon)
            *colon++ = 0;
        int ino = efs_mkdirs(fs, buf);
        if (ino < 0)
            return spec_fail(spec, buf, ino);
        if (colon) {
            unsigned mode = 0755, uid = 0, gid = 0;
            sscanf(colon, "%o:%u:%u", &mode, &uid, &gid);
            struct efs_dinode *d = efs_inode(fs, (uint32_t)ino);
            d->mode = (uint16_t)mode;
            d->uid = (uint16_t)uid;
            d->gid = (uint16_t)gid;
        }
        return 0;
    }
    *eq = 0;
    char *slash = strrchr(buf, '/');
    if (slash && slash != buf) {
        *slash = 0;
        int r = efs_mkdirs(fs, buf);
        *slash = '/';
        if (r < 0)
            return spec_fail(spec, buf, r);
    }
    if (eq[1] == '@') {                         /* "/path=@/existing": a hard link */
        int r = efs_link(fs, eq + 2, buf);
        return r ? spec_fail(spec, buf, r) : 0;
    }
    FILE *f = fopen(eq + 1, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", spec, eq + 1, strerror(errno));
        return -1;
    }
    struct stat hst;                            /* programs 0755, data files 0644, as on the host */
    uint16_t mode = fstat(fileno(f), &hst) == 0 && !(hst.st_mode & 0111) ? 0644 : 0755;
    int ino = efs_create(fs, buf, EFS_T_FILE, mode, 0, 0);
    if (ino < 0) {
        fclose(f);
        return spec_fail(spec, buf, ino);
    }
    uint8_t data[65536];
    size_t n;
    uint64_t off = 0;
    while ((n = fread(data, 1, sizeof(data), f)) > 0) {
        int r = efs_write(fs, (uint32_t)ino, off, data, n);
        if (r) {
            fclose(f);
            return spec_fail(spec, buf, r);
        }
        off += n;
    }
    fclose(f);
    return 0;
}
