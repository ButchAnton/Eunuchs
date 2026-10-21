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

static int swap_vol = -1;                    /* blk_rw volume */
static volatile bool swap_draining;          /* swapoff under way: no new slots */
static u32 swap_nslots;                      /* slot 0 is the header */
static u16 *slot_counts;
static u32 swap_nused, hint = 1, swap_peak;
static spinlock_t swap_lock = SPINLOCK_INIT("swap");

/* Swapping out is allowed (swap is on and not being turned off). */
bool swap_enabled(void) { return swap_vol >= 0 && !swap_draining; }
int swap_volume(void) { return swap_vol; }

/* The volume called name: "disk0p5", "/dev/disk0p5", or a GPT name ("swap"). */
int swap_find_volume(const char *name)
{
    if (!strncmp(name, "/dev/", 5))
        name += 5;
    for (int i = 0; i < blk_count(); i++)
        if (!strcmp(blk_name(i), name) || !strcmp(blk_label(i), name))
            return i;
    return -ENODEV;
}

int swap_on(int v)
{
    if (swap_vol >= 0)
        return -EBUSY;
    struct swap_header *h = page_alloc();
    if (!h)
        return -ENOMEM;
    int r = blk_rw(v, 0, h, PAGE_SIZE, false);
    u64 cap = blk_capacity(v) / SECTORS_PER_SLOT;
    if (r || memcmp(h->magic, SWAP_MAGIC, 8) || h->version != 1 || h->npages < 2 || h->npages > cap) {
        pr_warn("swap: %s has no valid swap header; not used\n", blk_name(v));
        page_free(h);
        return -EINVAL;
    }
    u32 n = h->npages;
    page_free(h);
    u16 *c = kzalloc(n * sizeof(*c));
    if (!c) {
        pr_warn("swap: no memory for the slot map of %u slots\n", n);
        return -ENOMEM;
    }
    c[0] = 0xffff;                      /* the header */
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    slot_counts = c;
    swap_nslots = n;
    swap_nused = 0;
    hint = 1;
    swap_vol = v;
    spin_unlock_irqrestore(&swap_lock, flags);
    pr_info("swap: %s, %lu MiB (%u slots)\n", blk_name(v), (unsigned long)((u64)n * PAGE_SIZE >> 20), n - 1);
    return 0;
}

void swap_init(void)
{
    if (cmdline_has("noswap")) {
        pr_info("swap: off (noswap)\n");
        return;
    }
    int v = swap_find_volume(SWAP_NAME);
    if (v < 0)
        pr_info("swap: no swap partition\n");
    else
        swap_on(v);
}

/* swapoff, from reclaim.c: stop new slots; at the end, with every slot back
   to 0, forget the volume (or, failing, carry on as before). */
void swap_drain(bool on) { swap_draining = on; }

u32 swap_used(void) { return swap_nused; }

void swap_retire(void)
{
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    u16 *c = slot_counts;
    int v = swap_vol;
    swap_vol = -1;
    slot_counts = NULL;
    swap_nslots = 0;
    swap_nused = 0;
    swap_draining = false;
    spin_unlock_irqrestore(&swap_lock, flags);
    kfree(c);
    pr_info("swap: %s off\n", blk_name(v));
}

u32 swap_alloc(void)
{
    if (!swap_enabled())
        return 0;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    for (u32 i = 0; i < swap_nslots - 1; i++) {
        u32 s = 1 + (hint - 1 + i) % (swap_nslots - 1);
        if (!slot_counts[s]) {
            slot_counts[s] = 1;
            swap_nused++;
            if (swap_nused > swap_peak)
                swap_peak = swap_nused;
            hint = s + 1 < swap_nslots ? s + 1 : 1;
            spin_unlock_irqrestore(&swap_lock, flags);
            return s;
        }
    }
    spin_unlock_irqrestore(&swap_lock, flags);
    return 0;
}

static bool slot_ok(u32 slot, const char *what)
{
    if (slot && slot < swap_nslots)
        return true;
    mm_corruption("swap_%s: slot %u out of range (1..%u): a corrupted swap PTE?", what, slot, swap_nslots - 1);
    return false;
}

void swap_dup(u32 slot)
{
    if (!slot_ok(slot, "dup"))
        return;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    if (!slot_counts[slot] || slot_counts[slot] >= 0xfffe)
        mm_corruption("swap_dup: slot %u has count %u", slot, slot_counts[slot]);
    else
        slot_counts[slot]++;
    spin_unlock_irqrestore(&swap_lock, flags);
}

void swap_free(u32 slot)
{
    if (!slot_ok(slot, "free"))
        return;
    unsigned long flags = spin_lock_irqsave(&swap_lock);
    if (!slot_counts[slot])
        mm_corruption("swap_free: slot %u is already free (double free)", slot);
    else if (--slot_counts[slot] == 0)
        swap_nused--;
    spin_unlock_irqrestore(&swap_lock, flags);
}

u32 swap_count(u32 slot)
{
    return slot && slot < swap_nslots ? __atomic_load_n(&slot_counts[slot], __ATOMIC_RELAXED) : 0;
}

int swap_write(u32 slot, struct page *pg)
{
    if (!slot_ok(slot, "write"))
        return -EINVAL;
    int r = blk_rw(swap_vol, (u64)slot * SECTORS_PER_SLOT, page_to_va(pg), PAGE_SIZE, true);
    trace(swap_out, slot, page_to_pa(pg), r);
    if (!r)
        vmc.swap_outs++;
    return r;
}

int swap_read(u32 slot, struct page *pg)
{
    if (!slot_ok(slot, "read"))
        return -EINVAL;
    int r = blk_rw(swap_vol, (u64)slot * SECTORS_PER_SLOT, page_to_va(pg), PAGE_SIZE, false);
    trace(swap_in, slot, page_to_pa(pg), r);
    if (!r)
        vmc.swap_ins++;
    return r;
}

void swap_stats(u64 *total, u64 *used)
{
    *total = swap_nslots ? swap_nslots - 1 : 0;
    *used = swap_nused;
}

void swap_print(print_fn print)
{
    if (swap_vol < 0) {
        print("no swap space\n");
        return;
    }
    print("%-10s %-8s %10s %10s %10s\n", "volume", "label", "size KiB", "used KiB", "swap_peak KiB");
    print("%-10s %-8s %10lu %10lu %10lu\n", blk_name(swap_vol), blk_label(swap_vol),
          (unsigned long)(swap_nslots - 1) * 4, (unsigned long)swap_nused * 4, (unsigned long)swap_peak * 4);
    print("pages written %lu, read %lu; dropped unwritten %lu\n", (unsigned long)vmc.swap_outs,
          (unsigned long)vmc.swap_ins, (unsigned long)vmc.swap_out_skipped);
    u32 shared = 0;
    for (u32 s = 1; s < swap_nslots; s++)
        shared += slot_counts[s] > 1;
    print("slots referenced more than once (shared after fork, or cached): %u\n", shared);
}
