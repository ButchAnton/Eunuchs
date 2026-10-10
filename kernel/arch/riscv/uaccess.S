/*
 * Raw user-memory copies. sstatus.SUM (supervisor may access user pages) is
 * set only for the duration of the copy, so any other kernel dereference of
 * a user pointer faults. A faulting access jumps to the fixup via
 * __ex_table and returns the number of bytes NOT copied. (trap.c first tries
 * to fault the page in: demand paging and COW work transparently here.)
 *
 * Callers must have checked that the user range is below USER_TOP
 * (uaccess.c does); SUM would otherwise also allow kernel addresses.
 */
#define SSTATUS_SUM (1 << 18)

    .section .text

/* size_t __copy_user(void *dst, const void *src, size_t n) */
    .globl __copy_user
    .balign 4
__copy_user:
    li      t1, SSTATUS_SUM
    csrs    sstatus, t1
1:  beqz    a2, 3f
2:  lbu     t0, 0(a1)
4:  sb      t0, 0(a0)
    addi    a0, a0, 1
    addi    a1, a1, 1
    addi    a2, a2, -1
    j       1b
3:  csrc    sstatus, t1
    li      a0, 0
    ret
9:  csrc    sstatus, t1
    mv      a0, a2
    ret
    .pushsection __ex_table, "a"
    .balign 8
    .dword  2b, 9b
    .dword  4b, 9b
    .popsection

/*
 * long __strncpy_user(char *dst, const char *usrc, size_t max)
 * Returns the string length (excluding NUL), max if no NUL within max
 * bytes, or -1 on a fault.
 */
    .globl __strncpy_user
    .balign 4
__strncpy_user:
    li      t1, SSTATUS_SUM
    csrs    sstatus, t1
    li      t2, 0
1:  bgeu    t2, a2, 3f
2:  lbu     t0, 0(a1)
    sb      t0, 0(a0)
    beqz    t0, 3f
    addi    a0, a0, 1
    addi    a1, a1, 1
    addi    t2, t2, 1
    j       1b
3:  csrc    sstatus, t1
    mv      a0, t2
    ret
9:  csrc    sstatus, t1
    li      a0, -1
    ret
    .pushsection __ex_table, "a"
    .balign 8
    .dword  2b, 9b
    .popsection
