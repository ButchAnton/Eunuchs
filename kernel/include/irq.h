#ifndef EUNUCHS_IRQ_H
#define EUNUCHS_IRQ_H

/* External interrupts (PLIC) and the timer. */

#include <types.h>

#define MAX_IRQS 64
#define HZ       100            /* timer ticks per second */

typedef void (*irq_handler_t)(void *arg);

void plic_init(void);
int irq_register(u32 irq, const char *name, irq_handler_t handler, void *arg);
void plic_handle(void);
void irq_print(int (*print)(const char *fmt, ...));

void timer_init(void);
void timer_interrupt(void);
void timer_rearm(void);
extern volatile u64 jiffies;
extern bool timer_running;

static inline u64 ms_to_ticks(u64 ms, u64 timebase) { return ms * timebase / 1000; }

/* Semihosting (QEMU -semihosting-config): host file I/O for trace dumps. */
long semihost_open(const char *path, int mode);
long semihost_write(long fd, const void *buf, size_t len);
long semihost_close(long fd);
#define SEMIHOST_MODE_WB 5

#endif
