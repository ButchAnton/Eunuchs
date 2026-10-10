/*
 * Fault-tolerant accessors (see safe.h). Each faulting instruction has an
 * __ex_table entry mapping it to a fixup that returns -1.
 */

    .section .text

.macro SAFE_READ name, load, store
    .globl \name
    .balign 4
\name:
1:  \load   t0, 0(a0)
    \store  t0, 0(a1)
    li      a0, 0
    ret
2:  li      a0, -1
    ret
    .pushsection __ex_table, "a"
    .balign 8
    .dword  1b, 2b
    .popsection
.endm

.macro SAFE_WRITE name, store
    .globl \name
    .balign 4
\name:
1:  \store  a1, 0(a0)
    li      a0, 0
    ret
2:  li      a0, -1
    ret
    .pushsection __ex_table, "a"
    .balign 8
    .dword  1b, 2b
    .popsection
.endm

SAFE_READ  safe_read_u8,  lbu, sb
SAFE_READ  safe_read_u16, lhu, sh
SAFE_READ  safe_read_u32, lwu, sw
SAFE_READ  safe_read_u64, ld,  sd
SAFE_WRITE safe_write_u8,  sb
SAFE_WRITE safe_write_u16, sh
SAFE_WRITE safe_write_u32, sw
SAFE_WRITE safe_write_u64, sd
