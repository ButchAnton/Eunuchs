/* long __syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) */
    .section .text
    .globl __syscall
    .type __syscall, @function
__syscall:
    mv      a7, a0
    mv      a0, a1
    mv      a1, a2
    mv      a2, a3
    mv      a3, a4
    mv      a4, a5
    mv      a5, a6
    ecall
    ret

/* Signal handlers return here (sa_restorer): back into the kernel. */
    .globl __restore_rt
    .type __restore_rt, @function
__restore_rt:
    li      a7, 139                 /* SYS_rt_sigreturn */
    ecall
