/*
 * Write-ahead log (redo log) for efs, after xv6.
 *
 * A file system operation brackets its changes with log_begin/log_end and
 * records every modified buffer with log_write (instead of writing it). When
 * the last concurrent operation ends, the transaction commits:
 *   1. write the modified blocks into the log area
 *   2. write the log header with their home locations  <- commit point
 *   3. copy them to their home locations
 *   4. clear the header
 * After a crash, mount replays a committed log (step 3). Either all of a
 * transaction's changes reach the disk or none do.
 *
 * Operations nest per thread (an iput that frees an inode inside another
 * operation joins it). log_write outside an operation is a bug and panics.
 *
 * Testing recovery: boot with fs.crash=after-log, after-commit or mid-install
 * to stop the machine (as if the power failed) at that point of the next
 * commit; the next boot must recover. `log` stats: /proc/mounts, `mounts`.
 */
#define LOG_SUBSYS SUBSYS_FS
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <trace.h>
#include <uapi/errno.h>

#define MAXOPBLOCKS 16          /* most blocks one operation may log */

struct efs_log {
    spinlock_t lock;
    struct waitqueue wq;
    u32 disk;
    u32 start;                  /* header block; data blocks follow */
    int outstanding;            /* operations in progress */
    bool committing;
    int syncing;                /* log_sync callers: new operations wait for the commit */
    u32 n;                      /* blocks in the current transaction */
    u32 blocks[EFS_LOGSIZE];
    u64 seq;
    const char *crash_at;
    /* statistics */
    u64 commits, logged, absorbed, recovered, max_n, commit_ticks;
};

static void read_head(struct efs_log *log, struct efs_logheader *out)
{
    struct buf *b = bread(log->disk, log->start);
    memcpy(out, b->data, sizeof(*out));
    brelse(b);
}

static void write_head(struct efs_log *log, u32 n)
{
    struct buf *b = bread(log->disk, log->start);
    struct efs_logheader *lh = (struct efs_logheader *)b->data;
    lh->magic = EFS_LOG_MAGIC;
    lh->n = n;
    lh->seq = log->seq;
    memcpy(lh->blocks, log->blocks, n * sizeof(u32));
    bwrite(b);
    brelse(b);
}

static void crash_point(struct efs_log *log, const char *where)
{
    if (log->crash_at && !strcmp(log->crash_at, where)) {
        pr_warn("fs.crash=%s: simulating a power failure during commit %lu\n", where,
                (unsigned long)log->seq);
        platform_exit(3);
    }
}

/* Copy committed blocks from the log to their home locations. */
static void install(struct efs_log *log, u32 n, const u32 *blocks, bool recovering)
{
    for (u32 i = 0; i < n; i++) {
        struct buf *from = bread(log->disk, log->start + 1 + i);
        struct buf *to = bread(log->disk, blocks[i]);
        memcpy(to->data, from->data, EFS_BSIZE);
        bwrite(to);
        if (!recovering)
            bunpin(to);
        brelse(from);
        brelse(to);
        if (i == n / 2)
            crash_point(log, "mid-install");
    }
}

static void commit(struct efs_log *log)
{
    if (!log->n)
        return;
    u64 t0 = rdtime();
    for (u32 i = 0; i < log->n; i++) {             /* 1. blocks into the log */
        struct buf *to = bread(log->disk, log->start + 1 + i);
        struct buf *from = bread(log->disk, log->blocks[i]);
        memcpy(to->data, from->data, EFS_BSIZE);
        bwrite(to);
        brelse(from);
        brelse(to);
    }
    crash_point(log, "after-log");
    write_head(log, log->n);                        /* 2. commit point */
    crash_point(log, "after-commit");
    install(log, log->n, log->blocks, false);       /* 3. home locations */
    u32 n = log->n;
    log->n = 0;
    write_head(log, 0);                             /* 4. done */
    u64 dt = rdtime() - t0;
    log->commits++;
    log->logged += n;
    log->commit_ticks += dt;
    if (n > log->max_n)
        log->max_n = n;
    trace(log_commit, log->seq, n, dt * 1000000 / platform.timebase);
    log->seq++;
}

static struct efs_log *the_log;     /* the root filesystem's (for crash testing) */

int efs_arm_crash(const char *point)
{
    if (strcmp(point, "after-log") && strcmp(point, "after-commit") && strcmp(point, "mid-install"))
        return -EINVAL;
    if (!the_log)
        return -ENODEV;
    the_log->crash_at = point;
    pr_warn("crash point '%s' armed for the next commit\n", point);
    return 0;
}

struct efs_log *log_create(struct super_block *sb, const struct efs_super *esb)
{
    struct efs_log *log = kzalloc(sizeof(*log));
    spin_init(&log->lock, "efs-log");
    waitqueue_init(&log->wq, "log-space");
    log->disk = sb->disk;
    log->start = esb->logstart;
    if (!the_log)                       /* crash testing applies to the root filesystem */
        log->crash_at = cmdline_get("fs.crash");

    struct efs_logheader lh;
    read_head(log, &lh);
    if (lh.magic != EFS_LOG_MAGIC) {
        pr_err("log header has bad magic %#x; not replaying\n", lh.magic);
    } else if (lh.n > EFS_LOGSIZE) {
        pr_err("log header claims %u blocks; not replaying\n", lh.n);
    } else if (lh.n) {
        pr_warn("recovering: replaying committed transaction %lu (%u blocks)\n",
                (unsigned long)lh.seq, lh.n);
        install(log, lh.n, lh.blocks, true);
        log->recovered = lh.n;
        log->seq = lh.seq;
        write_head(log, 0);
    }
    log->seq = lh.seq + 1;
    if (log->crash_at)
        pr_warn("fs.crash=%s armed: the next commit will stop the machine\n", log->crash_at);
    if (!the_log)
        the_log = log;
    return log;
}

void log_begin(struct efs_log *log)
{
    struct thread *t = current;
    if (t->fs_op_depth++)
        return;                             /* nested: already reserved */
    for (;;) {
        unsigned long flags = spin_lock_irqsave(&log->lock);
        if (!log->committing && !log->syncing &&
            log->n + (log->outstanding + 1) * MAXOPBLOCKS <= EFS_LOGSIZE) {
            log->outstanding++;
            spin_unlock_irqrestore(&log->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&log->lock, flags);
        t->wait_what = "log space";
        wait_event(&log->wq, !log->committing && !log->syncing &&
                   log->n + (log->outstanding + 1) * MAXOPBLOCKS <= EFS_LOGSIZE);
    }
}

void log_end(struct efs_log *log)
{
    struct thread *t = current;
    if (t->fs_op_depth <= 0)
        panic("log_end without log_begin");
    if (--t->fs_op_depth)
        return;
    bool do_commit = false;
    unsigned long flags = spin_lock_irqsave(&log->lock);
    log->outstanding--;
    if (log->outstanding == 0) {
        log->committing = true;
        do_commit = true;
    }
    spin_unlock_irqrestore(&log->lock, flags);
    if (!do_commit) {
        wake_up(&log->wq);                  /* more space may be reservable */
        return;
    }
    commit(log);                            /* sleeps on disk I/O: no locks held */
    flags = spin_lock_irqsave(&log->lock);
    log->committing = false;
    spin_unlock_irqrestore(&log->lock, flags);
    wake_up(&log->wq);
}

/*
 * Wait until every operation that has finished is on disk (sync, fsync,
 * reboot). A finished operation's blocks wait in the log until the last of
 * the operations running with it ends and commits them all; that commit
 * (seq moving past its value now) is the one to wait for. Meanwhile no new
 * operation starts (log_begin waits while `syncing`), so the ones running
 * drain and the commit comes even under a steady stream of writers.
 */
void log_sync(struct efs_log *log)
{
    if (current->fs_op_depth)
        panic("log_sync inside a transaction: it would wait for its own commit");
    unsigned long flags = spin_lock_irqsave(&log->lock);
    bool pending = log->n > 0 || log->committing;
    u64 target = log->seq + 1;
    if (pending)
        log->syncing++;
    spin_unlock_irqrestore(&log->lock, flags);
    if (!pending)
        return;
    current->wait_what = "log commit";
    wait_event(&log->wq, log->seq >= target || (!log->committing && log->n == 0));
    flags = spin_lock_irqsave(&log->lock);
    log->syncing--;
    spin_unlock_irqrestore(&log->lock, flags);
    wake_up(&log->wq);                      /* operations held for the commit */
}

void log_write(struct efs_log *log, struct buf *b)
{
    if (current->fs_op_depth <= 0)
        panic("log_write of block %u outside a transaction (missing fs_begin_op?)", b->blockno);
    unsigned long flags = spin_lock_irqsave(&log->lock);
    u32 i;
    for (i = 0; i < log->n; i++)
        if (log->blocks[i] == b->blockno)
            break;                          /* absorbed: already in this transaction */
    if (i == log->n) {
        if (log->n >= EFS_LOGSIZE) {
            spin_unlock_irqrestore(&log->lock, flags);
            panic("transaction too big: %u blocks (an operation exceeded MAXOPBLOCKS=%d?)",
                  log->n, MAXOPBLOCKS);
        }
        log->blocks[log->n++] = b->blockno;
        spin_unlock_irqrestore(&log->lock, flags);
        bpin(b);
        return;
    }
    log->absorbed++;
    spin_unlock_irqrestore(&log->lock, flags);
}

void log_print(struct efs_log *log, print_fn print)
{
    u64 tb = platform.timebase;
    print("  log: %lu commits (seq %lu), %lu blocks logged, %lu absorbed, largest %lu, "
          "avg commit %lu us; %d ops in progress, %u blocks pending; recovered %lu at mount%s\n",
          (unsigned long)log->commits, (unsigned long)log->seq, (unsigned long)log->logged,
          (unsigned long)log->absorbed, (unsigned long)log->max_n,
          (unsigned long)(log->commits ? log->commit_ticks * 1000000 / tb / log->commits : 0),
          log->outstanding, log->n, (unsigned long)log->recovered,
          log->crash_at ? "; fs.crash armed" : "");
}
