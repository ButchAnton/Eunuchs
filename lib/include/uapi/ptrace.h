/* ptrace: what a debugger can do to a process it traces. */
#ifndef _UAPI_PTRACE_H
#define _UAPI_PTRACE_H

#include <stdint.h>

/* requests (the Linux numbers) */
#define PTRACE_TRACEME     0    /* the parent will trace me (then exec) */
#define PTRACE_PEEKTEXT    1    /* read the word at addr into *data */
#define PTRACE_PEEKDATA    2
#define PTRACE_POKETEXT    4    /* write the word data at addr, page protection or not */
#define PTRACE_POKEDATA    5
#define PTRACE_CONT        7    /* resume; data: the signal to deliver (0: none) */
#define PTRACE_KILL        8
#define PTRACE_SINGLESTEP  9    /* EIO: RISC-V has no single-step trap; step in software */
#define PTRACE_GETREGS     12   /* data: struct user_regs */
#define PTRACE_SETREGS     13
#define PTRACE_GETFPREGS   14   /* data: struct user_fpregs */
#define PTRACE_SETFPREGS   15
#define PTRACE_ATTACH      16   /* trace a running process; it stops with SIGSTOP */
#define PTRACE_DETACH      17   /* stop tracing; data: the signal to deliver */
#define PTRACE_GETSIGINFO  0x4202   /* data: struct ptrace_siginfo */
/* Eunuchs: bulk memory access; data: struct ptrace_io (addr unused) */
#define PTRACE_READ        0x4e00
#define PTRACE_WRITE       0x4e01

/* r[0] is pc, r[1..31] are x1..x31 (the Linux user_regs_struct order) */
struct user_regs {
    uint64_t r[32];
};

struct user_fpregs {
    uint64_t f[32];
    uint64_t fcsr;
};

/* why the process stopped */
struct ptrace_siginfo {
    int32_t si_signo;
    int32_t si_code;            /* SI_* / TRAP_* / SEGV_* below */
    uint64_t si_addr;           /* faulting address, or the pc of a breakpoint */
};

#define SI_USER      0          /* sent with kill() */
#define TRAP_BRKPT   1          /* ebreak */
#define TRAP_EXEC    5          /* after a successful exec (Eunuchs) */
#define SEGV_MAPERR  1          /* no mapping at the address */
#define SEGV_ACCERR  2          /* mapped, but not with that access */
#define ILL_ILLOPC   1
#define BUS_ADRALN   1

struct ptrace_io {
    uint64_t addr;              /* in the traced process */
    uint64_t buf;               /* in the debugger */
    uint64_t len;
};

#endif
