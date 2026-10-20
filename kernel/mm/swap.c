/*
 * Swap space: the partition named "swap" (format: swap_format.h), divided
 * into page-sized slots. Reclaim writes anonymous pages there when memory is
 * short; a swapped-out page's PTE holds its slot (PTE_SWAP, mm.h) and the
 * next touch reads it back (uvm.c).
 *
 * Each slot has a count of what refers to it: the PTEs holding it (a page
 * swapped out by several processes that shared it after fork has one slot,
 * not one each) plus one while its page is in the swap cache (pagecache.c),
 * which is where a page sits between being unmapped and being written, and
 * after being read back while other PTEs still need the slot. A slot is free
 * at count 0. Nothing about slots is on disk: swap starts empty every boot.
 *
 * Boot option noswap: don't use the partition. Debugging: /proc/swaps,
 * `swap` in the monitor, the swap_out/swap_in trace events, and the swap
 * columns of /proc/<pid>/maps.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <spinlock.h>
#include <swap_format.h>
#include <trace.h>
#include <uapi/errno.h>
#include <virtio.h>

#define SECTORS_PER_SLOT (PAGE_SIZE / 512)

static int vol = -1;                    /* blk_rw volume */
static u32 nslots;                      /* slot 0 is the header */
static u16 *counts;
static u32 nused, hint = 1, peak;
static spinlock_t swap_lock = SPINLOCK_INIT("swap");

bool swap_enabled(void) { return vol >= 0; }

void swap_init(void)
{
    if (cmdline_has("noswap")) {
        pr_info("swap: off (noswap)\n");
        return;
    }
    int v = -1;
    for (int i = 0; i < blk_count(); i++)
        if (!strcmp(blk_label(i), SWAP_NAME))
            v = i;
    if (v < 0) {
        pr_info("swap: no swap partition\n");
        return;
    }
    struct swap_header *h = page_alloc();
    if (!h)
        return;
    int r = blk_rw(v, 0, h, PAGE_SIZE, false);
    u64 cap = blk_capacity(v) / SECTORS_PER_SLOT;
    if (r || memcmp(h->magic, SWAP_MAGIC, 8) || h->version != 1 || h->npages < 2 || h->npages > cap) {
        pr_warn("swap: %s (\"%s\") has no valid swap header; not used\n", blk_name(v), SWAP_NAME);
        page_free(h);
        return;
    }
    u32 n = h->npages;
    page_free(h);
    counts = kzalloc(n * sizeof(*counts));
    if (!counts) {
        pr_warn("swap: no memory for the slot map of %u slots\n", n);
        return;
    }
    counts[0] = 0xffff;                 /* the header */
    nslots = n;
    vol = v;
    pr_info("swap: %s, %lu MiB (%u slots)\n", blk_name(v), (unsigned long)((u64)n * PAGE_SIZE >> 20), n - 1);
}

u32 swap_alloc(void)
{
    if (vol < 0)
        return 0;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    for (u32 i = 0; i < nslots - 1; i++) {
        u32 s = 1 + (hint - 1 + i) % (nslots - 1);
        if (!counts[s]) {
            counts[s] = 1;
            nused++;
            if (nused > peak)
                peak = nused;
            hint = s + 1 < nslots ? s + 1 : 1;
            spin_unlock_irqrestore(&swap_lock, flags);
            return s;
        }
    }
    spin_unlock_irqrestore(&swap_lock, flags);
    return 0;
}

static bool slot_ok(u32 slot, const char *what)
{
    if (slot && slot < nslots)
        return true;
    mm_corruption("swap_%s: slot %u out of range (1..%u): a corrupted swap PTE?", what, slot, nslots - 1);
    return false;
}

void swap_dup(u32 slot)
{
    if (!slot_ok(slot, "dup"))
        return;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    if (!counts[slot] || counts[slot] >= 0xfffe)
        mm_corruption("swap_dup: slot %u has count %u", slot, counts[slot]);
    else
        counts[slot]++;
    spin_unlock_irqrestore(&swap_lock, flags);
}

void swap_free(u32 slot)
{
    if (!slot_ok(slot, "free"))
        return;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    if (!counts[slot])
        mm_corruption("swap_free: slot %u is already free (double free)", slot);
    else if (--counts[slot] == 0)
        nused--;
    spin_unlock_irqrestore(&swap_lock, flags);
}

u32 swap_count(u32 slot)
{
    return slot && slot < nslots ? __atomic_load_n(&counts[slot], __ATOMIC_RELAXED) : 0;
}

int swap_write(u32 slot, struct page *pg)
{
    if (!slot_ok(slot, "write"))
        return -EINVAL;
    int r = blk_rw(vol, (u64)slot * SECTORS_PER_SLOT, page_to_va(pg), PAGE_SIZE, true);
    trace(swap_out, slot, page_to_pa(pg), r);
    if (!r)
        vmc.swap_outs++;
    return r;
}

int swap_read(u32 slot, struct page *pg)
{
    if (!slot_ok(slot, "read"))
        return -EINVAL;
    int r = blk_rw(vol, (u64)slot * SECTORS_PER_SLOT, page_to_va(pg), PAGE_SIZE, false);
    trace(swap_in, slot, page_to_pa(pg), r);
    if (!r)
        vmc.swap_ins++;
    return r;
}

void swap_stats(u64 *total, u64 *used)
{
    *total = nslots ? nslots - 1 : 0;
    *used = nused;
}

void swap_print(print_fn print)
{
    if (vol < 0) {
        print("no swap space\n");
        return;
    }
    print("%-10s %-8s %10s %10s %10s\n", "volume", "label", "size KiB", "used KiB", "peak KiB");
    print("%-10s %-8s %10lu %10lu %10lu\n", blk_name(vol), blk_label(vol),
          (unsigned long)(nslots - 1) * 4, (unsigned long)nused * 4, (unsigned long)peak * 4);
    print("pages written %lu, read %lu; dropped unwritten %lu\n", (unsigned long)vmc.swap_outs,
          (unsigned long)vmc.swap_ins, (unsigned long)vmc.swap_out_skipped);
    u32 shared = 0;
    for (u32 s = 1; s < nslots; s++)
        shared += counts[s] > 1;
    print("slots referenced more than once (shared after fork, or cached): %u\n", shared);
}
