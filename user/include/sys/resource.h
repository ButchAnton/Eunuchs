#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/types.h>
#include <uapi/resource.h>

/* getrusage(RUSAGE_SELF | RUSAGE_CHILDREN, &ru) */
int getrusage(int who, struct rusage *ru);

#endif
