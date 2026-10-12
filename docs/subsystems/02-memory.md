# Memory management

Eunuchs manages memory in five layers. A buddy allocator hands out physical page frames, each described by a `struct page`. A slab allocator built on it provides `kmalloc` and fixed-size object caches. An Sv39 page-table layer maps the kernel image (W^X), the direct map, MMIO and kernel stacks. On top of all that, per-process address spaces (`struct mm`) are built from VMAs with demand paging and copy-on-write `fork`. Every layer is instrumented for debugging: memory is poisoned, slab objects have redzones, every allocation and free records its call site, and corruption is reported with backtraces and hexdumps.

## Responsibilities and scope

**This subsystem handles:**

- Discovering usable RAM at boot (from `struct platform_info`, which the device-tree parser fills) and reserving firmware, kernel image, DTB, DT `reserved-memory` and the `struct page` array itself (`kernel/mm/pmm.c: pmm_init`).
- Allocating and freeing physical pages in power-of-two blocks of order 0 to 10 (`alloc_pages`, `free_pages`, `page_alloc`, `page_free`), with reference counts for shared frames (`page_get`, `page_put`).
- Kernel heap: `kmalloc`, `kzalloc`, `kfree`, `ksize`, and named caches (`kmem_cache_create`, `kmem_cache_alloc`, `kmem_cache_free`).
- The kernel page table: building it in `vm_init`, `vm_map`/`vm_unmap`, `ioremap`, lookups (`vm_translate`, `vm_walk`), and lazily propagating new kernel top-level entries into process page tables.
- Guarded kernel thread stacks in a dedicated virtual area (`kstack_alloc`, `kstack_free`), including the TLB shootdown needed to unmap them.
- User address spaces: creation, VMA list, changing protections (`vma_protect`, behind `mprotect`), demand-zero paging, COW fork, teardown, cross-address-space reads and writes, activation on context switch.
- Safe kernel access to user memory (`copy_from_user`, `copy_to_user`, `strncpy_from_user`) and fault-tolerant debug accessors (`safe_read_*`, `safe_write_*`), both through the exception table.
- Corruption detection and reporting (`mm_corruption`, `heapcheck`, `slab leaks`, fault hints).

**It does not handle:**

- System-call argument parsing for `brk`, `mmap`, `munmap` and `mprotect`. Those live in `kernel/proc/syscall.c` (`do_mmap`, `do_munmap`, `do_mprotect` and the brk handler), which call `vma_add`/`vma_unmap`/`vma_protect`.
- ELF loading (`kernel/proc/elf.c: elf_load`, which uses `vma_add` and `uvm_populate`) and building the initial user stack (`kernel/proc/proc.c`, which uses `uvm_write`).
- Signal delivery and the crash report for a failed user fault (`user_fault` in `kernel/proc`).
- The IPI transport. TLB shootdown (`kernel/arch/riscv/smp.c: tlb_shootdown`) is documented here, but IPIs, `ipi_poll` and stop-the-world belong to the SMP subsystem.
- Swapping, file-backed mappings, page cache. None of these exist (see Limitations).

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
| `0x2000000000` (`USER_MMAP_BASE`) | Anonymous `mmap` area. A bump pointer (`mm->mmap_next`) leaves a 1-page gap after each mapping | `syscall.c: do_mmap` |
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

The PTE bits used are `PTE_V R W X U G A D` (bits 0 to 7), plus `PTE_COW` (bit 8, one of the RSW bits) for user COW pages.

Kernel permission sets preset `A` and `D`, so the hardware never needs to update them:

| Set | Bits |
|---|---|
| `PROT_KERNEL_RX` | R X G A |
| `PROT_KERNEL_RO` | R G A |
| `PROT_KERNEL_RW` | R W G A D |

User PTEs come from `uvm.c: prot_to_pte`: `U|A|D`, plus R/W/X from the VMA's `VM_READ/VM_WRITE/VM_EXEC`. User mappings are always 4 KiB leaves. The kernel direct map uses the largest level (1 GiB, 2 MiB or 4 KiB) that alignment and size allow (`vmm.c: do_map`).

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
struct vma { struct list_head node; uintptr_t start, end; u32 prot; enum vma_kind kind; char name[16]; };
struct mm  { pagetable_t pt; spinlock_t vma_lock; struct list_head vmas;
             uintptr_t brk_start, brk, mmap_next; u64 rss, faults, cow_copies; u32 kernel_gen; };
```

- `vmas` is a doubly linked list sorted by `start`, with non-overlapping `[start, end)` page-aligned ranges.
- `kind` is `VMA_ELF`, `VMA_HEAP`, `VMA_STACK` or `VMA_ANON`. It is only used for display.
- `prot` is a mask of `VM_READ`, `VM_WRITE` and `VM_EXEC`.
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

**Creation:** `mm_create` allocates the `mm` (`kzalloc`) and a zeroed root (`PG_PGTABLE`). It copies the kernel half (`vm_sync_kernel`), records `kernel_gen`, and sets `mmap_next = USER_MMAP_BASE`.

**`exec`** (`proc.c`) builds a complete new mm before giving up the old one:

1. `elf_load` creates `VMA_ELF` areas and eagerly fills them with `uvm_populate` plus `memcpy`.
2. The 8 MiB `VMA_STACK` VMA is added.
3. The argument strings and vectors are written with `uvm_write`.
4. `vm_activate(new)` runs, then `mm_destroy(old)`.

**VMAs:**

- `vma_add` validates its arguments (`-EINVAL`) and rejects overlaps (`-EEXIST`). It allocates the VMA with `kzalloc` and inserts it in sorted position under `vma_lock`.
- `vma_find` is a linear search.
- `vma_unmap(start, end)` handles each overlapping VMA in turn. It first unmaps the pages, then, under `vma_lock`, removes the VMA, trims it at either end, or splits it (the tail is re-added with `vma_add`).
- `vma_protect(mm, start, end, prot)` (behind `mprotect`) changes the protection of a page-aligned range:
  1. The whole range must be covered by VMAs without holes (`-ENOMEM` otherwise), as on Linux.
  2. Each overlapping VMA whose protection differs is split at `start` and `end` (the outside parts are re-added with `vma_add` and keep the old protection), and the inside part gets the new `prot`.
  3. Present PTEs are rewritten: `R` and `X` follow `prot`. Write permission is **never** put directly into a PTE that lacked it: such a page becomes `PTE_COW`, and the first write fault takes it (refcount 1) or copies it (shared). This is needed because `mm_fork` shares read-only pages without marking them COW, so making one writable in place would let the process write into its parent's or child's page. A page that was writable and stays writable keeps `W`; a page that loses write permission keeps a `COW` mark that the VMA check in `uvm_fault` makes harmless.
  4. `sfence.vma` if the mm is active. If the new protection includes execute, a local `fence.i` plus `sbi_remote_fence_i` to every hart, so code written through the data cache is what the instruction fetch sees on whichever hart the process runs next.
  The `mprotect` system call (226, `do_mprotect`) checks alignment and the range, refuses `PROT_WRITE|PROT_EXEC` with `EACCES` (W^X, as for `mmap`), and rounds the length up to whole pages. `tcc -run` uses it to make freshly generated code executable ([13](13-toolchain.md)).
- `uvm_peek(mm, addr, dst, n)` and `uvm_poke(mm, addr, src, n)` are a debugger's access to a stopped process (`ptrace`). Both go through `debug_page`, which faults the page in if a VMA covers it (`uvm_populate`, whatever the VMA's protection), so a debugger can read pages the program has not touched yet. `uvm_poke` ignores the VMA's protection (breakpoints go into r-x text), but first gives the process a private copy of a page whose frame is shared (refcount > 1: text pages are shared after `fork` without COW marks), so a breakpoint never appears in another process; a writable COW page that gets copied this way becomes writable. If any page written is executable, it runs `fence.i` locally and on every hart through SBI. Both return `-EFAULT` for an address with no VMA.
- `brk` and `mprotect`: if the top of the heap was given another protection (tcc's `-run` memory comes from `malloc`, so from the heap), `do_brk` starts a new `heap` VMA at the old top instead of growing the changed one.

**Page-table walks:**

- `next_present` finds the next valid level-0 PTE. It skips whole 1 GiB or 2 MiB ranges whose level-2 or level-1 entry is invalid, which makes tearing down the mostly empty 8 MiB stack cheap.
- `for_each_present` wraps it.
- `unmap_range` clears each PTE, decrements `rss`, and calls `page_put` on the frame. It flushes the TLB if the mm is active.

**Demand paging and COW (`uvm_fault(mm, addr, access)`):**

1. No VMA → `-EFAULT`. Access not allowed by `v->prot` → `-EACCES`.
2. `vm_walk(..., alloc=true)` gets the level-0 PTE, creating tables as needed (`-ENOMEM` on failure). `faults++`.
3. PTE not valid: `uvm_populate` maps a zeroed page with the VMA's protections. `rss++`.
4. Write to a `PTE_COW` page:
   - If the frame's `refcount == 1`, this is the last user, so it just sets `W` and clears `COW`.
   - Otherwise it copies into a new page, installs it with the VMA's protections, calls `page_put` on the old frame, and increments `cow_copies`.
5. Write to a present non-writable, non-COW page → `-EACCES`. This should not happen, given step 1.
6. If the mm is active, `sfence.vma va`.

**`mm_fork(parent)`:**

1. Creates a new mm and copies `brk_start`, `brk` and `mmap_next`.
2. For each parent VMA it adds a matching VMA. For each present page, any PTE that is writable or already COW becomes `(pte & ~W) | COW` in the parent. The PTE is copied into the child, the frame gets `page_get`, and the child's `rss++`.
3. Read-only pages (such as text) are shared without `COW`.
4. Flushes the parent's TLB if it is active.
5. On failure it calls `mm_destroy` on the child. The parent's pages stay marked COW and simply regain write access on their next write fault, because their refcount is back to 1.

**Teardown (`mm_destroy`):**

- It panics with "mm_destroy of the active address space" if called on the active mm. `exit` first calls `vm_activate(NULL)`.
- It unmaps and frees every VMA and warns "mm_destroy: N pages still counted resident (accounting bug)" if `rss` is not 0.
- `free_tables` recursively frees the lower-half tables (root entries 0 to 255) and the root.

**Cross-address-space access:**

- `uvm_write(mm, dst, src, n)` faults each destination page in for writing, so it works on an inactive mm and resolves COW. It copies through the direct map.
- `uvm_read(mm, src, dst, n)` only translates and copies. It never allocates, and returns `-EFAULT` for unmapped pages. The profiler and user backtraces (`proc.c`, `profile.c`) use it.

**Activation (`vm_activate(mm)`)** runs on every context switch, plus in `exec` and `exit`.

- It panics if the root page is not a live `PG_PGTABLE` page ("vm_activate: page table … is not a live page table (… freed while still in use?)").
- It re-syncs the kernel half if `kernel_gen` is stale.
- If the table differs from the current `satp`, it writes `satp` and runs `sfence.vma zero, zero`.
- `NULL` selects `kernel_pagetable`, which kernel threads and idle threads use.

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
| `mm->vma_lock` ("vma") | spinlock, irqsave | VMA list *changes* against readers from other processes | Writers are the owning thread only. Readers (`mm_print`, used by `/proc/<pid>/maps` and the monitor) also rely on `proc_lock` to keep the mm alive |

**Lock order** (from the code paths above): `c->lock` → `zone_lock`, and `kpt_lock` → `zone_lock`. `zone_lock` is a leaf. lockdep (`kernel/debug/lockdep.c`) checks the order at run time.

**Interrupt context.** All allocator locks disable interrupts, and nothing in pmm or slab sleeps, so `kmalloc`, `kfree`, `page_alloc` and `page_free` are usable from interrupt handlers. `kstack_free` and `kstack_alloc` may call `tlb_shootdown`, which must not run from an interrupt handler that interrupted a shootdown (comment in `smp.c: tlb_shootdown`).

**User mm.** `uvm.c` takes no lock for page tables or `rss`/`faults`. The design assumes that an mm is only modified by its single owning thread: during system calls, during its own page faults, and in fork/exec/exit run by that thread. Concurrent readers can therefore see a page table mid-update:

- `mm_print` walks page tables while holding `vma_lock`.
- `uvm_read` is used by the profiler and by user backtraces.

These readers only read through the direct map, and page tables are freed only by `mm_destroy`, after `exit` has detached the mm under `proc_lock`. COW refcounts are atomic (`page_get`/`page_put`). The `refcount == 1` test in `uvm_fault` is a plain read, but a stale "2" only causes an extra copy.

**TLB coherence:**

| Kind of change | How other CPUs see it |
|---|---|
| Kernel mappings that are added | Local `sfence.vma` only. Other harts are assumed not to cache invalid entries. Missing *top-level* entries in process tables are fixed lazily in the fault path |
| Kernel mappings that are removed | `tlb_shootdown()`. Today the only callers are `kstack.c: unmap_pages` and the SMP test |
| User page tables | Live on one CPU. Every `satp` switch flushes, and changes to the active mm flush locally (`unmap_range`, `uvm_fault`, `mm_fork`) |

**SUM.** The scheduler clears `SSTATUS_SUM` before every context switch (`sched.c`), so user access cannot leak into another thread. `SUM` is part of the saved `sstatus` in the trap frame, so a copy interrupted by preemption resumes with `SUM` set.

**Counters.** Statistics such as `tlb_shootdowns`, `large_allocs`, `large_frees` and `large_active_bytes` are updated without locks or atomics. They may be slightly off under SMP.

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

`mm_create`, `mm_destroy`, `mm_fork`, `vma_find`, `vma_add`, `vma_unmap`, `vma_protect`, `uvm_peek`, `uvm_poke`, `uvm_fault`, `uvm_populate`, `uvm_write`, `uvm_read`, `vm_activate`, `mm_print`, `vma_kind_name`, `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `user_range_ok`.

### Fault-tolerant access (`safe.h`)

`safe_read_u8/16/32/64(addr, &out)` and `safe_write_u8/16/32/64(addr, val)` return 0, or -1 on a fault. `extable_lookup(pc)` is a linear scan of `__ex_table`.

### System calls built on this subsystem

`brk`, `mmap` (anonymous private only, W^X enforced), `munmap` and `mprotect` (W^X enforced) are in `kernel/proc/syscall.c`. `fork` and `execve` (`kernel/proc/proc.c`) use `mm_fork`, `mm_create`, `uvm_write`, `vm_activate` and `mm_destroy`.

### Boot options

| Option | Effect |
|---|---|
| `nopoison` | Disables free-page poisoning and checking (`pmm.c`). Slab poisoning and redzones are always on |
| `log=mm:trace` | Logs every page allocation with its site (`pr_trace` in `alloc_pages_site`) |
| `log=trap:trace` | Logs extable fixups ("fault at … fixed up -> …") |
| `smp=<n>` | With `smp=1`, `tlb_shootdown` only flushes locally |

### /proc files

| File | Source |
|---|---|
| `/proc/meminfo` | `pmm_print`: RAM, total/reserved/in-use/free pages, alloc/free/failure/corruption counts, free blocks by order, kernel stack counts, reserved regions |
| `/proc/slabinfo` | `slab_print`: per-cache object size, slot size, objects per slab, active, total, slabs, allocs, frees, peak; large-kmalloc totals |
| `/proc/<pid>/maps` | `mm_print`: VMAs with prot, kind, resident pages, shared COW pages; rss, faults, COW copies, brk |

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
| `vm <pid> [va]` | `mm_print` or `vm_walk_print` on that process's table |
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

**GDB** (`tools/gdb/eunuchs.py`) has `eu-vm [va]`, `eu-meminfo`, `eu-page <addr>` and `eu-slab`. They read kernel memory directly, so they work even when the kernel is wedged.

**Tests:**

- `make test-host` runs `tests/host/test_buddy.c`: 3000 pages with a reserved hole, exhaustion, random alloc/free with an ownership map, full coalescing, and error paths.
- The in-kernel tests run with the `ktest` boot option or `ktest [prefix]` in the monitor (for example `ktest pmm`, `ktest kmalloc`, `ktest vm`). Corruption tests use `EXPECT_CORRUPTIONS(n, body)` in `test_mm.c`, which flips `mm_panic_on_corruption` and counts reports.

**Trace events:**

- `user_fault` (group `proc`) records every user page fault: `addr`, `access`, `result`.
- `trace_events.h` also declares `page_alloc`, `page_free`, `kmalloc` and `kfree` in group `mm` (disabled by default: `trace_groups_enabled = TG_ALL & ~TG_MM`). **No code currently emits them**, so enabling the `mm` group records nothing.

**Typical messages and their meaning:**

| Message | Meaning |
|---|---|
| `*** MEMORY CORRUPTION: page use-after-free: block pa … modified after free; bytes +a..+b …; last freed at S` | Someone wrote to a page after `free_pages`/`page_free`. Found at the next allocation of that block, or by `heapcheck` |
| `… double free of page pa …; previously freed at A, allocated at B` | `free_pages` on a block head that is still `PG_FREE` |
| `free_pages of pa … which is reserved memory` / `not the head of its block` / `still has N references` / `order mismatch` | Wrong argument to `free_pages`/`page_free` |
| `page_put: refcount of pa … went negative` | Unbalanced `page_put` |
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
- **No ASIDs.** Every `satp` change flushes the entire TLB, and `tlb_shootdown` always flushes everything rather than a range.

**User memory:**

- **No file-backed or shared mappings** (`do_mmap` accepts only `MAP_ANONYMOUS|MAP_PRIVATE`), **no swapping, no OOM killer.** A failed demand-page allocation sends `SIGBUS` to the faulting process.
- **`mmap` uses a bump pointer.** `mmap_next` only grows, and holes left by `munmap` are never reused.
- **User page-table pages are freed only by `mm_destroy`.** `munmap` leaves empty tables in place.
- **No locking for the user mm.** Correctness depends on processes having exactly one thread. Adding threads would need an mm lock and user TLB shootdowns.
- **VMA operations are linear.** VMAs are a linked list, so lookups and inserts are O(n), and `mm_fork` re-inserts each VMA with `vma_add`.
- **`vma_unmap` can leave a hole on failure.** If `vma_add` for the tail fails while splitting a VMA, the pages are already unmapped and the original VMA has already been trimmed. The tail part of the mapping silently disappears (the error is returned).
- **`vma_protect` is not failure-atomic.** If a split's `vma_add` fails part way through a multi-VMA range, the VMAs already processed keep their new protection.
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
10. **ASIDs and ranged shootdowns** — Use Sv39 ASIDs so that `vm_activate` need not flush the whole TLB on every switch. Let `tlb_shootdown` take a VA range, so `kstack.c: unmap_pages` flushes 16 KiB instead of everything. Impact: medium. Effort: medium.
11. **Make `vma_unmap` failure-atomic** — Allocate the tail VMA for a split *before* unmapping pages or trimming `v`, so `-ENOMEM` leaves the address space unchanged. Impact: low. Effort: small.
12. **Free empty user page tables and reuse `mmap` holes** — In `unmap_range`, free level-0 and level-1 tables that become empty. In `do_mmap`, search the VMA list for a gap instead of only bumping `mmap_next`. Long-running programs that map and unmap repeatedly currently leak address space and table pages until exit. Impact: low. Effort: medium.
13. **Make `ioremap` robust to overlap** — Check for containment or overlap against existing regions, map only the missing pages, and take `kpt_lock` (or a dedicated lock) around the `ioremaps[]` update. Impact: low. Effort: small.
14. **Boot option to slim slab debugging** — For example `noslabdebug`, which drops the metadata and redzones and uses a compact slot layout. This would allow measuring and running without the 48 bytes per object of overhead, as `nopoison` already does for pages. Impact: low. Effort: medium.
15. **Host tests for slab and uvm logic** — Only `buddy.c` is host-tested. Factoring the pure parts of `slab.c` (slot geometry, free-list handling, redzone checks) and of `uvm.c` (VMA list operations, `next_present`) so they can run under ASan would catch bugs like items 1 and 3 earlier. Impact: medium. Effort: medium.
16. **Atomic statistics** — Use `__atomic_add_fetch` (or the relevant lock) for `tlb_shootdowns`, `large_allocs`, `large_frees` and `large_active_bytes`, so that `/proc/slabinfo` and `cpus` show exact numbers under SMP. Impact: low. Effort: small.
17. **Failure-atomic, targeted `mprotect`** — Allocate the split VMAs before changing anything, so `vma_protect` either fully succeeds or leaves the address space untouched; and send the remote `fence.i` only to harts that have run this `mm` since its last `fence.i` (a per-mm CPU mask), instead of every hart through SBI. Impact: low. Effort: small.
