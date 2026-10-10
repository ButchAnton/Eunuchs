/*
 * SMP: bringing up the secondary harts, inter-processor interrupts, TLB
 * shootdown, and stopping the world for the debug monitor and panics.
 *
 * IPIs: the sender sets a bit in the target's cpu->ipi_pending and raises a
 * supervisor software interrupt on it via SBI. The handler (ipi_handle)
 * atomically takes all pending bits. Code that spins with interrupts off
 * (spinlocks, TLB shootdown waits, a parked CPU) calls ipi_poll(), which
 * services TLB-flush and stop requests directly: so a CPU spinning on a lock
 * still parks for the monitor (and shows where it was spinning), and two
 * CPUs shooting down each other's TLBs at the same time cannot deadlock.
 *
 * TLB shootdown: the initiator bumps tlb_gen and waits until every other
 * online CPU's tlb_done_gen has caught up; a target reads tlb_gen *before*
 * its sfence.vma, so its flush covers every change made before that
 * generation. Only kernel mappings need it: a user page table is live on one
 * CPU at a time, and every satp switch flushes.
 *
 * Stop-the-world: one CPU at a time owns `stop_owner` (the debug monitor or
 * a panic); the others park in cpu_park() with their interrupted register
 * state recorded, which `cpus` / `cpu <n>` in the monitor and panic reports
 * display.
 *
 * Debugging: `cpus` and `irq` (per-CPU IPI counts) in the monitor,
 * /proc/cpus, ipi_send/ipi_recv trace events, boot option smp=<n> to limit
 * the number of harts (smp=1: uniprocessor).
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
#include <sbi.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>

#define STOP_TIMEOUT_MS      200
#define SHOOTDOWN_TIMEOUT_MS 3000

int ncpus_online = 1;
static volatile u64 tlb_gen;
static volatile int stop_owner = -1;    /* CPU that has the others stopped */
u64 tlb_shootdowns;

const char *const ipi_names[IPI_MAX] = { "resched", "tlb-flush", "stop" };

extern char secondary_entry[];

static inline void cpu_relax(void)
{
    asm volatile("nop; nop; nop; nop" ::: "memory");
}

/* Frame pointer of our caller (RISC-V: saved fp sits at fp - 16). */
#define CALLER_FP() (*(uintptr_t *)((uintptr_t)__builtin_frame_address(0) - 16))

static u64 ms_ticks(u64 ms)
{
    return ms * (platform.timebase ? platform.timebase : 10000000) / 1000;
}

/* ---- sending and receiving ---- */

void smp_send_ipi(int cpu, enum ipi_kind kind)
{
    struct cpu *c = &cpus[cpu];
    __atomic_or_fetch(&c->ipi_pending, 1u << kind, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cpus[this_cpu_id()].ipi_sent[kind], 1, __ATOMIC_RELAXED);
    trace(ipi_send, cpu, kind, 0);
    sbi_send_ipi(1, c->hartid);
}

static void tlb_flush_ack(struct cpu *c)
{
    u64 g = __atomic_load_n(&tlb_gen, __ATOMIC_ACQUIRE);
    sfence_vma_all();
    if (g > c->tlb_done_gen)
        __atomic_store_n(&c->tlb_done_gen, g, __ATOMIC_RELEASE);
}

/*
 * Park until the stop owner releases us. Interrupts are off. tf is the
 * interrupted context (from an IPI); from a spin loop, pc/fp say where.
 */
static void cpu_park(struct trap_frame *tf, uintptr_t pc, uintptr_t fp)
{
    struct cpu *c = this_cpu();
    if (c->stopped)
        return;
    u64 t0 = rdtime();
    c->stop_tf = tf;
    c->stop_pc = tf ? tf->sepc : pc;
    c->stop_fp = tf ? TF_FP(tf) : fp;
    trace(cpu_stop, c->stop_pc, 0, 0);
    __atomic_store_n(&c->stopped, true, __ATOMIC_RELEASE);
    while (__atomic_load_n(&stop_owner, __ATOMIC_ACQUIRE) >= 0 &&
           stop_owner != c->id) {
        if (c->ipi_pending & (1u << IPI_TLB_FLUSH)) {
            __atomic_and_fetch(&c->ipi_pending, ~(1u << IPI_TLB_FLUSH), __ATOMIC_ACQ_REL);
            tlb_flush_ack(c);
        }
        cpu_relax();
    }
    /* The stopped time is nobody's CPU time, and not a lockup either. */
    u64 now = rdtime();
    c->last_switch += now - t0;
    c->last_tick = now;
    c->stop_tf = NULL;
    __atomic_store_n(&c->stopped, false, __ATOMIC_RELEASE);
}

/* Supervisor software interrupt. */
void ipi_handle(struct trap_frame *tf)
{
    struct cpu *c = this_cpu();
    csr_clear(sip, SIE_SSIE);
    u32 pending = __atomic_exchange_n(&c->ipi_pending, 0, __ATOMIC_ACQ_REL);
    for (int k = 0; k < IPI_MAX; k++) {
        if (pending & (1u << k)) {
            c->ipi_received[k]++;
            trace(ipi_recv, k, 0, 0);
        }
    }
    if (pending & (1u << IPI_RESCHED))
        c->need_resched = true;
    if (pending & (1u << IPI_TLB_FLUSH))
        tlb_flush_ack(c);
    if (pending & (1u << IPI_STOP))
        cpu_park(tf, 0, 0);
}

/* From spin loops with interrupts off: service requests that cannot wait. */
void ipi_poll(void)
{
    struct cpu *c = this_cpu();
    const u32 urgent = (1u << IPI_TLB_FLUSH) | (1u << IPI_STOP);
    if (!(c->ipi_pending & urgent))
        return;
    u32 p = __atomic_fetch_and(&c->ipi_pending, ~urgent, __ATOMIC_ACQ_REL) & urgent;
    if (p & (1u << IPI_TLB_FLUSH))
        tlb_flush_ack(c);
    if (p & (1u << IPI_STOP))
        cpu_park(NULL, (uintptr_t)__builtin_return_address(0), CALLER_FP());
}

/* ---- TLB shootdown ---- */

/*
 * Flush every CPU's TLB after a kernel mapping was removed or changed. Must
 * not be called with spinlocks held by code that the targets might be
 * waiting for with interrupts on... which cannot happen: spinlock holders
 * have interrupts off, and spinners poll. So this is safe from anywhere
 * except interrupt handlers that interrupted a shootdown (none do).
 */
void tlb_shootdown(void)
{
    bool irqs = intr_enabled();
    intr_off();
    struct cpu *me = this_cpu();
    sfence_vma_all();
    if (ncpus_online > 1) {
        u64 g = __atomic_add_fetch(&tlb_gen, 1, __ATOMIC_SEQ_CST);
        me->tlb_done_gen = g;
        tlb_shootdowns++;
        for (int i = 0; i < ncpus_online; i++)
            if (i != me->id && cpus[i].tlb_done_gen < g)
                smp_send_ipi(i, IPI_TLB_FLUSH);
        u64 start = rdtime();
        for (int i = 0; i < ncpus_online; i++) {
            while (__atomic_load_n(&cpus[i].tlb_done_gen, __ATOMIC_ACQUIRE) < g) {
                ipi_poll();
                cpu_relax();
                if (rdtime() - start > ms_ticks(SHOOTDOWN_TIMEOUT_MS)) {
                    struct thread *t = cpus[i].cur_thread;
                    panic("TLB shootdown: cpu%d did not respond within %d ms "
                          "(running thread %d (%s); interrupts off for too long?)",
                          i, SHOOTDOWN_TIMEOUT_MS, t ? t->tid : -1, t ? t->name : "?");
                }
            }
        }
    }
    if (irqs)
        intr_on();
}

/* ---- stop-the-world ---- */

/*
 * Stop all other CPUs. Returns false if another CPU already owns the stop:
 * the caller should park (smp_park_self) and try again. *missing gets the
 * mask of CPUs that did not park in time (interrupts off and not spinning).
 */
bool smp_stop_others(u32 *missing)
{
    int me = this_cpu_id();
    *missing = 0;
    if (ncpus_online <= 1)
        return true;
    int expected = -1;
    if (stop_owner != me &&
        !__atomic_compare_exchange_n(&stop_owner, &expected, me, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    for (int i = 0; i < ncpus_online; i++)
        if (i != me && !cpus[i].stopped)
            smp_send_ipi(i, IPI_STOP);
    u64 start = rdtime();
    for (int i = 0; i < ncpus_online; i++) {
        if (i == me)
            continue;
        while (!__atomic_load_n(&cpus[i].stopped, __ATOMIC_ACQUIRE) &&
               rdtime() - start < ms_ticks(STOP_TIMEOUT_MS))
            cpu_relax();
        if (!cpus[i].stopped)
            *missing |= 1u << i;
    }
    return true;
}

void smp_resume_others(void)
{
    if (stop_owner != this_cpu_id())
        return;
    for (int i = 0; i < ncpus_online; i++)
        cpus[i].last_tick = rdtime();       /* the watchdog must not count the pause */
    __atomic_store_n(&stop_owner, -1, __ATOMIC_RELEASE);
}

bool smp_stopped_by_me(void)
{
    return stop_owner == this_cpu_id();
}

bool smp_world_stopped(void)
{
    return stop_owner >= 0;
}

/* Someone else owns the stop: park here (interrupts off) until released. */
void smp_park_self(struct trap_frame *tf)
{
    bool irqs = intr_enabled();
    intr_off();
    cpu_park(tf, (uintptr_t)__builtin_return_address(0), CALLER_FP());
    if (irqs)
        intr_on();
}

/* ---- bring-up ---- */

void secondary_main(struct cpu *c)
{
    trap_init();
    csr_set(sie, SIE_SSIE);
    sfence_vma_all();
    timer_init_cpu();
    c->last_switch = rdtime();
    __atomic_add_fetch(&ncpus_online, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    pr_info("cpu%d online (hart %lu, idle thread %d)\n", c->id, c->hartid, c->idle->tid);
    cpu_idle_loop();
}

static bool start_cpu(int id, unsigned long hartid)
{
    struct cpu *c = &cpus[id];
    void *ovf = kstack_alloc();
    struct thread *idle = sched_create_idle(id);
    if (!ovf || !idle) {
        pr_err("cpu%d: out of memory for its stacks\n", id);
        return false;
    }
    c->id = id;
    c->hartid = hartid;
    c->overflow_stack_top = (uintptr_t)ovf + KSTACK_SIZE;
    c->idle = c->cur_thread = idle;
    c->kstack_top = (uintptr_t)idle->kstack + idle->kstack_size;
    c->last_tick = rdtime();

    long err = sbi_hart_start(hartid, __pa(secondary_entry), (unsigned long)c);
    if (err) {
        pr_err("cpu%d: SBI hart_start(hart %lu) failed: error %ld (hart status %ld)\n",
               id, hartid, err, sbi_hart_status(hartid));
        return false;
    }
    u64 start = rdtime();
    while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE)) {
        if (rdtime() - start > ms_ticks(2000)) {
            pr_err("cpu%d (hart %lu) did not come online (hart status %ld)\n", id, hartid,
                   sbi_hart_status(hartid));
            return false;
        }
        cpu_relax();
    }
    return true;
}

void smp_init(void)
{
    csr_set(sie, SIE_SSIE);             /* IPIs for the boot CPU too */
    cpus[0].online = true;
    cpus[0].last_tick = rdtime();

    long max = platform.ncpus;
    const char *opt = cmdline_get("smp");
    if (opt && *opt)
        max = kstrtoul(opt, NULL, 0);
    if (max > MAX_CPUS)
        max = MAX_CPUS;
    if (max <= 1 || platform.ncpus <= 1) {
        pr_info("smp: uniprocessor (%u hart%s in the device tree%s)\n", platform.ncpus,
                platform.ncpus == 1 ? "" : "s", max <= 1 && platform.ncpus > 1 ? ", smp=1" : "");
        return;
    }
    if (!sbi_probe(SBI_EXT_HSM)) {
        pr_warn("smp: firmware has no HSM extension; running on the boot hart only\n");
        return;
    }

    int id = 1;
    for (u32 i = 0; i < platform.ncpus && i < ARRAY_SIZE(platform.hartids) && id < max; i++) {
        unsigned long hart = platform.hartids[i];
        if (hart == platform.boot_hart)
            continue;
        if (!start_cpu(id, hart))
            break;
        id++;
    }
    pr_info("smp: %d of %u harts online (boot hart %lu is cpu0)\n", ncpus_online,
            platform.ncpus, platform.boot_hart);
}

/* ---- debugging ---- */

void cpus_print(print_fn print)
{
    u64 tb = platform.timebase ? platform.timebase : 10000000;
    u64 now = monitor_entry_time ? monitor_entry_time : rdtime();
    print("  CPU HART STATE     THREAD               IDLE%%  SWITCH  MIGR   TICKS  IPI-IN   WHERE\n");
    for (int i = 0; i < ncpus_online; i++) {
        struct cpu *c = &cpus[i];
        struct thread *t = c->cur_thread;
        const char *state = c->id == this_cpu_id() ? "this-cpu"
                          : c->stopped ? "stopped"
                          : smp_world_stopped() ? "no-stop!"
                          : t == c->idle ? "idle" : "running";
        u64 idle = c->idle->runtime + (t == c->idle ? now - c->last_switch : 0);
        u64 ipis = 0;
        for (int k = 0; k < IPI_MAX; k++)
            ipis += c->ipi_received[k];
        char where[80] = "";
        if (c->stopped && c->stop_pc)
            ksym_format(where, sizeof(where), c->stop_pc);
        char thr[24];
        ksnprintf(thr, sizeof(thr), "%d(%s)", t ? t->tid : -1, t ? t->name : "-");
        print("  %3d %4lu %-8s %-20s %4lu  %7lu %5lu %7lu %6lu   %s\n", i, c->hartid, state, thr,
              (unsigned long)(now ? idle * 100 / now : 0), (unsigned long)c->switches,
              (unsigned long)c->migrations, (unsigned long)c->ticks, (unsigned long)ipis, where);
        if (c->nheld) {
            for (int j = 0; j < MIN(c->nheld, MAX_HELD_LOCKS); j++) {
                char s[80];
                ksym_format(s, sizeof(s), c->held[j]->owner_pc);
                print("        holds spinlock '%s' (taken at %s)\n", c->held[j]->name, s);
            }
        }
    }
    print("  IPIs sent/received:");
    for (int k = 0; k < IPI_MAX; k++) {
        u64 s = 0, r = 0;
        for (int i = 0; i < ncpus_online; i++)
            s += cpus[i].ipi_sent[k], r += cpus[i].ipi_received[k];
        print(" %s %lu/%lu", ipi_names[k], (unsigned long)s, (unsigned long)r);
    }
    print("; TLB shootdowns %lu\n", (unsigned long)tlb_shootdowns);
    (void)tb;
}

/* Backtrace of a parked CPU from where it stopped. */
int cpu_backtrace(print_fn print, int id)
{
    if (id < 0 || id >= ncpus_online) {
        print("no cpu%d (%d online)\n", id, ncpus_online);
        return -1;
    }
    struct cpu *c = &cpus[id];
    if (id == this_cpu_id()) {
        backtrace_here(print);
        return 0;
    }
    if (!c->stopped) {
        print("cpu%d is not stopped (did not respond to the stop IPI: interrupts off and "
              "not spinning?); thread %d (%s)\n", id, c->cur_thread ? c->cur_thread->tid : -1,
              c->cur_thread ? c->cur_thread->name : "?");
        return -1;
    }
    backtrace_print(print, c->stop_pc, c->stop_fp);
    return 0;
}
