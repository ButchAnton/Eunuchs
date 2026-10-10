/*
 * void context_switch(struct context *old, struct context *new)
 *
 * Saves the callee-saved registers of the running thread into *old and
 * resumes the thread whose registers are in *new. Returns (in the new
 * thread) to wherever that thread called context_switch, or for a fresh
 * thread to thread_entry (its ctx.ra).
 *
 * A sleeping thread's backtrace starts at ctx.ra with frame pointer ctx.s[0].
 */
    .section .text
    .globl context_switch
    .balign 4
context_switch:
    sd      ra, 0(a0)
    sd      sp, 8(a0)
    sd      s0, 16(a0)
    sd      s1, 24(a0)
    sd      s2, 32(a0)
    sd      s3, 40(a0)
    sd      s4, 48(a0)
    sd      s5, 56(a0)
    sd      s6, 64(a0)
    sd      s7, 72(a0)
    sd      s8, 80(a0)
    sd      s9, 88(a0)
    sd      s10, 96(a0)
    sd      s11, 104(a0)

    ld      ra, 0(a1)
    ld      sp, 8(a1)
    ld      s0, 16(a1)
    ld      s1, 24(a1)
    ld      s2, 32(a1)
    ld      s3, 40(a1)
    ld      s4, 48(a1)
    ld      s5, 56(a1)
    ld      s6, 64(a1)
    ld      s7, 72(a1)
    ld      s8, 80(a1)
    ld      s9, 88(a1)
    ld      s10, 96(a1)
    ld      s11, 104(a1)
    ret

/*
 * First code run by a new thread (its ctx.ra). Clearing ra makes
 * thread_entry's saved return address 0, which ends backtraces cleanly.
 */
    .globl thread_start
    .balign 4
thread_start:
    li      ra, 0
    j       thread_entry
