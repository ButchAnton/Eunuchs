/*
 * eboot's entry. OpenSBI enters here in S-mode with the MMU off: a0 = hart
 * id, a1 = the device tree. Only the boot hart comes here (the others wait in
 * OpenSBI until the kernel starts them). Set up a stack and go to C, which
 * does not return: it jumps to the kernel or powers off.
 */
    .section .text.start
    .globl _start
_start:
    csrw    sie, zero
    la      sp, eboot_stack_top
    mv      fp, zero
    call    eboot_main
1:  wfi
    j       1b

    .bss
    .balign 16
    .space  16384
eboot_stack_top:
