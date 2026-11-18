#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <uapi/statfs.h>

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
