#include <debug.h>
#include <fmt.h>
#include <lockdep.h>
#include <percpu.h>
#include <smp.h>
#include <riscv.h>
#include <spinlock.h>

#define SPIN_TIMEOUT 100000000UL

static unsigned long irq_save(void)
{
    unsigned long flags = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    return flags;
}

static void irq_restore(unsigned long flags)
{
    if (flags)
        intr_on();
}

static void push_held(spinlock_t *l)
{
    struct cpu *c = this_cpu();
    if (c->nheld < MAX_HELD_LOCKS)
        c->held[c->nheld] = l;
    c->nheld++;
}

static void pop_held(spinlock_t *l)
{
    struct cpu *c = this_cpu();
    int n = MIN(c->nheld, MAX_HELD_LOCKS);
    /* Usually the innermost; release order may differ, so search. */
    for (int i = n - 1; i >= 0; i--) {
        if (c->held[i] == l) {
            for (int j = i; j < n - 1; j++)
                c->held[j] = c->held[j + 1];
            break;
        }
    }
    c->nheld--;
}

static void acquired(spinlock_t *l, uintptr_t pc)
{
    l->cpu = this_cpu()->id;
    l->owner_pc = pc;
    l->acquisitions++;
    push_held(l);
}

unsigned long spin_lock_irqsave(spinlock_t *l)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    unsigned long flags = irq_save();
    unsigned long spins = 0;

    if (l->locked && l->cpu == this_cpu()->id && panic_depth == 0) {
        char a[96], b[96];
        ksym_format(a, sizeof(a), l->owner_pc);
        ksym_format(b, sizeof(b), pc);
        panic("recursive spinlock: '%s' already held by this CPU (taken at %s), "
              "acquired again at %s", l->name ? l->name : "?", a, b);
    }

    lockdep_acquire(&l->ld_class, l->name, LD_SPIN, pc);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        if (spins++ == 0)
            l->contentions++;
        ipi_poll();
        if (spins == SPIN_TIMEOUT) {
            char a[96];
            ksym_format(a, sizeof(a), l->owner_pc);
            panic("deadlock: spinlock '%s' held by cpu %d at %s never released",
                  l->name ? l->name : "?", l->cpu, a);
        }
    }
    acquired(l, pc);
    return flags;
}

bool spin_trylock_irqsave(spinlock_t *l, unsigned long *flags)
{
    *flags = irq_save();
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        irq_restore(*flags);
        return false;
    }
    acquired(l, (uintptr_t)__builtin_return_address(0));
    return true;
}

void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags)
{
    if (!l->locked || l->cpu != this_cpu()->id) {
        if (panic_depth == 0)
            panic("unlock of spinlock '%s' not held by this CPU (locked=%u owner cpu %d)",
                  l->name ? l->name : "?", l->locked, l->cpu);
        return;
    }
    pop_held(l);
    l->owner_pc = 0;
    l->cpu = -1;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

bool spin_lock_debug(spinlock_t *l, unsigned long *flags)
{
    if (panic_depth || smp_world_stopped() || spin_held_by_me(l))
        return spin_trylock_irqsave(l, flags);
    *flags = spin_lock_irqsave(l);
    return true;
}

void spin_unlock_debug(spinlock_t *l, bool locked, unsigned long flags)
{
    if (locked)
        spin_unlock_irqrestore(l, flags);
}

bool spin_held_by_me(const spinlock_t *l)
{
    /* One load of our CPU id: we may migrate, but never while holding l. */
    return l->locked && l->cpu == this_cpu_id();
}

void spin_print_held_cpu(print_fn print, int cpu)
{
    struct cpu *c = &cpus[cpu];
    if (c->nheld == 0) {
        print("  cpu%d holds no spinlocks\n", c->id);
        return;
    }
    for (int i = 0; i < MIN(c->nheld, MAX_HELD_LOCKS); i++) {
        char s[96];
        ksym_format(s, sizeof(s), c->held[i]->owner_pc);
        print("  cpu%d holds '%s' (taken at %s)\n", c->id,
              c->held[i]->name ? c->held[i]->name : "?", s);
    }
}

void spin_print_held(print_fn print)
{
    struct cpu *me = this_cpu();
    spin_print_held_cpu(print, me->id);
    for (int i = 0; i < ncpus_online; i++)
        if (&cpus[i] != me)
            spin_print_held_cpu(print, i);
}
