#include "buddy.h"

void buddy_init(struct buddy_zone *z, struct page *pages, u64 base_pfn, u64 npages)
{
    z->pages = pages;
    z->base_pfn = base_pfn;
    z->npages = npages;
    z->free_pages = 0;
    for (int o = 0; o <= MAX_ORDER; o++) {
        list_init(&z->free[o]);
        z->nr_free[o] = 0;
    }
    for (u64 i = 0; i < npages; i++) {
        struct page *pg = &pages[i];
        pg->list.next = pg->list.prev = NULL;
        pg->flags = PG_RESERVED;
        pg->order = 0;
        pg->refcount = 0;
        pg->head = NULL;
        pg->alloc_site = pg->free_site = 0;
    }
}

static void add_free_block(struct buddy_zone *z, struct page *pg, unsigned order)
{
    pg->flags = PG_FREE;
    pg->order = order;
    /* FIFO: freed blocks are reused last, giving use-after-free bugs the
     * longest window to be caught by poison checks. */
    list_add_tail(&pg->list, &z->free[order]);
    z->nr_free[order]++;
}

static void remove_free_block(struct buddy_zone *z, struct page *pg, unsigned order)
{
    list_del(&pg->list);
    z->nr_free[order]--;
    pg->flags &= ~PG_FREE;
}

/* Insert a block, merging with free buddies as far as possible. */
static void free_block(struct buddy_zone *z, u64 pfn, unsigned order)
{
    while (order < MAX_ORDER) {
        u64 bpfn = pfn ^ (1UL << order);
        struct page *buddy = buddy_page(z, bpfn);
        if (!buddy || !(buddy->flags & PG_FREE) || buddy->order != order)
            break;
        /* The buddy block must lie entirely inside the zone. */
        if (bpfn + (1UL << order) > z->base_pfn + z->npages)
            break;
        remove_free_block(z, buddy, order);
        buddy->flags = 0;
        pfn &= ~(1UL << order);
        order++;
    }
    add_free_block(z, buddy_page(z, pfn), order);
}

void buddy_release_range(struct buddy_zone *z, u64 start, u64 end)
{
    if (start < z->base_pfn)
        start = z->base_pfn;
    if (end > z->base_pfn + z->npages)
        end = z->base_pfn + z->npages;

    for (u64 pfn = start; pfn < end; pfn++)
        buddy_page(z, pfn)->flags = 0;

    while (start < end) {
        unsigned order = 0;
        while (order < MAX_ORDER &&
               !(start & ((1UL << (order + 1)) - 1)) &&
               start + (1UL << (order + 1)) <= end)
            order++;
        free_block(z, start, order);
        z->free_pages += 1UL << order;
        start += 1UL << order;
    }
}

struct page *buddy_alloc(struct buddy_zone *z, unsigned order)
{
    if (order > MAX_ORDER)
        return NULL;

    unsigned o = order;
    while (o <= MAX_ORDER && list_empty(&z->free[o]))
        o++;
    if (o > MAX_ORDER)
        return NULL;

    struct page *pg = list_first_entry(&z->free[o], struct page, list);
    remove_free_block(z, pg, o);

    /* Split, returning upper halves to the free lists. */
    while (o > order) {
        o--;
        add_free_block(z, pg + (1UL << o), o);
    }

    pg->flags = 0;
    pg->order = order;
    pg->head = NULL;
    for (u64 i = 1; i < (1UL << order); i++) {
        pg[i].flags = PG_TAIL;
        pg[i].head = pg;
    }
    z->free_pages -= 1UL << order;
    return pg;
}

int buddy_free(struct buddy_zone *z, struct page *pg, unsigned order)
{
    if (order > MAX_ORDER || (pg->flags & (PG_FREE | PG_RESERVED | PG_TAIL)))
        return -1;
    u64 pfn = buddy_pfn(z, pg);
    if (pfn & ((1UL << order) - 1))
        return -1;

    for (u64 i = 1; i < (1UL << order); i++) {
        pg[i].flags = 0;
        pg[i].head = NULL;
    }
    pg->flags = 0;
    z->free_pages += 1UL << order;
    free_block(z, pfn, order);
    return 0;
}
