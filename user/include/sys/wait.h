#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <unistd.h>

#define WNOHANG   1
#define WUNTRACED 2
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WCOREDUMP(s)    ((s) & 0x80)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
struct rusage;
/* waitpid, and the child's resource use (its children's included) */
pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru);
pid_t wait3(int *status, int options, struct rusage *ru);

#endif
