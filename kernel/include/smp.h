#ifndef EUNUCHS_SMP_H
#define EUNUCHS_SMP_H

/* Multiprocessor support: see kernel/arch/riscv/smp.c. */

#include <percpu.h>
#include <types.h>

struct trap_frame;

extern const char *const ipi_names[IPI_MAX];
extern u64 tlb_shootdowns;

void smp_init(void);                        /* start the secondary harts */
void smp_send_ipi(int cpu, enum ipi_kind kind);
void ipi_handle(struct trap_frame *tf);     /* supervisor software interrupt */
void ipi_poll(void);                        /* from spin loops, interrupts off */

/* Flush all CPUs' TLBs (after removing or changing a kernel mapping). */
void tlb_shootdown(void);

/* Stop-the-world for the monitor and panics (see smp.c). */
bool smp_stop_others(u32 *missing);
void smp_resume_others(void);
void smp_park_self(struct trap_frame *tf);
bool smp_stopped_by_me(void);
bool smp_world_stopped(void);

/* Debugging. */
void cpus_print(int (*print)(const char *fmt, ...));
int cpu_backtrace(int (*print)(const char *fmt, ...), int cpu);

/* timer.c */
void timer_init_cpu(void);
void watchdog_check(void);

#endif
