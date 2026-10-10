/*
 * Sampling profiler.
 *
 * While profiling, every CPU's timer fires PROFILE_HZ times a second (the
 * scheduler still ticks at HZ; see timer.c) and each interrupt records one
 * sample: which CPU, thread and process, whether it was idle, and the whole
 * call stack: the kernel frames (from the interrupted kernel code, walked
 * with frame pointers) followed by the user frames of the process underneath
 * (for code in user mode, or under a system call). Stacks are read with
 * fault-free accessors only (safe_read, page-table lookups), so sampling can
 * never take a fault in interrupt context.
 *
 * Samples go into per-CPU byte buffers (variable-length records, no
 * locking: each CPU writes only its own, with interrupts off). When a
 * buffer is full, further samples on that CPU are counted as dropped.
 *
 * Results:
 *   /proc/profile, monitor `profile`   top functions and processes, in-kernel
 *   profile dump / `dump` [host path]  binary file via semihosting, then
 *   make flamegraph                    tools/profile/euprof.py: report,
 *                                      folded stacks, SVG flamegraph with
 *                                      kernel and user symbols
 *   eu-profile-dump (GDB)              the same file straight from memory,
 *                                      even from a hung machine
 * Control: boot option profile[=hz], monitor `profile start [hz]|stop|clear`,
 * user `profile start|stop|run CMD...` (SYS_debug DBG_PROFILE).
 */
#define LOG_SUBSYS SUBSYS_DEBUG
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <proc.h>
#include <profile.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <smp.h>
#include <trap.h>

#define PROF_BUF        (2u << 20)      /* bytes per CPU */
#define PROF_KDEPTH     24
#define PROF_UDEPTH     24

/* One record: this header, then nk kernel PCs, then nu user PCs (leaf first). */
struct prof_rec {
    u64 ts;
    u32 tid, pid;
    u16 cpu;
    u8 nk, nu;
    u8 flags;
    u8 pad[3];
    char comm[16];
};

struct prof_cpu {
    u8 *buf;
    u32 used;
    u64 samples, dropped;
};

static struct prof_cpu prof[MAX_CPUS];
volatile bool profiling;
u32 profile_hz;
static u64 started, stopped_at;

/* ---- sampling (timer interrupt, interrupts off) ---- */

static int user_stack(struct proc *p, uintptr_t pc, uintptr_t fp, u64 *out, int max)
{
    int n = 0;
    out[n++] = pc;
    while (n < max && p->mm && fp && !(fp & 7) && fp < USER_TOP) {
        u64 rec[2];                             /* {caller's fp, return address} at fp - 16 */
        if (uvm_read(p->mm, fp - 16, rec, sizeof(rec)) || !rec[1] || rec[0] <= fp)
            break;
        out[n++] = rec[1];
        fp = rec[0];
    }
    return n;
}

void profile_sample(struct trap_frame *tf)
{
    struct cpu *c = this_cpu();
    struct prof_cpu *pc = &prof[c->id];
    if (!pc->buf)
        return;
    u64 pcs[PROF_KDEPTH + PROF_UDEPTH];
    struct prof_rec r;
    memset(&r, 0, sizeof(r));
    struct thread *t = c->cur_thread;
    struct proc *p = t ? t->proc : NULL;
    bool user = !(tf->sstatus & SSTATUS_SPP);
    int nk = 0, nu = 0;
    if (!user) {
        pcs[nk++] = tf->sepc;
        nk += backtrace_collect(TF_FP(tf), (uintptr_t *)pcs + nk, PROF_KDEPTH - 1);
        while (nk > 1 && pcs[nk - 1] < KERNEL_BASE)
            nk--;               /* the trap's frame record ends with the user pc */
        /* the kernel stack ends at a trap from user mode: continue below it */
        if (p && p->mm && t->kstack) {
            struct trap_frame *utf = thread_user_tf(t);
            bool in_syscall = false;
            for (int i = 0; i < nk; i++) {
                uintptr_t off;
                const char *s = ksym_lookup(pcs[i] - (i ? 1 : 0), &off);
                if (s && !strcmp(s, "kernel_trap_entry"))
                    in_syscall = true;
            }
            if (in_syscall && utf->sepc && utf->sepc < USER_TOP)
                nu = user_stack(p, utf->sepc, TF_FP(utf), pcs + nk, PROF_UDEPTH);
        }
    } else if (p) {
        nu = user_stack(p, tf->sepc, TF_FP(tf), pcs, PROF_UDEPTH);
    }
    u32 size = (u32)(sizeof(r) + (u32)(nk + nu) * 8);
    if (pc->used + size > PROF_BUF) {
        pc->dropped++;
        return;
    }
    r.ts = rdtime();
    r.tid = t ? (u32)t->tid : 0;
    r.pid = p ? (u32)p->pid : 0;
    r.cpu = (u16)c->id;
    r.nk = (u8)nk;
    r.nu = (u8)nu;
    r.flags = (t == c->idle ? PROF_IDLE : 0) | (user ? PROF_USER : 0) |
              (c->irq_depth > 1 ? PROF_IN_IRQ : 0);
    if (t)
        memcpy(r.comm, p ? p->name : t->name, sizeof(r.comm) - 1);
    memcpy(pc->buf + pc->used, &r, sizeof(r));
    memcpy(pc->buf + pc->used + sizeof(r), pcs, (size_t)(nk + nu) * 8);
    __atomic_store_n(&pc->used, pc->used + size, __ATOMIC_RELEASE);   /* readers on other CPUs */
    pc->samples++;
}

/* ---- control ---- */

int profile_start(u32 hz)
{
    if (profiling)
        return 0;
    if (!hz)
        hz = 500;
    if (hz < HZ)
        hz = HZ;
    if (hz > 5000)
        hz = 5000;
    for (int i = 0; i < ncpus_online; i++) {
        if (!prof[i].buf) {
            prof[i].buf = kmalloc(PROF_BUF);
            if (!prof[i].buf) {
                pr_err("profile: no memory for the sample buffers\n");
                return -1;
            }
            prof[i].used = 0;
            prof[i].samples = prof[i].dropped = 0;
        }
    }
    profile_hz = hz;
    started = rdtime();
    stopped_at = 0;
    timer_set_profile_hz(hz);
    __atomic_store_n(&profiling, true, __ATOMIC_RELEASE);
    pr_info("profiling at %u Hz on %d CPUs (%u KiB of samples each)\n", hz, ncpus_online,
            PROF_BUF >> 10);
    return 0;
}

void profile_stop(void)
{
    if (!profiling)
        return;
    __atomic_store_n(&profiling, false, __ATOMIC_RELEASE);
    timer_set_profile_hz(0);
    stopped_at = rdtime();
    u64 n = 0, d = 0;
    for (int i = 0; i < ncpus_online; i++)
        n += prof[i].samples, d += prof[i].dropped;
    pr_info("profiling stopped: %lu samples%s%lu dropped (buffers full)\n", (unsigned long)n,
            d ? ", " : ", ", (unsigned long)d);
}

void profile_clear(void)
{
    profile_stop();
    for (int i = 0; i < MAX_CPUS; i++) {
        kfree(prof[i].buf);
        prof[i].buf = NULL;
        prof[i].used = 0;
        prof[i].samples = prof[i].dropped = 0;
    }
}

/* ---- in-kernel report ---- */

#define TOP 30

struct tally {
    const char *name;
    u64 self, total;
};

static void add(struct tally *t, int *n, int max, const char *name, bool self)
{
    for (int i = 0; i < *n; i++) {
        if (t[i].name == name) {
            t[i].total++;
            t[i].self += self;
            return;
        }
    }
    if (*n < max) {
        t[*n] = (struct tally){ name, self, 1 };
        (*n)++;
    }
}

void profile_report(print_fn print)
{
    u64 samples = 0, dropped = 0, idle = 0, user = 0, kernel = 0;
    for (int i = 0; i < ncpus_online; i++)
        samples += prof[i].samples, dropped += prof[i].dropped;
    u64 end = stopped_at ? stopped_at : rdtime();
    print("profile: %s, %u Hz, %lu samples over %lu ms on %d CPUs, %lu dropped\n",
          profiling ? "running" : samples ? "stopped" : "never started", profile_hz,
          (unsigned long)samples, (unsigned long)(started ? (end - started) * 1000 /
                                                  platform.timebase : 0),
          ncpus_online, (unsigned long)dropped);
    if (!samples)
        return;

    /* kernel functions (self = leaf, total = anywhere on the stack), processes */
    static struct tally funcs[256], comms[64];
    int nf = 0, nc = 0;
    for (int c = 0; c < ncpus_online; c++) {
        struct prof_cpu *pc = &prof[c];
        u32 used = __atomic_load_n(&pc->used, __ATOMIC_ACQUIRE);
        for (u32 off = 0; off < used;) {
            struct prof_rec r;
            memcpy(&r, pc->buf + off, sizeof(r));
            const u64 *pcs = (const u64 *)(pc->buf + off + sizeof(r));
            off += (u32)(sizeof(r) + (u32)(r.nk + r.nu) * 8);
            if (r.flags & PROF_IDLE) {
                idle++;
                continue;
            }
            if (r.flags & PROF_USER)
                user++;
            else
                kernel++;
            /* process names are interned by pointer: find or copy */
            const char *cname = NULL;
            for (int i = 0; i < nc; i++)
                if (!strncmp(comms[i].name, r.comm, 16))
                    cname = comms[i].name;
            if (!cname && nc < (int)ARRAY_SIZE(comms)) {
                char *copy = kmalloc(17);
                if (copy) {
                    memcpy(copy, r.comm, 16);
                    copy[16] = '\0';
                    cname = copy;
                }
            }
            if (cname)
                add(comms, &nc, ARRAY_SIZE(comms), cname, r.flags & PROF_USER);
            const char *seen[PROF_KDEPTH];
            int nseen = 0;
            for (int i = 0; i < r.nk; i++) {
                uintptr_t o;
                const char *s = ksym_lookup(pcs[i] - (i ? 1 : 0), &o);
                if (!s)
                    continue;
                bool dup = false;
                for (int k = 0; k < nseen; k++)
                    dup |= seen[k] == s;
                if (dup)
                    continue;                   /* recursion counts once per sample */
                seen[nseen++] = s;
                add(funcs, &nf, ARRAY_SIZE(funcs), s, i == 0);
            }
        }
    }
    u64 busy = samples - idle;
    print("  idle %lu%%, user %lu%%, kernel %lu%% (of all samples)\n",
          (unsigned long)(idle * 100 / samples), (unsigned long)(user * 100 / samples),
          (unsigned long)(kernel * 100 / samples));
    print("  by process/thread (busy samples; user = in user mode):\n");
    for (int k = 0; k < nc; k++) {
        int best = k;
        for (int j = k + 1; j < nc; j++)
            if (comms[j].total > comms[best].total)
                best = j;
        struct tally tmp = comms[k];
        comms[k] = comms[best];
        comms[best] = tmp;
        if (k < 12)
            print("    %5lu.%lu%%  %-16s (user %lu%%)\n",
                  (unsigned long)(comms[k].total * 100 / (busy ? busy : 1)),
                  (unsigned long)(comms[k].total * 1000 / (busy ? busy : 1) % 10), comms[k].name,
                  (unsigned long)(comms[k].self * 100 / (comms[k].total ? comms[k].total : 1)));
    }
    print("  kernel functions (of busy samples):   self   total\n");
    for (int k = 0; k < nf && k < TOP; k++) {
        int best = k;
        for (int j = k + 1; j < nf; j++)
            if (funcs[j].self > funcs[best].self ||
                (funcs[j].self == funcs[best].self && funcs[j].total > funcs[best].total))
                best = j;
        struct tally tmp = funcs[k];
        funcs[k] = funcs[best];
        funcs[best] = tmp;
        print("    %-34s %5lu.%lu%% %5lu.%lu%%\n", funcs[k].name,
              (unsigned long)(funcs[k].self * 100 / (busy ? busy : 1)),
              (unsigned long)(funcs[k].self * 1000 / (busy ? busy : 1) % 10),
              (unsigned long)(funcs[k].total * 100 / (busy ? busy : 1)),
              (unsigned long)(funcs[k].total * 1000 / (busy ? busy : 1) % 10));
    }
    for (int i = 0; i < nc; i++)
        kfree((void *)comms[i].name);
    print("  (user functions and flamegraphs: profile dump, then make flamegraph)\n");
}

/* ---- export ---- */

struct prof_file_hdr {
    char magic[8];                  /* "EUPROF1\0" */
    u32 version, ncpus;
    u64 timebase;
    u32 hz, rec_hdr;
    u64 dropped;
};

int profile_dump_file(const char *path)
{
    bool was = profiling;
    profile_stop();
    long fd = semihost_open(path, SEMIHOST_MODE_WB);
    if (fd < 0)
        return -1;
    struct prof_file_hdr h = { .version = 1, .ncpus = (u32)ncpus_online,
                               .timebase = platform.timebase, .hz = profile_hz,
                               .rec_hdr = sizeof(struct prof_rec) };
    memcpy(h.magic, "EUPROF1", 8);
    for (int i = 0; i < ncpus_online; i++)
        h.dropped += prof[i].dropped;
    long total = semihost_write(fd, &h, sizeof(h));
    for (int i = 0; i < ncpus_online; i++) {
        u64 n = prof[i].buf ? prof[i].used : 0;
        total += semihost_write(fd, &n, sizeof(n));
        if (n)
            total += semihost_write(fd, prof[i].buf, n);
    }
    semihost_close(fd);
    if (was)
        profile_start(profile_hz);
    return (int)total;
}

void profile_boot_option(void)
{
    const char *opt = cmdline_get("profile");
    if (opt)
        profile_start(*opt ? (u32)kstrtoul(opt, NULL, 10) : 0);
}
