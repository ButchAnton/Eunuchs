#ifndef _EUNUCHS_H
#define _EUNUCHS_H

/* Eunuchs debugging hooks for user programs (SYS_debug). */

#include <uapi/syscall.h>

void dbg_mark(const char *msg);        /* trace event + kernel log line */
void dbg_break(void);                  /* stop in the kernel debug monitor */
void dbg_strace(int mode);             /* 0 off, 1 on, 2 on + follow forks */
void dbg_ps(void);                     /* print the process table */
void dbg_files(void);                  /* print my open files */
int dbg_fscrash(const char *point);    /* testing: crash at that point of the next commit */
void dbg_vm(void);                     /* print my memory map */
/* profiler: 0 stop, 1 start (arg = Hz), 2 clear, 3 dump to host path (arg = char *) */
int dbg_profile(int op, long arg);

#endif
