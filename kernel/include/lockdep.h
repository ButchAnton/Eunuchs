#ifndef EUNUCHS_LOCKDEP_H
#define EUNUCHS_LOCKDEP_H

/*
 * Lock dependency checking ("lockdep-lite"). See kernel/debug/lockdep.c.
 *
 * Locks are grouped into classes by name and kind (every inode mutex is
 * class "inode"). Taking lock B while holding lock A records the dependency
 * A -> B. If B -> ... -> A was seen before, two threads taking these locks
 * concurrently can deadlock, even if it has never happened yet: lockdep
 * reports the inversion with the sites that established both orders.
 *
 * Boot option lockdep=off|warn|panic (default warn: report once, then stop
 * checking; `make test` uses panic).
 */

#include <types.h>

enum { LD_SPIN, LD_MUTEX };

void lockdep_init(void);
/* Before acquiring (not for trylocks: they cannot deadlock). */
void lockdep_acquire(u16 *class_cache, const char *name, int kind, uintptr_t pc);
/* A sleeping lock may not be taken with spinlocks held. */
void lockdep_might_sleep(const char *what, uintptr_t pc);
void lockdep_print(int (*print)(const char *fmt, ...));

extern bool lockdep_enabled;
extern u64 lockdep_reports;
extern bool lockdep_testing;    /* ktest: count reports, don't panic or switch off */

#endif
