#ifndef EUNUCHS_FS_H
#define EUNUCHS_FS_H

/*
 * Filesystems.
 *
 *   bcache.c   buffer cache over the block device (4 KiB blocks)
 *   log.c      write-ahead log: crash-consistent metadata+data transactions
 *   efs.c      the on-disk filesystem (format in efs_format.h)
 *   vfs.c      inode cache, path lookup, mounts, namespace operations
 *   file.c     open files, descriptors, file system calls
 *   devfs.c    /dev: console null zero kmsg disk0
 *   procfs.c   /proc: kernel state as text files
 *
 * Locking (outermost first): sb->ns_lock (any namespace change: create,
 * unlink, link, rename, mkdir, rmdir), then inode locks (directory before
 * entry), then buffer locks. Lookups lock one directory at a time. The mutex
 * deadlock detector watches all of these.
 */

#include <efs_format.h>
#include <list.h>
#include <sched.h>
#include <types.h>
#include <uapi/fs.h>

/* ---- buffer cache ---- */

struct buf {
    u32 blockno;
    u32 disk;                   /* which disk (blk_rw numbering) */
    bool valid;                 /* data read from disk */
    int refcnt;
    u32 pins;                   /* held by the log until installed */
    struct mutex lock;
    struct list_head lru;       /* most recently used first */
    u8 *data;                   /* EFS_BSIZE bytes */
    uintptr_t get_site;         /* who has it (debugging) */
    u64 last_used;
};

void bcache_init(void);
struct buf *bread(u32 disk, u32 blockno);   /* returns the buffer locked */
int bwrite(struct buf *b);              /* write through; caller holds it */
void brelse(struct buf *b);
void bpin(struct buf *b);
void bunpin(struct buf *b);
int bcache_busy(void);                  /* buffers still referenced (leak checks) */
void bcache_print(int (*print)(const char *fmt, ...));

/* ---- VFS ---- */

enum itype { I_FREE = 0, I_DIR = 1, I_FILE = 2, I_CHR = 3 };

struct inode;
struct file;
struct super_block;

/* Character-device / synthetic-file hooks (devfs, procfs). */
struct poll_table;
struct waitqueue;

struct chr_ops {
    int (*open)(struct file *f);
    long (*read)(struct file *f, void *buf, size_t n, bool user);
    long (*write)(struct file *f, const void *buf, size_t n, bool user);
    void (*close)(struct file *f);
    long (*ioctl)(struct file *f, unsigned long req, uintptr_t arg);
    /* Readiness (POLLIN/POLLOUT/...); register the wait queues that signal
     * changes with poll_wait(pt, wq). NULL: always readable and writable. */
    u32 (*poll)(struct file *f, struct poll_table *pt);
};

struct file *fd_get(long fd);
/* poll.c */
void poll_wait(struct poll_table *pt, struct waitqueue *wq);
u32 file_poll(struct file *f, struct poll_table *pt);

struct kdirent {
    u32 ino;
    u8 type;                    /* DT_* */
    char name[EFS_NAMELEN + 1];
};

/* Operations a filesystem provides. Unneeded ones may be NULL. */
struct fs_ops {
    int (*read_inode)(struct inode *ip);
    int (*write_inode)(struct inode *ip);
    struct inode *(*alloc_inode)(struct super_block *sb, enum itype type);
    void (*free_inode)(struct inode *ip);           /* truncate + mark free */
    long (*read)(struct inode *ip, void *dst, u64 off, size_t n, bool user);
    long (*write)(struct inode *ip, const void *src, u64 off, size_t n, bool user);
    int (*truncate)(struct inode *ip, u64 size);
    u32 (*lookup)(struct inode *dir, const char *name);
    int (*link)(struct inode *dir, const char *name, u32 ino);
    int (*unlink)(struct inode *dir, const char *name);
    int (*readdir)(struct inode *dir, u64 *off, struct kdirent *out);   /* 1 = entry, 0 = end */
    void (*begin_op)(struct super_block *sb);
    void (*end_op)(struct super_block *sb);
    void (*stat_fs)(struct super_block *sb, int (*print)(const char *fmt, ...));
    /* optional: put everything that has finished on disk */
    void (*sync)(struct super_block *sb);
    /*
     * For filesystems that work by name (9pfs), where alloc_inode + link and
     * the nlink bookkeeping of unlink and rename don't apply. When present,
     * the VFS calls these instead.
     */
    struct inode *(*create)(struct inode *dir, const char *name, enum itype type, int mode,
                            int *err);              /* returns the new inode locked */
    int (*remove)(struct inode *dir, const char *name, bool isdir);
    int (*rename)(struct inode *odir, const char *oname, struct inode *ndir, const char *nname);
    int (*refresh)(struct inode *ip);   /* re-read attributes that may have changed elsewhere */
};

struct fs_type {
    const char *name;
    int (*mount)(struct super_block *sb);
    const struct fs_ops *ops;
};

struct super_block {
    const struct fs_type *type;
    const struct fs_ops *ops;
    struct inode *root;
    struct inode *covered;      /* mount point in the parent fs (NULL for /) */
    struct mutex ns_lock;
    char path[32];
    void *priv;
    int disk;                   /* the disk a disk-based filesystem lives on */
    u32 dev;                    /* st_dev: 1, 2, ... in mount order */
    u64 errors;                 /* inconsistencies detected while running */
    struct list_head node;
};

struct inode {
    struct super_block *sb;
    u32 ino;
    int refcnt;
    struct mutex lock;
    bool valid;
    u16 type;                   /* enum itype */
    u16 nlink;
    u16 mode;
    u16 uid, gid;
    u64 size;
    u64 mtime, ctime;
    u32 rdev;
    const struct chr_ops *cops; /* devices and synthetic files */
    struct super_block *mounted;    /* a filesystem mounted on this directory */
    struct list_head cache_node;
    union {
        struct efs_dinode efs;
        u64 data[4];
    } u;
};

struct file {
    int refcnt;
    struct inode *ip;
    u64 off;
    int flags;
    bool readable, writable;
    const struct chr_ops *cops;
    void *priv;                 /* procfs snapshot etc. */
    size_t priv_len;
    char path[64];              /* as opened (debugging); "pipe:[n] read" for pipes */
};

extern struct inode *root_inode;

void vfs_init(void);
int vfs_mount(const struct fs_type *type, const char *path);
int vfs_mount_disk(const struct fs_type *type, const char *path, int disk);
int efs_label(int disk, char out[17]);         /* -1 if the disk holds no efs */
struct inode *iget(struct super_block *sb, u32 ino);
struct inode *idup(struct inode *ip);
void ilock(struct inode *ip);
void iunlock(struct inode *ip);
void iput(struct inode *ip);
void iunlockput(struct inode *ip);
int icache_busy(void);                  /* inodes referenced (leak checks) */

struct inode *namei(const char *path, int *err);
struct inode *nameiparent(const char *path, char *name, int *err);
struct file *vfs_open(const char *path, int flags, int mode, int *err);
int vfs_mkdir(const char *path, int mode);
int vfs_unlink(const char *path, bool dir);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_getcwd(struct inode *cwd, char *buf, size_t size);
void vfs_stat(struct inode *ip, struct stat *st);
/* May the current process access ip? mask: R_OK/W_OK/X_OK bits. 0 or -EACCES. */
int vfs_permission(struct inode *ip, int mask);
void fs_begin_op(struct super_block *sb);
void fs_end_op(struct super_block *sb);
void vfs_mounts_print(int (*print)(const char *fmt, ...));
void vfs_sync_all(void);
void vfs_sync(struct super_block *sb);
void icache_print(int (*print)(const char *fmt, ...));
void fs_error(struct super_block *sb, const char *fmt, ...) __printf_like(2, 3);

/* Read a whole file into kernel memory (exec). */
void *vfs_read_all(const char *path, size_t *size, int *err);

long file_read(struct file *f, void *buf, size_t n, bool user);
long file_write(struct file *f, const void *buf, size_t n, bool user);
struct file *file_dup(struct file *f);
void file_close(struct file *f);

/* Per-process descriptor tables (file.c). */
#define NOFILE 32
struct proc;
int fd_install(struct proc *p, struct file *f);
void files_fork(struct proc *child, struct proc *parent);
void files_close_all(struct proc *p);
void files_print(int (*print)(const char *fmt, ...), struct proc *p);       /* monitor */
void files_print_fds(int (*print)(const char *fmt, ...), struct proc *p);

/* tty.c (the console's line discipline and job control) */
struct tty;
extern const struct chr_ops tty_ops;
void tty_init(void);
void tty_input(char c);
struct tty *tty_console(void);
int tty_fg_pgrp(struct tty *tty);
void tty_set_ctty(struct proc *p);
void tty_print(int (*print)(const char *fmt, ...));

/* pipe.c */
int pipe_create(struct file **rf, struct file **wf);
bool file_is_pipe(const struct file *f);
void pipes_print(int (*print)(const char *fmt, ...));

/* Descriptor flags (FD_CLOEXEC is in uapi/fs.h) */
void files_exec(struct proc *p);            /* close FD_CLOEXEC descriptors */

/* Filesystem types. */
extern const struct fs_type efs_type, devfs_type, procfs_type, ninep_type;

/* log.c (efs) */
struct efs_log;
struct efs_log *log_create(struct super_block *sb, const struct efs_super *esb);
void log_begin(struct efs_log *log);
void log_end(struct efs_log *log);
void log_sync(struct efs_log *log);
void log_write(struct efs_log *log, struct buf *b);
void log_print(struct efs_log *log, int (*print)(const char *fmt, ...));
int efs_arm_crash(const char *point);
u32 efs_free_blocks(struct super_block *sb);           /* scans the bitmap */
u32 efs_free_blocks_counter(struct super_block *sb);   /* in-memory count */   /* testing: stop the machine mid-commit */

/* procfs text generation (also used by /dev/kmsg). */
int seq_print(const char *fmt, ...) __printf_like(1, 2);

#endif
