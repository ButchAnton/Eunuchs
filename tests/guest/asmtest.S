/* Assembled by tcc inside Eunuchs, from the host share (tools/run_tests.py,
   "host share (9p)"), and linked with tests/guest/asmtest.c. */
    .macro ADDK reg, k=1
    addi    \reg, \reg, \k
    .endm

    .text
    .globl  asm_sum, asm_shifts, asm_big, asm_table_sum, asm_atomic_inc, asm_call_twice

/* long asm_sum(long n): n + (n-1) + ... + 1, with local labels both ways */
asm_sum:
    mv      t0, zero
1:  beqz    a0, 2f
    add     t0, t0, a0
    ADDK    a0, -1
    j       1b
2:  mv      a0, t0
    ret

/* long asm_shifts(long x): srl ^ sra ^ sraw ^ sraiw */
asm_shifts:
    li      t1, 4
    srl     t2, a0, t1
    sra     t3, a0, t1
    sraw    t4, a0, t1
    sraiw   t5, a0, 3
    xor     a0, t2, t3
    xor     a0, a0, t4
    xor     a0, a0, t5
    ret

/* long asm_big(void) */
asm_big:
    li      a0, 0x123456789abcdef0
    ret

/* long asm_table_sum(void): la of data, .irp */
asm_table_sum:
    la      t0, table
    mv      a0, zero
    .irp    i, 0, 1, 2, 3
    ld      t1, \i*8(t0)
    add     a0, a0, t1
    .endr
    ret

/* long asm_atomic_inc(long *p): an lr/sc loop; returns the new value */
asm_atomic_inc:
1:  lr.d    t0, (a0)
    ADDK    t0
    sc.d    t1, t0, (a0)
    bnez    t1, 1b
    mv      a0, t0
    ret

/* long asm_call_twice(long x): twice(twice(x)), calling back into C */
asm_call_twice:
    addi    sp, sp, -16
    sd      ra, 8(sp)
    sd      fp, 0(sp)
    call    twice
    call    twice
    ld      fp, 0(sp)
    ld      ra, 8(sp)
    addi    sp, sp, 16
    ret

    .data
table:
    .dword  1, 20, 300, 4000
