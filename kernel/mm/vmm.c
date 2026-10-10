/*
 * Sv39 page tables.
 *
 * boot.S runs the kernel on an early table (1 GiB pages, all RWX). vm_init()
 * replaces it with the real kernel table:
 *   - kernel image with per-section permissions: text r-x, rodata r--,
 *     data/bss rw-, ksyms r--  (W^X: nothing is both writable and executable)
 *   - the rest of RAM rw- (direct map), using 2 MiB pages where aligned
 *   - MMIO regions registered with ioremap() rw-
 *   - nothing else: firmware memory, user space and NULL are unmapped
 *
 * Debugging: `pt` in the monitor (vm_dump / vm_walk_print), eu-vm in GDB, and
 * vm_fault_hint() which panics use to explain page faults.
 */
#define LOG_SUBSYS SUBSYS_MM
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <spinlock.h>

#define SATP_MODE_SV39  (8UL << 60)
#define MAX_IOREMAP     32

pagetable_t kernel_pagetable;
u32 kernel_root_gen;
/*
 * Serializes changes to the kernel page table (kernel stacks come and go on
 * every CPU). User page tables belong to their process's single thread.
 */
static spinlock_t kpt_lock = SPINLOCK_INIT("kernel-pt");
static bool vm_ready;           /* kernel_pagetable is live (vm_init finished) */

static struct {
    uintptr_t pa;
    size_t size;
} ioremaps[MAX_IOREMAP];
static int nr_ioremaps;

static bool va_canonical(uintptr_t va)
{
    /* Sv39: bits 63..39 must all equal bit 38. */
    i64 sv = (i64)va;
    return (sv << 25) >> 25 == sv;
}

static pagetable_t alloc_table(void)
{
    pagetable_t t = page_alloc_zeroed();
    if (t)
        va_to_page(t)->flags |= PG_PGTABLE;
    return t;
}

void pte_flags_str(pte_t pte, char *buf)
{
    buf[0] = pte & PTE_R ? 'r' : '-';
    buf[1] = pte & PTE_W ? 'w' : '-';
    buf[2] = pte & PTE_X ? 'x' : '-';
    buf[3] = pte & PTE_U ? 'u' : '-';
    buf[4] = pte & PTE_G ? 'g' : '-';
    buf[5] = pte & PTE_A ? 'a' : '-';
    buf[6] = pte & PTE_D ? 'd' : '-';
    buf[7] = pte & PTE_V ? ' ' : '!';
    buf[8] = '\0';
}

/*
 * Find the PTE mapping va. Stops at a leaf at any level (*level_out says
 * which). With alloc, creates intermediate tables down to target_level.
 */
static pagetable_t current_pagetable(void);

static pte_t *walk(pagetable_t pt, uintptr_t va, bool alloc, int target_level, int *level_out)
{
    pagetable_t root = pt;
    for (int level = 2; level > target_level; level--) {
        pte_t *pte = &pt[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_LEAF(*pte)) {
                if (level_out)
                    *level_out = level;
                return pte;
            }
            pt = __va(PTE_PA(*pte));
        } else {
            if (!alloc)
                return NULL;
            pagetable_t next = alloc_table();
            if (!next)
                return NULL;
            *pte = PA_TO_PTE(__pa(next)) | PTE_V;
            if (level == 2 && root == kernel_pagetable && va >= KERNEL_BASE && vm_ready) {
                /*
                 * New kernel root entry: process page tables must pick it up
                 * (the active one now, others in vm_activate). Not before
                 * vm_init is done: the active table is then the boot-time
                 * early table, whose entries must not be touched.
                 */
                kernel_root_gen++;
                pagetable_t active = current_pagetable();
                if (active != kernel_pagetable)
                    active[PX(2, va)] = *pte;
            }
            pt = next;
        }
    }
    if (level_out)
        *level_out = target_level;
    return &pt[PX(target_level, va)];
}

static int do_map(pagetable_t pt, uintptr_t va, uintptr_t pa, size_t size, u64 perm);
static int do_unmap(pagetable_t pt, uintptr_t va, size_t size);

int vm_map(pagetable_t pt, uintptr_t va, uintptr_t pa, size_t size, u64 perm)
{
    if (pt != kernel_pagetable || !vm_ready)
        return do_map(pt, va, pa, size, perm);
    unsigned long flags = spin_lock_irqsave(&kpt_lock);
    int r = do_map(pt, va, pa, size, perm);
    spin_unlock_irqrestore(&kpt_lock, flags);
    return r;
}

/*
 * Removing kernel mappings: the caller must tlb_shootdown() before reusing
 * the memory (other CPUs may still cache the old translation).
 */
int vm_unmap(pagetable_t pt, uintptr_t va, size_t size)
{
    if (pt != kernel_pagetable)
        return do_unmap(pt, va, size);
    unsigned long flags = spin_lock_irqsave(&kpt_lock);
    int r = do_unmap(pt, va, size);
    spin_unlock_irqrestore(&kpt_lock, flags);
    return r;
}

/*
 * Page fault on a kernel address while a process page table is active: if
 * the kernel page table has a top-level entry this table lacks (added on
 * another CPU since vm_activate last synced it), copy it and retry.
 */
bool vm_kernel_fault_fixup(uintptr_t va)
{
    pagetable_t cur = current_pagetable();
    if (!vm_ready || cur == kernel_pagetable || !va_canonical(va))
        return false;
    int i = PX(2, va);
    pte_t k = kernel_pagetable[i];
    if (!(k & PTE_V) || cur[i] == k)
        return false;
    cur[i] = k;
    sfence_vma_all();
    return true;
}

static int do_map(pagetable_t pt, uintptr_t va, uintptr_t pa, size_t size, u64 perm)
{
    if ((va | pa | size) & (PAGE_SIZE - 1) || !va_canonical(va)) {
        pr_err("vm_map: bad arguments va=%#lx pa=%#lx size=%#zx\n",
               (unsigned long)va, (unsigned long)pa, size);
        return -1;
    }

    while (size) {
        int level = 2;
        while (level > 0 && (((va | pa) & (LEVEL_SIZE(level) - 1)) || size < LEVEL_SIZE(level)))
            level--;

        int got;
        pte_t *pte = walk(pt, va, true, level, &got);
        if (!pte)
            return -1;
        if (got != level || (*pte & PTE_V)) {
            pr_err("vm_map: va %#lx already mapped (pte %#lx)\n",
                   (unsigned long)va, (unsigned long)*pte);
            return -1;
        }
        *pte = PA_TO_PTE(pa) | perm | PTE_V;
        va += LEVEL_SIZE(level);
        pa += LEVEL_SIZE(level);
        size -= LEVEL_SIZE(level);
    }
    sfence_vma_all();
    return 0;
}

static int do_unmap(pagetable_t pt, uintptr_t va, size_t size)
{
    while (size) {
        int level;
        pte_t *pte = walk(pt, va, false, 0, &level);
        if (!pte || !(*pte & PTE_V)) {
            pr_err("vm_unmap: va %#lx not mapped\n", (unsigned long)va);
            return -1;
        }
        if ((va & (LEVEL_SIZE(level) - 1)) || size < LEVEL_SIZE(level)) {
            pr_err("vm_unmap: partial unmap of a level-%d page at %#lx\n", level, (unsigned long)va);
            return -1;
        }
        *pte = 0;
        sfence_vma(va);
        va += LEVEL_SIZE(level);
        size -= LEVEL_SIZE(level);
    }
    return 0;
}

int vm_translate(pagetable_t pt, uintptr_t va, uintptr_t *pa, pte_t *pte_out)
{
    int level;
    if (!va_canonical(va))
        return -1;
    pte_t *pte = walk(pt, va, false, 0, &level);
    if (!pte || !(*pte & PTE_V) || !PTE_LEAF(*pte))
        return -1;
    if (pa)
        *pa = PTE_PA(*pte) + (va & (LEVEL_SIZE(level) - 1));
    if (pte_out)
        *pte_out = *pte;
    return 0;
}

static pagetable_t current_pagetable(void)
{
    return __va((csr_read(satp) & ((1UL << 44) - 1)) << PAGE_SHIFT);
}

pagetable_t vm_current(void)
{
    return current_pagetable();
}

pte_t *vm_walk(pagetable_t pt, uintptr_t va, bool alloc)
{
    int level;
    if (!va_canonical(va))
        return NULL;
    pte_t *pte = walk(pt, va, alloc, 0, &level);
    return pte && level == 0 ? pte : NULL;
}

void vm_sync_kernel(pagetable_t pt)
{
    for (int i = 256; i < 512; i++)
        pt[i] = kernel_pagetable[i];
}

void *ioremap(uintptr_t pa, size_t size)
{
    uintptr_t start = ALIGN_DOWN(pa, PAGE_SIZE);
    size_t len = PAGE_ALIGN(pa + size) - start;

    for (int i = 0; i < nr_ioremaps; i++)
        if (ioremaps[i].pa == start && ioremaps[i].size >= len)
            return __va(pa);
    if (nr_ioremaps == MAX_IOREMAP)
        panic("ioremap: too many regions");
    ioremaps[nr_ioremaps].pa = start;
    ioremaps[nr_ioremaps].size = len;
    nr_ioremaps++;

    /* Before vm_init the early table maps all low physical memory; afterwards map now. */
    if (kernel_pagetable && vm_map(kernel_pagetable, (uintptr_t)__va(start), start, len, PROT_KERNEL_RW))
        panic("ioremap of %#lx failed", (unsigned long)pa);
    return __va(pa);
}

static void map_or_die(uintptr_t pa_start, uintptr_t pa_end, u64 perm, const char *what)
{
    if (pa_end <= pa_start)
        return;
    if (vm_map(kernel_pagetable, (uintptr_t)__va(pa_start), pa_start, pa_end - pa_start, perm))
        panic("vm_init: cannot map %s", what);
    char f[9];
    pte_flags_str(perm | PTE_V, f);
    pr_debug("map %#lx-%#lx %s %s\n", (unsigned long)__va(pa_start), (unsigned long)__va(pa_end), f, what);
}

void vm_init(void)
{
    uintptr_t ram_end = platform.mem_base + platform.mem_size;

    kernel_pagetable = alloc_table();
    if (!kernel_pagetable)
        panic("vm_init: no memory for the kernel page table");

    map_or_die(__pa(__kernel_start), __pa(__text_end), PROT_KERNEL_RX, "kernel text");
    map_or_die(__pa(__rodata_start), __pa(__rodata_end), PROT_KERNEL_RO, "kernel rodata");
    map_or_die(__pa(__data_start), __pa(__data_end), PROT_KERNEL_RW, "kernel data+bss");
    /* Boot and emergency stacks: map only the upper half of each slot. */
    for (uintptr_t slot = (uintptr_t)__stacks_start; slot < (uintptr_t)__stacks_end; slot += KSTACK_SLOT)
        map_or_die(__pa(slot + KSTACK_SLOT - KSTACK_SIZE), __pa(slot + KSTACK_SLOT),
                   PROT_KERNEL_RW, "boot/emergency stack");
    map_or_die(__pa(__ksyms_start), __pa(__kernel_end), PROT_KERNEL_RO, "kernel symbols");
    map_or_die(__pa(__kernel_end), ram_end, PROT_KERNEL_RW, "RAM (direct map)");
    for (int i = 0; i < nr_ioremaps; i++)
        map_or_die(ioremaps[i].pa, ioremaps[i].pa + ioremaps[i].size, PROT_KERNEL_RW, "MMIO");

    csr_write(satp, SATP_MODE_SV39 | (__pa(kernel_pagetable) >> PAGE_SHIFT));
    sfence_vma_all();
    vm_ready = true;
    pr_info("paging on: kernel page table at pa %#lx, W^X kernel image, NULL unmapped\n",
            (unsigned long)__pa(kernel_pagetable));
}

/* ---- debugging ---- */

static uintptr_t sign_extend_va(uintptr_t va)
{
    return (uintptr_t)(((i64)va << 25) >> 25);
}

struct dump_state {
    print_fn print;
    bool active;
    uintptr_t va_start, va_end, pa_start;
    u64 perm;
    u64 count[3];
    u64 total;
};

static void flush_run(struct dump_state *st)
{
    if (!st->active)
        return;
    char f[9];
    pte_flags_str(st->perm | PTE_V, f);
    u64 size = st->va_end - st->va_start;
    st->print("  %016lx-%016lx -> %010lx  %8lu KiB  %s %s%s%s\n",
              (unsigned long)st->va_start, (unsigned long)st->va_end - 1,
              (unsigned long)st->pa_start, (unsigned long)(size >> 10), f,
              st->count[2] ? "1G " : "", st->count[1] ? "2M " : "", st->count[0] ? "4K" : "");
    st->active = false;
    st->count[0] = st->count[1] = st->count[2] = 0;
}

static void dump_leaf(struct dump_state *st, uintptr_t va, pte_t pte, int level)
{
    uintptr_t pa = PTE_PA(pte);
    u64 perm = pte & PTE_PERM_MASK & ~PTE_V;
    if (st->active && va == st->va_end && perm == st->perm &&
        pa == st->pa_start + (st->va_end - st->va_start)) {
        st->va_end += LEVEL_SIZE(level);
    } else {
        flush_run(st);
        st->active = true;
        st->va_start = va;
        st->va_end = va + LEVEL_SIZE(level);
        st->pa_start = pa;
        st->perm = perm;
    }
    st->count[level]++;
    st->total += LEVEL_SIZE(level);
}

static void dump_level(struct dump_state *st, pagetable_t t, int level, uintptr_t va_base)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = t[i];
        uintptr_t va = sign_extend_va(va_base | ((uintptr_t)i << (PAGE_SHIFT + 9 * level)));
        if (!(pte & PTE_V)) {
            flush_run(st);
            continue;
        }
        if (PTE_LEAF(pte) || level == 0)
            dump_leaf(st, va, pte, level);
        else
            dump_level(st, __va(PTE_PA(pte)), level - 1, va);
    }
}

void vm_dump(pagetable_t pt, print_fn print)
{
    struct dump_state st = { .print = print };
    if (!pt)
        pt = current_pagetable();
    print("page table at pa %#lx%s:\n", (unsigned long)__pa(pt),
          pt == kernel_pagetable ? " (kernel)" : "");
    print("  %-33s    %-10s  %12s  %s\n", "virtual", "physical", "size", "flags");
    dump_level(&st, pt, 2, 0);
    flush_run(&st);
    print("  total mapped: %lu KiB\n", (unsigned long)(st.total >> 10));
}

void vm_walk_print(pagetable_t pt, uintptr_t va, print_fn print)
{
    if (!pt)
        pt = current_pagetable();
    if (!va_canonical(va)) {
        print("%#lx is not a canonical Sv39 address (bits 63..39 must equal bit 38)\n",
              (unsigned long)va);
        return;
    }
    print("walk %#lx in page table at pa %#lx:\n", (unsigned long)va, (unsigned long)__pa(pt));
    for (int level = 2; level >= 0; level--) {
        int idx = PX(level, va);
        pte_t pte = pt[idx];
        char f[9], line[160];
        pte_flags_str(pte, f);
        int n = ksnprintf(line, sizeof(line), "  L%d[%3d] @ %#lx: pte %016lx  %s", level, idx,
                          (unsigned long)__pa(&pt[idx]), (unsigned long)pte, f);
        if (!(pte & PTE_V)) {
            print("%s -> not present\n", line);
            return;
        }
        if (PTE_LEAF(pte)) {
            uintptr_t pa = PTE_PA(pte) + (va & (LEVEL_SIZE(level) - 1));
            ksnprintf(line + n, sizeof(line) - n, " -> %s page, pa %#lx",
                      level == 2 ? "1G" : level == 1 ? "2M" : "4K", (unsigned long)pa);
            print("%s\n", line);
            return;
        }
        print("%s -> next table\n", line);
        pt = __va(PTE_PA(pte));
    }
}

static bool all_bytes(uintptr_t v, u8 b, int nbytes)
{
    for (int i = 0; i < nbytes; i++, v >>= 8)
        if ((u8)v != b)
            return false;
    return true;
}

const char *vm_fault_hint(uintptr_t addr, u64 scause)
{
    u64 code = SCAUSE_CODE(scause);
    bool store = code == EXC_STORE_PAGE_FAULT || code == EXC_STORE_ACCESS;

    if (addr < PAGE_SIZE)
        return "NULL pointer dereference";
    if (addr < 0x10000)
        return "near-NULL address: member of a NULL struct pointer?";
    if (all_bytes(addr >> 16, POISON_UNINIT, 6))
        return "pointer loaded from uninitialized kmalloc memory (0x5a poison)";
    if (all_bytes(addr >> 16, POISON_FREE, 6))
        return "pointer loaded from freed memory (0x6b poison): use after free";
    if (all_bytes(addr >> 16, POISON_REDZONE, 6))
        return "pointer loaded from a slab redzone (0xbb): out-of-bounds read";
    if (!va_canonical(addr))
        return "non-canonical address: corrupted pointer";
    if (addr < KERNEL_BASE)
        return "user-space address accessed from the kernel";
    if (addr_in_stack_guard(addr))
        return "kernel stack overflow (access to a stack guard page)";
    if (addr >= (uintptr_t)__kernel_start && addr < (uintptr_t)__rodata_end && store)
        return "write to read-only kernel text/rodata (W^X)";
    if (addr >= (uintptr_t)__ksyms_start && addr < (uintptr_t)__kernel_end && store)
        return "write to the read-only kernel symbol table";
    uintptr_t pa = __pa(addr);
    if (pa >= platform.mem_base && pa < __pa(__kernel_start))
        return "access to firmware (OpenSBI) memory";
    if (pa < platform.mem_base || pa >= platform.mem_base + platform.mem_size)
        return "unmapped physical address: MMIO region not ioremap()ed?";
    return NULL;
}
