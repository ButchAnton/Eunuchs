/*
 * ppoll(2): wait for any of several files to become ready.
 *
 * Each pass marks the caller sleeping, then asks every file for its
 * readiness (chr_ops.poll); each poll op registers the wait queues that will
 * signal a change with poll_wait(). If nothing is ready the caller sleeps
 * until one of them is woken, the timeout passes, or a signal arrives. Being
 * marked sleeping *before* the checks is what makes this race-free: a
 * wakeup between a check and the sleep just makes the sleep return at once.
 *
 * Debugging: a thread blocked here shows "poll" in ps; strace decodes ppoll.
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <fs.h>
#include <mm.h>
#include <riscv.h>
#include <proc.h>
#include <sched.h>
#include <uapi/errno.h>
#include <uapi/net.h>

#define POLL_MAX_FDS    64
#define POLL_MAX_WAITS  128

struct poll_table {
    int n;
    struct {
        struct waitqueue *wq;
        struct wait_entry we;
    } e[POLL_MAX_WAITS];
};

void poll_wait(struct poll_table *pt, struct waitqueue *wq)
{
    if (!pt || pt->n == POLL_MAX_WAITS)
        return;
    pt->e[pt->n].wq = wq;
    pt->e[pt->n].we = (struct wait_entry){ .t = current, .queued = false };
    add_wait_queue(wq, &pt->e[pt->n].we);
    pt->n++;
}

static void poll_clear(struct poll_table *pt)
{
    for (int i = 0; i < pt->n; i++)
        remove_wait_queue(pt->e[i].wq, &pt->e[i].we);
    pt->n = 0;
}

u32 file_poll(struct file *f, struct poll_table *pt)
{
    if (f->cops && f->cops->poll)
        return f->cops->poll(f, pt);
    return (f->readable ? POLLIN : 0) | (f->writable ? POLLOUT : 0);  /* disk files */
}

long do_ppoll(const u64 *a)
{
    uintptr_t ufds = a[0];
    u64 nfds = a[1];
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;
    struct pollfd fds[POLL_MAX_FDS];
    if (nfds && copy_from_user(fds, ufds, nfds * sizeof(struct pollfd)))
        return -EFAULT;
    u64 deadline = 0;
    bool forever = true;
    if (a[2]) {
        struct { long sec, nsec; } ts;
        if (copy_from_user(&ts, a[2], sizeof(ts)))
            return -EFAULT;
        forever = false;
        deadline = rdtime() + ms_to_rdtime((u64)ts.sec * 1000 + (u64)ts.nsec / 1000000);
    }

    struct poll_table *pt = kzalloc(sizeof(*pt));
    if (!pt)
        return -ENOMEM;
    long ready = 0;
    for (;;) {
        set_current_sleeping("poll");
        ready = 0;
        for (int i = 0; i < (int)nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = fd_get(fds[i].fd);
            u32 m = f ? file_poll(f, pt) : POLLNVAL;
            fds[i].revents = (short)(m & ((u32)fds[i].events | POLLERR | POLLHUP | POLLNVAL));
            if (fds[i].revents)
                ready++;
        }
        if (ready || (!forever && rdtime() >= deadline))
            break;
        if (thread_signal_pending(current)) {
            ready = -EINTR;
            break;
        }
        schedule_until(forever ? 0 : deadline);
        poll_clear(pt);
    }
    set_current_running();
    poll_clear(pt);
    kfree(pt);
    if (ready >= 0 && nfds && copy_to_user(ufds, fds, nfds * sizeof(struct pollfd)))
        return -EFAULT;
    return ready;
}
