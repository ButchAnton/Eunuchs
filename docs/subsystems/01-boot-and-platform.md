# Boot, platform discovery, console and kernel logging

This document covers everything from the first instruction the kernel executes to the point where `kmain` turns into cpu0's idle thread. That includes the early page table in `boot.S`, the linker layout, the device-tree parser that fills `struct platform_info`, the kernel command line, the SBI wrappers, and the 16550 console. It also covers the logging stack used by the rest of the kernel (`printk` ring, per-subsystem levels, panic reports) and the embedded symbol table, which the two-pass link in the `Makefile` produces.

## Responsibilities and scope

**In scope:**

- Entry from OpenSBI (`_start`), zeroing BSS, the early Sv39 page table, jumping to the high (linked) address.
- Kernel image layout (`kernel.ld`): section order, page alignment for W^X, the guarded boot and overflow stacks, the `.ksyms` section.
- The order of initialization in `kmain`, and the `kinit` thread's choice between tests and `/bin/init`.
- Device tree (DTB) parsing into the global `platform`, and the `dtb`/`plat` printers.
- Kernel command line parsing, the table of known options and their validation.
- The SBI call layer (base, legacy console, TIME, IPI, RFENCE `remote_fence_i` for `mprotect`, HSM, SRST) and the `sifive,test0` exit device.
- The console: the SBI fallback, the polled 16550 transmit path, the UART receive interrupt and `Ctrl-]`.
- Kernel logging: `klog`/`pr_*`, the 64 KiB log ring, thresholds, `quiet`, `log=`.
- `panic` and `panic_trap` reports, and how the machine ends (monitor or exit code).
- The `.ksyms` symbol table: generation, the two-pass link, lookup.
- The boot side of secondary harts: `secondary_entry`, `start_cpu`, `secondary_main`.

**Out of scope (covered elsewhere):**

- The real kernel page table (`vm_init`), `ioremap` internals, the page and slab allocators (memory document). This document only says what they need from boot.
- IPIs, TLB shootdown, stop-the-world internals (SMP document). Panic only *uses* `smp_stop_others`/`smp_park_self`.
- The trap vector and dispatch (`trapentry.S`, `trap.c`), except `trap_init` setting `stvec`.
- The TTY line discipline (`tty.c`). The console only hands received characters to `tty_input`.
- The debug monitor itself (`debug/monitor.c`), backtraces (`backtrace_print`), trace (`trace_print`) and lockdep. This document lists the monitor commands that belong to this subsystem.

## Source map

| File | Role |
|---|---|
| `kernel/arch/riscv/boot.S` | `_start` (boot hart entry), early page table, `secondary_entry`, boot and overflow stacks |
| `kernel/arch/riscv/kernel.ld` | Link address, section order and alignment, linker-provided boundary symbols |
| `kernel/core/main.c` | `kmain` (init order), `kinit` (first kernel thread), `print_sbi_info` |
| `kernel/core/sched.c: cpu_early_init` | First call in `kmain`: fills `cpus[0]` id, hart id, overflow stack |
| `kernel/core/fdt.c` | DTB walker, `fdt_parse` (fills `platform`), `fdt_dump`, `platform_print`; defines `struct platform_info platform` |
| `kernel/core/cmdline.c` | Tokenizes `bootargs`; `cmdline_get/has/raw`; option table and `cmdline_check` |
| `kernel/arch/riscv/platform.c` | `sifive,test0` finisher: `platform_init`, `platform_exit`, `platform_reboot` |
| `kernel/arch/riscv/sbi.c` | `sbi_call` and wrappers for the SBI extensions the kernel uses |
| `kernel/core/console.c` | Console front end: UART or SBI fallback, receive interrupt, `Ctrl-]`, blocking `console_getc` |
| `kernel/drivers/uart16550.c` | NS16550A register access: init, polled TX, RX poll, RX interrupt enable |
| `kernel/core/printk.c` | Log ring, `vklog`, thresholds, `kprintf`, `dmesg_print`, `log=`/`quiet` parsing |
| `kernel/core/panic.c` | `panic`, `panic_trap`, `panic_print`, the panic report and finish |
| `kernel/debug/ksyms.c` | Lookup in the embedded symbol table |
| `tools/gen_ksyms.py` | Generates `ksyms.S` from `nm -n` output |
| `Makefile` (two-pass link rules) | `ksyms0.S` → `kernel.pass1.elf` → `ksyms.S` → `kernel.elf`, plus the "symbols moved" check |
| `kernel/arch/riscv/smp.c: smp_init, start_cpu, secondary_main` | Starting secondary harts via SBI HSM |
| `kernel/include/platform.h` | `struct platform_info`, `struct mmio_dev`, linker symbols |
| `kernel/include/printk.h` | Levels, subsystems (`LOG_SUBSYSTEMS` X-macro), `pr_*` macros, ring API |
| `kernel/include/sbi.h` | SBI extension IDs and wrapper prototypes |
| `kernel/include/console.h` | Console and UART prototypes |
| `kernel/include/debug.h` | `cmdline_*` prototypes, `monitor_enter`, `panic` |
| `kernel/include/percpu.h` | `struct cpu` and the `CPU_*` offsets used by `boot.S` |

## Concepts and data structures

### Address space at boot

The kernel is linked at `KERNEL_BASE + 0x80200000` (`KERNEL_BASE = 0xffffffc000000000`, `kernel.ld` and `kernel/include/mm.h`) and loaded by QEMU/OpenSBI at physical `0x80200000`. `__va(pa)` is `pa + KERNEL_BASE` and `__pa(va)` is `va - KERNEL_BASE` (`mm.h`), so the kernel image sits inside the direct map.

Until paging is on, only PC-relative code may run. `la` in `_start` therefore yields *physical* addresses before the jump and *virtual* ones after it (`boot.S` comments).

### The early page table (`boot.S: early_pagetable`)

One 4 KiB root table in `.bss`, page aligned. It holds only 1 GiB leaf entries (Sv39 level-2 leaves):

| Root index | Maps | PTE bits |
|---|---|---|
| `_start_pa >> 30` (2 for `0x80200000`) | identity: the GiB containing the kernel | `0xcf` = V R W X A D |
| 256 .. 319 (`KERNEL_ROOT_IDX` + `EARLY_GIGAPAGES` = 64) | `KERNEL_BASE + [0, 64 GiB)` → `[0, 64 GiB)` | `0xef` = V R W X G A D |

Everything is RWX. The identity entry only exists so the instructions right after `csrw satp` keep executing; the 64 GiB direct map covers RAM, the DTB and all of QEMU `virt`'s MMIO. `vm_init` later replaces this table with `kernel_pagetable`, but it never modifies or frees `early_pagetable`, which matters because secondary harts use it again (see below).

### Kernel image layout (`kernel.ld`)

```
KERNEL_BASE+0x80200000  __kernel_start
  .text        .text.boot first (_start), then all .text
  ALIGN(4096)  __text_end, __rodata_start
  .rodata      .srodata, .rodata, .ktests (KTEST table), __ex_table
  ALIGN(4096)  __rodata_end, __data_start
  .data        .sdata, .data
  .bss         __bss_start .. __bss_end (.sbss, .bss, COMMON)
  ALIGN(4096)  __data_end
  ALIGN(32768) .stacks (NOLOAD): boot stack slot, overflow stack slot
  ALIGN(4096)  __ksyms_start
  .ksyms       generated symbol table (must stay last)
  ALIGN(4096)  __kernel_end
```

Page alignment of the boundaries lets `vm_init` (`kernel/mm/vmm.c`) map text RX, rodata RO, data+bss RW, `.ksyms` RO, and only the upper half of each stack slot. All of these symbols are declared in `platform.h`.

### Boot and overflow stacks (`boot.S`, `.stacks`)

Each stack occupies a `KSTACK_SLOT` (32 KiB) aligned slot whose lower `KSTACK_SLOT - KSTACK_SIZE` (16 KiB) is a guard and whose upper `KSTACK_SIZE` (16 KiB) is the stack:

| Slot | Guard | Stack | Used by |
|---|---|---|---|
| 0 | 16 KiB | `boot_stack` .. `boot_stack_top` | `kmain`; later the stack of thread `idle/0` (`sched.c: sched_init`) |
| 1 | 16 KiB | `overflow_stack` .. `overflow_stack_top` | cpu0's emergency stack for kernel stack overflows (`cpu_early_init`) |

The guard only works after `vm_init`, which leaves the lower halves unmapped. Under the early page table the whole slot is mapped. `.stacks` is `NOLOAD` and is not zeroed by the BSS loop.

### `struct platform_info` (`platform.h`, defined in `fdt.c`)

The single global `platform` is the kernel's description of the machine. It is filled once, by `fdt_parse` on the boot hart, plus `boot_hart` from `kmain`. After that it is read-only by convention.

| Field | Meaning / source |
|---|---|
| `dtb`, `dtb_size` | Physical address of the DTB and its `totalsize` header field. `pmm_init` reserves this range. |
| `boot_hart` | Hart id OpenSBI entered on (`a0`), set by `kmain`. |
| `ncpus` | Number of nodes with `device_type = "cpu"`. Counts all of them, even beyond the array. |
| `hartids[8]` | First `reg` cell of the first 8 cpu nodes. Size is a literal 8, which matches `MAX_CPUS` (8, `percpu.h`) only by coincidence. |
| `timebase` | `timebase-frequency` (any node; the last one seen wins). Ticks per second of `rdtime`. |
| `mem_base`, `mem_size` | First `reg` entry of the first memory node (`device_type = "memory"` or name `memory@...`). |
| `uart` | First node compatible with `ns16550a` or `ns16550`. |
| `plic`, `clint` | Nodes compatible with `riscv,plic0`/`sifive,plic-1.0.0` and `riscv,clint0`/`sifive,clint0` (last one wins). |
| `test` | `sifive,test0` (QEMU's exit "finisher"). |
| `rtc` | `google,goldfish-rtc`. |
| `virtio[MAX_VIRTIO]`, `nvirtio` | Every `virtio,mmio` node, up to `MAX_VIRTIO` (8), in DTB order. |
| `reserved[8]`, `nreserved` | `/memreserve/` entries and children of `/reserved-memory` (first `reg` entry each). Extra entries are dropped. |
| `model[64]` | Root node's `model`. |
| `bootargs[256]` | `/chosen/bootargs`, truncated to 255 characters. |

`struct mmio_dev` is `{ base, size, irq }`. `irq` is the first cell of the node's `interrupts` property, or 0 if it has none. The parser does not look at `interrupt-parent` or `interrupts-extended`; on QEMU `virt` the single-cell value is the PLIC source number (the UART is 10, checked by `kernel/tests/test_debug.c: device_tree`).

### FDT parser state (`fdt.c`)

- `struct fdt_header`: the 10 big-endian header words. Only `magic`, `totalsize`, `off_dt_struct`, `off_dt_strings`, `off_mem_rsvmap` and `size_dt_struct` are used. `version` is not checked.
- `struct node_state`: per open node: `name`, the `#address-cells`/`#size-cells` it declares for its children (defaults 2 and 1), pointers to its `reg` and `compatible` values, `device_type`, and the first `interrupts` cell. The parser keeps a stack of these (`struct parse_ctx`, `MAX_DEPTH` = 16, a function-local `static`).
- Invariant: a node's `reg` is decoded with its **parent's** cell counts (`reg_first`). This is why a node is classified only at its end token (`finish_node`), when both its own properties and its parent's are known.

### Command line (`cmdline.c`)

- `raw[256]`: the original string (for `cmdline_raw`, the boot log and `plat`).
- `cmdline_buf[256]`: a copy in which whitespace is overwritten with NULs.
- `words[MAX_ARGS]` (32) and `nwords`: pointers to each word. A word is `key` or `key=value`; values cannot contain spaces (no quoting).
- `options[]`: the table of every known option: key, argument syntax, optional `|`-separated allowed values (`choices`), help text. The file comment asks to keep it in sync with `docs/DEBUGGING.md`.

| Option | Choices validated | Consumed by |
|---|---|---|
| `help` | | `cmdline_check` |
| `log=subsys:level,...` | | `printk_apply_cmdline` |
| `quiet` | | `printk_apply_cmdline` |
| `break` | | `kmain` |
| `panic=shutdown` | `shutdown` | `panic.c: panic_finish` |
| `init=path` | | `kinit` |
| `ktest[=prefix]`, `utest[=mode]`, `tracedump=path` | | `kinit` |
| `trace=groups` | | trace |
| `profile[=hz]` | | `profile_boot_option` |
| `nopoison` | | `pmm_init` |
| `smp=n` | | `smp_init` |
| `lockdep=off\|warn\|panic` | `off\|warn\|panic` | lockdep |
| `watchdog=seconds`, `hung=seconds` | | timer, scheduler |
| `fs.crash=point` | `after-log\|after-commit\|mid-install` | `fs/log.c` |
| `ip=...`, `net.drop=percent` | | network |

### Logging (`printk.h`, `printk.c`)

- **Levels** (`enum log_level`): `LOG_ERR` 0, `LOG_WARN` 1, `LOG_INFO` 2, `LOG_DEBUG` 3, `LOG_TRACE` 4. A larger number means more verbose.
- **Subsystems** (`LOG_SUBSYSTEMS` X-macro): `core boot panic trap mm sched proc syscall vfs fs blk net drv debug test`. Each `.c` file defines `LOG_SUBSYS` before including `printk.h`. The X-macro generates both `enum log_subsys` and `log_subsys_names[]`.
- **`log_threshold[SUBSYS_MAX]`**: per-subsystem threshold, `LOG_INFO` initially. A message is recorded only if `level <= log_threshold[subsys]`.
- **`console_level`**: initially `LOG_TRACE` (everything recorded is also printed). `quiet` sets it to `LOG_WARN`.
- **Ring**: `char log_buf[LOG_BUF_SIZE]` (64 KiB) and `volatile u64 log_head`, the total number of bytes ever written. Byte `pos` lives at `log_buf[pos % LOG_BUF_SIZE]`; the valid bytes are `[max(0, head - 64 KiB), head)`. Both are deliberately non-static so that GDB's `eu-dmesg` can read them from a halted kernel. The ring holds formatted text only, with no per-record metadata.
- **Line format** (`vklog`): `[%5lu.%06lu] <cpu> <E|W|I|D|T> <subsys>: <message>\n`. The time is `rdtime()` divided by `platform.timebase` (10 MHz if the timebase is unknown). Lines are capped at 512 bytes including the header, and a missing trailing newline is added.
- **`log_lock`** (spinlock `"log"`): serializes ring writes with console output.

### Panic state (`panic.c`)

- `int panic_depth`: global; non-zero once any CPU has entered `panic_begin`. While it is non-zero `vklog`, `console_write_locked` and `spin_lock_irqsave`'s recursion check change behavior (they skip the lock or the check).
- `static volatile int panic_cpu`: the CPU that owns the panic (CAS from -1). It is never reset.
- `static u32 panic_unstopped`: mask of CPUs that did not park within `smp_stop_others`' timeout.

### Embedded symbol table (`.ksyms`)

Generated assembly (`tools/gen_ksyms.py`) defines four globals in section `.ksyms`:

| Symbol | Type | Content |
|---|---|---|
| `ksyms_num` | `u64` | Number of entries |
| `ksyms_addrs[]` | `u64[]` | Addresses, ascending |
| `ksyms_offs[]` | `u32[]` | Offset of each name in `ksyms_names` |
| `ksyms_names` | `char[]` | NUL-terminated names, back to back |

Only `nm` types `t`, `T` and `W` (text) are kept. Names beginning with `.L`, `$` or `__ksym` are skipped. When several names share an address, the generator keeps one, ranked by: no leading `__` first, then global (`T`) before local (`t`), then alphabetical. Symbol sizes are not recorded.

## How it works

### 1. Boot hart entry (`boot.S: _start`)

OpenSBI enters `_start` in S-mode with the MMU off, at physical `0x80200000`, with `a0` = hart id and `a1` = physical DTB address. Only the boot hart arrives here; the others stay parked in OpenSBI.

1. Disable interrupts: `sie = 0`, clear `sstatus.SIE`. Set `sscratch = 0`; the trap entry reads that as "trap came from the kernel".
2. Save the hart id in `s1` and the DTB address in `s2`.
3. Zero `[__bss_start, __bss_end)` 8 bytes at a time, with physical addresses. This also zeroes `early_pagetable`, which lives in `.bss`.
4. Write the identity gigapage entry: index `_start >> 30`, PTE `(gp << 28) | 0xcf` (PPN = gp << 18, shifted left by 10).
5. Write root entries 256..319 mapping physical gigapages 0..63, PTE flags `0xef` (global).
6. `satp = (8 << 60) | (early_pagetable >> 12)` (Sv39), with `sfence.vma` before and after.
7. Load the absolute address `.Lhigh` from a literal (`.Lhigh_addr`) and `jr` to it. The kernel now runs at its linked address.
8. `tp = cpus` (`&cpus[0]`), `sp = boot_stack_top`, `fp = 0` (terminates frame-pointer backtraces), restore `a0`/`a1`, and `call kmain`. If `kmain` ever returns, the hart loops on `wfi`.

`gp` is never set. This is fine because `kernel.ld` does not define `__global_pointer$`, so the linker does not relax accesses to be gp-relative.

### 2. `kmain` (`kernel/core/main.c`)

The order matters; each step relies on the ones before it.

| Step | Call | Why here |
|---|---|---|
| 1 | `cpu_early_init(hartid)` (`sched.c`) | `cpus[0].id = 0`, `.hartid`, `.overflow_stack_top`; the trap path needs these. |
| 2 | `trap_init()` | `stvec = kernel_trap_entry` (direct mode), so early faults produce a report instead of a silent hang. |
| 3 | `platform.boot_hart = hartid` | |
| 4 | `fdt_parse(dtb)` | Fills `platform`. Errors are remembered (`dt_err`) but reported later, once the console works. |
| 5 | `platform_init()` | `ioremap`s the exit device (only records it; see below). |
| 6 | `console_init()` | Switches output from SBI to the UART if the DTB named one. |
| 7 | `cmdline_init(platform.bootargs)`, `printk_apply_cmdline()` | Thresholds and `quiet` take effect before the first `pr_info`. |
| 8 | Banner `Eunuchs 0.1 (riscv64), built <date> <time>` | |
| 9 | `if (dt_err) panic("cannot parse device tree at %#lx")` | |
| 10 | `print_sbi_info()`, `platform_print(boot_print)` | SBI version, implementation, extensions TIME/IPI/HSM/SRST; the platform table. |
| 11 | Kernel range, size and `ksym_count()`; `command line: "..."` | |
| 12 | `cmdline_check()` | Warns about unknown options and bad values; prints the table for `help`. |
| 13 | `lockdep_init()` | |
| 14 | `break` → `monitor_enter("boot option 'break' (before memory management init)", NULL, true)` | |
| 15 | `pmm_init`, `vm_init`, `slab_init`, `trace_init`, `sched_init`, `proc_init`, `plic_init`, `tty_init` | Other subsystems. `vm_init` switches to the real page table; `sched_init` turns the boot context into thread `idle/0` on `boot_stack`. |
| 16 | `console_enable_irq()` | Needs `plic_init` (to register the IRQ) and `tty_init` (to receive input). |
| 17 | `timer_init()`, `smp_init()`, `profile_boot_option()` | |
| 18 | `kthread_create("kinit", kinit, NULL)`, `kthread_detach` | Panics if creation fails. |
| 19 | `cpu_idle_loop()` | Never returns. |

`boot_print` is a small `print_fn` adapter that logs at `LOG_INFO` under `SUBSYS_BOOT`; `platform_print` takes a printf-like function so the same code serves the boot log, the monitor (`kprintf`) and others.

**Before `vm_init`** everything is reached through the early page table. `ioremap` (`vmm.c`) called from `platform_init` and `uart_init` only records the region in its table and returns `__va(pa)`, which already works through the 64 GiB direct map. `vm_init` then maps every recorded region into `kernel_pagetable`. Any MMIO touched before `vm_init` therefore must go through `ioremap`, or it will become unmapped when the real table is installed.

**Before `console_init`** `console_putc` falls back to `sbi_console_putchar` (legacy SBI extension 0x01). In practice nothing in `kmain` prints before step 6. Even a device-tree error is only reported at step 9, and if the DTB named no UART, all output keeps going through SBI.

### 3. `kinit` (first kernel thread)

1. `blk_init()`; if it fails: `panic("no disk: the root filesystem lives on a virtio block device (make run attaches build/fs.img)")`.
2. `vfs_init()`, `net_init()`.
3. If `ktest[=prefix]`: `ktest_run(prefix)`. If `utest[=arg]`: spawn `/bin/init -1 /bin/utests [arg]` and wait for it with `proc_wait_kernel`.
4. If either ran: optionally `trace_dump_file(tracedump)` through semihosting, then `platform_exit(failed ? 1 : 0)`.
5. Otherwise spawn `init=` (default `/bin/init`) after logging `boot complete; starting ... (Ctrl-] enters the debug monitor)`, or panic `cannot start %s`.

### 4. Device-tree parsing (`fdt.c`)

`fdt_parse(dtb)`:

1. Reject `dtb == 0` or a bad magic (`0xd00dfeed`) by returning -1.
2. Record `platform.dtb` and `platform.dtb_size = totalsize`.
3. Walk the `/memreserve/` block: 16-byte big-endian `(address, size)` pairs until a zero pair, each passed to `add_reserved`.
4. `fdt_walk(dtb, parse_visit, &ctx)`.

`fdt_walk` is a generic token walker over the structure block, `[off_dt_struct, off_dt_struct + size_dt_struct)`:

- `FDT_BEGIN_NODE`: the name follows inline (padded to 4 bytes). Depth is incremented; at `MAX_DEPTH` (16) the walk fails. Calls `visit(depth, name, NULL, ...)`.
- `FDT_PROP`: `len`, `nameoff`, then the value padded to 4. Calls `visit(depth, node, strings + nameoff, val, len)`.
- `FDT_END_NODE`: calls `visit(-depth - 1, ...)` and decrements depth.
- `FDT_NOP` is skipped; `FDT_END` ends the walk; any other token fails.

`parse_visit` maintains the `node_state` stack. On node begin it resets the state and applies the spec defaults (`#address-cells` 2, `#size-cells` 1). For each property it records `#address-cells`, `#size-cells`, `reg`, `compatible`, `device_type` and the first `interrupts` cell. Three properties write straight into `platform`: `timebase-frequency` from any node, `bootargs` but only in a node named `chosen`, and `model` but only at depth 0. On node end (depth > 0) it calls `finish_node(node, parent)`, which:

1. Decodes the node's first `reg` entry with the parent's cell sizes (`reg_first`).
2. If the parent is named exactly `reserved-memory`, adds a reserved region and stops.
3. A memory node (by `device_type` or by the `memory@` name prefix) sets `mem_base`/`mem_size` if not yet set.
4. A cpu node appends its `reg` to `hartids` (if there is room) and always increments `ncpus`.
5. Nodes with both `compatible` and `reg` are matched against the compatible strings in the table above (`compat_has` scans the NUL-separated list).

`fdt_dump` reuses `fdt_walk` with `dump_visit` to print the tree in a DTS-like form: string lists when the value looks like printable NUL-separated strings, `<cells>` when the length is a multiple of 4, and `[bytes]` otherwise. `platform_print` prints the `platform` fields.

### 5. Command line (`cmdline.c`)

- `cmdline_init(bootargs)` copies into `raw` and `cmdline_buf`, then splits `cmdline_buf` on whitespace (`kisspace`) into at most 32 words. Words past 32 are dropped without a warning.
- `cmdline_get(key)` returns, for the **first** word that equals `key` or starts with `key=`: `""` for a bare flag, the text after `=` otherwise, or `NULL` if no word matches. Prefixes do not match: `cmdline_has("pan")` is false for `panic=shutdown` (tested in `test_debug.c: cmdline`).
- `cmdline_check()` looks every word's key (truncated to 31 characters) up in `options[]`. Unknown keys produce `unknown boot option '%s' ignored (boot with 'help' for the list)`. For options with `choices`, a value not in the list produces `boot option %s: '%s' is not one of %s`. Values of options without `choices` (numbers, paths) are not checked here. With `help`, the whole table is printed at `LOG_INFO`.

The consumers parse their own values. For example `smp_init` uses `kstrtoul`, so `smp=abc` becomes 0, which means uniprocessor.

### 6. SBI layer (`sbi.c`)

`sbi_call(ext, fid, a0, a1, a2)` puts `ext` in `a7` and `fid` in `a6`, executes `ecall`, and returns `{ error = a0, value = a1 }`. Only three argument registers are passed, which is enough for every call used here.

| Wrapper | Extension / function | Used by |
|---|---|---|
| `sbi_console_putchar`, `sbi_console_getchar` | legacy 0x01 / 0x02 (the result is in `a0`, returned as `.error`) | console before `console_init`, or if the DTB has no UART |
| `sbi_spec_version`, `sbi_impl_id`, `sbi_impl_version`, `sbi_probe` | BASE (0x10), fid 0–3 | `print_sbi_info`, `smp_init` (HSM probe) |
| `sbi_impl_name` | table: BBL, OpenSBI, Xvisor, KVM, RustSBI, Diosix, Coffer | `print_sbi_info` |
| `sbi_set_timer` | TIME fid 0 | `timer.c`, `idle.c: cpu_wait_ticks` |
| `sbi_send_ipi(mask, base)` | IPI (`"sPI"`) fid 0 | `smp.c: smp_send_ipi` (always `mask = 1, base = hartid`) |
| `sbi_hart_start`, `sbi_hart_status` | HSM fid 0 and 2 | `start_cpu` |
| `sbi_shutdown(failure)`, `sbi_reboot()` | SRST fid 0 (type 0 shutdown with reason 0/1; type 1 cold reboot) | `platform_exit`, `platform_reboot` |

Both SRST wrappers fall into a `wfi` loop if the call returns.

### 7. Ending the machine (`platform.c`)

`platform_init` `ioremap`s the `sifive,test0` device (size from the DTB, default `0x1000`). `platform_exit(code)` writes `FINISHER_PASS` (`0x5555`) for code 0, otherwise `(code << 16) | FINISHER_FAIL` (`0x3333`); QEMU then exits with `code` as its exit status. If the device is missing, or QEMU ignores the write, it falls back to `sbi_shutdown(code != 0)`. `platform_reboot` writes `FINISHER_RESET` (`0x7777`), then calls `sbi_reboot`.

Exit codes used by the kernel:

| Code | Meaning | Where |
|---|---|---|
| 0 | tests passed / `halt` | `kinit`, monitor `halt` |
| 1 | some test failed | `kinit` |
| 2 | panic (`panic=shutdown`, or after leaving the post-mortem monitor) | `panic.c: panic_finish` |
| 3 | recursive panic; also `fs.crash=` simulated power failure | `panic.c: panic_begin`, `fs/log.c: crash_point` |
| 4 | stack overflow while handling a stack overflow | `trap.c: stack_overflow` |

### 8. Console (`console.c`, `uart16550.c`)

**Output** is always synchronous and polled. `console_putc` turns `\n` into `\r\n`. If the UART is ready it calls `uart_putc`, which spins on `LSR.THRE` and writes `THR`; otherwise it uses `sbi_console_putchar`. Polling keeps printk and panic output working in any context, including with interrupts off.

**UART init** (`uart_init`): `ioremap(base, 8)`; `IER = 0`; set `DLAB`, divisor 3 ("38400 baud; QEMU ignores it"); `LCR = 8N1`; `FCR = 0x07` (enable and clear FIFOs). Receive interrupts stay off until `console_enable_irq`.

**Input** has two paths:

- Interrupt-driven, for normal operation. `console_enable_irq` registers `uart_irq` for `platform.uart.irq` with `irq_register` and, on success, calls `uart_enable_rx_irq` (`MCR.OUT2`, `IER.RX`). `uart_irq` drains the FIFO; each character goes through `console_rx_char`, which sends `Ctrl-]` (`0x1d`) to `monitor_enter("console break (Ctrl-])", this_cpu()->irq_tf, true)` and everything else to `tty_input`. PLIC interrupts all go to cpu0 (see ARCHITECTURE.md), so this runs on cpu0 in interrupt context.
- Polled, for the monitor. `console_getc_nowait` reads `LSR.DR`/`RBR` (or SBI legacy getchar). `console_getc` blocks by repeatedly calling `console_getc_nowait` and sleeping about 10 ms in between with `cpu_wait_ticks(timebase / 100)`. It does not spin, because every MMIO read is a QEMU exit that takes QEMU's global lock.

`console_write_locked` (used by `tty.c` for user output) takes `log_lock`, so user output and kernel log lines do not interleave mid-line.

### 9. Logging path (`printk.c`)

```
pr_info(...)                     macro: if (LOG_INFO <= log_threshold[LOG_SUBSYS]) klog(...)
  klog -> vklog(level, subsys, fmt, ap)
     re-check threshold
     format "[sec.usec] cpu L subsys: " + message into line[512] (on the stack)
     clamp to 510 bytes; append '\n' if missing
     if panic_depth == 0: spin_lock_irqsave(&log_lock)
     log_ring_write(line, n)           ring append, log_head += n
     if level <= console_level: console_write(line, n)   (polled UART)
     unlock
```

Notes:

- The threshold check happens in the macro too, so disabled `pr_debug` calls cost one load and compare, and their arguments are not evaluated.
- `kprintf` formats straight to `console_putc`, bypassing the ring, the thresholds and `log_lock`. The monitor uses it with the world stopped.
- `log_err_print`, `log_warn_print` (subsystem `debug`) and `panic_print` (subsystem `panic`, level `err`) are `print_fn` adapters for multi-line reports such as backtraces.
- Readers: `dmesg_print(last_lines)` prints the ring to the console, skipping the partial oldest line after wraparound and optionally only the last N lines. `log_ring_copy(buf, size)` copies the valid part oldest-first; `/dev/kmsg` (`fs/devfs.c: kmsg_open`) uses it to take a 64 KiB snapshot at open time. Neither takes `log_lock`.
- `printk_apply_cmdline` handles `quiet` (`console_level = LOG_WARN`) and `log=` entries (`subsys:level` or `all:level`, comma-separated, applied left to right). It copies the spec into a 128-byte buffer. A missing colon or a bad level gives `log=: bad entry '%s'`; an unknown subsystem gives `log=: unknown subsystem '%s'`. Levels can be names or single digits (`log_level_lookup`).

### 10. Panic (`panic.c`)

`panic(fmt, ...)` and `panic_trap(tf)` share the same skeleton:

1. **`panic_begin(tf)`**: interrupts off. CAS `panic_cpu` from -1 to this CPU. If another CPU already owns the panic, this CPU parks forever in `smp_park_self`; the first report will show where it is. `++panic_depth`; at depth 3 it prints `*** recursive panic; halting ***` with `kprintf` and calls `platform_exit(3)`. At depth 1 it stops the other CPUs with `smp_stop_others(&panic_unstopped)`; if the monitor on another CPU already owns the stopped world, it parks and retries until that CPU releases it.
2. **Header**: `KERNEL PANIC on cpuN (hart H): <msg>` (the message is formatted into a 256-byte buffer). For `panic_trap`, the header names the cause (`trap_cause_name`), the mode, and the symbolized `sepc`. It also decodes the trap: the instruction bits for illegal instructions; for page and access faults, the faulting address, `vm_fault_hint` ("likely cause") and a page-table walk (`vm_walk_print`); `stval` for anything else. Then `trap_dump(tf)`.
3. **Backtrace**: `panic` starts at its caller (pc = return address − 2, fp read with `safe_read_u64` from `current_fp() - 16`), because `panic` is `noreturn` and its return address may lie past the caller's end. `panic_trap` starts at `sepc` and the frame's fp.
4. **`panic_context`**: current thread and whether in interrupt context, held spinlocks, and at depth 1 every other CPU (its thread, held locks, and either a backtrace from where it parked or `did not stop: interrupts off and not spinning on a lock`), followed by the last 16 trace events.
5. **`panic_finish`**: with `panic=shutdown`, `platform_exit(2)`. Otherwise `monitor_enter("panic", tf, false)` (continuing is refused), and if the monitor returns, print `halting.` and `platform_exit(2)`.

All of the report goes through `vklog`, so it lands in the ring as well (`dmesg`, `eu-dmesg`). Because `panic_depth` is already non-zero, `vklog` skips `log_lock`; a panic raised while this CPU held `log_lock` (for example a fault in console output) does not deadlock.

### 11. Symbol table and the two-pass link (`Makefile`, `gen_ksyms.py`, `ksyms.c`)

1. `build/ksyms0.S` = `gen_ksyms.py < /dev/null`: an empty table (`ksyms_num = 0`) that still defines all four symbols.
2. `build/kernel.pass1.elf` = all kernel objects + `ksyms0.o`.
3. `build/ksyms.S` = `riscv64-elf-nm -n kernel.pass1.elf | gen_ksyms.py`.
4. `build/kernel.elf` = the same objects + `ksyms.o`.
5. Check: the `t`/`T` lines of `nm -n` for both passes must be identical, otherwise the build fails with `error: text symbols moved between link passes`.

This works because `.ksyms` is the last section in `kernel.ld`: growing it from empty to full only moves `__kernel_end`, not any code or data address. The check enforces that.

Lookups:

- `ksym_lookup(addr, &off)`: binary search for the largest `ksyms_addrs[i] <= addr`. Returns NULL if the table is empty, `addr` is below the first symbol, or `addr >= __text_end`. Because sizes are unknown, any text address resolves to the nearest preceding symbol.
- `ksym_format(buf, size, addr)`: `name`, `name+0xoff`, or the raw hex address.
- `ksym_find(name)`: linear scan; returns the first match in address order, or 0.
- `ksym_count()`: shown in the boot log and by `plat`.

### 12. Secondary harts (boot side)

`smp_init` (`smp.c`, called late in `kmain`):

1. Enable `SIE_SSIE` on the boot CPU and mark `cpus[0]` online.
2. `max = platform.ncpus`, overridden by `smp=` (`kstrtoul`), capped at `MAX_CPUS` (8). If `max <= 1` or the DTB lists one hart, log `smp: uniprocessor (...)` and return.
3. If `sbi_probe(SBI_EXT_HSM)` fails, log a warning and stay on the boot hart.
4. For each `platform.hartids[i]` other than `boot_hart`, call `start_cpu(id++, hart)` until `max` CPUs exist. Stop at the first failure.
5. Log `smp: N of M harts online (boot hart H is cpu0)`.

Kernel CPU numbers are therefore dense (0 .. n−1), and cpu0 is always the boot hart, which can be any hart id (OpenSBI picks it at random).

`start_cpu(id, hartid)`:

1. Allocate an overflow stack (`kstack_alloc`) and an idle thread (`sched_create_idle(id)`).
2. Fill `cpus[id]`: `id`, `hartid`, `overflow_stack_top`, `idle` and `cur_thread`, `kstack_top` (= the top of the idle thread's stack), `last_tick`.
3. `sbi_hart_start(hartid, __pa(secondary_entry), (unsigned long)&cpus[id])`.
4. Spin up to 2000 ms (`ms_ticks(2000)`) for `cpus[id].online`.

`boot.S: secondary_entry` (MMU off, `a0` = hart id, `a1` = the `struct cpu *`, a kernel VA):

1. Interrupts off, `sscratch = 0`.
2. `satp` = the **early** page table (its physical address via PC-relative `la`), so the identity gigapage keeps the next instructions mapped.
3. Jump high (`.Lsecondary_high_addr`).
4. Load `kernel_pagetable` (a direct-map VA), subtract `KERNEL_BASE`, and switch `satp` to it with `sfence.vma`. This is needed because the idle stack lives in `KSTACK_AREA`, which only `kernel_pagetable` maps.
5. `tp = a1`, `sp = CPU_KSTACK_TOP(tp)` (offset 32, kept in sync with `struct cpu` by static asserts in `sched.c`), `fp = 0`, `call secondary_main(tp)`.

`secondary_main(c)`: `trap_init`, enable `SIE_SSIE`, `sfence_vma_all`, `timer_init_cpu`, set `last_switch`, atomically increment `ncpus_online`, release-store `c->online = true`, log `cpuN online (hart H, idle thread T)`, and enter `cpu_idle_loop`.

## Concurrency and locking

- **Single-threaded phases.** Everything in `kmain` before `smp_init` runs on one hart with interrupts off (`_start` clears `sie` and `sstatus.SIE`). `platform`, the command line tables and the early page table are written only in this phase and are treated as read-only afterwards, with no lock.
- **`log_lock`** (spinlock, `printk.c`): protects `log_buf`/`log_head` updates and serializes console output from `vklog` and `console_write_locked`. It is taken with `spin_lock_irqsave`, so the whole polled UART write of a line happens with interrupts off on that CPU, and other CPUs that log spin (servicing TLB-flush and stop IPIs through `ipi_poll`). It is a leaf lock: nothing is acquired under it except the UART registers.
- **Bypass during panic.** Once `panic_depth > 0`, `vklog` and `console_write_locked` do not take `log_lock`. The other CPUs are normally parked by then, so this is safe in practice. Note that `panic_depth` is a global, not per-CPU: a CPU that failed to stop also skips the lock.
- **Unlocked readers.** `dmesg_print`, `log_ring_copy` (`/dev/kmsg`) and GDB's `eu-dmesg` read `log_head` and the ring without `log_lock`. A concurrent writer can overwrite the oldest bytes while they are being copied; the result may have a damaged first line.
- **`kprintf`** is not serialized with anything. It is intended for the monitor and last-resort messages, when the other CPUs are stopped.
- **UART registers** have no lock of their own. TX is serialized by `log_lock` for log and user output. RX is read in `uart_irq` on cpu0, or by the monitor with interrupts off and the world stopped.
- **Interrupt context.** `pr_*` may be called from interrupt handlers (the lock is irqsave). `console_rx_char` runs in interrupt context and may enter the monitor from there.
- **Secondary bring-up.** The boot CPU writes `cpus[id]` before `sbi_hart_start`. The SBI call (an M-mode trap) and the new hart's start order those writes before the new hart reads them. The new hart publishes itself with `__atomic_add_fetch(&ncpus_online)` and then a release store of `online`, which `start_cpu` reads with an acquire load.
- **Panic ownership** is decided by an atomic CAS on `panic_cpu`; stop-the-world ownership by `smp_stop_others` (SMP document).

## Interfaces

### Functions for other subsystems

| Function | Header | Notes |
|---|---|---|
| `extern struct platform_info platform` | `platform.h` | Hardware description; read-only after boot |
| `fdt_dump(print)`, `platform_print(print)` | `platform.h` | Printers for any `print_fn` |
| `platform_exit(code)`, `platform_reboot()` | `platform.h` | `noreturn` |
| `__kernel_start`, `__text_end`, ... , `__kernel_end`, `boot_stack`, `overflow_stack_top` | `platform.h` | Linker symbols |
| `cmdline_get(key)`, `cmdline_has(key)`, `cmdline_raw()` | `debug.h` | `""` for bare flags, `NULL` if absent |
| `pr_err/warn/info/debug/trace(...)` | `printk.h` | Need `#define LOG_SUBSYS SUBSYS_xxx` first |
| `klog`, `vklog` | `printk.h` | Explicit level and subsystem |
| `log_err_print`, `log_warn_print`, `panic_print` | `printk.h`, `debug.h` | `print_fn` adapters |
| `kprintf` | `printk.h` | Unlogged, unlocked console output |
| `log_threshold[]`, `console_level` | `printk.h` | Writable at run time (monitor `log`, GDB `eu-log`) |
| `log_ring_head`, `log_ring_copy`, `dmesg_print`, `log_ring_write` | `printk.h` | |
| `log_subsys_lookup`, `log_level_lookup` | `printk.h` | |
| `console_putc`, `console_write`, `console_write_locked` | `console.h` | |
| `console_getc`, `console_getc_nowait` | `console.h` | Polled input (monitor) |
| `console_rx_char` | `console.h` | Feed a received character (UART IRQ) |
| `sbi_set_timer`, `sbi_send_ipi`, `sbi_hart_start`, `sbi_hart_status`, `sbi_probe` | `sbi.h` | |
| `panic(fmt, ...)`, `panic_trap(tf)`, `panic_depth` | `debug.h` | |
| `ksym_lookup`, `ksym_format`, `ksym_find`, `ksym_count` | `debug.h` | Text symbols only |

### Boot options

See the table under "Command line". The ones handled in this subsystem are `help`, `log=`, `quiet`, `break`, `panic=shutdown`, `init=`, `ktest`, `utest`, `tracedump=` and `smp=`. Pass them with `make run APPEND="..."`; QEMU puts them in `/chosen/bootargs`.

### Files

- `/dev/kmsg`: a snapshot of the log ring, taken at open (`fs/devfs.c`). The user `dmesg` command (an `eutils` name) reads it.
- `/proc/uptime` uses `platform.timebase` (`fs/procfs.c: gen_uptime`).

### Monitor commands (`debug/monitor.c`)

| Command | Effect |
|---|---|
| `dmesg [lines]` | `dmesg_print` |
| `log [<subsys\|all\|console> <level>]` | Show or set thresholds and `console_level` |
| `plat` | `platform_print`, the command line, the kernel range and the symbol count |
| `dtb` | `fdt_dump` |
| `sym <addr\|name>` | `ksym_format` / `ksym_find` |
| `reboot`, `halt [code]` | `platform_reboot`, `platform_exit` |

### GDB (`tools/gdb/eunuchs.py`)

`eu-dmesg [N]` (reads `log_buf`/`log_head` directly), `eu-log [subsys level]`, `eu-platform`.

## Debugging and observability

**Boot log.** A normal boot prints, in order: the banner; SBI version and extensions; the platform table (`model`, `harts`, `memory`, `timebase`, `dtb`, `uart`, `plic`, `clint`, `rtc`, `test`, `virtio0..7`, `reserved`); the kernel range and symbol count; the command line; then the other subsystems; `cpuN online` per secondary hart; `smp: N of M harts online`; and `boot complete; starting /bin/init`. Every line has the form `[sec.usec] cpu level subsys:`. Use `log=boot:debug` or `log=all:debug` for more, and `quiet` to keep the console to warnings.

**Inspecting the platform.** `plat` and `dtb` in the monitor; `eu-platform` in GDB. `boot with help` lists the options. `make qemu-log` records QEMU's trap and guest-error trace in `build/qemu.log`, which helps when the kernel dies before the console works.

**Stopping early.** `break` enters the monitor right after the command line is processed, before `pmm_init`. Use `make debug` + `make gdb` to stop at `_start` itself: set a breakpoint on the *physical* address `0x80200000` for code before the jump (GDB's symbols are the high addresses), or on `kmain`.

**Logs that survive a hang.** `eu-dmesg` reads the ring from a wedged kernel. After a panic, `dmesg` in the monitor shows the full report again.

**Symbols.** If the boot log says `0 symbols`, the image is a pass-1 link or the table is empty, and backtraces will print raw addresses. The build fails with `error: text symbols moved between link passes` if something placed after `.ksyms`, or a size-dependent layout, changed code addresses; keep `.ksyms` last in `kernel.ld`.

**Typical messages:**

| Message | Meaning |
|---|---|
| `KERNEL PANIC ...: cannot parse device tree at 0x...` | Bad DTB pointer or magic, nesting deeper than 16, or an unknown structure token. |
| `unknown boot option 'x' ignored` | Typo, or an option missing from `cmdline.c: options[]`. |
| `boot option K: 'v' is not one of ...` | A value outside `choices`. |
| `log=: bad entry '...'` / `log=: unknown subsystem '...'` | Malformed `log=` item (each needs `subsys:level`). |
| `smp: uniprocessor (...)` | One hart in the DTB, or `smp=1`/`smp=0`/an unparsable `smp=` value. |
| `smp: firmware has no HSM extension; running on the boot hart only` | SBI lacks HSM. |
| `cpuN: SBI hart_start(hart H) failed: error E (hart status S)` | HSM refused (for example the hart is not stopped). |
| `cpuN (hart H) did not come online (hart status S)` | The hart started but did not reach `secondary_main` within 2 s; suspect `secondary_entry` or the page tables. |
| `cpuN: out of memory for its stacks` | `kstack_alloc` or `sched_create_idle` failed. |
| `ioremap: too many regions` | More than 32 `ioremap` calls (`vmm.c: MAX_IOREMAP`). |
| `no disk: the root filesystem lives on a virtio block device ...` | QEMU started without the disk. |
| `*** recursive panic; halting ***` | A panic inside the panic path, twice; QEMU exits with 3. |
| `did not stop: interrupts off and not spinning on a lock` | In a panic report: that CPU ignored the stop IPI. |

**Tests.** `kernel/tests/test_debug.c` covers this subsystem: `ksyms_lookup`, `device_tree` (QEMU `virt` addresses: RAM at `0x80000000`, UART at `0x10000000` IRQ 10, timebase 10 MHz, 8 virtio slots), `cmdline` (expects `make test`'s `ktest panic=shutdown`), and `log_ring_and_thresholds`.

## Limitations and known issues

- **The FDT parser trusts the blob.** `fdt_walk` bounds the walk by `size_dt_struct` but does not check that names, property values, string offsets (`nameoff`) or the structure block lie within `totalsize`. The `/memreserve/` loop in `fdt_parse` has no bound at all. `version` is not checked. A corrupt DTB can make the kernel read wild memory (through the 64 GiB RWX early map) before the console works.
- **Simplified DT semantics.**
  - Only the first `reg` entry of each node is used, so multiple memory banks or multi-range devices are ignored, and only the first memory node counts.
  - `status = "disabled"` is ignored, so a disabled cpu node would still be counted and started.
  - `interrupts` is read as a single cell, regardless of `interrupt-parent` and `#interrupt-cells`.
  - `timebase-frequency` is taken from whichever node has it last.
  - The UART is first-wins but PLIC/CLINT are last-wins.
  - `/reserved-memory` children without `reg` (dynamic allocations) are ignored, and `no-map` is not distinguished from reusable regions.
- **Fixed-size tables that drop entries silently:** `platform.reserved[8]`, `platform.hartids[8]`, `platform.virtio[8]`, `model[64]`, `bootargs[256]`, the 32-word command line, a 31-character key in `cmdline_check`, and the 128-byte `log=` buffer. Only `ioremap` panics on overflow.
- **Weak option validation.** Only options with `choices` are value-checked. Numeric options parse with `kstrtoul` and treat garbage as 0 (for example `smp=x` silently becomes uniprocessor). Duplicate keys: the first wins, with no warning.
- **Logging holds a spinlock with interrupts off during polled UART output.** A 500-byte line is written byte by byte under `log_lock`. Heavy logging (`log=all:trace`, `log=net:debug`) therefore adds interrupt latency and makes other CPUs spin.
- **The ring is plain text.** There are no per-record level, subsystem or sequence fields, so `/dev/kmsg` cannot filter and readers cannot detect what they missed. `/dev/kmsg` is a one-shot snapshot, not a stream.
- **Unlocked ring readers** can return a damaged first line under concurrent logging. `kprintf` output can interleave with log lines if used outside the monitor.
- **Inconsistent timebase fallbacks.** `vklog`, `console_getc` and `smp.c: ms_ticks` assume 10 MHz when `platform.timebase` is 0, but `timer.c: timer_init` uses `platform.timebase` directly (interval 0).
- **Early boot output** before `console_init` uses the SBI *legacy* console, which newer SBI implementations may omit (SBI v2 replaces it with DBCN). Only a DTB without a UART relies on it after boot.
- **The early page table** maps 64 GiB RWX until `vm_init`, so W^X, NULL and stack-guard protection are not in force during the early part of `kmain`, including the `break` monitor.
- **Symbol table.** Text symbols only (`ksym_lookup` rejects addresses at or above `__text_end`). There are no sizes, so an address in padding or past a function's end is attributed to the previous function. Duplicate static names are ambiguous for `ksym_find`, which is also a linear scan.
- **`sbi_shutdown`/`sbi_reboot` hang in `wfi`** if SRST is missing and the finisher did not end the machine; there is no fallback and no message.
- **Secondary bring-up failure handling.** If `start_cpu` fails, the stacks allocated for it are not freed. A hart that times out (2 s) but then starts anyway still increments `ncpus_online` and runs on `cpus[id]` without the boot CPU knowing.
- **Stale comments and docs.**
  - `console.c: console_getc` says "Phase 3 replaces this with a UART receive interrupt"; the IRQ exists, and `console_getc` is now the monitor's polled path.
  - `uart16550.c` says received characters go to "the console ring"; they go to `tty_input`.
  - `ksyms.c` says `.ksyms` is "placed after .bss"; it is after `.stacks`.
  - `debug/monitor.c` (header comment) and `docs/DEBUGGING.md` say the monitor is entered "at the end of boot". `kmain`/`kinit` do not do that.
  - `uart_disable_rx_irq` has no caller.

## Potential improvements

1. **Bounds-check the device-tree parser** — Validate `totalsize`, `version`, and that the structure block, the strings block, every name, `nameoff` and property value, and the `/memreserve/` table lie inside the blob; cap the reserve loop. This turns a corrupt or truncated DTB into a clean `cannot parse device tree` instead of wild reads. Impact: medium. Effort: small.
2. **Honor `status`, all `reg` entries and all memory nodes** — Skip `status = "disabled"` nodes (important for cpu nodes, which `smp_init` would try to start), and record every memory bank, so `pmm_init` can use discontiguous RAM. Impact: medium. Effort: medium.
3. **Warn when fixed tables overflow** — `add_reserved`, `hartids`, `virtio`, the 32-word command line, `bootargs` truncation and the 128-byte `log=` copy all drop data silently. A `pr_warn` at each spot costs little, and a dropped reserved region can mean memory corruption. Impact: medium. Effort: small.
4. **Size `platform.hartids` by `MAX_CPUS`** — Replace the literal 8 in `platform.h` with `MAX_CPUS` (or a shared constant) so raising `MAX_CPUS` cannot silently limit `smp_init`. Impact: low. Effort: small.
5. **Validate numeric boot options** — Extend `options[]` with a "number" kind (and a range) checked in `cmdline_check`, and warn on duplicate keys. `smp=x`, `watchdog=10s` and similar typos currently become 0. Impact: low. Effort: small.
6. **Stop holding `log_lock` across polled UART output** — Write the line to the ring under the lock, then emit the console output under a separate console lock (or from a per-line sequence number), so other CPUs logging are not blocked for the whole UART write with interrupts off. Impact: medium. Effort: medium.
7. **Structured log records with sequence numbers** — Store level, subsystem, CPU and a sequence number per record, and make `/dev/kmsg` a streaming reader that can block for new messages and filter by level or subsystem. This also lets readers detect overwritten data instead of printing a damaged line. Impact: medium. Effort: medium.
8. **Consistent reads of the ring** — Have `dmesg_print` and `log_ring_copy` read `log_head` before and after copying and drop bytes that were overwritten meanwhile (or take `log_lock` for the copy into a buffer). Impact: low. Effort: small.
9. **Record symbol sizes in `.ksyms`** — Emit `nm -S` sizes in `gen_ksyms.py` and have `ksym_lookup` return NULL (or `name+off?`) past the end of a function. Optionally include data symbols, so `sym` and fault reports can name globals. Impact: medium. Effort: small.
10. **Faster, unambiguous `ksym_find`** — Add a name-sorted index (generated by `gen_ksyms.py`) for binary search, and report when a name is ambiguous (duplicate statics) instead of returning the first. Impact: low. Effort: small.
11. **Robust machine exit** — Probe SRST once at boot; in `sbi_shutdown`/`sbi_reboot`, print a message and fall back to the legacy shutdown extension when SRST is missing, instead of an endless `wfi`. Impact: low. Effort: small.
12. **Use SBI DBCN for the pre-UART console** — When `sbi_probe` finds the Debug Console extension, use it instead of the legacy putchar/getchar, which SBI v2 deprecates. Impact: low. Effort: small.
13. **Harden secondary bring-up** — Free the stacks if `start_cpu` fails. Add a per-CPU "start abandoned" flag that `secondary_main` checks before incrementing `ncpus_online`, so a hart that starts after the 2 s timeout parks instead of joining silently. Impact: low. Effort: small.
14. **One timebase helper** — Replace the four copies of `platform.timebase ? platform.timebase : 10000000` (`printk.c`, `console.c`, `smp.c`, `sched.c`) and the unguarded use in `timer_init` with one function, and panic early with a clear message if the DTB lacks `timebase-frequency`. Impact: low. Effort: small.
15. **Fix stale comments, docs and dead code** — Update `console.c: console_getc`, the `uart16550.c` header, the `ksyms.c` header, the monitor header and `DEBUGGING.md` ("at the end of boot"). Remove or use `uart_disable_rx_irq` (for example, while the monitor polls the UART). Replace the `__DATE__`/`__TIME__` banner with a build id, so identical sources give identical images. Impact: low. Effort: small.
