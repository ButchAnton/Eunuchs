#ifndef EUNUCHS_UAPI_SYSCALL_H
#define EUNUCHS_UAPI_SYSCALL_H

/*
 * System call numbers, shared by the kernel, the user libc, strace, and
 * tools/trace/eutrace.py (which parses this list; keep one per line).
 * Numbers follow Linux/RISC-V where an equivalent exists; >= 500 are ours.
 *
 * X(number, name, nargs, "strace argument formats")
 *   d int   u unsigned   x hex   p pointer   s user string
 *   b user buffer whose length is the next argument   a argv array   k signal   o octal
 *   n struct sockaddr_in (shown as address:port)
 */
#define SYSCALLS(X)                          \
    X(17,  getcwd,      2, "pu")             \
    X(23,  dup,         1, "d")              \
    X(24,  dup3,        3, "ddx")            \
    X(25,  fcntl,       3, "ddx")            \
    X(29,  ioctl,       3, "dxx")            \
    X(34,  mkdirat,     3, "dsx")            \
    X(35,  unlinkat,    3, "dsx")            \
    X(37,  linkat,      5, "dsdsx")          \
    X(38,  renameat,    4, "dsds")           \
    X(48,  faccessat,   3, "dsx")            \
    X(49,  chdir,       1, "s")              \
    X(52,  fchmod,      2, "do")             \
    X(53,  fchmodat,    3, "dso")            \
    X(54,  fchownat,    5, "dsddx")          \
    X(56,  openat,      4, "dsxx")           \
    X(57,  close,       1, "d")              \
    X(59,  pipe2,       2, "px")             \
    X(61,  getdents64,  3, "dpu")            \
    X(62,  lseek,       3, "ddd")            \
    X(63,  read,        3, "dpu")            \
    X(64,  write,       3, "dbu")            \
    X(73,  ppoll,       4, "pdpp")           \
    X(79,  fstatat,     4, "dspx")           \
    X(88,  utimensat,   4, "dspx")           \
    X(80,  fstat,       2, "dp")             \
    X(81,  sync,        0, "")               \
    X(82,  fsync,       1, "d")              \
    X(93,  exit,        1, "d")              \
    X(101, nanosleep,   2, "pp")             \
    X(113, clock_gettime, 2, "dp")           \
    X(117, ptrace,      4, "ddxx")           \
    X(124, sched_yield, 0, "")               \
    X(129, kill,        2, "dk")             \
    X(144, setgid,      1, "d")              \
    X(146, setuid,      1, "d")              \
    X(133, rt_sigsuspend, 1, "p")            \
    X(134, rt_sigaction, 3, "kpp")           \
    X(135, rt_sigprocmask, 3, "dpp")         \
    X(139, rt_sigreturn, 0, "")              \
    X(154, setpgid,     2, "dd")             \
    X(155, getpgid,     1, "d")              \
    X(156, getsid,      1, "d")              \
    X(157, setsid,      0, "")               \
    X(166, umask,       1, "o")              \
    X(169, gettimeofday, 2, "pp")            \
    X(172, getpid,      0, "")               \
    X(173, getppid,     0, "")               \
    X(174, getuid,      0, "")               \
    X(175, geteuid,     0, "")               \
    X(176, getgid,      0, "")               \
    X(177, getegid,     0, "")               \
    X(198, socket,      3, "ddd")            \
    X(200, bind,        3, "dnd")            \
    X(201, listen,      2, "dd")             \
    X(202, accept,      3, "dpp")            \
    X(203, connect,     3, "dnd")            \
    X(204, getsockname, 3, "dpp")            \
    X(205, getpeername, 3, "dpp")            \
    X(206, sendto,      6, "dbuxnd")         \
    X(207, recvfrom,    6, "dpuxpp")         \
    X(208, setsockopt,  5, "dddpd")          \
    X(209, getsockopt,  5, "dddpp")          \
    X(210, shutdown,    2, "dd")             \
    X(214, brk,         1, "p")              \
    X(215, munmap,      2, "pu")             \
    X(220, fork,        0, "")               \
    X(221, execve,      3, "sap")            \
    X(222, mmap,        6, "puxxdx")         \
    X(226, mprotect,    3, "pux")            \
    X(242, accept4,     4, "dppx")           \
    X(260, wait4,       4, "dpxp")           \
    X(500, sleep_ms,    1, "u")              \
    X(501, uptime_ms,   0, "")               \
    X(502, debug,       3, "dxx")

enum {
#define X(nr, name, nargs, fmt) SYS_##name = nr,
    SYSCALLS(X)
#undef X
};

#define SYSCALL_MAX 512

/* SYS_debug operations */
enum {
    DBG_MARK   = 0,     /* arg: user string -> trace event + log line */
    DBG_BREAK  = 1,     /* enter the kernel debug monitor */
    DBG_STRACE = 2,     /* arg: 0 off, 1 on (inherited by exec), 2 on + follow forks */
    DBG_PS     = 3,     /* print the process table on the console */
    DBG_FILES  = 4,     /* print this process's open files */
    DBG_VM     = 5,     /* print this process's memory map */
    DBG_FSCRASH = 6,    /* arg: "after-log" | "after-commit" | "mid-install": stop the
                           machine at that point of the next filesystem commit */
    DBG_PROFILE = 7,    /* arg: 0 stop, 1 start (arg2 = Hz, 0 default), 2 clear,
                           3 dump to the host file named by arg2 (semihosting) */
};

/* mmap */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED  ((void *)-1)

/* wait4 */
#define WNOHANG 1

#endif
