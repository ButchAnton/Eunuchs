#ifndef EUNUCHS_UAPI_SIGNAL_H
#define EUNUCHS_UAPI_SIGNAL_H

/* Signals: numbers (Linux values), names, default actions; shared with libc. */

#include <stdint.h>

/* X(name, number, default action, description); actions: T terminate,
 * C terminate + "core dumped", I ignore, S stop, K continue. */
#define SIGNALS(X)                                          \
    X(SIGHUP,    1, 'T', "Hangup")                          \
    X(SIGINT,    2, 'T', "Interrupt")                       \
    X(SIGQUIT,   3, 'C', "Quit")                            \
    X(SIGILL,    4, 'C', "Illegal instruction")             \
    X(SIGTRAP,   5, 'C', "Trace/breakpoint trap")           \
    X(SIGABRT,   6, 'C', "Aborted")                         \
    X(SIGBUS,    7, 'C', "Bus error")                       \
    X(SIGFPE,    8, 'C', "Floating point exception")        \
    X(SIGKILL,   9, 'T', "Killed")                          \
    X(SIGUSR1,  10, 'T', "User defined signal 1")           \
    X(SIGSEGV,  11, 'C', "Segmentation fault")              \
    X(SIGUSR2,  12, 'T', "User defined signal 2")           \
    X(SIGPIPE,  13, 'T', "Broken pipe")                     \
    X(SIGALRM,  14, 'T', "Alarm clock")                     \
    X(SIGTERM,  15, 'T', "Terminated")                      \
    X(SIGCHLD,  17, 'I', "Child exited")                    \
    X(SIGCONT,  18, 'K', "Continued")                       \
    X(SIGSTOP,  19, 'S', "Stopped (signal)")                \
    X(SIGTSTP,  20, 'S', "Stopped")                         \
    X(SIGTTIN,  21, 'S', "Stopped (tty input)")             \
    X(SIGTTOU,  22, 'S', "Stopped (tty output)")            \
    X(SIGWINCH, 28, 'I', "Window changed")

enum {
#define X(name, num, act, desc) name = num,
    SIGNALS(X)
#undef X
};

#define NSIG 32
#define SIGBIT(s) (1u << (s))

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_RESTART   0x10000000     /* accepted; interrupted calls still fail with EINTR */
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);      /* libc sets this: calls rt_sigreturn */
    uint32_t sa_mask;
    uint32_t sa_pad;
};

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* wait4 status for stopped children: (sig << 8) | 0x7f */
#define WUNTRACED 2

#endif
