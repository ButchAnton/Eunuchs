#ifndef EUNUCHS_SPINLOCK_H
#define EUNUCHS_SPINLOCK_H

/*
 * Spinlocks. Acquiring disables interrupts; the saved state is returned to
 * the caller (irqsave style), so nested locks may be released in any order.
 *
 * Debugging: each lock records its owner CPU and the caller PC that took it;
 * each CPU keeps a stack of the locks it holds (`locks` in the monitor, and
 * panics print it). Errors detected:
 *   - recursive acquisition on the same CPU (immediate panic, names both sites)
 *   - unlock of a lock this CPU does not hold
 *   - a lock that cannot be taken within SPIN_TIMEOUT spins (deadlock report)
 *   - schedule() while holding a spinlock (see sched.c)
 *   - lock-order inversions between any two lock classes (lockdep.c)
 *
 * While spinning, a CPU services TLB-shootdown and stop requests
 * (ipi_poll), so it can't deadlock a shootdown and the monitor can still
 * show where it was spinning.
 *
 * Debug paths (monitor, panic) use spin_trylock_irqsave() and carry on
 * without the lock if it is held, rather than deadlock on a wedged kernel.
 */

#include <types.h>

typedef struct spinlock {
    volatile u32 locked;
    int cpu;                /* owner CPU id, -1 if free */
    const char *name;
    uintptr_t owner_pc;     /* caller that acquired it */
    u64 acquisitions;
    u64 contentions;
    u16 ld_class;           /* lockdep class, assigned on first use */
} spinlock_t;

#define SPINLOCK_INIT(n) \
    { .locked = 0, .cpu = -1, .name = (n), .owner_pc = 0, .acquisitions = 0, .contentions = 0, \
      .ld_class = 0 }

static inline void spin_init(spinlock_t *l, const char *name)
{
    *l = (spinlock_t)SPINLOCK_INIT(name);
}

unsigned long spin_lock_irqsave(spinlock_t *l);
bool spin_trylock_irqsave(spinlock_t *l, unsigned long *flags);
void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags);
bool spin_held_by_me(const spinlock_t *l);

static inline bool spin_is_locked(const spinlock_t *l) { return l->locked; }

/*
 * For debug output (ps, traces, procfs): take the lock normally, but only
 * try it while panicking or with the world stopped (its holder may be
 * parked mid-update). Returns whether the lock was taken; release with
 * spin_unlock_debug().
 */
bool spin_lock_debug(spinlock_t *l, unsigned long *flags);
void spin_unlock_debug(spinlock_t *l, bool locked, unsigned long flags);

/* Print the spinlocks held by every CPU (this one first). */
void spin_print_held(int (*print)(const char *fmt, ...));
void spin_print_held_cpu(int (*print)(const char *fmt, ...), int cpu);

#endif
