#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif
typedef int pid_t;
typedef unsigned short uid_t;
typedef unsigned short gid_t;
typedef unsigned int mode_t;
typedef long off_t;
typedef unsigned int ino_t;
typedef unsigned int dev_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef int64_t time_t;
#endif
typedef long suseconds_t;
typedef long clock_t;

#endif
