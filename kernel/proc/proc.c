/*
 * Processes: creation, fork, exec, exit, wait, kill, and fault reports.
 *
 * Each process has one thread (thread->proc). The process struct outlives
 * its thread as a zombie until the parent's wait4 (or a kernel waiter)
 * collects the exit status. Orphans are reparented to init (pid 1).
 */
#define LOG_SUBSYS SUBSYS_PROC
#include <console.h>
#include <debug.h>
#include <fs.h>
#include <fmt.h>
#include <kstring.h>
#include <platform.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <trace.h>
#include <trap.h>
#include <uapi/errno.h>
#include <uapi/ptrace.h>
#include <uapi/syscall.h>

#define MAX_ARGS     32
#define MAX_ARG_LEN  4096       /* total bytes of argument strings */

/*
 * proc_lock protects: the process list and parent/child links, process
 * state (alive/zombie, stopped), signal state (pending bits, stop/continue),
 * process groups and sessions, and the p->mm / p->exe / p->thread pointers.
 * A process is freed only by whoever reaps it, under proc_lock, so holding
 * proc_lock keeps every process in the list alive: kill() and procfs rely on
 * that. Lock order: proc_lock before wait queue locks and sched_lock.
 */
LIST_HEAD(all_procs);
spinlock_t proc_lock = SPINLOCK_INIT("proc");
static int next_pid = 1;
static struct proc *initproc;
static bool init_has_kernel_waiter;

void proc_init(void)
{
    pr_info("processes: user space %#lx-%#lx, stack %lu KiB at %#lx\n",
            USER_MIN_ADDR, USER_TOP, USER_STACK_SIZE >> 10, USER_STACK_TOP);
}

static struct proc *proc_alloc(const char *name, struct proc *parent)
{
    struct proc *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    strlcpy(p->name, name, sizeof(p->name));
    p->state = P_ALIVE;
    p->started = rdtime();
    list_init(&p->children);
    waitqueue_init(&p->child_wq, "wait4");
    waitqueue_init(&p->exit_wq, "proc-exit");
    waitqueue_init(&p->stop_wq, "stopped");

    unsigned long flags = spin_lock_irqsave(&proc_lock);
    p->pid = next_pid++;
    p->parent = parent;
    if (parent)
        list_add_tail(&p->sibling, &parent->children);
    list_add_tail(&p->all_node, &all_procs);
    spin_unlock_irqrestore(&proc_lock, flags);
    return p;
}

/* Remove a zombie from all lists and free it (proc_lock held by caller). */
static void proc_unlink(struct proc *p)
{
    if (p->parent)
        list_del(&p->sibling);
    list_del(&p->all_node);
}

/* Caller holds proc_lock (or the world is stopped). */
struct proc *proc_find(int pid)
{
    struct proc *p;
    list_for_each_entry(p, &all_procs, all_node)
        if (p->pid == pid)
            return p;
    return NULL;
}

/* ---- program images ---- */

/*
 * A program's file contents, kept for the life of the processes running it
 * (shared across fork) so the kernel can symbolize user backtraces.
 */
static struct exec_image *image_load(const char *path, int *err, u16 *mode, u16 *uid, u16 *gid)
{
    struct inode *ip = namei(path, err);
    if (!ip)
        return NULL;
    ilock(ip);
    int perm = ip->type != I_FILE ? -EACCES : vfs_permission(ip, X_OK);
    *mode = ip->mode;
    *uid = ip->uid;
    *gid = ip->gid;
    iunlockput(ip);
    if (perm) {
        *err = perm;
        return NULL;
    }
    size_t size;
    void *data = vfs_read_all(path, &size, err);
    if (!data)
        return NULL;
    struct exec_image *img = kzalloc(sizeof(*img));
    if (!img) {
        kfree(data);
        *err = -ENOMEM;
        return NULL;
    }
    img->refcnt = 1;
    img->data = data;
    img->size = size;
    strlcpy(img->path, path, sizeof(img->path));
    return img;
}

static void image_put(struct exec_image *img)
{
    if (img && __atomic_sub_fetch(&img->refcnt, 1, __ATOMIC_ACQ_REL) == 0) {
        kfree(img->data);
        kfree(img);
    }
}

/* Swap in a new program image (procfs may be reading the old one). */
static void set_image(struct proc *p, struct exec_image *img)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct exec_image *old = p->exe;
    p->exe = img;
    p->image = img ? img->data : NULL;
    p->image_size = img ? img->size : 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    image_put(old);
}

/* pid of the n-th process by increasing pid, or 0 (procfs directory listings). */
int proc_nth_pid(int n)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *p = proc_nth(n);
    int pid = p ? p->pid : 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    return pid;
}

bool proc_exists(int pid)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    bool found = proc_find(pid) != NULL;
    spin_unlock_irqrestore(&proc_lock, flags);
    return found;
}

/* The n-th process by increasing pid. Caller holds proc_lock. */
struct proc *proc_nth(int n)
{
    int last = 0;
    for (int i = 0; i <= n; i++) {
        struct proc *best = NULL, *q;
        list_for_each_entry(q, &all_procs, all_node)
            if (q->pid > last && (!best || q->pid < best->pid))
                best = q;
        if (!best)
            return NULL;
        if (i == n)
            return best;
        last = best->pid;
    }
    return NULL;
}

static const char *basename(const char *path)
{
    const char *slash = NULL;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            slash = s;
    return slash ? slash + 1 : path;
}

/* ---- exec ---- */

/*
 * Replace the calling process's image. Must run in p's own thread. On
 * success the user trap frame is set up to start the program; returns argc
 * (which becomes a0). On failure the old image is untouched.
 */
static long do_exec(struct proc *p, const char *path, char *const argv[], char *const envp[])
{
    char err[96];
    int e;
    u16 fmode, fuid, fgid;
    struct exec_image *img = image_load(path, &e, &fmode, &fuid, &fgid);
    if (!img) {
        pr_debug("exec %s: %s\n", path, errno_name(-e));
        return e == -EISDIR ? -EACCES : e;
    }
    struct mm *mm = mm_create();
    if (!mm) {
        image_put(img);
        return -ENOMEM;
    }

    uintptr_t entry, end;
    if (elf_load(mm, img->data, img->size, &entry, &end, err, sizeof(err))) {
        pr_warn("exec %s: %s\n", path, err);
        mm_destroy(mm);
        image_put(img);
        return -ENOEXEC;
    }
    if (vma_add(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP,
                VM_READ | VM_WRITE, VMA_STACK, "stack")) {
        mm_destroy(mm);
        image_put(img);
        return -ENOMEM;
    }
    mm->brk_start = mm->brk = PAGE_ALIGN(end);

    /*
     * Initial stack: strings at the top, then
     *   argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, AT_NULL (0, 0)
     */
    int argc = 0, envc = 0;
    while (argv[argc])
        argc++;
    while (envp && envp[envc])
        envc++;
    uintptr_t sp = USER_STACK_TOP;
    u64 vec[1 + MAX_ARGS + 1 + MAX_ARGS + 1 + 2];
    int nvec = 0;
    vec[nvec++] = argc;
    for (int k = 0; k < 2; k++) {
        char *const *strs = k ? envp : argv;
        int n = k ? envc : argc;
        for (int i = 0; i < n; i++) {
            size_t len = strlen(strs[i]) + 1;
            sp -= len;
            if (uvm_write(mm, sp, strs[i], len))
                goto nomem;
            vec[nvec++] = sp;
        }
        vec[nvec++] = 0;
    }
    vec[nvec++] = 0;                    /* AT_NULL */
    vec[nvec++] = 0;
    sp = ALIGN_DOWN(sp - nvec * 8, 16);
    if (uvm_write(mm, sp, vec, nvec * 8))
        goto nomem;

    /* Commit: from here on, the new image replaces the old one. */
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct mm *old = p->mm;
    p->mm = mm;                         /* procfs readers hold proc_lock */
    spin_unlock_irqrestore(&proc_lock, flags);
    vm_activate(mm);
    mm_destroy(old);
    strlcpy(p->name, basename(path), sizeof(p->name));
    strlcpy(current->name, p->name, sizeof(current->name));
    p->cmdline_len = 0;
    for (int i = 0; i < argc && p->cmdline_len < sizeof(p->cmdline); i++) {
        size_t room = sizeof(p->cmdline) - p->cmdline_len;
        size_t len = strlen(argv[i]) + 1;
        if (len > room)
            len = room;
        memcpy(p->cmdline + p->cmdline_len, argv[i], len);
        p->cmdline_len += len;
    }
    if (p->cmdline_len)
        p->cmdline[p->cmdline_len - 1] = 0;
    set_image(p, img);
    signal_exec(p);
    if (fmode & S_ISUID)
        p->euid = p->suid = fuid;
    if (fmode & S_ISGID)
        p->egid = p->sgid = fgid;
    files_exec(p);                      /* close FD_CLOEXEC descriptors */

    struct trap_frame *tf = thread_user_tf(current);
    memset(tf, 0, sizeof(*tf));
    tf->sepc = entry;
    tf->regs[2] = sp;
    tf->regs[10] = argc;
    tf->regs[11] = sp + 8;
    tf->regs[12] = sp + 8 + (argc + 1) * 8;     /* envp */
    /* User mode, interrupts on after sret, no SUM, FP off until the program uses it. */
    tf->sstatus = (csr_read(sstatus) & ~(SSTATUS_SPP | SSTATUS_SIE | SSTATUS_SUM | SSTATUS_FS))
                  | SSTATUS_SPIE;
    current->fp_used = false;

    u64 n[2] = { 0, 0 };
    memcpy(n, p->name, sizeof(n));
    trace(proc_exec, p->pid, n[0], n[1]);
    pr_debug("pid %d exec %s: entry %#lx, %d args\n", p->pid, path, (unsigned long)entry, argc);
    if (p->tracer) {
        /* the debugger gets control before the first instruction */
        p->si_signo = SIGTRAP;
        p->si_code = TRAP_EXEC;
        p->si_addr = entry;
        signal_send(p, SIGTRAP, p->pid);
    }
    return argc;

nomem:
    mm_destroy(mm);
    image_put(img);
    return -ENOMEM;
}

/* Copy a NULL-terminated user string vector into strings[*used...]. */
static long copy_strv(uintptr_t uvec, char **out, char *strings, size_t *used)
{
    int n = 0;
    for (; uvec; n++) {
        u64 uptr;
        if (n == MAX_ARGS)
            return -E2BIG;
        if (copy_from_user(&uptr, uvec + n * 8, 8))
            return -EFAULT;
        if (!uptr)
            break;
        long r = strncpy_from_user(strings + *used, uptr, MAX_ARG_LEN - *used);
        if (r < 0)
            return r == -ENAMETOOLONG ? -E2BIG : r;
        out[n] = strings + *used;
        *used += r + 1;
    }
    out[n] = NULL;
    return n;
}

long sys_execve(uintptr_t upath, uintptr_t uargv, uintptr_t uenvp)
{
    char path[128];
    char *argv[MAX_ARGS + 1], *envp[MAX_ARGS + 1];
    long r = strncpy_from_user(path, upath, sizeof(path));
    if (r < 0)
        return r;

    char *strings = kmalloc(MAX_ARG_LEN);
    if (!strings)
        return -ENOMEM;
    size_t used = 0;
    r = copy_strv(uargv, argv, strings, &used);
    if (r >= 0) {
        if (r == 0) {
            argv[0] = (char *)basename(path);
            argv[1] = NULL;
        }
        r = copy_strv(uenvp, envp, strings, &used);
    }
    if (r >= 0)
        r = do_exec(current_proc(), path, argv, envp);
    kfree(strings);
    return r;
}

/* ---- creation ---- */

struct spawn_args {
    char path[128];
    int argc;
    char *argv[MAX_ARGS + 1];
    char strings[512];
};

static void user_entry(void *arg)
{
    struct spawn_args *sa = arg;
    struct proc *p = current_proc();
    static char *const env[] = { "PATH=/bin", "HOME=/", "TERM=vt100", NULL };
    long r = do_exec(p, sa->path, sa->argv, env);
    if (r < 0) {
        pr_err("cannot start %s: %s\n", sa->path, errno_name(-r));
        kfree(sa);
        proc_exit(WEXITSTATUS_ENC(127));
    }
    kfree(sa);
    intr_off();
    user_return(thread_user_tf(current));
}

struct proc *proc_spawn(const char *path, char *const argv[])
{
    struct spawn_args *sa = kzalloc(sizeof(*sa));
    if (!sa)
        return NULL;
    strlcpy(sa->path, path, sizeof(sa->path));
    size_t used = 0;
    for (int i = 0; argv && argv[i] && i < MAX_ARGS; i++) {
        size_t len = strlen(argv[i]) + 1;
        if (used + len > sizeof(sa->strings))
            break;
        memcpy(sa->strings + used, argv[i], len);
        sa->argv[sa->argc++] = sa->strings + used;
        used += len;
    }
    if (!sa->argc)
        sa->argv[sa->argc++] = (char *)basename(sa->path);

    struct proc *p = proc_alloc(basename(path), NULL);
    struct thread *t = p ? kthread_create_suspended(p->name, user_entry, sa) : NULL;
    if (!t) {
        kfree(sa);
        kfree(p);
        return NULL;
    }
    if (p->pid == 1) {
        initproc = p;
        init_has_kernel_waiter = false;
    }
    /* stdin, stdout, stderr on the console; cwd at the root. */
    for (int fd = 0; fd < 3; fd++) {
        int err;
        struct file *f = vfs_open("/dev/console", O_RDWR, 0, &err);
        if (!f)
            panic("cannot open /dev/console for %s: %s (is /dev mounted?)", path, errno_name(-err));
        p->files[fd] = f;
    }
    p->cwd = idup(root_inode);
    p->umask = 022;
    p->pgid = p->sid = p->pid;          /* a new session, the console its terminal */
    tty_set_ctty(p);
    t->proc = p;
    p->thread = t;
    kthread_detach(t);
    kthread_start(t);
    return p;
}

static void fork_child_entry(void *arg)
{
    (void)arg;
    intr_off();
    user_return(thread_user_tf(current));
}

long sys_fork(void)
{
    struct proc *parent = current_proc();
    struct proc *child = proc_alloc(parent->name, parent);
    if (!child)
        return -ENOMEM;
    child->mm = mm_fork(parent->mm);
    struct thread *t = child->mm ? kthread_create_suspended(parent->name, fork_child_entry, NULL)
                                 : NULL;
    if (!t) {
        unsigned long flags = spin_lock_irqsave(&proc_lock);
        proc_unlink(child);
        spin_unlock_irqrestore(&proc_lock, flags);
        if (child->mm)
            mm_destroy(child->mm);
        kfree(child);
        return -ENOMEM;
    }
    if (parent->exe)
        __atomic_add_fetch(&parent->exe->refcnt, 1, __ATOMIC_RELAXED);
    child->exe = parent->exe;
    child->image = parent->image;
    child->image_size = parent->image_size;
    child->strace = parent->strace == 2 ? 2 : 0;
    child->uid = parent->uid, child->euid = parent->euid, child->suid = parent->suid;
    child->gid = parent->gid, child->egid = parent->egid, child->sgid = parent->sgid;
    child->umask = parent->umask;
    memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
    child->cmdline_len = parent->cmdline_len;
    files_fork(child, parent);
    signal_fork(child, parent);

    struct trap_frame *tf = thread_user_tf(t);
    memcpy(tf, thread_user_tf(current), sizeof(*tf));
    tf->regs[10] = 0;                   /* fork() returns 0 in the child */
    if (current->fp_used) {             /* the child gets a copy of our FP registers */
        unsigned long fl = csr_read(sstatus) & SSTATUS_SIE;
        intr_off();
        fp_save_regs(&current->fp);
        if (fl)
            intr_on();
        t->fp = current->fp;
        t->fp_used = true;
    }
    t->proc = child;
    child->thread = t;
    kthread_detach(t);
    kthread_start(t);
    return child->pid;
}

/* ---- exit and wait ---- */

void proc_exit(int status)
{
    struct proc *p = current_proc();
    if (!p)
        panic("proc_exit from a kernel thread");
    if (p == initproc && !init_has_kernel_waiter)
        panic("init (pid 1) exited with status %#x", status);

    trace(proc_exit, p->pid, status, 0);
    pr_debug("pid %d (%s) exits with status %#x\n", p->pid, p->name, status);

    /*
     * Detach the address space first: if we are preempted while destroying
     * it, the scheduler must not switch back onto it (it would re-activate a
     * page table that is being freed).
     */
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct mm *mm = p->mm;
    p->mm = NULL;
    spin_unlock_irqrestore(&proc_lock, flags);
    vm_activate(NULL);
    mm_destroy(mm);
    files_close_all(p);
    set_image(p, NULL);

    /*
     * Become a zombie and notify, all under proc_lock: the parent may reap
     * (free) us as soon as it can take the lock, so nothing here may touch
     * p afterwards. The parent itself can't go away meanwhile either.
     */
    bool wake_init = false;
    flags = spin_lock_irqsave(&proc_lock);
    ptrace_tracer_exit(p);
    struct proc *c, *tmp;
    list_for_each_entry_safe(c, tmp, &p->children, sibling) {
        list_del(&c->sibling);
        c->parent = initproc != p ? initproc : NULL;
        if (c->parent) {
            list_add_tail(&c->sibling, &c->parent->children);
            wake_init |= c->state == P_ZOMBIE;
        }
    }
    p->exit_status = status;
    p->state = P_ZOMBIE;
    p->thread = NULL;
    struct proc *parent = p->parent;
    if (parent) {
        signal_send_locked(parent, SIGCHLD, p->pid);
        wake_up(&parent->child_wq);
    }
    if (p->tracer && p->tracer != parent) {
        p->ptrace_exit_report = true;
        signal_send_locked(p->tracer, SIGCHLD, p->pid);
        wake_up(&p->tracer->child_wq);
    }
    if (wake_init && initproc)
        wake_up(&initproc->child_wq);
    wake_up(&p->exit_wq);
    current->proc = NULL;
    spin_unlock_irqrestore(&proc_lock, flags);
    thread_exit(status);
}

static bool child_matches(struct proc *c, long pid)
{
    return pid == -1 || (pid > 0 && c->pid == pid) || (pid == 0 && c->pgid == current_proc()->pgid) ||
           (pid < -1 && c->pgid == -pid);
}

/*
 * What wait4 reports about a child c of current: its exit, a job-control
 * stop (with WUNTRACED), or a tracing stop if we are its tracer. A tracing
 * stop is the tracer's business: the parent doesn't see it.
 */
static bool reportable(struct proc *c, long options)
{
    if (c->state == P_ZOMBIE)
        return true;
    if (!c->stopped || c->stop_reported)
        return false;
    if (c->ptrace_stopped)
        return c->tracer == current_proc();
    return options & WUNTRACED;
}

/* A process we trace without being its parent: its tracing stops and its exit. */
static bool tracee_reportable(struct proc *c)
{
    return (c->state == P_ZOMBIE && c->ptrace_exit_report) ||
           (c->state == P_ALIVE && c->ptrace_stopped && !c->stop_reported);
}

/* proc_lock held: a child or foreign tracee to report; *any: there is something to wait for */
static struct proc *find_reportable(struct proc *p, long pid, long options, bool *any)
{
    struct proc *c;
    *any = false;
    list_for_each_entry(c, &p->children, sibling) {
        if (!child_matches(c, pid))
            continue;
        *any = true;
        if (c->state == P_ZOMBIE && c->ptrace_exit_report && c->tracer && c->tracer != p)
            continue;               /* its tracer hears of the exit first, as in Linux */
        if (reportable(c, options))
            return c;
    }
    list_for_each_entry(c, &all_procs, all_node) {
        if (c->tracer != p || c->parent == p || !child_matches(c, pid))
            continue;
        *any = true;
        if (tracee_reportable(c))
            return c;
    }
    return NULL;
}

static bool has_zombie(struct proc *p, long pid, long options)
{
    bool any;
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    bool found = find_reportable(p, pid, options, &any) != NULL;
    spin_unlock_irqrestore(&proc_lock, flags);
    return found;
}

long sys_wait4(long pid, uintptr_t ustatus, long options, uintptr_t rusage)
{
    (void)rusage;
    struct proc *p = current_proc();
    for (;;) {
        bool any;
        unsigned long flags = spin_lock_irqsave(&proc_lock);
        struct proc *z = find_reportable(p, pid, options, &any);
        if (z && z->parent != p && z->state == P_ZOMBIE) {
            /* a tracee that isn't our child exited: tell us, then its parent may reap it */
            z->ptrace_exit_report = false;
            z->tracer = NULL;
            if (z->parent)
                wake_up(&z->parent->child_wq);
            int status = z->exit_status, cpid = z->pid;
            spin_unlock_irqrestore(&proc_lock, flags);
            if (ustatus && copy_to_user(ustatus, &status, sizeof(status)))
                return -EFAULT;
            return cpid;
        }
        if (z && z->state != P_ZOMBIE) {        /* stopped: report, keep it */
            z->stop_reported = true;
            int status = WSTOPPED_ENC(z->stop_sig), cpid = z->pid;
            spin_unlock_irqrestore(&proc_lock, flags);
            if (ustatus && copy_to_user(ustatus, &status, sizeof(status)))
                return -EFAULT;
            return cpid;
        }
        if (z)
            proc_unlink(z);
        spin_unlock_irqrestore(&proc_lock, flags);

        if (z) {
            int status = z->exit_status, cpid = z->pid;
            kfree(z);
            if (ustatus && copy_to_user(ustatus, &status, sizeof(status)))
                return -EFAULT;
            return cpid;
        }
        if (!any)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        current->wait_what = "wait4";
        if (wait_event_interruptible(&p->child_wq, has_zombie(p, pid, options)))
            return -ERESTARTSYS;
    }
}

int proc_wait_kernel(struct proc *p)
{
    if (p == initproc)
        init_has_kernel_waiter = true;
    wait_event(&p->exit_wq, p->state == P_ZOMBIE);
    int status = p->exit_status;
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    proc_unlink(p);
    spin_unlock_irqrestore(&proc_lock, flags);
    if (p == initproc)
        initproc = NULL;
    kfree(p);
    return status;
}

/* ---- debugging ---- */

int proc_user_backtrace(print_fn print, struct proc *p, uintptr_t pc, uintptr_t fp)
{
    int n = 0, repeats = 0;
    uintptr_t last = 0;
    for (; n < 64; n++) {
        uintptr_t off;
        if (n > 1 && pc == last) {         /* collapse recursion */
            repeats++;
            goto next;
        }
        if (repeats) {
            print("    ... frame repeated %d more times (recursion)\n", repeats);
            repeats = 0;
        }
        last = pc;
        const char *sym = elf_symbolize(p->image, p->image_size, n ? pc - 1 : pc, &off);
        if (sym)
            print("    #%-2d %#010lx  %s+%#lx\n", n, (unsigned long)pc, sym,
                  (unsigned long)(n ? off + 1 : off));
        else
            print("    #%-2d %#010lx  ?\n", n, (unsigned long)pc);
    next:;
        u64 rec[2];                         /* {prev fp, ra} at fp-16 */
        if (!p->mm || !fp || (fp & 7) || uvm_read(p->mm, fp - 16, rec, sizeof(rec)))
            break;
        if (!rec[1] || rec[0] <= fp)
            break;
        pc = rec[1];
        fp = rec[0];
    }
    if (repeats)
        print("    ... frame repeated %d more times (recursion; truncated)\n", repeats);
    return n;
}

static int err_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_ERR, SUBSYS_PROC, fmt, ap);
    va_end(ap);
    return 0;
}

/* A user program did something fatal: explain it, then kill the process. */
void user_fault(struct trap_frame *tf, const char *what, int sig)
{
    struct proc *p = current_proc();
    u32 code = SCAUSE_CODE(tf->scause);
    p->si_signo = sig;
    p->si_addr = code == EXC_BREAKPOINT || code == EXC_ILLEGAL_INSN ? tf->sepc : tf->stval;
    p->si_code = sig == SIGSEGV && p->mm && vma_find(p->mm, tf->stval) ? SEGV_ACCERR : 1;
    if (signal_handled(p, sig) || p->tracer) {
        pr_debug("pid %d (%s): %s at %#lx -> %s handler\n", p->pid, p->name, what,
                 (unsigned long)tf->stval, signal_name(sig));
        signal_send(p, sig, p->pid);
        return;                         /* delivered on the way back to user mode */
    }
    uintptr_t addr = tf->stval;
    struct vma *v = p->mm ? vma_find(p->mm, addr) : NULL;
    char why[96] = "";

    if (SCAUSE_CODE(tf->scause) >= EXC_INSN_PAGE_FAULT && p->mm) {
        struct vma *stack = NULL, *it;
        list_for_each_entry(it, &p->mm->vmas, node)
            if (it->kind == VMA_STACK)
                stack = it;
        if (addr < PAGE_SIZE)
            ksnprintf(why, sizeof(why), "NULL pointer dereference");
        else if (addr >= USER_TOP)
            ksnprintf(why, sizeof(why), "kernel address: user code cannot touch kernel memory");
        else if (stack && addr < stack->start && addr >= stack->start - (1UL << 20))
            ksnprintf(why, sizeof(why), "stack overflow (just below the %lu KiB stack)",
                      (stack->end - stack->start) >> 10);
        else if (v)
            ksnprintf(why, sizeof(why), "VMA '%s' is %c%c%c", v->name,
                      v->prot & VM_READ ? 'r' : '-', v->prot & VM_WRITE ? 'w' : '-',
                      v->prot & VM_EXEC ? 'x' : '-');
        else
            ksnprintf(why, sizeof(why), "address not in any mapping");
    }

    uintptr_t off;
    const char *sym = elf_symbolize(p->image, p->image_size, tf->sepc, &off);
    if (SCAUSE_CODE(tf->scause) == EXC_ILLEGAL_INSN)
        klog(LOG_ERR, SUBSYS_PROC, "pid %d (%s) killed by %s: %s (instruction bits %#lx)\n",
             p->pid, p->name, signal_name(sig), what, (unsigned long)tf->stval);
    else
        klog(LOG_ERR, SUBSYS_PROC, "pid %d (%s) killed by %s: %s: %s at %#lx%s%s\n",
             p->pid, p->name, signal_name(sig), trap_cause_name(tf->scause), what,
             (unsigned long)addr, why[0] ? " -- " : "", why);
    klog(LOG_ERR, SUBSYS_PROC, "  pc %#lx <%s+%#lx>  ra %#lx  sp %#lx\n",
         (unsigned long)tf->sepc, sym ? sym : "?", (unsigned long)(sym ? off : 0),
         (unsigned long)TF_RA(tf), (unsigned long)TF_SP(tf));
    klog(LOG_ERR, SUBSYS_PROC, "  user backtrace:\n");
    proc_user_backtrace(err_print, p, tf->sepc, TF_FP(tf));
    klog(LOG_ERR, SUBSYS_PROC, "  memory map:\n");
    mm_print(err_print, p->mm);
    proc_exit(WSIGNAL_ENC(sig) | WCOREFLAG);
}

void proc_list_print(print_fn print)
{
    struct proc *p;
    u64 tb = platform.timebase ? platform.timebase : 10000000;
    unsigned long flags;
    bool locked = spin_lock_debug(&proc_lock, &flags);
    print("  PID  PPID PGID  SID NAME             STATE  TID  SYSCALLS  RSS KiB  STRACE  "
          "SIGPEND  SIGBLK   AGE s\n");
    list_for_each_entry(p, &all_procs, all_node) {
        print("  %3d  %4d %4d %4d %-16s %-6s %4d %9lu %8lu  %-6s  %08x %08x %lu\n", p->pid,
              p->parent ? p->parent->pid : 0, p->pgid, p->sid, p->name,
              p->state == P_ZOMBIE ? "zombie" : p->ptrace_stopped ? "t" : p->stopped ? "T" :
              p->thread ? thread_state_name(p->thread->state) : "?",
              p->thread ? p->thread->tid : -1, (unsigned long)p->syscalls,
              (unsigned long)(p->mm ? p->mm->rss * 4 : 0),
              p->strace == 2 ? "follow" : p->strace ? "on" : "off", p->sig_pending,
              p->sig_blocked, (unsigned long)((rdtime() - p->started) / tb));
    }
    spin_unlock_debug(&proc_lock, locked, flags);
}
