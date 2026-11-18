#ifndef _UAPI_RESOURCE_H
#define _UAPI_RESOURCE_H

#include <uapi/fs.h>                    /* struct timeval */

/* getrusage(who, ...) */
#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN  (-1)

/* Linux's layout. Eunuchs fills ru_utime, ru_stime, ru_maxrss (KiB),
   ru_minflt, ru_majflt, ru_nsignals, ru_nvcsw and ru_nivcsw; the rest are 0. */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt;
    long ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

/* times(): in clock ticks (sysconf(_SC_CLK_TCK) = 100 per second) */
#define EUNUCHS_CLK_TCK 100

struct tms {
    long tms_utime, tms_stime;
    long tms_cutime, tms_cstime;
};

#endif
