/*
 * Periodic timer tick via the SBI timer, on every CPU. Wall-clock time
 * always comes from rdtime, so ticks lost while interrupts are off (e.g.
 * sitting in the debug monitor) don't skew sleeps; missed_ticks counts them.
 * jiffies advances on cpu0 only.
 *
 * Hard-lockup watchdog: every tick doubles as a heartbeat (cpu->last_tick).
 * Each CPU's tick checks the others; a CPU without a tick for
 * `watchdog=<seconds>` (default 10, 0 = off) has had interrupts off all
 * that time, which is a kernel bug: panic. The panic stops the other CPUs;
 * if the stuck one is spinning on a lock it parks too, and the report shows
 * where it was.
 */
#define LOG_SUBSYS SUBSYS_SCHED
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <sbi.h>
#include <sched.h>
#include <profile.h>
#include <smp.h>

volatile u64 jiffies;
bool timer_running;
static u64 interval;            /* timebase ticks between timer interrupts */
static u32 per_tick = 1;        /* timer interrupts per scheduler tick (profiling: more) */
static u64 lockup_ticks;

void timer_init_cpu(void)
{
    struct cpu *c = this_cpu();
    u64 now = rdtime();
    c->last_tick = now;
    c->next_deadline = now + interval;
    sbi_set_timer(c->next_deadline);
    csr_set(sie, SIE_STIE);
}

void timer_init(void)
{
    interval = platform.timebase / HZ;
    u64 secs = 10;
    const char *w = cmdline_get("watchdog");
    if (w && *w)
        secs = kstrtoul(w, NULL, 0);
    lockup_ticks = secs * platform.timebase;
    timer_running = true;
    timer_init_cpu();
    pr_info("timer: %d Hz tick (%lu timebase ticks) on every CPU; lockup watchdog %s%lu s\n",
            HZ, (unsigned long)interval, secs ? "" : "off, ", (unsigned long)secs);
}

void watchdog_check(void)
{
    if (!lockup_ticks || ncpus_online < 2 || smp_world_stopped() || monitor_entry_time ||
        panic_depth)
        return;
    u64 now = rdtime();
    int me = this_cpu()->id;
    for (int i = 0; i < ncpus_online; i++) {
        struct cpu *c = &cpus[i];
        u64 last = c->last_tick;
        if (i == me || !c->online || c->stopped || now <= last || now - last < lockup_ticks)
            continue;
        bool expected = false;
        if (!__atomic_compare_exchange_n(&c->lockup_reported, &expected, true, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        struct thread *t = c->cur_thread;
        char held[96] = "";
        size_t n = 0;
        for (int j = 0; j < MIN(c->nheld, MAX_HELD_LOCKS); j++)
            n += ksnprintf(held + n, sizeof(held) - n, "%s'%s'", j ? ", " : "; holding ",
                           c->held[j]->name);
        panic("watchdog: hard lockup on cpu%d: no timer interrupt for %lu ms "
              "(interrupts off); it is running thread %d (%s)%s",
              i, (unsigned long)((now - last) * 1000 / platform.timebase),
              t ? t->tid : -1, t ? t->name : "?", held);
    }
}

/*
 * Profiling: the timer interrupts PROFILE_HZ times a second, each one
 * takes a sample, and every per_tick-th one is also a scheduler tick.
 */
void timer_set_profile_hz(u32 hz)
{
    u32 n = hz > HZ ? hz / HZ : 1;
    u64 iv = platform.timebase / ((u64)HZ * n);
    __atomic_store_n(&interval, iv, __ATOMIC_RELAXED);
    __atomic_store_n(&per_tick, n, __ATOMIC_RELAXED);
}

void timer_interrupt(void)
{
    struct cpu *c = this_cpu();
    u64 now = rdtime();
    u64 iv = interval;
    c->next_deadline += iv;
    if (c->next_deadline <= now) {
        c->missed_ticks += (now - c->next_deadline) / iv + 1;
        c->next_deadline = now + iv;
    }
    sbi_set_timer(c->next_deadline);
    c->last_tick = now;
    if (profiling)
        profile_sample(c->irq_tf);
    if (++c->subticks < per_tick)
        return;
    c->subticks = 0;
    if (c->id == 0)
        jiffies++;
    c->ticks++;
    watchdog_check();
    sched_tick();
}

/* Restore the tick deadline after someone else borrowed the timer (cpu_wait_ticks). */
void timer_rearm(void)
{
    struct cpu *c = this_cpu();
    u64 now = rdtime();
    if (c->next_deadline <= now)
        c->next_deadline = now + interval;
    sbi_set_timer(c->next_deadline);
}
