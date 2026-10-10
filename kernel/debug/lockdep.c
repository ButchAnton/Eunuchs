/*
 * Lock dependency checking: catches lock-order inversions before they
 * deadlock. See lockdep.h.
 *
 * Classes: a lock's class is (kind, name); the class id is cached in the
 * lock (spinlock_t.ld_class, struct mutex.ld_class) on first use.
 *
 * Graph: deps[a] is a bitmap of classes b such that b was acquired while a
 * was held. A new edge a -> b is checked with a breadth-first search for a
 * path b -> ... -> a; finding one means a cycle. For each edge the first
 * occurrence is recorded (thread, CPU, both acquisition sites) so the report
 * can show how every link of the cycle came about.
 *
 * What counts as "held" when acquiring: this CPU's spinlocks, plus the
 * current thread's mutexes unless we are in an interrupt handler (the
 * handler is not running on behalf of the interrupted thread). Nesting two
 * locks of the same class (two inodes, two buffers) is not checked: their
 * ordering is the caller's business, as in Linux without annotations.
 *
 * Internal state is protected by a raw test-and-set lock that lockdep does
 * not track; reports are printed after dropping it, with cpu->in_lockdep set
 * so the printk lock is not checked recursively.
 *
 * Debugging: `lockdep` in the monitor and /proc/lockdep list the classes
 * and dependencies with the sites that created them.
 */
#define LOG_SUBSYS SUBSYS_DEBUG
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <lockdep.h>
#include <printk.h>
#include <riscv.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>

#define LD_CLASSES  192
#define LD_WORDS    (LD_CLASSES / 64)
#define LD_EDGES    1024

struct ld_class {
    const char *name;
    u8 kind;
};

struct ld_edge {
    u16 from, to;
    u16 cpu;
    int tid;
    char thread[16];
    uintptr_t from_pc, to_pc;   /* where `from` was taken; where `to` was being taken */
};

bool lockdep_enabled = true;
u64 lockdep_reports;
bool lockdep_testing;
static bool ld_panic;
static struct ld_class classes[LD_CLASSES];     /* [0] unused: "no class yet" */
static int nclasses = 1;
static u64 deps[LD_CLASSES][LD_WORDS];
static struct ld_edge edges[LD_EDGES];
static int nedges;
static u64 checks;
static volatile u32 ld_raw;
static bool class_overflow_warned;

static const char *kind_name(int kind)
{
    return kind == LD_MUTEX ? "mutex" : "spinlock";
}

static void raw_lock(void)
{
    while (__atomic_exchange_n(&ld_raw, 1, __ATOMIC_ACQUIRE))
        ipi_poll();
}

static void raw_unlock(void)
{
    __atomic_store_n(&ld_raw, 0, __ATOMIC_RELEASE);
}

void lockdep_init(void)
{
    const char *mode = cmdline_get("lockdep");
    if (mode && !strcmp(mode, "off"))
        lockdep_enabled = false;
    else if (mode && !strcmp(mode, "panic"))
        ld_panic = true;
    pr_info("lockdep: %s\n", !lockdep_enabled ? "off"
                             : ld_panic ? "on, inversions panic" : "on, inversions warn once");
}

/* raw lock held */
static u16 lookup_class(const char *name, int kind)
{
    if (!name)
        name = "?";
    for (int i = 1; i < nclasses; i++)
        if (classes[i].kind == kind && !strcmp(classes[i].name, name))
            return (u16)i;
    if (nclasses == LD_CLASSES)
        return 0;
    classes[nclasses].name = name;
    classes[nclasses].kind = (u8)kind;
    return (u16)nclasses++;
}

static u16 class_of(u16 *cache, const char *name, int kind)
{
    u16 id = __atomic_load_n(cache, __ATOMIC_RELAXED);
    if (id)
        return id;
    raw_lock();
    id = lookup_class(name, kind);
    raw_unlock();
    if (!id && !class_overflow_warned) {
        class_overflow_warned = true;
        pr_warn("lockdep: more than %d lock classes; new classes are not checked\n",
                LD_CLASSES - 1);
    }
    __atomic_store_n(cache, id, __ATOMIC_RELAXED);
    return id;
}

/* Not __builtin_ctzl: libgcc's version can't be linked into a higher-half kernel. */
static int lowest_bit(u64 v)
{
    int n = 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
}

static bool has_dep(int a, int b)
{
    return deps[a][b / 64] & (1UL << (b % 64));
}

/* raw lock held. Path from -> ... -> to as class ids into path[]; length or 0. */
static int find_path(int from, int to, u16 *path, int max)
{
    u16 queue[LD_CLASSES], parent[LD_CLASSES];
    u64 seen[LD_WORDS] = { 0 };
    int head = 0, tail = 0;
    queue[tail++] = (u16)from;
    seen[from / 64] |= 1UL << (from % 64);
    parent[from] = (u16)from;
    while (head < tail) {
        int a = queue[head++];
        if (a == to) {
            int n = 0;
            for (int x = to; n < max; x = parent[x]) {
                path[n++] = (u16)x;
                if (x == from)
                    break;
            }
            for (int i = 0; i < n / 2; i++) {         /* reverse: from first */
                u16 t = path[i];
                path[i] = path[n - 1 - i];
                path[n - 1 - i] = t;
            }
            return n;
        }
        for (int w = 0; w < LD_WORDS; w++) {
            u64 bits = deps[a][w] & ~seen[w];
            while (bits) {
                int b = w * 64 + lowest_bit(bits);
                bits &= bits - 1;
                seen[w] |= 1UL << (b % 64);
                parent[b] = (u16)a;
                queue[tail++] = (u16)b;
            }
        }
    }
    return 0;
}

static const struct ld_edge *find_edge(int from, int to)
{
    for (int i = 0; i < nedges; i++)
        if (edges[i].from == from && edges[i].to == to)
            return &edges[i];
    return NULL;
}

static void print_class(char *buf, size_t size, int id)
{
    ksnprintf(buf, size, "%s '%s'", kind_name(classes[id].kind), classes[id].name);
}

static void report_inversion(int from, int to, uintptr_t from_pc, uintptr_t to_pc,
                             const u16 *path, int plen)
{
    struct thread *t = current;
    char a[64], b[64], s1[96], s2[96];
    print_class(a, sizeof(a), from);
    print_class(b, sizeof(b), to);
    ksym_format(s1, sizeof(s1), to_pc);
    ksym_format(s2, sizeof(s2), from_pc);

    pr_err("LOCKDEP: possible deadlock: lock order inversion\n");
    pr_err("  cpu%d, thread %d (%s) is acquiring %s at %s\n", this_cpu()->id, t ? t->tid : -1,
           t ? t->name : "-", b, s1);
    pr_err("  while holding %s, taken at %s\n", a, s2);
    pr_err("  but the opposite order (%s first) was seen before:\n", classes[to].name);
    for (int i = 0; i + 1 < plen; i++) {
        const struct ld_edge *e = find_edge(path[i], path[i + 1]);
        char x[64], y[64];
        print_class(x, sizeof(x), path[i]);
        print_class(y, sizeof(y), path[i + 1]);
        if (!e) {
            pr_err("    %s -> %s\n", x, y);
            continue;
        }
        ksym_format(s1, sizeof(s1), e->from_pc);
        ksym_format(s2, sizeof(s2), e->to_pc);
        pr_err("    %s -> %s: thread %d (%s) on cpu%d\n", x, y, e->tid, e->thread, e->cpu);
        pr_err("        held %s, taken at %s\n", x, s1);
        pr_err("        then acquired %s at %s\n", y, s2);
    }
    pr_err("  if these run concurrently, each can end up waiting for the other's lock.\n");
    pr_err("  backtrace of the acquisition:\n");
    backtrace_here(log_err_print);
    trace(lock_inversion, (u64)classes[from].name, (u64)classes[to].name, to_pc);
}

static void add_dep(int from, int to, uintptr_t from_pc, uintptr_t to_pc)
{
    if (!from || !to || from == to || has_dep(from, to))
        return;
    u16 path[32];
    raw_lock();
    if (has_dep(from, to) || !lockdep_enabled) {
        raw_unlock();
        return;
    }
    int plen = find_path(to, from, path, ARRAY_SIZE(path));
    if (plen && lockdep_testing) {
        lockdep_reports++;
        raw_unlock();
        pr_info("lockdep (self-test): inversion '%s' -> '%s' detected\n", classes[from].name,
                classes[to].name);
        return;
    }
    if (plen) {
        /* Report once and stop checking: the graph is now known to be cyclic. */
        lockdep_enabled = false;
        lockdep_reports++;
        raw_unlock();
        report_inversion(from, to, from_pc, to_pc, path, plen);    /* in_lockdep is set */
        if (ld_panic)
            panic("lockdep: lock order inversion between '%s' and '%s' (details above)",
                  classes[from].name, classes[to].name);
        pr_err("lockdep: further checking disabled\n");
        return;
    }
    deps[from][to / 64] |= 1UL << (to % 64);
    if (nedges < LD_EDGES) {
        struct ld_edge *e = &edges[nedges++];
        struct thread *t = current;
        e->from = (u16)from;
        e->to = (u16)to;
        e->cpu = (u16)this_cpu()->id;
        e->tid = t ? t->tid : -1;
        strlcpy(e->thread, t ? t->name : "-", sizeof(e->thread));
        e->from_pc = from_pc;
        e->to_pc = to_pc;
    }
    raw_unlock();
}

void lockdep_acquire(u16 *class_cache, const char *name, int kind, uintptr_t pc)
{
    if (!lockdep_enabled || panic_depth || monitor_entry_time)
        return;
    unsigned long sie = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    struct cpu *c = this_cpu();
    if (c->in_lockdep)
        goto out;
    c->in_lockdep = true;
    checks++;
    int to = class_of(class_cache, name, kind);
    for (int i = 0; i < MIN(c->nheld, MAX_HELD_LOCKS); i++) {
        struct spinlock *h = c->held[i];
        add_dep(class_of(&h->ld_class, h->name, LD_SPIN), to, h->owner_pc, pc);
    }
    struct thread *t = c->cur_thread;
    if (t && c->irq_depth == 0) {
        for (int i = 0; i < MIN(t->nheld_mutex, MAX_HELD_MUTEX); i++) {
            struct mutex *m = t->held_mutex[i];
            add_dep(class_of(&m->ld_class, m->name, LD_MUTEX), to, m->owner_pc, pc);
        }
    }
    c->in_lockdep = false;
out:
    if (sie)
        intr_on();
}

void lockdep_might_sleep(const char *what, uintptr_t pc)
{
    if (!lockdep_enabled || panic_depth || monitor_entry_time)
        return;
    unsigned long sie = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    struct cpu *c = this_cpu();
    if (c->nheld == 0 || c->in_lockdep) {
        if (sie)
            intr_on();
        return;
    }
    struct spinlock *h = c->held[MIN(c->nheld, MAX_HELD_LOCKS) - 1];
    char s1[96], s2[96];
    ksym_format(s1, sizeof(s1), pc);
    ksym_format(s2, sizeof(s2), h->owner_pc);
    lockdep_reports++;
    c->in_lockdep = true;
    pr_err("LOCKDEP: %s at %s while holding spinlock '%s' (taken at %s): it may sleep\n",
           what, s1, h->name ? h->name : "?", s2);
    backtrace_here(log_err_print);
    c->in_lockdep = false;
    if (ld_panic && !lockdep_testing)
        panic("lockdep: sleeping operation with spinlock '%s' held", h->name ? h->name : "?");
    if (sie)
        intr_on();
}

void lockdep_print(print_fn print)
{
    print("lockdep: %s; %d classes, %d dependencies, %lu checks, %lu reports\n",
          lockdep_enabled ? "on" : "off", nclasses - 1, nedges, (unsigned long)checks,
          (unsigned long)lockdep_reports);
    for (int a = 1; a < nclasses; a++) {
        int n = 0;
        for (int b = 1; b < nclasses; b++)
            n += has_dep(a, b);
        print("  %-8s %-18s", kind_name(classes[a].kind), classes[a].name);
        if (n) {
            print(" ->");
            for (int b = 1; b < nclasses; b++)
                if (has_dep(a, b))
                    print(" %s%s", classes[b].kind == LD_MUTEX ? "mutex:" : "", classes[b].name);
        }
        print("\n");
    }
}
