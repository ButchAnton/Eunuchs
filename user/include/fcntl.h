#ifndef _FCNTL_H
#define _FCNTL_H

#include <uapi/fs.h>

int open(const char *path, int flags, ...);
int creat(const char *path, int mode);

#endif
