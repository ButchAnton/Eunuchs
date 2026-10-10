/*
 * Pipes: a 4 KiB ring between a read end and a write end (two struct files
 * sharing a struct pipe). Readers block while empty (EOF once every writer
 * is gone); writers block while full and get EPIPE + SIGPIPE when every
 * reader is gone. Waits are interruptible by signals (EINTR).
 *
 * Debugging: every pipe is on a list shown by `pipes` in the monitor and in
 * /proc/pipes (buffered bytes, open ends, totals, who waits).
 */
#define LOG_SUBSYS SUBSYS_VFS
#include <debug.h>
#include <fmt.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <uapi/errno.h>
#include <uapi/net.h>

#define PIPE_SIZE 4096

struct pipe {
    struct mutex lock;
    u8 *buf;
    u64 rpos, wpos;             /* monotonic; bytes buffered = wpos - rpos */
    int readers, writers;
    int ends;                   /* struct files still using it; the last one frees */
    struct waitqueue rwait, wwait;
    u64 id;
    u64 total;
    struct list_head node;
};

static LIST_HEAD(pipes);
static spinlock_t pipes_lock = SPINLOCK_INIT("pipes");
static u64 next_id = 1;

static long pipe_read(struct file *f, void *dst, size_t n, bool user)
{
    struct pipe *pp = f->priv;
    mutex_lock(&pp->lock);
    while (pp->wpos == pp->rpos && pp->writers > 0) {
        mutex_unlock(&pp->lock);
        current->wait_what = "pipe read";
        if (wait_event_interruptible(&pp->rwait, pp->wpos != pp->rpos || pp->writers == 0))
            return -ERESTARTSYS;
        mutex_lock(&pp->lock);
    }
    size_t avail = pp->wpos - pp->rpos, done = 0;
    n = MIN(n, avail);
    while (done < n) {
        size_t off = pp->rpos % PIPE_SIZE;
        size_t c = MIN(n - done, PIPE_SIZE - off);
        long r = user ? copy_to_user((uintptr_t)dst + done, pp->buf + off, c)
                      : (memcpy((u8 *)dst + done, pp->buf + off, c), 0);
        if (r) {
            if (!done)
                done = (size_t)-EFAULT;
            break;
        }
        pp->rpos += c;
        done += c;
    }
    mutex_unlock(&pp->lock);
    wake_up(&pp->wwait);
    return (long)done;
}

static long pipe_write(struct file *f, const void *src, size_t n, bool user)
{
    struct pipe *pp = f->priv;
    size_t done = 0;
    mutex_lock(&pp->lock);
    while (done < n) {
        if (pp->readers == 0) {
            mutex_unlock(&pp->lock);
            signal_send(current_proc(), SIGPIPE, current_proc()->pid);
            return done ? (long)done : -EPIPE;
        }
        size_t space = PIPE_SIZE - (pp->wpos - pp->rpos);
        if (!space) {
            mutex_unlock(&pp->lock);
            wake_up(&pp->rwait);
            current->wait_what = "pipe write";
            if (wait_event_interruptible(&pp->wwait, pp->wpos - pp->rpos < PIPE_SIZE ||
                                                     pp->readers == 0))
                return done ? (long)done : -ERESTARTSYS;
            mutex_lock(&pp->lock);
            continue;
        }
        size_t off = pp->wpos % PIPE_SIZE;
        size_t c = MIN(MIN(n - done, space), PIPE_SIZE - off);
        long r = user ? copy_from_user(pp->buf + off, (uintptr_t)src + done, c)
                      : (memcpy(pp->buf + off, (const u8 *)src + done, c), 0);
        if (r) {
            mutex_unlock(&pp->lock);
            return done ? (long)done : -EFAULT;
        }
        pp->wpos += c;
        pp->total += c;
        done += c;
    }
    mutex_unlock(&pp->lock);
    wake_up(&pp->rwait);
    return (long)done;
}

/*
 * The two ends may be closed at the same time on different CPUs. Whoever
 * frees the pipe must be sure the other closer is completely done with it,
 * including the tail of its mutex_unlock() and its wakeups, so each closer
 * drops `ends` as its very last access.
 */
static void pipe_close(struct file *f)
{
    struct pipe *pp = f->priv;
    mutex_lock(&pp->lock);
    if (f->readable)
        pp->readers--;
    if (f->writable)
        pp->writers--;
    mutex_unlock(&pp->lock);
    wake_up(&pp->rwait);
    wake_up(&pp->wwait);
    if (__atomic_sub_fetch(&pp->ends, 1, __ATOMIC_ACQ_REL) == 0) {
        unsigned long flags = spin_lock_irqsave(&pipes_lock);
        list_del(&pp->node);
        spin_unlock_irqrestore(&pipes_lock, flags);
        page_free(pp->buf);
        kfree(pp);
    }
}

/* Unlocked reads: poll rechecks after every wakeup. */
static u32 pipe_poll(struct file *f, struct poll_table *pt)
{
    struct pipe *pp = f->priv;
    u32 m = 0;
    if (f->readable) {
        poll_wait(pt, &pp->rwait);
        if (pp->wpos != pp->rpos)
            m |= POLLIN;
        if (pp->writers == 0)
            m |= POLLHUP;
    }
    if (f->writable) {
        poll_wait(pt, &pp->wwait);
        if (pp->readers == 0)
            m |= POLLERR;
        else if (pp->wpos - pp->rpos < PIPE_SIZE)
            m |= POLLOUT;
    }
    return m;
}

static const struct chr_ops pipe_ops = { .read = pipe_read, .write = pipe_write, .close = pipe_close,
                                         .poll = pipe_poll };

static struct file *end(struct pipe *pp, bool read)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    f->refcnt = 1;
    f->readable = read;
    f->writable = !read;
    f->cops = &pipe_ops;
    f->priv = pp;
    ksnprintf(f->path, sizeof(f->path), "pipe:[%lu] %s", (unsigned long)pp->id,
              read ? "read" : "write");
    return f;
}

int pipe_create(struct file **rf, struct file **wf)
{
    struct pipe *pp = kzalloc(sizeof(*pp));
    if (!pp || !(pp->buf = page_alloc())) {
        kfree(pp);
        return -ENOMEM;
    }
    mutex_init(&pp->lock, "pipe");
    waitqueue_init(&pp->rwait, "pipe-read");
    waitqueue_init(&pp->wwait, "pipe-write");
    pp->readers = pp->writers = 1;
    pp->ends = 2;
    unsigned long flags = spin_lock_irqsave(&pipes_lock);
    pp->id = next_id++;
    list_add_tail(&pp->node, &pipes);
    spin_unlock_irqrestore(&pipes_lock, flags);
    *rf = end(pp, true);
    *wf = end(pp, false);
    if (!*rf || !*wf)
        panic("pipe_create: out of memory");     /* tiny allocations */
    return 0;
}

bool file_is_pipe(const struct file *f)
{
    return f->cops == &pipe_ops;
}

void pipes_print(print_fn print)
{
    struct pipe *pp;
    unsigned long flags;
    bool locked = spin_lock_debug(&pipes_lock, &flags);
    print("  %-6s %8s %7s %7s %10s\n", "pipe", "buffered", "readers", "writers", "total");
    list_for_each_entry(pp, &pipes, node)
        print("  %-6lu %8lu %7d %7d %10lu\n", (unsigned long)pp->id,
              (unsigned long)(pp->wpos - pp->rpos), pp->readers, pp->writers,
              (unsigned long)pp->total);
    spin_unlock_debug(&pipes_lock, locked, flags);
}
