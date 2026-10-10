/*
 * Sleeping synchronization: wait queues, mutexes, semaphores.
 *
 * Lock order: a wait queue's lock, then sched_lock (wake_thread).
 *
 * Mutex debugging: each mutex records its owner thread and the PC that
 * locked it. Recursive locking, unlocking a mutex you don't own, locking in
 * interrupt context, and deadlock cycles (A waits for B's mutex while B
 * waits for A's) are detected when they happen and reported with the full
 * chain of threads, mutexes and lock sites. Lock-order inversions that have
 * not deadlocked (yet) are lockdep's job (lockdep.c); every thread keeps a
 * stack of the mutexes it holds for that. A mutex wait that lasts too long
 * is reported by the hung-thread detector (sched.c).
 */
#define LOG_SUBSYS SUBSYS_SCHED
#include <debug.h>
#include <fmt.h>
#include <lockdep.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <sched.h>
#include <trace.h>

/* ---- wait queues ---- */

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    spin_init(&wq->lock, name);
    list_init(&wq->waiters);
    wq->name = name;
}

void prepare_to_wait(struct waitqueue *wq, struct wait_entry *we)
{
    struct thread *t = current;
    unsigned long flags = spin_lock_irqsave(&wq->lock);
    if (!we->queued) {
        list_add_tail(&we->node, &wq->waiters);
        we->queued = true;
    }
    unsigned long f2 = spin_lock_irqsave(&sched_lock);
    t->state = T_SLEEPING;
    spin_unlock_irqrestore(&sched_lock, f2);
    if (!t->wait_what) {
        t->wait_what = wq->name;
        t->wait_obj = wq;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

void finish_wait(struct waitqueue *wq, struct wait_entry *we)
{
    struct thread *t = current;
    unsigned long flags = spin_lock_irqsave(&wq->lock);
    if (we->queued) {
        list_del(&we->node);
        we->queued = false;
    }
    unsigned long f2 = spin_lock_irqsave(&sched_lock);
    t->state = T_RUNNING;
    spin_unlock_irqrestore(&sched_lock, f2);
    t->wait_what = NULL;
    t->wait_obj = NULL;
    spin_unlock_irqrestore(&wq->lock, flags);
}

void add_wait_queue(struct waitqueue *wq, struct wait_entry *we)
{
    unsigned long flags = spin_lock_irqsave(&wq->lock);
    if (!we->queued) {
        list_add_tail(&we->node, &wq->waiters);
        we->queued = true;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

void remove_wait_queue(struct waitqueue *wq, struct wait_entry *we)
{
    unsigned long flags = spin_lock_irqsave(&wq->lock);
    if (we->queued) {
        list_del(&we->node);
        we->queued = false;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

static int do_wake(struct waitqueue *wq, bool one)
{
    int woken = 0;
    struct wait_entry *we, *tmp;
    unsigned long flags = spin_lock_irqsave(&wq->lock);
    list_for_each_entry_safe(we, tmp, &wq->waiters, node) {
        list_del(&we->node);
        we->queued = false;
        if (wake_thread(we->t)) {
            woken++;
            if (one)
                break;
        }
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    return woken;
}

int wake_up(struct waitqueue *wq) { return do_wake(wq, false); }
int wake_up_one(struct waitqueue *wq) { return do_wake(wq, true); }

/* ---- mutexes ---- */

void mutex_init(struct mutex *m, const char *name)
{
    spin_init(&m->lock, name);
    m->owner = NULL;
    m->owner_pc = 0;
    waitqueue_init(&m->wq, name);
    m->name = name;
    m->contentions = 0;
}

static void push_held(struct thread *t, struct mutex *m)
{
    if (t->nheld_mutex < MAX_HELD_MUTEX)
        t->held_mutex[t->nheld_mutex] = m;
    t->nheld_mutex++;
}

static void pop_held(struct thread *t, struct mutex *m)
{
    int n = MIN(t->nheld_mutex, MAX_HELD_MUTEX);
    for (int i = n - 1; i >= 0; i--) {
        if (t->held_mutex[i] == m) {
            for (int j = i; j < n - 1; j++)
                t->held_mutex[j] = t->held_mutex[j + 1];
            break;
        }
    }
    t->nheld_mutex--;
}

static bool trylock_at(struct mutex *m, uintptr_t pc)
{
    unsigned long flags = spin_lock_irqsave(&m->lock);
    bool ok = m->owner == NULL;
    if (ok) {
        m->owner = current;
        m->owner_pc = pc;
        push_held(current, m);
    }
    spin_unlock_irqrestore(&m->lock, flags);
    return ok;
}

bool mutex_trylock(struct mutex *m)
{
    return trylock_at(m, (uintptr_t)__builtin_return_address(0));
}

/* Follow owner -> blocked_on links; a path back to `self` is a deadlock. */
static void check_deadlock(struct mutex *m, struct thread *self)
{
    struct mutex *cur = m;
    for (int depth = 0; depth < 16 && cur; depth++) {
        struct thread *owner = cur->owner;
        if (!owner)
            return;
        if (owner == self) {
            panic_print("MUTEX DEADLOCK detected:\n");
            struct thread *w = self;
            struct mutex *mx = m;
            for (int i = 0; i < 16 && mx && mx->owner; i++) {
                struct thread *o = mx->owner;
                char site[96];
                ksym_format(site, sizeof(site), mx->owner_pc);
                panic_print("  thread %d (%s) waits for mutex '%s', held by thread %d (%s), "
                            "locked at %s\n", w->tid, w->name, mx->name, o->tid, o->name, site);
                if (o == self)
                    break;
                w = o;
                mx = o->blocked_on;
            }
            panic("mutex deadlock (cycle above)");
        }
        cur = owner->blocked_on;
    }
}

void mutex_lock(struct mutex *m)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    struct thread *t = current;

    if (in_interrupt())
        panic("mutex_lock('%s') in interrupt context", m->name);
    if (m->owner == t) {
        char a[96], b[96];
        ksym_format(a, sizeof(a), m->owner_pc);
        ksym_format(b, sizeof(b), pc);
        panic("recursive mutex_lock('%s') by thread %d (%s): locked at %s, again at %s",
              m->name, t->tid, t->name, a, b);
    }
    lockdep_might_sleep("mutex_lock", pc);
    lockdep_acquire(&m->ld_class, m->name, LD_MUTEX, pc);
    if (trylock_at(m, pc))
        return;

    __atomic_add_fetch(&m->contentions, 1, __ATOMIC_RELAXED);
    struct thread *owner = m->owner;
    trace(mutex_contended, m->name, owner ? owner->tid : 0, pc);
    t->blocked_since = rdtime();
    t->hung_reported = false;
    t->blocked_on = m;
    t->wait_what = "mutex";
    t->wait_obj = m;
    check_deadlock(m, t);
    wait_event(&m->wq, trylock_at(m, pc));
    t->blocked_on = NULL;
    if (t->hung_reported)
        klog(LOG_WARN, SUBSYS_SCHED, "hung thread %d (%s) finally got mutex '%s' after %lu ms\n",
             t->tid, t->name, m->name,
             (unsigned long)((rdtime() - t->blocked_since) / (platform.timebase / 1000)));
}

void mutex_unlock(struct mutex *m)
{
    struct thread *t = current;
    if (m->owner != t) {
        char s[96];
        ksym_format(s, sizeof(s), (uintptr_t)__builtin_return_address(0));
        panic("mutex_unlock('%s') at %s by thread %d (%s), but the owner is %s%d",
              m->name, s, t->tid, t->name, m->owner ? "thread " : "nobody/",
              m->owner ? m->owner->tid : -1);
    }
    unsigned long flags = spin_lock_irqsave(&m->lock);
    m->owner = NULL;
    m->owner_pc = 0;
    pop_held(t, m);
    spin_unlock_irqrestore(&m->lock, flags);
    wake_up_one(&m->wq);
}

/* ---- semaphores ---- */

void sem_init(struct semaphore *s, long count, const char *name)
{
    spin_init(&s->lock, name);
    s->count = count;
    waitqueue_init(&s->wq, name);
    s->name = name;
}

bool sem_trydown(struct semaphore *s)
{
    unsigned long flags = spin_lock_irqsave(&s->lock);
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    spin_unlock_irqrestore(&s->lock, flags);
    return ok;
}

void sem_down(struct semaphore *s)
{
    if (sem_trydown(s))
        return;
    current->wait_what = "sem";
    current->wait_obj = s;
    wait_event(&s->wq, sem_trydown(s));
}

void sem_up(struct semaphore *s)
{
    unsigned long flags = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, flags);
    wake_up_one(&s->wq);
}
