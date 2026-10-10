#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <unistd.h>
#include <uapi/signal.h>

typedef uint32_t sigset_t;

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
sighandler_t signal(int sig, sighandler_t handler);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigsuspend(const sigset_t *mask);
int pause(void);
int raise(int sig);
const char *strsignal(int sig);

static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t *s) { *s = ~0u; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { *s |= SIGBIT(sig); return 0; }
static inline int sigdelset(sigset_t *s, int sig) { *s &= ~SIGBIT(sig); return 0; }
static inline int sigismember(const sigset_t *s, int sig) { return (*s >> sig) & 1; }

#endif
