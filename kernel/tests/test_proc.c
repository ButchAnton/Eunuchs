#include <fs.h>
#include <kstring.h>
#include <ktest.h>
#include <mm.h>
#include <proc.h>
#include <uapi/errno.h>

static u64 free_now(void)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    return st.free_pages;
}

KTEST(vma_add_find_overlap_unmap)
{
    struct mm *mm = mm_create();
    KT_ASSERT(mm != NULL);
    KT_EXPECT_EQ(vma_add(mm, 0x10000, 0x20000, VM_READ, VMA_ANON, "a"), 0);
    KT_EXPECT_EQ(vma_add(mm, 0x30000, 0x40000, VM_READ, VMA_ANON, "b"), 0);
    KT_EXPECT_EQ(vma_add(mm, 0x1f000, 0x31000, VM_READ, VMA_ANON, "x"), -EEXIST);
    KT_EXPECT_EQ(vma_add(mm, 0x10001, 0x20000, VM_READ, VMA_ANON, "x"), -EINVAL);
    KT_EXPECT(vma_find(mm, 0x1ffff) && !strcmp(vma_find(mm, 0x1ffff)->name, "a"));
    KT_EXPECT(vma_find(mm, 0x20000) == NULL);

    /* Punch a hole in the middle of "b": it splits in two. */
    KT_EXPECT_EQ(vma_unmap(mm, 0x34000, 0x36000), 0);
    KT_EXPECT(vma_find(mm, 0x33fff) != NULL);
    KT_EXPECT(vma_find(mm, 0x35000) == NULL);
    KT_EXPECT(vma_find(mm, 0x36000) != NULL);
    mm_destroy(mm);
}

KTEST(demand_paging_and_cow)
{
    u64 before = free_now();
    struct mm *a = mm_create();
    KT_ASSERT(a != NULL);
    KT_ASSERT(vma_add(a, 0x100000, 0x104000, VM_READ | VM_WRITE, VMA_ANON, "rw") == 0);
    KT_ASSERT(vma_add(a, 0x200000, 0x201000, VM_READ, VMA_ANON, "ro") == 0);

    KT_EXPECT_EQ(a->rss, 0);
    KT_EXPECT_EQ(uvm_fault(a, 0x100123, FAULT_WRITE), 0);           /* demand-zero */
    KT_EXPECT_EQ(a->rss, 1);
    KT_EXPECT_EQ(uvm_fault(a, 0x200000, FAULT_WRITE), -EACCES);     /* read-only VMA */
    KT_EXPECT_EQ(uvm_fault(a, 0x300000, FAULT_READ), -EFAULT);      /* no VMA */
    u32 v = 0xabcd;
    KT_ASSERT(uvm_write(a, 0x100200, &v, sizeof(v)) == 0);

    struct mm *b = mm_fork(a);
    KT_ASSERT(b != NULL);
    pte_t *pa_pte = vm_walk(a->pt, 0x100000, false);
    pte_t *pb_pte = vm_walk(b->pt, 0x100000, false);
    KT_ASSERT(pa_pte && pb_pte);
    KT_EXPECT_EQ(PTE_PA(*pa_pte), PTE_PA(*pb_pte));                 /* shared frame */
    KT_EXPECT((*pa_pte & PTE_COW) && !(*pa_pte & PTE_W));
    struct page *pg = pa_to_page(PTE_PA(*pa_pte));
    KT_EXPECT_EQ(pg->refcount, 2);

    /* Child writes: gets its own copy; the parent's data is untouched. */
    u32 w = 0x1234;
    KT_ASSERT(uvm_write(b, 0x100200, &w, sizeof(w)) == 0);
    KT_EXPECT(PTE_PA(*vm_walk(b->pt, 0x100000, false)) != PTE_PA(*pa_pte));
    KT_EXPECT_EQ(pg->refcount, 1);
    KT_EXPECT_EQ(b->cow_copies, 1);
    u32 r = 0;
    KT_EXPECT_EQ(uvm_read(a, 0x100200, &r, sizeof(r)), 0);
    KT_EXPECT_EQ(r, 0xabcd);

    /* Parent writes: last reference, so the page is just made writable. */
    KT_EXPECT_EQ(uvm_fault(a, 0x100200, FAULT_WRITE), 0);
    KT_EXPECT((*pa_pte & PTE_W) && !(*pa_pte & PTE_COW));
    KT_EXPECT_EQ(a->cow_copies, 0);

    mm_destroy(b);
    mm_destroy(a);
    KT_EXPECT_EQ(free_now(), before);                               /* nothing leaked */
}

KTEST(copy_user_rejects_bad_ranges)
{
    char buf[16];
    /* No process: user addresses are unmapped and must fail cleanly. */
    KT_EXPECT_EQ(copy_from_user(buf, 0x1000, 8), -EFAULT);
    KT_EXPECT_EQ(copy_to_user(0x1000, buf, 8), -EFAULT);
    /* Kernel addresses are rejected before any access. */
    KT_EXPECT_EQ(copy_from_user(buf, (uintptr_t)buf, 8), -EFAULT);
    KT_EXPECT_EQ(copy_from_user(buf, USER_TOP - 4, 8), -EFAULT);
    KT_EXPECT_EQ(strncpy_from_user(buf, 0x1000, sizeof(buf)), -EFAULT);
}

KTEST(elf_load_from_disk)
{
    size_t size;
    int err = 0;
    void *img = vfs_read_all("/bin/hello", &size, &err);
    KT_ASSERT(img != NULL);
    KT_EXPECT(vfs_read_all("/bin/nope", &size, &err) == NULL);
    KT_EXPECT_EQ(err, -ENOENT);

    struct mm *mm = mm_create();
    uintptr_t entry = 0, end = 0;
    char errmsg[96] = "";
    KT_ASSERT(elf_load(mm, img, size, &entry, &end, errmsg, sizeof(errmsg)) == 0);
    KT_EXPECT(entry >= USER_MIN_ADDR && entry < end);
    struct vma *text = vma_find(mm, entry);
    KT_EXPECT(text && (text->prot & VM_EXEC) && !(text->prot & VM_WRITE));

    uintptr_t off;
    KT_EXPECT_STREQ(elf_symbolize(img, size, entry, &off), "_start");
    KT_EXPECT_EQ(off, 0);
    mm_destroy(mm);

    /* Garbage is rejected with a reason, not a crash. */
    mm = mm_create();
    static const u8 junk[64] = { 0x7f, 'E', 'L', 'F', 1 };
    KT_EXPECT_EQ(elf_load(mm, junk, sizeof(junk), &entry, &end, errmsg, sizeof(errmsg)), -ENOEXEC);
    KT_EXPECT(strstr(errmsg, "64-bit") != NULL);
    mm_destroy(mm);
    kfree(img);
}
