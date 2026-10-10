/*
 * Trap entry and return (stvec, direct mode), for traps from both user and
 * kernel mode. Saves a full struct trap_frame, calls trap_handler(tf),
 * restores, and srets.
 *
 * sscratch convention:
 *   in user mode    sscratch = this CPU's struct cpu (the kernel's tp)
 *   in kernel mode  sscratch = 0
 * so `csrrw tp, sscratch, tp` at entry tells us where we came from.
 *
 * From user: switch to the current thread's kernel stack (cpu->kstack_top);
 * the frame sits at the very top of it (thread_user_tf()). The user sp and tp
 * are saved into the frame.
 *
 * From kernel: push the frame on the current stack. Every kernel stack
 * occupies the upper half of a slot whose lower half is an unmapped guard
 * (mm.h), so a usable sp has bit KSTACK_GUARD_BIT set. If the room left would
 * reach into the guard, switch to the per-CPU emergency stack first;
 * trap_handler() reports the overflow (original sp in cpu->overflow_sp).
 *
 * Above the frame we build a fake frame record {fp, ra = sepc} and point s0
 * at it, so kernel backtraces continue through the trap into the interrupted
 * code (for user traps fp = 0 ends the chain). The CFI directives give GDB
 * the same view of kernel-mode traps.
 */
#include <mm.h>
#include <percpu.h>

#define FRAME_REGS   288            /* 32 regs + sepc, sstatus, scause, stval */
#define FRAME_SIZE   (FRAME_REGS + 16)
#define TF_SEPC      256
#define TF_SSTATUS   264
#define TF_SCAUSE    272
#define TF_STVAL     280
#define SSTATUS_SPP  0x100

.macro SAVE_GPRS
    sd      x1,  1*8(sp)
    sd      x3,  3*8(sp)
    sd      x4,  4*8(sp)
    sd      x5,  5*8(sp)
    sd      x6,  6*8(sp)
    sd      x7,  7*8(sp)
    sd      x8,  8*8(sp)
    sd      x9,  9*8(sp)
    sd      x10, 10*8(sp)
    sd      x11, 11*8(sp)
    sd      x12, 12*8(sp)
    sd      x13, 13*8(sp)
    sd      x14, 14*8(sp)
    sd      x15, 15*8(sp)
    sd      x16, 16*8(sp)
    sd      x17, 17*8(sp)
    sd      x18, 18*8(sp)
    sd      x19, 19*8(sp)
    sd      x20, 20*8(sp)
    sd      x21, 21*8(sp)
    sd      x22, 22*8(sp)
    sd      x23, 23*8(sp)
    sd      x24, 24*8(sp)
    sd      x25, 25*8(sp)
    sd      x26, 26*8(sp)
    sd      x27, 27*8(sp)
    sd      x28, 28*8(sp)
    sd      x29, 29*8(sp)
    sd      x30, 30*8(sp)
    sd      x31, 31*8(sp)
.endm

    .section .text
    .globl kernel_trap_entry
    .balign 4
kernel_trap_entry:
    .cfi_startproc
    .cfi_signal_frame
    .cfi_return_column 64
    csrrw   tp, sscratch, tp
    bnez    tp, .Lfrom_user

    /* ---- from kernel mode: undo the swap (sscratch stays 0) ---- */
    csrrw   tp, sscratch, zero
    sd      t0, CPU_SCRATCH(tp)
    addi    t0, sp, -(FRAME_SIZE + 256)
    srli    t0, t0, KSTACK_GUARD_BIT
    andi    t0, t0, 1
    bnez    t0, 1f
    sd      sp, CPU_OVERFLOW_SP(tp)     /* stack overflow: switch stacks */
    ld      sp, CPU_OVERFLOW_STACK(tp)
1:  ld      t0, CPU_SCRATCH(tp)
    addi    sp, sp, -FRAME_SIZE
    .cfi_def_cfa_offset FRAME_SIZE
    SAVE_GPRS
    addi    t0, sp, FRAME_SIZE
    sd      t0, 2*8(sp)                 /* interrupted sp */
    sd      s0, FRAME_REGS(sp)          /* fake frame record: prev fp */
    j       .Lsave_csrs

    /* ---- from user mode: tp = struct cpu *, sscratch = user tp ---- */
.Lfrom_user:
    sd      t0, CPU_SCRATCH(tp)
    mv      t0, sp
    ld      sp, CPU_KSTACK_TOP(tp)
    addi    sp, sp, -FRAME_SIZE
    sd      t0, 2*8(sp)                 /* user sp */
    ld      t0, CPU_SCRATCH(tp)
    SAVE_GPRS
    csrr    t0, sscratch
    sd      t0, 4*8(sp)                 /* user tp */
    csrw    sscratch, zero
    sd      zero, FRAME_REGS(sp)        /* fake frame record ends the chain */

.Lsave_csrs:
    sd      zero, 0(sp)
    csrr    t0, sepc
    csrr    t1, sstatus
    csrr    t2, scause
    csrr    t3, stval
    sd      t0, TF_SEPC(sp)
    sd      t1, TF_SSTATUS(sp)
    sd      t2, TF_SCAUSE(sp)
    sd      t3, TF_STVAL(sp)
    sd      t0, FRAME_REGS+8(sp)        /* fake frame record: ra = sepc */
    addi    s0, sp, FRAME_SIZE

    .cfi_offset 64, TF_SEPC - FRAME_SIZE
    .cfi_offset 2, 2*8 - FRAME_SIZE
    .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .cfi_offset \n, \n*8 - FRAME_SIZE
    .endr

    mv      a0, sp
    call    trap_handler

.Ltrap_return:
    /* The handler may have changed sepc/sstatus (skip ecall/ebreak, exec). */
    ld      t0, TF_SEPC(sp)
    ld      t1, TF_SSTATUS(sp)
    csrw    sepc, t0
    csrw    sstatus, t1
    andi    t1, t1, SSTATUS_SPP
    bnez    t1, 2f
    csrw    sscratch, tp                /* returning to user: remember our tp */
    j       3f
2:  /*
     * Returning to kernel code: keep the live tp. The interrupted thread may
     * have been preempted here and resumed on another CPU, so the tp saved
     * in the frame can belong to the CPU it ran on before.
     */
    sd      tp, 4*8(sp)
3:
    ld      x1,  1*8(sp)
    ld      x3,  3*8(sp)
    ld      x4,  4*8(sp)
    ld      x5,  5*8(sp)
    ld      x6,  6*8(sp)
    ld      x7,  7*8(sp)
    ld      x8,  8*8(sp)
    ld      x9,  9*8(sp)
    ld      x10, 10*8(sp)
    ld      x11, 11*8(sp)
    ld      x12, 12*8(sp)
    ld      x13, 13*8(sp)
    ld      x14, 14*8(sp)
    ld      x15, 15*8(sp)
    ld      x16, 16*8(sp)
    ld      x17, 17*8(sp)
    ld      x18, 18*8(sp)
    ld      x19, 19*8(sp)
    ld      x20, 20*8(sp)
    ld      x21, 21*8(sp)
    ld      x22, 22*8(sp)
    ld      x23, 23*8(sp)
    ld      x24, 24*8(sp)
    ld      x25, 25*8(sp)
    ld      x26, 26*8(sp)
    ld      x27, 27*8(sp)
    ld      x28, 28*8(sp)
    ld      x29, 29*8(sp)
    ld      x30, 30*8(sp)
    ld      x31, 31*8(sp)
    ld      sp,  2*8(sp)
    sret
    .cfi_endproc

/*
 * void user_return(struct trap_frame *tf)
 * Enter user mode with the registers in tf (a fresh exec or a forked child).
 * Interrupts must be off.
 */
    .globl user_return
    .balign 4
user_return:
    mv      sp, a0
    j       .Ltrap_return
