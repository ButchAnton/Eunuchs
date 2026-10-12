#ifndef EUNUCHS_SCHED_H
#define EUNUCHS_SCHED_H

/*
 * Kernel threads and the scheduler (round-robin, preemptive on timer ticks),
 * plus the sleeping primitives built on it: wait queues, mutexes, semaphores
 * and timed sleep.
 *
 * Debugging: `ps`, `bt <tid>`, `sched` and `locks` in the monitor; eu-ps and
 * eu-bt in GDB; sched_switch/wakeup/... trace events. Misuse is caught:
 * sleeping in interrupt context or with a spinlock held, recursive or foreign
 * mutex unlock, and mutex deadlock cycles (reported with the whole chain).
 */

#include <list.h>
#include <percpu.h>
#include <spinlock.h>
#include <types.h>

enum thread_state {
    T_RUNNING,      /* on a CPU */
    T_RUNNABLE,     /* on the run queue */
    T_SLEEPING,     /* waiting for an event */
    T_DEAD,         /* exited; waiting to be joined/reaped */
};

/* Callee-saved registers, saved by context_switch (switch.S). */
struct context {
    u64 ra;
    u64 sp;
    u64 s[12];
};

struct waitqueue {
    spinlock_t lock;
    struct list_head waiters;
    const char *name;
};

struct mutex;
struct proc;

#define MAX_HELD_MUTEX 8

/* User floating-point state (f0-f31 and fcsr), saved at context switches. */
struct fpstate {
    u64 f[32];
    u64 fcsr;
};
void fp_save_regs(struct fpstate *fp);          /* fp.S: hardware -> memory */
void fp_load_regs(const struct fpstate *fp);    /* fp.S: memory -> hardware */

struct thread {
    struct context ctx;             /* must be first (switch.S) */
    bool fp_used;                   /* a user thread that has used floating point */
    struct fpstate fp;              /* its FP registers while it isn't running */
    int tid;
    char name[16];
    enum thread_state state;
    bool on_runq;
    bool on_cpu;
    bool detached;
    volatile bool exit_done;        /* the post-exit wakeup is finished: may be freed */

    void *kstack;                   /* lowest usable address */
    size_t kstack_size;
    struct proc *proc;              /* NULL for kernel threads */
    int fs_op_depth;                /* nesting of filesystem transactions */
    bool interruptible;             /* in an interruptible sleep: signals wake it */

    void (*fn)(void *);
    void *arg;
    int exit_code;
    struct waitqueue exit_wq;

    /* What a sleeping thread waits for (shown by `ps`). */
    const char *wait_what;
    const void *wait_obj;
    struct mutex *blocked_on;
    u64 blocked_since;              /* rdtime when it started waiting for blocked_on */
    bool hung_reported;             /* the hung-thread detector reported this wait */

    /* Mutexes held now, innermost last (lockdep, `locks`). */
    int nheld_mutex;
    struct mutex *held_mutex[MAX_HELD_MUTEX];

    int cpu;                        /* CPU it is running on, or last ran on */
    int bound_cpu;                  /* runs only there (kthread_bind), or -1 */
    u64 migrations;                 /* times it moved to another CPU */

    u64 wake_time;                  /* rdtime deadline for timed sleep */
    bool on_sleepq;

    struct list_head all_node;
    struct list_head run_node;
    struct list_head sleep_node;

    u64 runtime;                    /* rdtime ticks spent running */
    u64 switches;                   /* times switched in */
    u64 created;                    /* rdtime at creation */
    uintptr_t create_site;
};

extern struct list_head all_threads;
/* rdtime when the debug monitor was entered (0 when not in it); the monitor's
 * stop-the-world time is not charged to the interrupted thread. */
extern u64 monitor_entry_time;
extern spinlock_t sched_lock;
extern bool sched_ready;

void sched_init(void);
void cpu_idle_loop(void) __noreturn;
struct thread *sched_create_idle(int cpu);  /* for a secondary CPU (smp.c) */
void hung_thread_check(void);               /* mutex waits that never end */
u64 hung_set_threshold_ms(u64 ms);          /* returns the old one (tests) */

struct thread *kthread_create(const char *name, void (*fn)(void *), void *arg);
/* Like kthread_create, but not runnable until kthread_start(). */
struct thread *kthread_create_suspended(const char *name, void (*fn)(void *), void *arg);
void kthread_start(struct thread *t);
/* Pin a not-yet-started thread to one CPU (per-CPU work, tests). */
void kthread_bind(struct thread *t, int cpu);
int kthread_join(struct thread *t);
void kthread_detach(struct thread *t);
void thread_exit(int code) __noreturn;
struct thread *thread_find(int tid);
const char *thread_name(int tid);   /* also knows recently exited threads */

void schedule(void);            /* voluntary: a T_SLEEPING caller sleeps */
void preempt_schedule(void);    /* involuntary: the caller stays runnable */
void yield(void);
void sleep_ms(u64 ms);
/* Multi-source waits (poll, netd): see sched.c. */
void set_current_sleeping(const char *what);
void set_current_running(void);
void schedule_until(u64 deadline);          /* rdtime deadline, 0 = none */
u64 ms_to_rdtime(u64 ms);
bool wake_thread(struct thread *t);
bool wake_interruptible(struct thread *t);  /* only if in an interruptible sleep */
void sched_tick(void);              /* timer interrupt */

/* May the caller block (sleeping locks, I/O)? False in interrupt handlers
 * and in the debug monitor, which runs with interrupts off. */
static inline bool can_sleep(void)
{
    unsigned long sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return sched_ready && !in_interrupt() && (sstatus & 2);
}

/* Debugging output. */
void thread_list_print(int (*print)(const char *fmt, ...));
int thread_backtrace(int (*print)(const char *fmt, ...), int tid);
void sched_print(int (*print)(const char *fmt, ...));
const char *thread_state_name(enum thread_state s);
u64 thread_cpu_ticks(struct thread *t);
size_t kstack_used(const void *base, size_t size);

/* ---- wait queues ---- */

struct wait_entry {
    struct list_head node;
    struct thread *t;
    bool queued;
};

#define WAITQUEUE_INIT(wq, n) \
    { .lock = SPINLOCK_INIT(n), .waiters = LIST_HEAD_INIT((wq).waiters), .name = (n) }

void waitqueue_init(struct waitqueue *wq, const char *name);
void prepare_to_wait(struct waitqueue *wq, struct wait_entry *we);
void finish_wait(struct waitqueue *wq, struct wait_entry *we);
int wake_up(struct waitqueue *wq);          /* wakes all; returns count */
/* Queue/dequeue a wait entry without changing the thread's state (poll). */
void add_wait_queue(struct waitqueue *wq, struct wait_entry *we);
void remove_wait_queue(struct waitqueue *wq, struct wait_entry *we);
int wake_up_one(struct waitqueue *wq);

/* Sleep until `cond` is true. cond is re-checked after every wakeup. */
#define wait_event(wq, cond)                                                \
    do {                                                                    \
        struct wait_entry __we = { .t = current, .queued = false };        \
        for (;;) {                                                          \
            prepare_to_wait((wq), &__we);                                   \
            if (cond)                                                       \
                break;                                                      \
            schedule();                                                     \
        }                                                                   \
        finish_wait((wq), &__we);                                           \
    } while (0)

/* Has the thread's process a deliverable signal? (signal.c; false for kernel threads) */
bool thread_signal_pending(struct thread *t);

/*
 * Like wait_event, but a pending signal ends the wait: evaluates to 0 when
 * `cond` became true, -EINTR (-4) when interrupted.
 */
#define wait_event_interruptible(wq, cond)                                  \
    ({                                                                      \
        int __r = 0;                                                        \
        struct wait_entry __we = { .t = current, .queued = false };         \
        current->interruptible = true;                                      \
        for (;;) {                                                          \
            prepare_to_wait((wq), &__we);                                   \
            if (cond)                                                       \
                break;                                                      \
            if (thread_signal_pending(current)) {                           \
                __r = -4;                                                   \
                break;                                                      \
            }                                                               \
            schedule();                                                     \
        }                                                                   \
        current->interruptible = false;                                     \
        finish_wait((wq), &__we);                                           \
        __r;                                                                \
    })

/* ---- mutexes (sleeping locks with owner tracking) ---- */

struct mutex {
    spinlock_t lock;
    struct thread *owner;
    uintptr_t owner_pc;
    struct waitqueue wq;
    const char *name;
    u64 contentions;
    u16 ld_class;                   /* lockdep class, assigned on first use */
};

#define MUTEX_INIT(m, n) \
    { .lock = SPINLOCK_INIT(n), .owner = NULL, .owner_pc = 0, \
      .wq = WAITQUEUE_INIT((m).wq, n), .name = (n), .contentions = 0, .ld_class = 0 }

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

/* ---- counting semaphores ---- */

struct semaphore {
    spinlock_t lock;
    long count;
    struct waitqueue wq;
    const char *name;
};

void sem_init(struct semaphore *s, long count, const char *name);
void sem_down(struct semaphore *s);
bool sem_trydown(struct semaphore *s);
void sem_up(struct semaphore *s);

#endif
