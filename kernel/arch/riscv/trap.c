#define LOG_SUBSYS SUBSYS_TRAP
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <percpu.h>
#include <platform.h>
#include <printk.h>
#include <proc.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>
#include <uapi/errno.h>
#include <riscv.h>
#include <safe.h>
#include <trap.h>

extern void kernel_trap_entry(void);
extern const struct extable_entry __ex_table_start[], __ex_table_end[];

const char *const reg_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *const exc_names[EXC_MAX] = {
    [EXC_INSN_MISALIGNED]  = "instruction address misaligned",
    [EXC_INSN_ACCESS]      = "instruction access fault",
    [EXC_ILLEGAL_INSN]     = "illegal instruction",
    [EXC_BREAKPOINT]       = "breakpoint",
    [EXC_LOAD_MISALIGNED]  = "load address misaligned",
    [EXC_LOAD_ACCESS]      = "load access fault",
    [EXC_STORE_MISALIGNED] = "store/AMO address misaligned",
    [EXC_STORE_ACCESS]     = "store/AMO access fault",
    [EXC_ECALL_U]          = "environment call from U-mode",
    [EXC_ECALL_S]          = "environment call from S-mode",
    [EXC_INSN_PAGE_FAULT]  = "instruction page fault",
    [EXC_LOAD_PAGE_FAULT]  = "load page fault",
    [EXC_STORE_PAGE_FAULT] = "store/AMO page fault",
    [EXC_SW_CHECK]         = "software check",
    [EXC_HW_ERROR]         = "hardware error",
};

static const char *const irq_names[IRQ_MAX] = {
    [IRQ_S_SOFT]  = "supervisor software interrupt",
    [IRQ_S_TIMER] = "supervisor timer interrupt",
    [IRQ_S_EXT]   = "supervisor external interrupt",
    [IRQ_COUNTER] = "counter overflow interrupt",
};

u64 exception_counts[EXC_MAX];     /* all CPUs (atomic); interrupts are per CPU */

bool (*trap_breakpoint_hook)(struct trap_frame *tf);

void trap_init(void)
{
    csr_write(stvec, (uintptr_t)kernel_trap_entry);   /* direct mode */
}

const char *trap_cause_name(u64 scause)
{
    u64 code = SCAUSE_CODE(scause);
    const char *name = NULL;
    if (scause & SCAUSE_INTR) {
        if (code < IRQ_MAX)
            name = irq_names[code];
    } else if (code < EXC_MAX) {
        name = exc_names[code];
    }
    return name ? name : "unknown/reserved";
}

uintptr_t extable_lookup(uintptr_t pc)
{
    for (const struct extable_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (e->insn == pc)
            return e->fixup;
    return 0;
}

static bool is_memory_fault(u64 code)
{
    switch (code) {
    case EXC_LOAD_MISALIGNED:
    case EXC_LOAD_ACCESS:
    case EXC_STORE_MISALIGNED:
    case EXC_STORE_ACCESS:
    case EXC_LOAD_PAGE_FAULT:
    case EXC_STORE_PAGE_FAULT:
        return true;
    default:
        return false;
    }
}

/* Length of the instruction at pc: 2 for compressed (low bits != 11), else 4. */
static unsigned insn_length(uintptr_t pc)
{
    u16 lo;
    if (safe_read_u16(pc, &lo))
        return 4;
    return (lo & 3) == 3 ? 4 : 2;
}

/*
 * The `ebreak` of a semihosting call (see semihost.c) traps to us when QEMU
 * runs without -semihosting-config. Recognize it and fail the call.
 */
static bool is_semihost_ebreak(uintptr_t pc)
{
    u32 before, after;
    return !safe_read_u32(pc - 4, &before) && !safe_read_u32(pc + 4, &after) &&
           before == 0x01f01013 && after == 0x40705013;
}

static __noreturn void stack_overflow(struct trap_frame *tf)
{
    struct cpu *c = this_cpu();
    uintptr_t sp = c->overflow_sp;

    if (c->in_overflow) {
        kprintf("\n*** kernel stack overflow while handling a stack overflow; halting\n");
        platform_exit(4);
    }
    c->in_overflow = true;
    tf->regs[2] = sp;                   /* the real interrupted sp */
    struct thread *t = c->cur_thread;
    panic_print("KERNEL STACK OVERFLOW: sp=%#lx in thread %d (%s), stack %p-%p\n",
                (unsigned long)sp, t ? t->tid : -1, t ? t->name : "?",
                t ? t->kstack : NULL, t ? (void *)((u8 *)t->kstack + t->kstack_size) : NULL);
    panic_print("  (deep recursion or a large local array; now running on the emergency stack)\n");
    panic_trap(tf);
}

static void handle_interrupt(struct trap_frame *tf, u64 code)
{
    struct cpu *c = this_cpu();
    struct trap_frame *outer = c->irq_tf;

    if (code < IRQ_MAX)
        c->irq_count[code]++;
    c->irq_depth++;
    c->irq_tf = tf;
    c->last_irq = 0;
    trace(irq_entry, code, 0, 0);

    switch (code) {
    case IRQ_S_TIMER:
        timer_interrupt();
        break;
    case IRQ_S_EXT:
        plic_handle();
        break;
    case IRQ_S_SOFT:
        ipi_handle(tf);
        break;
    default:
        csr_clear(sie, 1UL << code);
        pr_warn("unexpected %s (scause=%#lx); masked\n",
                trap_cause_name(tf->scause), (unsigned long)tf->scause);
        break;
    }

    trace(irq_exit, code, c->last_irq, 0);
    c->irq_tf = outer;
    c->irq_depth--;

    /*
     * Preempt on return if the tick asked for it and the interrupted code
     * had interrupts enabled (so it holds no spinlocks).
     */
    if (c->need_resched && c->irq_depth == 0 && sched_ready && (tf->sstatus & SSTATUS_SPIE))
        preempt_schedule();
}

/* Exceptions raised by user code. */
static void user_exception(struct trap_frame *tf, u64 code)
{
    struct proc *p = current_proc();
    if (!p)
        panic("user-mode trap but the current thread has no process");

    switch (code) {
    case EXC_ECALL_U:
        syscall_dispatch(tf);
        break;
    case EXC_INSN_PAGE_FAULT:
    case EXC_LOAD_PAGE_FAULT:
    case EXC_STORE_PAGE_FAULT: {
        int access = code == EXC_INSN_PAGE_FAULT ? FAULT_EXEC
                   : code == EXC_LOAD_PAGE_FAULT ? FAULT_READ : FAULT_WRITE;
        intr_on();
        int r = uvm_fault(p->mm, tf->stval, access);
        intr_off();
        trace(user_fault, tf->stval, access, r);
        if (r)
            user_fault(tf, r == -ENOMEM ? "out of memory" : r == -EACCES ? "permission denied"
                                                                       : "no mapping",
                       r == -ENOMEM ? SIGBUS : SIGSEGV);
        break;
    }
    case EXC_BREAKPOINT: {
        u16 lo = 3;
        copy_from_user(&lo, tf->sepc, sizeof(lo));
        char reason[64];
        ksnprintf(reason, sizeof(reason), "user breakpoint in pid %d (%s)", p->pid, p->name);
        monitor_enter(reason, tf, true);
        tf->sepc += (lo & 3) == 3 ? 4 : 2;
        break;
    }
    case EXC_ILLEGAL_INSN:
        user_fault(tf, (tf->sstatus & (3UL << 13)) == 0 && (tf->stval & 0x7f) == 0x07
                       ? "floating point is not supported in user mode yet"
                       : "illegal instruction", SIGILL);
        break;
    case EXC_INSN_MISALIGNED:
    case EXC_LOAD_MISALIGNED:
    case EXC_STORE_MISALIGNED:
        user_fault(tf, "misaligned access", SIGBUS);
        break;
    default:
        user_fault(tf, "bad access", SIGSEGV);
        break;
    }
}

/* Last stop before sret to user mode: reschedule, deliver signals. */
static void return_to_user_checks(struct trap_frame *tf)
{
    if (this_cpu()->need_resched)
        yield();
    if (signal_pending(current_proc())) {
        intr_on();
        signal_deliver(tf);             /* may stop or exit */
    }
    syscall_restart_check(tf);
    intr_off();
}

void trap_handler(struct trap_frame *tf)
{
    u64 code = SCAUSE_CODE(tf->scause);
    bool from_user = !(tf->sstatus & SSTATUS_SPP);

    if (unlikely(this_cpu()->overflow_sp))
        stack_overflow(tf);

    if (tf->scause & SCAUSE_INTR) {
        handle_interrupt(tf, code);
        if (from_user)
            return_to_user_checks(tf);
        return;
    }

    if (code < EXC_MAX)
        __atomic_add_fetch(&exception_counts[code], 1, __ATOMIC_RELAXED);

    if (from_user) {
        user_exception(tf, code);
        return_to_user_checks(tf);
        return;
    }

    /*
     * A kernel address whose top-level entry another CPU added after this
     * page table copied the kernel half (vm_activate syncs on the next
     * switch; this catches accesses before that).
     */
    if ((code == EXC_LOAD_PAGE_FAULT || code == EXC_STORE_PAGE_FAULT ||
         code == EXC_INSN_PAGE_FAULT) && tf->stval >= KERNEL_BASE && vm_kernel_fault_fixup(tf->stval))
        return;

    if (is_memory_fault(code)) {
        /*
         * A user access by copy_{from,to}_user may need demand paging or
         * COW: resolve it and retry. Only the registered uaccess
         * instructions qualify; any other kernel touch of user memory falls
         * through to a panic with an explanation.
         */
        struct proc *p = current_proc();
        if (p && p->mm && tf->stval < USER_TOP && extable_lookup(tf->sepc) &&
            (code == EXC_LOAD_PAGE_FAULT || code == EXC_STORE_PAGE_FAULT) &&
            uvm_fault(p->mm, tf->stval,
                      code == EXC_LOAD_PAGE_FAULT ? FAULT_READ : FAULT_WRITE) == 0)
            return;
        uintptr_t fixup = extable_lookup(tf->sepc);
        if (fixup) {
            pr_trace("fault at %#lx (addr %#lx) fixed up -> %#lx\n",
                     (unsigned long)tf->sepc, (unsigned long)tf->stval, (unsigned long)fixup);
            tf->sepc = fixup;
            return;
        }
    }

    if (code == EXC_BREAKPOINT) {
        if (is_semihost_ebreak(tf->sepc)) {
            tf->regs[10] = (u64)-1;     /* a0: semihosting unavailable */
            tf->sepc += 4;
            return;
        }
        unsigned len = insn_length(tf->sepc);
        if (!(trap_breakpoint_hook && trap_breakpoint_hook(tf)))
            monitor_enter("breakpoint (ebreak)", tf, true);
        tf->sepc += len;
        return;
    }

    panic_trap(tf);
}

void trap_dump(const struct trap_frame *tf, print_fn print)
{
    char sym[96];

    ksym_format(sym, sizeof(sym), tf->sepc);
    print("  sepc    %016lx  %s\n", (unsigned long)tf->sepc, sym);
    ksym_format(sym, sizeof(sym), TF_RA(tf));
    print("  ra      %016lx  %s\n", (unsigned long)TF_RA(tf), sym);
    print("  scause  %016lx  %s\n", (unsigned long)tf->scause, trap_cause_name(tf->scause));
    print("  stval   %016lx\n", (unsigned long)tf->stval);
    print("  sstatus %016lx  (SPP=%c SPIE=%d SIE=%d SUM=%d)\n",
          (unsigned long)tf->sstatus,
          (tf->sstatus & SSTATUS_SPP) ? 'S' : 'U',
          !!(tf->sstatus & SSTATUS_SPIE), !!(tf->sstatus & SSTATUS_SIE),
          !!(tf->sstatus & SSTATUS_SUM));
    for (int i = 0; i < 32; i += 4) {
        print("  %-4s %016lx  %-4s %016lx  %-4s %016lx  %-4s %016lx\n",
              reg_names[i], (unsigned long)tf->regs[i],
              reg_names[i + 1], (unsigned long)tf->regs[i + 1],
              reg_names[i + 2], (unsigned long)tf->regs[i + 2],
              reg_names[i + 3], (unsigned long)tf->regs[i + 3]);
    }
}

void trap_print_counts(print_fn print)
{
    print("exceptions:\n");
    for (int i = 0; i < EXC_MAX; i++)
        if (exception_counts[i])
            print("  %2d %-32s %lu\n", i, exc_names[i] ? exc_names[i] : "?",
                  (unsigned long)exception_counts[i]);
    print("interrupts:        ");
    for (int c = 0; c < ncpus_online; c++)
        print("       cpu%d", c);
    print("\n");
    for (int i = 0; i < IRQ_MAX; i++) {
        u64 sum = 0;
        for (int c = 0; c < ncpus_online; c++)
            sum += cpus[c].irq_count[i];
        if (!sum)
            continue;
        const char *name = irq_names[i] ? irq_names[i] : "?";
        if (!strncmp(name, "supervisor ", 11))
            name += 11;
        print("  %2d %-15.15s", i, name);
        for (int c = 0; c < ncpus_online; c++)
            print(" %10lu", (unsigned long)cpus[c].irq_count[i]);
        print("\n");
    }
}
