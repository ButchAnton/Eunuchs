/*
 * User program entry. The kernel starts us with sp -> argc, argv[], NULL,
 * envp[] NULL, auxv; a0 = argc, a1 = argv as a convenience.
 */
    .section .text.start
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    mv      fp, zero                /* end of the frame-pointer chain */
    ld      a0, 0(sp)
    addi    a1, sp, 8
    slli    a2, a0, 3
    add     a2, a2, a1
    addi    a2, a2, 8               /* envp = argv + argc + 1 */
    call    __libc_start
1:  j       1b
