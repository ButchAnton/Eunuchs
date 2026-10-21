/*
 * User address spaces: VMAs, demand paging, copy-on-write fork, file
 * mappings, and the process side of swapping.
 *
 * Every user page is reference counted (struct page refcount): each PTE
 * mapping it holds one, and so does the page cache while the page is in it.
 *
 *   anonymous   zero-filled on first touch. fork shares all pages: writable
 *               ones become read-only + PTE_COW in both processes and are
 *               copied on the first write (or just made writable again if
 *               nobody else has the page).
 *   file        (mmap of a file, VMA_FILE) pages come from the page cache.
 *               MAP_SHARED maps the cache page itself; the first write to
 *               it marks the page dirty (the PTE starts read-only), and it
 *               goes back to the file later (pagecache.c). MAP_PRIVATE maps
 *               the cache page read-only + COW, so a write copies it.
 *   swapped     reclaim (uvm_reclaim) replaces the PTE of a cold anonymous
 *               page with a swap PTE (mm.h: PTE_SWAP and the slot); the
 *               next touch reads it back (swap_in). A cold file page's PTE
 *               is just cleared: the file has it.
 *
 * Accessed bits: reclaim clears PTE_A to see what is used. Hardware either
 * sets it again (Svadu) or faults; uvm_fault then sets it. PTE_D is always
 * set: dirtiness of shared file pages is tracked by write faults instead.
 *
 * Locking: mm->lock (a mutex) around every change to the page table or the
 * VMA list; vma_lock (a spinlock) too for VMA list changes, for readers
 * that cannot sleep. Order: mm->lock, then the filesystem (file faults,
 * closing a file), then pc_lock. TLB: changes bump mm->tlb_gen, so other
 * CPUs flush this address space's ASID when they next run it; reclaim,
 * which changes processes running elsewhere, also shoots down every TLB.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <fs.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <sbi.h>
#include <smp.h>
#include <uapi/errno.h>

static u64 next_mm_id = 1;

static u64 prot_to_pte(u32 prot)
{
    u64 pte = PTE_U | PTE_A | PTE_D;
    if (prot & VM_READ) pte |= PTE_R;
    if (prot & VM_WRITE) pte |= PTE_W;
    if (prot & VM_EXEC) pte |= PTE_X;
    return pte;
}

const char *vma_kind_name(enum vma_kind k)
{
    switch (k) {
    case VMA_ELF: return "elf";
    case VMA_HEAP: return "heap";
    case VMA_STACK: return "stack";
    case VMA_ANON: return "anon";
    case VMA_FILE: return "file";
    }
    return "?";
}

/* This CPU may hold stale translations of mm: flush them if mm is the
   active address space; other CPUs flush when they next switch to it. */
static void tlb_changed(struct mm *mm, uintptr_t va)
{
    __atomic_add_fetch(&mm->tlb_gen, 1, __ATOMIC_RELEASE);
    if (mm->pt == vm_current()) {
        if (va)
            sfence_vma(va);
        else
            sfence_vma_all();
    }
}

struct mm *mm_create(void)
{
    struct mm *mm = kzalloc(sizeof(*mm));
    if (!mm)
        return NULL;
    mm->pt = page_alloc_zeroed();
    if (!mm->pt) {
        kfree(mm);
        return NULL;
    }
    va_to_page(mm->pt)->flags |= PG_PGTABLE;
    vm_sync_kernel(mm->pt);
    mm->kernel_gen = kernel_root_gen;
    list_init(&mm->vmas);
    mutex_init(&mm->lock, "mm");
    spin_init(&mm->vma_lock, "vma");
    mm->id = __atomic_fetch_add(&next_mm_id, 1, __ATOMIC_RELAXED);
    return mm;
}

struct vma *vma_find(struct mm *mm, uintptr_t addr)
{
    struct vma *v;
    list_for_each_entry(v, &mm->vmas, node)
        if (addr >= v->start && addr < v->end)
            return v;
    return NULL;
}

/* A new VMA in the sorted list, or an error. */
static struct vma *vma_insert(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot,
                              enum vma_kind kind, const char *name, int *err)
{
    if ((start | end) & (PAGE_SIZE - 1) || start >= end || end > USER_TOP || start < PAGE_SIZE) {
        *err = -EINVAL;
        return NULL;
    }
    struct vma *v;
    struct list_head *pos = &mm->vmas;
    list_for_each_entry(v, &mm->vmas, node) {
        if (start < v->end && v->start < end) {
            *err = -EEXIST;
            return NULL;
        }
        if (v->start >= end) {
            pos = &v->node;
            break;
        }
    }
    struct vma *n = kzalloc(sizeof(*n));
    if (!n) {
        *err = -ENOMEM;
        return NULL;
    }
    n->start = start;
    n->end = end;
    n->prot = prot;
    n->kind = kind;
    strlcpy(n->name, name, sizeof(n->name));
    unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
    list_add_tail(&n->node, pos);       /* before pos: keeps the list sorted */
    spin_unlock_irqrestore(&mm->vma_lock, flags);
    return n;
}

int vma_add(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot, enum vma_kind kind,
            const char *name)
{
    int err = 0;
    return vma_insert(mm, start, end, prot, kind, name, &err) ? 0 : err;
}

int vma_add_file(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot, struct file *f, u64 pgoff,
                 bool shared)
{
    int err = 0;
    const char *base = f->path;
    for (const char *s = f->path; *s; s++)
        if (*s == '/' && s[1])
            base = s + 1;
    struct vma *v = vma_insert(mm, start, end, prot, VMA_FILE, base, &err);
    if (!v)
        return err;
    v->file = file_dup(f);
    v->pgoff = pgoff;
    v->shared = shared;
    return 0;
}

/* A VMA's second half [at, v->end) as a VMA of its own (its file too). */
static struct vma *vma_split_copy(struct mm *mm, struct vma *v, uintptr_t at, uintptr_t end, int *err)
{
    struct vma *n = kzalloc(sizeof(*n));
    if (!n) {
        *err = -ENOMEM;
        return NULL;
    }
    *n = *v;
    n->start = at;
    n->end = end;
    if (v->file) {
        n->file = file_dup(v->file);
        n->pgoff = v->pgoff + (at - v->start) / PAGE_SIZE;
    }
    (void)mm;
    return n;
}

static void vma_free(struct vma *v)
{
    if (v->file)
        file_close(v->file);
    kfree(v);
}

/*
 * The next PTE in use (a page or a swapped-out page) at or after *va (below
 * end), skipping empty page tables whole (1 GiB / 2 MiB at a time) instead
 * of looking up every page: a process's 8 MiB stack is mostly unmapped.
 * (The profiler found per-page lookups in unmap_range taking a fifth of the
 * CPU in a fork/exec-heavy load.) User page tables have only 4 KiB leaves.
 */
static pte_t *next_used(pagetable_t root, uintptr_t *va, uintptr_t end)
{
    while (*va < end) {
        pte_t l2 = root[PX(2, *va)];
        if (!(l2 & PTE_V)) {
            *va = ALIGN_DOWN(*va, LEVEL_SIZE(2)) + LEVEL_SIZE(2);
            continue;
        }
        pte_t l1 = ((pagetable_t)__va(PTE_PA(l2)))[PX(1, *va)];
        if (!(l1 & PTE_V)) {
            *va = ALIGN_DOWN(*va, LEVEL_SIZE(1)) + LEVEL_SIZE(1);
            continue;
        }
        pte_t *l0 = &((pagetable_t)__va(PTE_PA(l1)))[PX(0, *va)];
        if (*l0)
            return l0;
        *va += PAGE_SIZE;
    }
    return NULL;
}

#define for_each_used(pt, va, start, end, pte)                                \
    for (uintptr_t va = (start); ((pte) = next_used((pt), &va, (end))) != NULL; va += PAGE_SIZE)

static bool table_empty(const pte_t *t)
{
    for (int i = 0; i < 512; i++)
        if (t[i])
            return false;
    return true;
}

/*
 * Free the page tables in [start, end) that no longer map anything. Each
 * entry is cleared, and the TLB flushed, before its table is freed, so a
 * walk (a profiler sample, the monitor) never follows a freed table.
 */
static void prune_tables(struct mm *mm, uintptr_t start, uintptr_t end)
{
    void *freed[16];
    int nfreed = 0;
    for (uintptr_t a2 = ALIGN_DOWN(start, LEVEL_SIZE(2)); a2 < end; a2 += LEVEL_SIZE(2)) {
        pte_t *e2 = &mm->pt[PX(2, a2)];
        if (!(*e2 & PTE_V))
            continue;
        pagetable_t l1 = __va(PTE_PA(*e2));
        uintptr_t lo = MAX(start, a2), hi = MIN(end, a2 + LEVEL_SIZE(2));
        for (uintptr_t a1 = ALIGN_DOWN(lo, LEVEL_SIZE(1)); a1 < hi; a1 += LEVEL_SIZE(1)) {
            pte_t *e1 = &l1[PX(1, a1)];
            if (!(*e1 & PTE_V) || !table_empty(__va(PTE_PA(*e1))))
                continue;
            freed[nfreed++] = __va(PTE_PA(*e1));
            *e1 = 0;
            if (nfreed == 16) {
                tlb_changed(mm, 0);
                while (nfreed)
                    page_free(freed[--nfreed]);
            }
        }
        if (table_empty(l1)) {
            freed[nfreed++] = l1;
            *e2 = 0;
            if (nfreed == 16) {
                tlb_changed(mm, 0);
                while (nfreed)
                    page_free(freed[--nfreed]);
            }
        }
    }
    if (nfreed) {
        tlb_changed(mm, 0);
        while (nfreed)
            page_free(freed[--nfreed]);
    }
}

/* What a PTE held goes: the page reference, or the swap slot reference. */
static void drop_pte(struct mm *mm, pte_t pte)
{
    if (PTE_IS_SWAP(pte)) {
        swap_free(SWAP_SLOT(pte));
        mm->swapped--;
        return;
    }
    struct page *pg = pa_to_page(PTE_PA(pte));
    mm->rss--;
    if (!pg)
        return;
    if ((pte & PTE_W) && (pg->flags & PG_CACHE))
        __atomic_sub_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);    /* a shared writable mapping */
    page_put(pg);
}

/* Drop every mapped page in [start, end), then the page tables left empty. */
static void unmap_range(struct mm *mm, uintptr_t start, uintptr_t end)
{
    pte_t *pte;
    bool any = false;
    for_each_used(mm->pt, va, start, end, pte) {
        pte_t old = *pte;
        *pte = 0;
        any = true;
        if (old & PTE_V) {
            /* flush before the page can be reused (it may be freed now) */
            tlb_changed(mm, va);
        }
        drop_pte(mm, old);
    }
    if (any)
        prune_tables(mm, start, end);
}

/* Caller holds mm->lock. Splits are allocated first: an error changes nothing. */
int vma_unmap_locked(struct mm *mm, uintptr_t start, uintptr_t end)
{
    struct vma *v, *tmp;
    if ((start | end) & (PAGE_SIZE - 1) || start >= end || end > USER_TOP)
        return -EINVAL;
    list_for_each_entry_safe(v, tmp, &mm->vmas, node) {
        if (end <= v->start || start >= v->end)
            continue;
        uintptr_t s = MAX(start, v->start), e = MIN(end, v->end);
        struct vma *tail = NULL;
        if (s > v->start && e < v->end) {               /* a hole in the middle */
            int err = 0;
            tail = vma_split_copy(mm, v, e, v->end, &err);
            if (!tail)
                return err;
        }
        unmap_range(mm, s, e);
        unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
        if (s == v->start && e == v->end) {
            list_del(&v->node);
            spin_unlock_irqrestore(&mm->vma_lock, flags);
            vma_free(v);
        } else if (s == v->start) {
            if (v->file)
                v->pgoff += (e - v->start) / PAGE_SIZE;
            v->start = e;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        } else if (e == v->end) {
            v->end = s;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        } else {
            v->end = s;
            list_add(&tail->node, &v->node);
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        }
    }
    return 0;
}

int vma_unmap(struct mm *mm, uintptr_t start, uintptr_t end)
{
    mutex_lock(&mm->lock);
    int r = vma_unmap_locked(mm, start, end);
    mutex_unlock(&mm->lock);
    return r;
}

/*
 * mprotect: give [start, end) new permissions, splitting VMAs at the edges.
 * The whole range must be mapped. Write permission is never put straight
 * into a private PTE: fork shares read-only pages without marking them COW,
 * so a page that becomes writable is marked COW and the first write fault
 * takes it (or copies it if it is shared). Shared file pages likewise stay
 * read-only until a write fault marks them dirty. Pages that become
 * executable get the instruction caches of every hart synchronized.
 */
static int protect_locked(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot)
{
    if ((start | end) & (PAGE_SIZE - 1) || start >= end || end > USER_TOP)
        return -EINVAL;
    /* the range must be covered without holes */
    for (uintptr_t a = start; a < end;) {
        struct vma *v = vma_find(mm, a);
        if (!v)
            return -ENOMEM;
        a = v->end;
    }
    struct vma *v, *tmp;
    list_for_each_entry_safe(v, tmp, &mm->vmas, node) {
        if (end <= v->start || start >= v->end || v->prot == prot)
            continue;
        if (v->shared && (prot & VM_WRITE) && !v->file->writable)
            return -EACCES;
        uintptr_t s = MAX(start, v->start), e = MIN(end, v->end);
        /* split off the parts outside the range; they keep the old protection */
        struct vma *head = NULL, *tail = NULL;
        int err = 0;
        if (s > v->start && !(head = vma_split_copy(mm, v, v->start, s, &err)))
            return err;
        if (e < v->end && !(tail = vma_split_copy(mm, v, e, v->end, &err))) {
            if (head)
                vma_free(head);
            return err;
        }
        unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
        if (head) {
            head->pgoff = v->pgoff;             /* (the copy took it from s) */
            list_add_tail(&head->node, &v->node);
            if (v->file)
                v->pgoff += (s - v->start) / PAGE_SIZE;
            v->start = s;
        }
        if (tail) {
            list_add(&tail->node, &v->node);
            v->end = e;
        }
        v->prot = prot;
        spin_unlock_irqrestore(&mm->vma_lock, flags);
        pte_t *pte;
        for_each_used(mm->pt, va, s, e, pte) {
            if (!(*pte & PTE_V))
                continue;               /* swapped out: gets v->prot when it comes back */
            pte_t old = *pte, n = old & ~(PTE_R | PTE_W | PTE_X | PTE_COW);
            struct page *pg = pa_to_page(PTE_PA(old));
            if (prot & VM_READ)
                n |= PTE_R;
            if (prot & VM_EXEC)
                n |= PTE_X;
            if (v->shared) {
                if ((old & PTE_W) && (prot & VM_WRITE))
                    n |= PTE_W;
                else if ((old & PTE_W) && pg)
                    __atomic_sub_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);
            } else if (old & PTE_W) {
                n |= prot & VM_WRITE ? PTE_W : PTE_COW;
            } else if ((old & PTE_COW) || (prot & VM_WRITE)) {
                n |= PTE_COW;
            }
            *pte = n;
        }
    }
    tlb_changed(mm, 0);
    if (prot & VM_EXEC) {
        asm volatile("fence.i" ::: "memory");
        sbi_remote_fence_i(0, -1UL);
    }
    return 0;
}

int vma_protect(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot)
{
    mutex_lock(&mm->lock);
    int r = protect_locked(mm, start, end, prot);
    mutex_unlock(&mm->lock);
    return r;
}

static void free_tables(pte_t *t, int level)
{
    for (int i = 0; i < 512; i++) {
        if (level == 2 && i >= 256)
            break;                          /* upper half belongs to the kernel */
        pte_t pte = t[i];
        if ((pte & PTE_V) && !PTE_LEAF(pte) && level > 0)
            free_tables(__va(PTE_PA(pte)), level - 1);
    }
    page_free(t);
}

void mm_destroy(struct mm *mm)
{
    struct vma *v, *tmp;
    if (!mm)
        return;
    if (mm->pt == vm_current())
        panic("mm_destroy of the active address space");
    mutex_lock(&mm->lock);                  /* reclaim may be looking at it */
    list_for_each_entry_safe(v, tmp, &mm->vmas, node) {
        pte_t *pte;
        for_each_used(mm->pt, va, v->start, v->end, pte) {
            pte_t old = *pte;
            *pte = 0;
            drop_pte(mm, old);
        }
        unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
        list_del(&v->node);
        spin_unlock_irqrestore(&mm->vma_lock, flags);
        vma_free(v);
    }
    if (mm->rss || mm->swapped)
        pr_warn("mm_destroy: %lu pages still counted resident, %lu swapped (accounting bug)\n",
                (unsigned long)mm->rss, (unsigned long)mm->swapped);
    free_tables(mm->pt, 2);
    mutex_unlock(&mm->lock);
    kfree(mm);
}

/* A zeroed anonymous page at va (exec's loader; ptrace). Caller holds the lock
   or owns the mm alone. */
void *uvm_populate(struct mm *mm, uintptr_t va)
{
    struct vma *v = vma_find(mm, va);
    if (!v)
        return NULL;
    va = ALIGN_DOWN(va, PAGE_SIZE);
    pte_t *pte = vm_walk(mm->pt, va, true);
    if (!pte)
        return NULL;
    if (*pte & PTE_V)
        return __va(PTE_PA(*pte));
    if (*pte)
        return NULL;                        /* swapped out: needs a fault */
    struct page *pg = user_page_alloc(true);
    if (!pg)
        return NULL;
    *pte = PA_TO_PTE(page_to_pa(pg)) | prot_to_pte(v->prot) | PTE_V;
    mm->rss++;
    return page_to_va(pg);
}

/*
 * Faults never allocate memory or read swap while holding mm->lock: memory
 * may have to be reclaimed first, possibly from this very process, which
 * reclaim can only do while its lock is free. A fault that needs a page
 * returns -EAGAIN if *spare is empty, and one that needs a swap slot read
 * returns -EAGAIN with fault_ctx.read_slot set; fault_relock does that with
 * the lock dropped (the page read goes into the swap cache, held by
 * ctx.held so reclaim leaves it) and looks at the fault again.
 */
struct fault_ctx {
    struct page *spare;         /* a free page for the fault to take */
    struct page *held;          /* a swap-cache page read for it */
    u32 read_slot;              /* the fault wants this slot read */
};

static struct page *take_spare(struct page **spare, bool zero)
{
    struct page *pg = *spare;
    if (!pg)
        return NULL;
    *spare = NULL;
    if (zero)
        memset(page_to_va(pg), 0, PAGE_SIZE);
    return pg;
}

/*
 * A write to a read-only private page: take it if nobody else has it, else
 * copy it. A page that is only in the swap cache besides (its slot needed
 * by nothing else) leaves the swap cache first, so it can be taken.
 */
static int cow_break(struct mm *mm, struct vma *v, pte_t *pte, uintptr_t va, struct page **spare)
{
    struct page *pg = pa_to_page(PTE_PA(*pte));
    if (pg && (pg->flags & PG_SWAPCACHE) && pg->refcount == 2 && swap_count((u32)pg->cache.index) == 1)
        pc_remove(pg);                      /* now only ours */
    if (pg && pg->refcount == 1 && !(pg->flags & (PG_CACHE | PG_SWAPCACHE))) {
        *pte = (*pte | PTE_W) & ~PTE_COW;   /* last user: just take it */
        return 0;
    }
    struct page *copy = take_spare(spare, false);
    if (!copy)
        return -EAGAIN;
    memcpy(page_to_va(copy), __va(PTE_PA(*pte)), PAGE_SIZE);
    *pte = PA_TO_PTE(page_to_pa(copy)) | prot_to_pte(v->prot) | PTE_V;
    tlb_changed(mm, va);
    if (pg)
        page_put(pg);
    mm->cow_copies++;
    return 0;
}

/* A swapped-out page comes back. */
static int swap_in(struct mm *mm, struct vma *v, pte_t *pte, uintptr_t va, int access,
                   struct page **spare)
{
    struct fault_ctx *ctx = container_of(spare, struct fault_ctx, spare);
    u32 slot = SWAP_SLOT(*pte);
    struct page *pg = pc_find(&swap_owner, 0, slot);
    if (!pg) {
        ctx->read_slot = slot;          /* read it with the lock dropped */
        return -EAGAIN;
    }
    int held = ctx->held == pg;         /* fault_relock's reference, dropped later */
    /* our reference to pg becomes the PTE's; the PTE's slot reference goes */
    swap_free(slot);
    mm->swapped--;
    mm->rss++;
    pte_t perms = prot_to_pte(v->prot);
    if (pg->refcount == 2 + held && swap_count(slot) == 1) {
        pc_remove(pg);                  /* nobody else needs the slot: the page is ours */
        *pte = PA_TO_PTE(page_to_pa(pg)) | perms | PTE_V;
        return 0;
    }
    /* others still have the slot: share the page, copy on write */
    *pte = PA_TO_PTE(page_to_pa(pg)) | (perms & ~PTE_W) | (v->prot & VM_WRITE ? PTE_COW : 0) | PTE_V;
    if ((access & FAULT_WRITE) && (v->prot & VM_WRITE))
        return cow_break(mm, v, pte, va, spare);
    return 0;
}

/* A file page comes in (from the page cache, read from the file if needed). */
static int file_fault(struct mm *mm, struct vma *v, pte_t *pte, uintptr_t va, int access,
                      struct page **spare)
{
    struct inode *ip = v->file->ip;
    u64 index = v->pgoff + (va - v->start) / PAGE_SIZE;
    ilock(ip);
    if (index * PAGE_SIZE >= ip->size) {
        iunlock(ip);
        return -ENXIO;                  /* past the end of the file: SIGBUS */
    }
    bool major;
    int err = 0;
    struct page *pg = pc_get_spare(ip, index, &major, &err, spare);
    iunlock(ip);
    if (!pg)
        return err;
    if (major) {
        mm->major_faults++;
        vmc.major_faults++;
    }
    pte_t perms = prot_to_pte(v->prot);
    mm->rss++;
    if (v->shared) {
        if ((access & FAULT_WRITE) && (v->prot & VM_WRITE)) {
            page_set_flag(pg, PG_DIRTY);
            __atomic_add_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);
            *pte = PA_TO_PTE(page_to_pa(pg)) | perms | PTE_V;
        } else {
            *pte = PA_TO_PTE(page_to_pa(pg)) | (perms & ~PTE_W) | PTE_V;
        }
        return 0;
    }
    *pte = PA_TO_PTE(page_to_pa(pg)) | (perms & ~PTE_W) | (v->prot & VM_WRITE ? PTE_COW : 0) | PTE_V;
    if ((access & FAULT_WRITE) && (v->prot & VM_WRITE))
        return cow_break(mm, v, pte, va, spare);
    return 0;
}

/* force: ptrace's access, regardless of the VMA's permissions. */
static int fault_locked(struct mm *mm, uintptr_t addr, int access, bool force, struct page **spare)
{
    struct vma *v = vma_find(mm, addr);
    if (!v)
        return -EFAULT;
    if (!force && (((access & FAULT_WRITE) && !(v->prot & VM_WRITE)) ||
                   ((access & FAULT_EXEC) && !(v->prot & VM_EXEC)) ||
                   ((access & FAULT_READ) && !(v->prot & VM_READ))))
        return -EACCES;

    uintptr_t va = ALIGN_DOWN(addr, PAGE_SIZE);
    pte_t *pte = vm_walk(mm->pt, va, true);
    if (!pte)
        return -ENOMEM;
    int r = 0;
    if (!*pte) {                                    /* demand paging */
        if (v->kind == VMA_FILE) {
            r = file_fault(mm, v, pte, va, access, spare);
        } else {
            struct page *pg = take_spare(spare, true);
            if (!pg)
                return -EAGAIN;
            *pte = PA_TO_PTE(page_to_pa(pg)) | prot_to_pte(v->prot) | PTE_V;
            mm->rss++;
        }
    } else if (PTE_IS_SWAP(*pte)) {
        r = swap_in(mm, v, pte, va, access, spare);
    } else {
        if (!(*pte & PTE_A))
            *pte |= PTE_A;                          /* (reclaim cleared it; no Svadu) */
        if ((access & FAULT_WRITE) && !(*pte & PTE_W)) {
            struct page *pg = pa_to_page(PTE_PA(*pte));
            if (v->shared && pg && (pg->flags & PG_CACHE) && (v->prot & VM_WRITE || force)) {
                page_set_flag(pg, PG_DIRTY);        /* the first write to a shared file page */
                __atomic_add_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);
                *pte |= PTE_W;
            } else if (*pte & PTE_COW) {
                r = cow_break(mm, v, pte, va, spare);
            } else if (!force) {
                return -EACCES;                     /* shouldn't happen: not COW */
            }
        }
    }
    if (r != -EAGAIN) {
        mm->faults++;
        vmc.faults++;
    }
    if (mm->pt == vm_current())
        sfence_vma(va);
    return r;
}

/* Fault with mm->lock held: a page is allocated, if one is needed, with the
   lock dropped (and the fault looked at again: things may have changed). */
/* Bring swap slot `slot` into the swap cache (no locks held); the page is
   returned referenced, or NULL with *err set. */
static struct page *read_swap_slot(struct mm *mm, u32 slot, int *err)
{
    struct page *pg = pc_find(&swap_owner, 0, slot);
    if (pg)
        return pg;
    pg = user_page_alloc(false);
    if (!pg) {
        *err = -ENOMEM;
        return NULL;
    }
    if (swap_read(slot, pg)) {
        page_put(pg);
        *err = -EIO;
        return NULL;
    }
    struct page *other = pc_insert(pg, &swap_owner, 0, slot);    /* clean: the slot has it */
    if (other) {
        page_put(pg);
        return other;
    }
    swap_dup(slot);                     /* the swap cache's reference to the slot */
    mm->major_faults++;
    vmc.major_faults++;
    return pg;
}

static int fault_relock(struct mm *mm, uintptr_t addr, int access, bool force)
{
    struct fault_ctx ctx = { 0 };
    int r;
    for (int tries = 0; (r = fault_locked(mm, addr, access, force, &ctx.spare)) == -EAGAIN; tries++) {
        u32 slot = ctx.read_slot;
        ctx.read_slot = 0;
        mutex_unlock(&mm->lock);
        if (slot) {
            /* (the PTE keeps the slot: nobody else changes our PTEs to free it) */
            if (ctx.held)
                page_put(ctx.held);
            ctx.held = read_swap_slot(mm, slot, &r);
        } else if (!ctx.spare) {
            ctx.spare = user_page_alloc(false);
            if (!ctx.spare)
                r = -ENOMEM;
        }
        mutex_lock(&mm->lock);
        if (r != -EAGAIN)
            break;
        if (tries > 100) {
            r = -ENOMEM;                /* (memory too tight to make progress) */
            break;
        }
    }
    if (ctx.spare)
        page_put(ctx.spare);
    if (ctx.held)
        page_put(ctx.held);
    return r;
}

int uvm_fault(struct mm *mm, uintptr_t addr, int access)
{
    mutex_lock(&mm->lock);
    int r = fault_relock(mm, addr, access, false);
    mutex_unlock(&mm->lock);
    return r;
}

struct mm *mm_fork(struct mm *parent)
{
    struct mm *mm = mm_create();
    if (!mm)
        return NULL;
    mutex_lock(&parent->lock);
    mm->brk_start = parent->brk_start;
    mm->brk = parent->brk;

    struct vma *v;
    list_for_each_entry(v, &parent->vmas, node) {
        int err = 0;
        struct vma *n = vma_insert(mm, v->start, v->end, v->prot, v->kind, v->name, &err);
        if (!n)
            goto fail;
        if (v->file) {
            n->file = file_dup(v->file);
            n->pgoff = v->pgoff;
            n->shared = v->shared;
        }
        pte_t *ppte;
        for_each_used(parent->pt, va, v->start, v->end, ppte) {
            pte_t *cpte = vm_walk(mm->pt, va, true);
            if (!cpte)
                goto fail;
            if (PTE_IS_SWAP(*ppte)) {
                swap_dup(SWAP_SLOT(*ppte));
                *cpte = *ppte;
                mm->swapped++;
                continue;
            }
            struct page *pg = pa_to_page(PTE_PA(*ppte));
            if (v->shared) {
                if ((*ppte & PTE_W) && pg)
                    __atomic_add_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);
            } else if (*ppte & (PTE_W | PTE_COW)) {
                *ppte = (*ppte & ~PTE_W) | PTE_COW;
            }
            *cpte = *ppte;
            if (pg)
                page_get(pg);
            mm->rss++;
        }
    }
    tlb_changed(parent, 0);                         /* the parent lost write access */
    mutex_unlock(&parent->lock);
    return mm;
fail:
    tlb_changed(parent, 0);
    mutex_unlock(&parent->lock);
    mm_destroy(mm);
    return NULL;
}

/* Copy into another (possibly inactive) address space, faulting pages in. */
int uvm_write(struct mm *mm, uintptr_t dst, const void *src, size_t n)
{
    const u8 *s = src;
    mutex_lock(&mm->lock);
    while (n) {
        if (fault_relock(mm, dst, FAULT_WRITE, false)) {
            mutex_unlock(&mm->lock);
            return -EFAULT;
        }
        pte_t *pte = vm_walk(mm->pt, dst, false);
        size_t chunk = MIN(n, PAGE_SIZE - (dst & (PAGE_SIZE - 1)));
        memcpy((u8 *)__va(PTE_PA(*pte)) + (dst & (PAGE_SIZE - 1)), s, chunk);
        dst += chunk;
        s += chunk;
        n -= chunk;
    }
    mutex_unlock(&mm->lock);
    return 0;
}

/*
 * Read another address space through its page table; never faults pages in
 * and takes no lock (profiler samples, backtraces of a stopped process).
 * Page tables are only freed after their entry is cleared and flushed, so
 * the walk sees either the table or nothing.
 */
int uvm_read(struct mm *mm, uintptr_t src, void *dst, size_t n)
{
    u8 *d = dst;
    while (n) {
        uintptr_t pa;
        if (!user_range_ok(src, 1) || vm_translate(mm->pt, src, &pa, NULL))
            return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (src & (PAGE_SIZE - 1)));
        memcpy(d, __va(pa), chunk);
        src += chunk;
        d += chunk;
        n -= chunk;
    }
    return 0;
}

/*
 * A debugger's view of a stopped process's memory (ptrace). The page under
 * va, brought in if the VMA allows it to exist. For a write to a private
 * mapping the page is made private first: text pages are shared after fork
 * without COW marks, and a breakpoint must not show up in the other process
 * (or in the file). Writes to shared file mappings go to the file. Caller
 * holds mm->lock.
 */
static void *debug_page(struct mm *mm, uintptr_t va, bool for_write, bool *exec)
{
    struct vma *v = vma_find(mm, va);
    if (!v)
        return NULL;
    va = ALIGN_DOWN(va, PAGE_SIZE);
    pte_t *pte = vm_walk(mm->pt, va, false);
    if (!pte || !(*pte & PTE_V)) {
        if (fault_relock(mm, va, FAULT_READ, true))
            return NULL;
        pte = vm_walk(mm->pt, va, false);
        if (!pte || !(*pte & PTE_V))
            return NULL;
    }
    *exec = v->prot & VM_EXEC;
    if (for_write) {
        struct page *pg = pa_to_page(PTE_PA(*pte));
        if (v->shared) {
            if (pg && (pg->flags & PG_CACHE))
                page_set_flag(pg, PG_DIRTY);
        } else if (pg && (pg->refcount > 1 || (pg->flags & (PG_CACHE | PG_SWAPCACHE)))) {
            struct page *copy = user_page_alloc(false);
            if (!copy)
                return NULL;
            memcpy(page_to_va(copy), __va(PTE_PA(*pte)), PAGE_SIZE);
            pte_t flags = *pte & 0x3ff;
            if ((flags & PTE_COW) && (v->prot & VM_WRITE))
                flags = (flags | PTE_W) & ~PTE_COW;     /* ours now */
            *pte = PA_TO_PTE(page_to_pa(copy)) | flags;
            page_put(pg);
            mm->cow_copies++;
            tlb_changed(mm, va);
        }
    }
    return __va(PTE_PA(*pte));
}

int uvm_peek_locked(struct mm *mm, uintptr_t addr, void *dst, size_t n)
{
    u8 *d = dst;
    while (n) {
        bool exec;
        u8 *page = user_range_ok(addr, 1) ? debug_page(mm, addr, false, &exec) : NULL;
        if (!page)
            return -EFAULT;
        size_t off = addr & (PAGE_SIZE - 1), chunk = MIN(n, PAGE_SIZE - off);
        memcpy(d, page + off, chunk);
        addr += chunk;
        d += chunk;
        n -= chunk;
    }
    return 0;
}

int uvm_poke_locked(struct mm *mm, uintptr_t addr, const void *src, size_t n)
{
    const u8 *s = src;
    bool any_exec = false;
    int r = 0;
    while (n) {
        bool exec;
        u8 *page = user_range_ok(addr, 1) ? debug_page(mm, addr, true, &exec) : NULL;
        if (!page) {
            r = -EFAULT;
            break;
        }
        size_t off = addr & (PAGE_SIZE - 1), chunk = MIN(n, PAGE_SIZE - off);
        memcpy(page + off, s, chunk);
        any_exec |= exec;
        addr += chunk;
        s += chunk;
        n -= chunk;
    }
    if (any_exec) {
        /* new instructions: every hart's instruction cache must see them */
        asm volatile("fence.i" ::: "memory");
        sbi_remote_fence_i(0, -1UL);
    }
    return r;
}

int uvm_peek(struct mm *mm, uintptr_t addr, void *dst, size_t n)
{
    mutex_lock(&mm->lock);
    int r = uvm_peek_locked(mm, addr, dst, n);
    mutex_unlock(&mm->lock);
    return r;
}

int uvm_poke(struct mm *mm, uintptr_t addr, const void *src, size_t n)
{
    mutex_lock(&mm->lock);
    int r = uvm_poke_locked(mm, addr, src, n);
    mutex_unlock(&mm->lock);
    return r;
}

/* ---- mincore, swapoff ---- */

/*
 * mincore: for each page of [start, start + pages pages), whether it is in
 * memory: mapped here, or (for a file mapping) in the page cache. Caller
 * holds mm->lock; the range must be mapped.
 */
int uvm_mincore(struct mm *mm, uintptr_t start, size_t pages, u8 *out)
{
    for (size_t i = 0; i < pages; i++) {
        uintptr_t va = start + i * PAGE_SIZE;
        struct vma *v = vma_find(mm, va);
        if (!v)
            return -ENOMEM;
        pte_t *pte = vm_walk(mm->pt, va, false);
        out[i] = 0;
        if (pte && (*pte & PTE_V)) {
            out[i] = 1;
        } else if ((!pte || !*pte) && v->kind == VMA_FILE) {
            struct inode *ip = v->file->ip;
            struct page *pg = pc_find(ip->sb, ip->ino, v->pgoff + (va - v->start) / PAGE_SIZE);
            if (pg) {
                out[i] = 1;
                page_put(pg);
            }
        }
    }
    return 0;
}

/* swapoff: bring every swapped-out page of mm back (caller holds mm->lock;
   memory is allocated with it held: other processes give way). */
int uvm_swapin_all(struct mm *mm)
{
    struct vma *v;
    list_for_each_entry(v, &mm->vmas, node) {
        pte_t *pte;
        for_each_used(mm->pt, va, v->start, v->end, pte) {
            if (!PTE_IS_SWAP(*pte))
                continue;
            struct fault_ctx ctx = { 0 };
            int r;
            while ((r = swap_in(mm, v, pte, va, FAULT_READ, &ctx.spare)) == -EAGAIN && ctx.read_slot) {
                u32 slot = ctx.read_slot;
                ctx.read_slot = 0;
                if (ctx.held)
                    page_put(ctx.held);
                if (!(ctx.held = read_swap_slot(mm, slot, &r)))
                    break;
            }
            if (ctx.held)
                page_put(ctx.held);
            if (r)
                return r == -EAGAIN ? -ENOMEM : r;
        }
    }
    __atomic_add_fetch(&mm->tlb_gen, 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- reclaim, msync, mmap's address search ---- */

/*
 * Unmap up to `want` cold pages of mm (caller holds mm->lock). The page
 * references (and the writable-mapping counts of shared file pages) are
 * dropped only after every CPU has flushed the old translations: until then
 * the process may still write to a page through its TLB, so the page must
 * not be written out or freed yet.
 */
unsigned long uvm_reclaim(struct mm *mm, unsigned long want, unsigned long *scanned)
{
    enum { MAX = 64 };
    struct page *put[MAX];
    bool wrote[MAX];
    int nput = 0;
    bool changed = false;
    if (want > MAX)
        want = MAX;
    *scanned = 0;
    /* from where the last pass stopped, round to there once */
    struct vma *v;
    for (int lap = 0; lap < 2 && (unsigned long)nput < want; lap++) {
        list_for_each_entry(v, &mm->vmas, node) {
            uintptr_t from = lap == 0 ? MAX(v->start, mm->scan_va) : v->start;
            uintptr_t to = lap == 0 ? v->end : MIN(v->end, mm->scan_va);
            if (from >= to)
                continue;
            pte_t *pte;
            for_each_used(mm->pt, va, from, to, pte) {
                if (!(*pte & PTE_V))
                    continue;                       /* already in swap */
                (*scanned)++;
                mm->scan_va = va + PAGE_SIZE;
                if (*pte & PTE_A) {
                    *pte &= ~PTE_A;                 /* used lately: next time */
                    changed = true;
                    continue;
                }
                struct page *pg = pa_to_page(PTE_PA(*pte));
                if (!pg)
                    continue;
                if (pg->flags & PG_CACHE) {         /* a file page: the file has it */
                    wrote[nput] = *pte & PTE_W;
                    if (wrote[nput])
                        page_set_flag(pg, PG_DIRTY);
                    *pte = 0;
                } else {                            /* anonymous: to swap */
                    if (!swap_enabled())
                        continue;
                    u32 slot;
                    if (pg->flags & PG_SWAPCACHE) {
                        slot = (u32)pg->cache.index;
                    } else {
                        slot = swap_alloc();        /* count 1: the swap cache's */
                        if (!slot)
                            continue;               /* swap is full */
                        page_set_flag(pg, PG_DIRTY);
                        pc_insert(pg, &swap_owner, 0, slot);
                    }
                    swap_dup(slot);                 /* the PTE's */
                    *pte = SWAP_PTE(slot);
                    mm->swapped++;
                    wrote[nput] = false;
                }
                put[nput++] = pg;
                mm->rss--;
                changed = true;
                if ((unsigned long)nput >= want)
                    goto done;
            }
        }
        mm->scan_va = 0;
    }
done:
    if (changed) {
        /* the process may be running on another CPU */
        __atomic_add_fetch(&mm->tlb_gen, 1, __ATOMIC_RELEASE);
        tlb_shootdown();
    }
    for (int i = 0; i < nput; i++) {
        if (wrote[i])
            __atomic_sub_fetch(&put[i]->cache.wmap, 1, __ATOMIC_RELAXED);
        page_put(put[i]);
    }
    return nput;
}

/* The lowest free range of len bytes from USER_MMAP_BASE up, with a guard
   page on each side; 0 if there is none below the stack. */
uintptr_t uvm_find_gap(struct mm *mm, size_t len)
{
    uintptr_t limit = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
    uintptr_t cand = USER_MMAP_BASE;
    struct vma *v;
    list_for_each_entry(v, &mm->vmas, node) {   /* (sorted by address) */
        if (v->end + PAGE_SIZE <= cand)
            continue;                           /* below the candidate */
        if (cand + len + PAGE_SIZE <= v->start)
            break;                              /* fits before v */
        cand = v->end + PAGE_SIZE;
    }
    return cand + len <= limit ? cand : 0;
}

int uvm_msync(struct mm *mm, uintptr_t start, uintptr_t end)
{
    enum { MAXF = 8 };
    struct { struct file *f; u64 first, last; } todo[MAXF];
    int n = 0;
    mutex_lock(&mm->lock);
    for (uintptr_t a = start; a < end;) {
        struct vma *v = vma_find(mm, a);
        if (!v) {
            mutex_unlock(&mm->lock);
            for (int i = 0; i < n; i++)
                file_close(todo[i].f);
            return -ENOMEM;
        }
        uintptr_t s = a, e = MIN(end, v->end);
        a = e;
        if (!v->shared)
            continue;
        /* write-protect: the next write marks the page dirty again */
        pte_t *pte;
        for_each_used(mm->pt, va, s, e, pte) {
            if ((*pte & (PTE_V | PTE_W)) != (PTE_V | PTE_W))
                continue;
            struct page *pg = pa_to_page(PTE_PA(*pte));
            *pte &= ~PTE_W;
            if (pg) {
                page_set_flag(pg, PG_DIRTY);
                __atomic_sub_fetch(&pg->cache.wmap, 1, __ATOMIC_RELAXED);
            }
        }
        if (n < MAXF) {
            todo[n].f = file_dup(v->file);
            todo[n].first = v->pgoff + (s - v->start) / PAGE_SIZE;
            todo[n].last = v->pgoff + (e - v->start) / PAGE_SIZE - 1;
            n++;
        }
    }
    tlb_changed(mm, 0);
    mutex_unlock(&mm->lock);
    int err = 0;
    for (int i = 0; i < n; i++) {
        struct inode *ip = todo[i].f->ip;
        int r = pc_writeback(ip->sb, ip->ino, todo[i].first, todo[i].last, true);
        if (r && !err)
            err = r;
        file_close(todo[i].f);
    }
    return err;
}

/* ---- switching address spaces ---- */

void vm_activate(struct mm *mm)
{
    pagetable_t pt = mm ? mm->pt : kernel_pagetable;
    if (mm) {
        struct page *pg = va_to_page(pt);
        if (!pg || !(pg->flags & PG_PGTABLE) || (pg->flags & PG_FREE))
            panic("vm_activate: page table %p of mm %p is not a live page table "
                  "(flags %#x; freed while still in use?)", pt, mm, pg ? pg->flags : 0);
    }
    if (mm && mm->kernel_gen != kernel_root_gen) {
        vm_sync_kernel(mm->pt);
        mm->kernel_gen = kernel_root_gen;
    }
    vm_switch(pt, mm ? mm->id : 0, mm ? __atomic_load_n(&mm->tlb_gen, __ATOMIC_ACQUIRE) : 0);
}

void mm_print(print_fn print, struct mm *mm)
{
    struct vma *v;
    if (!mm) {
        print("  (no address space)\n");
        return;
    }
    /* page counts walk the page table: only if nothing is changing it */
    bool counts = mutex_trylock(&mm->lock);
    unsigned long flags;
    bool locked = spin_lock_debug(&mm->vma_lock, &flags);
    print("  %-33s %-4s %-6s %8s %6s  %s\n", "range", "prot", "kind", "resident", "swap", "name");
    list_for_each_entry(v, &mm->vmas, node) {
        u64 present = 0, cow = 0, swapped = 0, dirty = 0;
        pte_t *pte;
        if (counts) {
            for_each_used(mm->pt, va, v->start, v->end, pte) {
                if (!(*pte & PTE_V)) {
                    swapped++;
                    continue;
                }
                present++;
                if (*pte & PTE_COW)
                    cow++;
                struct page *pg = pa_to_page(PTE_PA(*pte));
                if (pg && (pg->flags & PG_CACHE) && (pg->flags & PG_DIRTY))
                    dirty++;
            }
        }
        char extra[48] = "";
        int k = 0;
        if (cow)
            k += ksnprintf(extra + k, sizeof(extra) - k, "(%lu shared COW) ", (unsigned long)cow);
        if (dirty)
            k += ksnprintf(extra + k, sizeof(extra) - k, "(%lu dirty) ", (unsigned long)dirty);
        if (v->file)
            ksnprintf(extra + k, sizeof(extra) - k, "%s+0x%lx %s", v->shared ? "shared" : "private",
                      (unsigned long)(v->pgoff * PAGE_SIZE), v->file->path);
        print("  %016lx-%016lx %c%c%c  %-6s %5lu pg %6lu  %-10s %s\n", (unsigned long)v->start,
              (unsigned long)v->end, v->prot & VM_READ ? 'r' : '-',
              v->prot & VM_WRITE ? 'w' : '-', v->prot & VM_EXEC ? 'x' : '-',
              vma_kind_name(v->kind), (unsigned long)present, (unsigned long)swapped, v->name, extra);
    }
    spin_unlock_debug(&mm->vma_lock, locked, flags);
    if (counts)
        mutex_unlock(&mm->lock);
    else
        print("  (the address space is changing: page counts not shown)\n");
    print("  rss %lu pages, %lu swapped, %lu faults (%lu major), %lu COW copies, brk %#lx-%#lx\n",
          (unsigned long)mm->rss, (unsigned long)mm->swapped, (unsigned long)mm->faults,
          (unsigned long)mm->major_faults, (unsigned long)mm->cow_copies,
          (unsigned long)mm->brk_start, (unsigned long)mm->brk);
}
