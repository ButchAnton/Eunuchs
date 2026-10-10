#ifndef EUNUCHS_UAPI_FS_H
#define EUNUCHS_UAPI_FS_H

/* File API types shared by the kernel and the user libc. */

#include <stdint.h>

#define AT_FDCWD      (-100)
#define AT_REMOVEDIR  0x200

#define O_RDONLY      00
#define O_WRONLY      01
#define O_RDWR        02
#define O_ACCMODE     03
#define O_CREAT       0100
#define O_EXCL        0200
#define O_TRUNC       01000
#define O_APPEND      02000
#define O_NONBLOCK    04000
#define O_DIRECTORY   0200000
#define O_CLOEXEC     02000000

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_blocks;     /* 512-byte units */
    uint64_t st_mtime;      /* ns since the epoch */
    uint64_t st_ctime;
    uint32_t st_rdev;
    uint32_t st_pad;
};

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8

/* getdents64: an array of fixed-size records. */
struct dirent {
    uint32_t d_ino;
    uint8_t d_type;
    char d_name[59];
};

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define PATH_MAX 256
#define NAME_MAX 59

#endif
