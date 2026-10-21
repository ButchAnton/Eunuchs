/*
 * The page cache: pages of files, and the swap cache (anonymous pages that
 * have a swap slot), in one hash table keyed by (owner, inode, index) and
 * one LRU list that reclaim walks from the cold end.
 *
 * File pages are keyed by the file's super_block and inode number, not the
 * in-memory inode, so they outlive the inode (which the VFS frees when the
 * last reference goes) and a file read again is still cached. Writes go to
 * the file first (through the filesystem and its log, as before) and then
 * into any cached pages they cover (pc_update), so read(), write() and
 * mmap() see the same bytes. Pages written through a shared mapping are
 * marked dirty by the fault that made them writable and go back to the file
 * at msync, fsync, sync, from kswapd's periodic writeback, and before
 * reclaim frees them.
 *
 * Swap-cache pages are keyed by &swap_owner and their slot (swap.c).
 *
 * References: the cache holds one on every page in it; anyone using a page
 * (a PTE, a reader) holds another. A page with only the cache's reference
 * is unmapped and may be freed (after writing it, if dirty).
 *
 * Locking: pc_lock (a spinlock) covers the hash table, the LRU and the
 * PG_CACHE/PG_SWAPCACHE flags. Filling a file page happens under the
 * file's inode lock, which also orders fills against writes and truncation.
 * Debugging: /proc/pagecache, `pagecache` in the monitor, /proc/vmstat.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <spinlock.h>
#include <trace.h>
#include <uapi/errno.h>

const char swap_owner;

#define PC_HASH 4096
static struct page *hash[PC_HASH];
static spinlock_t pc_lock = SPINLOCK_INIT("pagecache");
static LIST_HEAD(pc_lru);                  /* coldest first */
static u64 nr_file, nr_swap;

static unsigned hfn(const void *owner, u32 ino, u64 index)
{
    u64 h = (uintptr_t)owner * 0x9e3779b97f4a7c15ULL ^ (u64)ino * 0xff51afd7ed558ccdULL ^
            index * 0xc4ceb9fe1a85ec53ULL;
    return (unsigned)((h ^ (h >> 31)) % PC_HASH);
}

static struct page *lookup_locked(const void *owner, u32 ino, u64 index)
{
    for (struct page *pg = hash[hfn(owner, ino, index)]; pg; pg = pg->cache.hnext)
        if (pg->cache.owner == owner && pg->cache.ino == ino && pg->cache.index == index)
            return pg;
    return NULL;
}

/* Out of the hash and the LRU; the caller drops the cache's reference. */
static void unhash_locked(struct page *pg)
{
    struct page **pp = &hash[hfn(pg->cache.owner, pg->cache.ino, pg->cache.index)];
    while (*pp && *pp != pg)
        pp = &(*pp)->cache.hnext;
    if (*pp)
        *pp = pg->cache.hnext;
    list_del(&pg->list);
    if (pg->flags & PG_SWAPCACHE)
        nr_swap--;
    else
        nr_file--;
    page_clear_flag(pg, PG_CACHE | PG_SWAPCACHE | PG_DIRTY | PG_REFERENCED);
}

/* After unhash_locked, outside pc_lock: the cache's reference (and slot). */
static void release(struct page *pg, bool swapcache, u32 slot)
{
    if (swapcache)
        swap_free(slot);
    page_put(pg);
}

struct page *pc_find(const void *owner, u32 ino, u64 index)
{
    unsigned long flags = spin_lock_irqsave(&pc_lock);
    struct page *pg = lookup_locked(owner, ino, index);
    if (pg) {
        page_get(pg);
        page_set_flag(pg, PG_REFERENCED);
    }
    spin_unlock_irqrestore(&pc_lock, flags);
    return pg;
}

struct page *pc_insert(struct page *pg, const void *owner, u32 ino, u64 index)
{
    unsigned long flags = spin_lock_irqsave(&pc_lock);
    struct page *old = lookup_locked(owner, ino, index);
    if (old) {
        page_get(old);
        spin_unlock_irqrestore(&pc_lock, flags);
        return old;
    }
    pg->cache.owner = owner;
    pg->cache.ino = ino;
    pg->cache.index = index;
    pg->cache.wmap = 0;
    unsigned h = hfn(owner, ino, index);
    pg->cache.hnext = hash[h];
    hash[h] = pg;
    list_add_tail(&pg->list, &pc_lru);
    page_set_flag(pg, owner == &swap_owner ? PG_SWAPCACHE : PG_CACHE);
    if (owner == &swap_owner)
        nr_swap++;
    else
        nr_file++;
    page_get(pg);                       /* the cache's reference */
    spin_unlock_irqrestore(&pc_lock, flags);
    return NULL;
}

void pc_remove(struct page *pg)
{
    unsigned long flags = spin_lock_irqsave(&pc_lock);
    if (!(pg->flags & (PG_CACHE | PG_SWAPCACHE))) {
        spin_unlock_irqrestore(&pc_lock, flags);
        return;
    }
    bool sw = pg->flags & PG_SWAPCACHE;
    u32 slot = (u32)pg->cache.index;
    unhash_locked(pg);
    spin_unlock_irqrestore(&pc_lock, flags);
    release(pg, sw, slot);
}

struct page *pc_get(struct inode *ip, u64 index, bool *major, int *err)
{
    return pc_get_spare(ip, index, major, err, NULL);
}

struct page *pc_get_spare(struct inode *ip, u64 index, bool *major, int *err, struct page **spare)
{
    if (major)
        *major = false;
    struct page *pg = pc_find(ip->sb, ip->ino, index);
    if (pg) {
        vmc.cache_hits++;
        return pg;
    }
    if (spare && !*spare) {
        *err = -EAGAIN;                 /* the caller allocates without its locks */
        return NULL;
    }
    vmc.cache_misses++;
    if (spare) {
        pg = *spare;
        *spare = NULL;
        memset(page_to_va(pg), 0, PAGE_SIZE);
    } else if (!(pg = user_page_alloc(true))) {     /* zero: the part past the end of the file */
        *err = -ENOMEM;
        return NULL;
    }
    long r = ip->sb->ops->read(ip, page_to_va(pg), index * PAGE_SIZE, PAGE_SIZE, false);
    if (r < 0) {
        page_put(pg);
        *err = (int)r;
        return NULL;
    }
    trace(pagecache_fill, ip->ino, index, r);
    if (major)
        *major = true;
    struct page *old = pc_insert(pg, ip->sb, ip->ino, index);
    if (old) {                          /* (only with a filesystem that fills unlocked) */
        page_put(pg);
        return old;
    }
    return pg;
}

void pc_update(struct inode *ip, u64 off, const void *src, size_t n)
{
    if (!n)
        return;
    for (u64 index = off / PAGE_SIZE; index <= (off + n - 1) / PAGE_SIZE; index++) {
        struct page *pg = pc_find(ip->sb, ip->ino, index);
        if (!pg)
            continue;
        u64 lo = MAX(off, index * PAGE_SIZE), hi = MIN(off + n, (index + 1) * PAGE_SIZE);
        memcpy((u8 *)page_to_va(pg) + (lo - index * PAGE_SIZE), (const u8 *)src + (lo - off), hi - lo);
        page_put(pg);
    }
}

/* Every cached page of (sb, ino) from page `first` on, out of the cache. */
static void drop_from(struct super_block *sb, u32 ino, u64 first)
{
    for (;;) {
        struct page *batch[32];
        int n = 0;
        unsigned long flags = spin_lock_irqsave(&pc_lock);
        struct page *pg, *tmp;
        list_for_each_entry_safe(pg, tmp, &pc_lru, list) {
            if (pg->cache.owner != sb || pg->cache.ino != ino || pg->cache.index < first)
                continue;
            unhash_locked(pg);
            batch[n++] = pg;
            if (n == 32)
                break;
        }
        spin_unlock_irqrestore(&pc_lock, flags);
        for (int i = 0; i < n; i++)
            release(batch[i], false, 0);
        if (n < 32)
            return;
    }
}

void pc_truncate(struct inode *ip, u64 size)
{
    drop_from(ip->sb, ip->ino, (size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (size % PAGE_SIZE) {
        struct page *pg = pc_find(ip->sb, ip->ino, size / PAGE_SIZE);
        if (pg) {
            memset((u8 *)page_to_va(pg) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
            page_put(pg);
        }
    }
}

void pc_drop_inode(struct super_block *sb, u32 ino)
{
    drop_from(sb, ino, 0);
}

/*
 * Write one dirty file page to its file. Clean afterwards unless something
 * still maps it writable (then the next writeback writes it again).
 * Returns 0, -EAGAIN (the file is busy and !lock_wait) or an error.
 */
static int write_page(struct page *pg, bool lock_wait)
{
    struct super_block *sb = (struct super_block *)pg->cache.owner;
    struct inode *ip = iget(sb, pg->cache.ino);
    if (!ip)
        return -ENOMEM;
    int r = 0;
    fs_begin_op(sb);
    if (lock_wait)
        ilock(ip);
    else if (!ilock_try(ip)) {
        fs_end_op(sb);
        iput(ip);
        return -EAGAIN;
    }
    if ((pg->flags & PG_CACHE) && (pg->flags & PG_DIRTY)) {
        u64 off = pg->cache.index * PAGE_SIZE;
        if (!ip->valid || off >= ip->size) {
            page_clear_flag(pg, PG_DIRTY);      /* past the end: nothing to write */
        } else {
            size_t len = MIN((u64)PAGE_SIZE, ip->size - off);
            if (!__atomic_load_n(&pg->cache.wmap, __ATOMIC_RELAXED))
                page_clear_flag(pg, PG_DIRTY);  /* (a write fault after this sets it again) */
            long w = sb->ops->write ? sb->ops->write(ip, page_to_va(pg), off, len, false) : -EROFS;
            if (w != (long)len) {
                page_set_flag(pg, PG_DIRTY);
                r = w < 0 ? (int)w : -EIO;
            } else {
                vmc.writebacks++;
                trace(writeback, ip->ino, pg->cache.index, len);
            }
        }
    }
    iunlock(ip);
    fs_end_op(sb);
    iput(ip);
    return r;
}

int pc_writeback(struct super_block *sb, u32 ino, u64 first, u64 last, bool lock_wait)
{
    /* a snapshot of the dirty pages, then write each once (pages still
       mapped writable stay dirty: a loop until none were left would spin) */
    size_t cap = __atomic_load_n(&nr_file, __ATOMIC_RELAXED) + 16;
    struct page **batch = kmalloc(cap * sizeof(*batch));
    if (!batch)
        return -ENOMEM;
    size_t n = 0;
    unsigned long flags = spin_lock_irqsave(&pc_lock);
    struct page *pg;
    list_for_each_entry(pg, &pc_lru, list) {
        if (n == cap)
            break;
        if (!(pg->flags & PG_CACHE) || !(pg->flags & PG_DIRTY))
            continue;
        if (sb && (pg->cache.owner != sb || (ino && (pg->cache.ino != ino || pg->cache.index < first ||
                                                     pg->cache.index > last))))
            continue;
        page_get(pg);
        batch[n++] = pg;
    }
    spin_unlock_irqrestore(&pc_lock, flags);
    int err = 0;
    for (size_t i = 0; i < n; i++) {
        int r = write_page(batch[i], lock_wait);
        if (r && r != -EAGAIN && !err)
            err = r;
        page_put(batch[i]);
    }
    kfree(batch);
    return err;
}

unsigned long pc_shrink(unsigned long want, bool may_write)
{
    unsigned long freed = 0;
    unsigned long budget = 2 * want + 64;
    while (freed < want && budget) {
        enum { BATCH = 32 };
        struct page *drop[BATCH], *write[BATCH];
        bool dsw[BATCH];
        u32 dslot[BATCH];
        int nd = 0, nw = 0;
        unsigned long flags = spin_lock_irqsave(&pc_lock);
        while (budget && nd < BATCH && nw < BATCH && freed + nd < want && !list_empty(&pc_lru)) {
            struct page *pg = list_first_entry(&pc_lru, struct page, list);
            budget--;
            list_move_tail(&pg->list, &pc_lru);        /* the clock hand moves on */
            if (pg->flags & PG_REFERENCED) {
                page_clear_flag(pg, PG_REFERENCED);
                continue;
            }
            if (__atomic_load_n(&pg->refcount, __ATOMIC_ACQUIRE) != 1)
                continue;                           /* mapped, or being read */
            bool sw = pg->flags & PG_SWAPCACHE;
            u32 slot = (u32)pg->cache.index;
            if (sw && swap_count(slot) == 1) {
                vmc.swap_out_skipped++;             /* no PTE needs the slot: just drop it */
            } else if (pg->flags & PG_DIRTY) {
                if (sw || may_write) {
                    page_get(pg);
                    write[nw++] = pg;
                }
                continue;
            }
            unhash_locked(pg);
            dsw[nd] = sw;
            dslot[nd] = slot;
            drop[nd++] = pg;
        }
        spin_unlock_irqrestore(&pc_lock, flags);
        for (int i = 0; i < nd; i++)
            release(drop[i], dsw[i], dslot[i]);
        freed += nd;
        for (int i = 0; i < nw; i++) {
            struct page *pg = write[i];
            int r;
            if (pg->flags & PG_SWAPCACHE) {
                r = swap_write((u32)pg->cache.index, pg);
                if (!r)
                    page_clear_flag(pg, PG_DIRTY);
            } else {
                r = write_page(pg, false);
            }
            /* written and still unused: free it now */
            flags = spin_lock_irqsave(&pc_lock);
            bool ok = !r && (pg->flags & (PG_CACHE | PG_SWAPCACHE)) && !(pg->flags & PG_DIRTY) &&
                      __atomic_load_n(&pg->refcount, __ATOMIC_ACQUIRE) == 2;
            bool sw = pg->flags & PG_SWAPCACHE;
            u32 slot = (u32)pg->cache.index;
            if (ok)
                unhash_locked(pg);
            spin_unlock_irqrestore(&pc_lock, flags);
            page_put(pg);                           /* ours */
            if (ok) {
                release(pg, sw, slot);
                freed++;
            }
        }
        if (!nd && !nw && budget && list_empty(&pc_lru))
            break;
    }
    return freed;
}

/* swapoff: the swap cache goes; pages still mapped stay as ordinary
   anonymous pages (copied on write if shared). */
void pc_drop_swap_cache(void)
{
    for (;;) {
        struct page *batch[32];
        u32 slots[32];
        int n = 0;
        unsigned long flags = spin_lock_irqsave(&pc_lock);
        struct page *pg, *tmp;
        list_for_each_entry_safe(pg, tmp, &pc_lru, list) {
            if (!(pg->flags & PG_SWAPCACHE))
                continue;
            slots[n] = (u32)pg->cache.index;
            unhash_locked(pg);
            batch[n++] = pg;
            if (n == 32)
                break;
        }
        spin_unlock_irqrestore(&pc_lock, flags);
        for (int i = 0; i < n; i++)
            release(batch[i], true, slots[i]);
        if (n < 32)
            return;
    }
}

/* Forget every second chance (dropping caches: everything unused goes). */
void pc_clear_referenced(void)
{
    unsigned long flags = spin_lock_irqsave(&pc_lock);
    struct page *pg;
    list_for_each_entry(pg, &pc_lru, list)
        page_clear_flag(pg, PG_REFERENCED);
    spin_unlock_irqrestore(&pc_lock, flags);
}

u64 pc_count(void)
{
    return __atomic_load_n(&nr_file, __ATOMIC_RELAXED) + __atomic_load_n(&nr_swap, __ATOMIC_RELAXED);
}

void pc_stats(u64 *file_pages, u64 *swap_pages, u64 *dirty, u64 *mapped)
{
    u64 d = 0, m = 0;
    unsigned long flags;
    bool locked = spin_lock_debug(&pc_lock, &flags);
    struct page *pg;
    list_for_each_entry(pg, &pc_lru, list) {
        if (pg->flags & PG_DIRTY)
            d++;
        if (pg->refcount > 1)
            m++;
    }
    *file_pages = nr_file;
    *swap_pages = nr_swap;
    spin_unlock_debug(&pc_lock, locked, flags);
    *dirty = d;
    *mapped = m;
}

/* Per file: the files with the most cached pages. */
void pc_print(print_fn print)
{
    u64 f, s, d, m;
    pc_stats(&f, &s, &d, &m);
    print("page cache: %lu file pages (%lu KiB), %lu swap-cache pages; %lu dirty, %lu in use\n",
          (unsigned long)f, (unsigned long)(f * 4), (unsigned long)s, (unsigned long)d,
          (unsigned long)m);
    print("lookups: %lu hits, %lu misses; %lu pages written back\n", (unsigned long)vmc.cache_hits,
          (unsigned long)vmc.cache_misses, (unsigned long)vmc.writebacks);
    enum { TOP = 16 };
    struct { const void *owner; u32 ino; u32 pages, dirty, mapped; } top[TOP];
    int n = 0;
    unsigned long flags;
    bool locked = spin_lock_debug(&pc_lock, &flags);
    struct page *pg;
    list_for_each_entry(pg, &pc_lru, list) {
        if (!(pg->flags & PG_CACHE))
            continue;
        int i = 0;
        while (i < n && (top[i].owner != pg->cache.owner || top[i].ino != pg->cache.ino))
            i++;
        if (i == n) {
            if (n == TOP)
                continue;
            top[n++] = (typeof(top[0])){ pg->cache.owner, pg->cache.ino, 0, 0, 0 };
        }
        top[i].pages++;
        top[i].dirty += !!(pg->flags & PG_DIRTY);
        top[i].mapped += pg->refcount > 1;
    }
    spin_unlock_debug(&pc_lock, locked, flags);
    if (n)
        print("  %-12s %7s %6s %6s %6s\n", "filesystem", "inode", "pages", "dirty", "mapped");
    for (int i = 0; i < n; i++) {
        const struct super_block *sb = top[i].owner;
        print("  %-12s %7u %6u %6u %6u\n", sb->path, top[i].ino, top[i].pages, top[i].dirty,
              top[i].mapped);
    }
    if (n == TOP)
        print("  (the first %d files on the LRU)\n", TOP);
}
