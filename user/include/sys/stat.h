#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <uapi/fs.h>

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, int mode);
struct timespec;
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
int chmod(const char *path, int mode);
int fchmod(int fd, int mode);
int umask(int mask);

#endif
