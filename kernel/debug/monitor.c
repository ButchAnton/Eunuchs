/*
 * Kernel debug monitor (kdb-style). Entered on panic, on `ebreak`
 * (debug_break()), with the `break` boot option, and at the end of boot.
 * Runs with interrupts off and polls the console, so it works even when the
 * rest of the kernel is wedged.
 *
 * SMP: entering stops all other CPUs (stop IPIs; a CPU spinning on a lock
 * parks from its spin loop) and leaving releases them. Only one CPU at a
 * time can own the stopped world: a second CPU entering the monitor (or
 * panicking) parks until the first one continues. `cpus` shows every CPU,
 * `cpu <n>` points regs/bt at where CPU n was stopped.
 */
#define LOG_SUBSYS SUBSYS_DEBUG
#include <console.h>
#include <debug.h>
#include <kstring.h>
#include <ktest.h>
#include <fs.h>
#include <irq.h>
#include <lockdep.h>
#include <net.h>
#include <profile.h>
#include <smp.h>
#include <virtio.h>
#include <mm.h>
#include <sched.h>
#include <trace.h>
#include <platform.h>
#include <proc.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <trap.h>

#define LINE_MAX  128
#define MAX_ARGV  8
#define HIST_SIZE 16

enum { MON_STAY = 0, MON_EXIT = 1 };

static struct trap_frame *mon_tf;        /* frame of the CPU being viewed */
static struct trap_frame *mon_self_tf;   /* frame of the CPU running the monitor */
static int mon_view = -1;                /* CPU being viewed (`cpu <n>`), -1 = this one */
static bool mon_can_continue;
static int mon_depth;

static char history[HIST_SIZE][LINE_MAX];
static int hist_count;

/* ---- line editing ---- */

static void erase_line(int len)
{
    while (len--)
        kprintf("\b \b");
}

static int readline(char *buf, int size)
{
    int len = 0;
    int hpos = hist_count;

    for (;;) {
        int c = console_getc();
        switch (c) {
        case '\r':
        case '\n':
            kprintf("\n");
            buf[len] = '\0';
            return len;
        case 0x7f:      /* DEL */
        case '\b':
            if (len) {
                len--;
                erase_line(1);
            }
            break;
        case 0x15:      /* ^U */
            erase_line(len);
            len = 0;
            break;
        case 0x03:      /* ^C */
            kprintf("^C\n");
            buf[0] = '\0';
            return 0;
        case 0x1b: {    /* ESC [ A / ESC [ B : history */
            if (console_getc() != '[')
                break;
            int dir = console_getc();
            if (dir != 'A' && dir != 'B')
                break;
            int idx = hpos + (dir == 'A' ? -1 : 1);
            int oldest = hist_count > HIST_SIZE ? hist_count - HIST_SIZE : 0;
            if (idx < oldest || idx > hist_count)
                break;
            hpos = idx;
            erase_line(len);
            if (idx == hist_count)
                buf[0] = '\0';
            else
                strlcpy(buf, history[idx % HIST_SIZE], size);
            len = strlen(buf);
            kprintf("%s", buf);
            break;
        }
        default:
            if (kisprint(c) && len < size - 1) {
                buf[len++] = (char)c;
                console_putc((char)c);
            }
            break;
        }
    }
}

static int tokenize(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < MAX_ARGV) {
        while (kisspace(*line))
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && !kisspace(*line))
            line++;
    }
    return argc;
}

/*
 * Address expressions: number (0x.., decimal), symbol[+off], or $reg from the
 * trap frame ($sepc, $stval, $sp, $a0, ...).
 */
static bool parse_addr(const char *s, uintptr_t *out)
{
    const char *end;

    if (s[0] == '$') {
        if (!mon_tf)
            return false;
        if (!strcmp(s + 1, "sepc") || !strcmp(s + 1, "pc")) { *out = mon_tf->sepc; return true; }
        if (!strcmp(s + 1, "stval")) { *out = mon_tf->stval; return true; }
        for (int i = 0; i < 32; i++)
            if (!strcmp(s + 1, reg_names[i])) {
                *out = mon_tf->regs[i];
                return true;
            }
        return false;
    }
    if (kisdigit(s[0])) {
        *out = kstrtoul(s, &end, 0);
        return *end == '\0';
    }

    char name[64];
    strlcpy(name, s, sizeof(name));
    uintptr_t off = 0;
    char *plus = strchr(name, '+');
    if (plus) {
        *plus = '\0';
        off = kstrtoul(plus + 1, &end, 0);
        if (*end)
            return false;
    }
    uintptr_t base = ksym_find(name);
    if (!base)
        return false;
    *out = base + off;
    return true;
}

/* ---- commands ---- */

static int cmd_help(int argc, char **argv);

static int cmd_regs(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!mon_tf && mon_view >= 0) {
        char sym[96];
        ksym_format(sym, sizeof(sym), cpus[mon_view].stop_pc);
        kprintf("cpu%d parked from a spin loop at %s; registers were not captured "
                "(use 'bt')\n", mon_view, sym);
        return MON_STAY;
    }
    if (!mon_tf) {
        kprintf("no trap frame (monitor not entered from a trap); see 'csr'\n");
        return MON_STAY;
    }
    trap_dump(mon_tf, kprintf);
    return MON_STAY;
}

static int cmd_csr(int argc, char **argv)
{
    (void)argc, (void)argv;
    kprintf("  sstatus  %016lx\n", csr_read(sstatus));
    kprintf("  sie      %016lx\n", csr_read(sie));
    kprintf("  sip      %016lx\n", csr_read(sip));
    kprintf("  stvec    %016lx\n", csr_read(stvec));
    kprintf("  sepc     %016lx\n", csr_read(sepc));
    kprintf("  scause   %016lx\n", csr_read(scause));
    kprintf("  stval    %016lx\n", csr_read(stval));
    kprintf("  satp     %016lx\n", csr_read(satp));
    kprintf("  sscratch %016lx\n", csr_read(sscratch));
    kprintf("  time     %016lx\n", (unsigned long)rdtime());
    return MON_STAY;
}

static int cmd_mem(int argc, char **argv)
{
    uintptr_t addr, len = 64;
    if (argc < 2 || !parse_addr(argv[1], &addr)) {
        kprintf("usage: mem <addr|sym|$reg> [len]   (physical memory: use p2v first)\n");
        return MON_STAY;
    }
    if (argc > 2)
        len = kstrtoul(argv[2], NULL, 0);
    hexdump_safe(kprintf, addr, MIN(len, (uintptr_t)4096));
    return MON_STAY;
}

static int cmd_meminfo(int argc, char **argv)
{
    (void)argc, (void)argv;
    pmm_print(kprintf);
    return MON_STAY;
}

static int cmd_vmstat(int argc, char **argv)
{
    (void)argc, (void)argv;
    vm_counters_print(kprintf);
    asid_print(kprintf);
    return MON_STAY;
}

static int cmd_swap(int argc, char **argv)
{
    (void)argc, (void)argv;
    swap_print(kprintf);
    return MON_STAY;
}

static int cmd_pagecache(int argc, char **argv)
{
    (void)argc, (void)argv;
    pc_print(kprintf);
    return MON_STAY;
}

static int cmd_pt(int argc, char **argv)
{
    uintptr_t va;
    if (argc < 2) {
        vm_dump(NULL, kprintf);
    } else if (parse_addr(argv[1], &va)) {
        vm_walk_print(NULL, va, kprintf);
        const char *hint = vm_fault_hint(va, EXC_LOAD_PAGE_FAULT);
        if (hint)
            kprintf("note: %s\n", hint);
    } else {
        kprintf("usage: pt [va]   (no argument: dump all mappings of the active table)\n");
    }
    return MON_STAY;
}

static int cmd_v2p(int argc, char **argv)
{
    uintptr_t va, pa;
    pte_t pte;
    char f[9];
    if (argc < 2 || !parse_addr(argv[1], &va)) {
        kprintf("usage: v2p <va>\n");
        return MON_STAY;
    }
    if (vm_translate(kernel_pagetable ? kernel_pagetable : NULL, va, &pa, &pte)) {
        kprintf("%#lx is not mapped\n", (unsigned long)va);
        return MON_STAY;
    }
    pte_flags_str(pte, f);
    kprintf("%#lx -> pa %#lx  [%s]\n", (unsigned long)va, (unsigned long)pa, f);
    return MON_STAY;
}

static int cmd_p2v(int argc, char **argv)
{
    uintptr_t pa;
    if (argc < 2 || !parse_addr(argv[1], &pa)) {
        kprintf("usage: p2v <pa>\n");
        return MON_STAY;
    }
    kprintf("pa %#lx -> direct map va %p\n", (unsigned long)pa, __va(pa));
    return MON_STAY;
}

static int cmd_page(int argc, char **argv)
{
    uintptr_t a;
    if (argc < 2 || !parse_addr(argv[1], &a)) {
        kprintf("usage: page <pa|va>\n");
        return MON_STAY;
    }
    page_print(kprintf, a >= KERNEL_BASE ? __pa(a) : a);
    return MON_STAY;
}

static int cmd_slab(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "leaks"))
        slab_leak_report(kprintf, argc > 2 ? argv[2] : NULL);
    else if (argc == 1)
        slab_print(kprintf);
    else
        kprintf("usage: slab [leaks [cache-prefix]]\n");
    return MON_STAY;
}

static int cmd_heapcheck(int argc, char **argv)
{
    (void)argc, (void)argv;
    bool saved = mm_panic_on_corruption;
    mm_panic_on_corruption = false;     /* report everything, don't stop at the first */
    int pages = pmm_verify_free();
    int objs = slab_verify();
    mm_panic_on_corruption = saved;
    kprintf("heap check: %d corrupted free page blocks, %d bad slab objects\n", pages, objs);
    return MON_STAY;
}

static int cmd_kmalloc(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: kmalloc <size>   (for experiments; free with kfree)\n");
        return MON_STAY;
    }
    void *p = kmalloc(kstrtoul(argv[1], NULL, 0));
    kprintf("%p\n", p);
    return MON_STAY;
}

static int cmd_kfree(int argc, char **argv)
{
    uintptr_t a;
    if (argc < 2 || !parse_addr(argv[1], &a)) {
        kprintf("usage: kfree <addr>\n");
        return MON_STAY;
    }
    kfree((void *)a);
    return MON_STAY;
}

static int cmd_poke(int argc, char **argv)
{
    uintptr_t addr;
    if (argc < 3 || !parse_addr(argv[1], &addr)) {
        kprintf("usage: poke <addr> <value> [size 1|2|4|8, default 8]\n");
        return MON_STAY;
    }
    u64 val = kstrtoul(argv[2], NULL, 0);
    unsigned size = argc > 3 ? kstrtoul(argv[3], NULL, 0) : 8;
    int r;
    switch (size) {
    case 1: r = safe_write_u8(addr, (u8)val); break;
    case 2: r = safe_write_u16(addr, (u16)val); break;
    case 4: r = safe_write_u32(addr, (u32)val); break;
    case 8: r = safe_write_u64(addr, val); break;
    default: kprintf("bad size\n"); return MON_STAY;
    }
    if (r)
        kprintf("write faulted\n");
    return MON_STAY;
}

static int cmd_bt(int argc, char **argv)
{
    if (argc > 1) {
        thread_backtrace(kprintf, (int)kstrtoul(argv[1], NULL, 0));
    } else if (mon_view >= 0) {
        cpu_backtrace(kprintf, mon_view);
    } else if (mon_tf) {
        backtrace_print(kprintf, mon_tf->sepc, TF_FP(mon_tf));
    } else {
        backtrace_here(kprintf);
    }
    return MON_STAY;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!sched_ready) {
        kprintf("scheduler not started yet\n");
        return MON_STAY;
    }
    thread_list_print(kprintf);
    kprintf("  states: R running, r runnable, S sleeping, D dead\n");
    return MON_STAY;
}

static int cmd_sched(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (sched_ready)
        sched_print(kprintf);
    kprintf("jiffies %lu (HZ=%d)\n", (unsigned long)jiffies, HZ);
    return MON_STAY;
}

static int cmd_irq(int argc, char **argv)
{
    (void)argc, (void)argv;
    irq_print(kprintf);
    return MON_STAY;
}

static int cmd_locks(int argc, char **argv)
{
    (void)argc, (void)argv;
    spin_print_held(kprintf);
    if (!sched_ready)
        return MON_STAY;
    struct thread *t;
    list_for_each_entry(t, &all_threads, all_node) {
        for (int i = 0; i < MIN(t->nheld_mutex, MAX_HELD_MUTEX); i++) {
            char s[96];
            ksym_format(s, sizeof(s), t->held_mutex[i]->owner_pc);
            kprintf("  thread %d (%s) holds mutex '%s' (locked at %s)\n", t->tid, t->name,
                    t->held_mutex[i]->name, s);
        }
    }
    list_for_each_entry(t, &all_threads, all_node) {
        if (t->blocked_on) {
            struct mutex *m = t->blocked_on;
            char s[96];
            ksym_format(s, sizeof(s), m->owner_pc);
            kprintf("  thread %d (%s) blocked on mutex '%s' held by %d (%s) since %s\n",
                    t->tid, t->name, m->name, m->owner ? m->owner->tid : -1,
                    m->owner ? m->owner->name : "-", s);
        }
    }
    return MON_STAY;
}

static int cmd_trace(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!*sub) {
        trace_status(kprintf);
    } else if (!strcmp(sub, "show")) {
        trace_print(kprintf, argc > 2 ? (int)kstrtoul(argv[2], NULL, 0) : 40);
    } else if (!strcmp(sub, "clear")) {
        trace_clear();
    } else if (!strcmp(sub, "groups") && argc > 2) {
        if (trace_set_groups(argv[2]))
            kprintf("groups: sched,irq,lock,mm,mark,syscall,proc,blk,fs,net,all,none\n");
        trace_status(kprintf);
    } else if (!strcmp(sub, "dump")) {
        const char *path = argc > 2 ? argv[2] : "build/trace.bin";
        int n = trace_dump_file(path);
        if (n < 0)
            kprintf("dump failed: QEMU semihosting not enabled? (make run enables it)\n");
        else
            kprintf("wrote %d bytes to %s on the host; convert with: make trace-json\n", n, path);
    } else {
        kprintf("usage: trace [show [n] | clear | groups <list> | dump [host-path]]\n");
    }
    return MON_STAY;
}

/* ---- processes ---- */

static struct proc *arg_proc(int argc, char **argv, const char *usage)
{
    struct proc *p = argc > 1 ? proc_find((int)kstrtoul(argv[1], NULL, 0)) : NULL;
    if (!p)
        kprintf("%s\n", argc > 1 ? "no such process" : usage);
    return p;
}

static int cmd_procs(int argc, char **argv)
{
    (void)argc, (void)argv;
    proc_list_print(kprintf);
    return MON_STAY;
}

static int cmd_vm(int argc, char **argv)
{
    struct proc *p = arg_proc(argc, argv, "usage: vm <pid> [va]");
    if (!p)
        return MON_STAY;
    if (!p->mm) {
        kprintf("pid %d has no address space\n", p->pid);
        return MON_STAY;
    }
    uintptr_t va;
    if (argc > 2 && parse_addr(argv[2], &va))
        vm_walk_print(p->mm->pt, va, kprintf);
    else
        mm_print(kprintf, p->mm);
    return MON_STAY;
}

static int cmd_ubt(int argc, char **argv)
{
    struct proc *p = arg_proc(argc, argv, "usage: ubt <pid>   (user-space backtrace)");
    if (!p || !p->thread)
        return MON_STAY;
    struct trap_frame *tf = thread_user_tf(p->thread);
    kprintf("pid %d (%s) user pc %#lx sp %#lx\n", p->pid, p->name, (unsigned long)tf->sepc,
            (unsigned long)TF_SP(tf));
    proc_user_backtrace(kprintf, p, tf->sepc, TF_FP(tf));
    return MON_STAY;
}

static int cmd_strace(int argc, char **argv)
{
    struct proc *p = arg_proc(argc, argv, "usage: strace <pid> [on|off|follow]");
    if (!p)
        return MON_STAY;
    const char *mode = argc > 2 ? argv[2] : "on";
    p->strace = !strcmp(mode, "off") ? 0 : !strcmp(mode, "follow") ? 2 : 1;
    kprintf("strace for pid %d: %s\n", p->pid, mode);
    return MON_STAY;
}

static int cmd_files(int argc, char **argv)
{
    struct proc *p = arg_proc(argc, argv, "usage: files <pid>");
    if (p)
        files_print(kprintf, p);
    return MON_STAY;
}

static int cmd_mounts(int argc, char **argv)
{
    (void)argc, (void)argv;
    vfs_mounts_print(kprintf);
    return MON_STAY;
}

static int cmd_bcache(int argc, char **argv)
{
    (void)argc, (void)argv;
    bcache_print(kprintf);
    return MON_STAY;
}

static int cmd_icache(int argc, char **argv)
{
    (void)argc, (void)argv;
    icache_print(kprintf);
    return MON_STAY;
}

static int cmd_tty(int argc, char **argv)
{
    (void)argc, (void)argv;
    tty_print(kprintf);
    return MON_STAY;
}

static int cmd_pipes(int argc, char **argv)
{
    (void)argc, (void)argv;
    pipes_print(kprintf);
    return MON_STAY;
}

static int cmd_blk(int argc, char **argv)
{
    (void)argc, (void)argv;
    blk_print(kprintf);
    p9_print(kprintf);
    virtio_print(kprintf);
    return MON_STAY;
}

static int cmd_kill(int argc, char **argv)
{
    struct proc *p = arg_proc(argc, argv, "usage: kill <pid>");
    if (p) {
        /* Don't wait for proc_lock: its holder may be parked. */
        unsigned long flags;
        bool locked = spin_lock_debug(&proc_lock, &flags);
        int r = signal_send_locked(p, SIGKILL, 0);
        spin_unlock_debug(&proc_lock, locked, flags);
        kprintf("%s\n", r ? "failed" : "SIGKILL sent (takes effect when it runs)");
    }
    return MON_STAY;
}

/* ---- demos: spawn threads to watch with ps / trace ---- */

static struct semaphore ping_sem, pong_sem;

static void pinger(void *arg)
{
    long rounds = (long)arg;
    for (long i = 0; i < rounds; i++) {
        sem_down(&ping_sem);
        trace_mark("ping", i, 0);
        sleep_ms(5);
        sem_up(&pong_sem);
    }
}

static void ponger(void *arg)
{
    long rounds = (long)arg;
    for (long i = 0; i < rounds; i++) {
        sem_down(&pong_sem);
        trace_mark("pong", i, 0);
        sleep_ms(5);
        sem_up(&ping_sem);
    }
}

static void spinner(void *arg)
{
    u64 end = rdtime() + (u64)(uintptr_t)arg * platform.timebase / 1000;
    while (rdtime() < end)
        ;                       /* never yields: only preemption lets others run */
}

static struct mutex demo_a = MUTEX_INIT(demo_a, "demo-A");
static struct mutex demo_b = MUTEX_INIT(demo_b, "demo-B");

static void locker(void *arg)
{
    bool ab = arg != NULL;
    mutex_lock(ab ? &demo_a : &demo_b);
    sleep_ms(20);
    mutex_lock(ab ? &demo_b : &demo_a);     /* the other thread holds this one */
    mutex_unlock(ab ? &demo_b : &demo_a);
    mutex_unlock(ab ? &demo_a : &demo_b);
}

static void spawn(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = kthread_create(name, fn, arg);
    if (t) {
        kthread_detach(t);
        kprintf("spawned thread %d (%s)\n", t->tid, name);
    }
}

static int cmd_demo(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (!sched_ready) {
        kprintf("scheduler not started yet\n");
    } else if (!strcmp(what, "pingpong")) {
        long rounds = argc > 2 ? (long)kstrtoul(argv[2], NULL, 0) : 20;
        sem_init(&ping_sem, 1, "ping");
        sem_init(&pong_sem, 0, "pong");
        spawn("ping", pinger, (void *)rounds);
        spawn("pong", ponger, (void *)rounds);
    } else if (!strcmp(what, "spin")) {
        spawn("spin", spinner, (void *)(uintptr_t)(argc > 2 ? kstrtoul(argv[2], NULL, 0) : 2000));
    } else if (!strcmp(what, "deadlock")) {
        spawn("lock-AB", locker, (void *)1);
        spawn("lock-BA", locker, NULL);
    } else {
        kprintf("usage: demo pingpong [rounds] | spin [ms] | deadlock\n");
        return MON_STAY;
    }
    kprintf("type 'c' to let them run; Ctrl-] to come back, then 'ps' / 'trace show'\n");
    return MON_STAY;
}

static volatile int recurse_limit = 1 << 30;     /* volatile: GCC can't prove it's infinite */

static int __noinline recurse(int depth)
{
    volatile char pad[256];
    pad[0] = (char)depth;
    if (depth >= recurse_limit)
        return 0;
    return recurse(depth + 1) + pad[0];
}

static int cmd_sym(int argc, char **argv)
{
    uintptr_t addr;
    char buf[96];
    if (argc < 2 || !parse_addr(argv[1], &addr)) {
        kprintf("usage: sym <addr|name>\n");
        return MON_STAY;
    }
    ksym_format(buf, sizeof(buf), addr);
    kprintf("%#lx = %s\n", (unsigned long)addr, buf);
    return MON_STAY;
}

static int cmd_dmesg(int argc, char **argv)
{
    dmesg_print(argc > 1 ? (int)kstrtoul(argv[1], NULL, 0) : 0);
    return MON_STAY;
}

static int cmd_log(int argc, char **argv)
{
    if (argc == 3) {
        int level = log_level_lookup(argv[2]);
        int s = log_subsys_lookup(argv[1]);
        if (level < 0 || (s < 0 && strcmp(argv[1], "all") && strcmp(argv[1], "console"))) {
            kprintf("unknown subsystem or level\n");
            return MON_STAY;
        }
        if (!strcmp(argv[1], "console"))
            console_level = level;
        else if (s < 0)
            for (int i = 0; i < SUBSYS_MAX; i++)
                log_threshold[i] = level;
        else
            log_threshold[s] = level;
    } else if (argc != 1) {
        kprintf("usage: log [<subsys|all|console> <err|warn|info|debug|trace>]\n");
        return MON_STAY;
    }
    for (int i = 0; i < SUBSYS_MAX; i++)
        kprintf("  %-8s %s\n", log_subsys_names[i], log_level_names[log_threshold[i]]);
    kprintf("  %-8s %s\n", "console", log_level_names[console_level]);
    return MON_STAY;
}

static int cmd_dtb(int argc, char **argv)
{
    (void)argc, (void)argv;
    fdt_dump(kprintf);
    return MON_STAY;
}

static int cmd_plat(int argc, char **argv)
{
    (void)argc, (void)argv;
    platform_print(kprintf);
    kprintf("  cmdline   \"%s\"\n", cmdline_raw());
    kprintf("  kernel    %#lx-%#lx, ksyms %lu\n", (unsigned long)__kernel_start,
            (unsigned long)__kernel_end, (unsigned long)ksym_count());
    return MON_STAY;
}

static int cmd_traps(int argc, char **argv)
{
    (void)argc, (void)argv;
    trap_print_counts(kprintf);
    return MON_STAY;
}

static int cmd_ktest(int argc, char **argv)
{
    ktest_run(argc > 1 ? argv[1] : NULL);
    return MON_STAY;
}

static int cmd_crash(int argc, char **argv)
{
    const char *kind = argc > 1 ? argv[1] : "";
    if (!strcmp(kind, "null"))
        kprintf("read %d\n", *(volatile int *)8);
    else if (!strcmp(kind, "store"))
        *(volatile int *)(uintptr_t)0x1000000 = 1;
    else if (!strcmp(kind, "illegal"))
        asm volatile(".word 0");
    else if (!strcmp(kind, "ebreak"))
        debug_break();
    else if (!strcmp(kind, "stack"))
        kprintf("%d\n", recurse(0));                       /* runs off the stack */
    else if (!strcmp(kind, "wx"))
        *(volatile u8 *)(uintptr_t)cmd_crash = 0;           /* write to kernel text */
    else if (!strcmp(kind, "uaf")) {
        struct node { struct node *next; long val; };
        struct node *n = kmalloc(sizeof(*n));
        n->next = n;
        kfree(n);
        kprintf("read %ld\n", n->next->val);                 /* n->next is now 0x6b6b... */
    } else if (!strcmp(kind, "overflow")) {
        char *buf = kmalloc(16);
        buf[16] = '!';                                      /* one byte too far */
        kfree(buf);
    } else if (!strcmp(kind, "panic"))
        panic("requested from monitor");
    else if (!strcmp(kind, "assert"))
        assert(1 + 1 == 3);
    else
        kprintf("usage: crash null|store|wx|uaf|overflow|stack|illegal|ebreak|panic|assert\n");
    return MON_STAY;
}

static int cmd_cpus(int argc, char **argv)
{
    (void)argc, (void)argv;
    cpus_print(kprintf);
    return MON_STAY;
}

static int cmd_cpu(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("viewing cpu%d (monitor runs on cpu%d); usage: cpu <n>\n",
                mon_view >= 0 ? mon_view : this_cpu()->id, this_cpu()->id);
        return MON_STAY;
    }
    int n = (int)kstrtoul(argv[1], NULL, 0);
    if (n < 0 || n >= ncpus_online) {
        kprintf("no cpu%d (%d online)\n", n, ncpus_online);
        return MON_STAY;
    }
    if (n == this_cpu()->id) {
        mon_view = -1;
        mon_tf = mon_self_tf;
    } else if (!cpus[n].stopped) {
        kprintf("cpu%d did not stop (interrupts off and not spinning?): nothing to show\n", n);
        return MON_STAY;
    } else {
        mon_view = n;
        mon_tf = cpus[n].stop_tf;
    }
    struct cpu *c = &cpus[n];
    char sym[96] = "";
    if (mon_view >= 0)
        ksym_format(sym, sizeof(sym), c->stop_pc);
    kprintf("now viewing cpu%d (hart %lu): thread %d (%s)%s%s%s; regs, bt and $reg refer to it\n",
            n, c->hartid, c->cur_thread ? c->cur_thread->tid : -1,
            c->cur_thread ? c->cur_thread->name : "?", sym[0] ? ", stopped at " : "", sym,
            mon_view >= 0 && !mon_tf ? " (in a spin loop)" : "");
    return MON_STAY;
}

static int cmd_lockdep(int argc, char **argv)
{
    (void)argc, (void)argv;
    lockdep_print(kprintf);
    return MON_STAY;
}

static int cmd_profile(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "report";
    if (!strcmp(sub, "start")) {
        profile_start(argc > 2 ? (u32)kstrtoul(argv[2], NULL, 0) : 0);
        kprintf("sampling resumes when you continue ('c')\n");
    } else if (!strcmp(sub, "stop")) {
        profile_stop();
    } else if (!strcmp(sub, "clear")) {
        profile_clear();
    } else if (!strcmp(sub, "report")) {
        profile_report(kprintf);
    } else if (!strcmp(sub, "dump")) {
        const char *path = argc > 2 ? argv[2] : "build/profile.bin";
        int n = profile_dump_file(path);
        if (n < 0)
            kprintf("dump failed: QEMU semihosting not enabled? (make run enables it)\n");
        else
            kprintf("wrote %d bytes to %s on the host; then: make flamegraph\n", n, path);
    } else {
        kprintf("usage: profile [start [hz] | stop | report | dump [host-path] | clear]\n");
    }
    return MON_STAY;
}

/* Networking state, read without net_lock (its holder may be parked). */
static int cmd_net(int argc, char **argv)
{
    (void)argc, (void)argv;
    net_print_dev(kprintf);
    kprintf("ARP:\n");
    arp_print(kprintf);
    net_print_stats(kprintf);
    if (net_lock.owner)
        kprintf("(net_lock is held by thread %d (%s): figures may be mid-update)\n",
                net_lock.owner->tid, net_lock.owner->name);
    return MON_STAY;
}

static int cmd_netstat(int argc, char **argv)
{
    (void)argc, (void)argv;
    kprintf("TCP:\n");
    tcp_print(kprintf);
    kprintf("UDP and raw:\n");
    udp_print(kprintf);
    return MON_STAY;
}

static int cmd_continue(int argc, char **argv)
{
    (void)argc, (void)argv;
    if (!mon_can_continue) {
        kprintf("cannot continue from here (use 'reboot' or 'halt')\n");
        return MON_STAY;
    }
    return MON_EXIT;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc, (void)argv;
    platform_reboot();
}

static int cmd_halt(int argc, char **argv)
{
    platform_exit(argc > 1 ? (int)kstrtoul(argv[1], NULL, 0) : 0);
}

struct mon_cmd {
    const char *name;
    const char *args;
    const char *help;
    int (*fn)(int argc, char **argv);
};

static const struct mon_cmd commands[] = {
    { "help",     "",                    "this list", cmd_help },
    { "regs",     "",                    "registers from the trap frame", cmd_regs },
    { "csr",      "",                    "supervisor CSRs", cmd_csr },
    { "bt",       "[tid]",               "backtrace (trap frame, current, or a thread)", cmd_bt },
    { "ps",       "",                    "threads: state, CPU time, stack use, wait reason", cmd_ps },
    { "sched",    "",                    "run queue, sleep queue, CPU stats", cmd_sched },
    { "cpus",     "",                    "all CPUs: state, thread, where stopped, IPIs", cmd_cpus },
    { "cpu",      "<n>",                 "view CPU n (regs/bt show where it stopped)", cmd_cpu },
    { "irq",      "",                    "interrupt counts and handlers", cmd_irq },
    { "procs",    "",                    "user processes", cmd_procs },
    { "vm",       "<pid> [va]",          "process memory map, or walk its page table", cmd_vm },
    { "ubt",      "<pid>",               "user-space backtrace (symbolized)", cmd_ubt },
    { "strace",   "<pid> [on|off|follow]", "trace a process's system calls", cmd_strace },
    { "kill",     "<pid>",               "kill a process", cmd_kill },
    { "files",    "<pid>",               "open files and cwd of a process", cmd_files },
    { "mounts",   "",                    "mounted filesystems, space, log statistics", cmd_mounts },
    { "bcache",   "",                    "buffer cache stats and held buffers", cmd_bcache },
    { "icache",   "",                    "in-memory inodes, references, lock holders", cmd_icache },
    { "tty",      "",                    "terminal: termios, foreground group, pending input", cmd_tty },
    { "pipes",    "",                    "pipes: buffered bytes, open ends", cmd_pipes },
    { "blk",      "",                    "block device statistics, virtio devices", cmd_blk },
    { "net",      "",                    "interfaces, ARP table, protocol statistics", cmd_net },
    { "profile",  "[start|stop|report|dump]", "sampling profiler (make flamegraph after dump)", cmd_profile },
    { "netstat",  "",                    "TCP connections (windows, RTO, retransmits), UDP sockets", cmd_netstat },
    { "locks",    "",                    "held spinlocks and mutexes, blocked threads", cmd_locks },
    { "lockdep",  "",                    "lock classes and the order dependencies seen", cmd_lockdep },
    { "trace",    "[show|clear|groups|dump]", "event trace (flight recorder)", cmd_trace },
    { "demo",     "<what>",              "spawn demo threads (pingpong|spin|deadlock)", cmd_demo },
    { "mem",      "<addr> [len]",        "hex dump (addr: number, sym[+off], $reg)", cmd_mem },
    { "poke",     "<addr> <val> [size]", "write memory", cmd_poke },
    { "sym",      "<addr|name>",         "symbol lookup", cmd_sym },
    { "meminfo",  "",                    "physical memory summary", cmd_meminfo },
    { "vmstat",   "",                    "paging counters: faults, cache, swap, reclaim, ASIDs", cmd_vmstat },
    { "swap",     "",                    "swap space: slots used, pages in and out", cmd_swap },
    { "pagecache", "",                   "cached file pages by file, dirty pages", cmd_pagecache },
    { "pt",       "[va]",                "page table dump, or walk one address", cmd_pt },
    { "v2p",      "<va>",                "translate virtual -> physical", cmd_v2p },
    { "p2v",      "<pa>",                "physical -> direct-map virtual", cmd_p2v },
    { "page",     "<pa|va>",             "struct page details for a frame", cmd_page },
    { "slab",     "[leaks [prefix]]",    "slab caches / live allocations by site", cmd_slab },
    { "heapcheck", "",                   "verify all free pages and slab objects", cmd_heapcheck },
    { "kmalloc",  "<size>",              "allocate (experiments)", cmd_kmalloc },
    { "kfree",    "<addr>",              "free (experiments)", cmd_kfree },
    { "dmesg",    "[lines]",             "kernel log", cmd_dmesg },
    { "log",      "[subsys level]",      "show/set log levels", cmd_log },
    { "plat",     "",                    "platform summary", cmd_plat },
    { "dtb",      "",                    "dump device tree", cmd_dtb },
    { "traps",    "",                    "exception/interrupt counters", cmd_traps },
    { "ktest",    "[prefix]",            "run kernel unit tests", cmd_ktest },
    { "crash",    "<kind>",              "trigger a failure (null|store|wx|uaf|overflow|stack|...)", cmd_crash },
    { "c",        "",                    "continue execution", cmd_continue },
    { "reboot",   "",                    "reboot the machine", cmd_reboot },
    { "halt",     "[code]",              "power off (QEMU exits with code)", cmd_halt },
};

static int cmd_help(int argc, char **argv)
{
    (void)argc, (void)argv;
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++)
        kprintf("  %-9s %-26s %s\n", commands[i].name, commands[i].args, commands[i].help);
    kprintf("  keys: up/down history, ^U clear line, ^C cancel\n");
    return MON_STAY;
}

static int dispatch(int argc, char **argv)
{
    if (!strcmp(argv[0], "continue"))
        argv[0] = "c";
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++)
        if (!strcmp(argv[0], commands[i].name))
            return commands[i].fn(argc, argv);
    kprintf("unknown command '%s' (try 'help')\n", argv[0]);
    return MON_STAY;
}

void monitor_enter(const char *reason, struct trap_frame *tf, bool can_continue)
{
    bool irqs = intr_enabled();
    char line[LINE_MAX];
    char *argv[MAX_ARGV];

    intr_off();
    /* Own the stopped world (panic may already); wait if another CPU does. */
    bool took_stop = false;
    u32 missing = 0;
    if (!smp_stopped_by_me()) {
        while (!smp_stop_others(&missing))
            smp_park_self(tf);
        took_stop = true;
    }

    struct trap_frame *saved_tf = mon_tf, *saved_self = mon_self_tf;
    int saved_view = mon_view;
    bool saved_cc = mon_can_continue;
    mon_tf = mon_self_tf = tf;
    mon_view = -1;
    mon_can_continue = can_continue;
    mon_depth++;
    u64 entered = rdtime();
    if (mon_depth == 1)
        monitor_entry_time = entered;

    pr_info("entering monitor: %s\n", reason);
    kprintf("\n=== Eunuchs debug monitor: %s ===\n", reason);
    if (tf) {
        char sym[96];
        ksym_format(sym, sizeof(sym), tf->sepc);
        kprintf("stopped at %#lx <%s>\n", (unsigned long)tf->sepc, sym);
    }
    if (ncpus_online > 1) {
        kprintf("on cpu%d; other CPUs stopped", this_cpu()->id);
        for (int i = 0; i < ncpus_online; i++)
            if (missing & (1u << i))
                kprintf(" (cpu%d did NOT stop: interrupts off?)", i);
        kprintf(" ('cpus', 'cpu <n>')\n");
    }
    kprintf("type 'help' for commands%s\n", can_continue ? ", 'c' to continue" : "");

    for (;;) {
        if (mon_view >= 0)
            kprintf("mon(cpu%d)> ", mon_view);
        else if (mon_depth > 1)
            kprintf("mon[%d]> ", mon_depth);
        else
            kprintf("mon> ");
        if (readline(line, sizeof(line)) == 0)
            continue;
        if (hist_count == 0 || strcmp(history[(hist_count - 1) % HIST_SIZE], line))
            strlcpy(history[hist_count++ % HIST_SIZE], line, LINE_MAX);
        int argc = tokenize(line, argv);
        if (argc && dispatch(argc, argv) == MON_EXIT)
            break;
    }

    mon_depth--;
    if (mon_depth == 0) {
        /* Don't charge the stopped time to whoever was running. */
        if (sched_ready)
            this_cpu()->last_switch += rdtime() - entered;
        monitor_entry_time = 0;
    }
    mon_tf = saved_tf;
    mon_self_tf = saved_self;
    mon_view = saved_view;
    mon_can_continue = saved_cc;
    pr_info("leaving monitor\n");
    if (took_stop)
        smp_resume_others();
    if (irqs)
        intr_on();
}
