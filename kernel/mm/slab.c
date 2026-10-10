/*
 * Slab allocator and kmalloc.
 *
 * Every object slot is laid out as
 *
 *   [ left redzone 8 ][ object (size class) ][ right redzone 8 ][ meta 32 ]
 *
 * and is fully checked:
 *   - redzones and the slack between the requested size and the size class
 *     hold POISON_REDZONE; kfree reports any byte changed (buffer overflow /
 *     underflow) with the offset, size and allocation site
 *   - freed objects are filled with POISON_FREE and checked when reused
 *     (use-after-free writes); freelists are FIFO so reuse is delayed
 *   - meta tracks state, allocation and free sites: double frees and frees of
 *     foreign/interior pointers are reported with both sites
 *   - new objects are filled with POISON_UNINIT so uninitialized pointer use
 *     faults at a recognizable address (see vm_fault_hint)
 *
 * Allocations above KMALLOC_MAX go straight to the page allocator (PG_LARGE)
 * with the tail slack redzoned. `slab` / `slab leaks` in the monitor and
 * eu-slab in GDB show the state.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <spinlock.h>

#define REDZONE      8
#define KMALLOC_MAX  2048
#define MAX_CACHES   32
#define MAX_SLAB_ORDER 3

enum obj_state {
    OBJ_FREE  = 0xf4ee0bec,
    OBJ_ALLOC = 0xa1100c8d,
};

struct obj_meta {
    void *next_free;
    uintptr_t alloc_site;
    uintptr_t free_site;
    u32 state;
    u32 req_size;
};

struct kmem_cache {
    char name[24];
    size_t obj_size;            /* object area (size class) */
    size_t slot_size;           /* redzones + object + meta */
    unsigned order;             /* pages per slab = 2^order */
    u32 objs_per_slab;
    struct list_head partial, full, empty;
    u64 nr_slabs, active, peak, allocs, frees;
    spinlock_t lock;
};

static struct kmem_cache caches[MAX_CACHES];
static int nr_caches;
static struct kmem_cache *kmalloc_caches[16];
static const size_t kmalloc_sizes[] = { 8, 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048 };

static u64 large_allocs, large_frees, large_active_bytes;
static bool slab_ready;

static const char *site_str(char *buf, size_t n, uintptr_t site)
{
    if (!site)
        return "?";
    ksym_format(buf, n, site);
    return buf;
}

const char *kmem_cache_name(const struct kmem_cache *c)
{
    return c ? c->name : "?";
}

/* ---- slot geometry ---- */

static inline u8 *slot_obj(u8 *slot) { return slot + REDZONE; }
static inline struct obj_meta *slot_meta(struct kmem_cache *c, u8 *slot)
{
    return (struct obj_meta *)(slot + REDZONE + c->obj_size + REDZONE);
}
static inline u8 *obj_slot(void *obj) { return (u8 *)obj - REDZONE; }

static u8 *slab_base(struct page *pg) { return page_to_va(pg); }

static void slab_page_set(struct page *pg, struct kmem_cache *c)
{
    pg->flags |= PG_SLAB;
    pg->slab.cache = c;
    pg->slab.free_head = pg->slab.free_tail = NULL;
    pg->slab.inuse = 0;
}

/* ---- cache creation ---- */

struct kmem_cache *kmem_cache_create(const char *name, size_t size)
{
    if (nr_caches == MAX_CACHES)
        panic("kmem_cache_create(%s): too many caches", name);
    struct kmem_cache *c = &caches[nr_caches++];

    memset(c, 0, sizeof(*c));
    strlcpy(c->name, name, sizeof(c->name));
    c->obj_size = ALIGN_UP(size ? size : 1, 8);
    c->slot_size = REDZONE + c->obj_size + REDZONE + sizeof(struct obj_meta);
    c->order = 0;
    while (c->order < MAX_SLAB_ORDER && (PAGE_SIZE << c->order) / c->slot_size < 8)
        c->order++;
    c->objs_per_slab = (PAGE_SIZE << c->order) / c->slot_size;
    list_init(&c->partial);
    list_init(&c->full);
    list_init(&c->empty);
    c->lock = (spinlock_t)SPINLOCK_INIT(c->name);
    return c;
}

void slab_init(void)
{
    char name[24];
    for (size_t i = 0; i < ARRAY_SIZE(kmalloc_sizes); i++) {
        ksnprintf(name, sizeof(name), "kmalloc-%zu", kmalloc_sizes[i]);
        kmalloc_caches[i] = kmem_cache_create(name, kmalloc_sizes[i]);
    }
    slab_ready = true;
    pr_info("slab allocator ready: %zu kmalloc caches, redzones %d bytes, "
            "large allocations > %d bytes use pages\n",
            ARRAY_SIZE(kmalloc_sizes), REDZONE, KMALLOC_MAX);
}

/* ---- slabs ---- */

static struct page *new_slab(struct kmem_cache *c)
{
    struct page *pg = alloc_pages_site(c->order, (uintptr_t)new_slab);
    if (!pg)
        return NULL;
    slab_page_set(pg, c);
    u8 *base = slab_base(pg);

    for (u32 i = 0; i < c->objs_per_slab; i++) {
        u8 *slot = base + i * c->slot_size;
        struct obj_meta *m = slot_meta(c, slot);
        memset(slot, POISON_REDZONE, REDZONE);
        memset(slot_obj(slot), POISON_FREE, c->obj_size);
        memset(slot_obj(slot) + c->obj_size, POISON_REDZONE, REDZONE);
        m->state = OBJ_FREE;
        m->alloc_site = m->free_site = 0;
        m->req_size = 0;
        m->next_free = NULL;
        if (pg->slab.free_tail)
            slot_meta(c, pg->slab.free_tail)->next_free = slot;
        else
            pg->slab.free_head = slot;
        pg->slab.free_tail = slot;
    }
    c->nr_slabs++;
    return pg;
}

static void check_redzones(struct kmem_cache *c, u8 *slot, const char *when)
{
    struct obj_meta *m = slot_meta(c, slot);
    char s1[96], s2[96];
    size_t req = m->state == OBJ_ALLOC ? m->req_size : c->obj_size;

    u8 *bad = memchr_inv(slot, POISON_REDZONE, REDZONE);
    if (bad) {
        mm_corruption_dump((uintptr_t)slot, REDZONE + 16, "heap underflow in %s object %p (%u bytes, allocated at %s): "
                      "redzone byte at offset -%ld overwritten (found %#x), detected at %s",
                      c->name, slot_obj(slot), m->req_size,
                      site_str(s1, sizeof(s1), m->alloc_site),
                      (long)(slot_obj(slot) - bad), *bad, when);
        return;
    }

    /* Right redzone = slack after the requested size + the guard bytes. */
    u8 *rz = slot_obj(slot) + req;
    size_t rzlen = c->obj_size - req + REDZONE;
    bad = memchr_inv(rz, POISON_REDZONE, rzlen);
    if (bad) {
        u8 *last = bad;
        for (u8 *p = bad; p < rz + rzlen; p++)
            if (*p != POISON_REDZONE)
                last = p;
        uintptr_t dump = ALIGN_DOWN((uintptr_t)rz, 16);
        if (m->state == OBJ_ALLOC)
            mm_corruption_dump(dump, 32, "heap overflow in %s object %p (%zu bytes requested, allocated at %s): "
                          "bytes +%ld..+%ld written past the end, detected at %s",
                          c->name, slot_obj(slot), req, site_str(s1, sizeof(s1), m->alloc_site),
                          (long)(bad - slot_obj(slot)), (long)(last - slot_obj(slot)), when);
        else
            mm_corruption_dump(dump, 32, "redzone after free %s object %p overwritten (bytes +%ld..+%ld), "
                          "detected at %s; object last freed at %s", c->name, slot_obj(slot),
                          (long)(bad - slot_obj(slot)), (long)(last - slot_obj(slot)), when,
                          site_str(s2, sizeof(s2), m->free_site));
    }
}

/* Take a free slot from the cache (under the lock); NULL if out of memory. */
static u8 *take_slot(struct kmem_cache *c)
{
    unsigned long flags = spin_lock_irqsave(&c->lock);
    struct page *pg = NULL;

    if (!list_empty(&c->partial)) {
        pg = list_first_entry(&c->partial, struct page, list);
    } else if (!list_empty(&c->empty)) {
        pg = list_first_entry(&c->empty, struct page, list);
        list_del(&pg->list);
        list_add(&pg->list, &c->partial);
    } else if ((pg = new_slab(c))) {
        list_add(&pg->list, &c->partial);
    }

    u8 *slot = NULL;
    if (pg) {
        slot = pg->slab.free_head;
        pg->slab.free_head = slot_meta(c, slot)->next_free;
        if (!pg->slab.free_head)
            pg->slab.free_tail = NULL;
        if (++pg->slab.inuse == c->objs_per_slab) {
            list_del(&pg->list);
            list_add(&pg->list, &c->full);
        }
        c->allocs++;
        if (++c->active > c->peak)
            c->peak = c->active;
    }
    spin_unlock_irqrestore(&c->lock, flags);
    return slot;
}

void *kmem_cache_alloc_site(struct kmem_cache *c, uintptr_t site)
{
    u8 *slot = take_slot(c);
    if (!slot)
        return NULL;

    struct obj_meta *m = slot_meta(c, slot);
    char s1[96], s2[96];
    if (m->state != OBJ_FREE)
        mm_corruption("%s freelist corrupted: slot %p has state %#x", c->name, slot, m->state);
    u8 *bad = memchr_inv(slot_obj(slot), POISON_FREE, c->obj_size);
    if (bad) {
        mm_corruption_dump(ALIGN_DOWN((uintptr_t)bad, 16), 32,
                           "use-after-free write to %s object %p: byte +%ld changed to %#x "
                           "after it was freed at %s (allocated at %s)",
                           c->name, slot_obj(slot), (long)(bad - slot_obj(slot)), *bad,
                           site_str(s1, sizeof(s1), m->free_site),
                           site_str(s2, sizeof(s2), m->alloc_site));
    }
    check_redzones(c, slot, "allocation");

    m->state = OBJ_ALLOC;
    m->alloc_site = site;
    m->req_size = c->obj_size;
    m->next_free = NULL;
    void *obj = slot_obj(slot);
    memset(obj, POISON_UNINIT, c->obj_size);
    return obj;
}

static void obj_free(struct kmem_cache *c, struct page *pg, u8 *slot, uintptr_t site)
{
    struct obj_meta *m = slot_meta(c, slot);
    char s1[96], s2[96], s3[96];

    if (m->state == OBJ_FREE) {
        mm_corruption("double free of %s object %p: already freed at %s (allocated at %s), "
                      "freed again at %s", c->name, slot_obj(slot),
                      site_str(s1, sizeof(s1), m->free_site),
                      site_str(s2, sizeof(s2), m->alloc_site),
                      site_str(s3, sizeof(s3), site));
        return;
    }
    if (m->state != OBJ_ALLOC) {
        mm_corruption("kfree(%p): %s object metadata corrupted (state %#x)", slot_obj(slot),
                      c->name, m->state);
        return;
    }
    check_redzones(c, slot, "kfree");

    /* Restore all guard bytes so a reported overflow is not re-reported forever. */
    memset(slot, POISON_REDZONE, REDZONE);
    memset(slot_obj(slot), POISON_FREE, c->obj_size);
    memset(slot_obj(slot) + c->obj_size, POISON_REDZONE, REDZONE);
    m->state = OBJ_FREE;
    m->free_site = site;
    m->next_free = NULL;

    unsigned long flags = spin_lock_irqsave(&c->lock);
    if (pg->slab.free_tail)
        slot_meta(c, pg->slab.free_tail)->next_free = slot;
    else
        pg->slab.free_head = slot;
    pg->slab.free_tail = slot;

    bool was_full = pg->slab.inuse == c->objs_per_slab;
    pg->slab.inuse--;
    c->frees++;
    c->active--;
    struct page *release = NULL;
    if (pg->slab.inuse == 0) {
        list_del(&pg->list);
        /* Keep one empty slab cached; return the rest to the page allocator. */
        if (list_empty(&c->empty)) {
            list_add(&pg->list, &c->empty);
        } else {
            release = pg;
            c->nr_slabs--;
        }
    } else if (was_full) {
        list_del(&pg->list);
        list_add_tail(&pg->list, &c->partial);
    }
    spin_unlock_irqrestore(&c->lock, flags);

    if (release) {
        release->flags &= ~PG_SLAB;
        free_pages(release, c->order);
    }
}

/* Validate a pointer as a slab object; returns its slot or NULL (reported). */
static u8 *find_slot(const void *p, struct page **pg_out, const char *op)
{
    struct page *pg = va_to_page(p);
    if (!pg) {
        mm_corruption("%s(%p): not a heap pointer (outside managed RAM)", op, p);
        return NULL;
    }
    pg = compound_head(pg);
    *pg_out = pg;
    if (!(pg->flags & PG_SLAB))
        return NULL;
    struct kmem_cache *c = pg->slab.cache;
    size_t off = (u8 *)p - slab_base(pg);
    size_t idx = off / c->slot_size;
    u8 *slot = slab_base(pg) + idx * c->slot_size;
    if (idx >= c->objs_per_slab || (u8 *)p != slot_obj(slot)) {
        struct obj_meta *m = slot_meta(c, slot);
        char s[96];
        mm_corruption("%s(%p): pointer is %ld bytes into %s object %p (allocated at %s), "
                      "not its start", op, p, (long)((u8 *)p - slot_obj(slot)), c->name,
                      slot_obj(slot), site_str(s, sizeof(s), m->alloc_site));
        return NULL;
    }
    return slot;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    struct page *pg;
    if (!obj)
        return;
    u8 *slot = find_slot(obj, &pg, "kmem_cache_free");
    if (!slot)
        return;
    if (pg->slab.cache != c) {
        mm_corruption("kmem_cache_free(%s, %p): object belongs to cache %s", c->name, obj,
                      pg->slab.cache->name);
        return;
    }
    obj_free(c, pg, slot, (uintptr_t)__builtin_return_address(0));
}

/* ---- kmalloc ---- */

static struct kmem_cache *size_to_cache(size_t size)
{
    for (size_t i = 0; i < ARRAY_SIZE(kmalloc_sizes); i++)
        if (size <= kmalloc_sizes[i])
            return kmalloc_caches[i];
    return NULL;
}

static unsigned size_to_order(size_t size)
{
    unsigned order = 0;
    while ((PAGE_SIZE << order) < size)
        order++;
    return order;
}

void *kmalloc_site(size_t size, uintptr_t site)
{
    if (!slab_ready)
        panic("kmalloc before slab_init");

    struct kmem_cache *c = size_to_cache(size);
    if (c) {
        void *obj = kmem_cache_alloc_site(c, site);
        if (obj) {
            /* Tighten the redzone to the requested size. */
            struct obj_meta *m = slot_meta(c, obj_slot(obj));
            m->req_size = size;
            memset((u8 *)obj + size, POISON_REDZONE, c->obj_size - size);
        }
        return obj;
    }

    unsigned order = size_to_order(size);
    if (order > MAX_ORDER) {
        char s[96];
        pr_warn("kmalloc(%zu) from %s: too large\n", size, site_str(s, sizeof(s), site));
        return NULL;
    }
    struct page *pg = alloc_pages_site(order, site);
    if (!pg)
        return NULL;
    pg->flags |= PG_LARGE;
    pg->large_size = size;
    u8 *p = page_to_va(pg);
    memset(p, POISON_UNINIT, size);
    memset(p + size, POISON_REDZONE, (PAGE_SIZE << order) - size);
    large_allocs++;
    large_active_bytes += size;
    return p;
}

void *kmalloc(size_t size)
{
    return kmalloc_site(size, (uintptr_t)__builtin_return_address(0));
}

void *kzalloc(size_t size)
{
    return kzalloc_site(size, (uintptr_t)__builtin_return_address(0));
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
    return kmem_cache_alloc_site(c, (uintptr_t)__builtin_return_address(0));
}

void *kzalloc_site(size_t size, uintptr_t site)
{
    void *p = kmalloc_site(size, site);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(const void *p)
{
    uintptr_t site = (uintptr_t)__builtin_return_address(0);
    struct page *pg;
    char s[96];

    if (!p)
        return;
    u8 *slot = find_slot(p, &pg, "kfree");
    if (slot) {
        obj_free(pg->slab.cache, pg, slot, site);
        return;
    }
    if (!pg || !(pg->flags & PG_LARGE)) {
        if (pg && (pg->flags & PG_RESERVED))
            mm_corruption("kfree(%p): not a heap pointer; it points into reserved memory (%s)",
                          p, pmm_region_name(__pa(p)) ?: "?");
        else if (pg && (pg->flags & PG_FREE))
            mm_corruption("kfree(%p): page is free; double free of a large kmalloc? "
                          "(last freed at %s)", p, site_str(s, sizeof(s), pg->free_site));
        else if (pg && !(pg->flags & PG_SLAB))
            mm_corruption("kfree(%p): not allocated by kmalloc (page flags %#x, "
                          "page allocated at %s)", p, pg->flags,
                          site_str(s, sizeof(s), pg->alloc_site));
        return;
    }
    if (p != page_to_va(pg)) {
        mm_corruption("kfree(%p): pointer is %ld bytes into a %zu-byte allocation from %s",
                      p, (long)((u8 *)p - (u8 *)page_to_va(pg)), pg->large_size,
                      site_str(s, sizeof(s), pg->alloc_site));
        return;
    }
    size_t size = pg->large_size;
    size_t slack = (PAGE_SIZE << pg->order) - size;
    u8 *bad = memchr_inv((u8 *)p + size, POISON_REDZONE, slack);
    if (bad)
        mm_corruption("heap overflow in large kmalloc %p (%zu bytes, allocated at %s): "
                      "byte +%ld written past the end", p, size,
                      site_str(s, sizeof(s), pg->alloc_site), (long)(bad - (u8 *)p));
    pg->flags &= ~PG_LARGE;
    large_frees++;
    large_active_bytes -= size;
    free_pages(pg, pg->order);
}

size_t ksize(const void *p)
{
    struct page *pg;
    if (!p)
        return 0;
    u8 *slot = find_slot(p, &pg, "ksize");
    if (slot)
        return slot_meta(pg->slab.cache, slot)->req_size;
    return pg && (pg->flags & PG_LARGE) ? pg->large_size : 0;
}

/* ---- introspection ---- */

static int verify_list(struct kmem_cache *c, struct list_head *list)
{
    int problems = 0;
    struct page *pg;
    list_for_each_entry(pg, list, list) {
        for (u32 i = 0; i < c->objs_per_slab; i++) {
            u8 *slot = slab_base(pg) + i * c->slot_size;
            struct obj_meta *m = slot_meta(c, slot);
            u64 before = mm_corruptions;
            if (m->state == OBJ_FREE) {
                u8 *bad = memchr_inv(slot_obj(slot), POISON_FREE, c->obj_size);
                if (bad) {
                    char s1[96], s2[96];
                    mm_corruption_dump(ALIGN_DOWN((uintptr_t)bad, 16), 32,
                                  "use-after-free write to %s object %p: byte +%ld changed to "
                                  "%#x after it was freed at %s (allocated at %s)",
                                  c->name, slot_obj(slot), (long)(bad - slot_obj(slot)), *bad,
                                  site_str(s1, sizeof(s1), m->free_site),
                                  site_str(s2, sizeof(s2), m->alloc_site));
                    memset(slot_obj(slot), POISON_FREE, c->obj_size);
                }
            } else if (m->state != OBJ_ALLOC) {
                mm_corruption("%s slot %p: metadata corrupted (state %#x)", c->name, slot,
                              m->state);
            }
            check_redzones(c, slot, "heap check");
            if (mm_corruptions != before)
                problems++;
        }
    }
    return problems;
}

int slab_verify(void)
{
    int problems = 0;
    for (int i = 0; i < nr_caches; i++) {
        struct kmem_cache *c = &caches[i];
        unsigned long flags;
        bool locked = spin_lock_debug(&c->lock, &flags);
        problems += verify_list(c, &c->partial);
        problems += verify_list(c, &c->full);
        problems += verify_list(c, &c->empty);
        spin_unlock_debug(&c->lock, locked, flags);
    }
    return problems;
}

void slab_print(print_fn print)
{
    print("%-16s %6s %6s %5s %8s %8s %6s %10s %10s %8s\n", "cache", "objsz", "slot", "/slab",
          "active", "total", "slabs", "allocs", "frees", "peak");
    for (int i = 0; i < nr_caches; i++) {
        struct kmem_cache *c = &caches[i];
        print("%-16s %6zu %6zu %5u %8lu %8lu %6lu %10lu %10lu %8lu\n", c->name, c->obj_size,
              c->slot_size, c->objs_per_slab, (unsigned long)c->active,
              (unsigned long)(c->nr_slabs * c->objs_per_slab), (unsigned long)c->nr_slabs,
              (unsigned long)c->allocs, (unsigned long)c->frees, (unsigned long)c->peak);
    }
    print("large (>%d B): %lu allocs, %lu frees, %lu bytes live\n", KMALLOC_MAX,
          (unsigned long)large_allocs, (unsigned long)large_frees,
          (unsigned long)large_active_bytes);
}

#define MAX_SITES 64
struct site_stat {
    uintptr_t site;
    const char *cache;
    u64 count, bytes;
};

static void account(struct site_stat *s, int *n, uintptr_t site, const char *cache, u64 bytes)
{
    for (int i = 0; i < *n; i++)
        if (s[i].site == site && s[i].cache == cache) {
            s[i].count++;
            s[i].bytes += bytes;
            return;
        }
    if (*n < MAX_SITES)
        s[(*n)++] = (struct site_stat){ site, cache, 1, bytes };
}

static void scan_list(struct kmem_cache *c, struct list_head *list, struct site_stat *s, int *n,
                      int *objs)
{
    struct page *pg;
    list_for_each_entry(pg, list, list) {
        for (u32 i = 0; i < c->objs_per_slab; i++) {
            u8 *slot = slab_base(pg) + i * c->slot_size;
            struct obj_meta *m = slot_meta(c, slot);
            if (m->state == OBJ_ALLOC) {
                account(s, n, m->alloc_site, c->name, m->req_size);
                (*objs)++;
            }
        }
    }
}

int slab_leak_report(print_fn print, const char *filter)
{
    static struct site_stat sites[MAX_SITES];
    int n = 0, objs = 0;
    size_t flen = filter ? strlen(filter) : 0;

    for (int i = 0; i < nr_caches; i++) {
        struct kmem_cache *c = &caches[i];
        if (flen && strncmp(c->name, filter, flen))
            continue;
        unsigned long flags;
        bool locked = spin_lock_debug(&c->lock, &flags);
        scan_list(c, &c->partial, sites, &n, &objs);
        scan_list(c, &c->full, sites, &n, &objs);
        spin_unlock_debug(&c->lock, locked, flags);
        if (!locked)
            print("(cache %s is locked; scanned without the lock)\n", c->name);
    }
    if (!flen || !strncmp("large", filter, flen)) {
        uintptr_t ram_end = platform.mem_base + platform.mem_size;
        for (uintptr_t pa = platform.mem_base; pa < ram_end; pa += PAGE_SIZE) {
            struct page *pg = pa_to_page(pa);
            if (pg && (pg->flags & PG_LARGE)) {
                account(sites, &n, pg->alloc_site, "large", pg->large_size);
                objs++;
            }
        }
    }

    /* Sort by bytes, descending. */
    for (int i = 1; i < n; i++) {
        struct site_stat t = sites[i];
        int j = i - 1;
        while (j >= 0 && sites[j].bytes < t.bytes) {
            sites[j + 1] = sites[j];
            j--;
        }
        sites[j + 1] = t;
    }

    print("%8s %10s  %-14s %s\n", "objects", "bytes", "cache", "allocation site");
    for (int i = 0; i < n; i++) {
        char s[96];
        print("%8lu %10lu  %-14s %s\n", (unsigned long)sites[i].count,
              (unsigned long)sites[i].bytes, sites[i].cache,
              site_str(s, sizeof(s), sites[i].site));
    }
    print("%d live allocations%s\n", objs, n == MAX_SITES ? " (site table full; truncated)" : "");
    return objs;
}
