#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>
#include <uapi/fs.h>

typedef int64_t time_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday;
};

time_t time(time_t *t);
int clock_gettime(int clock, struct timespec *ts);
int gettimeofday(struct timeval *tv, void *tz);
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned sleep(unsigned seconds);
struct tm *gmtime_r(const time_t *t, struct tm *out);

#endif
