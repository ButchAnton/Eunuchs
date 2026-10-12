#ifndef EUNUCHS_EFS_FORMAT_H
#define EUNUCHS_EFS_FORMAT_H

/*
 * On-disk format of efs, the Eunuchs filesystem. Shared by the kernel
 * (kernel/fs/efs.c, log.c) and the host tools (tools/mkfs: mkfs and fsck),
 * so it uses only <stdint.h> types.
 *
 * Layout, in 4 KiB blocks:
 *   0                  unused (boot block)
 *   1                  superblock
 *   logstart..         write-ahead log: 1 header block + EFS_LOGSIZE data blocks
 *   inodestart..       inode table, EFS_IPB inodes per block
 *   bmapstart..        free-block bitmap, 1 bit per block (1 = used)
 *   datastart..size-1  data blocks
 *
 * Files use EFS_NDIRECT direct blocks, one single-indirect and one
 * double-indirect block (max ~4 GiB). Directories are files of fixed-size
 * entries; inode 0 means "empty slot". The root directory is inode 1.
 */

#include <stdint.h>

#define EFS_MAGIC        0x53465545u    /* "EUFS" */
#define EFS_LOG_MAGIC    0x474f4c45u    /* "ELOG" */
#define EFS_VERSION      1
#define EFS_BSIZE        4096
#define EFS_SECTORS_PER_BLOCK (EFS_BSIZE / 512)
#define EFS_ROOT_INO     1
#define EFS_NDIRECT      12
#define EFS_NINDIRECT    (EFS_BSIZE / 4)
#define EFS_MAXFILE      ((uint64_t)(EFS_NDIRECT + EFS_NINDIRECT + \
                          (uint64_t)EFS_NINDIRECT * EFS_NINDIRECT) * EFS_BSIZE)
#define EFS_LOGSIZE      64             /* data blocks in the log */
#define EFS_NAMELEN      59             /* max name length (60 bytes with NUL) */

enum efs_type {
    EFS_T_FREE = 0,
    EFS_T_DIR  = 1,
    EFS_T_FILE = 2,
};

struct efs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t bsize;
    uint32_t size;          /* total blocks */
    uint32_t ninodes;
    uint32_t nlog;          /* log blocks including the header */
    uint32_t logstart;
    uint32_t inodestart;
    uint32_t bmapstart;
    uint32_t datastart;
    uint64_t mkfs_time;     /* ns since the epoch */
    char label[16];         /* volume label ("root", "home"); zeros in old images */
};

struct efs_dinode {
    uint16_t type;          /* enum efs_type */
    uint16_t nlink;
    uint16_t mode;          /* permission bits */
    uint16_t uid;
    uint16_t gid;
    uint16_t reserved0;
    uint32_t flags;
    uint64_t size;
    uint64_t mtime;         /* ns since the epoch */
    uint64_t ctime;
    uint32_t addrs[EFS_NDIRECT + 2];    /* direct, single-, double-indirect */
    uint8_t reserved[32];
};

#define EFS_IPB (EFS_BSIZE / sizeof(struct efs_dinode))

struct efs_dirent {
    uint32_t ino;           /* 0 = free slot */
    char name[EFS_NAMELEN + 1];
};

#define EFS_DPB (EFS_BSIZE / sizeof(struct efs_dirent))

struct efs_logheader {
    uint32_t magic;
    uint32_t n;             /* committed blocks; 0 = log empty */
    uint64_t seq;           /* transaction number */
    uint32_t blocks[EFS_LOGSIZE];   /* home block number of each log block */
};

_Static_assert(sizeof(struct efs_dinode) == 128, "efs_dinode size");
_Static_assert(sizeof(struct efs_dirent) == 64, "efs_dirent size");
_Static_assert(sizeof(struct efs_logheader) <= EFS_BSIZE, "log header fits a block");

#endif
