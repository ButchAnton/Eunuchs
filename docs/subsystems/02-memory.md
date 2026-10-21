# Memory management

Eunuchs manages memory in five layers. A buddy allocator hands out physical page frames, each described by a `struct page`. A slab allocator built on it provides `kmalloc` and fixed-size object caches. An Sv39 page-table layer maps the kernel image (W^X), the direct map, MMIO and kernel stacks. On top of all that, per-process address spaces (`struct mm`) are built from VMAs with demand paging, copy-on-write `fork` and file mappings; a page cache holds file pages, a swap partition takes anonymous pages when memory is short, and kswapd reclaims in the background. Address spaces are tagged with ASIDs, so switching between them need not flush the TLB. Every layer is instrumented for debugging: memory is poisoned, slab objects have redzones, every allocation and free records its call site, and corruption is reported with backtraces and hexdumps.

## Responsibilities and scope

**This subsystem handles:**

- Discovering usable RAM at boot (from `struct platform_info`, which the device-tree parser fills) and reserving firmware, kernel image, DTB, DT `reserved-memory` and the `struct page` array itself (`kernel/mm/pmm.c: pmm_init`).
- Allocating and freeing physical pages in power-of-two blocks of order 0 to 10 (`alloc_pages`, `free_pages`, `page_alloc`, `page_free`), with reference counts for shared frames (`page_get`, `page_put`).
- Kernel heap: `kmalloc`, `kzalloc`, `kfree`, `ksize`, and named caches (`kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`).
- The kernel page table: building it in `vm_init`, `vm_map`/`vm_unmap`, `ioremap`, lookups (`vm_translate`, `vm_walk`), and lazily propagating new kernel top-level entries into process page tables.
- Guarded kernel thread stacks in a dedicated virtual area (`kstack_alloc`, `kstack_free`), including the TLB shootdown needed to unmap them.
- User address spaces: creation, VMA list (with reuse of freed ranges), changing protections (`vma_protect`, behind `mprotect`), demand-zero paging, COW fork, file mappings (`MAP_SHARED`, `MAP_PRIVATE`, `msync`), teardown with page-table pruning, cross-address-space reads and writes, activation on context switch with ASIDs.
- The page cache (`pagecache.c`): file pages and the swap cache in one hash table and LRU.
- Swap (`swap.c`) on the disk's `swap` partition, and reclaim (`reclaim.c`): free-memory marks, kswapd, direct reclaim, `user_page_alloc`.
- Safe kernel access to user memory (`copy_from_user`, `copy_to_user`, `strncpy_from_user`) and fault-tolerant debug accessors (`safe_read_*`, `safe_write_*`), both through the exception table.
- Corruption detection and reporting (`mm_corruption`, `heapcheck`, `slab leaks`, fault hints).

**It does not handle:**

- System-call argument parsing for `brk`, `mmap`, `munmap` and `mprotect`. Those live in `kernel/proc/syscall.c` (`do_mmap`, `do_munmap`, `do_mprotect` and the brk handler), which call `vma_add`/`vma_unmap`/`vma_protect`.
- ELF loading (`kernel/proc/elf.c: elf_load`, which uses `vma_add` and `uvm_populate`) and building the initial user stack (`kernel/proc/proc.c`, which uses `uvm_write`).
- Signal delivery and the crash report for a failed user fault (`user_fault` in `kernel/proc`).
- The IPI transport. TLB shootdown (`kernel/arch/riscv/smp.c: tlb_shootdown`) is documented here, but IPIs, `ipi_poll` and stop-the-world belong to the SMP subsystem.
- The filesystems' own reads and writes (`kernel/fs/`), which the page cache sits beside: efs file reads go through it, writes go to the filesystem and then update it.

**Boundaries:**

- The scheduler calls `vm_activate` on every context switch (`kernel/core/sched.c`), then clears `SSTATUS_SUM`.
- The trap handler (`kernel/arch/riscv/trap.c: trap_handler`, `user_exception`) calls `uvm_fault` and `vm_kernel_fault_fixup`.
- `panic_trap` uses `vm_fault_hint` to explain kernel faults.

## Source map

| File | Role |
|---|---|
| `kernel/include/mm.h` | Public API: layout constants, `struct page`, page flags, PTE bits, pmm/vmm/kstack/slab prototypes, poison values |
| `kernel/mm/buddy.h`, `kernel/mm/buddy.c` | Buddy allocator core: pure bookkeeping over a `struct page` array, no locking and no access to frame contents. Shared with the host test |
| `kernel/mm/pmm.c` | Physical memory manager: boot reservation, the global zone and its lock, page poisoning, alloc/free sites, refcounts, statistics, `mm_corruption` reporting, `meminfo`/`page` output |
| `kernel/mm/slab.c` | Slab caches and `kmalloc` with redzones, poisoning, per-object metadata, leak report, heap verification |
| `kernel/mm/vmm.c` | Sv39 page tables: `vm_init`, `walk`, `vm_map`/`vm_unmap`, `ioremap`, `vm_kernel_fault_fixup`, page-table dumps, `vm_fault_hint` |
| `kernel/mm/kstack.c` | Kernel stacks in `KSTACK_AREA` with guards, a cache of freed but still-mapped stacks, `addr_in_stack_guard` |
| `kernel/mm/uvm.c` | User address spaces: `mm_create`/`mm_destroy`/`mm_fork`, VMAs, `uvm_fault`, `uvm_populate`, `uvm_read`/`uvm_write`, `vm_activate`, `mm_print` |
| `kernel/mm/pagecache.c` | The page cache and swap cache: `pc_get`, `pc_find`, `pc_insert`, `pc_update`, `pc_truncate`, `pc_drop_inode`, `pc_writeback`, `pc_shrink`, `/proc/pagecache` |
| `kernel/mm/swap.c`, `kernel/include/swap_format.h` | Swap space: the partition, slot counts, `swap_read`/`swap_write`, `swap_dup`, `/proc/swaps` |
| `kernel/mm/reclaim.c` | Free-memory marks, kswapd, `reclaim`, `user_page_alloc`, the `vmc` counters (`/proc/vmstat`) |
| `kernel/include/proc.h` | `struct mm`, `struct vma`, user layout constants, `PTE_COW`, `VM_*`, `FAULT_*`, uaccess prototypes, `user_range_ok` |
| `kernel/arch/riscv/uaccess.S` | `__copy_user`, `__strncpy_user`: byte loops with `SUM` set and `__ex_table` entries |
| `kernel/proc/uaccess.c` | `copy_from_user`, `copy_to_user`, `strncpy_from_user`: range checks around the assembly |
| `kernel/arch/riscv/safe.S`, `kernel/include/safe.h` | `safe_read_u{8,16,32,64}`, `safe_write_u{8,16,32,64}`, `struct extable_entry`, `extable_lookup` |
| `kernel/arch/riscv/trap.c` | User page faults (`user_exception`), kernel fault order: kernel root-entry fixup, uaccess demand paging, extable fixup, panic (`trap_handler`), `extable_lookup` |
| `kernel/arch/riscv/smp.c` | `tlb_shootdown`, `tlb_flush_ack` |
| `kernel/arch/riscv/kernel.ld` | Page-aligned sections, `__ex_table`, the `.stacks` section (boot and emergency stacks in 32 KiB slots) |
| `tests/host/test_buddy.c` | Host test of `buddy.c` under ASan/UBSan |
| `kernel/tests/test_mm.c` | In-kernel tests: pmm, kmalloc error detection, leak report, kernel W^X, map/unmap, fault hints |
| `kernel/tests/test_proc.c` | `vma_add_find_overlap_unmap`, `demand_paging_and_cow`, `copy_user_rejects_bad_ranges` |
| `kernel/tests/test_smp.c` | `tlb_shootdown_invalidates_every_cpu` |

## Concepts and data structures

### Virtual address layout (Sv39)

| Range | Contents | Defined in |
|---|---|---|
| `0x0` – `0xffff` | Unmapped. `USER_MIN_ADDR` is `0x10000`; `vma_add` refuses anything below `PAGE_SIZE` | `proc.h` |
| `0x10000` – … | ELF `PT_LOAD` segments, then the heap (`brk_start` = page-aligned end of the image) | `proc.c`, `elf.c` |
| `0x2000000000` (`USER_MMAP_BASE`) | `mmap` area (anonymous and file mappings): the lowest free range with a guard page on each side (`uvm_find_gap`), so freed ranges are reused | `syscall.c: do_mmap` |
| `0x3ffff00000 - 8 MiB` – `0x3ffff00000` (`USER_STACK_TOP`) | User stack VMA, `USER_STACK_SIZE` = 8 MiB, demand-paged | `proc.h` |
| `0x4000000000` (`USER_TOP`) | End of user space (the lower half of Sv39) | `proc.h` |
| `0xffffffc000000000` (`KERNEL_BASE`) + PA | Direct map of RAM (from `__kernel_end`) and of `ioremap`ped MMIO | `mm.h`, `vmm.c: vm_init` |
| `0xffffffc080200000` | Kernel image inside the direct map, per-section permissions | `kernel.ld` |
| `0xffffffe800000000` (`KSTACK_AREA`) | `KSTACK_MAX` = 512 slots of `KSTACK_SLOT` = 32 KiB. Each has an unmapped 16 KiB guard below 16 KiB (`KSTACK_SIZE`) of stack | `mm.h`, `kstack.c` |

The conversions `__va(pa)` and `__pa(va)` simply add or subtract `KERNEL_BASE`. They are only valid for direct-map addresses. `__pa` of a `KSTACK_AREA` address gives nonsense, and `pa_to_page` returns NULL for it.

Physical memory on QEMU `virt` starts at `0x80000000`. OpenSBI occupies `0x80000000` up to `__kernel_start` (`0x80200000`). That range is reserved and *not* in the direct map, so touching it faults. `vm_fault_hint` reports such a fault as "access to firmware (OpenSBI) memory".

### `struct page` (`kernel/include/mm.h`)

There is one `struct page` per physical frame of RAM. They are stored in the `mem_map` array, which `pmm_init` places at the first page-aligned spot after `__kernel_end` that overlaps no reserved region.

| Field | Meaning |
|---|---|
| `list` | Link in a buddy free list (free block heads) or in a slab cache's `partial`/`full`/`empty` list |
| `flags` | `PG_FREE`, `PG_RESERVED`, `PG_SLAB`, `PG_LARGE`, `PG_PGTABLE`, `PG_TAIL` (see below). `0` means "allocated head, or interior of a free block" |
| `order` | Block order (meaningful on heads only) |
| `refcount` | Set to 1 by `alloc_pages_site`, 0 on free. `page_get`/`page_put` adjust it atomically. User pages shared by COW fork have refcount > 1 |
| `head` | For `PG_TAIL` pages: the head page of the block. `compound_head()` follows it |
| `alloc_site`, `free_site` | Return address of the last allocator and last freer (printed with `ksym_format`) |
| `slab.{cache, free_head, free_tail, inuse}` | For `PG_SLAB` heads: owning cache, FIFO free list of slots, objects in use |
| `large_size` | For `PG_LARGE` heads: the requested `kmalloc` size |

Page flags:

| Flag | Set by | Meaning |
|---|---|---|
| `PG_FREE` | `buddy.c: add_free_block` | Head of a block on a free list |
| `PG_RESERVED` | `buddy_init` (all pages), cleared by `buddy_release_range` | Firmware, kernel image, DTB, DT reserved-memory, `mem_map`. Never allocatable |
| `PG_SLAB` | `slab.c: slab_page_set` | Head of a slab |
| `PG_LARGE` | `slab.c: kmalloc_site` | Head of a >2 KiB kmalloc |
| `PG_PGTABLE` | `vmm.c: alloc_table`, `uvm.c: mm_create` | Page-table page. `vm_activate` checks it on the root |
| `PG_TAIL` | `buddy_alloc` | Non-head page of an *allocated* multi-page block |
| `PG_CACHE` | `pagecache.c: pc_insert` | In the page cache: a file's page (`cache.owner` = the super_block, `cache.ino`, `cache.index`) |
| `PG_SWAPCACHE` | `pc_insert` with `&swap_owner` | In the swap cache: an anonymous page with a swap slot (`cache.index`) |
| `PG_DIRTY` | write faults on shared file pages, reclaim | Newer than its copy in the file or the swap slot |
| `PG_REFERENCED` | readers, reclaim | Used since reclaim last looked (second chance on the LRU) |
| `PG_USER` | `user_page_alloc` | User memory, anonymous or cache (statistics, `page`) |

User-memory flags change under several locks, so they are set and cleared atomically (`page_set_flag`, `page_clear_flag`). A cache page uses the `cache` member of the page's union (`owner`, `ino`, `wmap`: writable shared mappings of it, `index`, `hnext`: hash chain) and its `list` links it into the reclaim LRU.

Invariants:

- On an allocated block, the head has `flags & PG_TAIL == 0`, and every other page has `PG_TAIL` with `head` pointing to the head.
- On a free block, only the head carries `PG_FREE` and a valid `order`. Interior pages of a free block have `flags == 0`. This is why `page <pa>` prints "allocated" for a page in the middle of a free block, and why a double free is not always detected (see Limitations).
- `refcount` is only meaningful on heads.

### The buddy zone (`kernel/mm/buddy.h`)

```c
struct buddy_zone {
    struct page *pages;   u64 base_pfn;   u64 npages;
    struct list_head free[MAX_ORDER + 1];  u64 nr_free[MAX_ORDER + 1];
    u64 free_pages;
};
```

- There is one zone (`static struct buddy_zone zone` in `pmm.c`) covering all RAM, `[platform.mem_base, mem_base + mem_size)`.
- `MAX_ORDER` is 10, so the largest block is 2^10 pages = 4 MiB.
- Free lists are FIFO. `add_free_block` uses `list_add_tail` and `buddy_alloc` takes `list_first_entry`, so a freed block is reused as late as possible. This gives poison checks the longest window to catch use-after-free.
- Buddies are found by `pfn ^ (1 << order)`, using absolute PFNs. `free_block` merges only if the buddy is `PG_FREE`, has the same order, and lies entirely inside the zone.

### Slab caches (`kernel/mm/slab.c`)

```c
struct kmem_cache {
    char name[24];
    size_t obj_size;      /* size class, 8-byte aligned */
    size_t slot_size;     /* REDZONE + obj_size + REDZONE + sizeof(struct obj_meta) */
    unsigned order;       /* pages per slab = 2^order, at most MAX_SLAB_ORDER (3) */
    u32 objs_per_slab;
    struct list_head partial, full, empty;
    u64 nr_slabs, active, peak, allocs, frees;
    spinlock_t lock;
};
```

Caches are statically allocated (`caches[MAX_CACHES]`, `MAX_CACHES` = 32) and never destroyed. `kmem_cache_create` picks the smallest order (up to 3) that holds at least 8 slots.

Each object slot is laid out like this:

```
 slot ─┬─ left redzone (8 bytes, 0xbb)
       ├─ object, obj_size bytes  ◄── pointer returned to caller
       │    [0, req_size)      : caller's data (0x5a on alloc; 0x6b while free)
       │    [req_size, obj_size): slack, filled with 0xbb (part of right redzone)
       ├─ right redzone (8 bytes, 0xbb)
       └─ struct obj_meta (32 bytes): next_free, alloc_site, free_site, state, req_size
```

- `obj_meta.state` is `OBJ_FREE` (`0xf4ee0bec`) or `OBJ_ALLOC` (`0xa1100c8d`). Any other value means the metadata was overwritten.
- The per-slab free list threads through `obj_meta.next_free` and is FIFO (`free_head` → `free_tail`).

The kmalloc size classes and the geometry they produce (computed from `kmem_cache_create` with the 48 bytes of per-slot overhead):

| Cache | obj_size | slot | slab order | objects/slab | unused bytes/slab |
|---|---:|---:|---:|---:|---:|
| kmalloc-8 | 8 | 56 | 0 | 73 | 8 |
| kmalloc-16 | 16 | 64 | 0 | 64 | 0 |
| kmalloc-32 | 32 | 80 | 0 | 51 | 16 |
| kmalloc-64 | 64 | 112 | 0 | 36 | 64 |
| kmalloc-96 | 96 | 144 | 0 | 28 | 64 |
| kmalloc-128 | 128 | 176 | 0 | 23 | 48 |
| kmalloc-192 | 192 | 240 | 0 | 17 | 16 |
| kmalloc-256 | 256 | 304 | 0 | 13 | 144 |
| kmalloc-512 | 512 | 560 | 1 | 14 | 352 |
| kmalloc-1024 | 1024 | 1072 | 2 | 15 | 304 |
| kmalloc-2048 | 2048 | 2096 | 3 | 15 | 1328 |

Requests larger than `KMALLOC_MAX` (2048) take whole pages from `alloc_pages_site` with order `size_to_order(size)`. The head is marked `PG_LARGE`, and the slack from `size` to the end of the block is redzoned with `0xbb`.

The only custom cache created in the tree is in the tests (`test-widget` in `kernel/tests/test_mm.c`). All other kernel code uses `kmalloc`.

### Poison values (`kernel/include/mm.h`)

| Value | Name | Where |
|---|---|---|
| `0x6b` | `POISON_FREE` | Free pages (unless `nopoison`) and free slab objects |
| `0x5a` | `POISON_UNINIT` | New `kmalloc`/`kmem_cache_alloc` objects (not `kzalloc`) |
| `0xbb` | `POISON_REDZONE` | Slab redzones and slack, large-kmalloc tail slack |
| `0x57` | `STACK_POISON` | Unused kernel stack bytes (high-water marks in `ps`) |

`vm_fault_hint` recognizes a fault address whose bits 16 to 63 are all one of the first three bytes, and names the likely bug.

### Page-table entries (`kernel/include/mm.h`, `proc.h`)

The PTE bits used are `PTE_V R W X U G A D` (bits 0 to 7), plus `PTE_COW` (bit 8, one of the RSW bits) for user COW pages. A **swap PTE** has `V` clear, `PTE_SWAP` (bit 9) set, and the swap slot in bits 10 and up (`PTE_IS_SWAP`).

Kernel permission sets preset `A` and `D`, so the hardware never needs to update them:

| Set | Bits |
|---|---|
| `PROT_KERNEL_RX` | R X G A |
| `PROT_KERNEL_RO` | R G A |
| `PROT_KERNEL_RW` | R W G A D |

User PTEs come from `uvm.c: prot_to_pte`: `U|A|D`, plus R/W/X from the VMA's `VM_READ/VM_WRITE/VM_EXEC`. Reclaim clears `A` to see which pages are used; if the hardware does not set it again (no Svadu, as on QEMU), the next access faults and `uvm_fault` sets it. `D` is always set: dirtiness of shared file pages is tracked by write faults instead. User mappings are always 4 KiB leaves. The kernel direct map uses the largest level (1 GiB, 2 MiB or 4 KiB) that alignment and size allow (`vmm.c: do_map`).

### Kernel page table and the kernel half of process tables

`kernel_pagetable` is the master copy. Every `struct mm` has its own root, whose entries 256 to 511 (the upper half) are copies of the kernel root's entries (`vm_sync_kernel`). Intermediate kernel tables are shared, so only *new top-level entries* need propagating.

This is tracked with a generation counter:

- `kernel_root_gen` is bumped whenever `walk` creates a new level-2 table for a kernel address after `vm_ready`.
- `mm->kernel_gen` records the generation the mm last synced at.
- `vm_activate` re-syncs if the two differ.
- `vm_kernel_fault_fixup` repairs a table that is already active.

Kernel intermediate tables are never freed, so the kernel half only ever grows.

### Kernel stacks (`kernel/mm/kstack.c`)

```
KSTACK_AREA + idx*32K:  [ guard 16 KiB, unmapped ][ stack 16 KiB, mapped RW ]
                                                   ^ kstack_alloc() returns this (lowest usable address)
```

- Every valid stack pointer has bit 14 (`KSTACK_GUARD_BIT`) set. `trapentry.S` tests this bit to detect overflow and switch to the emergency stack.
- The boot and emergency stacks in the image's `.stacks` section use the same 32 KiB-slot layout. `vm_init` maps only their upper halves.
- State:
  - `slot_used` is a 512-bit bitmap.
  - `cache[KSTACK_CACHE]` (32 entries) holds freed stacks that stay mapped.
  - Counters `cache_hits` and `unmapped`.

### User address spaces (`kernel/include/proc.h`)

```c
struct vma { struct list_head node; uintptr_t start, end; u32 prot; enum vma_kind kind; char name[16];
             struct file *file; u64 pgoff; bool shared; };            /* VMA_FILE */
struct mm  { pagetable_t pt; struct mutex lock; spinlock_t vma_lock; struct list_head vmas;
             uintptr_t brk_start, brk; u64 rss, swapped, faults, cow_copies, major_faults;
             u32 kernel_gen; u64 id, tlb_gen; uintptr_t scan_va; };
```

- `vmas` is a doubly linked list sorted by `start`, with non-overlapping `[start, end)` page-aligned ranges.
- `kind` is `VMA_ELF`, `VMA_HEAP`, `VMA_STACK`, `VMA_ANON` or `VMA_FILE`. A file VMA holds a reference to the open file, the file page at `start` (`pgoff`), and whether writes go to the file (`shared`).
- `prot` is a mask of `VM_READ`, `VM_WRITE` and `VM_EXEC`.
- `lock` (a mutex) is held while the page table or the VMA list changes: by the owner (faults, mmap, munmap, brk, mprotect, fork, exit), by a debugger (ptrace), and by reclaim (`mutex_trylock`). `vma_lock` additionally serializes VMA list changes against readers that cannot sleep (`/proc/<pid>/maps` under `proc_lock`, the monitor).
- `swapped` counts the mm's pages out in swap; `major_faults` the faults that read a file or swap; `id` (unique for the life of the system) and `tlb_gen` (bumped by every PTE change other CPUs may cache) drive the ASIDs; `scan_va` is where reclaim continues next time.
- `rss` counts mapped pages. A shared COW page counts once in each mm.

## How it works

### Boot sequence

`kmain` calls `pmm_init`, then `vm_init`, then `slab_init` (see `docs/ARCHITECTURE.md`). Before `vm_init`, the kernel runs on `boot.S`'s early table: identity gigapage plus 64 GiB of RWX direct map.

**`pmm_init` (`pmm.c`):**

1. Checks the `nopoison` boot option (`cmdline_has`).
2. Records reserved regions with `reserve()`, which rounds them to page boundaries. There are at most `MAX_RESERVED` = 16, and overflowing panics. The regions are:
   - `[ram_start, __kernel_start)` "firmware (OpenSBI)"
   - the kernel image
   - the DTB
   - each DT reserved-memory range
3. Sizes `mem_map` as `npages * sizeof(struct page)` and places it at the first page after `__kernel_end` that overlaps no reserved region. It panics if that does not fit. The array is reserved as "mem_map (struct page array)". With the current struct layout it is about 88 bytes per page, roughly 2.75 MiB for 128 MiB of RAM.
4. Sorts the reserved regions (insertion sort).
5. Calls `buddy_init`, which marks every page `PG_RESERVED`.
6. Walks the gaps between reserved regions. For each gap it fills the memory with `0x6b` (if poisoning is on) and calls `buddy_release_range`. That function clears the flags and frees the range in the largest naturally aligned blocks, merging with neighbours through `free_block`.
7. Sets `pmm_ready` and logs each reserved region and the totals.

**`vm_init` (`vmm.c`)** builds `kernel_pagetable` with `map_or_die`:

| Range | Permissions |
|---|---|
| `__kernel_start`..`__text_end` | `PROT_KERNEL_RX` |
| `__rodata_start`..`__rodata_end` (includes `.ktests` and `__ex_table`) | `PROT_KERNEL_RO` |
| `__data_start`..`__data_end` (data and bss) | `PROT_KERNEL_RW` |
| Upper half of each 32 KiB slot in `.stacks` | `PROT_KERNEL_RW` |
| `__ksyms_start`..`__kernel_end` | `PROT_KERNEL_RO` |
| `__kernel_end`..end of RAM (direct map) | `PROT_KERNEL_RW` |
| Every `ioremap` region recorded so far | `PROT_KERNEL_RW` |

Then it writes `satp` (Sv39 mode, `SATP_MODE_SV39` = `8UL << 60`), flushes, and sets `vm_ready`.

Anything not listed stays unmapped: NULL, the user half, OpenSBI memory, alignment gaps between sections, and stack guards. Secondary harts switch to `kernel_pagetable` in `secondary_entry`.

**`slab_init` (`slab.c`)** creates the 11 `kmalloc-N` caches and sets `slab_ready`. Calling `kmalloc` earlier panics ("kmalloc before slab_init"), and so does `alloc_pages` before `pmm_init`.

### Page allocation and free (`pmm.c`)

**`alloc_pages_site(order, site)`:**

1. Under `zone_lock` (irqsave), calls `buddy_alloc`. That takes the first block from the smallest non-empty order ≥ `order` and splits it, putting the upper halves back on lower free lists. It marks the head (`flags = 0`, `order`) and the tails (`PG_TAIL`, `head`). It also updates the `stat_allocs`/`stat_failures` counters.
2. On failure it logs `out of memory: order-N allocation from <site> failed (M pages free)` and returns NULL.
3. Outside the lock, if poisoning is on, `check_poison` verifies the whole block is still `0x6b`. If it is not, it reports `page use-after-free: block pa … modified after free; bytes +a..+b (first: x); last freed at <site>`.
4. Sets `refcount = 1` and `alloc_site`. Emits a `pr_trace` line if the `mm` log level is `trace`.

The wrappers are:

- `alloc_pages(order)`, which records the caller.
- `page_alloc()`, an order-0 page returned as a kernel VA. Its contents are poisoned (`0x6b`), *not* zeroed.
- `page_alloc_zeroed()`.

**`free_pages_site(pg, order, site)`** validates before touching anything. Each failure goes to `mm_corruption` and the free is aborted:

- `PG_FREE` → "double free of page …", with the previous free and alloc sites.
- `PG_RESERVED` or `PG_TAIL` → "free_pages of pa … which is reserved memory / not the head of its block".
- `refcount > 1` → "… still has N references".
- `pg->order != order` → "free_pages order mismatch".

If the checks pass, it re-poisons the block, records `free_site`, sets `refcount = 0`, then calls `buddy_free` under `zone_lock`. `buddy_free` re-checks the flags and alignment, clears the tail flags, and merges with free buddies.

**`page_get`/`page_put`** use atomics. `page_put` frees the block (with its own `order`) when the count reaches 0, and reports a negative count as corruption.

### Corruption reporting (`pmm.c: vcorruption`)

All detectors call `mm_corruption` or `mm_corruption_dump`. These:

1. Format the message.
2. Increment `mm_corruptions`.
3. Log `*** MEMORY CORRUPTION: <msg>` at `LOG_ERR`.
4. Optionally hexdump with `hexdump_safe`.
5. Print `detected at:` with a backtrace.
6. Panic with "memory corruption detected (details above)" unless `mm_panic_on_corruption` is false. Tests and `heapcheck` set it to false temporarily.

When the panic is suppressed, the detecting function returns without performing the bad operation. For example, a double `kfree` is not executed.

### kmalloc and slab (`slab.c`)

**Allocation, `kmalloc_site(size, site)`:**

1. `size_to_cache` does a linear scan for the first class ≥ `size`. A zero size goes to `kmalloc-8`.
2. Small sizes go to `kmem_cache_alloc_site`:
   1. `take_slot`, under `c->lock`, picks a slab: the first `partial` slab, else the `empty` slab (moved to `partial`), else `new_slab`. `new_slab` allocates `2^order` pages and formats every slot (redzones `0xbb`, object `0x6b`, meta `OBJ_FREE`) into a FIFO free list. It then pops the head slot, moves the slab to `full` if it is now full, and updates `allocs`, `active` and `peak`.
   2. Outside the lock, it verifies the slot. The state must be `OBJ_FREE`, otherwise it reports "freelist corrupted". The object must still be all `0x6b`, otherwise it reports "use-after-free write to <cache> object …". `check_redzones(..., "allocation")` also runs.
   3. It sets `OBJ_ALLOC`, `alloc_site` and `req_size = obj_size`, and fills the object with `0x5a`.
3. `kmalloc_site` then tightens the redzone: it sets `req_size = size` and fills `[size, obj_size)` with `0xbb`. Overflowing into the size-class slack is therefore detected too.
4. Large sizes: `size_to_order`. Orders above `MAX_ORDER` are logged as "kmalloc(N) from <site>: too large" and return NULL. Otherwise it calls `alloc_pages_site`, sets `PG_LARGE` and `large_size`, fills `[0,size)` with `0x5a` and the rest of the block with `0xbb`, and updates the `large_*` counters.

`kzalloc_site` is `kmalloc_site` followed by `memset(p, 0, size)`.

**Freeing, `kfree(p)`:**

1. `find_slot` maps the pointer to its head page (`va_to_page` + `compound_head`).
   - A pointer outside managed RAM is reported as "not a heap pointer (outside managed RAM)".
   - For a `PG_SLAB` page, it computes the slot index and requires `p` to be exactly the object start. Otherwise it reports "pointer is N bytes into <cache> object …".
2. For slab objects, `obj_free` does the following:
   1. Rejects `OBJ_FREE` (reported as "double free of … already freed at A (allocated at B), freed again at C") and any other non-`OBJ_ALLOC` state (reported as "metadata corrupted").
   2. Runs `check_redzones(..., "kfree")`. An underflow into the left redzone is reported with a negative offset. An overflow past `req_size` is reported with the exact byte range.
   3. Restores all guard bytes, fills the object with `0x6b`, and records `OBJ_FREE` and `free_site`.
   4. Under `c->lock`, appends the slot to the slab's free list tail. A full slab moves to the *tail* of `partial`. A slab that becomes empty is kept as the cache's single `empty` slab, or released if one is already cached. Release (clear `PG_SLAB`, `free_pages`) happens after dropping the lock.
3. Non-slab pointers:
   - If not `PG_LARGE`, the pointer is classified for the report: reserved memory (with the region name from `pmm_region_name`), a free page ("double free of a large kmalloc?"), or "not allocated by kmalloc".
   - For `PG_LARGE`: an interior pointer is rejected. Otherwise it checks the slack for `0xbb` ("heap overflow in large kmalloc …"), clears `PG_LARGE`, and calls `free_pages`.

`kmem_cache_free(c, obj)` additionally checks that the object belongs to `c`. `ksize(p)` returns `req_size` for slab objects and `large_size` for large ones.

### Kernel page tables (`vmm.c`)

**`walk(pt, va, alloc, target_level, &level)`** descends from level 2.

- It stops early at a leaf (a superpage), reporting the level.
- With `alloc`, it creates missing tables with `alloc_table`: a zeroed page marked `PG_PGTABLE`.
- If the new table is a level-2 entry for a kernel address in `kernel_pagetable` after `vm_ready`, it bumps `kernel_root_gen` and copies the entry into the currently active root, if that is a process table.

**`vm_map(pt, va, pa, size, perm)`:**

- Takes `kpt_lock` for `kernel_pagetable` once `vm_ready` is set. User tables are not locked.
- `do_map` requires page alignment and a canonical VA. For each chunk it picks the largest level whose size is aligned and fits, and refuses to overwrite a valid PTE ("vm_map: va … already mapped") or to map inside an existing superpage.
- It finishes with a local `sfence.vma`. On failure, chunks already mapped stay mapped.

**`vm_unmap(pt, va, size)`:**

- `do_unmap` clears leaves and runs a local `sfence.vma va` for each one.
- It refuses unmapped addresses and partial unmaps of a superpage.
- For kernel mappings the caller must then call `tlb_shootdown()` before reusing the memory. Intermediate tables are never freed.

**`vm_translate`** returns the physical address and leaf PTE, handling superpages. **`vm_walk`** returns only level-0 PTEs (4 KiB leaves) and is used for user tables.

**`ioremap(pa, size)`** records the page-rounded region in `ioremaps[MAX_IOREMAP]` (32 entries; overflow panics) and returns `__va(pa)`.

- A repeat call with the same start and a size no larger than before reuses the existing entry.
- Before `vm_init`, the region is only recorded, and `vm_init` maps it. Afterwards it is mapped immediately with `PROT_KERNEL_RW`.

**`vm_kernel_fault_fixup(va)`** handles kernel page faults on kernel addresses while a process table is active. If the kernel root has a valid entry for `va` that differs from the active root's entry, it copies the entry, flushes, and returns true so the faulting instruction is retried.

### Kernel stacks and TLB shootdown

**`kstack_alloc`:**

1. Pops a cached stack if there is one. It re-poisons it with `0x57` and counts a cache hit.
2. Otherwise it claims a free slot in `slot_used` under `kstack_lock`. If all 512 slots are in use, it logs "kstack_alloc: all 512 stack slots in use" and returns NULL.
3. It then maps four `page_alloc` pages one by one with `vm_map(kernel_pagetable, …)`. On failure it unwinds through `unmap_pages` and `release_slot`.
4. It fills the stack with `0x57`.

**`kstack_free`:**

1. Validates the address, panicking with "kstack_free: %p is not a kernel stack" if it is not one.
2. If fewer than 32 stacks are cached, it caches this one, still mapped.
3. Otherwise `unmap_pages` translates each page, `vm_unmap`s it, calls `tlb_shootdown()` once for the whole stack, and only then calls `page_free` on the frames. Finally it releases the slot.

**`tlb_shootdown` (`smp.c`):**

1. Disables interrupts and flushes locally.
2. With more than one CPU, atomically increments `tlb_gen` to `g`, sets its own `tlb_done_gen = g`, and sends `IPI_TLB_FLUSH` to every CPU whose `tlb_done_gen < g`.
3. Spins until each CPU's `tlb_done_gen ≥ g`. While spinning it calls `ipi_poll()`, so two simultaneous shootdowns cannot deadlock.
4. Panics after `SHOOTDOWN_TIMEOUT_MS` (3000 ms) with "TLB shootdown: cpuN did not respond within 3000 ms (running thread …; interrupts off for too long?)".

Targets run `tlb_flush_ack`, from the IPI handler, from `ipi_poll` in spin loops, or from `cpu_park`. It reads `tlb_gen` *before* `sfence.vma zero, zero` and then publishes that generation, so the flush is guaranteed to cover every change made before it.

Only kernel mappings need shootdowns. A user page table is live on at most one CPU, because each process has one thread, and every `satp` change in `vm_activate` flushes the whole TLB.

### User address spaces (`uvm.c`)

**Creation:** `mm_create` allocates the `mm` (`kzalloc`) and a zeroed root (`PG_PGTABLE`). It copies the kernel half (`vm_sync_kernel`), records `kernel_gen`, and takes a new `id`.

**`exec`** (`proc.c`) builds a complete new mm before giving up the old one:

1. `elf_load` creates `VMA_ELF` areas and eagerly fills them with `uvm_populate` plus `memcpy`.
2. The 8 MiB `VMA_STACK` VMA is added.
3. The argument strings and vectors are written with `uvm_write`.
4. `vm_activate(new)` runs, then `mm_destroy(old)`.

**VMAs:**

- `vma_add` validates its arguments (`-EINVAL`) and rejects overlaps (`-EEXIST`). It allocates the VMA with `kzalloc` and inserts it in sorted position under `vma_lock`.
- `vma_find` is a linear search.
- `vma_unmap(start, end)` (`vma_unmap_locked` with `mm->lock` held) handles each overlapping VMA in turn: it unmaps the pages, then removes the VMA, trims it, or splits it. It is failure-atomic: the VMA a split needs is allocated before anything changes, so `-ENOMEM` leaves the address space as it was. A file VMA's last piece drops its file reference.
- `uvm_find_gap(mm, len)` returns the lowest free range of `len` bytes from `USER_MMAP_BASE` up, with a guard page on each side, below the stack's gap; `mmap` without `MAP_FIXED` uses it (after trying the address hint), so ranges freed by `munmap` are reused. (`mmap_next`, the old bump pointer, is gone.)
- `vma_protect(mm, start, end, prot)` (behind `mprotect`) changes the protection of a page-aligned range:
  1. The whole range must be covered by VMAs without holes (`-ENOMEM` otherwise), as on Linux.
  2. Each overlapping VMA whose protection differs is split at `start` and `end` (the outside parts are re-added with `vma_add` and keep the old protection), and the inside part gets the new `prot`.
  3. Present PTEs are rewritten: `R` and `X` follow `prot`. Write permission is **never** put directly into a PTE that lacked it: such a page becomes `PTE_COW`, and the first write fault takes it (refcount 1) or copies it (shared). This is needed because `mm_fork` shares read-only pages without marking them COW, so making one writable in place would let the process write into its parent's or child's page. A page that was writable and stays writable keeps `W`; a page that loses write permission keeps a `COW` mark that the VMA check in `uvm_fault` makes harmless.
  4. `sfence.vma` if the mm is active. If the new protection includes execute, a local `fence.i` plus `sbi_remote_fence_i` to every hart, so code written through the data cache is what the instruction fetch sees on whichever hart the process runs next.
  The `mprotect` system call (226, `do_mprotect`) checks alignment and the range, refuses `PROT_WRITE|PROT_EXEC` with `EACCES` (W^X, as for `mmap`), and rounds the length up to whole pages. `tcc -run` uses it to make freshly generated code executable ([13](13-toolchain.md)).
- `uvm_peek(mm, addr, dst, n)` and `uvm_poke(mm, addr, src, n)` are a debugger's access to a stopped process (`ptrace`). Both go through `debug_page`, which faults the page in if a VMA covers it (`uvm_populate`, whatever the VMA's protection), so a debugger can read pages the program has not touched yet. `uvm_poke` ignores the VMA's protection (breakpoints go into r-x text), but first gives the process a private copy of a page whose frame is shared (refcount > 1: text pages are shared after `fork` without COW marks), so a breakpoint never appears in another process; a writable COW page that gets copied this way becomes writable. If any page written is executable, it runs `fence.i` locally and on every hart through SBI. Both return `-EFAULT` for an address with no VMA.
- `brk` and `mprotect`: if the top of the heap was given another protection (tcc's `-run` memory comes from `malloc`, so from the heap), `do_brk` starts a new `heap` VMA at the old top instead of growing the changed one.

**Page-table walks:**

- `next_used` finds the next non-zero level-0 PTE (present, or a swap PTE). It skips whole 1 GiB or 2 MiB ranges whose level-2 or level-1 entry is invalid, which makes tearing down the mostly empty 8 MiB stack cheap.
- `for_each_used` wraps it.
- `unmap_range` clears each PTE and drops what it held (`drop_pte`): a page reference (and, for a writable shared file page, its `cache.wmap` count), or a swap slot (`swap_free`, `swapped--`). Each present PTE gets `tlb_changed` (a local flush if the mm is active, and `tlb_gen++`) before its page can be freed. Then `prune_tables` frees page-table pages left empty, clearing the parent entry and flushing before each table is freed.
- Every PTE change goes through `tlb_changed`, so other CPUs flush this mm's ASID when they next run it.

**Faults (`uvm_fault(mm, addr, access)`).** `uvm_fault` takes `mm->lock` and calls `fault_relock`, which loops on `fault_locked`. The rule is that **a fault never allocates memory or reads swap while holding `mm->lock`** (allocation may reclaim, and reclaim takes other processes' `mm->lock`): when `fault_locked` needs a page it returns `-EAGAIN`, and `fault_relock` drops the lock, allocates a spare page (`user_page_alloc`) or reads the swap slot into the swap cache (`read_swap_slot`), takes the lock again and retries, since anything may have changed meanwhile (`struct fault_ctx` carries the spare and the held page; more than 100 rounds gives `-ENOMEM`). `fault_locked`:

1. No VMA → `-EFAULT`. Access not allowed by `v->prot` → `-EACCES` (ptrace's `force` skips this).
2. `vm_walk(..., alloc=true)` gets the level-0 PTE, creating tables as needed (`-ENOMEM` on failure).
3. **Empty PTE**: for a file VMA, `file_fault` (below); otherwise a zeroed spare page mapped with the VMA's protections, `rss++`.
4. **Swap PTE**: `swap_in` takes the page from the swap cache (the slot was read into it unlocked). If nothing else needs the slot, the page leaves the swap cache and becomes this process's private page (the slot is freed); otherwise (the slot is shared after `fork`, or the page is in the cache for others) it is mapped read-only + `COW`. An I/O error gives `-EIO` (SIGBUS).
5. **Present PTE**: sets `A` if reclaim cleared it. A write to a non-writable page: a shared file page (`VMA_FILE`, `shared`) gets `PG_DIRTY`, `cache.wmap++` and `W` (the first write to it); a `COW` page goes to `cow_break`, which takes the page if this mm is its only user (sets `W`, clears `COW`) or copies it into the spare page (`cow_copies++`); anything else is `-EACCES`.
6. `faults++` (and `vmc.faults`); `sfence.vma va` if the mm is active.

**File faults (`file_fault`).** The page is `pgoff + (va - start) / PAGE_SIZE` of the VMA's file. A page wholly past the end of the file gives `-ENXIO`, which `user_exception` turns into SIGBUS "past the end of the mapped file". Otherwise the page comes from the page cache (`pc_get_spare`, under the file's inode lock, with the spare page supplied for a miss; a miss counts as a major fault). `MAP_SHARED` maps the cache page itself, read-only until the first write (step 5); `MAP_PRIVATE` maps it read-only + `COW`, so a write copies it.

**`mm_fork(parent)`:**

1. Creates a new mm and copies `brk_start` and `brk`, under the parent's `mm->lock`.
2. For each parent VMA it adds a matching VMA (a file VMA takes another reference to the file). For each used PTE: a swap PTE is copied and its slot's count raised (`swap_dup`, `swapped++`); in a shared file VMA the PTE is copied as it is, `W` included (both processes write the same cache page, `cache.wmap` counting both); otherwise a writable or COW PTE becomes `(pte & ~W) | COW` in the parent. The PTE is copied into the child, the frame gets `page_get`, and the child's `rss++`.
3. Read-only pages (such as text) are shared without `COW`.
4. Flushes the parent's TLB if it is active.
5. On failure it calls `mm_destroy` on the child. The parent's pages stay marked COW and simply regain write access on their next write fault, because their refcount is back to 1.

**Teardown (`mm_destroy`):**

- It panics with "mm_destroy of the active address space" if called on the active mm. `exit` first calls `vm_activate(NULL)`.
- It unmaps and frees every VMA (dropping pages, swap slots and file references) and warns "mm_destroy: N pages still counted resident (accounting bug)" if `rss` is not 0.
- `free_tables` recursively frees the lower-half tables (root entries 0 to 255) and the root.

**`uvm_msync(mm, start, end)`** (behind `msync`): for shared file pages in the range, it write-protects the PTEs (so the next write marks the page dirty again), then writes dirty pages back to the file (`pc_writeback`).

**Cross-address-space access:**

- `uvm_write(mm, dst, src, n)` faults each destination page in for writing, so it works on an inactive mm and resolves COW. It copies through the direct map.
- ptrace uses `uvm_peek_locked`/`uvm_poke_locked` with the tracee's `mm->lock`, taken with `mutex_trylock` under `proc_lock` (`lock_tracee_mm`), so the tracee's mm cannot go away.
- `uvm_read(mm, src, dst, n)` only translates and copies. It never allocates, and returns `-EFAULT` for unmapped pages. The profiler and user backtraces (`proc.c`, `profile.c`) use it.

**Activation (`vm_activate(mm)`)** runs on every context switch, plus in `exec` and `exit`.

- It panics if the root page is not a live `PG_PGTABLE` page ("vm_activate: page table … is not a live page table (… freed while still in use?)").
- It re-syncs the kernel half if `kernel_gen` is stale.
- It switches `satp` through `vm_switch` (`vmm.c`), which tags the address space with an ASID (below) and flushes only when needed.
- `NULL` selects `kernel_pagetable`, which kernel threads and idle threads use.

### The page cache (`pagecache.c`)

File pages and the swap cache share one hash table (4096 buckets) keyed by **(owner, ino, index)** and one LRU list:

- **File pages**: `owner` is the file's `super_block`, `ino` its inode number, `index` the page number in the file. Keying by (sb, ino) rather than by the in-memory inode means cached pages outlive the inode, which the VFS frees as soon as its last reference goes, so a file read again is still cached.
- **Swap-cache pages**: `owner` is `&swap_owner`, `index` the swap slot.

The cache holds one reference on every page in it, and anyone using a page (a PTE, a reader) holds another; a page with only the cache's reference is unmapped and may be freed (after it is written, if dirty). `pc_lock` (a spinlock) covers the hash table, the LRU and the `PG_CACHE`/`PG_SWAPCACHE` flags; filling a file page happens under the file's inode lock, which also orders fills against writes and truncation.

- **Reads**: efs's file `read()` copies from cached pages (`pc_get` fills a missing page from the file; `cache_hits`/`cache_misses`). User copies are done with no filesystem lock held, from the cache page (or through a kernel bounce buffer in `file.c`), so a fault on the user buffer can take `mm->lock` without inverting the lock order.
- **Writes**: `write()` goes to the filesystem and its log as before, then `pc_update` copies the new bytes into any cached pages they cover, so `read`, `write` and `mmap` see the same bytes.
- **Truncation and deletion**: `O_TRUNC` calls `pc_truncate` (forget pages past the end, zero the tail of the last); the final `iput` of an unlinked file calls `pc_drop_inode`.
- **Dirty pages** come only from writes through shared mappings. They go back to the file (`pc_writeback`) at `msync`, `fsync`, `sync` (`vfs_sync_all` writes them before waiting for the log), every 5 s from kswapd, and before reclaim frees them. `ilock_try` (new in `vfs.c`) lets writeback skip files whose lock is busy.
- **Shrinking**: `pc_shrink(want, may_write)` walks the LRU from the cold end, giving a page with `PG_REFERENCED` a second chance, and frees unmapped pages, writing dirty ones first when allowed (swap-cache pages to their slot; file pages only when `may_write`).

### Swap (`swap.c`, `swap_format.h`)

Swap space is the partition named `swap` (`SWAP_MB`, 128 MiB by default; GPT type `GPT_TYPE_SWAP`, GUID `57535545-5041-4e55-8000-45554e554348`). Page 0 is a header (magic `EUSWAP01`, version 1, `npages`, a label); pages 1..`npages`−1 are page-sized slots. `swap_init` finds the volume by name, checks the header, and logs `swap: disk0p5, 128 MiB (N slots)`; `noswap` on the command line turns swap off.

**`swapon` and `swapoff`** (system calls 224 and 225, root only). `swap_find_volume` accepts `/dev/NAME`, a volume name (`disk0p5`) or a GPT name (`swap`); `swap_on` reads and validates the header and starts using the volume. `swapoff` runs `swap_off` (`reclaim.c`):

1. `swap_drain(true)`: `swap_enabled()` becomes false, so no new slots are given out and `uvm_reclaim` leaves anonymous pages alone.
2. Each process with swapped-out pages (found by `lock_next_swapped`: `mutex_trylock` of its `mm->lock` under `proc_lock`, else wait 1 ms and look again) gets `uvm_swapin_all`, which reads its pages back, allocating under the mm lock.
3. Under `reclaim_lock`, `pc_drop_swap_cache`: swap-cache pages still mapped become ordinary anonymous pages.
4. If no slot is in use, `swap_retire`; otherwise it tries again, up to 4 rounds, then fails with `EBUSY`.

If the pages do not fit (`ENOMEM`) or a read fails (`EIO`), swap stays on and the kernel logs `swapoff: out of memory bringing pages back; swap stays on`.

Nothing about slots is on disk: swap starts empty every boot. In memory each slot has a `u16` count: the PTEs holding it (a page swapped out after `fork` keeps one slot shared by both processes) plus one while its page is in the swap cache. A slot is free at 0. `swap_alloc`, `swap_dup` and `swap_free` maintain the counts; `swap_write`/`swap_read` do the I/O through `blk_rw`.

### Reclaim and kswapd (`reclaim.c`)

Free memory has three marks, in pages: **min** = RAM/128 (a reserve user allocations leave for the kernel), **low** = RAM/64 (below it kswapd wakes) and **high** = RAM/32 (where kswapd stops). Every user page (anonymous, page cache, swap cache) comes from `user_page_alloc(zero)`:

- below **low** it wakes kswapd;
- below **min** it reclaims itself (direct reclaim); if kswapd holds the reclaim lock, it waits 2 ms and retries instead;
- after 8 rounds in a row with no progress anywhere, it logs `out of memory for user pages: N pages free, nothing left to reclaim` and returns NULL: the fault fails with `-ENOMEM` and the process gets SIGBUS ("out of memory").

Kernel allocations (`alloc_pages`, `kmalloc`) do not reclaim; they rely on the reserve.

`reclaim(want, direct)` runs under `reclaim_lock`, one reclaimer at a time, and repeats:

1. `pc_shrink`: free cold unmapped page-cache and swap-cache pages, writing dirty ones first. kswapd may write file pages; direct reclaim may not, since its caller may hold filesystem locks.
2. If that was not enough, `uvm_reclaim` visits the processes in turn (taking each `mm->lock` with `mutex_trylock`, under `proc_lock` so the mm cannot go away) from where it left off (`scan_va`). A PTE with `A` set loses it (a second chance). Otherwise the page is unmapped: a file page just goes (the file has it); an anonymous page gets a swap slot, goes into the swap cache dirty, and its PTE becomes a swap PTE (`swapped++`). Page references are dropped only after `tlb_shootdown`, since the process may be running on another CPU. Pages unmapped from their last process are freed by the next `pc_shrink`.

kswapd (a kernel thread, visible in `ps`) wakes when an allocation sees free memory below low, and at least once a second; it reclaims up to high, and every 5 s writes dirty file pages back.

### mincore and drop_caches

- **`mincore(addr, len, vec)`** (system call 232, `uvm_mincore`): `vec[i]` bit 0 is set if page `i` is mapped, or, in a file mapping, if the file's page is in the page cache. An unaligned `addr` is `EINVAL`; a range with unmapped pages is `ENOMEM`. `fincore` uses it on a file it maps without touching.
- **`/proc/drop_caches`** (mode 0644): reading it says what could be dropped; writing anything calls `pc_drop_all`, which writes dirty pages back, then clears the second-chance marks and runs `pc_shrink` under `reclaim_lock` until nothing more goes (pages still mapped stay). It is one of procfs's writable root files ([06](06-filesystems.md#procfs)).

### ASIDs (`vmm.c: asid_init`, `vm_switch`)

`asid_init` probes how many ASID bits `satp` keeps (16 on QEMU) by writing all ones and reading back; with too few, or with the boot option `noasid`, ASIDs are not used and every switch flushes the TLB. Otherwise each CPU has `ASID_SLOTS` = 15 slots, each remembering `(mm id, tlb_gen)`. `vm_switch` gives the incoming mm the slot that already has its id, or takes the next slot round-robin; it flushes that ASID (`sfence.vma zero, asid`) only when a new mm takes over the slot or the mm's `tlb_gen` changed since this CPU last ran it, and otherwise switches with no flush at all. Every PTE change bumps `tlb_gen` (`tlb_changed`). `/proc/asids` shows per-CPU switches with and without a flush.

### Faults (`trap.c`)

**User-mode page faults** (`user_exception`):

1. Instruction, load and store page faults map to `FAULT_EXEC`, `FAULT_READ` and `FAULT_WRITE`.
2. `uvm_fault` runs with interrupts enabled.
3. A `user_fault` trace event (`addr`, `access`, `result`) is recorded.
4. On error the process gets `SIGBUS` ("out of memory", for `-ENOMEM`) or `SIGSEGV` ("permission denied" or "no mapping").
5. Misaligned accesses give `SIGBUS`. Access faults give `SIGSEGV` ("bad access").

**Kernel-mode faults** are tried in this order:

1. Page fault with `stval >= KERNEL_BASE`: `vm_kernel_fault_fixup` (lazy kernel-root sync).
2. For memory faults (load/store page, access and misaligned faults):
   - If a process with an mm is current, `stval < USER_TOP`, the faulting `sepc` has an `__ex_table` entry, and the fault is a load or store *page* fault, call `uvm_fault` and retry on success. This gives `copy_*_user` transparent demand paging and COW.
   - Otherwise, if `sepc` has an extable entry, jump to its fixup. `__copy_user` then returns the number of bytes not copied, `__strncpy_user` returns -1, and `safe_*` returns -1.
3. Otherwise the fault is not handled and `panic_trap` runs. It prints `vm_fault_hint` (NULL, near-NULL, poison patterns, non-canonical, user address from kernel, stack guard, write to text/rodata/ksyms, firmware memory, unmapped physical/MMIO) and the page-table walk.

### User copies (`uaccess.S`, `uaccess.c`)

- `copy_from_user` and `copy_to_user` reject ranges that do not lie entirely below `USER_TOP` (`user_range_ok`) with `-EFAULT`. They then call `__copy_user`, which sets `sstatus.SUM`, copies byte by byte, and clears `SUM` on both the success and the fixup path.
- `strncpy_from_user` caps the copy at `USER_TOP`. It returns:
  - the length;
  - `-EFAULT` on a fault, or if no NUL appears before `USER_TOP`;
  - `-ENAMETOOLONG` (with `dst[max-1] = 0`) if no NUL appears within `max` bytes.

The range check matters because `SUM` would otherwise let the copy loop touch any address.

## Concurrency and locking

| Lock | Type | Protects | Notes |
|---|---|---|---|
| `zone_lock` ("pmm") | spinlock, irqsave | the buddy zone (free lists, counts, page flags of free blocks), `stat_*` | Held only around `buddy_alloc`/`buddy_free`. Poison checks and memsets run outside it. `pmm_verify_free` holds it while scanning all free memory |
| `c->lock` (named after the cache, e.g. "kmalloc-64") | spinlock, irqsave | a cache's slab lists, per-slab free lists, `inuse`, counters | Held across `new_slab`, which calls `alloc_pages_site` and formats the slab |
| `kpt_lock` ("kernel-pt") | spinlock, irqsave | `kernel_pagetable` updates after `vm_ready` | Held across `walk`, which may allocate tables (taking `zone_lock`) |
| `kstack_lock` ("kstack") | spinlock, irqsave | `slot_used`, `cache[]`, counters | Dropped before mapping, unmapping or shootdown |
| `mm->lock` | mutex | the page table, `rss`/`swapped`, the VMA list | Held by faults, mmap, munmap, brk, mprotect, msync, fork, exit, ptrace (`mutex_trylock` under `proc_lock`) and reclaim (`mutex_trylock`). Never held while allocating a user page or reading swap (faults drop it and retry) |
| `mm->vma_lock` ("vma") | spinlock, irqsave | VMA list *changes* against readers that cannot sleep | Readers (`mm_print`, used by `/proc/<pid>/maps` and the monitor) also rely on `proc_lock` to keep the mm alive |
| `pc_lock` | spinlock | the page cache's hash table, LRU, and the `PG_CACHE`/`PG_SWAPCACHE` flags | A file page is filled under its inode lock |
| `reclaim_lock` | mutex | one reclaimer at a time (kswapd or a direct reclaimer) | A direct reclaimer that finds it held waits 2 ms and retries |

**Lock order** (from the code paths above): `c->lock` → `zone_lock`, and `kpt_lock` → `zone_lock`. `zone_lock` is a leaf. For user memory: `mm->lock`, then the filesystem (a file fault takes the inode lock; closing a mapped file), then `pc_lock`. User copies in `read`/`write` are made with no filesystem lock held, so a fault on the user buffer (which takes `mm->lock`) cannot invert this order. lockdep (`kernel/debug/lockdep.c`) checks the order at run time.

**Interrupt context.** All allocator locks disable interrupts, and nothing in pmm or slab sleeps, so `kmalloc`, `kfree`, `page_alloc` and `page_free` are usable from interrupt handlers. `kstack_free` and `kstack_alloc` may call `tlb_shootdown`, which must not run from an interrupt handler that interrupted a shootdown (comment in `smp.c: tlb_shootdown`).

**User mm.** Every change to a page table or VMA list happens under `mm->lock` (see the table). Faults allocate and read swap with it dropped and re-check afterwards. Readers that cannot sleep (`mm_print` for `/proc/<pid>/maps` and the monitor's `vm`, which uses `mutex_trylock`) take `vma_lock` and rely on `proc_lock`; `uvm_read` (profiler, user backtraces) only translates and reads through the direct map. Page references are atomic (`page_get`/`page_put`), and user-memory page flags are set and cleared atomically.

**TLB coherence:**

| Kind of change | How other CPUs see it |
|---|---|
| Kernel mappings that are added | Local `sfence.vma` only. Other harts are assumed not to cache invalid entries. Missing *top-level* entries in process tables are fixed lazily in the fault path |
| Kernel mappings that are removed | `tlb_shootdown()`. Today the only callers are `kstack.c: unmap_pages` and the SMP test |
| User page tables | Every PTE change bumps `mm->tlb_gen` and flushes locally if the mm is active; another CPU flushes the mm's ASID when it next runs it (`vm_switch`). Reclaim, which changes processes that may be running elsewhere, also calls `tlb_shootdown()` before dropping page references |

**SUM.** The scheduler clears `SSTATUS_SUM` before every context switch (`sched.c`), so user access cannot leak into another thread. `SUM` is part of the saved `sstatus` in the trap frame, so a copy interrupted by preemption resumes with `SUM` set.

**Counters.** Statistics such as `tlb_shootdowns`, `large_allocs`, `large_frees`, `large_active_bytes` and the `vmc` paging counters (`/proc/vmstat`) are updated without locks or atomics. They may be slightly off under SMP.

## Interfaces

### Physical pages (`mm.h`)

| Function | Notes |
|---|---|
| `alloc_pages(order)`, `alloc_pages_site(order, site)` | Returns the head `struct page` or NULL. Contents poisoned. Panics before `pmm_init` |
| `free_pages(pg, order)` | `order` must match the allocation |
| `page_alloc()`, `page_alloc_zeroed()`, `page_free(va)` | Order-0 pages by kernel VA |
| `page_get(pg)`, `page_put(pg)` | Reference counting. `put` frees at 0 |
| `pa_to_page`, `page_to_pa`, `page_to_va`, `va_to_page`, `compound_head` | Conversions. `pa_to_page` returns NULL outside managed RAM or before `pmm_init` |
| `pmm_get_stats`, `pmm_print`, `pmm_verify_free`, `page_print`, `pmm_region_name` | Introspection |
| `mm_corruption`, `mm_corruption_dump`, `mm_panic_on_corruption`, `mm_corruptions` | Reporting, shared with slab and usable by other subsystems |

### Kernel heap

| Function | Notes |
|---|---|
| `kmalloc`, `kzalloc`, `kmalloc_site`, `kzalloc_site` | Up to 4 MiB (order 10). New memory is `0x5a`-filled unless `kzalloc` |
| `kfree`, `ksize` | NULL-safe. Invalid pointers are reported |
| `kmem_cache_create(name, size)` | At most 32 caches in total (11 are taken by kmalloc). Panics when full |
| `kmem_cache_alloc`, `kmem_cache_alloc_site`, `kmem_cache_free`, `kmem_cache_name` | |
| `slab_print`, `slab_verify`, `slab_leak_report(print, prefix)` | Introspection |

Use the `*_site` variants in wrapper functions and pass `__builtin_return_address(0)`, so that `slab leaks` and `page` attribute allocations to the real caller (`docs/DEBUGGING.md`).

### Page tables and kernel VA

`vm_map`, `vm_unmap` (follow with `tlb_shootdown()` for kernel mappings), `vm_translate`, `vm_walk`, `vm_current`, `vm_sync_kernel`, `ioremap`, `vm_dump`, `vm_walk_print`, `vm_fault_hint`, `pte_flags_str`, `sfence_vma`, `sfence_vma_all`, `kstack_alloc`, `kstack_free`, `kstack_stats`, `addr_in_stack_guard`, `tlb_shootdown` (`smp.h`).

### User memory (`proc.h`)

`mm_create`, `mm_destroy`, `mm_fork`, `vma_find`, `vma_add`, `vma_add_file`, `vma_unmap`, `vma_unmap_locked`, `vma_protect`, `uvm_find_gap`, `uvm_msync`, `uvm_reclaim`, `uvm_peek`, `uvm_poke`, `uvm_peek_locked`, `uvm_poke_locked`, `uvm_fault`, `uvm_populate`, `uvm_write`, `uvm_read`, `vm_activate`, `mm_print`, `vma_kind_name`, `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `user_range_ok`.

### Page cache, swap and reclaim (`mm.h`)

`user_page_alloc(zero)`, `reclaim(want, direct)`, `swap_off`, `pc_drop_all`, `pc_drop_swap_cache`, `uvm_swapin_all`, `uvm_mincore`, `kswapd_wake`, `reclaim_init`; `pc_get`, `pc_get_spare`, `pc_find`, `pc_insert`, `pc_remove`, `pc_update`, `pc_truncate`, `pc_drop_inode`, `pc_writeback`, `pc_shrink`, `pc_stats`; `swap_init`, `swap_find_volume`, `swap_on`, `swap_drain`, `swap_retire`, `swap_enabled`, `swap_alloc`, `swap_dup`, `swap_free`, `swap_read`, `swap_write`; `vmc` (the counters).

### Fault-tolerant access (`safe.h`)

`safe_read_u8/16/32/64(addr, &out)` and `safe_write_u8/16/32/64(addr, val)` return 0, or -1 on a fault. `extable_lookup(pc)` is a linear scan of `__ex_table`.

### System calls built on this subsystem

`brk`, `mmap`, `munmap`, `mprotect`, `msync`, `mincore` (232), `swapon` (224) and `swapoff` (225) are in `kernel/proc/syscall.c`; the address-space calls run under `mm->lock` (`mincore` 256 pages at a time). `mmap` maps anonymous memory (`MAP_PRIVATE` only: shared anonymous memory is `EINVAL`) or a file (`MAP_SHARED` or `MAP_PRIVATE`): a non-regular file is `ENODEV`, a shared writable mapping of a file not open for writing `EACCES`, an unaligned offset `EINVAL`; W^X is enforced. `MAP_FIXED` replaces what is at the address; otherwise the address is a hint, used if the range is free, else `uvm_find_gap` chooses. `msync(227)` takes `MS_ASYNC`, `MS_SYNC` and `MS_INVALIDATE` (`uvm_msync` writes dirty shared pages back in every case). `fork` and `execve` (`kernel/proc/proc.c`) use `mm_fork`, `mm_create`, `uvm_write`, `vm_activate` and `mm_destroy`.

### Boot options

| Option | Effect |
|---|---|
| `nopoison` | Disables free-page poisoning and checking (`pmm.c`). Slab poisoning and redzones are always on |
| `log=mm:trace` | Logs every page allocation with its site (`pr_trace` in `alloc_pages_site`) |
| `log=trap:trace` | Logs extable fixups ("fault at … fixed up -> …") |
| `smp=<n>` | With `smp=1`, `tlb_shootdown` only flushes locally |
| `noswap` | Do not use the swap partition |
| `noasid` | Do not use ASIDs: every address-space switch flushes the TLB |

### /proc files

| File | Source |
|---|---|
| `/proc/meminfo` | `pmm_print` and `vm_memory_print`: page-cache, swap-cache and swap lines (the `free` command shows them too); RAM, total/reserved/in-use/free pages, alloc/free/failure/corruption counts, free blocks by order, kernel stack counts, reserved regions |
| `/proc/slabinfo` | `slab_print`: per-cache object size, slot size, objects per slab, active, total, slabs, allocs, frees, peak; large-kmalloc totals |
| `/proc/<pid>/maps` | `mm_print`: VMAs with prot, kind, resident and swapped pages, shared COW pages, `(N dirty)`, and `shared+0xOFF /path` or `private+0xOFF /path` for file mappings; a footer with rss, swapped and major faults, plus faults, COW copies and brk |
| `/proc/vmstat` | `vm_counters_print` (also `total` and `reserved` pages): `faults`, `major_faults`, `cache_hits`, `cache_misses`, `swap_outs`, `swap_ins`, `swap_out_skipped`, `writebacks`, `scanned`, `unmapped`, `freed`, `kswapd_runs`, `direct_reclaims`, `alloc_stalls`, `oom`, and the free-memory marks |
| `/proc/swaps` | the swap volume, size, used and peak |
| `/proc/stat` | `uptime_ms`, `cpus`, `cpuN idle_ms X switches Y` per CPU, `procs`, `procs_running`, `procs_blocked` (what `top` and `vmstat` use) |
| `/proc/drop_caches` | Writable by root: drop the clean, unmapped page cache (see above) |
| `/proc/pagecache` | cached pages by file (and the swap cache), dirty and mapped counts |
| `/proc/asids` | per CPU: ASID switches with and without a flush |

### Monitor commands (`kernel/debug/monitor.c`)

| Command | Function |
|---|---|
| `meminfo` | `pmm_print` |
| `pt` / `pt <va>` | `vm_dump` of the active table (coalesced runs, with 1G/2M/4K counts) / `vm_walk_print` plus `vm_fault_hint` |
| `v2p <va>` | `vm_translate` in `kernel_pagetable` only |
| `p2v <pa>` | Direct-map address |
| `page <pa\|va>` | `page_print`: flags, order, refcount, block head, alloc/free sites, slab cache, kmalloc size |
| `slab` / `slab leaks [prefix]` | `slab_print` / `slab_leak_report`. The `large` prefix selects page-backed kmallocs |
| `heapcheck` | `pmm_verify_free` and `slab_verify` with panics suppressed |
| `vm <pid> [va]` | `mm_print` or `vm_walk_print` on that process's table (takes `mm->lock` with `mutex_trylock`) |
| `vmstat`, `swap`, `pagecache` | The paging counters, the swap space, the page cache |
| `mem <addr> [len]`, `poke <addr> <val> [size]` | `hexdump_safe` / `safe_write_*` |
| `kmalloc <size>`, `kfree <addr>` | Experiments |
| `crash uaf\|overflow\|wx\|stack\|null\|store` | Trigger the corresponding detectors and fault paths |

## Debugging and observability

**Where to look first:**

- `meminfo` for exhaustion or fragmentation: look at free blocks by order and the `failures` count.
- `slab` for a growing `active` count.
- `slab leaks` to see who holds the memory. It groups live objects by allocation site and cache, sorted by bytes, with at most 64 sites.
- `page <addr>` to get a frame's history.
- `vm <pid>` for a process's map.
- **Paging**: the `top`, `vmstat`, `free`, `pmap`, `fincore` and `swapon` commands ([10](10-user-space.md#the-memory-tools-top-vmstat-free-pmap-fincore-dropcache-swapon)); `/proc/vmstat` (or `vmstat` in the monitor) for faults, major faults, cache hits, swap-outs and swap-ins, reclaim scans and `oom`; `/proc/meminfo` and `free` for page-cache, swap-cache and swap totals; `/proc/swaps`, `/proc/pagecache`, `/proc/asids`; the swap column and `(N dirty)` of `/proc/<pid>/maps`; `kswapd` and threads waiting in reclaim in `ps`. `memhog MB` (`user/bin/memhog.c`) fills that much anonymous memory, forks, checks every word in both processes, and prints the paging counters that changed: the quick way to watch swap work (boot with `MEM=64M` to force it).

**GDB** (`tools/gdb/eunuchs.py`) has `eu-vm [va]`, `eu-meminfo`, `eu-page <addr>` (with the cache flags, the cache owner, inode and index, or the swap slot), `eu-slab`, `eu-mm PID` (VMAs with resident, swapped and COW pages, file mappings, the mm's counters, id, `tlb_gen` and lock holder), `eu-swap` (the volume, slots used and peak, draining, and slots by reference count) and `eu-pagecache` (file and swap-cache pages, dirty and in use, the top 20 files, all the `vmc` counters). The static names they read are `pc_lru`, `swap_vol`, `slot_counts`, `swap_nslots`, `swap_nused`, `swap_peak` and `swap_draining`. They read kernel memory directly, so they work even when the kernel is wedged.

**Tests:**

- `make test-host` runs `tests/host/test_buddy.c`: 3000 pages with a reserved hole, exhaustion, random alloc/free with an ownership map, full coalescing, and error paths.
- The in-kernel tests run with the `ktest` boot option or `ktest [prefix]` in the monitor (for example `ktest pmm`, `ktest kmalloc`, `ktest vm`). Corruption tests use `EXPECT_CORRUPTIONS(n, body)` in `test_mm.c`, which flips `mm_panic_on_corruption` and counts reports.

**Trace events:**

- `user_fault` (group `proc`) records every user page fault: `addr`, `access`, `result`.
- `pagecache_fill`, `writeback`, `swap_out`, `swap_in` and `reclaim` record paging activity; they are in group `mm`, which is off by default (`trace=mm` turns it on).
- `trace_events.h` also declares `page_alloc`, `page_free`, `kmalloc` and `kfree` in group `mm` (disabled by default: `trace_groups_enabled = TG_ALL & ~TG_MM`). **No code currently emits them**, so enabling the `mm` group records nothing.

**Typical messages and their meaning:**

| Message | Meaning |
|---|---|
| `*** MEMORY CORRUPTION: page use-after-free: block pa … modified after free; bytes +a..+b …; last freed at S` | Someone wrote to a page after `free_pages`/`page_free`. Found at the next allocation of that block, or by `heapcheck` |
| `… double free of page pa …; previously freed at A, allocated at B` | `free_pages` on a block head that is still `PG_FREE` |
| `free_pages of pa … which is reserved memory` / `not the head of its block` / `still has N references` / `order mismatch` | Wrong argument to `free_pages`/`page_free` |
| `page_put: refcount of pa … went negative` | Unbalanced `page_put` |
| `out of memory for user pages: N pages free, nothing left to reclaim` | Memory and swap are full; the faulting process gets SIGBUS (`oom` in `/proc/vmstat` counts these) |
| `swap: disk0p5, M MiB (N slots)` / `swap: no swap partition` / `has no valid swap header; not used` / `swap: off (noswap)` | Swap at boot (also from `swapon`) |
| `swapoff: out of memory bringing pages back; swap stays on` | `swapoff` failed with `ENOMEM` or `EIO` |
| `drop_caches: freed N pages (N KiB)` (debug level) | A write to `/proc/drop_caches` |
| `reclaim: free memory marks min …, low …, high … pages; kswapd started` | Reclaim at boot |
| `ASIDs: 16 bits; 15 per CPU` / `ASIDs off (noasid)` | ASIDs at boot |
| `heap overflow in kmalloc-N object P (R bytes requested, allocated at S): bytes +a..+b written past the end, detected at kfree` | Overflow past the requested size (including into size-class slack) |
| `heap underflow in … redzone byte at offset -k overwritten` | Write before the object start |
| `use-after-free write to kmalloc-N object …` | A free object's `0x6b` fill changed. Found on reallocation or `heapcheck` |
| `double free of kmalloc-N object …: already freed at A …, freed again at C` | |
| `kfree(P): pointer is N bytes into …` / `not a heap pointer …` / `not allocated by kmalloc (page flags …)` | Bad pointer passed to `kfree` |
| `<cache> freelist corrupted: slot … has state …` / `metadata corrupted` | Something overwrote `obj_meta`. Usually an overflow larger than 8 bytes from the previous slot |
| `out of memory: order-N allocation from S failed (M pages free)` | Buddy exhausted, or fragmented for that order |
| `kmalloc(N) from S: too large` | Request above 4 MiB |
| `kstack_alloc: all 512 stack slots in use` | Too many live threads |
| `vm_map: va … already mapped` / `vm_unmap: va … not mapped` / `partial unmap of a level-N page` | Page-table API misuse |
| `mm_destroy: N pages still counted resident (accounting bug)` | `rss` bookkeeping mismatch |
| `vm_activate: page table … is not a live page table …` | Switching to an mm whose root was freed |
| `TLB shootdown: cpuN did not respond within 3000 ms …` | A CPU has had interrupts off without spinning on a lock for 3 s |
| `ioremap: too many regions`, `too many reserved memory regions`, `no room for mem_map`, `kmem_cache_create(…): too many caches` | Static table limits reached (panic) |

Kernel faults land in `panic_trap` with a `vm_fault_hint` diagnosis, such as "pointer loaded from freed memory (0x6b poison): use after free" or "kernel stack overflow (access to a stack guard page)". To find the writer of a corrupted value, `make record` the run, replay it under GDB, set a watchpoint and `reverse-continue` (`docs/DEBUGGING.md`).

**Profiling note:** with poisoning on, `memset` and `memchr_inv` from pmm and slab show up prominently. Boot with `nopoison` to measure page-allocator cost without page poisoning.

## Limitations and known issues

**Physical memory:**

- **One zone, one global lock.** There are no per-CPU page lists. Every page allocation and free on every CPU takes `zone_lock`.
- **Double-free detection misses coalesced blocks.** When a freed block merges with a lower buddy, the freed head's `PG_FREE` is not set and its flags are left at 0 (`buddy.c: free_block`). A second `page_free` of that page then passes every check in `free_pages_site` and in `buddy_free`. `free_block` inserts it as a new free block *inside* the already-free larger block, which leads to overlapping allocations later. The kernel test `pmm_detects_double_free` does not specifically exercise the merged-upper-buddy case.
- **Interior pages of free blocks look allocated** (`flags == 0`), which can mislead `page` output.
- **Boot poisoning** `memset`s all free RAM (about 120 MiB on the default machine), and `heapcheck` scans it all with `zone_lock` held and interrupts off.
- **`pmm_init` trusts reserved regions to lie inside RAM.** A reserved region starting above `ram_end` would make the gap poisoning `memset` run past the end of RAM.

**Kernel heap:**

- **Debug overhead is permanent.** Every slab object carries 48 bytes of redzones and metadata (`kmalloc-8` uses 56-byte slots), and slab poisoning cannot be turned off. `kmalloc-2048` wastes 1328 bytes per 32 KiB slab.
- **Size class lookup is a linear scan.** There are no per-CPU object caches, and `new_slab` formats the whole slab (up to 32 KiB) while holding the cache lock.
- **`kmalloc` maximum is 4 MiB** (`MAX_ORDER`), and caches cannot be destroyed.

**Page tables and kernel VA:**

- **`ioremap` regions are matched only by exact start address.** A second, larger request with the same start, or an overlapping request, makes `vm_map` fail and `ioremap` panic. `nr_ioremaps` is not locked (all callers are init code).
- **`vm_map` does not roll back** partial mappings on failure.
- **Newly mapped kernel pages are not shot down.** The code relies on other harts not caching invalid PTEs. QEMU behaves this way, but the RISC-V privileged spec allows harts to cache them, so a hart could take a spurious fault that `vm_kernel_fault_fixup` does not fix (it only repairs missing root entries), followed by a panic.
- **`tlb_shootdown` always flushes everything** rather than a range or one ASID. Kernel global mappings and user mappings share the TLB; with ASIDs, only user switches avoid flushing.

**User memory:**

- **No shared anonymous memory** (`MAP_SHARED|MAP_ANONYMOUS` is `EINVAL`), **no swap readahead** (each swap-in reads one page), **no OOM killer**: when memory and swap are exhausted, the faulting process gets SIGBUS.
- **One swap volume, and `swapoff` needs room.** Only one volume is used at a time (`swapon` of a second is `EBUSY`). `swapoff` must bring every swapped page back into memory, so with more swapped out than fits it fails (`ENOMEM`) and swap stays on: on a 64 MiB machine, 80 MiB in use fails cleanly, 30 MiB comes back. Paging in for `swapoff` runs one page at a time, in the caller's thread.
- **Swap capacity with `fork`.** A page read back from swap while another process still holds its slot takes both a page and the slot. `memhog 160` (which forks) on 128 MiB of RAM with 128 MiB of swap fills swap completely this way.
- **Truncation and mappings.** `pc_truncate` detaches cached pages from the cache but not from the mappings that hold them, so a mapping beyond the new end of file keeps the old data instead of getting SIGBUS.
- **exec does not use the page cache**: programs are still read whole with `vfs_read_all` into kernel memory, and text is not shared between processes running the same program.
- **Only user memory reclaims.** Kernel allocations (`kmalloc`, page tables, kernel stacks) never trigger reclaim; they rely on the `min` reserve.
- **The monitor's `vm`** uses `mutex_trylock` on `mm->lock` and shows nothing for an mm that is busy.
- **One thread per mm.** `mm->lock` makes the mm safe for the debugger and reclaim, but user threads would also need per-thread TLB shootdowns (today a running mm is only on one CPU).
- **VMA operations are linear.** VMAs are a linked list, so lookups and inserts are O(n), and `mm_fork` re-inserts each VMA with `vma_add`.
- **`vma_protect` is not failure-atomic across VMAs.** Each VMA's splits are allocated before it is changed, but if a later VMA's split fails, the VMAs already processed keep their new protection. `mprotect` with a length of 0 returns 0.
- **Making memory executable is expensive.** Every `mprotect` that adds `PROT_EXEC` sends a remote `fence.i` to all harts through SBI, whether or not they ever ran the process.
- **`uvm_write` counts as faults.** It calls `uvm_fault` for every page it writes, so `mm->faults` includes exec-time stack writes.

**Uaccess and extable:**

- **Byte-by-byte copies.** `__copy_user` and `__strncpy_user` move one byte at a time.
- **The uaccess retry in `trap_handler` applies to every `__ex_table` entry**, including `safe_read_*`/`safe_write_*`. Those run without `SUM`, so a `safe_*` access to a valid user address while a process is current faults, `uvm_fault` returns 0 (the page is mapped or gets mapped), and the instruction is retried forever. For example, monitor `mem 0x10000` after a user breakpoint hangs. See the bug list in the report.
- **Interrupt handlers can run with `SUM` set.** `SUM` is not cleared on kernel trap entry, so a handler that interrupts a `copy_*_user` runs with user access enabled.
- **`extable_lookup` is a linear scan.** This is fine at today's table size: 11 entries (8 in `safe.S`, 3 in `uaccess.S`).

**Kernel stacks:**

- **Limits.** At most 512 kernel stacks (threads plus per-CPU emergency stacks), and 32 cached.
- **Cached stacks are not checked.** They are not poisoned while in the cache, so a stale write into a freed stack goes unnoticed.

**Statistics:** some counters are racy under SMP (see Concurrency).

## Potential improvements

1. **Make double free of coalesced pages detectable** — Mark every page of a free block, for example with a `PG_FREE` "body" bit set in `free_block`/`add_free_block` and cleared on split and alloc. Alternatively, have `buddy_free` check whether any enclosing block at a higher order is free. Today a second `page_free` of an upper buddy corrupts the free lists silently. Add a host test in `tests/host/test_buddy.c` for this case. Impact: high. Effort: small.
2. **Restrict the uaccess demand-paging retry to uaccess code** — In `trap.c: trap_handler`, only call `uvm_fault` when `sepc` lies inside `__copy_user`/`__strncpy_user`, for example with a separate `__uaccess_table` or a flag in `struct extable_entry`. This fixes the infinite fault loop for `safe_read_*` on user addresses (monitor `mem`, `hexdump_safe`, backtraces through corrupt frame pointers). Impact: medium. Effort: small.
3. **Initialize `*pg_out` in `slab.c: find_slot`** — When `va_to_page` fails, `kfree` and `ksize` go on to read an uninitialized `pg`. This only happens when `mm_panic_on_corruption` is false (tests, `heapcheck`), but it is undefined behaviour. Impact: low. Effort: small.
4. **Emit the declared MM trace events** — `page_alloc`, `page_free`, `kmalloc` and `kfree` exist in `trace_events.h` (group `mm`) but no code calls `trace()` for them. Emitting them from `alloc_pages_site`, `free_pages_site`, `kmalloc_site` and `kfree` would make allocation churn visible in Perfetto. Impact: medium. Effort: small.
5. **Clear `SUM` on kernel trap entry** — Clear `SSTATUS_SUM` in `trapentry.S` after saving `sstatus`. It is restored from the frame on return, so interrupt handlers and the fault path never run with user access enabled. Impact: medium. Effort: small.
6. **Word-at-a-time user copies** — Copy 8 bytes at a time in `uaccess.S` when source and destination are aligned, falling back to bytes at the edges and keeping exact "bytes not copied" on faults. Every `read`/`write` system call goes through this loop. Impact: medium. Effort: medium.
7. **Allocate slab pages outside the cache lock** — In `take_slot`, drop `c->lock` before `new_slab`, format the slab, then re-take the lock and insert it. This removes a nested `zone_lock` acquisition and up to 32 KiB of `memset` with interrupts off from the critical section. Impact: medium. Effort: small.
8. **Per-CPU page and object caches** — Add a small per-CPU free list of order-0 pages and per-CPU magazines for the busiest kmalloc classes, to cut contention on `zone_lock` and `c->lock` with several CPUs. Keep the FIFO-delay property optional for debugging. Impact: medium. Effort: large.
9. **Handle spurious faults on new kernel mappings** — Extend `vm_kernel_fault_fixup` so that if `vm_translate(kernel_pagetable, va)` succeeds with compatible permissions, it runs a local `sfence.vma` and retries. This makes kernel stack and `ioremap` mappings safe on harts that cache invalid PTEs. Impact: medium. Effort: small.
10. **Make `ioremap` robust to overlap** — Check for containment or overlap against existing regions, map only the missing pages, and take `kpt_lock` (or a dedicated lock) around the `ioremaps[]` update. Impact: low. Effort: small.
11. **Boot option to slim slab debugging** — For example `noslabdebug`, which drops the metadata and redzones and uses a compact slot layout. This would allow measuring and running without the 48 bytes per object of overhead, as `nopoison` already does for pages. Impact: low. Effort: medium.
12. **Host tests for slab and uvm logic** — Only `buddy.c` is host-tested. Factoring the pure parts of `slab.c` (slot geometry, free-list handling, redzone checks) and of `uvm.c` (VMA list operations, `next_present`) so they can run under ASan would catch bugs like items 1 and 3 earlier. Impact: medium. Effort: medium.
13. **Atomic statistics** — Use `__atomic_add_fetch` (or the relevant lock) for `tlb_shootdowns`, `large_allocs`, `large_frees` and `large_active_bytes`, so that `/proc/slabinfo` and `cpus` show exact numbers under SMP. Impact: low. Effort: small.
14. **Failure-atomic, targeted `mprotect`** — Allocate the split VMAs before changing anything, so `vma_protect` either fully succeeds or leaves the address space untouched; and send the remote `fence.i` only to harts that have run this `mm` since its last `fence.i` (a per-mm CPU mask), instead of every hart through SBI. Impact: low. Effort: small.
15. **Swap readahead and clustering** — Read neighbouring slots on a swap-in, and write runs of cold pages to adjacent slots, so paging does large sequential I/O instead of one page at a time. Impact: medium. Effort: medium.
16. **Shared anonymous memory** — `MAP_SHARED|MAP_ANONYMOUS` backed by an anonymous object in the page cache (owner: the object), so `fork`ed processes can share writable memory. Impact: medium. Effort: medium.
17. **exec through the page cache** — Map program text and data from the file (private mappings) instead of reading the whole file into `kmalloc` memory; text pages are then shared by every process running the program and the 4 MiB program limit goes. Impact: high. Effort: medium.
18. **Truncation that reaches mappings** — Unmap (or SIGBUS) the pages of a file beyond its new end in every mapping, as Linux does, instead of leaving them with the old data. Impact: low. Effort: medium.
19. **Ranged and ASID-targeted shootdowns** — Let `tlb_shootdown` flush one ASID or one range, and only on CPUs that have run the mm. Impact: low. Effort: small.
