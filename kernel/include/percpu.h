#ifndef EUNUCHS_PERCPU_H
#define EUNUCHS_PERCPU_H

/*
 * Per-CPU state. The tp register always points at the running hart's
 * struct cpu while in the kernel (boot.S, secondary_entry; trap entry swaps
 * it in from sscratch for traps from user mode).
 *
 * Threads migrate between CPUs, so a thread with interrupts enabled may be
 * preempted between reading tp and using the struct it points to. Code that
 * runs with interrupts on must therefore read per-CPU fields with a single
 * tp-relative load (current, this_cpu_id(), in_interrupt() below); only
 * code with interrupts off (trap handlers, spinlock holders, the scheduler)
 * may hold on to this_cpu().
 *
 * The first fields are used by trapentry.S and boot.S; keep the CPU_*
 * offsets in sync (checked by static asserts in sched.c).
 */

#define CPU_OVERFLOW_STACK  8   /* offsetof(struct cpu, overflow_stack_top) */
#define CPU_OVERFLOW_SP     16  /* offsetof(struct cpu, overflow_sp) */
#define CPU_SCRATCH         24  /* offsetof(struct cpu, scratch) */
#define CPU_KSTACK_TOP      32  /* offsetof(struct cpu, kstack_top) */

#ifndef __ASSEMBLY__

#include <types.h>

#define MAX_CPUS        8
#define MAX_HELD_LOCKS  8

/* Inter-processor interrupt kinds (smp.c). */
enum ipi_kind {
    IPI_RESCHED,        /* run the scheduler: work was queued */
    IPI_TLB_FLUSH,      /* sfence.vma, then acknowledge (TLB shootdown) */
    IPI_STOP,           /* park until released (debug monitor, panic) */
    IPI_MAX,
};

struct thread;
struct spinlock;
struct trap_frame;

struct cpu {
    struct thread *cur_thread;      /* must be first: `current` is ld 0(tp) */
    uintptr_t overflow_stack_top;   /* emergency stack for kernel stack overflows */
    uintptr_t overflow_sp;          /* sp at the overflowing trap; 0 normally */
    uintptr_t scratch;              /* trap entry temporary */
    uintptr_t kstack_top;           /* current thread's kernel stack top (user traps);
                                       before a secondary starts: its boot sp */
    unsigned long hartid;
    int id;
    int irq_depth;                  /* > 0 inside an interrupt handler */
    bool need_resched;
    bool in_overflow;
    volatile bool online;           /* running the scheduler */
    struct trap_frame *irq_tf;      /* frame of the interrupted context */
    u32 last_irq;                   /* PLIC source being handled (tracing) */

    /* Spinlocks held right now, innermost last (debugging, `locks`). */
    int nheld;
    struct spinlock *held[MAX_HELD_LOCKS];
    bool in_lockdep;                /* lockdep reporting: don't recurse */

    struct thread *idle;
    struct thread *switch_from;     /* thread that performed the last switch */
    struct thread *prev_dead;       /* dead thread to finish after a switch */
    bool prev_dead_free;            /* ...and free it (it was detached) */
    u64 last_switch;                /* rdtime at the last context switch */
    u64 switches;
    u64 migrations;                 /* threads that last ran elsewhere */

    /* Timer tick (timer.c) and the lockup watchdog. */
    u64 next_deadline;
    u64 ticks;
    u64 missed_ticks;
    u32 subticks;                   /* timer interrupts since the last scheduler tick */
    volatile u64 last_tick;         /* rdtime of the last tick: heartbeat */
    bool lockup_reported;

    /* Interrupt statistics (/proc/interrupts, `irq`, `cpus`). */
    u64 irq_count[16];              /* by scause interrupt code (IRQ_MAX) */
    u64 ipi_sent[IPI_MAX];
    u64 ipi_received[IPI_MAX];

    /* IPIs: bits of enum ipi_kind waiting to be handled. */
    volatile u32 ipi_pending;
    volatile u64 tlb_done_gen;      /* last shootdown generation flushed */

    /* Stop-the-world (monitor/panic): where this CPU was when it parked. */
    volatile bool stopped;
    struct trap_frame *stop_tf;     /* when stopped from an interrupt */
    uintptr_t stop_pc, stop_fp;     /* when stopped from a spin loop */
};

extern struct cpu cpus[MAX_CPUS];
extern int ncpus_online;

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    asm volatile("mv %0, tp" : "=r"(c));
    return c;
}

/* The running thread; NULL before sched_init(). One load: migration-safe. */
static inline struct thread *get_current(void)
{
    struct thread *t;
    asm volatile("ld %0, 0(tp)" : "=r"(t));
    return t;
}
#define current (get_current())

static inline int this_cpu_id(void)
{
    int id;
    asm volatile("lw %0, %1(tp)" : "=r"(id) : "i"(offsetof(struct cpu, id)));
    return id;
}

static inline bool in_interrupt(void)
{
    int depth;
    asm volatile("lw %0, %1(tp)" : "=r"(depth) : "i"(offsetof(struct cpu, irq_depth)));
    return depth > 0;
}

#endif /* __ASSEMBLY__ */
#endif
