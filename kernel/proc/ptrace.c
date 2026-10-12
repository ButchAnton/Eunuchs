/*
 * ptrace: the kernel side of a debugger (user/bin/db).
 *
 * A traced process stops whenever a signal is about to be delivered to it
 * (signal_deliver -> ptrace_stop), including the SIGTRAP sent after a
 * successful exec and the one an ebreak raises. Its tracer sees the stop
 * with wait4 (WIFSTOPPED, without needing WUNTRACED), looks at and changes
 * its registers and memory, and resumes it with PTRACE_CONT, choosing which
 * signal (if any) gets delivered. There is no single-step: RISC-V has no
 * trace trap, so the debugger steps by planting breakpoints at the possible
 * next instructions.
 *
 * Locking: tracer, ptrace_stopped and ptrace_resume_sig change under
 * proc_lock. A stopped tracee is asleep in ptrace_stop and doesn't touch its
 * registers or memory; the tracer reads and writes them under proc_lock,
 * which also keeps the tracee (and its mm) from going away meanwhile.
 * Copies to and from the tracer's own memory happen with the lock dropped,
 * since they can fault.
 */
#define LOG_SUBSYS SUBSYS_PROC

#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <sched.h>
#include <trace.h>
#include <trap.h>
#include <uapi/errno.h>
#include <uapi/ptrace.h>
#include <uapi/signal.h>

/* The FP registers live in thread->fp while we are stopped, so the tracer
 * reads and writes them there: save them before the stop is announced
 * (the context switch would do it, but only later), and load them after. */
static void fp_to_memory(bool load)
{
    if (!current->fp_used)
        return;
    unsigned long fl = csr_read(sstatus) & SSTATUS_SIE;
    intr_off();
    if (load)
        fp_load_regs(&current->fp);
    else
        fp_save_regs(&current->fp);
    if (fl)
        intr_on();
}

/* Called by the tracee on its way to deliver sig: stop until the tracer resumes us. */
int ptrace_stop(struct proc *p, int sig)
{
    fp_to_memory(false);
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *t = p->tracer;
    if (!t || (p->sig_pending & SIGBIT(SIGKILL))) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return sig;
    }
    p->ptrace_stopped = true;
    p->stopped = true;
    p->stop_sig = sig;
    p->stop_reported = false;
    p->ptrace_resume_sig = 0;
    signal_send_locked(t, SIGCHLD, p->pid);
    wake_up(&t->child_wq);
    spin_unlock_irqrestore(&proc_lock, flags);

    pr_debug("pid %d (%s) stopped for its tracer (pid %d) by %s\n", p->pid, p->name, t->pid,
             signal_name(sig));
    trace(proc_stop, p->pid, sig, 1);
    current->wait_what = "traced";
    wait_event(&p->stop_wq, !p->ptrace_stopped || (p->sig_pending & SIGBIT(SIGKILL)));

    flags = spin_lock_irqsave(&proc_lock);
    p->ptrace_stopped = false;
    p->stopped = false;
    int resume = p->ptrace_resume_sig;
    p->ptrace_resume_sig = 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    fp_to_memory(true);             /* PTRACE_SETFPREGS may have changed them */
    return resume;
}

/* The tracer is exiting (proc_lock held): its tracees carry on untraced. */
void ptrace_tracer_exit(struct proc *tracer)
{
    struct proc *c;
    list_for_each_entry(c, &all_procs, all_node) {
        if (c->tracer != tracer)
            continue;
        c->tracer = NULL;
        if (c->ptrace_exit_report && c->parent)
            wake_up(&c->parent->child_wq);  /* it held back its exit for us */
        c->ptrace_exit_report = false;
        if (c->ptrace_stopped) {
            c->ptrace_stopped = false;
            c->ptrace_resume_sig = 0;
            wake_up(&c->stop_wq);
        }
    }
}

static bool may_attach(struct proc *me, struct proc *c)
{
    if (me->euid == 0)
        return true;
    return c->uid == me->euid && c->euid == me->euid && c->suid == me->euid &&
           c->gid == me->egid && c->egid == me->egid;
}

/* The tracee, found and checked with proc_lock held: traced by me and stopped. */
static struct proc *stopped_tracee(int pid, long *err)
{
    struct proc *c = proc_find(pid);
    if (!c || c->tracer != current_proc() || c->state != P_ALIVE) {
        *err = -ESRCH;
        return NULL;
    }
    if (!c->ptrace_stopped || !c->thread || !c->mm) {
        *err = -ESRCH;              /* as in Linux: it must be stopped */
        return NULL;
    }
    return c;
}

static void resume(struct proc *c, int sig)
{
    c->ptrace_resume_sig = sig;
    c->ptrace_stopped = false;
    wake_up(&c->stop_wq);
}

/* PTRACE_READ / PTRACE_WRITE: copy through a kernel page, a page at a time. */
static long ptrace_io(int pid, uintptr_t udesc, bool write)
{
    struct ptrace_io io;
    if (copy_from_user(&io, udesc, sizeof(io)))
        return -EFAULT;
    if (io.len > (1UL << 20))
        return -EINVAL;
    u8 *bounce = page_alloc();
    if (!bounce)
        return -ENOMEM;
    long r = 0;
    for (u64 done = 0; done < io.len && !r;) {
        size_t chunk = MIN(io.len - done, PAGE_SIZE);
        if (write && copy_from_user(bounce, io.buf + done, chunk)) {
            r = -EFAULT;
            break;
        }
        unsigned long flags = spin_lock_irqsave(&proc_lock);
        struct proc *c = stopped_tracee(pid, &r);
        if (c)
            r = write ? uvm_poke(c->mm, io.addr + done, bounce, chunk)
                      : uvm_peek(c->mm, io.addr + done, bounce, chunk);
        spin_unlock_irqrestore(&proc_lock, flags);
        if (r)
            break;
        if (!write && copy_to_user(io.buf + done, bounce, chunk)) {
            r = -EFAULT;
            break;
        }
        done += chunk;
    }
    page_free(bounce);
    return r == -EFAULT && !write ? -EIO : r;
}

long do_ptrace(const u64 *a)
{
    long req = (long)a[0];
    int pid = (int)a[1];
    uintptr_t addr = a[2], data = a[3];
    struct proc *me = current_proc();
    long r = 0;
    unsigned long flags;

    switch (req) {
    case PTRACE_TRACEME:
        flags = spin_lock_irqsave(&proc_lock);
        if (me->tracer || !me->parent)
            r = -EPERM;
        else
            me->tracer = me->parent;
        spin_unlock_irqrestore(&proc_lock, flags);
        return r;

    case PTRACE_ATTACH: {
        flags = spin_lock_irqsave(&proc_lock);
        struct proc *c = proc_find(pid);
        if (!c || c->state != P_ALIVE)
            r = -ESRCH;
        else if (c == me || c->pid == 1 || c->tracer || !c->mm || !may_attach(me, c))
            r = -EPERM;
        else {
            c->tracer = me;
            signal_send_locked(c, SIGSTOP, me->pid);
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        if (!r)
            pr_debug("pid %d (%s) attached to pid %d\n", me->pid, me->name, pid);
        return r;
    }

    case PTRACE_KILL: {
        flags = spin_lock_irqsave(&proc_lock);
        struct proc *c = proc_find(pid);
        if (!c || c->tracer != me)
            r = -ESRCH;
        else
            signal_send_locked(c, SIGKILL, me->pid);
        spin_unlock_irqrestore(&proc_lock, flags);
        return r;
    }

    case PTRACE_SINGLESTEP:
        return -EIO;

    case PTRACE_READ:
    case PTRACE_WRITE:
        return ptrace_io(pid, data, req == PTRACE_WRITE);
    }

    /* everything else: copy the tracer's argument in first (it may fault) */
    struct user_regs regs;
    struct user_fpregs fpregs;
    if (req == PTRACE_SETREGS && copy_from_user(&regs, data, sizeof(regs)))
        return -EFAULT;
    if (req == PTRACE_SETFPREGS && copy_from_user(&fpregs, data, sizeof(fpregs)))
        return -EFAULT;
    if ((req == PTRACE_CONT || req == PTRACE_DETACH) && data >= NSIG)
        return -EIO;

    u64 word = 0;
    struct ptrace_siginfo si;
    flags = spin_lock_irqsave(&proc_lock);
    struct proc *c = stopped_tracee(pid, &r);
    if (!c) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return r;
    }
    struct thread *t = c->thread;
    struct trap_frame *tf = thread_user_tf(t);
    switch (req) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
        r = uvm_peek(c->mm, addr, &word, sizeof(word)) ? -EIO : 0;
        break;
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        r = uvm_poke(c->mm, addr, &data, sizeof(data)) ? -EIO : 0;
        break;
    case PTRACE_GETREGS:
        regs.r[0] = tf->sepc;
        for (int i = 1; i < 32; i++)
            regs.r[i] = tf->regs[i];
        break;
    case PTRACE_SETREGS:
        tf->sepc = regs.r[0];
        for (int i = 1; i < 32; i++)
            tf->regs[i] = regs.r[i];
        break;
    case PTRACE_GETFPREGS:
        if (t->fp_used) {
            memcpy(fpregs.f, t->fp.f, sizeof(fpregs.f));
            fpregs.fcsr = t->fp.fcsr;
        } else {
            memset(&fpregs, 0, sizeof(fpregs));
        }
        break;
    case PTRACE_SETFPREGS:
        memcpy(t->fp.f, fpregs.f, sizeof(fpregs.f));
        t->fp.fcsr = fpregs.fcsr;
        t->fp_used = true;          /* loaded as ptrace_stop returns ... */
        if (!(tf->sstatus & SSTATUS_FS))
            tf->sstatus |= SSTATUS_FS_INITIAL;  /* ... and usable without a trap */
        break;
    case PTRACE_GETSIGINFO:
        si.si_signo = c->stop_sig;
        si.si_code = c->stop_sig == c->si_signo ? c->si_code : SI_USER;
        si.si_addr = c->stop_sig == c->si_signo ? c->si_addr : 0;
        break;
    case PTRACE_CONT:
        resume(c, (int)data);
        break;
    case PTRACE_DETACH:
        c->tracer = NULL;
        resume(c, (int)data);
        break;
    default:
        r = -EIO;
        break;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    if (r)
        return r;
    if ((req == PTRACE_PEEKTEXT || req == PTRACE_PEEKDATA) && copy_to_user(data, &word, sizeof(word)))
        return -EFAULT;
    if (req == PTRACE_GETREGS && copy_to_user(data, &regs, sizeof(regs)))
        return -EFAULT;
    if (req == PTRACE_GETFPREGS && copy_to_user(data, &fpregs, sizeof(fpregs)))
        return -EFAULT;
    if (req == PTRACE_GETSIGINFO && copy_to_user(data, &si, sizeof(si)))
        return -EFAULT;
    return 0;
}
