/*
 * Kernel entry. OpenSBI enters here in S-mode, MMU off, at the physical load
 * address (0x80200000), with a0 = hart id, a1 = physical address of the DTB.
 * Only the boot hart arrives here; the others stay parked in OpenSBI until
 * smp_init() starts them via SBI HSM at secondary_entry (below).
 *
 * The kernel is linked at KERNEL_BASE + PA. Until paging is on, only
 * PC-relative code may run (no absolute addresses, no C).
 *
 * Early page table (1 GiB pages, all RWX; replaced by vm_init()):
 *   - identity map of the GiB containing the kernel, so the instructions
 *     right after enabling paging still execute
 *   - KERNEL_BASE + [0, 64 GiB) -> [0, 64 GiB): the direct map, which also
 *     covers MMIO and the DTB
 */
#include <mm.h>
#include <percpu.h>

#define PTE_EARLY        0xcf   /* V R W X A D */
#define PTE_EARLY_GLOBAL 0xef   /* V R W X G A D */
#define EARLY_GIGAPAGES  64
#define KERNEL_ROOT_IDX  256    /* PX(2, KERNEL_BASE) */

    .section .text.boot
    .globl _start
_start:
    csrw    sie, zero
    csrci   sstatus, 2
    csrw    sscratch, zero      /* trap entry: sscratch == 0 means "trap from kernel" */
    mv      s1, a0              /* hart id  */
    mv      s2, a1              /* DTB (PA) */

    /* Zero BSS (pc-relative la yields physical addresses here). */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    la      t0, early_pagetable

    /* Identity gigapage for the kernel's load address. */
    la      t1, _start
    srli    t1, t1, 30          /* gigapage number */
    slli    t2, t1, 28          /* PTE: PPN = gp << 18, shifted left 10 */
    ori     t2, t2, PTE_EARLY
    slli    t3, t1, 3
    add     t3, t0, t3
    sd      t2, 0(t3)

    /* Direct map: root entries 256.. -> physical gigapages 0.. */
    li      t3, KERNEL_ROOT_IDX * 8
    add     t3, t0, t3
    li      t1, 0
    li      t4, EARLY_GIGAPAGES
3:  slli    t2, t1, 28
    ori     t2, t2, PTE_EARLY_GLOBAL
    sd      t2, 0(t3)
    addi    t3, t3, 8
    addi    t1, t1, 1
    blt     t1, t4, 3b

    /* satp = Sv39 | root PPN */
    srli    t0, t0, 12
    li      t1, 8
    slli    t1, t1, 60
    or      t0, t0, t1
    sfence.vma
    csrw    satp, t0
    sfence.vma

    /* Jump to the linked (high) address. */
    ld      t0, .Lhigh_addr
    jr      t0

.Lhigh:
    /* Now running at KERNEL_BASE + PA; la yields high addresses. */
    la      tp, cpus            /* per-CPU data (hart 0 = cpus[0]) */
    la      sp, boot_stack_top
    mv      fp, zero            /* terminates frame-pointer backtraces */
    mv      a0, s1
    mv      a1, s2
    call    kmain

    /* kmain must not return. */
4:  wfi
    j       4b

    .balign 8
.Lhigh_addr:
    .dword  .Lhigh

/*
 * Secondary harts, started by smp_init() with sbi_hart_start(): S-mode, MMU
 * off, a0 = hart id, a1 = opaque = this hart's struct cpu (kernel VA), whose
 * kstack_top holds the sp to use (the top of its idle thread's stack).
 *
 * The early page table is still intact (vm_init never modifies it), and its
 * identity gigapage keeps the next instructions mapped while paging comes
 * on. Once running high, switch to the real kernel page table: the stack
 * lives in KSTACK_AREA, which only kernel_pagetable maps.
 */
    .globl secondary_entry
    .balign 4
secondary_entry:
    csrw    sie, zero
    csrci   sstatus, 2
    csrw    sscratch, zero
    la      t0, early_pagetable
    srli    t0, t0, 12
    li      t1, 8
    slli    t1, t1, 60
    or      t0, t0, t1
    sfence.vma
    csrw    satp, t0
    sfence.vma
    ld      t0, .Lsecondary_high_addr
    jr      t0

.Lsecondary_high:
    la      t0, kernel_pagetable
    ld      t0, 0(t0)           /* root table, direct-map VA */
    li      t1, KERNEL_BASE
    sub     t0, t0, t1
    srli    t0, t0, 12
    li      t1, 8
    slli    t1, t1, 60
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
    mv      tp, a1
    ld      sp, CPU_KSTACK_TOP(tp)
    mv      fp, zero
    mv      a0, tp
    call    secondary_main
5:  wfi
    j       5b

    .balign 8
.Lsecondary_high_addr:
    .dword  .Lsecondary_high

    .section .bss
    .balign 4096
    .globl early_pagetable
early_pagetable:
    .space  4096

    /*
     * Stacks live in KSTACK_SLOT-aligned slots whose lower half is an
     * unmapped guard (see mm.h); vm_init() maps only the upper halves.
     */
    .section .stacks, "aw", @nobits
    .balign KSTACK_SLOT
    .space  KSTACK_SLOT - KSTACK_SIZE   /* guard */
    .globl boot_stack
boot_stack:
    .space  KSTACK_SIZE
    .globl boot_stack_top
boot_stack_top:

    .space  KSTACK_SLOT - KSTACK_SIZE   /* guard */
    .globl overflow_stack
overflow_stack:
    .space  KSTACK_SIZE
    .globl overflow_stack_top
overflow_stack_top:
