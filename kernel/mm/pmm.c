/*
 * Physical memory manager.
 *
 * Boot: RAM comes from the device tree; firmware, the kernel image, the DTB,
 * DT reserved-memory and the mem_map array itself are recorded as reserved
 * regions (see `meminfo`). Everything else goes to the buddy allocator.
 *
 * Debugging: free pages are filled with POISON_FREE and checked on
 * allocation, so writes through stale pointers to freed pages are caught.
 * Every block remembers its last allocation and free site. Disable poisoning
 * with the `nopoison` boot option.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <spinlock.h>
#include "buddy.h"

static struct buddy_zone zone;
static spinlock_t zone_lock = SPINLOCK_INIT("pmm");
static bool pmm_ready;
static bool page_poison = true;

static u64 stat_allocs, stat_frees, stat_failures;

struct mem_region reserved_regions[MAX_RESERVED];
int nr_reserved_regions;

bool mm_panic_on_corruption = true;
u64 mm_corruptions;

/* ---- corruption reporting ---- */

static int mm_err_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_ERR, SUBSYS_MM, fmt, ap);
    va_end(ap);
    return 0;
}

static void vcorruption(uintptr_t dump_addr, size_t dump_len, const char *fmt, va_list ap)
{
    char msg[256];
    kvsnprintf(msg, sizeof(msg), fmt, ap);

    mm_corruptions++;
    klog(LOG_ERR, SUBSYS_MM, "*** MEMORY CORRUPTION: %s\n", msg);
    if (dump_len)
        hexdump_safe(mm_err_print, dump_addr, dump_len);
    klog(LOG_ERR, SUBSYS_MM, "detected at:\n");
    backtrace_here(mm_err_print);
    if (mm_panic_on_corruption)
        panic("memory corruption detected (details above)");
}

void mm_corruption(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vcorruption(0, 0, fmt, ap);
    va_end(ap);
}

void mm_corruption_dump(uintptr_t dump_addr, size_t dump_len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vcorruption(dump_addr, dump_len, fmt, ap);
    va_end(ap);
}

static const char *site_str(char *buf, size_t n, uintptr_t site)
{
    if (!site)
        return "?";
    ksym_format(buf, n, site);
    return buf;
}

/* ---- boot-time setup ---- */

static void reserve(uintptr_t start, uintptr_t end, const char *what)
{
    if (end <= start)
        return;
    if (nr_reserved_regions == MAX_RESERVED)
        panic("too many reserved memory regions");
    reserved_regions[nr_reserved_regions++] = (struct mem_region){
        ALIGN_DOWN(start, PAGE_SIZE), PAGE_ALIGN(end), what };
}

static bool overlaps_reserved(uintptr_t start, uintptr_t end)
{
    for (int i = 0; i < nr_reserved_regions; i++)
        if (start < reserved_regions[i].end && reserved_regions[i].start < end)
            return true;
    return false;
}

/* Sort reserved regions by start (insertion sort; there are few). */
static void sort_reserved(void)
{
    for (int i = 1; i < nr_reserved_regions; i++) {
        struct mem_region r = reserved_regions[i];
        int j = i - 1;
        while (j >= 0 && reserved_regions[j].start > r.start) {
            reserved_regions[j + 1] = reserved_regions[j];
            j--;
        }
        reserved_regions[j + 1] = r;
    }
}

void pmm_init(void)
{
    uintptr_t ram_start = platform.mem_base;
    uintptr_t ram_end = platform.mem_base + platform.mem_size;

    if (cmdline_has("nopoison"))
        page_poison = false;

    reserve(ram_start, __pa(__kernel_start), "firmware (OpenSBI)");
    reserve(__pa(__kernel_start), __pa(__kernel_end), "kernel image");
    reserve(platform.dtb, platform.dtb + platform.dtb_size, "device tree");
    for (int i = 0; i < platform.nreserved; i++)
        reserve(platform.reserved[i].base,
                platform.reserved[i].base + platform.reserved[i].size, "DT reserved-memory");

    /* mem_map: first free spot after the kernel image. */
    u64 npages = platform.mem_size >> PAGE_SHIFT;
    size_t map_bytes = PAGE_ALIGN(npages * sizeof(struct page));
    uintptr_t map_pa = __pa(__kernel_end);
    while (overlaps_reserved(map_pa, map_pa + map_bytes))
        map_pa += PAGE_SIZE;
    if (map_pa + map_bytes > ram_end)
        panic("no room for mem_map (%zu bytes)", map_bytes);
    reserve(map_pa, map_pa + map_bytes, "mem_map (struct page array)");
    sort_reserved();

    buddy_init(&zone, __va(map_pa), pa_to_pfn(ram_start), npages);

    /* Release every gap between reserved regions. */
    uintptr_t cur = ram_start;
    for (int i = 0; i <= nr_reserved_regions; i++) {
        uintptr_t gap_end = i < nr_reserved_regions ? reserved_regions[i].start : ram_end;
        if (gap_end > cur) {
            if (page_poison)
                memset(__va(cur), POISON_FREE, gap_end - cur);
            buddy_release_range(&zone, pa_to_pfn(cur), pa_to_pfn(gap_end));
        }
        if (i < nr_reserved_regions && reserved_regions[i].end > cur)
            cur = reserved_regions[i].end;
    }

    pmm_ready = true;
    for (int i = 0; i < nr_reserved_regions; i++)
        pr_info("reserved %#lx-%#lx %6lu KiB  %s\n",
                (unsigned long)reserved_regions[i].start, (unsigned long)reserved_regions[i].end,
                (unsigned long)(reserved_regions[i].end - reserved_regions[i].start) >> 10,
                reserved_regions[i].what);
    pr_info("%lu MiB RAM, %lu MiB free; page poisoning %s\n",
            (unsigned long)(platform.mem_size >> 20),
            (unsigned long)(zone.free_pages >> (20 - PAGE_SHIFT)),
            page_poison ? "on" : "off (nopoison)");
}

/* ---- allocation ---- */

struct page *pa_to_page(uintptr_t pa)
{
    if (!pmm_ready)
        return NULL;
    return buddy_page(&zone, pa_to_pfn(pa));
}

uintptr_t page_to_pa(const struct page *pg)
{
    return pfn_to_pa(buddy_pfn(&zone, pg));
}

static void check_poison(struct page *pg, unsigned order)
{
    size_t size = PAGE_SIZE << order;
    u8 *va = page_to_va(pg);
    u8 *bad = memchr_inv(va, POISON_FREE, size);
    if (!bad)
        return;

    char s1[96];
    u8 *last = bad;
    for (u8 *p = bad; p < va + size; p++)
        if (*p != POISON_FREE)
            last = p;
    mm_corruption_dump(ALIGN_DOWN((uintptr_t)bad, 16), 32,
                  "page use-after-free: block pa %#lx order %u modified after free; "
                  "bytes +%#lx..+%#lx (first: %#x); last freed at %s",
                  (unsigned long)page_to_pa(pg), order,
                  (unsigned long)(bad - va), (unsigned long)(last - va), *bad,
                  site_str(s1, sizeof(s1), pg->free_site));
}

struct page *alloc_pages_site(unsigned order, uintptr_t site)
{
    if (!pmm_ready)
        panic("alloc_pages before pmm_init");

    unsigned long flags = spin_lock_irqsave(&zone_lock);
    struct page *pg = buddy_alloc(&zone, order);
    if (pg)
        stat_allocs++;
    else
        stat_failures++;
    spin_unlock_irqrestore(&zone_lock, flags);

    if (!pg) {
        char s[96];
        pr_warn("out of memory: order-%u allocation from %s failed (%lu pages free)\n",
                order, site_str(s, sizeof(s), site), (unsigned long)zone.free_pages);
        return NULL;
    }
    if (page_poison)
        check_poison(pg, order);
    pg->refcount = 1;
    pg->alloc_site = site;
    if (LOG_TRACE <= log_threshold[LOG_SUBSYS]) {
        char s[96];
        pr_trace("alloc order %u -> pa %#lx (%s)\n", order, (unsigned long)page_to_pa(pg),
                 site_str(s, sizeof(s), site));
    }
    return pg;
}

struct page *alloc_pages(unsigned order)
{
    return alloc_pages_site(order, (uintptr_t)__builtin_return_address(0));
}

static void free_pages_site(struct page *pg, unsigned order, uintptr_t site)
{
    char s1[96], s2[96];

    if (!pg)
        return;
    uintptr_t pa = page_to_pa(pg);

    if (pg->flags & PG_FREE) {
        mm_corruption("double free of page pa %#lx (order %u); previously freed at %s, "
                      "allocated at %s", (unsigned long)pa, order,
                      site_str(s1, sizeof(s1), pg->free_site), site_str(s2, sizeof(s2), pg->alloc_site));
        return;
    }
    if (pg->flags & (PG_RESERVED | PG_TAIL)) {
        mm_corruption("free_pages of pa %#lx which is %s", (unsigned long)pa,
                      (pg->flags & PG_RESERVED) ? "reserved memory" : "not the head of its block");
        return;
    }
    if (pg->refcount > 1) {
        mm_corruption("free_pages of pa %#lx which still has %d references (allocated at %s)",
                      (unsigned long)pa, pg->refcount, site_str(s1, sizeof(s1), pg->alloc_site));
        return;
    }
    if (pg->order != order) {
        mm_corruption("free_pages order mismatch for pa %#lx: allocated order %u, freed as %u "
                      "(allocated at %s)", (unsigned long)pa, pg->order, order,
                      site_str(s1, sizeof(s1), pg->alloc_site));
        return;
    }

    if (page_poison)
        memset(page_to_va(pg), POISON_FREE, PAGE_SIZE << order);
    pg->free_site = site;
    pg->refcount = 0;

    unsigned long flags = spin_lock_irqsave(&zone_lock);
    int err = buddy_free(&zone, pg, order);
    if (!err)
        stat_frees++;
    spin_unlock_irqrestore(&zone_lock, flags);
    if (err)
        mm_corruption("buddy_free rejected pa %#lx order %u", (unsigned long)pa, order);
}

void free_pages(struct page *pg, unsigned order)
{
    free_pages_site(pg, order, (uintptr_t)__builtin_return_address(0));
}

void *page_alloc(void)
{
    struct page *pg = alloc_pages_site(0, (uintptr_t)__builtin_return_address(0));
    return pg ? page_to_va(pg) : NULL;
}

void *page_alloc_zeroed(void)
{
    struct page *pg = alloc_pages_site(0, (uintptr_t)__builtin_return_address(0));
    if (!pg)
        return NULL;
    void *va = page_to_va(pg);
    memset(va, 0, PAGE_SIZE);
    return va;
}

void page_free(void *va)
{
    if (va)
        free_pages_site(va_to_page(va), 0, (uintptr_t)__builtin_return_address(0));
}

void page_get(struct page *pg)
{
    __atomic_add_fetch(&pg->refcount, 1, __ATOMIC_RELAXED);
}

void page_put(struct page *pg)
{
    int r = __atomic_sub_fetch(&pg->refcount, 1, __ATOMIC_ACQ_REL);
    if (r == 0) {
        free_pages_site(pg, pg->order, (uintptr_t)__builtin_return_address(0));
    } else if (r < 0) {
        char s[96];
        mm_corruption("page_put: refcount of pa %#lx went negative (%d); last freed at %s",
                      (unsigned long)page_to_pa(pg), r, site_str(s, sizeof(s), pg->free_site));
    }
}

const char *pmm_region_name(uintptr_t pa)
{
    for (int i = 0; i < nr_reserved_regions; i++)
        if (pa >= reserved_regions[i].start && pa < reserved_regions[i].end)
            return reserved_regions[i].what;
    return NULL;
}

/* ---- introspection ---- */

int pmm_verify_free(void)
{
    int bad_blocks = 0;
    if (!pmm_ready || !page_poison)
        return 0;

    unsigned long flags = spin_lock_irqsave(&zone_lock);
    for (int o = 0; o <= MAX_ORDER; o++) {
        struct page *pg;
        list_for_each_entry(pg, &zone.free[o], list) {
            u8 *va = page_to_va(pg);
            if (memchr_inv(va, POISON_FREE, PAGE_SIZE << o)) {
                bad_blocks++;
                /* Report via the normal path, then restore the poison. */
                check_poison(pg, o);
                memset(va, POISON_FREE, PAGE_SIZE << o);
            }
        }
    }
    spin_unlock_irqrestore(&zone_lock, flags);
    return bad_blocks;
}

void pmm_get_stats(struct pmm_stats *st)
{
    memset(st, 0, sizeof(*st));
    if (!pmm_ready)
        return;
    st->total_pages = zone.npages;
    st->free_pages = zone.free_pages;
    for (u64 i = 0; i < zone.npages; i++)
        if (zone.pages[i].flags & PG_RESERVED)
            st->reserved_pages++;
    st->allocs = stat_allocs;
    st->frees = stat_frees;
    st->failures = stat_failures;
    for (int o = 0; o <= MAX_ORDER; o++)
        st->free_blocks[o] = zone.nr_free[o];
}

void pmm_print(print_fn print)
{
    struct pmm_stats st;

    if (!pmm_ready) {
        print("physical memory manager not initialized yet\n");
        return;
    }
    pmm_get_stats(&st);
    u64 used = st.total_pages - st.free_pages - st.reserved_pages;
    print("RAM        %#lx-%#lx\n", (unsigned long)platform.mem_base,
          (unsigned long)(platform.mem_base + platform.mem_size));
    print("  total    %8lu pages  %6lu KiB\n", (unsigned long)st.total_pages,
          (unsigned long)(st.total_pages * 4));
    print("  reserved %8lu pages  %6lu KiB\n", (unsigned long)st.reserved_pages,
          (unsigned long)(st.reserved_pages * 4));
    print("  in use   %8lu pages  %6lu KiB\n", (unsigned long)used, (unsigned long)(used * 4));
    print("  free     %8lu pages  %6lu KiB\n", (unsigned long)st.free_pages,
          (unsigned long)(st.free_pages * 4));
    print("allocs %lu, frees %lu, failures %lu, corruptions %lu, poisoning %s\n",
          (unsigned long)st.allocs, (unsigned long)st.frees, (unsigned long)st.failures,
          (unsigned long)mm_corruptions, page_poison ? "on" : "off");
    print("free blocks by order:");
    for (int o = 0; o <= MAX_ORDER; o++)
        print(" %d:%lu", o, (unsigned long)st.free_blocks[o]);
    u64 ks_used, ks_cached, ks_reuses, ks_unmaps;
    kstack_stats(&ks_used, &ks_cached, &ks_reuses, &ks_unmaps);
    print("\nkernel stacks: %lu in use, %lu cached (%lu reuses, %lu unmapped)",
          (unsigned long)ks_used, (unsigned long)ks_cached, (unsigned long)ks_reuses,
          (unsigned long)ks_unmaps);
    print("\nreserved regions:\n");
    for (int i = 0; i < nr_reserved_regions; i++)
        print("  %#lx-%#lx %6lu KiB  %s\n", (unsigned long)reserved_regions[i].start,
              (unsigned long)reserved_regions[i].end,
              (unsigned long)(reserved_regions[i].end - reserved_regions[i].start) >> 10,
              reserved_regions[i].what);
}

void page_print(print_fn print, uintptr_t pa)
{
    char s[96];
    struct page *pg = pa_to_page(pa);

    if (!pg) {
        print("pa %#lx is not managed RAM\n", (unsigned long)pa);
        return;
    }
    struct page *head = compound_head(pg);
    print("pa %#lx  pfn %#lx  struct page @ %p\n", (unsigned long)pa,
          (unsigned long)pa_to_pfn(pa), (void *)pg);
    print("  flags   %#x:%s%s%s%s%s%s%s\n", pg->flags,
          pg->flags & PG_FREE ? " free" : "", pg->flags & PG_RESERVED ? " reserved" : "",
          pg->flags & PG_SLAB ? " slab" : "", pg->flags & PG_LARGE ? " large-kmalloc" : "",
          pg->flags & PG_PGTABLE ? " pagetable" : "", pg->flags & PG_TAIL ? " tail" : "",
          pg->flags ? "" : " allocated");
    if (head != pg)
        print("  block   head pa %#lx (order %u)\n", (unsigned long)page_to_pa(head), head->order);
    else
        print("  order   %u  refcount %d\n", pg->order, pg->refcount);
    print("  alloc   %s\n", site_str(s, sizeof(s), head->alloc_site));
    print("  freed   %s\n", site_str(s, sizeof(s), head->free_site));
    if (head->flags & PG_SLAB)
        print("  slab    cache %s, %u objects in use\n",
              kmem_cache_name(head->slab.cache), head->slab.inuse);
    if (head->flags & PG_LARGE)
        print("  kmalloc %zu bytes\n", head->large_size);
}
