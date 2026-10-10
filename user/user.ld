/* User programs: static, loaded at 0x10000, page-aligned segments (W^X). */
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);      /* r-x */
    rodata PT_LOAD FLAGS(4);    /* r-- */
    data PT_LOAD FLAGS(6);      /* rw- */
}

SECTIONS
{
    . = 0x10000;
    .text : { *(.text.start) *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.srodata .srodata.*) *(.rodata .rodata.*) } :rodata
    . = ALIGN(4096);
    .data : { *(.sdata .sdata.*) *(.data .data.*) } :data
    .bss : {
        __bss_start = .;
        *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
        __bss_end = .;
    } :data
    PROVIDE(__global_pointer$ = __bss_start);
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
