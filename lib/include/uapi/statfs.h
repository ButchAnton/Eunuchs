#ifndef _UAPI_STATFS_H
#define _UAPI_STATFS_H

/* statfs(2), fstatfs(2): Linux's 64-bit layout */
struct statfs {
    long f_type;                /* the filesystem's magic number */
    long f_bsize;               /* block size */
    unsigned long f_blocks;     /* data blocks */
    unsigned long f_bfree;      /* free blocks */
    unsigned long f_bavail;     /* free blocks for unprivileged users */
    unsigned long f_files;      /* inodes */
    unsigned long f_ffree;      /* free inodes */
    struct { int val[2]; } f_fsid;
    long f_namelen;             /* longest file name */
    long f_frsize;              /* fragment size (= f_bsize) */
    long f_flags;               /* ST_* mount flags */
    long f_spare[4];
};

#define EFS_SUPER_MAGIC   0x53465545    /* "EUFS" */
#define DEVFS_SUPER_MAGIC 0x1373
#define PROC_SUPER_MAGIC  0x9fa0
#define V9FS_MAGIC        0x01021997

#define ST_RDONLY 1
#define ST_NOSUID 2

/* mount(2) flags (accepted; Eunuchs has no read-only mounts yet) */
#define MS_RDONLY 1
/* umount2(2) flags */
#define MNT_FORCE  1
#define MNT_DETACH 2

#endif
