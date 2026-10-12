/*
 * eboot runs where it is loaded: physical 0x82000000 (32 MiB into RAM),
 * above the kernel it loads at 0x80200000, which must end below it.
 * GNU ld and kld (tools/kld) both read this.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x82000000;
    PROVIDE(__eboot_start = .);

    .text : {
        *(.text.start)
        *(.text .text.*)
    }
    .rodata : {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
        *(.data.ro .data.ro.*)
    }
    .data : {
        *(.sdata .sdata.*)
        *(.data .data.*)
    }
    . = ALIGN(8);
    PROVIDE(__bss_start = .);
    .bss : {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(8);
    PROVIDE(__eboot_end = .);
}
