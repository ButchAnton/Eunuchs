/*
 * Saving and restoring the user floating-point registers. The kernel is
 * built without floating point, so this file turns the D extension on for
 * itself; sstatus.FS must be non-zero for these instructions, so both
 * routines enable it first.
 */
    .option push
    .option arch, +d

    .text
    .globl fp_save_regs
fp_save_regs:                   /* a0: struct fpstate * */
    li      t0, 1 << 13
    csrs    sstatus, t0
    fsd     f0, 0(a0)
    fsd     f1, 8(a0)
    fsd     f2, 16(a0)
    fsd     f3, 24(a0)
    fsd     f4, 32(a0)
    fsd     f5, 40(a0)
    fsd     f6, 48(a0)
    fsd     f7, 56(a0)
    fsd     f8, 64(a0)
    fsd     f9, 72(a0)
    fsd     f10, 80(a0)
    fsd     f11, 88(a0)
    fsd     f12, 96(a0)
    fsd     f13, 104(a0)
    fsd     f14, 112(a0)
    fsd     f15, 120(a0)
    fsd     f16, 128(a0)
    fsd     f17, 136(a0)
    fsd     f18, 144(a0)
    fsd     f19, 152(a0)
    fsd     f20, 160(a0)
    fsd     f21, 168(a0)
    fsd     f22, 176(a0)
    fsd     f23, 184(a0)
    fsd     f24, 192(a0)
    fsd     f25, 200(a0)
    fsd     f26, 208(a0)
    fsd     f27, 216(a0)
    fsd     f28, 224(a0)
    fsd     f29, 232(a0)
    fsd     f30, 240(a0)
    fsd     f31, 248(a0)
    frcsr   t1
    sd      t1, 256(a0)
    ret

    .globl fp_load_regs
fp_load_regs:                   /* a0: const struct fpstate * */
    li      t0, 1 << 13
    csrs    sstatus, t0
    fld     f0, 0(a0)
    fld     f1, 8(a0)
    fld     f2, 16(a0)
    fld     f3, 24(a0)
    fld     f4, 32(a0)
    fld     f5, 40(a0)
    fld     f6, 48(a0)
    fld     f7, 56(a0)
    fld     f8, 64(a0)
    fld     f9, 72(a0)
    fld     f10, 80(a0)
    fld     f11, 88(a0)
    fld     f12, 96(a0)
    fld     f13, 104(a0)
    fld     f14, 112(a0)
    fld     f15, 120(a0)
    fld     f16, 128(a0)
    fld     f17, 136(a0)
    fld     f18, 144(a0)
    fld     f19, 152(a0)
    fld     f20, 160(a0)
    fld     f21, 168(a0)
    fld     f22, 176(a0)
    fld     f23, 184(a0)
    fld     f24, 192(a0)
    fld     f25, 200(a0)
    fld     f26, 208(a0)
    fld     f27, 216(a0)
    fld     f28, 224(a0)
    fld     f29, 232(a0)
    fld     f30, 240(a0)
    fld     f31, 248(a0)
    ld      t1, 256(a0)
    fscsr   t1
    ret

    .option pop
