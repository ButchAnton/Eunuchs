/*
 * 9pfs: a host directory, through 9P2000.L over virtio (virtio_9p.c).
 *
 * Mounted at /mnt/<tag> for every 9P device whose mount point exists (with
 * QEMU's -fsdev local,path=DIR,security_model=none -device
 * virtio-9p-device,fsdev=...,mount_tag=TAG). The server is the host: names,
 * permissions and times are the host's, and other programs on the host can
 * change the files at any time.
 *
 * Inodes. 9P names a file by its qid.path (64 bits); the VFS by a 32-bit
 * inode number. Each qid.path seen gets a node with a small number (the
 * root is 1), kept for the life of the mount. A node holds:
 *   - fid: a fid walked to the file, for getattr/setattr and as the base of
 *     walks (0 until the file has been looked up);
 *   - ofid: a fid opened for I/O, made on the first read or write (read-write
 *     if the host allows it, else read-only); a file created here starts
 *     with one;
 *   - dfid and a buffer: a directory opened for Treaddir.
 * A node's fids are only changed with its inode locked (the VFS holds the
 * lock around lookup, read, write and readdir); the node table itself is
 * under a spinlock.
 *
 * Nothing is cached but the attributes in the inode, and the VFS refreshes
 * those (Tgetattr) at every stat and open, so changes made on the host show
 * up. Every path component is a Twalk.
 */
#define LOG_SUBSYS SUBSYS_FS
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <spinlock.h>
#include <uapi/errno.h>
#include <virtio.h>

/* message types (9P2000.L) */
enum {
    Tlerror = 6, Rlerror,
    Tstatfs = 8, Rstatfs,
    Tlopen = 12, Rlopen,
    Tlcreate = 14, Rlcreate,
    Tgetattr = 24, Rgetattr,
    Tsetattr = 26, Rsetattr,
    Treaddir = 40, Rreaddir,
    Tfsync = 50, Rfsync,
    Tlink = 70, Rlink,
    Tmkdir = 72, Rmkdir,
    Trenameat = 74, Rrenameat,
    Tunlinkat = 76, Runlinkat,
    Tversion = 100, Rversion,
    Tattach = 104, Rattach,
    Twalk = 110, Rwalk,
    Tread = 116, Rread,
    Twrite = 118, Rwrite,
    Tclunk = 120, Rclunk,
};

#define NOTAG 0xffff
#define NOFID 0xffffffffu
#define P9_MSIZE 65536
#define P9_IOCHUNK 32768        /* bytes per Tread/Twrite (kmalloc'd buffers) */
#define P9_DIRBUF 8192

/* Linux open flags, as 9P2000.L carries them */
#define L_O_RDONLY 0
#define L_O_WRONLY 1
#define L_O_RDWR 2
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_DIRECTORY 0200000

/* Tgetattr / Tsetattr masks */
#define GETATTR_BASIC 0x7ffULL
#define SETATTR_MODE 0x1
#define SETATTR_UID 0x2
#define SETATTR_GID 0x4
#define SETATTR_SIZE 0x8
#define SETATTR_MTIME 0x20
#define SETATTR_MTIME_SET 0x100

#define S_IFMT_ 0170000
#define S_IFDIR_ 0040000

struct node {
    u64 qpath;
    u32 ino;
    u32 fid, ofid, dfid;
    bool writable;              /* ofid was opened read-write */
    u8 *dbuf;                   /* the last Treaddir reply's entries */
    u32 dlen;
    u64 dstart;                 /* the offset that batch was read from */
    u16 mode, uid, gid;         /* as last sent or read, to send only changes */
    u64 mtime;
    struct node *hnext;         /* qpath hash chain */
};

#define NHASH 256

struct p9fs {
    int dev;
    u32 msize;
    spinlock_t lock;            /* nodes, by_ino, hash, next_fid */
    struct node **by_ino;
    u32 nnodes, cap;
    struct node *hash[NHASH];
    u32 next_fid;
    u64 rpcs, errors;           /* requests; transport or protocol failures */
    u64 lerrors;                /* error replies (lookups of missing names make most) */
};

static struct p9fs *fsof(struct super_block *sb) { return sb->priv; }

/* ---- messages ---- */

struct msg {
    u8 *b;
    u32 len, cap;
    bool bad;
};

static void put(struct msg *m, u64 v, int n)
{
    if (m->len + n > m->cap) {
        m->bad = true;
        return;
    }
    for (int i = 0; i < n; i++)
        m->b[m->len++] = (u8)(v >> (8 * i));
}

static void put_str(struct msg *m, const char *s)
{
    size_t n = strlen(s);
    put(m, n, 2);
    if (m->len + n > m->cap) {
        m->bad = true;
        return;
    }
    memcpy(m->b + m->len, s, n);
    m->len += n;
}

static bool start(struct msg *m, u32 cap, u8 type)
{
    m->b = kmalloc(cap);
    m->cap = cap;
    m->len = 0;
    m->bad = !m->b;
    if (m->bad)
        return false;
    put(m, 0, 4);               /* size, filled in by rpc */
    put(m, type, 1);
    put(m, NOTAG, 2);
    return true;
}

struct reply {
    u8 *b;
    u32 len, pos;
};

static u64 get(struct reply *r, int n)
{
    u64 v = 0;
    if (r->pos + n > r->len) {
        r->pos = r->len + 1;    /* mark it short */
        return 0;
    }
    for (int i = 0; i < n; i++)
        v |= (u64)r->b[r->pos + i] << (8 * i);
    r->pos += n;
    return v;
}

static u64 get_qid(struct reply *r)
{
    get(r, 1);                  /* type */
    get(r, 4);                  /* version */
    return get(r, 8);           /* path */
}

static u16 tags;

/*
 * Send m (which this frees) and receive the reply into a buffer of rxcap
 * bytes (freed with kfree(r->b) by the caller on success). Returns 0, or
 * -errno: the server's Rlerror code, or a transport error.
 */
static int rpc(struct super_block *sb, struct msg *m, u32 rxcap, struct reply *r)
{
    struct p9fs *fs = fsof(sb);
    r->b = NULL;
    if (m->bad) {
        kfree(m->b);
        return -ENOMEM;
    }
    u8 type = m->b[4];
    u16 tag = type == Tversion ? NOTAG : (u16)(__atomic_add_fetch(&tags, 1, __ATOMIC_RELAXED) % 0xfffe);
    m->b[0] = (u8)m->len;
    m->b[1] = (u8)(m->len >> 8);
    m->b[2] = (u8)(m->len >> 16);
    m->b[3] = (u8)(m->len >> 24);
    m->b[5] = (u8)tag;
    m->b[6] = (u8)(tag >> 8);
    u8 *rx = kmalloc(rxcap);
    if (!rx) {
        kfree(m->b);
        return -ENOMEM;
    }
    long n = p9_transact(fs->dev, m->b, m->len, rx, rxcap);
    kfree(m->b);
    fs->rpcs++;
    if (n < 7) {
        kfree(rx);
        fs->errors++;
        return n < 0 ? (int)n : -EIO;
    }
    r->b = rx;
    r->len = (u32)n;
    r->pos = 7;
    if (rx[4] == Rlerror) {
        int e = (int)get(r, 4);
        kfree(rx);
        r->b = NULL;
        fs->lerrors++;
        return e > 0 ? -e : -EIO;
    }
    if (rx[4] != type + 1) {
        pr_warn("9p: reply type %u to request type %u\n", rx[4], type);
        kfree(rx);
        r->b = NULL;
        fs->errors++;
        return -EIO;
    }
    return 0;
}

/* A request whose reply carries nothing we need. */
static int rpc_simple(struct super_block *sb, struct msg *m)
{
    struct reply r;
    int e = rpc(sb, m, 64, &r);
    kfree(r.b);
    return e;
}

/* ---- nodes and fids ---- */

static u32 new_fid(struct p9fs *fs)
{
    unsigned long flags = spin_lock_irqsave(&fs->lock);
    u32 f = fs->next_fid++;
    if (fs->next_fid == NOFID)
        fs->next_fid = 2;
    spin_unlock_irqrestore(&fs->lock, flags);
    return f;
}

static void clunk(struct super_block *sb, u32 fid)
{
    struct msg m;
    if (!fid || !start(&m, 32, Tclunk))
        return;
    put(&m, fid, 4);
    rpc_simple(sb, &m);
}

static struct node *node_by_ino(struct super_block *sb, u32 ino)
{
    struct p9fs *fs = fsof(sb);
    unsigned long flags = spin_lock_irqsave(&fs->lock);
    struct node *n = ino && ino <= fs->nnodes ? fs->by_ino[ino] : NULL;
    spin_unlock_irqrestore(&fs->lock, flags);
    return n;
}

/* A directory changed: its buffered Treaddir batch is out of date. */
static void dir_changed(struct inode *dir)
{
    struct node *dn = node_by_ino(dir->sb, dir->ino);
    if (dn) {
        kfree(dn->dbuf);
        dn->dbuf = NULL;
    }
}

/* The node for a qid.path, made if new. Its fid is not touched. */
static struct node *node_for(struct super_block *sb, u64 qpath)
{
    struct p9fs *fs = fsof(sb);
    unsigned h = (unsigned)(qpath ^ (qpath >> 17)) % NHASH;
    struct node *fresh = kzalloc(sizeof(*fresh));
    unsigned long flags = spin_lock_irqsave(&fs->lock);
    for (struct node *n = fs->hash[h]; n; n = n->hnext)
        if (n->qpath == qpath) {
            spin_unlock_irqrestore(&fs->lock, flags);
            kfree(fresh);
            return n;
        }
    if (!fresh) {
        spin_unlock_irqrestore(&fs->lock, flags);
        return NULL;
    }
    if (fs->nnodes + 1 >= fs->cap) {
        /* grow by_ino: allocate outside the lock and retry */
        u32 cap = fs->cap ? fs->cap * 2 : 1024;
        spin_unlock_irqrestore(&fs->lock, flags);
        kfree(fresh);
        struct node **v = kzalloc(cap * sizeof(*v));
        if (!v)
            return NULL;
        flags = spin_lock_irqsave(&fs->lock);
        if (cap > fs->cap) {
            if (fs->by_ino)
                memcpy(v, fs->by_ino, fs->cap * sizeof(*v));
            struct node **old = fs->by_ino;
            fs->by_ino = v;
            fs->cap = cap;
            spin_unlock_irqrestore(&fs->lock, flags);
            kfree(old);
        } else {
            spin_unlock_irqrestore(&fs->lock, flags);
            kfree(v);
        }
        return node_for(sb, qpath);
    }
    fresh->qpath = qpath;
    fresh->ino = ++fs->nnodes;
    fs->by_ino[fresh->ino] = fresh;
    fresh->hnext = fs->hash[h];
    fs->hash[h] = fresh;
    spin_unlock_irqrestore(&fs->lock, flags);
    return fresh;
}

/* Walk from fid by one name (or none: a clone) to a new fid; its qid.path in *qpath. */
static int walk(struct super_block *sb, u32 fid, const char *name, u32 *newfid, u64 *qpath)
{
    struct p9fs *fs = fsof(sb);
    struct msg m;
    if (!start(&m, 32 + (name ? strlen(name) : 0), Twalk))
        return -ENOMEM;
    u32 nf = new_fid(fs);
    put(&m, fid, 4);
    put(&m, nf, 4);
    put(&m, name ? 1 : 0, 2);
    if (name)
        put_str(&m, name);
    struct reply r;
    int e = rpc(sb, &m, 64 + 13, &r);
    if (e)
        return e;
    u16 nq = (u16)get(&r, 2);
    u64 qp = nq ? get_qid(&r) : 0;
    kfree(r.b);
    if (name && nq != 1) {
        /* a partial walk makes no fid */
        return -ENOENT;
    }
    *newfid = nf;
    if (qpath)
        *qpath = qp;
    return 0;
}

/* Fill ip from Tgetattr on the node's fid. */
static int getattr(struct inode *ip, struct node *n)
{
    u32 fid = n->fid ? n->fid : n->ofid;
    if (!fid)
        return -ESTALE;
    struct msg m;
    if (!start(&m, 32, Tgetattr))
        return -ENOMEM;
    put(&m, fid, 4);
    put(&m, GETATTR_BASIC, 8);
    struct reply r;
    int e = rpc(ip->sb, &m, 256, &r);
    if (e)
        return e;
    get(&r, 8);                 /* valid */
    get_qid(&r);
    u32 mode = (u32)get(&r, 4);
    u32 uid = (u32)get(&r, 4), gid = (u32)get(&r, 4);
    u64 nlink = get(&r, 8);
    get(&r, 8);                 /* rdev */
    u64 size = get(&r, 8);
    get(&r, 8);                 /* blksize */
    get(&r, 8);                 /* blocks */
    get(&r, 8); get(&r, 8);     /* atime */
    u64 msec = get(&r, 8), mnsec = get(&r, 8);
    u64 csec = get(&r, 8), cnsec = get(&r, 8);
    bool short_reply = r.pos > r.len;
    kfree(r.b);
    if (short_reply)
        return -EIO;
    ip->type = (mode & S_IFMT_) == S_IFDIR_ ? I_DIR : I_FILE;
    ip->mode = mode & 07777;
    ip->uid = (u16)uid;
    ip->gid = (u16)gid;
    ip->nlink = nlink > 0xffff ? 0xffff : (u16)nlink;
    ip->size = size;
    ip->mtime = msec * 1000000000ULL + mnsec;
    ip->ctime = csec * 1000000000ULL + cnsec;
    ip->rdev = 0;
    n->mode = ip->mode;
    n->uid = ip->uid;
    n->gid = ip->gid;
    n->mtime = ip->mtime;
    return 0;
}

/* Make sure the node has an fid opened for I/O (and a writable one if asked). */
static int open_io(struct inode *ip, struct node *n, bool for_write)
{
    if (n->ofid && (n->writable || !for_write))
        return 0;
    if (!n->fid)
        return -ESTALE;
    struct super_block *sb = ip->sb;
    for (int attempt = 0; attempt < 2; attempt++) {
        u32 flags = attempt == 0 ? L_O_RDWR : L_O_RDONLY;
        if (attempt == 1 && for_write)
            return -EACCES;
        u32 nf;
        int e = walk(sb, n->fid, NULL, &nf, NULL);
        if (e)
            return e;
        struct msg m;
        if (!start(&m, 32, Tlopen)) {
            clunk(sb, nf);
            return -ENOMEM;
        }
        put(&m, nf, 4);
        put(&m, flags, 4);
        e = rpc_simple(sb, &m);
        if (e) {
            clunk(sb, nf);
            if (e == -EACCES || e == -EPERM || e == -EROFS)
                continue;
            return e;
        }
        if (n->ofid)
            clunk(sb, n->ofid);
        n->ofid = nf;
        n->writable = flags == L_O_RDWR;
        return 0;
    }
    return -EACCES;
}

/* ---- fs_ops ---- */

static int p9_read_inode(struct inode *ip)
{
    struct node *n = node_by_ino(ip->sb, ip->ino);
    if (!n)
        return -ESTALE;
    return getattr(ip, n);
}

static int p9_refresh(struct inode *ip)
{
    return p9_read_inode(ip);
}

static int p9_write_inode(struct inode *ip)
{
    struct node *n = node_by_ino(ip->sb, ip->ino);
    if (!n || !(n->fid || n->ofid))
        return -ESTALE;
    u32 valid = 0;
    if (ip->mode != n->mode)
        valid |= SETATTR_MODE;
    if (ip->uid != n->uid)
        valid |= SETATTR_UID;
    if (ip->gid != n->gid)
        valid |= SETATTR_GID;
    if (ip->mtime != n->mtime)
        valid |= SETATTR_MTIME | SETATTR_MTIME_SET;
    if (!valid)
        return 0;
    struct msg m;
    if (!start(&m, 80, Tsetattr))
        return -ENOMEM;
    put(&m, n->fid ? n->fid : n->ofid, 4);
    put(&m, valid, 4);
    put(&m, ip->mode, 4);
    put(&m, ip->uid, 4);
    put(&m, ip->gid, 4);
    put(&m, 0, 8);              /* size */
    put(&m, 0, 8);              /* atime */
    put(&m, 0, 8);
    put(&m, ip->mtime / 1000000000ULL, 8);
    put(&m, ip->mtime % 1000000000ULL, 8);
    int e = rpc_simple(ip->sb, &m);
    if (!e) {
        n->mode = ip->mode;
        n->uid = ip->uid;
        n->gid = ip->gid;
        n->mtime = ip->mtime;
    }
    return e;
}

static int p9_truncate(struct inode *ip, u64 size)
{
    struct node *n = node_by_ino(ip->sb, ip->ino);
    if (!n || !(n->fid || n->ofid))
        return -ESTALE;
    struct msg m;
    if (!start(&m, 80, Tsetattr))
        return -ENOMEM;
    put(&m, n->fid ? n->fid : n->ofid, 4);
    put(&m, SETATTR_SIZE, 4);
    put(&m, 0, 4);
    put(&m, 0, 4);
    put(&m, 0, 4);
    put(&m, size, 8);
    for (int i = 0; i < 4; i++)
        put(&m, 0, 8);
    int e = rpc_simple(ip->sb, &m);
    if (!e)
        ip->size = size;
    return e;
}

static long p9_read(struct inode *ip, void *dst, u64 off, size_t n, bool user)
{
    struct node *nd = node_by_ino(ip->sb, ip->ino);
    if (!nd)
        return -ESTALE;
    int e = open_io(ip, nd, false);
    if (e)
        return e;
    size_t done = 0;
    while (done < n) {
        u32 want = (u32)MIN(n - done, (size_t)P9_IOCHUNK);
        struct msg m;
        if (!start(&m, 32, Tread))
            return done ? (long)done : -ENOMEM;
        put(&m, nd->ofid, 4);
        put(&m, off + done, 8);
        put(&m, want, 4);
        struct reply r;
        e = rpc(ip->sb, &m, 11 + want, &r);
        if (e)
            return done ? (long)done : e;
        u32 got = (u32)get(&r, 4);
        if (got > want || r.pos + got > r.len) {
            kfree(r.b);
            return done ? (long)done : -EIO;
        }
        if (user) {
            if (copy_to_user((uintptr_t)dst + done, r.b + r.pos, got)) {
                kfree(r.b);
                return done ? (long)done : -EFAULT;
            }
        } else {
            memcpy((u8 *)dst + done, r.b + r.pos, got);
        }
        kfree(r.b);
        done += got;
        if (got < want)
            break;
    }
    return (long)done;
}

static long p9_write(struct inode *ip, const void *src, u64 off, size_t n, bool user)
{
    struct node *nd = node_by_ino(ip->sb, ip->ino);
    if (!nd)
        return -ESTALE;
    int e = open_io(ip, nd, true);
    if (e)
        return e;
    size_t done = 0;
    while (done < n) {
        u32 want = (u32)MIN(n - done, (size_t)P9_IOCHUNK);
        struct msg m;
        if (!start(&m, 32 + want, Twrite))
            return done ? (long)done : -ENOMEM;
        put(&m, nd->ofid, 4);
        put(&m, off + done, 8);
        put(&m, want, 4);
        if (user) {
            if (copy_from_user(m.b + m.len, (uintptr_t)src + done, want)) {
                kfree(m.b);
                return done ? (long)done : -EFAULT;
            }
        } else {
            memcpy(m.b + m.len, (const u8 *)src + done, want);
        }
        m.len += want;
        struct reply r;
        e = rpc(ip->sb, &m, 64, &r);
        if (e)
            return done ? (long)done : e;
        u32 got = (u32)get(&r, 4);
        kfree(r.b);
        done += got;
        if (got < want)
            break;
    }
    if (off + done > ip->size)
        ip->size = off + done;
    return (long)done;
}

static u32 p9_lookup(struct inode *dir, const char *name)
{
    struct super_block *sb = dir->sb;
    struct node *dn = node_by_ino(sb, dir->ino);
    if (!dn || !dn->fid)
        return 0;
    u32 nf;
    u64 qpath;
    if (walk(sb, dn->fid, name, &nf, &qpath))
        return 0;
    struct node *n = node_for(sb, qpath);
    if (!n) {
        clunk(sb, nf);
        return 0;
    }
    if (n->fid) {
        clunk(sb, nf);          /* we have one already */
    } else {
        n->fid = nf;
    }
    return n->ino;
}

static int p9_link(struct inode *dir, const char *name, u32 ino)
{
    struct node *dn = node_by_ino(dir->sb, dir->ino), *n = node_by_ino(dir->sb, ino);
    if (!dn || !n || !dn->fid || !n->fid)
        return -ESTALE;
    struct msg m;
    if (!start(&m, 32 + strlen(name), Tlink))
        return -ENOMEM;
    put(&m, dn->fid, 4);
    put(&m, n->fid, 4);
    put_str(&m, name);
    int e = rpc_simple(dir->sb, &m);
    if (!e)
        dir_changed(dir);
    return e;
}

static int p9_readdir(struct inode *dir, u64 *off, struct kdirent *out)
{
    struct super_block *sb = dir->sb;
    struct node *dn = node_by_ino(sb, dir->ino);
    if (!dn || !dn->fid)
        return 0;
    if (!dn->dfid) {
        u32 nf;
        if (walk(sb, dn->fid, NULL, &nf, NULL))
            return 0;
        struct msg m;
        if (!start(&m, 32, Tlopen)) {
            clunk(sb, nf);
            return 0;
        }
        put(&m, nf, 4);
        put(&m, L_O_RDONLY | L_O_DIRECTORY, 4);
        if (rpc_simple(sb, &m)) {
            clunk(sb, nf);
            return 0;
        }
        dn->dfid = nf;
    }
    if (*off == 0) {
        /* a new listing: never from an old batch (the directory may have changed) */
        kfree(dn->dbuf);
        dn->dbuf = NULL;
    }
    for (int tries = 0; tries < 2; tries++) {
        /* the entry at *off: in the buffered batch, or read a new batch from there */
        if (dn->dbuf) {
            struct reply r = { dn->dbuf, dn->dlen, 0 };
            u64 cur = dn->dstart;
            while (r.pos < r.len) {
                u64 qpath = get_qid(&r);
                u64 next = get(&r, 8);
                u8 type = (u8)get(&r, 1);
                u16 len = (u16)get(&r, 2);
                if (r.pos + len > r.len)
                    break;
                const char *name = (const char *)r.b + r.pos;
                r.pos += len;
                if (cur == *off) {
                    if (len > EFS_NAMELEN) {
                        cur = next;     /* a name we can't hold: skip it */
                        *off = next;
                        continue;
                    }
                    struct node *n = node_for(sb, qpath);
                    out->ino = n ? n->ino : 0;
                    out->type = type;
                    memcpy(out->name, name, len);
                    out->name[len] = 0;
                    *off = next;
                    return 1;
                }
                cur = next;
            }
        }
        /* not buffered: fetch the batch that starts at *off */
        kfree(dn->dbuf);
        dn->dbuf = NULL;
        struct msg m;
        if (!start(&m, 32, Treaddir))
            return 0;
        put(&m, dn->dfid, 4);
        put(&m, *off, 8);
        put(&m, P9_DIRBUF, 4);
        struct reply r;
        if (rpc(sb, &m, 11 + P9_DIRBUF, &r))
            return 0;
        u32 count = (u32)get(&r, 4);
        if (!count || r.pos + count > r.len) {
            kfree(r.b);
            return 0;           /* the end */
        }
        u8 *data = kmalloc(count);
        if (!data) {
            kfree(r.b);
            return 0;
        }
        memcpy(data, r.b + r.pos, count);
        kfree(r.b);
        dn->dbuf = data;
        dn->dlen = count;
        dn->dstart = *off;
    }
    return 0;
}

/* Create a file (opened for I/O) or directory in dir; returns it locked. */
static struct inode *p9_create(struct inode *dir, const char *name, enum itype type, int mode,
                               int *err)
{
    struct super_block *sb = dir->sb;
    struct node *dn = node_by_ino(sb, dir->ino);
    if (!dn || !dn->fid) {
        *err = -ESTALE;
        return NULL;
    }
    struct proc *p = current_proc();
    u32 gid = p ? p->egid : 0;
    u64 qpath = 0;
    u32 ofid = 0;
    struct msg m;
    struct reply r;
    int e;
    if (type == I_DIR) {
        if (!start(&m, 40 + strlen(name), Tmkdir)) {
            *err = -ENOMEM;
            return NULL;
        }
        put(&m, dn->fid, 4);
        put_str(&m, name);
        put(&m, mode & 07777, 4);
        put(&m, gid, 4);
        e = rpc(sb, &m, 64, &r);
        if (!e) {
            qpath = get_qid(&r);
            kfree(r.b);
        }
    } else {
        /* Tlcreate turns the fid it is given into the new, open file */
        e = walk(sb, dn->fid, NULL, &ofid, NULL);
        if (!e) {
            if (!start(&m, 40 + strlen(name), Tlcreate)) {
                clunk(sb, ofid);
                *err = -ENOMEM;
                return NULL;
            }
            put(&m, ofid, 4);
            put_str(&m, name);
            put(&m, L_O_RDWR | L_O_CREAT | L_O_EXCL, 4);
            put(&m, mode & 07777, 4);
            put(&m, gid, 4);
            e = rpc(sb, &m, 64, &r);
            if (!e) {
                qpath = get_qid(&r);
                kfree(r.b);
            } else {
                clunk(sb, ofid);
            }
        }
    }
    if (e) {
        *err = e;
        return NULL;
    }
    dir_changed(dir);
    struct node *n = node_for(sb, qpath);
    if (!n) {
        clunk(sb, ofid);
        *err = -ENOMEM;
        return NULL;
    }
    if (ofid) {
        if (n->ofid)
            clunk(sb, n->ofid);
        n->ofid = ofid;
        n->writable = true;
    }
    if (!n->fid) {
        u32 nf;
        u64 q;
        if (walk(sb, dn->fid, name, &nf, &q) == 0)
            n->fid = nf;
    }
    struct inode *ip = iget(sb, n->ino);
    if (!ip) {
        *err = -ENOMEM;
        return NULL;
    }
    ilock(ip);
    if (!ip->valid) {
        *err = -EIO;
        iunlockput(ip);
        return NULL;
    }
    return ip;
}

static int p9_remove(struct inode *dir, const char *name, bool isdir)
{
    struct node *dn = node_by_ino(dir->sb, dir->ino);
    if (!dn || !dn->fid)
        return -ESTALE;
    struct msg m;
    if (!start(&m, 32 + strlen(name), Tunlinkat))
        return -ENOMEM;
    put(&m, dn->fid, 4);
    put_str(&m, name);
    put(&m, isdir ? AT_REMOVEDIR : 0, 4);
    int e = rpc_simple(dir->sb, &m);
    if (!e)
        dir_changed(dir);
    return e;
}

static int p9_rename(struct inode *odir, const char *oname, struct inode *ndir, const char *nname)
{
    struct node *on = node_by_ino(odir->sb, odir->ino), *nn = node_by_ino(odir->sb, ndir->ino);
    if (!on || !nn || !on->fid || !nn->fid)
        return -ESTALE;
    struct msg m;
    if (!start(&m, 40 + strlen(oname) + strlen(nname), Trenameat))
        return -ENOMEM;
    put(&m, on->fid, 4);
    put_str(&m, oname);
    put(&m, nn->fid, 4);
    put_str(&m, nname);
    int e = rpc_simple(odir->sb, &m);
    if (!e) {
        dir_changed(odir);
        dir_changed(ndir);
    }
    return e;
}

static void p9_stat_fs(struct super_block *sb, int (*print)(const char *fmt, ...))
{
    struct p9fs *fs = fsof(sb);
    print("  9p: tag \"%s\", msize %u, %u inodes known, %lu requests, %lu error replies, %lu failures\n",
          p9_tag(fs->dev), fs->msize, fs->nnodes, (unsigned long)fs->rpcs, (unsigned long)fs->lerrors,
          (unsigned long)fs->errors);
}

static const struct fs_ops p9_ops = {
    .read_inode = p9_read_inode,
    .write_inode = p9_write_inode,
    .read = p9_read,
    .write = p9_write,
    .truncate = p9_truncate,
    .lookup = p9_lookup,
    .link = p9_link,
    .readdir = p9_readdir,
    .create = p9_create,
    .remove = p9_remove,
    .rename = p9_rename,
    .refresh = p9_refresh,
    .stat_fs = p9_stat_fs,
};

/* sb->disk is the 9P device number. */
static int p9_mount(struct super_block *sb)
{
    struct p9fs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->dev = sb->disk;
    fs->next_fid = 2;
    spin_init(&fs->lock, "9pfs");
    sb->priv = fs;

    struct msg m;
    struct reply r;
    if (!start(&m, 64, Tversion))
        return -ENOMEM;
    put(&m, P9_MSIZE, 4);
    put_str(&m, "9P2000.L");
    int e = rpc(sb, &m, 64, &r);
    if (e)
        goto fail;
    fs->msize = (u32)get(&r, 4);
    kfree(r.b);
    if (fs->msize < P9_IOCHUNK + 24) {
        pr_err("9p: server msize %u is too small\n", fs->msize);
        e = -EINVAL;
        goto fail;
    }

    u32 rootfid = new_fid(fs);
    if (!start(&m, 64, Tattach)) {
        e = -ENOMEM;
        goto fail;
    }
    put(&m, rootfid, 4);
    put(&m, NOFID, 4);
    put_str(&m, "root");
    put_str(&m, "");
    put(&m, 0, 4);              /* n_uname: the server decides (security_model) */
    e = rpc(sb, &m, 64, &r);
    if (e)
        goto fail;
    u64 qpath = get_qid(&r);
    kfree(r.b);
    struct node *rn = node_for(sb, qpath);
    if (!rn) {
        e = -ENOMEM;
        goto fail;
    }
    rn->fid = rootfid;
    sb->root = iget(sb, rn->ino);
    if (!sb->root) {
        e = -ENOMEM;
        goto fail;
    }
    ilock(sb->root);
    bool ok = sb->root->valid && sb->root->type == I_DIR;
    iunlock(sb->root);
    if (!ok) {
        iput(sb->root);
        e = -EIO;
        goto fail;
    }
    return 0;
fail:
    sb->priv = NULL;
    kfree(fs->by_ino);
    kfree(fs);
    return e;
}

const struct fs_type ninep_type = { "9p", p9_mount, &p9_ops };
