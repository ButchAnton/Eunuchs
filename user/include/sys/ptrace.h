#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H

#include <sys/types.h>
#include <uapi/ptrace.h>

/* As in glibc: PTRACE_PEEK* return the word read (check errno, set to 0
   beforehand, to tell a word of -1 from an error); the rest return 0 or -1. */
long ptrace(int request, pid_t pid, void *addr, void *data);

#endif
