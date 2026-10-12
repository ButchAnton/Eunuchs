/*
 * User address spaces: VMAs, demand paging, copy-on-write fork.
 *
 * Every user page is reference counted (struct page refcount). fork shares
 * all pages: writable ones become read-only + PTE_COW in both processes and
 * are copied on the first write (or just made writable again if the other
 * side has already dropped its reference).
 *
 * Locking: an mm is only touched by its own (single) thread, or by fork /
 * exit / exec running in that thread, so no lock is needed yet.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <proc.h>
#include <riscv.h>
#include <sbi.h>
#include <uapi/errno.h>

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
    }
    return "?";
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
    spin_init(&mm->vma_lock, "vma");
    mm->mmap_next = USER_MMAP_BASE;
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

int vma_add(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot, enum vma_kind kind,
            const char *name)
{
    if ((start | end) & (PAGE_SIZE - 1) || start >= end || end > USER_TOP || start < PAGE_SIZE)
        return -EINVAL;
    struct vma *v;
    struct list_head *pos = &mm->vmas;
    list_for_each_entry(v, &mm->vmas, node) {
        if (start < v->end && v->start < end)
            return -EEXIST;
        if (v->start >= end) {
            pos = &v->node;
            break;
        }
    }
    struct vma *n = kzalloc(sizeof(*n));
    if (!n)
        return -ENOMEM;
    n->start = start;
    n->end = end;
    n->prot = prot;
    n->kind = kind;
    strlcpy(n->name, name, sizeof(n->name));
    unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
    list_add_tail(&n->node, pos);       /* before pos: keeps the list sorted */
    spin_unlock_irqrestore(&mm->vma_lock, flags);
    return 0;
}

/*
 * The next present page at or after *va (below end), skipping empty page
 * tables whole (1 GiB / 2 MiB at a time) instead of looking up every page:
 * a process's 8 MiB stack is mostly unmapped. (The profiler found
 * per-page lookups in unmap_range taking a fifth of the CPU in a
 * fork/exec-heavy load.) User page tables have only 4 KiB leaves.
 */
static pte_t *next_present(pagetable_t root, uintptr_t *va, uintptr_t end)
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
        if (*l0 & PTE_V)
            return l0;
        *va += PAGE_SIZE;
    }
    return NULL;
}

#define for_each_present(pt, va, start, end, pte)                              \
    for (uintptr_t va = (start); ((pte) = next_present((pt), &va, (end))) != NULL; \
         va += PAGE_SIZE)

/* Drop every mapped page in [start, end). */
static void unmap_range(struct mm *mm, uintptr_t start, uintptr_t end)
{
    pte_t *pte;
    for_each_present(mm->pt, va, start, end, pte) {
        struct page *pg = pa_to_page(PTE_PA(*pte));
        *pte = 0;
        mm->rss--;
        if (pg)
            page_put(pg);
    }
    if (mm->pt == vm_current())
        sfence_vma_all();
}

int vma_unmap(struct mm *mm, uintptr_t start, uintptr_t end)
{
    struct vma *v, *tmp;
    if ((start | end) & (PAGE_SIZE - 1) || start >= end || end > USER_TOP)
        return -EINVAL;
    list_for_each_entry_safe(v, tmp, &mm->vmas, node) {
        if (end <= v->start || start >= v->end)
            continue;
        uintptr_t s = MAX(start, v->start), e = MIN(end, v->end);
        unmap_range(mm, s, e);
        unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
        if (s == v->start && e == v->end) {
            list_del(&v->node);
            spin_unlock_irqrestore(&mm->vma_lock, flags);
            kfree(v);
        } else if (s == v->start) {
            v->start = e;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        } else if (e == v->end) {
            v->end = s;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        } else {
            /* Hole in the middle: split. */
            uintptr_t old_end = v->end;
            v->end = s;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
            int r = vma_add(mm, e, old_end, v->prot, v->kind, v->name);
            if (r)
                return r;
        }
    }
    return 0;
}

/*
 * mprotect: give [start, end) new permissions, splitting VMAs at the edges.
 * The whole range must be mapped. Write permission is never put straight
 * into a PTE: fork shares read-only pages without marking them COW, so a page
 * that becomes writable is marked COW and the first write fault takes it (or
 * copies it if it is shared). Pages that become executable get the
 * instruction caches of every hart synchronized.
 */
int vma_protect(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot)
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
        uintptr_t s = MAX(start, v->start), e = MIN(end, v->end);
        /* split off the parts outside the range; they keep the old protection */
        if (s > v->start) {
            uintptr_t old_start = v->start;
            unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
            v->start = s;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
            int r = vma_add(mm, old_start, s, v->prot, v->kind, v->name);
            if (r) {
                flags = spin_lock_irqsave(&mm->vma_lock);
                v->start = old_start;
                spin_unlock_irqrestore(&mm->vma_lock, flags);
                return r;
            }
        }
        if (e < v->end) {
            uintptr_t old_end = v->end;
            unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
            v->end = e;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
            int r = vma_add(mm, e, old_end, v->prot, v->kind, v->name);
            if (r) {
                flags = spin_lock_irqsave(&mm->vma_lock);
                v->end = old_end;
                spin_unlock_irqrestore(&mm->vma_lock, flags);
                return r;
            }
        }
        v->prot = prot;
        pte_t *pte;
        for_each_present(mm->pt, va, s, e, pte) {
            pte_t old = *pte, n = old & ~(PTE_R | PTE_W | PTE_X);
            if (prot & VM_READ)
                n |= PTE_R;
            if (prot & VM_EXEC)
                n |= PTE_X;
            if (old & PTE_W)
                n |= prot & VM_WRITE ? PTE_W : PTE_COW;
            else if (prot & VM_WRITE)
                n |= PTE_COW;
            *pte = n;
        }
    }
    if (mm->pt == vm_current())
        sfence_vma_all();
    if (prot & VM_EXEC) {
        asm volatile("fence.i" ::: "memory");
        sbi_remote_fence_i(0, -1UL);
    }
    return 0;
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
    list_for_each_entry_safe(v, tmp, &mm->vmas, node) {
        unmap_range(mm, v->start, v->end);
        list_del(&v->node);
        kfree(v);
    }
    if (mm->rss)
        pr_warn("mm_destroy: %lu pages still counted resident (accounting bug)\n",
                (unsigned long)mm->rss);
    free_tables(mm->pt, 2);
    kfree(mm);
}

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
    void *page = page_alloc_zeroed();
    if (!page)
        return NULL;
    *pte = PA_TO_PTE(__pa(page)) | prot_to_pte(v->prot) | PTE_V;
    mm->rss++;
    return page;
}

int uvm_fault(struct mm *mm, uintptr_t addr, int access)
{
    struct vma *v = vma_find(mm, addr);
    if (!v)
        return -EFAULT;
    if (((access & FAULT_WRITE) && !(v->prot & VM_WRITE)) ||
        ((access & FAULT_EXEC) && !(v->prot & VM_EXEC)) ||
        ((access & FAULT_READ) && !(v->prot & VM_READ)))
        return -EACCES;

    uintptr_t va = ALIGN_DOWN(addr, PAGE_SIZE);
    pte_t *pte = vm_walk(mm->pt, va, true);
    if (!pte)
        return -ENOMEM;
    mm->faults++;

    if (!(*pte & PTE_V)) {                          /* demand paging */
        if (!uvm_populate(mm, va))
            return -ENOMEM;
    } else if ((access & FAULT_WRITE) && (*pte & PTE_COW)) {
        struct page *pg = pa_to_page(PTE_PA(*pte));
        if (pg->refcount == 1) {                    /* last user: just take it */
            *pte = (*pte | PTE_W) & ~PTE_COW;
        } else {
            void *copy = page_alloc();
            if (!copy)
                return -ENOMEM;
            memcpy(copy, __va(PTE_PA(*pte)), PAGE_SIZE);
            *pte = PA_TO_PTE(__pa(copy)) | prot_to_pte(v->prot) | PTE_V;
            page_put(pg);
            mm->cow_copies++;
        }
    } else if ((access & FAULT_WRITE) && !(*pte & PTE_W)) {
        return -EACCES;                             /* shouldn't happen: not COW */
    }
    if (mm->pt == vm_current())
        sfence_vma(va);
    return 0;
}

struct mm *mm_fork(struct mm *parent)
{
    struct mm *mm = mm_create();
    if (!mm)
        return NULL;
    mm->brk_start = parent->brk_start;
    mm->brk = parent->brk;
    mm->mmap_next = parent->mmap_next;

    struct vma *v;
    list_for_each_entry(v, &parent->vmas, node) {
        if (vma_add(mm, v->start, v->end, v->prot, v->kind, v->name))
            goto fail;
        pte_t *ppte;
        for_each_present(parent->pt, va, v->start, v->end, ppte) {
            pte_t *cpte = vm_walk(mm->pt, va, true);
            if (!cpte)
                goto fail;
            if (*ppte & (PTE_W | PTE_COW))
                *ppte = (*ppte & ~PTE_W) | PTE_COW;
            *cpte = *ppte;
            page_get(pa_to_page(PTE_PA(*ppte)));
            mm->rss++;
        }
    }
    if (parent->pt == vm_current())
        sfence_vma_all();                           /* parent lost write access */
    return mm;
fail:
    mm_destroy(mm);
    return NULL;
}

/* Copy into another (possibly inactive) address space, faulting pages in. */
int uvm_write(struct mm *mm, uintptr_t dst, const void *src, size_t n)
{
    const u8 *s = src;
    while (n) {
        if (uvm_fault(mm, dst, FAULT_WRITE))
            return -EFAULT;
        pte_t *pte = vm_walk(mm->pt, dst, false);
        size_t chunk = MIN(n, PAGE_SIZE - (dst & (PAGE_SIZE - 1)));
        memcpy((u8 *)__va(PTE_PA(*pte)) + (dst & (PAGE_SIZE - 1)), s, chunk);
        dst += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

/* Read another address space through its page table; never faults pages in. */
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
    if (pt != vm_current()) {
        csr_write(satp, (8UL << 60) | (__pa(pt) >> PAGE_SHIFT));
        sfence_vma_all();
    }
}

void mm_print(print_fn print, struct mm *mm)
{
    struct vma *v;
    if (!mm) {
        print("  (no address space)\n");
        return;
    }
    unsigned long flags;
    bool locked = spin_lock_debug(&mm->vma_lock, &flags);
    print("  %-33s %-4s %-6s %8s  %s\n", "range", "prot", "kind", "resident", "name");
    list_for_each_entry(v, &mm->vmas, node) {
        u64 present = 0, cow = 0;
        pte_t *pte;
        for_each_present(mm->pt, va, v->start, v->end, pte) {
            present++;
            if (*pte & PTE_COW)
                cow++;
        }
        char extra[32] = "";
        if (cow)
            ksnprintf(extra, sizeof(extra), "(%lu shared COW)", (unsigned long)cow);
        print("  %016lx-%016lx %c%c%c  %-6s %5lu pg  %-10s %s\n", (unsigned long)v->start,
              (unsigned long)v->end, v->prot & VM_READ ? 'r' : '-',
              v->prot & VM_WRITE ? 'w' : '-', v->prot & VM_EXEC ? 'x' : '-',
              vma_kind_name(v->kind), (unsigned long)present, v->name, extra);
    }
    spin_unlock_debug(&mm->vma_lock, locked, flags);
    print("  rss %lu pages, %lu faults, %lu COW copies, brk %#lx-%#lx\n",
          (unsigned long)mm->rss, (unsigned long)mm->faults, (unsigned long)mm->cow_copies,
          (unsigned long)mm->brk_start, (unsigned long)mm->brk);
}
