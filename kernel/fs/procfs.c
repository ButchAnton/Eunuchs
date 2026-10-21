/*
 * procfs, mounted on /proc: kernel state as text, generated when a file is
 * opened (reads see one consistent snapshot). Uses the same print functions
 * as the debug monitor.
 *
 *   /proc/{meminfo,vmstat,swaps,pagecache,asids,stat,slabinfo,interrupts,threads,sched,
 *          procs,mounts,bcache,blk,icache,trace,virtio,uptime,pipes,tty,cpus,lockdep}
 *   /proc/drop_caches (root may write: drop unused cached pages)
 *   /proc/<pid>/{status,maps,fds,stack,ustack}      /proc/self -> current
 *   /proc/net/{dev,arp,tcp,udp,stats}
 *
 * Per-process files are generated with proc_lock held (except for the
 * cwd's path, which needs sleeping I/O), so the process, its mm and its
 * program image cannot go away underneath, even while it exits or execs on
 * another CPU.
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <debug.h>
#include <fmt.h>
#include <fs.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <proc.h>
#include <lockdep.h>
#include <net.h>
#include <profile.h>
#include <riscv.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>
#include <uapi/errno.h>
#include <virtio.h>

#define PROC_ROOT    1
#define NET_DIR      0x80000     /* /proc/net; its files follow */
#define PID_BASE     0x100000
#define PID_FILES    8
#define SEQ_MAX      (64 * 1024)

/* ---- text generation ---- */

static struct mutex gen_lock = MUTEX_INIT(gen_lock, "procfs-gen");
static char *seq_buf;
static size_t seq_len;

int seq_print(const char *fmt, ...)
{
    if (!seq_buf || seq_len >= SEQ_MAX - 1)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(seq_buf + seq_len, SEQ_MAX - seq_len, fmt, ap);
    va_end(ap);
    seq_len = MIN(seq_len + n, (size_t)SEQ_MAX - 1);
    return n;
}

static void gen_uptime(print_fn print)
{
    u64 t = rdtime(), tb = platform.timebase;
    print("%lu.%02lu\n", (unsigned long)(t / tb), (unsigned long)((t % tb) * 100 / tb));
}

static void gen_trace(print_fn print) { trace_print(print, 200); }

/* For top and vmstat: times in ms since boot, per CPU busy and idle, and
   process counts (running: runnable or on a CPU; blocked: uninterruptible). */
static void gen_stat(print_fn print)
{
    u64 tb = platform.timebase ? platform.timebase : 1;
    print("uptime_ms %lu\ncpus %d\n", (unsigned long)(rdtime() * 1000 / tb), ncpus_online);
    for (int i = 0; i < ncpus_online; i++) {
        u64 idle = thread_cpu_ticks(cpus[i].idle) * 1000 / tb;
        print("cpu%d idle_ms %lu switches %lu\n", i, (unsigned long)idle, (unsigned long)cpus[i].switches);
    }
    int procs = 0, running = 0, blocked = 0;
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *p;
    list_for_each_entry(p, &all_procs, all_node) {
        procs++;
        struct thread *t = p->thread;
        if (p->state == P_ZOMBIE || !t)
            continue;
        if (t->state == T_RUNNING || t->state == T_RUNNABLE)
            running++;
        else if (t->state == T_SLEEPING && !t->interruptible)
            blocked++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    print("procs %d\nprocs_running %d\nprocs_blocked %d\n", procs, running, blocked);
}

/* /proc/drop_caches: reading says what could go; writing anything (root:
   the file is 0644) writes dirty file pages back and frees every cached
   page nobody maps. For measuring with a cold cache. */
static void gen_drop_caches(print_fn print)
{
    u64 f, s, d, m;
    pc_stats(&f, &s, &d, &m);
    print("%lu cached pages (%lu dirty, %lu in use); write 1 here to drop the unused ones\n",
          (unsigned long)(f + s), (unsigned long)d, (unsigned long)m);
}

static long write_drop_caches(const char *buf, size_t n)
{
    (void)buf;
    unsigned long freed = pc_drop_all();
    pr_debug("drop_caches: freed %lu pages (%lu KiB)\n", freed, freed * 4);
    return (long)n;
}

static const struct {
    const char *name;
    void (*gen)(print_fn print);
} root_files[] = {
    { "meminfo", pmm_print },
    { "slabinfo", slab_print },
    { "interrupts", irq_print },
    { "threads", thread_list_print },
    { "sched", sched_print },
    { "procs", proc_list_print },
    { "mounts", vfs_mounts_print },
    { "bcache", bcache_print },
    { "blk", blk_print },
    { "icache", icache_print },
    { "trace", gen_trace },
    { "virtio", virtio_print },
    { "9p", p9_print },
    { "uptime", gen_uptime },
    { "pipes", pipes_print },
    { "tty", tty_print },
    { "cpus", cpus_print },
    { "lockdep", lockdep_print },
    { "profile", profile_report },
    { "vmstat", vm_counters_print },
    { "swaps", swap_print },
    { "pagecache", pc_print },
    { "asids", asid_print },
    { "stat", gen_stat },
    { "drop_caches", gen_drop_caches },
};
#define NROOT ARRAY_SIZE(root_files)

/* /proc/net: generated with net_lock held */
#define NET_GEN(name, fn)                                                   \
    static void gen_net_##name(print_fn print)                              \
    {                                                                       \
        mutex_lock(&net_lock);                                              \
        fn(print);                                                          \
        mutex_unlock(&net_lock);                                            \
    }
NET_GEN(dev, net_print_dev)
NET_GEN(arp, arp_print)
NET_GEN(tcp, tcp_print)
NET_GEN(udp, udp_print)
NET_GEN(stats, net_print_stats)

static const struct {
    const char *name;
    void (*gen)(print_fn print);
} net_files[] = {
    { "dev", gen_net_dev }, { "arp", gen_net_arp }, { "tcp", gen_net_tcp },
    { "udp", gen_net_udp }, { "stats", gen_net_stats },
};
#define NNET ARRAY_SIZE(net_files)

static const char *const pid_files[] = { NULL, "status", "maps", "fds", "stack", "ustack", "cmdline" };
#define NPIDF ARRAY_SIZE(pid_files)

static u32 pid_dir(int pid) { return PID_BASE + (u32)pid * PID_FILES; }

/* proc_lock held. */
static void gen_pid_locked(struct proc *p, int which, print_fn print)
{
    switch (which) {
    case 1: {
        struct thread *t = p->thread;
        print("pid %d\nppid %d\nname %s\nstate %s\n", p->pid, p->parent ? p->parent->pid : 0,
              p->name, p->state == P_ZOMBIE ? "zombie" : t ? thread_state_name(t->state) : "?");
        if (t)
            print("tid %d\nwaiting %s\nswitches %lu\n", t->tid, t->wait_what ? t->wait_what : "-",
                  (unsigned long)t->switches);
        print("syscalls %lu\nrss_kib %lu\nstrace %d\n", (unsigned long)p->syscalls,
              (unsigned long)(p->mm ? p->mm->rss * 4 : 0), p->strace);
        if (p->mm)
            print("swap_kib %lu\nmin_flt %lu\nmaj_flt %lu\ncow %lu\n", (unsigned long)p->mm->swapped * 4,
                  (unsigned long)(p->mm->faults - p->mm->major_faults), (unsigned long)p->mm->major_faults,
                  (unsigned long)p->mm->cow_copies);
        print("uid %u euid %u gid %u egid %u umask %03o\n", p->uid, p->euid, p->gid, p->egid,
              p->umask);
        print("pgid %d\nsid %d\nstopped %d\nsig_pending %08x\nsig_blocked %08x\n", p->pgid,
              p->sid, p->stopped, p->sig_pending, p->sig_blocked);
        /* For ps: Unix state letter, times in ms, sizes in KiB, terminal. */
        char st = p->state == P_ZOMBIE ? 'Z' : p->ptrace_stopped ? 't' : p->stopped ? 'T' : !t ? '?'
                : t->state == T_RUNNING || t->state == T_RUNNABLE ? 'R'
                : t->state == T_SLEEPING ? (t->interruptible ? 'S' : 'D') : 'X';
        u64 vsz = 0;
        if (p->mm) {
            struct vma *v;
            unsigned long vf = spin_lock_irqsave(&p->mm->vma_lock);
            list_for_each_entry(v, &p->mm->vmas, node)
                vsz += v->end - v->start;
            spin_unlock_irqrestore(&p->mm->vma_lock, vf);
        }
        u64 tbase = platform.timebase ? platform.timebase : 1;
        print("ustate %c\nstart_ms %lu\ncpu_ms %lu\nvsz_kib %lu\ntty %s\ntpgid %d\npsr %d\n", st,
              (unsigned long)(p->started * 1000 / tbase),
              (unsigned long)(t ? thread_cpu_ticks(t) * 1000 / tbase : 0),
              (unsigned long)(vsz / 1024), p->ctty ? "console" : "-",
              p->ctty ? tty_fg_pgrp(p->ctty) : -1, t ? t->cpu : -1);
        print("sig_caught");
        for (int s = 1; s < 32; s++)
            if (p->sigactions[s].handler > 1)
                print(" %s", signal_name(s));
        print("\nsig_ignored");
        for (int s = 1; s < 32; s++)
            if (p->sigactions[s].handler == 1)
                print(" %s", signal_name(s));
        print("\n");
        break;
    }
    case 2: mm_print(print, p->mm); break;
    case 3: files_print_fds(print, p); break;
    case 4:
        if (p->thread)
            thread_backtrace(print, p->thread->tid);
        break;
    case 5:
        if (p->thread) {
            struct trap_frame *tf = thread_user_tf(p->thread);
            proc_user_backtrace(print, p, tf->sepc, TF_FP(tf));
        }
        break;
    case 6:
        /* Like Linux: the arguments, each ending in a NUL. */
        for (u16 i = 0; i < p->cmdline_len; i++)
            print("%c", p->cmdline[i]);
        break;
    }
}

static void gen_pid(int pid, int which, print_fn print)
{
    unsigned long flags;
    struct proc *p;
    if (which == 3) {
        /* The cwd path needs I/O: take a reference, resolve it unlocked. */
        struct inode *cwd = NULL;
        flags = spin_lock_irqsave(&proc_lock);
        p = proc_find(pid);
        if (p && p->cwd)
            cwd = idup(p->cwd);
        spin_unlock_irqrestore(&proc_lock, flags);
        if (cwd) {
            char path[PATH_MAX] = "?";
            vfs_getcwd(cwd, path, sizeof(path));
            iput(cwd);
            print("  cwd %s\n", path);
        }
    }
    flags = spin_lock_irqsave(&proc_lock);
    p = proc_find(pid);
    if (p)
        gen_pid_locked(p, which, print);
    spin_unlock_irqrestore(&proc_lock, flags);
}

static int proc_open(struct file *f)
{
    u32 ino = f->ip->ino;
    char *buf = kmalloc(SEQ_MAX);
    if (!buf)
        return -ENOMEM;
    mutex_lock(&gen_lock);
    seq_buf = buf;
    seq_len = 0;
    if (ino >= 2 && ino < 2 + NROOT) {
        root_files[ino - 2].gen(seq_print);
    } else if (ino > NET_DIR && ino <= NET_DIR + NNET) {
        net_files[ino - NET_DIR - 1].gen(seq_print);
    } else if (ino >= PID_BASE) {
        gen_pid((int)((ino - PID_BASE) / PID_FILES), (int)((ino - PID_BASE) % PID_FILES), seq_print);
    }
    f->priv_len = seq_len;
    seq_buf = NULL;
    mutex_unlock(&gen_lock);
    f->priv = buf;
    return 0;
}

static long proc_read(struct file *f, void *buf, size_t n, bool user)
{
    if (f->off >= f->priv_len)
        return 0;
    n = MIN(n, f->priv_len - f->off);
    if (user ? copy_to_user((uintptr_t)buf, (char *)f->priv + f->off, n)
             : (memcpy(buf, (char *)f->priv + f->off, n), 0))
        return -EFAULT;
    f->off += n;
    return n;
}

static void proc_close(struct file *f)
{
    kfree(f->priv);
}

/* Root files that may also be written (mode 0644: by root). */
static const struct {
    const char *name;
    long (*write)(const char *buf, size_t n);
} root_writers[] = {
    { "drop_caches", write_drop_caches },
};

static long (*writer(u32 ino))(const char *, size_t)
{
    if (ino < 2 || ino >= 2 + NROOT)
        return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(root_writers); i++)
        if (!strcmp(root_writers[i].name, root_files[ino - 2].name))
            return root_writers[i].write;
    return NULL;
}

static long proc_write(struct file *f, const void *buf, size_t n, bool user)
{
    long (*w)(const char *, size_t) = writer(f->ip->ino);
    if (!w)
        return -EACCES;
    char k[64];
    size_t c = MIN(n, sizeof(k) - 1);
    if (user ? copy_from_user(k, (uintptr_t)buf, c) : (memcpy(k, buf, c), 0))
        return -EFAULT;
    k[c] = 0;
    return w(k, n);
}

static const struct chr_ops proc_file_ops = { .open = proc_open, .read = proc_read, .close = proc_close };
static const struct chr_ops proc_rw_ops = { .open = proc_open, .read = proc_read, .write = proc_write,
                                            .close = proc_close };

/* ---- the filesystem ---- */

static int pr_read_inode(struct inode *ip)
{
    u32 ino = ip->ino;
    if (ino == PROC_ROOT || ino == NET_DIR ||
        (ino >= PID_BASE && (ino - PID_BASE) % PID_FILES == 0)) {
        if (ino >= PID_BASE && !proc_exists((int)((ino - PID_BASE) / PID_FILES)))
            return -ENOENT;
        ip->type = I_DIR;
        ip->nlink = 2;
        ip->mode = 0555;
        return 0;
    }
    ip->type = I_FILE;
    ip->nlink = 1;
    bool rw = writer(ino) != NULL;
    ip->mode = rw ? 0644 : 0444;
    ip->cops = rw ? &proc_rw_ops : &proc_file_ops;
    return 0;
}

static u32 pr_lookup(struct inode *dir, const char *name)
{
    if (!strcmp(name, "."))
        return dir->ino;
    if (!strcmp(name, ".."))
        return PROC_ROOT;
    if (dir->ino == PROC_ROOT) {
        for (u32 i = 0; i < NROOT; i++)
            if (!strcmp(name, root_files[i].name))
                return 2 + i;
        if (!strcmp(name, "net"))
            return NET_DIR;
        if (!strcmp(name, "self") && current_proc())
            return pid_dir(current_proc()->pid);
        const char *end;
        unsigned long pid = kstrtoul(name, &end, 10);
        if (!*end && proc_exists((int)pid))
            return pid_dir((int)pid);
        return 0;
    }
    if (dir->ino == NET_DIR) {
        for (u32 i = 0; i < NNET; i++)
            if (!strcmp(name, net_files[i].name))
                return NET_DIR + 1 + i;
        return 0;
    }
    for (u32 i = 1; i < NPIDF; i++)
        if (!strcmp(name, pid_files[i]))
            return dir->ino + i;
    return 0;
}

static int pr_readdir(struct inode *dir, u64 *off, struct kdirent *de)
{
    u64 i = (*off)++;
    if (i < 2) {
        de->ino = i ? PROC_ROOT : dir->ino;
        de->type = DT_DIR;
        strlcpy(de->name, i ? ".." : ".", sizeof(de->name));
        return 1;
    }
    i -= 2;
    if (dir->ino == NET_DIR) {
        if (i >= NNET)
            return 0;
        de->ino = NET_DIR + 1 + (u32)i;
        de->type = DT_REG;
        strlcpy(de->name, net_files[i].name, sizeof(de->name));
        return 1;
    }
    if (dir->ino != PROC_ROOT) {
        if (i + 1 >= NPIDF)
            return 0;
        de->ino = dir->ino + 1 + (u32)i;
        de->type = DT_REG;
        strlcpy(de->name, pid_files[i + 1], sizeof(de->name));
        return 1;
    }
    if (i < NROOT) {
        de->ino = 2 + (u32)i;
        de->type = DT_REG;
        strlcpy(de->name, root_files[i].name, sizeof(de->name));
        return 1;
    }
    if (i == NROOT) {
        de->ino = NET_DIR;
        de->type = DT_DIR;
        strlcpy(de->name, "net", sizeof(de->name));
        return 1;
    }
    /* Then one directory per process, by increasing pid. */
    int after = (int)(i - NROOT - 1);   /* index among processes */
    int pid = proc_nth_pid(after);
    if (!pid)
        return 0;
    de->ino = pid_dir(pid);
    de->type = DT_DIR;
    ksnprintf(de->name, sizeof(de->name), "%d", pid);
    return 1;
}

static const struct fs_ops proc_ops = {
    .read_inode = pr_read_inode,
    .lookup = pr_lookup,
    .readdir = pr_readdir,
};

static int pr_mount(struct super_block *sb)
{
    sb->root = iget(sb, PROC_ROOT);
    return 0;
}

const struct fs_type procfs_type = { .name = "procfs", .mount = pr_mount, .ops = &proc_ops };
