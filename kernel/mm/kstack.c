/*
 * Kernel thread stacks in KSTACK_AREA: KSTACK_SIZE of mapped stack on top of
 * an equally sized unmapped guard, per KSTACK_SLOT. New stacks are filled
 * with STACK_POISON so `ps` can report each thread's high-water mark.
 *
 * Freeing a stack means unmapping it, and with SMP every other CPU may still
 * cache its translation: a TLB shootdown (IPIs to all CPUs) per freed
 * stack. A small cache of freed stacks that stay mapped makes the common
 * create/exit churn cheap; only stacks beyond KSTACK_CACHE are unmapped.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <smp.h>
#include <spinlock.h>

#define KSTACK_CACHE 32

static spinlock_t kstack_lock = SPINLOCK_INIT("kstack");
static u64 slot_used[KSTACK_MAX / 64];
static void *cache[KSTACK_CACHE];       /* freed, still mapped */
static int ncached;
static u64 cache_hits, unmapped;

static uintptr_t slot_base(int idx)
{
    return KSTACK_AREA + (uintptr_t)idx * KSTACK_SLOT + (KSTACK_SLOT - KSTACK_SIZE);
}

static void release_slot(int idx)
{
    unsigned long flags = spin_lock_irqsave(&kstack_lock);
    slot_used[idx / 64] &= ~(1UL << (idx % 64));
    spin_unlock_irqrestore(&kstack_lock, flags);
}

/*
 * Unmap and free the first `len` bytes of pages mapped at base. Other CPUs
 * may still cache translations for the stack (its thread may have run
 * there), so shoot those down before the pages can be reused.
 */
static void unmap_pages(uintptr_t base, size_t len)
{
    uintptr_t pas[KSTACK_SIZE / PAGE_SIZE];
    int n = 0;
    for (uintptr_t off = 0; off < len; off += PAGE_SIZE) {
        if (vm_translate(kernel_pagetable, base + off, &pas[n], NULL))
            panic("kstack: stack page %#lx not mapped", (unsigned long)(base + off));
        vm_unmap(kernel_pagetable, base + off, PAGE_SIZE);
        n++;
    }
    tlb_shootdown();
    for (int i = 0; i < n; i++)
        page_free(__va(pas[i]));
}

void *kstack_alloc(void)
{
    int idx = -1;
    unsigned long flags = spin_lock_irqsave(&kstack_lock);
    if (ncached) {
        void *base = cache[--ncached];
        cache_hits++;
        spin_unlock_irqrestore(&kstack_lock, flags);
        memset(base, STACK_POISON, KSTACK_SIZE);
        return base;
    }
    for (int i = 0; i < KSTACK_MAX; i++) {
        if (!(slot_used[i / 64] & (1UL << (i % 64)))) {
            slot_used[i / 64] |= 1UL << (i % 64);
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&kstack_lock, flags);
    if (idx < 0) {
        pr_warn("kstack_alloc: all %d stack slots in use\n", KSTACK_MAX);
        return NULL;
    }

    uintptr_t base = slot_base(idx);
    for (uintptr_t off = 0; off < KSTACK_SIZE; off += PAGE_SIZE) {
        void *pg = page_alloc();
        if (!pg || vm_map(kernel_pagetable, base + off, __pa(pg), PAGE_SIZE, PROT_KERNEL_RW)) {
            page_free(pg);
            unmap_pages(base, off);
            release_slot(idx);
            return NULL;
        }
    }
    memset((void *)base, STACK_POISON, KSTACK_SIZE);
    return (void *)base;
}

void kstack_free(void *base_ptr)
{
    uintptr_t base = (uintptr_t)base_ptr;
    int idx = (base - KSTACK_AREA) / KSTACK_SLOT;
    if (base < KSTACK_AREA || idx >= KSTACK_MAX || base != slot_base(idx))
        panic("kstack_free: %p is not a kernel stack", base_ptr);

    unsigned long flags = spin_lock_irqsave(&kstack_lock);
    if (ncached < KSTACK_CACHE) {
        cache[ncached++] = base_ptr;
        spin_unlock_irqrestore(&kstack_lock, flags);
        return;
    }
    unmapped++;
    spin_unlock_irqrestore(&kstack_lock, flags);
    unmap_pages(base, KSTACK_SIZE);
    release_slot(idx);
}

void kstack_stats(u64 *in_use, u64 *cached, u64 *hits, u64 *unmaps)
{
    unsigned long flags = spin_lock_irqsave(&kstack_lock);
    u64 n = 0;
    for (int i = 0; i < KSTACK_MAX / 64; i++)
        for (u64 w = slot_used[i]; w; w &= w - 1)      /* no libgcc popcount */
            n++;
    *in_use = n - ncached;
    *cached = ncached;
    *hits = cache_hits;
    *unmaps = unmapped;
    spin_unlock_irqrestore(&kstack_lock, flags);
}

bool addr_in_stack_guard(uintptr_t addr)
{
    if (addr >= KSTACK_AREA && addr < KSTACK_AREA + (uintptr_t)KSTACK_MAX * KSTACK_SLOT)
        return (addr - KSTACK_AREA) % KSTACK_SLOT < KSTACK_SLOT - KSTACK_SIZE;
    if (addr >= (uintptr_t)__stacks_start && addr < (uintptr_t)__stacks_end)
        return (addr - (uintptr_t)__stacks_start) % KSTACK_SLOT < KSTACK_SLOT - KSTACK_SIZE;
    return false;
}
