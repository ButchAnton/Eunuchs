#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <ktest.h>
#include <mm.h>
#include <platform.h>
#include <riscv.h>
#include <safe.h>

void kmain(unsigned long hartid, uintptr_t dtb);

static u64 rng_state = 0x9e3779b97f4a7c15ULL;
static u64 rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static u64 free_pages_now(void)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    return st.free_pages;
}

/* Run `body` with corruption reports counted instead of panicking. */
#define EXPECT_CORRUPTIONS(n, body)                                          \
    do {                                                                     \
        bool __saved = mm_panic_on_corruption;                               \
        u64 __before = mm_corruptions;                                       \
        mm_panic_on_corruption = false;                                      \
        body;                                                                \
        mm_panic_on_corruption = __saved;                                    \
        KT_EXPECT_EQ(mm_corruptions - __before, (n));                        \
    } while (0)

/* ---- physical pages ---- */

KTEST(pmm_orders_and_alignment)
{
    u64 before = free_pages_now();
    for (unsigned order = 0; order <= 6; order++) {
        struct page *pg = alloc_pages(order);
        KT_ASSERT(pg != NULL);
        uintptr_t pa = page_to_pa(pg);
        KT_EXPECT_EQ(pa & ((PAGE_SIZE << order) - 1), 0);
        KT_EXPECT_EQ(free_pages_now(), before - (1UL << order));
        KT_EXPECT(pg->alloc_site != 0);
        /* Fresh pages arrive poisoned (proves nobody wrote them while free). */
        KT_EXPECT(memchr_inv(page_to_va(pg), POISON_FREE, PAGE_SIZE << order) == NULL);
        free_pages(pg, order);
        KT_EXPECT_EQ(free_pages_now(), before);
    }
}

KTEST(pmm_random_stress_no_overlap)
{
    enum { N = 256 };
    static struct page *blocks[N];
    static u8 orders[N];
    u64 before = free_pages_now();

    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < N; i++) {
            if (blocks[i] && (rng() & 1)) {
                /* Verify our tag survived: overlapping blocks would clobber it. */
                u64 *p = page_to_va(blocks[i]);
                for (size_t w = 0; w < (PAGE_SIZE << orders[i]) / 8; w += 512)
                    KT_EXPECT_EQ(p[w], (u64)i * 0x1000193 + w);
                free_pages(blocks[i], orders[i]);
                blocks[i] = NULL;
            } else if (!blocks[i]) {
                orders[i] = rng() % 4;
                blocks[i] = alloc_pages(orders[i]);
                KT_ASSERT(blocks[i] != NULL);
                u64 *p = page_to_va(blocks[i]);
                for (size_t w = 0; w < (PAGE_SIZE << orders[i]) / 8; w += 512)
                    p[w] = (u64)i * 0x1000193 + w;
            }
        }
    }
    for (int i = 0; i < N; i++)
        if (blocks[i]) {
            free_pages(blocks[i], orders[i]);
            blocks[i] = NULL;
        }
    KT_EXPECT_EQ(free_pages_now(), before);
    KT_EXPECT_EQ(pmm_verify_free(), 0);
}

KTEST(pmm_detects_double_free)
{
    void *p = page_alloc();
    KT_ASSERT(p != NULL);
    page_free(p);
    EXPECT_CORRUPTIONS(1, page_free(p));
}

KTEST(pmm_detects_write_after_free)
{
    u8 *p = page_alloc();
    KT_ASSERT(p != NULL);
    page_free(p);
    p[123] = 0x42;                          /* stale write */
    EXPECT_CORRUPTIONS(1, KT_EXPECT_EQ(pmm_verify_free(), 1));
    KT_EXPECT_EQ(pmm_verify_free(), 0);     /* re-poisoned */
}

KTEST(pmm_rejects_reserved_and_tail_frees)
{
    struct page *kernel_pg = pa_to_page(__pa(kmain));
    KT_ASSERT(kernel_pg != NULL);
    KT_EXPECT(kernel_pg->flags & PG_RESERVED);
    EXPECT_CORRUPTIONS(1, free_pages(kernel_pg, 0));

    struct page *pg = alloc_pages(2);
    KT_ASSERT(pg != NULL);
    KT_EXPECT(pg[1].flags & PG_TAIL);
    KT_EXPECT(compound_head(&pg[3]) == pg);
    EXPECT_CORRUPTIONS(1, free_pages(&pg[1], 0));
    EXPECT_CORRUPTIONS(1, free_pages(pg, 1));       /* wrong order */
    free_pages(pg, 2);
}

/* ---- kmalloc ---- */

KTEST(kmalloc_basic)
{
    static const size_t sizes[] = { 1, 7, 8, 9, 24, 64, 100, 255, 256, 1000, 2048 };
    void *ptrs[ARRAY_SIZE(sizes)];

    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        ptrs[i] = kmalloc(sizes[i]);
        KT_ASSERT(ptrs[i] != NULL);
        KT_EXPECT_EQ((uintptr_t)ptrs[i] & 7, 0);
        KT_EXPECT_EQ(ksize(ptrs[i]), sizes[i]);
        /* New memory carries the "uninitialized" poison. */
        KT_EXPECT(memchr_inv(ptrs[i], POISON_UNINIT, sizes[i]) == NULL);
        memset(ptrs[i], (int)i, sizes[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++)
        for (size_t j = i + 1; j < ARRAY_SIZE(sizes); j++)
            KT_EXPECT(ptrs[i] != ptrs[j]);
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        KT_EXPECT(memchr_inv(ptrs[i], (int)i, sizes[i]) == NULL);
        kfree(ptrs[i]);
    }

    u8 *z = kzalloc(40);
    KT_ASSERT(z != NULL);
    KT_EXPECT(memchr_inv(z, 0, 40) == NULL);
    kfree(z);
    kfree(NULL);
}

KTEST(kmalloc_stress)
{
    enum { N = 512 };
    static void *p[N];
    static size_t sz[N];
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < N; i++) {
            if (p[i]) {
                KT_EXPECT(memchr_inv(p[i], i & 0xff, sz[i]) == NULL);
                kfree(p[i]);
                p[i] = NULL;
            }
            if (rng() & 1) {
                sz[i] = 1 + rng() % 600;
                p[i] = kmalloc(sz[i]);
                KT_ASSERT(p[i] != NULL);
                memset(p[i], i & 0xff, sz[i]);
            }
        }
    }
    for (int i = 0; i < N; i++) {
        kfree(p[i]);
        p[i] = NULL;
    }
    KT_EXPECT_EQ(slab_verify(), 0);
}

KTEST(kmalloc_detects_overflow_by_one)
{
    u8 *p = kmalloc(13);
    KT_ASSERT(p != NULL);
    p[13] = 'x';                        /* one past the requested size */
    EXPECT_CORRUPTIONS(1, kfree(p));
}

KTEST(kmalloc_detects_underflow)
{
    u8 *p = kmalloc(32);
    KT_ASSERT(p != NULL);
    p[-1] = 0;
    EXPECT_CORRUPTIONS(1, kfree(p));
}

KTEST(kmalloc_detects_double_free)
{
    void *p = kmalloc(48);
    KT_ASSERT(p != NULL);
    kfree(p);
    EXPECT_CORRUPTIONS(1, kfree(p));
}

KTEST(kmalloc_detects_use_after_free)
{
    u8 *p = kmalloc(64);
    KT_ASSERT(p != NULL);
    kfree(p);
    p[5] = 1;                           /* write through a stale pointer */
    EXPECT_CORRUPTIONS(1, KT_EXPECT_EQ(slab_verify(), 1));
    KT_EXPECT_EQ(slab_verify(), 0);
}

KTEST(kmalloc_detects_bad_pointers)
{
    u8 *p = kmalloc(100);
    KT_ASSERT(p != NULL);
    EXPECT_CORRUPTIONS(1, kfree(p + 8));            /* interior pointer: not freed */
    KT_EXPECT_EQ(ksize(p), 100);                    /* still allocated */
    kfree(p);

    static int not_heap;
    EXPECT_CORRUPTIONS(1, kfree(&not_heap));        /* kernel .bss */
}

KTEST(kmalloc_large)
{
    u8 *p = kmalloc(10000);
    KT_ASSERT(p != NULL);
    KT_EXPECT_EQ((uintptr_t)p & (PAGE_SIZE - 1), 0);
    KT_EXPECT(va_to_page(p)->flags & PG_LARGE);
    KT_EXPECT_EQ(ksize(p), 10000);
    memset(p, 1, 10000);
    kfree(p);

    p = kmalloc(5000);
    KT_ASSERT(p != NULL);
    p[5000] = 0;
    EXPECT_CORRUPTIONS(1, kfree(p));
}

struct widget {
    u64 a, b;
    char name[20];
};

KTEST(kmem_cache_custom)
{
    struct kmem_cache *c = kmem_cache_create("test-widget", sizeof(struct widget));
    struct widget *w[20];
    for (int i = 0; i < 20; i++) {
        w[i] = kmem_cache_alloc(c);
        KT_ASSERT(w[i] != NULL);
        w[i]->a = i;
    }
    for (int i = 0; i < 20; i++) {
        KT_EXPECT_EQ(w[i]->a, (u64)i);
        kmem_cache_free(c, w[i]);
    }

    void *k = kmalloc(sizeof(struct widget));
    EXPECT_CORRUPTIONS(1, kmem_cache_free(c, k));   /* wrong cache */
    kfree(k);
}

/* The leak report must attribute allocations to the function that made them. */
static void *__noinline leaky_alloc(size_t n)
{
    void *p = kmalloc(n);
    asm volatile("" ::: "memory");
    return p;
}

static char report[4096];
static size_t report_len;
static int capture(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(report + report_len, sizeof(report) - report_len, fmt, ap);
    va_end(ap);
    report_len = MIN(report_len + n, sizeof(report) - 1);
    return n;
}

KTEST(leak_report_attributes_sites)
{
    void *a = leaky_alloc(40), *b = leaky_alloc(40), *c = leaky_alloc(40);
    report_len = 0;
    slab_leak_report(capture, "kmalloc-64");
    KT_EXPECT(strstr(report, "leaky_alloc") != NULL);
    char *line = strstr(report, "leaky_alloc");
    while (line && line > report && line[-1] != '\n')
        line--;
    KT_EXPECT(line && kstrtoul(line, NULL, 10) == 3);   /* "       3 ... leaky_alloc+0x.." */
    kfree(a);
    kfree(b);
    kfree(c);
}

/* ---- page tables ---- */

static const char rodata_string[] = "read-only";
static u64 data_word = 1;

KTEST(vm_kernel_permissions)
{
    uintptr_t pa;
    pte_t pte;

    KT_ASSERT(vm_translate(kernel_pagetable, (uintptr_t)kmain, &pa, &pte) == 0);
    KT_EXPECT_EQ(pa, __pa(kmain));
    KT_EXPECT((pte & PTE_X) && !(pte & PTE_W));                 /* text: r-x */

    KT_ASSERT(vm_translate(kernel_pagetable, (uintptr_t)rodata_string, NULL, &pte) == 0);
    KT_EXPECT(!(pte & PTE_X) && !(pte & PTE_W));                /* rodata: r-- */

    KT_ASSERT(vm_translate(kernel_pagetable, (uintptr_t)&data_word, NULL, &pte) == 0);
    KT_EXPECT((pte & PTE_W) && !(pte & PTE_X));                 /* data: rw- */

    /* W^X is enforced by the MMU, not just recorded. */
    KT_EXPECT_EQ(safe_write_u8((uintptr_t)kmain, 0), -1);
    KT_EXPECT_EQ(safe_write_u8((uintptr_t)rodata_string, 0), -1);
    KT_EXPECT_EQ(safe_write_u64((uintptr_t)&data_word, 2), 0);

    KT_EXPECT(vm_translate(kernel_pagetable, 0, NULL, NULL) != 0);
    KT_EXPECT(vm_translate(kernel_pagetable, __pa(platform.mem_base) + KERNEL_BASE, NULL, NULL) != 0);
}

KTEST(vm_map_unmap_alias)
{
    const uintptr_t alias = 0xffffffe000000000UL;   /* unused region above the direct map */
    u64 *page = page_alloc_zeroed();
    KT_ASSERT(page != NULL);

    KT_ASSERT(vm_map(kernel_pagetable, alias, __pa(page), PAGE_SIZE, PROT_KERNEL_RW) == 0);
    sfence_vma(alias);
    *(volatile u64 *)alias = 0xfeedbeef;
    KT_EXPECT_EQ(page[0], 0xfeedbeef);                          /* same frame */
    KT_EXPECT(vm_map(kernel_pagetable, alias, __pa(page), PAGE_SIZE, PROT_KERNEL_RW) != 0);

    KT_ASSERT(vm_unmap(kernel_pagetable, alias, PAGE_SIZE) == 0);
    u64 v;
    KT_EXPECT_EQ(safe_read_u64(alias, &v), -1);
    page_free(page);
}

static bool hint_has(uintptr_t addr, u64 cause, const char *what)
{
    const char *h = vm_fault_hint(addr, cause);
    return h && strstr(h, what);
}

KTEST(vm_fault_hints)
{
    KT_EXPECT_STREQ(vm_fault_hint(0x8, EXC_LOAD_PAGE_FAULT), "NULL pointer dereference");
    KT_EXPECT(hint_has(0x6b6b6b6b6b6b6b6bUL, EXC_LOAD_PAGE_FAULT, "use after free"));
    KT_EXPECT(hint_has(0x5a5a5a5a5a5a5a5aUL, EXC_LOAD_PAGE_FAULT, "uninitialized"));
    KT_EXPECT(hint_has((uintptr_t)kmain, EXC_STORE_PAGE_FAULT, "W^X"));
    KT_EXPECT(hint_has(0x0000100000000000UL, EXC_LOAD_PAGE_FAULT, "non-canonical"));
    KT_EXPECT(vm_fault_hint((uintptr_t)&data_word, EXC_LOAD_PAGE_FAULT) == NULL);
}
