#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>
#include <uapi/fs.h>            /* struct timeval, struct timespec */

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif
