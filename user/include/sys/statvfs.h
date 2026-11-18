#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

#include <uapi/statfs.h>

/* POSIX's view of statfs */
struct statvfs {
    unsigned long f_bsize, f_frsize;
    unsigned long f_blocks, f_bfree, f_bavail;
    unsigned long f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif
