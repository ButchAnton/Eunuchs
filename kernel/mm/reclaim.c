/*
 * Reclaim: getting memory back when it runs short.
 *
 * Free memory has three marks (pages): min (a reserve user allocations
 * leave for the kernel), low (below it kswapd wakes) and high (where kswapd
 * stops). Reclaim (one reclaimer at a time, under reclaim_lock) repeats:
 *
 *   1. pc_shrink: free cold page-cache and swap-cache pages nobody maps,
 *      writing them first if they are dirty (swap-cache pages to their swap
 *      slot; file pages to their file, which kswapd does but direct reclaim
 *      does not, since its caller may hold filesystem locks);
 *   2. if that was not enough, uvm_reclaim each process in turn: PTEs whose
 *      accessed bit is set get it cleared (a second chance); the others are
 *      unmapped: file pages just go (they can be read again), anonymous
 *      pages go to the swap cache with a swap slot. Pages unmapped from
 *      their last process are then freed by the next pc_shrink.
 *
 * kswapd runs this in the background; an allocation that finds memory below
 * min runs it itself (direct reclaim) before taking a page. kswapd also
 * writes dirty file pages back every few seconds.
 *
 * Debugging: /proc/vmstat (the counters), /proc/meminfo, `vmstat` in the
 * monitor, the reclaim trace event, and the reclaim/kswapd lines of `ps`.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <sched.h>
#include <trace.h>

struct vm_counters vmc;

static u64 min_wm, low_wm, high_wm;
static struct thread *kswapd;
static struct waitqueue kswapd_wq;
static volatile bool kswapd_wanted;
static struct mutex reclaim_lock;
static int last_pid;                    /* uvm_reclaim goes round the processes */

#define WRITEBACK_INTERVAL_MS 5000

void kswapd_wake(void)
{
    if (!kswapd || kswapd_wanted)
        return;
    kswapd_wanted = true;
    wake_up(&kswapd_wq);
}

/*
 * The next process (by pid, after last_pid) with an mm we can lock without
 * waiting (a caller holding its own mm's lock fails the try, so it is
 * skipped); returns it locked. *wrapped says the end of the list was reached (last_pid starts
 * over). proc_lock keeps the mm alive until we hold its lock: exit and exec
 * unhook an mm from its process under proc_lock, then take its lock before
 * freeing it.
 */
static struct mm *next_mm(bool *wrapped)
{
    struct mm *found = NULL;
    *wrapped = false;
    unsigned long flags = spin_lock_irqsave(&proc_lock);
    struct proc *p, *best = NULL;
    list_for_each_entry(p, &all_procs, all_node)
        if (p->mm && p->pid > last_pid && (!best || p->pid < best->pid))
            best = p;
    if (!best) {
        last_pid = 0;
        *wrapped = true;
    } else {
        last_pid = best->pid;
        if (mutex_trylock(&best->mm->lock))
            found = best->mm;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return found;
}

unsigned long reclaim(unsigned long want, bool direct)
{
    unsigned long freed = 0, unmapped = 0, scanned = 0;
    if (direct) {
        /* kswapd (or another) is at it: let it work, then look again */
        if (!mutex_trylock(&reclaim_lock)) {
            sleep_ms(2);
            return 0;
        }
    } else {
        mutex_lock(&reclaim_lock);
    }
    for (int pass = 0; pass < 4 && freed < want; pass++) {
        freed += pc_shrink(want - freed, !direct);
        if (freed >= want)
            break;
        /* take pages from processes: a few from each, round the list (at
           most once round per pass) */
        unsigned long got = 0;
        int wraps = 0;
        for (int i = 0; i < 64 && got < 2 * (want - freed); i++) {
            bool wrapped;
            struct mm *mm = next_mm(&wrapped);
            if (wrapped && ++wraps > 1)
                break;
            if (!mm)
                continue;
            unsigned long s = 0;
            got += uvm_reclaim(mm, 32, &s);
            scanned += s;
            mutex_unlock(&mm->lock);
        }
        unmapped += got;
        if (!got && pass > 0)
            break;                      /* nothing left to take */
    }
    if (freed < want)
        freed += pc_shrink(want - freed, !direct);
    mutex_unlock(&reclaim_lock);
    vmc.scanned += scanned;
    vmc.unmapped += unmapped;
    vmc.freed += freed;
    trace(reclaim, want, freed, unmapped);
    return freed;
}

static void kswapd_main(void *arg)
{
    (void)arg;
    u64 next_writeback = rdtime() + ms_to_rdtime(WRITEBACK_INTERVAL_MS);
    bool stuck = false;                 /* the last run freed nothing */
    for (;;) {
        struct wait_entry we = { .t = current, .queued = false };
        prepare_to_wait(&kswapd_wq, &we);
        /* asleep until woken or the next writeback; at most a second, as
           kernel allocations don't wake us; after a fruitless run, a pause
           (an allocation that wakes us again ends it) */
        if (stuck || (!kswapd_wanted && pmm_free_pages() >= low_wm))
            schedule_until(MIN(next_writeback, rdtime() + ms_to_rdtime(stuck ? 20 : 1000)));
        finish_wait(&kswapd_wq, &we);
        kswapd_wanted = false;

        stuck = false;
        if (pmm_free_pages() < high_wm) {
            vmc.kswapd_runs++;
            while (pmm_free_pages() < high_wm) {
                if (!reclaim(high_wm - pmm_free_pages(), false)) {
                    stuck = true;
                    break;
                }
            }
        }
        if (rdtime() >= next_writeback) {
            pc_writeback(NULL, 0, 0, ~0ULL, false);
            next_writeback = rdtime() + ms_to_rdtime(WRITEBACK_INTERVAL_MS);
        }
    }
}

void reclaim_init(void)
{
    u64 total = pmm_total_pages();
    min_wm = MAX(total / 128, 64UL);
    low_wm = MAX(total / 64, 128UL);
    high_wm = MAX(total / 32, 256UL);
    waitqueue_init(&kswapd_wq, "kswapd");
    mutex_init(&reclaim_lock, "reclaim");
    swap_init();
    kswapd = kthread_create("kswapd", kswapd_main, NULL);
    if (kswapd)
        kthread_detach(kswapd);
    pr_info("reclaim: free memory marks min %lu, low %lu, high %lu pages; kswapd started\n",
            (unsigned long)min_wm, (unsigned long)low_wm, (unsigned long)high_wm);
}

struct page *user_page_alloc(bool zero)
{
    struct page *pg = NULL;
    int idle = 0;                       /* tries in a row that freed nothing anywhere */
    for (int tries = 0; tries < 1000 && idle < 8; tries++) {
        if (pmm_free_pages() > min_wm && (pg = alloc_pages(0)) != NULL)
            break;
        if (!tries)
            vmc.alloc_stalls++;
        kswapd_wake();
        vmc.direct_reclaims++;
        u64 before = __atomic_load_n(&vmc.freed, __ATOMIC_RELAXED);
        unsigned long n = reclaim(32, true);
        idle = n || __atomic_load_n(&vmc.freed, __ATOMIC_RELAXED) != before ? 0 : idle + 1;
    }
    if (!pg && !(pg = alloc_pages(0))) {    /* the reserve, as a last resort */
        vmc.oom++;
        pr_warn("out of memory for user pages: %lu pages free, nothing left to reclaim%s\n",
                (unsigned long)pmm_free_pages(), swap_enabled() ? "" : " (no swap)");
        return NULL;
    }
    pg->flags |= PG_USER;
    if (zero)
        memset(page_to_va(pg), 0, PAGE_SIZE);
    if (pmm_free_pages() < low_wm)
        kswapd_wake();
    return pg;
}

void vm_counters_print(print_fn print)
{
    u64 f, s, d, m, st, su;
    pc_stats(&f, &s, &d, &m);
    swap_stats(&st, &su);
    print("free %lu\nmin %lu\nlow %lu\nhigh %lu\n", (unsigned long)pmm_free_pages(),
          (unsigned long)min_wm, (unsigned long)low_wm, (unsigned long)high_wm);
    print("file_pages %lu\nswapcache_pages %lu\ndirty %lu\nmapped_cache %lu\n", (unsigned long)f,
          (unsigned long)s, (unsigned long)d, (unsigned long)m);
    print("swap_total %lu\nswap_used %lu\n", (unsigned long)st, (unsigned long)su);
#define C(x) print(#x " %lu\n", (unsigned long)vmc.x)
    C(faults); C(major_faults); C(cache_hits); C(cache_misses); C(swap_outs); C(swap_ins);
    C(swap_out_skipped); C(writebacks); C(scanned); C(unmapped); C(freed); C(kswapd_runs);
    C(direct_reclaims); C(alloc_stalls); C(oom);
#undef C
    print("asid_bits %u\n", asid_bits);
}

void vm_memory_print(print_fn print)
{
    u64 f, s, d, m, st, su;
    pc_stats(&f, &s, &d, &m);
    swap_stats(&st, &su);
    print("page cache %6lu KiB (%lu KiB dirty, %lu pages mapped)\n", (unsigned long)f * 4,
          (unsigned long)d * 4, (unsigned long)m);
    print("swap cache %6lu KiB\n", (unsigned long)s * 4);
    if (st)
        print("swap       %6lu KiB used of %lu KiB\n", (unsigned long)su * 4, (unsigned long)st * 4);
    else
        print("swap       none\n");
}
