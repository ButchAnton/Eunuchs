#ifndef _ERRNO_H
#define _ERRNO_H

#include <uapi/errno.h>

extern int errno;
char *strerror(int err);

#endif
