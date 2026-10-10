#include <debug.h>
#include <irq.h>
#include <kstring.h>
#include <ktest.h>
#include <mm.h>
#include <platform.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <trace.h>

static u64 ms_now(void)
{
    return rdtime() * 1000 / platform.timebase;
}

static volatile int flag;

static void set_flag_and_exit(void *arg)
{
    flag = (int)(long)arg;
    thread_exit(42);
}

KTEST(thread_runs_and_joins)
{
    flag = 0;
    struct thread *t = kthread_create("t-exit", set_flag_and_exit, (void *)7);
    KT_ASSERT(t != NULL);
    KT_EXPECT_EQ(kthread_join(t), 42);
    KT_EXPECT_EQ(flag, 7);
}

static volatile int counter;
static void bump(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}

KTEST(many_threads_no_leaks)
{
    struct pmm_stats before, after;
    struct thread *t[24];

    u64 used, cached_before, cached_after, reuses, unmaps;

    counter = 0;
    pmm_get_stats(&before);
    kstack_stats(&used, &cached_before, &reuses, &unmaps);
    for (int i = 0; i < 24; i++) {
        t[i] = kthread_create("t-bump", bump, NULL);
        KT_ASSERT(t[i] != NULL);
    }
    for (int i = 0; i < 24; i++)
        kthread_join(t[i]);
    pmm_get_stats(&after);
    kstack_stats(&used, &cached_after, &reuses, &unmaps);
    KT_EXPECT_EQ(counter, 24);
    /*
     * Stacks and thread structs all returned (slab may keep one cached slab;
     * freed stacks may sit in the kstack cache, still mapped).
     */
    u64 cached_pages = (cached_after - cached_before) * (KSTACK_SIZE / PAGE_SIZE);
    KT_EXPECT(after.free_pages + cached_pages + 2 >= before.free_pages);
}

/* Strict alternation through two semaphores. */
static struct semaphore sa, sb;
static char order[64];
static volatile int order_len;

static void alt(void *arg)
{
    char me = (char)(long)arg;
    struct semaphore *mine = me == 'A' ? &sa : &sb, *other = me == 'A' ? &sb : &sa;
    for (int i = 0; i < 10; i++) {
        sem_down(mine);
        order[order_len++] = me;
        sem_up(other);
    }
}

KTEST(semaphore_ping_pong)
{
    sem_init(&sa, 1, "test-A");
    sem_init(&sb, 0, "test-B");
    order_len = 0;
    struct thread *a = kthread_create("ping", alt, (void *)'A');
    struct thread *b = kthread_create("pong", alt, (void *)'B');
    KT_ASSERT(a && b);
    kthread_join(a);
    kthread_join(b);
    KT_EXPECT_EQ(order_len, 20);
    for (int i = 0; i < order_len; i++)
        KT_EXPECT_EQ(order[i], i % 2 ? 'B' : 'A');
}

static struct mutex test_mutex;
static volatile long shared;

static void mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        mutex_lock(&test_mutex);
        long v = shared;
        yield();                    /* invite others into the critical section */
        shared = v + 1;
        mutex_unlock(&test_mutex);
    }
}

KTEST(mutex_mutual_exclusion)
{
    mutex_init(&test_mutex, "test-mutex");
    shared = 0;
    struct thread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = kthread_create("mtx", mutex_worker, NULL);
    for (int i = 0; i < 4; i++)
        kthread_join(t[i]);
    KT_EXPECT_EQ(shared, 800);
    KT_EXPECT(test_mutex.contentions > 0);
    KT_EXPECT(test_mutex.owner == NULL);
}

static volatile bool stop_spinning;
static void spin_until_stopped(void *arg)
{
    (void)arg;
    while (!stop_spinning)
        ;                           /* never yields */
}

KTEST(timer_preempts_busy_thread)
{
    stop_spinning = false;
    u64 j0 = jiffies;
    struct thread *t = kthread_create("hog", spin_until_stopped, NULL);
    KT_ASSERT(t != NULL);
    yield();                        /* let the hog start */
    sleep_ms(30);                   /* only returns if the hog gets preempted */
    stop_spinning = true;
    kthread_join(t);
    KT_EXPECT(jiffies > j0);
    KT_EXPECT(t != NULL);
}

KTEST(sleep_duration)
{
    u64 t0 = ms_now();
    sleep_ms(40);
    u64 dt = ms_now() - t0;
    KT_EXPECT(dt >= 40);
    KT_EXPECT(dt < 200);
}

static struct waitqueue test_wq = WAITQUEUE_INIT(test_wq, "test-wq");
static volatile bool go;
static volatile int woke;

static void waiter(void *arg)
{
    (void)arg;
    wait_event(&test_wq, go);
    __atomic_add_fetch(&woke, 1, __ATOMIC_RELAXED);
}

KTEST(waitqueue_wakes_all)
{
    go = false;
    woke = 0;
    struct thread *t[3];
    for (int i = 0; i < 3; i++)
        t[i] = kthread_create("waiter", waiter, NULL);
    sleep_ms(20);                   /* all three are asleep now */
    for (int i = 0; i < 3; i++)
        KT_EXPECT_EQ(t[i]->state, T_SLEEPING);
    go = true;
    KT_EXPECT_EQ(wake_up(&test_wq), 3);
    for (int i = 0; i < 3; i++)
        kthread_join(t[i]);
    KT_EXPECT_EQ(woke, 3);
}

static void deep_stack(void *arg)
{
    volatile char buf[6000];
    buf[0] = 1;
    buf[sizeof(buf) - 1] = (char)(long)arg;
}

KTEST(stack_high_water_and_guard)
{
    struct thread *t = kthread_create("deep", deep_stack, NULL);
    KT_ASSERT(t != NULL);
    void *base = t->kstack;
    sleep_ms(10);                   /* let it run and exit */
    KT_EXPECT(kstack_used(base, KSTACK_SIZE) >= 6000);

    u64 v;
    KT_EXPECT(addr_in_stack_guard((uintptr_t)base - 8));
    KT_EXPECT_EQ(safe_read_u64((uintptr_t)base - 8, &v), -1);      /* guard unmapped */
    KT_EXPECT(!addr_in_stack_guard((uintptr_t)base + 8));
    kthread_join(t);
}

KTEST(trace_records_context_switches)
{
    extern struct trace_buf trace_bufs[];
    u32 saved = trace_groups_enabled;
    trace_groups_enabled = TG_ALL & ~TG_MM;
    trace_clear();

    flag = 0;
    struct thread *t = kthread_create("traced", set_flag_and_exit, (void *)1);
    KT_ASSERT(t != NULL);
    int tid = t->tid;
    kthread_join(t);

    /* The events may be on any CPU's ring (each records its own). */
    bool created = false, switched_in = false, exited = false;
    for (int c = 0; c < ncpus_online; c++) {
        struct trace_buf *b = &trace_bufs[c];
        for (u64 i = 0; i < MIN(b->head, (u64)TRACE_RECS); i++) {
            struct trace_rec *r = &b->recs[i];
            if (r->type == TE_thread_create && r->a[0] == (u64)tid)
                created = true;
            if (r->type == TE_sched_switch && r->a[1] == (u64)tid)
                switched_in = true;
            if (r->type == TE_thread_exit && r->a[0] == (u64)tid && r->a[1] == 42)
                exited = true;
        }
    }
    KT_EXPECT(created);
    KT_EXPECT(switched_in);
    KT_EXPECT(exited);
    trace_groups_enabled = saved;
}

/*
 * Regression test for a lost wakeup: the condition becomes true (and the
 * wakeup is spent) before the waiter queues itself; then the waiter is
 * preempted between prepare_to_wait() and checking its condition. Preemption
 * must not put it to sleep. (With the bug, this test hangs.)
 */
static struct waitqueue lw_wq = WAITQUEUE_INIT(lw_wq, "lost-wakeup");
static volatile bool lw_done;

static bool preempted_check(void)
{
    preempt_schedule();             /* "timer interrupt" right after prepare_to_wait */
    return lw_done;
}

static void lw_waiter(void *arg)
{
    (void)arg;
    wait_event(&lw_wq, preempted_check());
}

KTEST(preemption_does_not_lose_wakeups)
{
    lw_done = true;
    wake_up(&lw_wq);                /* nobody waiting yet: this wakeup is spent */
    struct thread *t = kthread_create("lw-waiter", lw_waiter, NULL);
    KT_ASSERT(t != NULL);
    kthread_join(t);                /* hangs if the waiter slept forever */
    KT_EXPECT(true);
}
