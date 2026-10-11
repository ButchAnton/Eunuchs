/*
 * mkfs / fsck for efs (format: kernel/include/efs_format.h). Host tool.
 *
 *   mkfs IMAGE SIZE_MB [/path/in/fs=host-file | /dir/ ...]   create an image
 *   mkfs --check IMAGE                                        check an image
 *
 * The checker validates everything it can derive: superblock layout, the
 * log, directory structure (".", "..", names), link counts, block pointers
 * (range, double use), the free bitmap (leaks and missing marks), and
 * orphaned inodes. Exit status: 0 clean, 1 errors found, 2 usage/I/O error.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "efs_format.h"

static uint8_t *img;
static uint32_t nblocks;
static struct efs_super *sb;

static uint8_t *block(uint32_t b)
{
    return img + (size_t)b * EFS_BSIZE;
}

static struct efs_dinode *dinode(uint32_t ino)
{
    return (struct efs_dinode *)block(sb->inodestart + ino / EFS_IPB) + ino % EFS_IPB;
}

static bool bit_get(uint32_t b)
{
    return block(sb->bmapstart + b / (EFS_BSIZE * 8))[(b % (EFS_BSIZE * 8)) / 8] >> (b % 8) & 1;
}

static void bit_set(uint32_t b)
{
    block(sb->bmapstart + b / (EFS_BSIZE * 8))[(b % (EFS_BSIZE * 8)) / 8] |= 1 << (b % 8);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mkfs: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}

/* ---------------------------------------------------------------- mkfs */

static uint32_t next_block, next_ino = EFS_ROOT_INO;
static uint64_t now_ns;

static uint32_t alloc_block(void)
{
    if (next_block >= nblocks)
        die("image full");
    bit_set(next_block);
    return next_block++;
}

static uint32_t alloc_inode(uint16_t type)
{
    if (next_ino >= sb->ninodes)
        die("out of inodes");
    struct efs_dinode *d = dinode(next_ino);
    memset(d, 0, sizeof(*d));
    d->type = type;
    d->mode = 0755;
    d->mtime = d->ctime = now_ns;
    return next_ino++;
}

/* Block number of file block `bn`, allocating as needed. */
static uint32_t bmap(struct efs_dinode *d, uint32_t bn)
{
    if (bn < EFS_NDIRECT) {
        if (!d->addrs[bn])
            d->addrs[bn] = alloc_block();
        return d->addrs[bn];
    }
    bn -= EFS_NDIRECT;
    if (bn < EFS_NINDIRECT) {
        if (!d->addrs[EFS_NDIRECT])
            d->addrs[EFS_NDIRECT] = alloc_block();
        uint32_t *ind = (uint32_t *)block(d->addrs[EFS_NDIRECT]);
        if (!ind[bn])
            ind[bn] = alloc_block();
        return ind[bn];
    }
    bn -= EFS_NINDIRECT;
    if (!d->addrs[EFS_NDIRECT + 1])
        d->addrs[EFS_NDIRECT + 1] = alloc_block();
    uint32_t *dbl = (uint32_t *)block(d->addrs[EFS_NDIRECT + 1]);
    if (!dbl[bn / EFS_NINDIRECT])
        dbl[bn / EFS_NINDIRECT] = alloc_block();
    uint32_t *ind = (uint32_t *)block(dbl[bn / EFS_NINDIRECT]);
    if (!ind[bn % EFS_NINDIRECT])
        ind[bn % EFS_NINDIRECT] = alloc_block();
    return ind[bn % EFS_NINDIRECT];
}

static void append(uint32_t ino, const void *data, size_t n)
{
    struct efs_dinode *d = dinode(ino);
    const uint8_t *p = data;
    while (n) {
        uint32_t off = d->size % EFS_BSIZE;
        size_t chunk = EFS_BSIZE - off < n ? EFS_BSIZE - off : n;
        memcpy(block(bmap(d, (uint32_t)(d->size / EFS_BSIZE))) + off, p, chunk);
        d->size += chunk;
        p += chunk;
        n -= chunk;
    }
}

static void dir_add(uint32_t dir, const char *name, uint32_t ino)
{
    struct efs_dirent de;
    memset(&de, 0, sizeof(de));
    if (strlen(name) > EFS_NAMELEN)
        die("name too long: %s", name);
    de.ino = ino;
    strcpy(de.name, name);
    append(dir, &de, sizeof(de));
    dinode(ino)->nlink++;
}

static uint32_t dir_find(uint32_t dir, const char *name)
{
    struct efs_dinode *d = dinode(dir);
    for (uint64_t off = 0; off < d->size; off += sizeof(struct efs_dirent)) {
        uint32_t bn = (uint32_t)(off / EFS_BSIZE);
        uint32_t b = bn < EFS_NDIRECT ? d->addrs[bn]
                   : ((uint32_t *)block(d->addrs[EFS_NDIRECT]))[bn - EFS_NDIRECT];
        struct efs_dirent *de = (struct efs_dirent *)(block(b) + off % EFS_BSIZE);
        if (de->ino && !strcmp(de->name, name))
            return de->ino;
    }
    return 0;
}

static uint32_t mkdir_in(uint32_t parent, const char *name)
{
    uint32_t ino = alloc_inode(EFS_T_DIR);
    dir_add(ino, ".", ino);
    dir_add(ino, "..", parent);
    dir_add(parent, name, ino);
    return ino;
}

/* Create the directories of `path` (all but the last component if !last). */
static uint32_t walk_create(const char *path, bool include_last, char *last_out)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    uint32_t dir = EFS_ROOT_INO;
    char *save, *tok = strtok_r(buf, "/", &save);
    while (tok) {
        char *next = strtok_r(NULL, "/", &save);
        if (!next && !include_last) {
            strcpy(last_out, tok);
            return dir;
        }
        uint32_t e = dir_find(dir, tok);
        if (!e)
            e = mkdir_in(dir, tok);
        else if (dinode(e)->type != EFS_T_DIR)
            die("%s: not a directory", tok);
        dir = e;
        tok = next;
    }
    return dir;
}

static int do_mkfs(const char *path, int size_mb, int nspecs, char **specs)
{
    nblocks = (uint32_t)size_mb * (1024 * 1024 / EFS_BSIZE);
    img = calloc(nblocks, EFS_BSIZE);
    if (!img)
        die("out of memory");
    now_ns = (uint64_t)time(NULL) * 1000000000ull;

    sb = (struct efs_super *)block(1);
    sb->magic = EFS_MAGIC;
    sb->version = EFS_VERSION;
    sb->bsize = EFS_BSIZE;
    sb->size = nblocks;
    sb->ninodes = 1024;
    sb->nlog = EFS_LOGSIZE + 1;
    sb->logstart = 2;
    sb->inodestart = sb->logstart + sb->nlog;
    sb->bmapstart = sb->inodestart + (uint32_t)((sb->ninodes + EFS_IPB - 1) / EFS_IPB);
    sb->datastart = sb->bmapstart + (nblocks + EFS_BSIZE * 8 - 1) / (EFS_BSIZE * 8);
    sb->mkfs_time = now_ns;
    for (uint32_t b = 0; b < sb->datastart; b++)
        bit_set(b);
    next_block = sb->datastart;
    ((struct efs_logheader *)block(sb->logstart))->magic = EFS_LOG_MAGIC;

    uint32_t root = alloc_inode(EFS_T_DIR);
    dir_add(root, ".", root);
    dir_add(root, "..", root);

    for (int i = 0; i < nspecs; i++) {
        char *eq = strchr(specs[i], '=');
        if (!eq) {                              /* "/dir/[:mode[:uid[:gid]]]" */
            char *colon = strchr(specs[i], ':');
            if (colon)
                *colon++ = 0;
            uint32_t ino = walk_create(specs[i], true, NULL);
            if (colon) {
                unsigned mode = 0755, uid = 0, gid = 0;
                sscanf(colon, "%o:%u:%u", &mode, &uid, &gid);
                dinode(ino)->mode = (uint16_t)mode;
                dinode(ino)->uid = (uint16_t)uid;
                dinode(ino)->gid = (uint16_t)gid;
            }
            continue;
        }
        *eq = 0;
        char name[EFS_NAMELEN + 1];
        uint32_t dir = walk_create(specs[i], false, name);
        if (eq[1] == '@') {                     /* "/path=@/existing": hard link */
            char tname[EFS_NAMELEN + 1];
            uint32_t tdir = walk_create(eq + 2, false, tname);
            uint32_t target = dir_find(tdir, tname);
            if (!target)
                die("%s: link target %s not found (list it first)", specs[i], eq + 2);
            dir_add(dir, name, target);
            continue;
        }
        FILE *f = fopen(eq + 1, "rb");
        if (!f)
            die("%s: %s", eq + 1, strerror(errno));
        uint32_t ino = alloc_inode(EFS_T_FILE);
        struct stat hst;                        /* programs 0755, data files 0644, as on the host */
        if (fstat(fileno(f), &hst) == 0 && !(hst.st_mode & 0111))
            dinode(ino)->mode = 0644;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            append(ino, buf, n);
        fclose(f);
        dir_add(dir, name, ino);
    }

    FILE *out = fopen(path, "wb");
    if (!out || fwrite(img, EFS_BSIZE, nblocks, out) != nblocks || fclose(out))
        die("%s: write failed", path);
    printf("mkfs: %s: %u blocks (%d MiB), %u inodes used, %u data blocks used\n", path, nblocks,
           size_mb, next_ino - 1, next_block - sb->datastart);
    return 0;
}

/* ---------------------------------------------------------------- fsck */

static int errors;
static uint16_t *refs;          /* link references found per inode */
static uint32_t *owner;         /* inode owning each block (0 = none) */

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ERROR: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    errors++;
}

static bool claim(uint32_t b, uint32_t ino, const char *what)
{
    if (b < sb->datastart || b >= sb->size) {
        err("inode %u: %s block %u outside the data area [%u, %u)", ino, what, b,
            sb->datastart, sb->size);
        return false;
    }
    if (owner[b]) {
        err("inode %u: %s block %u is also used by inode %u", ino, what, b, owner[b]);
        return false;
    }
    owner[b] = ino;
    if (!bit_get(b))
        err("inode %u: block %u in use but free in the bitmap", ino, b);
    return true;
}

/* Visit the blocks of an inode; returns the number of blocks (incl. indirect). */
static uint64_t check_blocks(uint32_t ino, struct efs_dinode *d)
{
    uint64_t n = 0;
    for (int i = 0; i < EFS_NDIRECT; i++)
        if (d->addrs[i] && claim(d->addrs[i], ino, "direct"))
            n++;
    if (d->addrs[EFS_NDIRECT] && claim(d->addrs[EFS_NDIRECT], ino, "indirect")) {
        n++;
        uint32_t *ind = (uint32_t *)block(d->addrs[EFS_NDIRECT]);
        for (int i = 0; i < EFS_NINDIRECT; i++)
            if (ind[i] && claim(ind[i], ino, "data"))
                n++;
    }
    if (d->addrs[EFS_NDIRECT + 1] && claim(d->addrs[EFS_NDIRECT + 1], ino, "double-indirect")) {
        n++;
        uint32_t *dbl = (uint32_t *)block(d->addrs[EFS_NDIRECT + 1]);
        for (int i = 0; i < EFS_NINDIRECT; i++) {
            if (!dbl[i] || !claim(dbl[i], ino, "indirect"))
                continue;
            n++;
            uint32_t *ind = (uint32_t *)block(dbl[i]);
            for (int j = 0; j < EFS_NINDIRECT; j++)
                if (ind[j] && claim(ind[j], ino, "data"))
                    n++;
        }
    }
    return n;
}

static uint32_t file_block(struct efs_dinode *d, uint64_t bn)
{
    if (bn < EFS_NDIRECT)
        return d->addrs[bn];
    bn -= EFS_NDIRECT;
    if (bn < EFS_NINDIRECT)
        return d->addrs[EFS_NDIRECT] ? ((uint32_t *)block(d->addrs[EFS_NDIRECT]))[bn] : 0;
    bn -= EFS_NINDIRECT;
    if (!d->addrs[EFS_NDIRECT + 1])
        return 0;
    uint32_t ind = ((uint32_t *)block(d->addrs[EFS_NDIRECT + 1]))[bn / EFS_NINDIRECT];
    return ind ? ((uint32_t *)block(ind))[bn % EFS_NINDIRECT] : 0;
}

static void check_dir(uint32_t ino, uint32_t parent, const char *path, int depth)
{
    struct efs_dinode *d = dinode(ino);
    bool dot = false, dotdot = false;

    if (depth > 64) {
        err("%s: directory nesting too deep (a cycle?)", path);
        return;
    }
    if (d->size % sizeof(struct efs_dirent))
        err("%s: directory size %llu not a multiple of %zu", path,
            (unsigned long long)d->size, sizeof(struct efs_dirent));
    for (uint64_t off = 0; off + sizeof(struct efs_dirent) <= d->size;
         off += sizeof(struct efs_dirent)) {
        uint32_t b = file_block(d, off / EFS_BSIZE);
        if (!b)
            continue;                           /* hole: reads as empty entries */
        if (b >= sb->size) {
            err("%s: directory block %u out of range", path, b);
            return;
        }
        struct efs_dirent *de = (struct efs_dirent *)(block(b) + off % EFS_BSIZE);
        if (!de->ino)
            continue;
        if (memchr(de->name, 0, sizeof(de->name)) == NULL || !de->name[0]) {
            err("%s: entry at offset %llu has a bad name", path, (unsigned long long)off);
            continue;
        }
        if (de->ino >= sb->ninodes || dinode(de->ino)->type == EFS_T_FREE) {
            err("%s/%s: points to %s inode %u", path, de->name,
                de->ino >= sb->ninodes ? "out-of-range" : "free", de->ino);
            continue;
        }
        refs[de->ino]++;
        if (!strcmp(de->name, ".")) {
            dot = true;
            if (de->ino != ino)
                err("%s: '.' points to %u, not itself (%u)", path, de->ino, ino);
            continue;
        }
        if (!strcmp(de->name, "..")) {
            dotdot = true;
            if (de->ino != parent)
                err("%s: '..' points to %u, not the parent %u", path, de->ino, parent);
            continue;
        }
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", de->name);
        struct efs_dinode *c = dinode(de->ino);
        if (c->type == EFS_T_DIR) {
            if (refs[de->ino] > 1)
                err("%s: directory has more than one parent (hard link to a directory?)", sub);
            else
                check_dir(de->ino, ino, sub, depth + 1);
        } else if (c->type != EFS_T_FILE) {
            err("%s: inode %u has unknown type %u", sub, de->ino, c->type);
        }
    }
    if (!dot)
        err("%s: missing '.'", path);
    if (!dotdot)
        err("%s: missing '..'", path);
}

static int do_check(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    struct stat st;
    stat(path, &st);
    nblocks = (uint32_t)(st.st_size / EFS_BSIZE);
    img = malloc((size_t)st.st_size);
    if (!img || fread(img, 1, (size_t)st.st_size, f) != (size_t)st.st_size)
        die("%s: read failed", path);
    fclose(f);
    sb = (struct efs_super *)block(1);

    printf("fsck %s:\n", path);
    if (sb->magic != EFS_MAGIC)
        die("not an efs image (magic %#x)", sb->magic);
    if (sb->bsize != EFS_BSIZE || sb->size > nblocks || sb->logstart != 2 ||
        sb->inodestart != sb->logstart + sb->nlog || sb->bmapstart <= sb->inodestart ||
        sb->datastart <= sb->bmapstart || sb->datastart >= sb->size) {
        err("superblock layout is inconsistent (size %u log %u+%u inodes %u bmap %u data %u)",
            sb->size, sb->logstart, sb->nlog, sb->inodestart, sb->bmapstart, sb->datastart);
        return 1;
    }

    struct efs_logheader *lh = (struct efs_logheader *)block(sb->logstart);
    if (lh->magic != EFS_LOG_MAGIC)
        err("log header has bad magic %#x", lh->magic);
    else if (lh->n > EFS_LOGSIZE)
        err("log header claims %u blocks (max %d)", lh->n, EFS_LOGSIZE);
    else if (lh->n)
        printf("  note: log holds a committed transaction (seq %llu, %u blocks); "
               "the kernel replays it at mount\n", (unsigned long long)lh->seq, lh->n);

    refs = calloc(sb->ninodes, sizeof(*refs));
    owner = calloc(sb->size, sizeof(*owner));
    for (uint32_t b = 0; b < sb->datastart; b++)
        if (!bit_get(b))
            err("metadata block %u free in the bitmap", b);

    if (dinode(EFS_ROOT_INO)->type != EFS_T_DIR) {
        err("root inode is not a directory");
        return 1;
    }
    refs[EFS_ROOT_INO]++;           /* the root's ".." counts for itself */
    check_dir(EFS_ROOT_INO, EFS_ROOT_INO, "/", 0);
    refs[EFS_ROOT_INO]--;

    uint32_t files = 0, dirs = 0;
    uint64_t used = 0;
    for (uint32_t ino = 1; ino < sb->ninodes; ino++) {
        struct efs_dinode *d = dinode(ino);
        if (d->type == EFS_T_FREE)
            continue;
        d->type == EFS_T_DIR ? dirs++ : files++;
        uint64_t nb = check_blocks(ino, d);
        used += nb;
        if (d->size > EFS_MAXFILE)
            err("inode %u: size %llu exceeds the maximum file size", ino,
                (unsigned long long)d->size);
        (void)nb;
        if (!refs[ino])
            err("inode %u (%s, %llu bytes, nlink %u) is allocated but not in any directory "
                "(orphan)", ino, d->type == EFS_T_DIR ? "dir" : "file",
                (unsigned long long)d->size, d->nlink);
        else if (d->nlink != refs[ino])
            err("inode %u: nlink is %u but %u directory entries refer to it", ino, d->nlink,
                refs[ino]);
    }
    uint32_t leaked = 0;
    for (uint32_t b = sb->datastart; b < sb->size; b++)
        if (bit_get(b) && !owner[b])
            leaked++;
    if (leaked)
        err("%u blocks marked used in the bitmap but not owned by any inode (leaked)", leaked);

    printf("  %u files, %u directories, %llu data blocks used of %u\n", files, dirs,
           (unsigned long long)used, sb->size - sb->datastart);
    printf("  %s: %d error%s\n", errors ? "FAILED" : "clean", errors, errors == 1 ? "" : "s");
    return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--check"))
        return do_check(argv[2]);
    if (argc >= 3)
        return do_mkfs(argv[1], atoi(argv[2]), argc - 3, argv + 3);
    fprintf(stderr, "usage: mkfs IMAGE SIZE_MB [/path=hostfile | /path=@/existing | /dir/]...\n"
                    "       mkfs --check IMAGE\n");
    return 2;
}
