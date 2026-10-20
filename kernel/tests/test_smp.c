/*
 * Multiprocessor tests: every CPU runs threads, locks hold under real
 * contention, IPIs arrive, TLB shootdown really invalidates other CPUs'
 * translations, stop-the-world parks everyone, and the lock debugging
 * (lockdep, hung-thread detector) catches what it should.
 *
 * Tests that need several CPUs pass trivially on one (make test runs -smp 4).
 */
#include <debug.h>
#include <kstring.h>
#include <ktest.h>
#include <lockdep.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <smp.h>

static u64 ms_now(void)
{
    return rdtime() * 1000 / platform.timebase;
}

/* Wait (yielding) until *v >= want or ms pass. */
static bool wait_for(volatile int *v, int want, u64 ms)
{
    u64 end = ms_now() + ms;
    while (__atomic_load_n(v, __ATOMIC_ACQUIRE) < want) {
        if (ms_now() > end)
            return false;
        yield();
    }
    return true;
}

KTEST(smp_all_harts_online)
{
    int want = MIN((int)platform.ncpus, MAX_CPUS);
    const char *opt = cmdline_get("smp");
    if (opt && *opt)
        want = MIN(want, (int)kstrtoul(opt, NULL, 0));
    KT_EXPECT_EQ(ncpus_online, MAX(want, 1));
    for (int i = 0; i < ncpus_online; i++) {
        KT_EXPECT(cpus[i].online);
        KT_EXPECT_EQ(cpus[i].id, i);
        KT_EXPECT(cpus[i].idle != NULL);
        KT_EXPECT(cpus[i].ticks > 0);            /* every CPU has its own tick */
    }
}

static volatile int ran_on[MAX_CPUS];

static void record_cpu(void *arg)
{
    int want = (int)(long)arg;
    ran_on[want] = this_cpu_id() == want ? 1 : -1;
}

KTEST(bound_threads_run_on_every_cpu)
{
    struct thread *t[MAX_CPUS];
    for (int i = 0; i < ncpus_online; i++) {
        ran_on[i] = 0;
        t[i] = kthread_create_suspended("kt-bound", record_cpu, (void *)(long)i);
        KT_ASSERT(t[i] != NULL);
        kthread_bind(t[i], i);
        kthread_start(t[i]);
    }
    for (int i = 0; i < ncpus_online; i++) {
        kthread_join(t[i]);
        KT_EXPECT_EQ(ran_on[i], 1);
    }
}

#define ADDERS      4
#define ADD_ROUNDS  20000

static spinlock_t count_lock = SPINLOCK_INIT("kt-counter");
static struct mutex count_mutex = MUTEX_INIT(count_mutex, "kt-counter-mutex");
static volatile u64 count;
static volatile int cpus_seen;

static void spin_adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < ADD_ROUNDS; i++) {
        unsigned long flags = spin_lock_irqsave(&count_lock);
        u64 v = count;              /* non-atomic read-modify-write */
        count = v + 1;
        spin_unlock_irqrestore(&count_lock, flags);
    }
    __atomic_or_fetch(&cpus_seen, 1 << this_cpu_id(), __ATOMIC_RELAXED);
}

KTEST(spinlock_exclusion_under_contention)
{
    struct thread *t[ADDERS];
    count = 0;
    cpus_seen = 0;
    u64 contended = count_lock.contentions;
    for (int i = 0; i < ADDERS; i++)
        KT_ASSERT((t[i] = kthread_create("kt-spinadd", spin_adder, NULL)) != NULL);
    for (int i = 0; i < ADDERS; i++)
        kthread_join(t[i]);
    KT_EXPECT_EQ(count, (u64)ADDERS * ADD_ROUNDS);
    klog(LOG_INFO, SUBSYS_TEST, "    %d adders on CPU mask %#x, %lu contended acquisitions\n",
         ADDERS, cpus_seen, (unsigned long)(count_lock.contentions - contended));
}

static void mutex_adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < ADD_ROUNDS / 10; i++) {
        mutex_lock(&count_mutex);
        u64 v = count;
        if (i % 64 == 0)
            yield();                /* sleep-ish while holding it: others must block */
        count = v + 1;
        mutex_unlock(&count_mutex);
    }
}

KTEST(mutex_exclusion_under_contention)
{
    struct thread *t[ADDERS];
    count = 0;
    for (int i = 0; i < ADDERS; i++)
        KT_ASSERT((t[i] = kthread_create("kt-mutexadd", mutex_adder, NULL)) != NULL);
    for (int i = 0; i < ADDERS; i++)
        kthread_join(t[i]);
    KT_EXPECT_EQ(count, (u64)ADDERS * (ADD_ROUNDS / 10));
}

KTEST(ipi_reaches_every_cpu)
{
    for (int i = 0; i < ncpus_online; i++) {
        intr_off();
        bool self = i == this_cpu_id();
        u64 before = cpus[i].ipi_received[IPI_RESCHED];
        if (!self)
            smp_send_ipi(i, IPI_RESCHED);
        intr_on();
        if (self)
            continue;
        u64 end = ms_now() + 200;
        while (cpus[i].ipi_received[IPI_RESCHED] == before && ms_now() < end)
            yield();
        KT_EXPECT(cpus[i].ipi_received[IPI_RESCHED] > before);
    }
}

/*
 * TLB shootdown: a thread on every CPU reads a kernel page (caching its
 * translation), the page is unmapped and shot down, and then every CPU must
 * fault on it. Without the shootdown QEMU's per-CPU TLBs keep serving the
 * stale translation (try with the tlb_shootdown() call removed).
 */
#define TLB_TEST_VA (KSTACK_AREA + (uintptr_t)KSTACK_MAX * KSTACK_SLOT)
static volatile int tlb_arrived, tlb_go, tlb_read_ok, tlb_faulted;

static void tlb_reader(void *arg)
{
    (void)arg;
    u64 v = 0;
    if (safe_read_u64(TLB_TEST_VA, &v) == 0 && v == 0x5eed5eed)
        __atomic_add_fetch(&tlb_read_ok, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&tlb_arrived, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&tlb_go, __ATOMIC_ACQUIRE))
        asm volatile("nop");        /* stay on this CPU, translation cached */
    if (safe_read_u64(TLB_TEST_VA, &v) != 0)
        __atomic_add_fetch(&tlb_faulted, 1, __ATOMIC_RELAXED);
}

KTEST(tlb_shootdown_invalidates_every_cpu)
{
    u64 *page = page_alloc_zeroed();
    KT_ASSERT(page != NULL);
    page[0] = 0x5eed5eed;
    KT_ASSERT(vm_map(kernel_pagetable, TLB_TEST_VA, __pa(page), PAGE_SIZE, PROT_KERNEL_RW) == 0);
    tlb_arrived = tlb_go = tlb_read_ok = tlb_faulted = 0;

    struct thread *t[MAX_CPUS];
    for (int i = 0; i < ncpus_online; i++) {
        t[i] = kthread_create_suspended("kt-tlb", tlb_reader, NULL);
        KT_ASSERT(t[i] != NULL);
        kthread_bind(t[i], i);
        kthread_start(t[i]);
    }
    KT_EXPECT(wait_for(&tlb_arrived, ncpus_online, 2000));
    u64 shootdowns = tlb_shootdowns;
    vm_unmap(kernel_pagetable, TLB_TEST_VA, PAGE_SIZE);
    tlb_shootdown();
    __atomic_store_n(&tlb_go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < ncpus_online; i++)
        kthread_join(t[i]);
    page_free(page);

    KT_EXPECT_EQ(tlb_read_ok, ncpus_online);
    KT_EXPECT_EQ(tlb_faulted, ncpus_online);
    if (ncpus_online > 1)
        KT_EXPECT(tlb_shootdowns > shootdowns);
}

KTEST(stop_the_world_parks_every_other_cpu)
{
    if (ncpus_online < 2)
        return;
    /* No printing while stopped: a parked CPU might hold the log lock. */
    intr_off();
    int me = this_cpu_id();
    u32 missing = 0, parked = 0;
    bool owned = smp_stop_others(&missing);
    if (owned) {
        for (int i = 0; i < ncpus_online; i++)
            if (i != me && cpus[i].stopped && cpus[i].stop_pc)
                parked |= 1u << i;
        smp_resume_others();
    }
    intr_on();
    KT_EXPECT(owned);
    KT_EXPECT_EQ(missing, 0);
    KT_EXPECT_EQ(parked, ((1u << ncpus_online) - 1) & ~(1u << me));
    u64 end = ms_now() + 200;
    for (int i = 0; i < ncpus_online; i++)
        while (cpus[i].stopped && ms_now() < end)
            yield();
    for (int i = 0; i < ncpus_online; i++)
        KT_EXPECT(!cpus[i].stopped);
}

static void nothing(void *arg)
{
    (void)arg;
}

/* Kernel stacks freed on one CPU and reused on another (TLB shootdowns). */
KTEST(thread_churn_across_cpus)
{
    for (int round = 0; round < 25; round++) {
        struct thread *t[8];
        for (int i = 0; i < 8; i++)
            KT_ASSERT((t[i] = kthread_create("kt-churn", nothing, NULL)) != NULL);
        for (int i = 0; i < 8; i++)
            kthread_join(t[i]);
    }
    KT_EXPECT_EQ(pmm_verify_free(), 0);
    KT_EXPECT_EQ(slab_verify(), 0);
}

static spinlock_t ld_a = SPINLOCK_INIT("kt-ld-A");
static spinlock_t ld_b = SPINLOCK_INIT("kt-ld-B");
static spinlock_t ld_c = SPINLOCK_INIT("kt-ld-C");
static spinlock_t ld_d = SPINLOCK_INIT("kt-ld-D");
static spinlock_t ld_e = SPINLOCK_INIT("kt-ld-E");
static struct mutex ld_m = MUTEX_INIT(ld_m, "kt-ld-M");

static void nest(spinlock_t *outer, spinlock_t *inner)
{
    unsigned long f1 = spin_lock_irqsave(outer);
    unsigned long f2 = spin_lock_irqsave(inner);
    spin_unlock_irqrestore(inner, f2);
    spin_unlock_irqrestore(outer, f1);
}

KTEST(lockdep_catches_inversions_before_they_deadlock)
{
    if (!lockdep_enabled)
        return;                     /* lockdep=off */
    lockdep_testing = true;
    u64 before = lockdep_reports;

    nest(&ld_a, &ld_b);             /* A -> B */
    nest(&ld_a, &ld_b);
    KT_EXPECT_EQ(lockdep_reports, before);
    nest(&ld_b, &ld_a);             /* B -> A: inversion */
    KT_EXPECT_EQ(lockdep_reports, before + 1);

    nest(&ld_c, &ld_d);             /* a longer cycle: C -> D -> E -> C */
    nest(&ld_d, &ld_e);
    KT_EXPECT_EQ(lockdep_reports, before + 1);
    nest(&ld_e, &ld_c);
    KT_EXPECT_EQ(lockdep_reports, before + 2);

    /* A sleeping lock under a spinlock is reported even when uncontended. */
    unsigned long f = spin_lock_irqsave(&ld_a);
    mutex_lock(&ld_m);
    mutex_unlock(&ld_m);
    spin_unlock_irqrestore(&ld_a, f);
    KT_EXPECT_EQ(lockdep_reports, before + 3);

    lockdep_testing = false;
    lockdep_reports = before;       /* these were expected */
}

static struct mutex hung_m = MUTEX_INIT(hung_m, "kt-hung");
static volatile int hung_waiting;

static void hung_waiter(void *arg)
{
    (void)arg;
    __atomic_store_n(&hung_waiting, 1, __ATOMIC_RELEASE);
    mutex_lock(&hung_m);
    mutex_unlock(&hung_m);
}

KTEST(hung_thread_detector_reports_long_mutex_waits)
{
    u64 old = hung_set_threshold_ms(50);
    mutex_lock(&hung_m);
    hung_waiting = 0;
    struct thread *t = kthread_create("kt-hung", hung_waiter, NULL);
    KT_ASSERT(t != NULL);
    KT_EXPECT(wait_for(&hung_waiting, 1, 1000));
    sleep_ms(150);
    /* (the check skips a round when the scheduler lock is busy) */
    for (int i = 0; i < 100 && !t->hung_reported; i++) {
        hung_thread_check();
        if (!t->hung_reported)
            sleep_ms(2);
    }
    KT_EXPECT(t->blocked_on == &hung_m);
    KT_EXPECT(t->hung_reported);
    mutex_unlock(&hung_m);
    kthread_join(t);
    hung_set_threshold_ms(old);
}
