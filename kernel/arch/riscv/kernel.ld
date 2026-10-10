/*
 * The kernel is linked at its direct-map address (KERNEL_BASE + 0x80200000)
 * and loaded at physical 0x80200000, where OpenSBI enters it in S-mode with
 * the MMU off. boot.S turns paging on and jumps up.
 *
 * Section boundaries are page-aligned so vm_init() can map each with its own
 * permissions (W^X).
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

KERNEL_BASE = 0xffffffc000000000;
KERNEL_PHYS = 0x80200000;

SECTIONS
{
    . = KERNEL_BASE + KERNEL_PHYS;
    PROVIDE(__kernel_start = .);

    .text : AT(KERNEL_PHYS) {
        *(.text.boot)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    PROVIDE(__text_end = .);
    PROVIDE(__rodata_start = .);

    .rodata : {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)

        /* In-kernel unit tests (KTEST) */
        . = ALIGN(8);
        PROVIDE(__ktests_start = .);
        KEEP(*(.ktests))
        PROVIDE(__ktests_end = .);

        /* Exception table: {faulting insn, fixup} pairs (safe.S, later copyin/out) */
        . = ALIGN(8);
        PROVIDE(__ex_table_start = .);
        KEEP(*(__ex_table))
        PROVIDE(__ex_table_end = .);
    }

    . = ALIGN(4096);
    PROVIDE(__rodata_end = .);
    PROVIDE(__data_start = .);

    .data : {
        *(.sdata .sdata.*)
        *(.data .data.*)
    }

    .bss : {
        PROVIDE(__bss_start = .);
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE(__bss_end = .);
    }

    . = ALIGN(4096);
    PROVIDE(__data_end = .);

    /* Boot and emergency stacks: 32 KiB-aligned slots, lower half = guard. */
    . = ALIGN(32768);
    .stacks (NOLOAD) : {
        PROVIDE(__stacks_start = .);
        *(.stacks)
        PROVIDE(__stacks_end = .);
    }

    . = ALIGN(4096);
    PROVIDE(__ksyms_start = .);

    /*
     * Symbol table, generated between link passes. Kept last so its size
     * cannot move any other symbol.
     */
    .ksyms : {
        KEEP(*(.ksyms))
    }

    . = ALIGN(4096);
    PROVIDE(__kernel_end = .);
}
