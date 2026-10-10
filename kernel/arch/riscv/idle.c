#include <irq.h>
#include <riscv.h>
#include <sbi.h>

/*
 * Sleep until a timer `ticks` from now; may wake early. Used by polling code
 * that runs with interrupts off (the debug monitor). wfi wakes on a pending
 * interrupt that is enabled in sie even while sstatus.SIE is off, so this
 * never actually takes a trap. Only the timer is enabled during the wait: a
 * device interrupt left pending in the PLIC would otherwise make wfi return
 * immediately and turn this into a busy loop.
 */
void cpu_wait_ticks(u64 ticks)
{
    bool irqs = intr_enabled();
    intr_off();
    unsigned long saved_sie = csr_read(sie);

    csr_write(sie, SIE_STIE);
    sbi_set_timer(rdtime() + ticks);
    asm volatile("wfi");
    if (timer_running)
        timer_rearm();              /* hand the timer back to the tick */
    else
        sbi_set_timer(~0ULL);       /* clears the pending timer interrupt */

    csr_write(sie, saved_sie);
    if (irqs)
        intr_on();
}
