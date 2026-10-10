#ifndef EUNUCHS_BUDDY_H
#define EUNUCHS_BUDDY_H

/*
 * Binary buddy allocator core: pure bookkeeping over a struct page array,
 * no locking, no memory access to the frames themselves. pmm.c wraps it with
 * locking, poisoning and statistics; tests/host/test_buddy.c drives it on the
 * host.
 */

#include <mm.h>

struct buddy_zone {
    struct page *pages;                 /* pages[i] describes pfn base_pfn + i */
    u64 base_pfn;
    u64 npages;
    struct list_head free[MAX_ORDER + 1];
    u64 nr_free[MAX_ORDER + 1];         /* free blocks per order */
    u64 free_pages;
};

/* All pages start PG_RESERVED. */
void buddy_init(struct buddy_zone *z, struct page *pages, u64 base_pfn, u64 npages);
/* Release [start, end) pfns (boot): clears PG_RESERVED, frees in max-size blocks. */
void buddy_release_range(struct buddy_zone *z, u64 start_pfn, u64 end_pfn);
struct page *buddy_alloc(struct buddy_zone *z, unsigned order);
/* Returns 0, or -1 if pg is not an allocated block head (caller reports). */
int buddy_free(struct buddy_zone *z, struct page *pg, unsigned order);

static inline u64 buddy_pfn(const struct buddy_zone *z, const struct page *pg)
{
    return z->base_pfn + (u64)(pg - z->pages);
}

static inline struct page *buddy_page(struct buddy_zone *z, u64 pfn)
{
    if (pfn < z->base_pfn || pfn >= z->base_pfn + z->npages)
        return NULL;
    return &z->pages[pfn - z->base_pfn];
}

#endif
