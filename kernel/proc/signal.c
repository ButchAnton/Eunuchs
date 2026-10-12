/*
 * Signals.
 *
 * Sending sets a pending bit and wakes the target if it sleeps
 * interruptibly. Delivery happens on the way back to user mode
 * (signal_deliver): default actions run in the kernel; a handler gets a
 * signal frame pushed on the user stack and is entered with a0 = signal,
 * ra = the libc restorer, which calls rt_sigreturn to restore the saved
 * registers and mask.
 *
 * Debugging: frames carry a magic number, so a handler that corrupts its
 * stack is reported ("bad signal frame") instead of resuming at a random pc.
 * Pending/blocked masks are in `procs`, /proc/<pid>/status; strace and the
 * trace (signal_send/signal_deliver) name the signals.
 */
#define LOG_SUBSYS SUBSYS_PROC
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>
#include <uapi/errno.h>
#include <uapi/syscall.h>

#define FRAME_MAGIC 0x5349474652414d45UL    /* "SIGFRAME" */
#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define STOP_SIGS   (SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU))

struct sigframe {
    u64 magic;
    u64 regs[32];
    u64 sepc;
    u32 blocked;                /* mask to restore */
    u32 sig;
    u64 fp_valid;               /* the interrupted code's FP registers follow */
    struct fpstate fp;
};

/*
 * Not __builtin_ctz: that becomes a libgcc call compiled for the medlow code
 * model, whose table lookup cannot reach a higher-half kernel at link time.
 */
static int lowest_bit(u32 v)
{
    int n = 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
}

static char default_action(int sig)
{
    switch (sig) {
#define X(name, num, act, desc) case num: return act;
        SIGNALS(X)
#undef X
    }
    return 'T';
}

const char *signal_name(int sig)
{
    switch (sig) {
#define X(name, num, act, desc) case num: return #name;
        SIGNALS(X)
#undef X
    }
    return "SIG?";
}

bool signal_pending(struct proc *p)
{
    return p && (p->sig_pending & (~p->sig_blocked | UNBLOCKABLE));
}

bool thread_signal_pending(struct thread *t)
{
    return t->proc && signal_pending(t->proc);
}

bool signal_handled(struct proc *p, int sig)
{
    return p->sigactions[sig].handler > (uintptr_t)SIG_IGN && !(p->sig_blocked & SIGBIT(sig));
}

static void wake(struct proc *p)
{
    struct thread *t = p->thread;
    if (t)
        wake_interruptible(t);
    wake_up(&p->stop_wq);
}

/*
 * Signal state changes under proc_lock (see proc.c): the sender's update of
 * the pending bits and stop state must not race with the target dequeuing
 * or stopping on another CPU, and holding the lock keeps the target alive.
 */
int signal_send(struct proc *p, int sig, int from_pid)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    int r = signal_send_locked(p, sig, from_pid);
    spin_unlock_irqrestore(&proc_lock, flags);
    return r;
}

int signal_send_locked(struct proc *p, int sig, int from_pid)
{
    if (!spin_held_by_me(&proc_lock) && !smp_world_stopped() && !monitor_entry_time)
        panic("signal_send_locked without proc_lock");
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    if (!p || p->state != P_ALIVE)
        return -ESRCH;
    if (sig == 0)
        return 0;
    trace(signal_send, p->pid, sig, from_pid);
    pr_debug("signal %s -> pid %d (%s) from pid %d\n", signal_name(sig), p->pid, p->name, from_pid);

    if (sig == SIGCONT || sig == SIGKILL) {
        p->cont_seq++;
        p->sig_pending &= ~STOP_SIGS;
        if (p->stopped && !p->ptrace_stopped) {    /* only the tracer ends a tracing stop */
            p->stopped = false;
            wake_up(&p->stop_wq);
        } else if (p->ptrace_stopped && sig == SIGKILL) {
            wake_up(&p->stop_wq);
        }
    }
    if (STOP_SIGS & SIGBIT(sig))
        p->sig_pending &= ~SIGBIT(SIGCONT);

    uintptr_t h = p->sigactions[sig].handler;
    bool ignored = h == (uintptr_t)SIG_IGN ||
                   (h == (uintptr_t)SIG_DFL && (default_action(sig) == 'I' || default_action(sig) == 'K'));
    if (ignored && !(UNBLOCKABLE & SIGBIT(sig)))
        return 0;                       /* discarded (SIGCONT already did its job) */
    p->sig_pending |= SIGBIT(sig);
    p->signals_received++;
    wake(p);
    return 0;
}

int signal_send_pgrp(int pgid, int sig, int from_pid)
{
    int n = 0;
    struct proc *p;
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    list_for_each_entry(p, &all_procs, all_node)
        if (p->pgid == pgid && p->state == P_ALIVE && signal_send_locked(p, sig, from_pid) == 0)
            n++;
    spin_unlock_irqrestore(&proc_lock, flags);
    return n ? 0 : -ESRCH;
}

/* proc_lock held */
void signal_notify_parent(struct proc *p, int code)
{
    (void)code;
    if (!p->parent)
        return;
    signal_send_locked(p->parent, SIGCHLD, p->pid);
    wake_up(&p->parent->child_wq);
}

void signal_fork(struct proc *child, struct proc *parent)
{
    memcpy(child->sigactions, parent->sigactions, sizeof(child->sigactions));
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->ctty = parent->ctty;
}

/* exec: handlers point into the old image; reset them (ignored stays ignored). */
void signal_exec(struct proc *p)
{
    for (int s = 1; s < NSIG; s++)
        if (p->sigactions[s].handler != (uintptr_t)SIG_IGN)
            memset(&p->sigactions[s], 0, sizeof(p->sigactions[s]));
}

static void do_stop(struct proc *p, int sig, u32 cont_seq)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    if (p->cont_seq != cont_seq) {      /* continued since the stop signal was dequeued */
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }
    p->stopped = true;
    p->stop_sig = sig;
    p->stop_reported = false;
    signal_notify_parent(p, sig);
    spin_unlock_irqrestore(&proc_lock, flags);
    pr_debug("pid %d (%s) stopped by %s\n", p->pid, p->name, signal_name(sig));
    trace(proc_stop, p->pid, sig, 0);
    current->wait_what = "stopped";
    wait_event(&p->stop_wq, !p->stopped || (p->sig_pending & SIGBIT(SIGKILL)));
}

static bool setup_frame(struct trap_frame *tf, int sig, struct ksigaction *act, u32 old_blocked)
{
    struct sigframe f;
    f.magic = FRAME_MAGIC;
    memcpy(f.regs, tf->regs, sizeof(f.regs));
    f.sepc = tf->sepc;
    f.blocked = old_blocked;
    f.sig = sig;
    f.fp_valid = current->fp_used;
    if (f.fp_valid) {
        unsigned long fl = csr_read(sstatus) & SSTATUS_SIE;
        intr_off();
        fp_save_regs(&current->fp);
        if (fl)
            intr_on();
        f.fp = current->fp;
    }
    uintptr_t sp = ALIGN_DOWN(TF_SP(tf) - sizeof(f), 16);
    if (!act->restorer || copy_to_user(sp, &f, sizeof(f)))
        return false;
    tf->regs[2] = sp;
    tf->regs[1] = act->restorer;        /* ra: handler returns into rt_sigreturn */
    tf->regs[10] = sig;
    tf->sepc = act->handler;
    return true;
}

/* An interrupted system call nobody handled: run it again. */
void syscall_restart_check(struct trap_frame *tf)
{
    struct proc *p = current_proc();
    if (p && p->syscall_restart) {
        p->syscall_restart = false;
        tf->regs[10] = p->restart_a0;
        tf->sepc -= 4;                  /* the ecall */
        if (tf->regs[17] == SYS_nanosleep)
            p->nanosleep_restarting = true;
    }
}

void signal_deliver(struct trap_frame *tf)
{
    struct proc *p = current_proc();
    while (p && signal_pending(p)) {
        unsigned long flags = spin_lock_irqsave(&proc_lock);
        u32 deliverable = p->sig_pending & (~p->sig_blocked | UNBLOCKABLE);
        if (!deliverable) {
            spin_unlock_irqrestore(&proc_lock, flags);
            break;
        }
        int sig = lowest_bit(deliverable);
        p->sig_pending &= ~SIGBIT(sig);
        u32 cont_seq = p->cont_seq;
        bool traced = p->tracer != NULL;
        spin_unlock_irqrestore(&proc_lock, flags);
        if (traced && sig != SIGKILL) {
            /* the debugger decides what (if anything) gets delivered */
            sig = ptrace_stop(p, sig);
            if (!sig)
                continue;
            flags = spin_lock_irqsave(&proc_lock);
            cont_seq = p->cont_seq;
            if ((p->sig_blocked & SIGBIT(sig)) && !(UNBLOCKABLE & SIGBIT(sig))) {
                p->sig_pending |= SIGBIT(sig);      /* passed on but blocked: stays pending */
                spin_unlock_irqrestore(&proc_lock, flags);
                continue;
            }
            spin_unlock_irqrestore(&proc_lock, flags);
        }
        struct ksigaction *act = &p->sigactions[sig];

        if (act->handler == (uintptr_t)SIG_IGN && !(UNBLOCKABLE & SIGBIT(sig)))
            continue;
        if (act->handler == (uintptr_t)SIG_DFL || (UNBLOCKABLE & SIGBIT(sig))) {
            switch (default_action(sig)) {
            case 'I':
            case 'K':
                continue;
            case 'S':
                do_stop(p, sig, cont_seq);
                continue;
            case 'C':
                proc_exit(WSIGNAL_ENC(sig) | WCOREFLAG);
            default:
                proc_exit(WSIGNAL_ENC(sig));
            }
        }

        if (p->syscall_restart) {
            p->syscall_restart = false;
            if (act->flags & SA_RESTART) {
                tf->regs[10] = p->restart_a0;   /* re-run the call after the handler */
                tf->sepc -= 4;
            } else {
                tf->regs[10] = (u64)-EINTR;
            }
        }
        u32 old = p->restore_blocked ? p->saved_blocked : p->sig_blocked;
        p->restore_blocked = false;
        trace(signal_deliver, sig, act->handler, 0);
        if (!setup_frame(tf, sig, act, old)) {
            klog(LOG_ERR, SUBSYS_PROC, "pid %d (%s): cannot deliver %s: %s; killing it\n",
                 p->pid, p->name, signal_name(sig),
                 act->restorer ? "user stack not writable (stack overflow?)" : "no restorer");
            proc_exit(WSIGNAL_ENC(SIGSEGV) | WCOREFLAG);
        }
        p->sig_blocked |= act->mask | (act->flags & SA_NODEFER ? 0 : SIGBIT(sig));
        p->sig_blocked &= ~UNBLOCKABLE;
        if (act->flags & SA_RESETHAND)
            memset(act, 0, sizeof(*act));
        return;                         /* one handler at a time */
    }
    if (p && p->restore_blocked) {
        p->sig_blocked = p->saved_blocked;
        p->restore_blocked = false;
    }
}

/* ---- system calls ---- */

long do_kill(const u64 *a)
{
    long pid = (long)a[0];
    int sig = (int)a[1];
    struct proc *self = current_proc();
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    if (pid > 0) {
        unsigned long flags = spin_lock_irqsave(&proc_lock);
        int r = signal_send_locked(proc_find((int)pid), sig, self->pid);
        spin_unlock_irqrestore(&proc_lock, flags);
        return r;
    }
    if (pid == 0)
        return signal_send_pgrp(self->pgid, sig, self->pid);
    if (pid < -1)
        return signal_send_pgrp((int)-pid, sig, self->pid);
    return -EINVAL;                     /* kill(-1): not supported */
}

long do_rt_sigaction(const u64 *a)
{
    struct proc *p = current_proc();
    int sig = (int)a[0];
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    struct ksigaction *k = &p->sigactions[sig];
    if (a[2]) {
        struct sigaction old = { (sighandler_t)k->handler, k->flags,
                                 (void (*)(void))k->restorer, k->mask, 0 };
        if (copy_to_user(a[2], &old, sizeof(old)))
            return -EFAULT;
    }
    if (a[1]) {
        if (UNBLOCKABLE & SIGBIT(sig))
            return -EINVAL;             /* SIGKILL/SIGSTOP cannot be caught or ignored */
        struct sigaction n;
        if (copy_from_user(&n, a[1], sizeof(n)))
            return -EFAULT;
        k->handler = (uintptr_t)n.sa_handler;
        k->flags = n.sa_flags;
        k->restorer = (uintptr_t)n.sa_restorer;
        k->mask = n.sa_mask & ~UNBLOCKABLE;
        if (k->handler == (uintptr_t)SIG_IGN)
            p->sig_pending &= ~SIGBIT(sig);
    }
    return 0;
}

long do_rt_sigprocmask(const u64 *a)
{
    struct proc *p = current_proc();
    u32 set;
    if (a[2] && copy_to_user(a[2], &p->sig_blocked, sizeof(u32)))
        return -EFAULT;
    if (!a[1])
        return 0;
    if (copy_from_user(&set, a[1], sizeof(set)))
        return -EFAULT;
    switch (a[0]) {
    case SIG_BLOCK: p->sig_blocked |= set; break;
    case SIG_UNBLOCK: p->sig_blocked &= ~set; break;
    case SIG_SETMASK: p->sig_blocked = set; break;
    default: return -EINVAL;
    }
    p->sig_blocked &= ~UNBLOCKABLE;
    return 0;
}

long do_rt_sigreturn(const u64 *a)
{
    (void)a;
    struct proc *p = current_proc();
    struct trap_frame *tf = thread_user_tf(current);
    struct sigframe f;
    if (copy_from_user(&f, TF_SP(tf), sizeof(f)) || f.magic != FRAME_MAGIC) {
        klog(LOG_ERR, SUBSYS_PROC, "pid %d (%s): rt_sigreturn with a bad signal frame at %#lx "
             "(magic %#lx): the handler corrupted its stack or sp\n", p->pid, p->name,
             (unsigned long)TF_SP(tf), (unsigned long)f.magic);
        user_fault(tf, "bad signal frame", SIGSEGV);
        return -EFAULT;
    }
    memcpy(&tf->regs[1], &f.regs[1], sizeof(f.regs) - sizeof(f.regs[0]));
    tf->sepc = f.sepc;
    p->sig_blocked = f.blocked & ~UNBLOCKABLE;
    if (f.fp_valid && current->fp_used) {   /* the handler may have used the FP registers */
        unsigned long fl = csr_read(sstatus) & SSTATUS_SIE;
        intr_off();
        current->fp = f.fp;
        fp_load_regs(&current->fp);
        if (fl)
            intr_on();
    }
    return (long)f.regs[10];            /* dispatch stores this in a0: keep it */
}

long do_rt_sigsuspend(const u64 *a)
{
    struct proc *p = current_proc();
    u32 mask;
    if (copy_from_user(&mask, a[0], sizeof(mask)))
        return -EFAULT;
    p->saved_blocked = p->sig_blocked;
    p->restore_blocked = true;
    p->sig_blocked = mask & ~UNBLOCKABLE;
    struct waitqueue wq = WAITQUEUE_INIT(wq, "sigsuspend");
    current->wait_what = "sigsuspend";
    wait_event_interruptible(&wq, false);
    return -EINTR;
}

/* ---- process groups and sessions ---- */

static long setpgid_locked(struct proc *self, long pid, long pgid_arg)
{
    struct proc *p = pid ? proc_find((int)pid) : self;
    int pgid = pgid_arg ? (int)pgid_arg : (p ? p->pid : 0);
    if (!p || (p != self && p->parent != self))
        return -ESRCH;
    if (p->sid != self->sid || p->pid == p->sid)
        return -EPERM;                  /* other session, or a session leader */
    if (pgid != p->pid) {
        bool exists = false;
        struct proc *q;
        list_for_each_entry(q, &all_procs, all_node)
            if (q->pgid == pgid && q->sid == self->sid)
                exists = true;
        if (!exists)
            return -EPERM;
    }
    p->pgid = pgid;
    return 0;
}

long do_setpgid(const u64 *a)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    long r = setpgid_locked(current_proc(), (long)a[0], (long)a[1]);
    spin_unlock_irqrestore(&proc_lock, flags);
    return r;
}

long do_getpgid(const u64 *a)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *p = a[0] ? proc_find((int)a[0]) : current_proc();
    long r = p ? p->pgid : -ESRCH;
    spin_unlock_irqrestore(&proc_lock, flags);
    return r;
}

long do_getsid(const u64 *a)
{
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *p = a[0] ? proc_find((int)a[0]) : current_proc();
    long r = p ? p->sid : -ESRCH;
    spin_unlock_irqrestore(&proc_lock, flags);
    return r;
}

long do_setsid(const u64 *a)
{
    (void)a;
    struct proc *p = current_proc();
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    long r;
    if (p->pgid == p->pid) {
        r = -EPERM;                     /* already a group leader */
    } else {
        p->sid = p->pgid = p->pid;
        p->ctty = NULL;
        r = p->sid;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return r;
}
