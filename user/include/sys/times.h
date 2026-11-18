#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H

#include <sys/types.h>
#include <uapi/resource.h>              /* struct tms */

/* CPU times in clock ticks (sysconf(_SC_CLK_TCK)); returns ticks since boot */
clock_t times(struct tms *buf);

#endif
