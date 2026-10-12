/*
 * Buffer cache: NBUF block buffers with LRU replacement. bread() returns a
 * locked buffer (a sleeping mutex, so the deadlock detector and `ps` show who
 * waits for whom); brelse() unlocks and drops the reference.
 *
 * Debugging: `bcache` in the monitor and /proc/bcache list referenced
 * buffers with the code that took them. If every buffer is referenced, we
 * panic with that list instead of hanging (it is almost always a missing
 * brelse). bcache_busy() lets tests assert that nothing leaked.
 */
#define LOG_SUBSYS SUBSYS_BLK
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <riscv.h>
#include <virtio.h>

#define NBUF 128

static struct buf bufs[NBUF];
static LIST_HEAD(lru);
static spinlock_t bc_lock = SPINLOCK_INIT("bcache");
static u64 hits, misses, writes;

void bcache_init(void)
{
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &bufs[i];
        b->data = page_alloc_zeroed();
        if (!b->data)
            panic("bcache: out of memory");
        mutex_init(&b->lock, "buf");
        list_add_tail(&b->lru, &lru);
    }
    pr_info("buffer cache: %d buffers of %d bytes\n", NBUF, EFS_BSIZE);
}

static void dump_held(print_fn print)
{
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &bufs[i];
        if (!b->refcnt && !b->pins)
            continue;
        char s[96];
        ksym_format(s, sizeof(s), b->get_site);
        print("  disk%u block %6u refcnt %d pins %u %s, taken by %s%s%s\n", b->disk, b->blockno, b->refcnt,
              b->pins, b->lock.owner ? "locked" : "unlocked", s,
              b->lock.owner ? " held by thread " : "",
              b->lock.owner ? b->lock.owner->name : "");
    }
}

static struct buf *bget(u32 disk, u32 blockno, uintptr_t site)
{
    unsigned long flags = spin_lock_irqsave(&bc_lock);
    struct buf *b;
    list_for_each_entry(b, &lru, lru) {
        if (b->blockno == blockno && b->disk == disk && (b->valid || b->refcnt)) {
            b->refcnt++;
            hits++;
            spin_unlock_irqrestore(&bc_lock, flags);
            mutex_lock(&b->lock);
            b->get_site = site;
            return b;
        }
    }
    /* Recycle the least recently used unreferenced buffer. */
    for (struct list_head *pos = lru.prev; pos != &lru; pos = pos->prev) {
        b = list_entry(pos, struct buf, lru);
        if (!b->refcnt && !b->pins) {
            b->blockno = blockno;
            b->disk = disk;
            b->valid = false;
            b->refcnt = 1;
            misses++;
            spin_unlock_irqrestore(&bc_lock, flags);
            mutex_lock(&b->lock);
            b->get_site = site;
            return b;
        }
    }
    spin_unlock_irqrestore(&bc_lock, flags);
    panic_print("buffer cache exhausted: all %d buffers are referenced:\n", NBUF);
    dump_held(panic_print);
    panic("bcache: no free buffers (a missing brelse?)");
}

struct buf *bread(u32 disk, u32 blockno)
{
    struct buf *b = bget(disk, blockno, (uintptr_t)__builtin_return_address(0));
    if (!b->valid) {
        if (blk_rw(disk, (u64)blockno * EFS_SECTORS_PER_BLOCK, b->data, EFS_BSIZE, false))
            pr_err("bread: I/O error on %s block %u\n", blk_name((int)disk), blockno);
        b->valid = true;
    }
    return b;
}

int bwrite(struct buf *b)
{
    if (b->lock.owner != current)
        panic("bwrite of block %u by a thread that does not hold the buffer", b->blockno);
    writes++;
    return blk_rw(b->disk, (u64)b->blockno * EFS_SECTORS_PER_BLOCK, b->data, EFS_BSIZE, true);
}

void brelse(struct buf *b)
{
    if (b->lock.owner != current)
        panic("brelse of block %u by a thread that does not hold the buffer", b->blockno);
    b->last_used = rdtime();
    mutex_unlock(&b->lock);
    unsigned long flags = spin_lock_irqsave(&bc_lock);
    if (--b->refcnt == 0) {
        list_del(&b->lru);
        list_add(&b->lru, &lru);        /* most recently used */
    }
    spin_unlock_irqrestore(&bc_lock, flags);
}

void bpin(struct buf *b)
{
    unsigned long flags = spin_lock_irqsave(&bc_lock);
    b->pins++;
    spin_unlock_irqrestore(&bc_lock, flags);
}

void bunpin(struct buf *b)
{
    unsigned long flags = spin_lock_irqsave(&bc_lock);
    b->pins--;
    spin_unlock_irqrestore(&bc_lock, flags);
}

int bcache_busy(void)
{
    int n = 0;
    for (int i = 0; i < NBUF; i++)
        n += bufs[i].refcnt > 0;
    return n;
}

void bcache_print(print_fn print)
{
    int valid = 0, busy = 0, pinned = 0;
    for (int i = 0; i < NBUF; i++) {
        valid += bufs[i].valid;
        busy += bufs[i].refcnt > 0;
        pinned += bufs[i].pins > 0;
    }
    u64 total = hits + misses;
    print("bcache: %d buffers, %d valid, %d referenced, %d pinned by the log\n", NBUF, valid,
          busy, pinned);
    print("  lookups %lu, hits %lu (%lu%%), misses %lu, writes %lu\n", (unsigned long)total,
          (unsigned long)hits, (unsigned long)(total ? hits * 100 / total : 0),
          (unsigned long)misses, (unsigned long)writes);
    if (busy || pinned)
        dump_held(print);
}
