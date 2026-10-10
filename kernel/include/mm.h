#ifndef EUNUCHS_MM_H
#define EUNUCHS_MM_H

/*
 * Memory management.
 *
 * Virtual layout (Sv39):
 *   0x0000000000000000 - 0x0000003fffffffff   user (Phase 4); unmapped in the kernel,
 *                                             so NULL dereferences always fault
 *   0xffffffc000000000 + PA                   direct map of RAM and MMIO
 *   0xffffffc080200000                        kernel image (inside the direct map,
 *                                             with W^X section permissions)
 *   0xffffffe800000000                        kernel thread stacks (KSTACK_AREA),
 *                                             each with an unmapped guard below
 */

#define PAGE_SHIFT   12
#define PAGE_SIZE    (1UL << PAGE_SHIFT)
#ifdef __ASSEMBLY__
#define KERNEL_BASE  0xffffffc000000000
#else
#define KERNEL_BASE  0xffffffc000000000UL
#endif

/*
 * Kernel stacks: each lives in the upper half of a KSTACK_SLOT-aligned slot;
 * the lower half is an unmapped guard. Every valid kernel sp therefore has
 * bit KSTACK_GUARD_BIT set, which trapentry.S checks to catch overflows.
 */
#define KSTACK_SLOT       (32 * 1024)
#define KSTACK_SIZE       (16 * 1024)
#define KSTACK_GUARD_BIT  14            /* log2(KSTACK_SLOT - KSTACK_SIZE) */

#ifndef __ASSEMBLY__

#include <types.h>
#include <list.h>

#define PAGE_MASK         (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x)     ALIGN_UP((uintptr_t)(x), PAGE_SIZE)

#define __va(pa)          ((void *)((uintptr_t)(pa) + KERNEL_BASE))
#define __pa(va)          ((uintptr_t)(va) - KERNEL_BASE)
#define pa_to_pfn(pa)     ((uintptr_t)(pa) >> PAGE_SHIFT)
#define pfn_to_pa(pfn)    ((uintptr_t)(pfn) << PAGE_SHIFT)

/* ---- struct page: one per physical page frame ---- */

enum page_flags {
    PG_FREE     = 1 << 0,   /* in the buddy allocator (head of a free block) */
    PG_RESERVED = 1 << 1,   /* firmware, kernel image, DTB, mem_map: never allocatable */
    PG_SLAB     = 1 << 2,   /* owned by a slab cache */
    PG_LARGE    = 1 << 3,   /* head of a large (multi-page) kmalloc */
    PG_PGTABLE  = 1 << 4,   /* page-table page */
    PG_TAIL     = 1 << 5,   /* non-head page of an allocated block */
};

struct kmem_cache;

struct page {
    struct list_head list;      /* buddy free list or slab list */
    u32 flags;
    u8 order;                   /* block order (heads only) */
    i32 refcount;               /* users of the frame (COW in Phase 4) */
    struct page *head;          /* PG_TAIL pages: head of their block */
    uintptr_t alloc_site;       /* caller PC of the last allocation (debug) */
    uintptr_t free_site;        /* caller PC of the last free (debug) */
    union {
        struct {                /* PG_SLAB */
            struct kmem_cache *cache;
            void *free_head;    /* FIFO freelist of objects */
            void *free_tail;
            u32 inuse;
        } slab;
        size_t large_size;      /* PG_LARGE: requested size */
    };
};

/* ---- physical page allocator (pmm.c) ---- */

#define MAX_ORDER 10            /* largest block: 2^10 pages = 4 MiB */

void pmm_init(void);
struct page *alloc_pages_site(unsigned order, uintptr_t site);
void free_pages(struct page *pg, unsigned order);

/* Records the caller as the allocation site. */
struct page *alloc_pages(unsigned order);

/* Single pages by kernel virtual address. Contents are poisoned, not zeroed. */
void *page_alloc(void);
void *page_alloc_zeroed(void);
void page_free(void *va);
/* Reference counting for shared frames (user pages, COW). put frees at 0. */
void page_get(struct page *pg);
void page_put(struct page *pg);

struct page *pa_to_page(uintptr_t pa);   /* NULL if not managed RAM */
static inline struct page *compound_head(struct page *pg)
{
    return (pg->flags & PG_TAIL) ? pg->head : pg;
}
uintptr_t page_to_pa(const struct page *pg);
static inline void *page_to_va(const struct page *pg) { return __va(page_to_pa(pg)); }
static inline struct page *va_to_page(const void *va) { return pa_to_page(__pa(va)); }

struct pmm_stats {
    u64 total_pages;            /* managed RAM */
    u64 free_pages;
    u64 reserved_pages;
    u64 allocs, frees, failures;
    u64 free_blocks[MAX_ORDER + 1];
};
void pmm_get_stats(struct pmm_stats *st);
void pmm_print(int (*print)(const char *fmt, ...));
/* Check every free block's poison; reports and re-poisons bad blocks. */
int pmm_verify_free(void);
void page_print(int (*print)(const char *fmt, ...), uintptr_t pa);

/* Physical memory regions reported at boot (`meminfo`). */
struct mem_region {
    uintptr_t start, end;       /* physical, [start, end) */
    const char *what;
};
#define MAX_RESERVED 16
extern struct mem_region reserved_regions[MAX_RESERVED];
const char *pmm_region_name(uintptr_t pa);  /* reserved region containing pa, or NULL */
extern int nr_reserved_regions;

/* ---- page tables (vmm.c) ---- */

typedef u64 pte_t;
typedef pte_t *pagetable_t;     /* kernel VA of a 4 KiB table */

#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)
#define PTE_PERM_MASK 0xffUL

#define PTE_PA(pte)        (((pte) >> 10) << PAGE_SHIFT)
#define PA_TO_PTE(pa)      (((uintptr_t)(pa) >> PAGE_SHIFT) << 10)
#define PTE_LEAF(pte)      ((pte) & (PTE_R | PTE_W | PTE_X))

#define PX(level, va)      (((uintptr_t)(va) >> (PAGE_SHIFT + 9 * (level))) & 0x1ff)
#define LEVEL_SIZE(level)  (1UL << (PAGE_SHIFT + 9 * (level)))

/* Common kernel permission sets (A/D preset so hardware never needs to). */
#define PROT_KERNEL_RX  (PTE_R | PTE_X | PTE_G | PTE_A)
#define PROT_KERNEL_RO  (PTE_R | PTE_G | PTE_A)
#define PROT_KERNEL_RW  (PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

extern pagetable_t kernel_pagetable;
extern u32 kernel_root_gen;     /* bumped when the kernel's upper-half root entries change */

void vm_init(void);
int vm_map(pagetable_t pt, uintptr_t va, uintptr_t pa, size_t size, u64 perm);
int vm_unmap(pagetable_t pt, uintptr_t va, size_t size);   /* kernel: then tlb_shootdown() */
bool vm_kernel_fault_fixup(uintptr_t va);
/* Translate va; returns 0 and fills pa/leaf pte, or -1 if unmapped. */
int vm_translate(pagetable_t pt, uintptr_t va, uintptr_t *pa, pte_t *pte);
/* Level-0 PTE for va (4 KiB pages only); creates tables if alloc. */
pte_t *vm_walk(pagetable_t pt, uintptr_t va, bool alloc);
void vm_sync_kernel(pagetable_t pt);    /* copy the kernel's upper-half root entries */
pagetable_t vm_current(void);           /* page table in satp */
void vm_dump(pagetable_t pt, int (*print)(const char *fmt, ...));
void vm_walk_print(pagetable_t pt, uintptr_t va, int (*print)(const char *fmt, ...));
const char *vm_fault_hint(uintptr_t addr, u64 scause);
void *ioremap(uintptr_t pa, size_t size);
void pte_flags_str(pte_t pte, char *buf);   /* "rwx-gad" style, buf >= 9 */

static inline void sfence_vma_all(void) { asm volatile("sfence.vma zero, zero" ::: "memory"); }
static inline void sfence_vma(uintptr_t va) { asm volatile("sfence.vma %0, zero" :: "r"(va) : "memory"); }

/* ---- kernel stacks (kstack.c) ---- */

#define KSTACK_AREA   0xffffffe800000000UL
#define KSTACK_MAX    512
#define STACK_POISON  0x57      /* unused stack bytes, for high-water marks */

void *kstack_alloc(void);       /* returns the lowest usable address */
void kstack_free(void *base);
void kstack_stats(u64 *in_use, u64 *cached, u64 *reuses, u64 *unmaps);
bool addr_in_stack_guard(uintptr_t addr);

/* ---- kernel heap: slab allocator (slab.c) ---- */

void slab_init(void);

void *kmalloc_site(size_t size, uintptr_t site);
void *kzalloc_site(size_t size, uintptr_t site);
void kfree(const void *p);
size_t ksize(const void *p);

/* These record their caller as the allocation site (see `slab leaks`). */
void *kmalloc(size_t size);
void *kzalloc(size_t size);

struct kmem_cache *kmem_cache_create(const char *name, size_t size);
void *kmem_cache_alloc_site(struct kmem_cache *c, uintptr_t site);
void kmem_cache_free(struct kmem_cache *c, void *obj);
void *kmem_cache_alloc(struct kmem_cache *c);

const char *kmem_cache_name(const struct kmem_cache *c);
void slab_print(int (*print)(const char *fmt, ...));
/* Check every slab object's redzones and free-object poison; returns problems found. */
int slab_verify(void);
/* List live allocations grouped by allocation site; returns objects listed. */
int slab_leak_report(int (*print)(const char *fmt, ...), const char *cache_filter);

/* ---- corruption reporting (pmm.c) ---- */

/*
 * Heap/page corruption (overflow, use-after-free, double free) is reported
 * with full detail and a backtrace, counted in mm_corruptions, and then
 * panics unless mm_panic_on_corruption is false (tests flip it).
 */
extern bool mm_panic_on_corruption;
extern u64 mm_corruptions;
void mm_corruption(const char *fmt, ...) __printf_like(1, 2);
/* Same, with a hexdump of [dump_addr, dump_addr + dump_len) in the report. */
void mm_corruption_dump(uintptr_t dump_addr, size_t dump_len, const char *fmt, ...)
    __printf_like(3, 4);

/* Poison patterns (also recognized by the fault-hint code). */
#define POISON_FREE      0x6b   /* freed slab objects and pages */
#define POISON_UNINIT    0x5a   /* freshly kmalloc'ed, not yet written */
#define POISON_REDZONE   0xbb   /* guard bytes around slab objects */

#endif /* __ASSEMBLY__ */
#endif
