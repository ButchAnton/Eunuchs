/*
 * Trace event recording, decoding and export. See trace.h.
 *
 * Dump file format (little endian), read by tools/trace/eutrace.py:
 *   header:  "EUTRACE1" u32 version=1, u32 ncpus, u64 timebase, u32 rec_size,
 *            u32 nthreads
 *   threads: nthreads x { u32 tid, u32 state, char name[16] }
 *   per cpu: u64 count, then count x struct trace_rec, oldest first
 */
#define LOG_SUBSYS SUBSYS_DEBUG
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <net.h>
#include <platform.h>
#include <proc.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>

struct trace_buf trace_bufs[MAX_CPUS];
u32 trace_groups_enabled = TG_ALL & ~TG_MM;

const u32 trace_event_group[TE_MAX] = {
#define X(name, group, a0, a1, a2) [TE_##name] = TG_##group,
    TRACE_EVENTS(X)
#undef X
};

static const char *const event_names[TE_MAX] = {
#define X(name, group, a0, a1, a2) [TE_##name] = #name,
    TRACE_EVENTS(X)
#undef X
};

static const char *const event_args[TE_MAX][3] = {
#define X(name, group, a0, a1, a2) [TE_##name] = { a0, a1, a2 },
    TRACE_EVENTS(X)
#undef X
};

static const struct {
    const char *name;
    u32 mask;
} groups[] = {
    { "sched", TG_SCHED }, { "irq", TG_IRQ }, { "lock", TG_LOCK },
    { "mm", TG_MM }, { "mark", TG_MARK }, { "syscall", TG_SYSCALL }, { "proc", TG_PROC },
    { "blk", TG_BLK }, { "fs", TG_FS }, { "net", TG_NET },
};

void trace_record(enum trace_event ev, u64 a0, u64 a1, u64 a2)
{
    unsigned long sie = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    struct cpu *c = this_cpu();
    struct trace_buf *b = &trace_bufs[c->id];
    struct trace_rec *r = &b->recs[b->head % TRACE_RECS];
    r->ts = rdtime();
    r->type = ev;
    r->cpu = c->id;
    r->tid = c->cur_thread ? c->cur_thread->tid : 0;
    r->a[0] = a0;
    r->a[1] = a1;
    r->a[2] = a2;
    b->head++;
    if (sie)
        intr_on();
}

int trace_set_groups(const char *spec)
{
    u32 mask = 0;
    char buf[64];
    strlcpy(buf, spec, sizeof(buf));
    for (char *item = buf; item && *item;) {
        char *next = strchr(item, ',');
        if (next)
            *next++ = '\0';
        if (!strcmp(item, "all")) {
            mask = TG_ALL;
        } else if (!strcmp(item, "none")) {
            mask = 0;
        } else {
            size_t i;
            for (i = 0; i < ARRAY_SIZE(groups); i++)
                if (!strcmp(item, groups[i].name))
                    break;
            if (i == ARRAY_SIZE(groups))
                return -1;
            mask |= groups[i].mask;
        }
        item = next;
    }
    trace_groups_enabled = mask;
    return 0;
}

void trace_init(void)
{
    const char *spec = cmdline_get("trace");
    if (spec && trace_set_groups(spec))
        pr_warn("trace=%s: unknown group (use sched,irq,lock,mm,mark,syscall,proc,blk,fs,net,all,none)\n", spec);
    pr_info("tracing: %d records/cpu, groups %#x\n", TRACE_RECS, trace_groups_enabled);
}

void trace_clear(void)
{
    for (int i = 0; i < MAX_CPUS; i++)
        trace_bufs[i].head = 0;
}

void trace_status(print_fn print)
{
    print("trace groups:");
    for (size_t i = 0; i < ARRAY_SIZE(groups); i++)
        print(" %s%s", trace_groups_enabled & groups[i].mask ? "+" : "-", groups[i].name);
    print("\n");
    for (int i = 0; i < MAX_CPUS; i++) {
        u64 h = trace_bufs[i].head;
        if (h)
            print("cpu%d: %lu events recorded, %lu kept (ring of %d)\n", i,
                  (unsigned long)h, (unsigned long)MIN(h, (u64)TRACE_RECS), TRACE_RECS);
    }
}

/* ---- decoding ---- */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

static int fmt_arg(char *buf, size_t size, const char *name, u64 v, const struct trace_rec *r)
{
    if (ends_with(name, "tid")) {
        return ksnprintf(buf, size, "%s=%d(%s)", name, (int)v, thread_name((int)v));
    }
    if (!strcmp(name, "sig"))
        return ksnprintf(buf, size, "%s", signal_name((int)v));
    if (ends_with(name, "pid") || !strcmp(name, "code") || !strcmp(name, "status") ||
        !strcmp(name, "sector") || !strcmp(name, "len") || !strcmp(name, "us") ||
        !strcmp(name, "seq") || !strcmp(name, "blocks") || !strcmp(name, "write"))
        return ksnprintf(buf, size, "%s=%ld", name, (long)v);
    if (ends_with(name, "state"))
        return ksnprintf(buf, size, "%s=%s", name, thread_state_name((enum thread_state)v));
    if (ends_with(name, "pc") || ends_with(name, "site")) {
        char s[80];
        ksym_format(s, sizeof(s), v);
        return ksnprintf(buf, size, "%s=%s", name, s);
    }
    if (!strcmp(name, "name16")) {
        char s[17];
        memcpy(s, &r->a[1], 16);
        s[16] = '\0';
        return ksnprintf(buf, size, "name=%s", s);
    }
    if (!strcmp(name, "text24")) {
        char s[25];
        memcpy(s, r->a, 24);
        s[24] = '\0';
        return ksnprintf(buf, size, "\"%s\"", s);
    }
    if (!strcmp(name, "nr")) {
        const char *sn = syscall_name((long)v);
        return sn ? ksnprintf(buf, size, "%s", sn) : ksnprintf(buf, size, "nr=%ld", (long)v);
    }
    if (!strcmp(name, "ret") || !strcmp(name, "result"))
        return ksnprintf(buf, size, "%s=%ld", name, (long)v);
    if (!strcmp(name, "str")) {
        char s[40];
        size_t i = 0;
        u8 ch;
        while (i < sizeof(s) - 1 && !safe_read_u8(v + i, &ch) && ch)
            s[i++] = (char)ch;
        s[i] = '\0';
        return ksnprintf(buf, size, "\"%s\"", s);
    }
    if (ends_with(name, "_ip"))
        return ksnprintf(buf, size, "%s=%u.%u.%u.%u", name, (unsigned)(v >> 24) & 255,
                         (unsigned)(v >> 16) & 255, (unsigned)(v >> 8) & 255, (unsigned)v & 255);
    if (!strncmp(name, "tcp_", 4))
        return ksnprintf(buf, size, "%s", tcp_state_name((enum tcp_state)v));
    if (!strcmp(name, "proto"))
        return ksnprintf(buf, size, "%s", v == 6 ? "TCP" : v == 17 ? "UDP" : v == 1 ? "ICMP" :
                                          v == 0x806 ? "ARP" : "?");
    if (!strcmp(name, "cpu"))
        return ksnprintf(buf, size, "cpu%d", (int)v);
    if (!strcmp(name, "ipi"))
        return ksnprintf(buf, size, "%s", v < IPI_MAX ? ipi_names[v] : "?");
    if (!strcmp(name, "cause"))
        return ksnprintf(buf, size, "%s", v == IRQ_S_TIMER ? "timer" : v == IRQ_S_EXT ? "external"
                                         : v == IRQ_S_SOFT ? "software" : "?");
    return ksnprintf(buf, size, "%s=%#lx", name, (unsigned long)v);
}

static void print_rec(print_fn print, const struct trace_rec *r)
{
    char line[200];
    u64 tbase = platform.timebase ? platform.timebase : 10000000;
    int n = ksnprintf(line, sizeof(line), "[%5lu.%06lu] c%u %3u %-8.8s %-15s",
                      (unsigned long)(r->ts / tbase),
                      (unsigned long)((r->ts % tbase) * 1000000 / tbase),
                      r->cpu, r->tid, thread_name(r->tid),
                      r->type < TE_MAX ? event_names[r->type] : "?");
    for (int i = 0; i < 3 && r->type < TE_MAX && n < (int)sizeof(line) - 1; i++) {
        const char *an = event_args[r->type][i];
        if (!an[0] || (i == 2 && !strcmp(event_args[r->type][1], "name16")) ||
            (i > 0 && !strcmp(event_args[r->type][0], "text24")) ||
            (!strcmp(an, "irq") && r->a[i] == 0))
            continue;
        line[n++] = ' ';
        n += fmt_arg(line + n, sizeof(line) - n, an, r->a[i], r);
    }
    print("%s\n", line);
}

/*
 * The last `last_n` events of all CPUs, merged in time order (rdtime is one
 * clock for all harts). First walk backwards from each ring's head to find
 * where every CPU's share of the newest last_n starts, then merge forwards.
 */
void trace_print(print_fn print, int last_n)
{
    int ncpu = ncpus_online;
    u64 head[MAX_CPUS], pos[MAX_CPUS], lo[MAX_CPUS];
    u64 total = 0;
    for (int c = 0; c < ncpu; c++) {
        head[c] = trace_bufs[c].head;
        lo[c] = head[c] - MIN(head[c], (u64)TRACE_RECS - 64);   /* margin: CPUs keep writing */
        pos[c] = head[c];
        total += head[c] - lo[c];
    }
    u64 want = last_n > 0 ? MIN((u64)last_n, total) : total;
    for (u64 k = 0; k < want; k++) {                /* step back over the newest */
        int best = -1;
        for (int c = 0; c < ncpu; c++)
            if (pos[c] > lo[c] && (best < 0 ||
                trace_bufs[c].recs[(pos[c] - 1) % TRACE_RECS].ts >
                trace_bufs[best].recs[(pos[best] - 1) % TRACE_RECS].ts))
                best = c;
        pos[best]--;
    }
    for (u64 k = 0; k < want; k++) {                /* and print them oldest first */
        int best = -1;
        for (int c = 0; c < ncpu; c++)
            if (pos[c] < head[c] && (best < 0 ||
                trace_bufs[c].recs[pos[c] % TRACE_RECS].ts <
                trace_bufs[best].recs[pos[best] % TRACE_RECS].ts))
                best = c;
        print_rec(print, &trace_bufs[best].recs[pos[best]++ % TRACE_RECS]);
    }
    if (!want)
        print("(no trace events recorded)\n");
}

/* ---- export ---- */

struct dump_header {
    char magic[8];
    u32 version;
    u32 ncpus;
    u64 timebase;
    u32 rec_size;
    u32 nthreads;
};

struct dump_thread {
    u32 tid;
    u32 state;
    char name[16];
};

int trace_dump_file(const char *path)
{
    long fd = semihost_open(path, SEMIHOST_MODE_WB);
    if (fd < 0)
        return -1;

    long total = 0;
    struct thread *t;
    struct dump_header h = { .version = 1, .ncpus = (u32)ncpus_online,
                             .timebase = platform.timebase, .rec_size = sizeof(struct trace_rec) };
    memcpy(h.magic, "EUTRACE1", 8);

    /* Snapshot the thread list: threads come and go on other CPUs meanwhile. */
    enum { MAX_DUMP_THREADS = 256 };
    struct dump_thread *dts = kmalloc(MAX_DUMP_THREADS * sizeof(*dts));
    if (dts) {
        unsigned long flags;
        bool locked = spin_lock_debug(&sched_lock, &flags);
        list_for_each_entry(t, &all_threads, all_node) {
            if (h.nthreads == MAX_DUMP_THREADS)
                break;
            struct dump_thread *dt = &dts[h.nthreads++];
            dt->tid = t->tid;
            dt->state = t->state;
            memcpy(dt->name, t->name, sizeof(dt->name));
        }
        spin_unlock_debug(&sched_lock, locked, flags);
    }
    total += semihost_write(fd, &h, sizeof(h));
    if (dts) {
        total += semihost_write(fd, dts, h.nthreads * sizeof(*dts));
        kfree(dts);
    }

    for (int c = 0; c < ncpus_online; c++) {
        struct trace_buf *b = &trace_bufs[c];
        /* Leave a margin: other CPUs keep recording while we write. */
        u64 head = b->head, count = MIN(head, (u64)TRACE_RECS - 256);
        total += semihost_write(fd, &count, sizeof(count));
        u64 start = (head - count) % TRACE_RECS;
        u64 first = MIN(count, (u64)TRACE_RECS - start);
        total += semihost_write(fd, &b->recs[start], first * sizeof(struct trace_rec));
        if (count > first)
            total += semihost_write(fd, &b->recs[0], (count - first) * sizeof(struct trace_rec));
    }

    semihost_close(fd);
    return (int)total;
}
