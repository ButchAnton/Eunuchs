#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <uapi/fs.h>

#define CLOCKS_PER_SEC 1000000L

/* Eunuchs has no time zones: local time is UTC. */
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
int clock_gettime(int clock, struct timespec *ts);
int gettimeofday(struct timeval *tv, void *tz);
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned sleep(unsigned seconds);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

#endif
