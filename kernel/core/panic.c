#define LOG_SUBSYS SUBSYS_PANIC
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>

int panic_depth;
static volatile int panic_cpu = -1;
static u32 panic_unstopped;     /* CPUs that did not park when we stopped them */

/* Panic output goes to the log ring too, so `dmesg`/`eu-dmesg` keep it. */
int panic_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_ERR, SUBSYS_PANIC, fmt, ap);
    va_end(ap);
    return 0;
}

/*
 * One CPU panics; the others are stopped so the machine state stays as it
 * was and their output doesn't interleave. A CPU that panics while another
 * one already is just parks (the first report shows where it is).
 */
static void panic_begin(struct trap_frame *tf)
{
    intr_off();
    int me = this_cpu()->id;
    int expected = -1;
    if (!__atomic_compare_exchange_n(&panic_cpu, &expected, me, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE) && expected != me) {
        for (;;)
            smp_park_self(tf);
    }
    if (++panic_depth > 2) {
        kprintf("\n*** recursive panic; halting ***\n");
        platform_exit(3);
    }
    if (panic_depth == 1) {
        /* If the monitor has the world stopped on another CPU, wait for it. */
        while (!smp_stop_others(&panic_unstopped))
            smp_park_self(tf);
    }
}

static void panic_other_cpus(void)
{
    int me = this_cpu()->id;
    for (int i = 0; i < ncpus_online; i++) {
        if (i == me)
            continue;
        struct cpu *c = &cpus[i];
        struct thread *t = c->cur_thread;
        panic_print("cpu%d (hart %lu): thread %d (%s)%s\n", i, c->hartid, t ? t->tid : -1,
                    t ? t->name : "?", c->irq_depth ? ", in interrupt context" : "");
        if (c->nheld)
            spin_print_held_cpu(panic_print, i);
        if (c->stopped) {
            backtrace_print(panic_print, c->stop_pc, c->stop_fp);
        } else {
            panic_print("  did not stop: interrupts off and not spinning on a lock%s\n",
                        panic_unstopped & (1u << i) ? "" : " (yet)");
        }
    }
}

/*
 * After a panic: with `panic=shutdown` (used by `make test`) exit QEMU with a
 * failure code; otherwise enter the debug monitor for post-mortem inspection.
 */
/* Context common to all panics: who was running, what locks, recent events. */
static void panic_context(void)
{
    struct cpu *c = this_cpu();
    if (c->cur_thread)
        panic_print("current thread: %d (%s)%s\n", c->cur_thread->tid, c->cur_thread->name,
                    c->irq_depth ? ", in interrupt context" : "");
    if (c->nheld)
        spin_print_held_cpu(panic_print, c->id);
    if (panic_depth == 1 && ncpus_online > 1) {
        panic_print("other CPUs:\n");
        panic_other_cpus();
    }
    if (panic_depth == 1) {
        panic_print("last trace events:\n");
        trace_print(panic_print, 16);
    }
}

static __noreturn void panic_finish(struct trap_frame *tf)
{
    const char *mode = cmdline_get("panic");
    if (mode && !strcmp(mode, "shutdown"))
        platform_exit(2);
    monitor_enter("panic", tf, false);
    kprintf("halting.\n");
    platform_exit(2);
}

void panic(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    panic_begin(NULL);
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    panic_print("KERNEL PANIC on cpu%d (hart %lu): %s\n", this_cpu()->id, this_cpu()->hartid, msg);
    panic_print("backtrace:\n");
    /*
     * Start at our caller: its fp is our saved fp, and its pc is inside the
     * call instruction (ra - 2; ra itself may lie past the end of the caller
     * because panic() is noreturn).
     */
    u64 caller_fp = 0;
    safe_read_u64(current_fp() - 16, &caller_fp);
    backtrace_print(panic_print, (uintptr_t)__builtin_return_address(0) - 2, caller_fp);
    panic_context();
    panic_finish(NULL);
}

void panic_trap(struct trap_frame *tf)
{
    char sym[96];

    panic_begin(tf);
    ksym_format(sym, sizeof(sym), tf->sepc);
    panic_print("KERNEL PANIC on cpu%d (hart %lu): unhandled %s in %s mode at %s\n",
                this_cpu()->id, this_cpu()->hartid, trap_cause_name(tf->scause),
                (tf->sstatus & SSTATUS_SPP) ? "kernel" : "user", sym);

    switch (SCAUSE_CODE(tf->scause)) {
    case EXC_ILLEGAL_INSN:
        panic_print("  instruction bits: %#lx\n", (unsigned long)tf->stval);
        break;
    case EXC_BREAKPOINT:
    case EXC_ECALL_S:
    case EXC_ECALL_U:
        break;
    case EXC_INSN_PAGE_FAULT:
    case EXC_LOAD_PAGE_FAULT:
    case EXC_STORE_PAGE_FAULT:
    case EXC_INSN_ACCESS:
    case EXC_LOAD_ACCESS:
    case EXC_STORE_ACCESS: {
        const char *hint = vm_fault_hint(tf->stval, tf->scause);
        panic_print("  faulting address: %#lx\n", (unsigned long)tf->stval);
        if (hint)
            panic_print("  likely cause: %s\n", hint);
        vm_walk_print(NULL, tf->stval, panic_print);
        break;
    }
    default:
        panic_print("  stval: %#lx\n", (unsigned long)tf->stval);
        break;
    }
    trap_dump(tf, panic_print);
    panic_print("backtrace:\n");
    backtrace_print(panic_print, tf->sepc, TF_FP(tf));
    panic_context();
    panic_finish(tf);
}
