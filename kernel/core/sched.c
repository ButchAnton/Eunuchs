/*
 * Kernel threads and a round-robin scheduler, on every CPU.
 *
 * One run queue for all CPUs, under sched_lock: every CPU takes the oldest
 * runnable thread, which balances load by construction and keeps every
 * scheduling decision under one lock (easy to reason about and to inspect
 * with `sched`). When a thread becomes runnable and this CPU is busy, an
 * idle CPU is woken with a reschedule IPI. Each CPU has its own idle thread
 * (idle/N), which runs only when the queue is empty.
 *
 * Locking: sched_lock protects the run queue, sleep queue, thread list and
 * thread states. It is held across context_switch: the thread that switches
 * in releases it (in schedule() after context_switch returns, or in
 * thread_entry for a brand-new thread). So a thread put back on the queue by
 * one CPU cannot be picked up by another before its registers are saved;
 * `on_cpu` stays set until then, and a wakeup that arrives in between just
 * marks it runnable.
 *
 * Preemption: every timer tick sets need_resched; the trap handler yields on
 * return from the interrupt if the interrupted code had interrupts enabled.
 *
 * Debugging: `ps` shows which CPU each thread is on and how often it
 * migrated, `sched` and `cpus` show per-CPU state; sched_switch trace
 * events carry the CPU, giving one Perfetto track per CPU.
 */
#define LOG_SUBSYS SUBSYS_SCHED
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <proc.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>

_Static_assert(offsetof(struct cpu, cur_thread) == 0, "percpu.h: current");
_Static_assert(offsetof(struct cpu, overflow_stack_top) == CPU_OVERFLOW_STACK, "percpu.h");
_Static_assert(offsetof(struct cpu, overflow_sp) == CPU_OVERFLOW_SP, "percpu.h");
_Static_assert(offsetof(struct cpu, scratch) == CPU_SCRATCH, "percpu.h");
_Static_assert(offsetof(struct cpu, kstack_top) == CPU_KSTACK_TOP, "percpu.h");
_Static_assert(offsetof(struct thread, ctx) == 0, "switch.S");
_Static_assert(sizeof(struct trap_frame) + 16 == TRAP_FRAME_SIZE, "trapentry.S");

struct cpu cpus[MAX_CPUS];
LIST_HEAD(all_threads);
spinlock_t sched_lock = SPINLOCK_INIT("sched");
bool sched_ready;
u64 monitor_entry_time;

static LIST_HEAD(runq);
static LIST_HEAD(sleepq);
static int next_tid = 1;
static struct thread boot_thread;
static u64 hung_ticks;          /* hung-thread threshold (boot option hung=<seconds>) */

extern void context_switch(struct context *old, struct context *new);
extern void thread_start(void);         /* switch.S: clears ra, jumps to thread_entry */

/* Called first thing in kmain: per-CPU basics needed by the trap path. */
void cpu_early_init(unsigned long hartid)
{
    struct cpu *c = this_cpu();
    c->id = 0;
    c->hartid = hartid;
    c->overflow_stack_top = (uintptr_t)overflow_stack_top;
}

static void hung_init(void);

const char *thread_state_name(enum thread_state s)
{
    switch (s) {
    case T_RUNNING: return "R";
    case T_RUNNABLE: return "r";
    case T_SLEEPING: return "S";
    case T_DEAD: return "D";
    }
    return "?";
}

static u64 tb(void)
{
    return platform.timebase ? platform.timebase : 10000000;
}

void sched_init(void)
{
    struct cpu *c = this_cpu();
    struct thread *t = &boot_thread;

    t->tid = 0;
    t->bound_cpu = 0;
    strlcpy(t->name, "idle/0", sizeof(t->name));
    t->state = T_RUNNING;
    t->on_cpu = true;
    t->detached = true;
    t->kstack = boot_stack;
    t->kstack_size = KSTACK_SIZE;
    t->created = rdtime();
    waitqueue_init(&t->exit_wq, "thread-exit");
    list_add_tail(&t->all_node, &all_threads);

    /* Poison the unused part of the boot stack for high-water reporting. */
    uintptr_t below = current_fp() - 1024;
    memset(boot_stack, STACK_POISON, below - (uintptr_t)boot_stack);

    c->cur_thread = c->idle = t;
    c->kstack_top = (uintptr_t)boot_stack_top;
    c->last_switch = rdtime();
    sched_ready = true;
    hung_init();
    pr_info("scheduler ready: round-robin, preemptive, boot context is thread 0 (idle/0)\n");
}

/* Idle thread for a secondary CPU: secondary_entry starts on its stack. */
struct thread *sched_create_idle(int cpu)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->kstack = kstack_alloc();
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    t->kstack_size = KSTACK_SIZE;
    ksnprintf(t->name, sizeof(t->name), "idle/%d", cpu);
    t->bound_cpu = cpu;
    t->state = T_RUNNING;
    t->on_cpu = true;
    t->detached = true;
    t->cpu = cpu;
    t->created = rdtime();
    t->create_site = (uintptr_t)__builtin_return_address(0);
    waitqueue_init(&t->exit_wq, "thread-exit");
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->tid = next_tid++;
    list_add_tail(&t->all_node, &all_threads);
    spin_unlock_irqrestore(&sched_lock, flags);
    return t;
}

/* ---- queues (sched_lock held) ---- */

/*
 * New work and this CPU is busy: wake an idle CPU to run it (one that has
 * no reschedule pending yet, so a burst of wakeups spreads out). An idle
 * CPU notices on its own: its idle loop reschedules after any interrupt.
 * A thread bound to a CPU kicks that one.
 */
static void kick_idle_cpu(struct thread *t)
{
    struct cpu *me = this_cpu();
    if (t->bound_cpu >= 0) {
        struct cpu *c = &cpus[t->bound_cpu];
        if (c != me && c->online && !(c->ipi_pending & (1u << IPI_RESCHED)))
            smp_send_ipi(c->id, IPI_RESCHED);
        return;
    }
    if (me->cur_thread == me->idle || ncpus_online < 2)
        return;
    for (int i = 0; i < ncpus_online; i++) {
        struct cpu *c = &cpus[i];
        if (c != me && c->online && c->cur_thread == c->idle && !c->need_resched &&
            !(c->ipi_pending & (1u << IPI_RESCHED))) {
            smp_send_ipi(i, IPI_RESCHED);
            return;
        }
    }
}

static void enqueue(struct thread *t)
{
    t->state = T_RUNNABLE;
    if (!t->on_runq) {
        list_add_tail(&t->run_node, &runq);
        t->on_runq = true;
        kick_idle_cpu(t);
    }
}

/* The oldest runnable thread allowed on this CPU. */
static struct thread *dequeue(int cpu)
{
    struct thread *t;
    list_for_each_entry(t, &runq, run_node) {
        if (t->bound_cpu < 0 || t->bound_cpu == cpu) {
            list_del(&t->run_node);
            t->on_runq = false;
            return t;
        }
    }
    return NULL;
}

static bool wake_locked(struct thread *t)
{
    if (t->state != T_SLEEPING)
        return false;
    if (t->on_sleepq) {
        list_del(&t->sleep_node);
        t->on_sleepq = false;
    }
    trace(sched_wakeup, t->tid, current ? current->tid : 0, 0);
    if (t->on_cpu)
        t->state = T_RUNNABLE;      /* still on its way to sleep; schedule() requeues */
    else
        enqueue(t);
    return true;
}

/*
 * Wake t if it sleeps interruptibly (signal delivery). Checked under
 * sched_lock: interruptible sleepers set the flag and then look for pending
 * signals under the same lock, so a signal can't slip in between.
 */
bool wake_interruptible(struct thread *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    bool woke = t->interruptible && wake_locked(t);
    spin_unlock_irqrestore(&sched_lock, flags);
    return woke;
}

bool wake_thread(struct thread *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    bool woke = wake_locked(t);
    spin_unlock_irqrestore(&sched_lock, flags);
    return woke;
}

/* ---- switching ---- */

/* Names of recently freed threads, so traces can still name them. */
#define NAME_CACHE 64
static struct {
    int tid;
    char name[16];
} name_cache[NAME_CACHE];
static unsigned name_cache_next;

/* For trace decoding: the name may be stale (only ever printed). */
const char *thread_name(int tid)
{
    unsigned long flags;
    bool locked = spin_lock_debug(&sched_lock, &flags);
    const char *name = "?";
    struct thread *t = thread_find(tid);
    if (t) {
        name = t->name;
    } else {
        for (int i = 0; i < NAME_CACHE; i++)
            if (name_cache[i].tid == tid && name_cache[i].name[0])
                name = name_cache[i].name;
    }
    spin_unlock_debug(&sched_lock, locked, flags);
    return name;
}

static void free_thread(struct thread *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    unsigned i = name_cache_next++ % NAME_CACHE;
    name_cache[i].tid = t->tid;
    memcpy(name_cache[i].name, t->name, sizeof(name_cache[i].name));
    spin_unlock_irqrestore(&sched_lock, flags);
    kstack_free(t->kstack);
    kfree(t);
}

/*
 * In the thread that was just switched to, with sched_lock held. Whether a
 * dead predecessor gets freed or announced is decided here, under the lock
 * (kthread_detach may race with us on another CPU).
 */
static void finish_switch(struct cpu *c, struct thread *from)
{
    from->on_cpu = false;
    if (from->state == T_DEAD) {
        c->prev_dead = from;
        c->prev_dead_free = from->detached;
        if (from->detached)
            list_del(&from->all_node);
    }
}

/*
 * After sched_lock is released: reap or announce a dead predecessor. A
 * joiner may run on another CPU as soon as it sees the thread dead and off
 * its CPU, so exit_done tells it when we no longer touch the thread.
 */
static void post_switch(struct cpu *c)
{
    struct thread *dead = c->prev_dead;
    if (!dead)
        return;
    c->prev_dead = NULL;
    if (c->prev_dead_free) {
        free_thread(dead);
    } else {
        wake_up(&dead->exit_wq);
        __atomic_store_n(&dead->exit_done, true, __ATOMIC_RELEASE);
    }
}

static void wait_exit_done(struct thread *t)
{
    while (!__atomic_load_n(&t->exit_done, __ATOMIC_ACQUIRE))
        asm volatile("nop" ::: "memory");
}

/*
 * Pick the next thread and switch to it.
 *
 * Voluntary (preempt = false): a thread whose state is T_SLEEPING is taken
 * off the CPU until woken; that is how sleeping works.
 *
 * Preemption (preempt = true, from the timer interrupt): the thread is always
 * requeued and keeps its state. It may be in the middle of
 * prepare_to_wait(); if we treated T_SLEEPING as "asleep" here, a wakeup that
 * already happened would be lost and the thread would sleep forever even
 * though its condition is true (seen as a hang in wait4). It goes to sleep
 * only when it calls schedule() itself after rechecking its condition.
 */
static void __schedule(bool preempt)
{
    if (!sched_ready)
        return;

    /*
     * Interrupts off before looking at this CPU: preempted in between, we
     * could migrate and then switch out another CPU's current thread.
     */
    unsigned long flags = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    struct cpu *c = this_cpu();
    struct thread *prev = c->cur_thread;
    if (c->nheld > 0) {
        char s[96];
        ksym_format(s, sizeof(s), c->held[c->nheld - 1]->owner_pc);
        panic("schedule() while holding spinlock '%s' (taken at %s)",
              c->held[c->nheld - 1]->name, s);
    }
    if (c->irq_depth > 0 && prev->state == T_SLEEPING)
        panic("thread %d (%s) tried to sleep in interrupt context", prev->tid, prev->name);

    spin_lock_irqsave(&sched_lock);         /* interrupts are already off */
    c->need_resched = false;

    if (prev == c->idle) {
        prev->state = T_RUNNABLE;
    } else if (preempt && prev->state == T_SLEEPING) {
        if (!prev->on_runq) {               /* requeue without changing its state */
            list_add_tail(&prev->run_node, &runq);
            prev->on_runq = true;
        }
    } else if (prev->state == T_RUNNING || prev->state == T_RUNNABLE) {
        enqueue(prev);
    }

    struct thread *next = dequeue(c->id);
    if (!next)
        next = c->idle;

    if (next == prev) {
        if (prev->state == T_RUNNABLE)
            prev->state = T_RUNNING;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    u64 now = rdtime();
    prev->runtime += now - c->last_switch;
    c->last_switch = now;
    trace(sched_switch, prev->tid, next->tid, prev->state);   /* recorded as prev */
    if (next->state == T_RUNNABLE)
        next->state = T_RUNNING;            /* a preempted sleeper stays T_SLEEPING */
    next->on_cpu = true;
    if (next->cpu != c->id && next->switches) {
        next->migrations++;
        c->migrations++;
    }
    next->cpu = c->id;
    next->switches++;
    c->switches++;
    c->cur_thread = next;
    c->switch_from = prev;
    c->kstack_top = (uintptr_t)next->kstack + next->kstack_size;
    vm_activate(next->proc ? next->proc->mm : NULL);
    csr_clear(sstatus, SSTATUS_SUM);        /* never leak user access across threads */
    if (prev->fp_used)                      /* the FP registers hold prev's user state */
        fp_save_regs(&prev->fp);

    context_switch(&prev->ctx, &next->ctx);

    /*
     * Back in `prev`, switched to by c->switch_from, which holds sched_lock
     * for us. Possibly on another CPU than the one we left: re-read it.
     */
    c = this_cpu();
    if (unlikely(c->cur_thread != prev))
        panic("scheduler: thread %d (%s) resumed on cpu%d, whose current thread is %d (%s)",
              prev->tid, prev->name, c->id, c->cur_thread->tid, c->cur_thread->name);
    finish_switch(c, c->switch_from);
    if (prev->fp_used)
        fp_load_regs(&prev->fp);
    /*
     * Interrupts stay off until post_switch is done: preempted in between,
     * we could resume on another CPU and finish *its* predecessor.
     */
    spin_unlock_irqrestore(&sched_lock, 0);
    post_switch(c);
    if (flags)
        intr_on();
}

void schedule(void)
{
    __schedule(false);
}

void preempt_schedule(void)
{
    __schedule(true);
}

void yield(void)
{
    schedule();
}

/* New threads start here (via thread_start in switch.S) holding sched_lock. */
__noreturn void thread_entry(void)
{
    struct cpu *c = this_cpu();
    finish_switch(c, c->switch_from);
    if (current->fp_used)       /* a forked child starts with its parent's FP registers */
        fp_load_regs(&current->fp);
    spin_unlock_irqrestore(&sched_lock, 0);
    post_switch(c);
    intr_on();                  /* from here on we may migrate: no more `c` */

    struct thread *t = current;
    t->fn(t->arg);
    thread_exit(0);
}

static struct thread *create_thread(const char *name, void (*fn)(void *), void *arg,
                                   bool run, uintptr_t site)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->kstack = kstack_alloc();
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    t->kstack_size = KSTACK_SIZE;
    t->bound_cpu = -1;
    strlcpy(t->name, name, sizeof(t->name));
    t->fn = fn;
    t->arg = arg;
    t->created = rdtime();
    t->create_site = site;
    waitqueue_init(&t->exit_wq, "thread-exit");
    t->ctx.ra = (u64)thread_start;
    /* The top TRAP_FRAME_SIZE bytes are reserved for the user trap frame. */
    t->ctx.sp = (u64)t->kstack + KSTACK_SIZE - TRAP_FRAME_SIZE;
    t->ctx.s[0] = 0;                    /* frame-pointer chain ends here */

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->tid = next_tid++;
    list_add_tail(&t->all_node, &all_threads);
    if (run)
        enqueue(t);
    else
        t->state = T_SLEEPING;
    if (!run)
        t->wait_what = "start";
    spin_unlock_irqrestore(&sched_lock, flags);

    u64 n[2] = { 0, 0 };
    memcpy(n, t->name, sizeof(n));
    trace(thread_create, t->tid, n[0], n[1]);
    if (LOG_DEBUG <= log_threshold[LOG_SUBSYS]) {
        char s[96];
        ksym_format(s, sizeof(s), t->create_site);
        pr_debug("thread %d (%s) created by %s\n", t->tid, t->name, s);
    }
    return t;
}

struct thread *kthread_create(const char *name, void (*fn)(void *), void *arg)
{
    return create_thread(name, fn, arg, true, (uintptr_t)__builtin_return_address(0));
}

struct thread *kthread_create_suspended(const char *name, void (*fn)(void *), void *arg)
{
    return create_thread(name, fn, arg, false, (uintptr_t)__builtin_return_address(0));
}

void kthread_bind(struct thread *t, int cpu)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->bound_cpu = cpu < ncpus_online ? cpu : -1;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void kthread_start(struct thread *t)
{
    t->wait_what = NULL;
    wake_thread(t);
}

struct trap_frame *thread_user_tf(struct thread *t)
{
    return (struct trap_frame *)((u8 *)t->kstack + t->kstack_size - TRAP_FRAME_SIZE);
}

void thread_exit(int code)
{
    struct thread *t = current;
    if (t == this_cpu()->idle)
        panic("the idle thread cannot exit");

    intr_off();                 /* no preemption between DEAD and the final switch */
    trace(thread_exit, t->tid, code, 0);
    t->exit_code = code;
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->state = T_DEAD;
    spin_unlock_irqrestore(&sched_lock, flags);
    schedule();
    panic("dead thread %d (%s) was scheduled again", t->tid, t->name);
}

int kthread_join(struct thread *t)
{
    if (t == current || t->detached)
        panic("kthread_join(%d): %s", t->tid, t == current ? "joining self" : "thread is detached");
    current->wait_what = "join";
    current->wait_obj = t;
    wait_event(&t->exit_wq, t->state == T_DEAD && !t->on_cpu);
    wait_exit_done(t);
    int code = t->exit_code;

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    list_del(&t->all_node);
    spin_unlock_irqrestore(&sched_lock, flags);
    free_thread(t);
    return code;
}

void kthread_detach(struct thread *t)
{
    bool reap = false;
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->detached = true;
    if (t->state == T_DEAD && !t->on_cpu) {
        /* Already past finish_switch as a joinable thread: we free it. */
        list_del(&t->all_node);
        reap = true;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (reap) {
        wait_exit_done(t);
        free_thread(t);
    }
}

/* Caller holds sched_lock (or the world is stopped). */
struct thread *thread_find(int tid)
{
    struct thread *t;
    list_for_each_entry(t, &all_threads, all_node)
        if (t->tid == tid)
            return t;
    return NULL;
}

/* ---- timed sleep and the tick ---- */

/* sched_lock held: put t on the sleep queue (sorted by deadline). */
static void sleepq_insert(struct thread *t, u64 deadline)
{
    t->wake_time = deadline;
    struct list_head *pos;
    list_for_each(pos, &sleepq) {
        if (list_entry(pos, struct thread, sleep_node)->wake_time > deadline)
            break;
    }
    list_add_tail(&t->sleep_node, pos);     /* insert before pos: sorted */
    t->on_sleepq = true;
}

/*
 * For waits on several things at once (poll): mark the caller sleeping
 * (interruptibly) *before* it checks its conditions, so a wakeup that
 * arrives in between makes schedule_until() return at once.
 */
void set_current_sleeping(const char *what)
{
    struct thread *t = current;
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->state = T_SLEEPING;
    t->interruptible = true;
    t->wait_what = what;
    t->wait_obj = NULL;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void set_current_running(void)
{
    struct thread *t = current;
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    if (t->state == T_SLEEPING || t->state == T_RUNNABLE)
        t->state = T_RUNNING;
    t->interruptible = false;
    t->wait_what = NULL;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/*
 * Sleep (the caller is T_SLEEPING) until woken or until rdtime reaches
 * `deadline` (0: no deadline).
 */
void schedule_until(u64 deadline)
{
    struct thread *t = current;
    if (deadline) {
        unsigned long flags = spin_lock_irqsave(&sched_lock);
        if (t->state == T_SLEEPING && !t->on_sleepq)
            sleepq_insert(t, deadline);
        spin_unlock_irqrestore(&sched_lock, flags);
    }
    schedule();
    if (deadline) {
        unsigned long flags = spin_lock_irqsave(&sched_lock);
        if (t->on_sleepq) {
            list_del(&t->sleep_node);
            t->on_sleepq = false;
        }
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

u64 ms_to_rdtime(u64 ms)
{
    return ms * tb() / 1000;
}

void sleep_ms(u64 ms)
{
    struct thread *t = current;
    if (!ms) {
        yield();
        return;
    }

    u64 deadline = rdtime() + ms * tb() / 1000;
    /* Loop: an early wakeup (e.g. kill) returns, but nothing else does. */
    while (rdtime() < deadline) {
        unsigned long flags = spin_lock_irqsave(&sched_lock);
        /*
         * A signal sent before we became interruptible found nothing to wake
         * (wake_interruptible checks under this lock): look for it now.
         */
        t->interruptible = true;
        if (thread_signal_pending(t)) {
            t->interruptible = false;
            spin_unlock_irqrestore(&sched_lock, flags);
            break;
        }
        sleepq_insert(t, deadline);
        t->state = T_SLEEPING;
        t->wait_what = "sleep";
        t->wait_obj = NULL;
        t->interruptible = true;
        spin_unlock_irqrestore(&sched_lock, flags);

        schedule();
        t->interruptible = false;
        if (thread_signal_pending(t))
            break;                      /* the caller reports EINTR */
    }
    t->wait_what = NULL;
}

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    u64 now = rdtime();
    while (!list_empty(&sleepq)) {
        struct thread *t = list_first_entry(&sleepq, struct thread, sleep_node);
        if (t->wake_time > now)
            break;
        wake_locked(t);
    }
    if (c->cur_thread != c->idle || !list_empty(&runq))
        c->need_resched = true;
    spin_unlock_irqrestore(&sched_lock, flags);

    if (c->id == 0 && hung_ticks && c->ticks % HZ == 0)
        hung_thread_check();
}

/* ---- hung-thread detector ---- */

static void hung_init(void)
{
    u64 secs = 30;
    const char *h = cmdline_get("hung");
    if (h && *h)
        secs = kstrtoul(h, NULL, 0);
    hung_ticks = secs * tb();
}

u64 hung_set_threshold_ms(u64 ms)
{
    u64 old = hung_ticks * 1000 / tb();
    hung_ticks = ms * tb() / 1000;
    return old;
}

/*
 * Once a second (cpu0's tick): report threads that have waited for a mutex
 * longer than hung=<seconds> (default 30, 0 = off). That is almost always a
 * lost unlock or a deadlock the cycle check can't see (e.g. through a wait
 * queue). Each wait is reported once, with both backtraces.
 */
void hung_thread_check(void)
{
    struct thread *t, *hung = NULL;
    unsigned long flags;
    if (!spin_trylock_irqsave(&sched_lock, &flags))
        return;                     /* busy: next second */
    u64 now = rdtime();
    list_for_each_entry(t, &all_threads, all_node) {
        /* blocked_since is set without sched_lock: it may be newer than `now`. */
        u64 since = t->blocked_since;
        if (t->blocked_on && t->state == T_SLEEPING && !t->hung_reported && now > since &&
            now - since > hung_ticks) {
            t->hung_reported = true;
            hung = t;
            break;
        }
    }
    if (!hung) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }
    /* Report with sched_lock held: neither thread can run, exit or move meanwhile. */
    struct mutex *m = hung->blocked_on;
    struct thread *o = m->owner;
    char site[96];
    ksym_format(site, sizeof(site), m->owner_pc);
    pr_warn("HUNG THREAD: %d (%s) has waited %lu ms for mutex '%s'\n", hung->tid, hung->name,
            (unsigned long)((now - hung->blocked_since) * 1000 / tb()), m->name);
    pr_warn("  owner: %s%d (%s), locked at %s\n", o ? "thread " : "none? ", o ? o->tid : -1,
            o ? o->name : "-", site);
    pr_warn("  waiter backtrace:\n");
    backtrace_print(log_warn_print, hung->ctx.ra, hung->ctx.s[0]);
    if (o && o != current && !o->on_cpu) {
        pr_warn("  owner backtrace (state %s%s%s):\n", thread_state_name(o->state),
                o->wait_what ? ", waiting on " : "", o->wait_what ? o->wait_what : "");
        backtrace_print(log_warn_print, o->ctx.ra, o->ctx.s[0]);
    } else if (o) {
        pr_warn("  owner is running on cpu%d\n", o->cpu);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void cpu_idle_loop(void)
{
    struct cpu *c = this_cpu();
    for (;;) {
        if (unlikely(c->cur_thread != c->idle || current != c->idle))
            panic("idle loop of cpu%d is running as thread %d (%s)", c->id, current->tid,
                  current->name);
        /* Check-then-wfi with interrupts off: no lost wakeups. */
        intr_off();
        if (list_empty(&runq) && !c->need_resched)
            asm volatile("wfi");
        intr_on();              /* pending interrupts are taken here */
        schedule();
    }
}

/* ---- debugging ---- */

size_t kstack_used(const void *base, size_t size)
{
    const u8 *p = base;
    size_t i = 0;
    while (i < size && p[i] == STACK_POISON)
        i++;
    return size - i;
}

/* "Now" for accounting: frozen while the monitor has the machine stopped. */
static u64 acct_now(void)
{
    return monitor_entry_time ? monitor_entry_time : rdtime();
}

static bool is_sched_internal(const char *sym)
{
    static const char *const internal[] = {
        "schedule", "yield", "context_switch", "thread_entry",
        "handle_interrupt", "trap_handler", NULL,
    };
    for (int i = 0; internal[i]; i++)
        if (sym && !strcmp(sym, internal[i]))
            return true;
    return false;
}

/* "where": first frames of a sleeping thread outside the scheduler itself. */
static void thread_where(struct thread *t, char *buf, size_t size)
{
    uintptr_t pcs[8];
    int n = backtrace_collect(t->ctx.s[0], pcs, 8);
    size_t len = 0;
    int shown = 0;
    buf[0] = '\0';
    bool preempted = false;
    for (int i = 0; i < n && shown < 2; i++) {
        uintptr_t off;
        const char *sym = ksym_lookup(pcs[i] - 1, &off);
        if (sym && !strcmp(sym, "kernel_trap_entry")) {
            /* Next frame is the interrupted pc itself (not a return address). */
            preempted = true;
            continue;
        }
        if (is_sched_internal(sym))
            continue;
        if (preempted && i > 0 && pcs[i - 1] && shown == 0) {
            sym = ksym_lookup(pcs[i], &off);
            off -= 1;
        }
        len += ksnprintf(buf + len, size - len, "%s%s%s+%#lx", shown ? " <- " : "",
                         preempted && shown == 0 ? "preempted in " : "",
                         sym ? sym : "?", (unsigned long)(off + 1));
        shown++;
        if (len >= size)
            break;
    }
}

static void wait_desc(struct thread *t, char *buf, size_t size)
{
    const char *w = t->wait_what;
    if (t->state != T_SLEEPING || !w) {
        buf[0] = '\0';
        return;
    }
    if (!strcmp(w, "mutex") && t->blocked_on) {
        struct thread *o = t->blocked_on->owner;
        ksnprintf(buf, size, "mutex '%s' held by %d(%s)", t->blocked_on->name,
                  o ? o->tid : -1, o ? o->name : "-");
    } else if (!strcmp(w, "sem") && t->wait_obj) {
        ksnprintf(buf, size, "sem '%s'", ((const struct semaphore *)t->wait_obj)->name);
    } else if (!strcmp(w, "sleep")) {
        u64 now = rdtime();
        u64 left = t->wake_time > now ? (t->wake_time - now) * 1000 / tb() : 0;
        ksnprintf(buf, size, "sleep (%lu ms left)", (unsigned long)left);
    } else if (!strcmp(w, "join") && t->wait_obj) {
        ksnprintf(buf, size, "join tid %d", ((const struct thread *)t->wait_obj)->tid);
    } else {
        ksnprintf(buf, size, "wq '%s'", w);
    }
}

/* Is t on a CPU right now (its saved context is stale)? */
static bool running_now(struct thread *t)
{
    return t->on_cpu && t->cpu >= 0 && t->cpu < ncpus_online && cpus[t->cpu].cur_thread == t;
}

/* CPU time used by t so far, including the current slice (rdtime ticks). */
u64 thread_cpu_ticks(struct thread *t)
{
    return t->runtime + (running_now(t) ? acct_now() - cpus[t->cpu].last_switch : 0);
}

void thread_list_print(print_fn print)
{
    unsigned long flags;
    bool locked = spin_lock_debug(&sched_lock, &flags);
    u64 now = acct_now();
    struct thread *t;
    struct thread *me = current;

    if (!locked)
        print("(sched_lock is held; showing an unlocked snapshot)\n");
    print("  TID NAME             ST CPU   CPU-ms  SWITCH MIGR  STACK     WAITING ON / WHERE\n");
    list_for_each_entry(t, &all_threads, all_node) {
        bool running = running_now(t);
        u64 rt = t->runtime + (running ? now - cpus[t->cpu].last_switch : 0);
        char wait[64], where[96];
        wait_desc(t, wait, sizeof(wait));
        if (t == me)
            strlcpy(where, "(running: this is the current thread)", sizeof(where));
        else if (running && cpus[t->cpu].stopped && cpus[t->cpu].stop_pc)
            ksnprintf(where, sizeof(where), "(stopped on cpu%d; see 'cpu %d')", t->cpu, t->cpu);
        else if (running)
            ksnprintf(where, sizeof(where), "(running on cpu%d)", t->cpu);
        else if (t->state == T_DEAD)
            strlcpy(where, "(exited; awaiting join)", sizeof(where));
        else
            thread_where(t, where, sizeof(where));
        size_t used = kstack_used(t->kstack, t->kstack_size);
        print("  %3d %-16s %-2s %c%-2d %8lu %7lu %4lu  %2zu.%zuK/%zuK  %s%s%s\n", t->tid, t->name,
              thread_state_name(t->state), running ? '*' : ' ', t->cpu,
              (unsigned long)(rt * 1000 / tb()), (unsigned long)t->switches,
              (unsigned long)t->migrations, used / 1024, (used % 1024) * 10 / 1024,
              t->kstack_size / 1024, wait, wait[0] && where[0] ? "  @ " : "", where);
    }
    spin_unlock_debug(&sched_lock, locked, flags);
}

/* With sched_lock held the thread can neither run nor be freed meanwhile. */
int thread_backtrace(print_fn print, int tid)
{
    unsigned long flags;
    bool locked = spin_lock_debug(&sched_lock, &flags);
    struct thread *t = thread_find(tid);
    if (!t) {
        spin_unlock_debug(&sched_lock, locked, flags);
        print("no thread %d\n", tid);
        return -1;
    }
    print("thread %d (%s), state %s, cpu%d\n", t->tid, t->name, thread_state_name(t->state),
          t->cpu);
    if (t == current) {
        backtrace_here(print);
    } else if (t->state == T_DEAD) {
        print("  (exited)\n");
    } else if (running_now(t) && cpus[t->cpu].stopped) {
        print("  (running on cpu%d, stopped at:)\n", t->cpu);
        backtrace_print(print, cpus[t->cpu].stop_pc, cpus[t->cpu].stop_fp);
    } else if (running_now(t)) {
        print("  (running on cpu%d right now: no saved context)\n", t->cpu);
    } else {
        /* Saved context: pc = where context_switch will return. */
        backtrace_print(print, t->ctx.ra, t->ctx.s[0]);
    }
    spin_unlock_debug(&sched_lock, locked, flags);
    return 0;
}

void sched_print(print_fn print)
{
    struct thread *t;
    u64 now = acct_now();
    u64 up = now ? now : 1;
    unsigned long flags;
    bool locked = spin_lock_debug(&sched_lock, &flags);

    for (int i = 0; i < ncpus_online; i++) {
        struct cpu *c = &cpus[i];
        struct thread *cur = c->cur_thread;
        u64 idle = c->idle->runtime + (cur == c->idle ? now - c->last_switch : 0);
        print("cpu%d: current %d(%s), %lu switches, %lu migrations in, need_resched=%d, "
              "idle %lu%%\n", c->id, cur->tid, cur->name, (unsigned long)c->switches,
              (unsigned long)c->migrations, c->need_resched, (unsigned long)(idle * 100 / up));
    }
    print("run queue (shared):");
    list_for_each_entry(t, &runq, run_node)
        print(" %d(%s)", t->tid, t->name);
    print("\nsleep queue:");
    list_for_each_entry(t, &sleepq, sleep_node)
        print(" %d(%s)+%lums", t->tid, t->name,
              (unsigned long)(t->wake_time > now ? (t->wake_time - now) * 1000 / tb() : 0));
    print("\n");
    spin_unlock_debug(&sched_lock, locked, flags);
}
