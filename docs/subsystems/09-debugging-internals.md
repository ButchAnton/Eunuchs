# Debugging infrastructure internals

This document explains how Eunuchs' debugging tools are built: the kdb-style monitor, the per-CPU trace ring and its Perfetto export, the sampling profiler, the embedded symbol table, the frame-pointer unwinder, fault-tolerant memory access, the panic path, the log ring, the `KTEST` framework, the GDB helpers and QEMU record/replay. [DEBUGGING.md](../DEBUGGING.md) is the user's guide to these tools. This document is for someone who wants to extend them or fix them, so it covers data layouts, file formats, control flow and the rules the code relies on.

## Responsibilities and scope

**In scope**

- Interactive inspection of a stopped machine: `kernel/debug/monitor.c`, together with the stop-the-world calls it uses (`smp_stop_others`, `smp_park_self`, `smp_resume_others` in `kernel/arch/riscv/smp.c`).
- Event tracing: the `trace()` macro, per-CPU rings, decoding in the kernel, the `EUTRACE1` dump, and the host tool `tools/trace/eutrace.py`.
- Sampling profiler: timer-driven sampling, the per-CPU buffers, the in-kernel report, the `EUPROF1` dump, and `tools/profile/euprof.py` (report, folded stacks, SVG flamegraph).
- Symbols: `.ksyms` generation by `tools/gen_ksyms.py` with a two-pass link; lookups in `kernel/debug/ksyms.c`.
- Backtraces, including through trap frames and into user space (`kernel/debug/backtrace.c`).
- Fault-tolerant memory access (`safe_read_*`/`safe_write_*`, `kernel/arch/riscv/safe.S`) and the exception table they use.
- Host file output through QEMU semihosting (`kernel/arch/riscv/semihost.c`).
- The panic report and post-mortem flow (`kernel/core/panic.c`), and the kernel log ring (`kernel/core/printk.c`).
- How lockdep reports problems (`kernel/debug/lockdep.c`). Its semantics, meaning which orders are recorded and why, are covered in the SMP document.
- In-kernel unit tests (`KTEST`, `kernel/debug/ktest.c`).
- GDB helpers (`tools/gdb/eunuchs.py`, `tools/gdb/init.gdb`) and the `record`/`replay` Makefile targets.

**Out of scope (owned elsewhere, used here)**

- The trap entry code and the trap frame layout (`kernel/arch/riscv/trapentry.S`, `trap.c`). This document only describes the fake frame record and the breakpoint and exception-table hooks.
- Spinlocks, IPIs and the scheduler. The monitor and panic code call `spin_lock_debug`, `thread_backtrace`, `cpus_print` and similar functions, but those belong to the core and SMP subsystems.
- The many `*_print(print_fn)` functions that monitor commands call (`pmm_print`, `slab_print`, `tcp_print`, ...). Each one belongs to its own subsystem. The monitor only dispatches to them.
- Memory-corruption detection (redzones, poisoning) belongs to `mm`. User tests and `tools/run_tests.py` belong to the test harness.

**Common convention.** Nearly every reporting function takes a `print_fn` (`typedef int (*print_fn)(const char *fmt, ...)`, `kernel/include/debug.h`). One implementation can therefore write to the console (`kprintf`), to the log ring as an error (`panic_print`, `log_err_print`), or into a procfs buffer (`seq_print`). New reporting code should follow this pattern so that it works from the monitor, from panics and from `/proc` alike.

## Source map

| File | Role |
|---|---|
| `kernel/debug/monitor.c` | Monitor: line editor, history, tokenizer, address parser, command table, `monitor_enter` |
| `kernel/debug/trace.c` | Trace rings, `trace_record`, group parsing, in-kernel decoding (`fmt_arg`, `trace_print`), `trace_dump_file` (EUTRACE1) |
| `kernel/include/trace.h` | `struct trace_rec`, `struct trace_buf`, `TRACE_RECS`, group bits, the `trace()` / `trace_mark()` macros |
| `kernel/include/trace_events.h` | `TRACE_EVENTS(X)` X-macro: one line per event (name, group, three argument names). Also parsed by Python. |
| `kernel/debug/profile.c` | Sampling (`profile_sample`), control, in-kernel report, `profile_dump_file` (EUPROF1) |
| `kernel/include/profile.h` | Public profiler API, sample flags `PROF_IDLE/USER/IN_IRQ` |
| `kernel/arch/riscv/timer.c` | `timer_set_profile_hz`, `timer_interrupt` (calls `profile_sample`), `watchdog_check` |
| `kernel/debug/backtrace.c` | Frame-pointer unwinder, `backtrace_print`, `backtrace_here` |
| `kernel/debug/ksyms.c` | Lookups in the embedded table: `ksym_lookup`, `ksym_find`, `ksym_format` |
| `tools/gen_ksyms.py` | Turns `nm -n` output into `ksyms.S` (the `.ksyms` section) |
| `kernel/arch/riscv/kernel.ld` | Places `.ktests` and `__ex_table` in `.rodata`, and `.ksyms` last |
| `kernel/debug/ktest.c`, `kernel/include/ktest.h` | `KTEST` registration in a linker section, assertions, runner |
| `kernel/debug/hexdump.c` | `hexdump_safe`: hex and ASCII dump that tolerates faults |
| `kernel/debug/lockdep.c` | Lock-order graph and inversion and might-sleep reports (reporting side covered here) |
| `kernel/core/panic.c` | `panic`, `panic_trap`, `panic_print`, panic serialization across CPUs |
| `kernel/core/printk.c` | Log ring `log_buf`/`log_head`, `vklog`, thresholds, `dmesg_print`, `kprintf` |
| `kernel/include/debug.h` | Declarations for ksyms, backtrace, hexdump, panic, `assert`/`BUG_ON`, monitor, cmdline |
| `kernel/arch/riscv/safe.S`, `kernel/include/safe.h` | Fault-tolerant 1/2/4/8-byte reads and writes; `struct extable_entry` |
| `kernel/arch/riscv/trap.c` | `extable_lookup`, `is_semihost_ebreak`, breakpoint handling, `trap_breakpoint_hook` |
| `kernel/arch/riscv/semihost.c` | `semihost_open/write/close` (QEMU host file I/O) |
| `kernel/arch/riscv/smp.c` | Stop-the-world (`smp_stop_others`, `cpu_park`, `ipi_poll`), `cpu_backtrace`, `cpus_print` |
| `kernel/core/console.c` | `console_rx_char`: Ctrl-] (0x1d) enters the monitor; `console_getc` polls |
| `tools/gdb/init.gdb` | Sets the architecture, connects to `:1234`, sources `eunuchs.py` |
| `tools/gdb/eunuchs.py` | `eu-*` commands that read kernel structures through GDB |
| `tools/trace/eutrace.py` | EUTRACE1 decoder: text and Perfetto/Chrome JSON; a small ELF reader also used by `euprof.py` |
| `tools/profile/euprof.py` | EUPROF1 decoder: report, folded stacks, SVG flamegraph |
| `Makefile` | Two-pass link, `trace-json`, `flamegraph`, `debug`/`gdb`, `record`/`replay*`/`replay-check` |

## Concepts and data structures

### Monitor state

All monitor state lives in file-static variables in `monitor.c`. There is one monitor for the whole machine, because only the CPU that owns the stopped world runs it.

| Variable | Meaning |
|---|---|
| `mon_tf` | Trap frame that `regs`, `bt` and `$reg` refer to. Either the frame of the CPU being viewed, or `NULL`. |
| `mon_self_tf` | Frame the monitor was entered with on its own CPU. `cpu <own id>` restores it. |
| `mon_view` | CPU selected with `cpu <n>`, or `-1` for the monitor's own CPU |
| `mon_can_continue` | Whether `c` is allowed (`false` after a panic) |
| `mon_depth` | Nesting depth. A breakpoint or panic while the monitor runs re-enters it, and the prompt then becomes `mon[2]>`. |
| `history[16][128]`, `hist_count` | Command history ring (`HIST_SIZE` 16, `LINE_MAX` 128) |

Commands are described by `struct mon_cmd { name, args, help, fn }` in the static array `commands[]`. A handler has the signature `int fn(int argc, char **argv)` and returns `MON_STAY` (0) or `MON_EXIT` (1). Only `cmd_continue` returns `MON_EXIT`. `tokenize` splits a line on whitespace into at most `MAX_ARGV` (8) arguments.

### Stop-the-world fields (`struct cpu`, `percpu.h`)

| Field | Set by | Used by |
|---|---|---|
| `stopped` | `cpu_park` | `smp_stop_others` (waits for it), `cmd_cpu`, `panic_other_cpus`, `cpu_backtrace` |
| `stop_tf` | `cpu_park`: the interrupted frame when parked from the stop IPI, `NULL` when parked from a spin loop | `cmd_cpu` → `mon_tf` |
| `stop_pc`, `stop_fp` | `cpu_park`: from `stop_tf`, or from the spin loop's caller | `backtrace_print` for that CPU |
| `irq_tf` | the interrupt path | Ctrl-] entry (`console_rx_char`), profiler |
| `in_lockdep` | lockdep | stops lockdep from recursing through printk's lock |

`stop_owner` (static in `smp.c`) holds the id of the CPU that has the others stopped, or -1. `monitor_entry_time` (`sched.c`) is non-zero while the outermost monitor runs. Several subsystems read it: lockdep and the watchdog turn themselves off, `signal_send_locked` relaxes its locking assertion, and CPU-time accounting freezes "now".

### Trace records and rings

```
struct trace_rec            (40 bytes, little endian; Python: struct "<QHHI3Q")
  0  u64 ts       rdtime() at record time (one clock for all harts)
  8  u16 type     enum trace_event (TE_<name>)
 10  u16 cpu
 12  u32 tid      cur_thread->tid, 0 if none
 16  u64 a[3]     arguments, meaning given by trace_events.h

struct trace_buf  (one per CPU: trace_bufs[MAX_CPUS])
  u64 head                  total records ever written on this CPU
  struct trace_rec recs[TRACE_RECS]   TRACE_RECS = 8192, slot = head % 8192
```

The valid records are `[max(0, head-8192), head)`. A ring is never reset except by `trace_clear`, which sets every `head` to 0. Each ring is 320 KiB (8192 × 40 bytes), statically allocated per CPU.

**Groups** (`enum trace_group`): `TG_SCHED`, `IRQ`, `LOCK`, `MM`, `MARK`, `SYSCALL`, `PROC`, `BLK`, `FS`, `NET`, which are bits 0–9 (`TG_ALL` = `0x3ff`). The global mask is `trace_groups_enabled`. It defaults to `TG_ALL & ~TG_MM`.

**The event X-macro.** `trace_events.h` defines `TRACE_EVENTS(X)`, with one `X(name, GROUP, "arg0", "arg1", "arg2")` per line. From it `trace.h`/`trace.c` generate:

- `enum trace_event { TE_name, ..., TE_MAX }`. An event's position in the list is its numeric type in dumps.
- `trace_event_group[TE_MAX]`, which maps each event to its `TG_` bit.
- `event_names[]` and `event_args[][3]`, which are used only for decoding.

There are 35 events today, from `sched_switch` to `tcp_rexmit`.

**Argument names control decoding.** The kernel decoder (`trace.c: fmt_arg` and `print_rec`) and the host decoder (`eutrace.py: Trace.fmt_arg` and `describe`) pick a format from the argument's *name*:

| Name rule | Kernel (`fmt_arg`) | `eutrace.py` |
|---|---|---|
| ends in `tid` | `name=N(threadname)` via `thread_name()` | same, names from the dump's thread table plus `thread_create` events |
| `sig` | signal name | signal name (own table) |
| ends in `pid`; `code status sector len us seq blocks write` | signed decimal | signed decimal |
| `ret`, `result` | signed decimal | signed decimal |
| ends in `state` | `thread_state_name()` | `R r S D` |
| ends in `pc` or `site` | `ksym_format` | ELF symbol (`--elf`) |
| `name16` | 16 bytes in `a[1..2]` as a string; arg 2 is then skipped | same |
| `text24` | 24 bytes in `a[0..2]` as a string; args 1–2 skipped | same |
| `nr` | system call name (`syscall_name`) | from `lib/include/uapi/syscall.h` |
| `str` | pointer to a NUL-terminated string, read with `safe_read_u8` (up to 39 chars) | read from the ELF's PROGBITS sections, so only strings in the kernel image |
| ends in `_ip` | dotted quad | **not handled** (hex) |
| starts with `tcp_` | `tcp_state_name` | **not handled** |
| `proto`, `cpu`, `ipi`, `cause` | TCP/UDP/ICMP/ARP, `cpuN`, IPI name, timer/external/software | only `cause` handled |
| `irq` with value 0 | omitted | omitted |
| `""` | unused, omitted | omitted |
| anything else | `name=0x…` | `name=0x…` |

Note the rule order. `tid` is tested before `pid`, so `from_pid` is decimal and `owner_tid` is a thread.

### EUTRACE1 dump format

Written by `trace.c: trace_dump_file` and by `eunuchs.py: _trace_dump_bytes`, and read by `eutrace.py: Trace.__init__`. Everything is little endian.

```
header (32 bytes)
  char magic[8]   "EUTRACE1"
  u32  version    1
  u32  ncpus      ncpus_online
  u64  timebase   platform.timebase (ticks per second; decoder assumes 10 MHz if 0)
  u32  rec_size   sizeof(struct trace_rec) = 40 (decoder rejects anything else)
  u32  nthreads
threads: nthreads x { u32 tid; u32 state; char name[16]; }   (24 bytes each)
per CPU, cpu 0..ncpus-1:
  u64 count
  count x struct trace_rec, oldest first
```

The kernel writer caps the thread table at 256 entries (`MAX_DUMP_THREADS`) and writes at most `TRACE_RECS - 256` records per CPU. It leaves that margin because other CPUs keep recording while it writes. The GDB writer reads a halted machine, so it writes every thread and the full ring. Neither writer stores the event table, so the decoder takes the event numbering from the `trace_events.h` in the source tree it runs from.

### Profiler records and buffers

```
struct prof_rec            (40 bytes; Python: struct "<QIIHBBB3x16s")
  0  u64 ts
  8  u32 tid, pid          pid 0 for kernel threads
 16  u16 cpu
 18  u8  nk, nu            number of kernel PCs, then user PCs, following the header
 20  u8  flags             PROF_IDLE 1, PROF_USER 2, PROF_IN_IRQ 4
 21  u8  pad[3]
 24  char comm[16]         process name, or thread name for kernel threads
 40  u64 pcs[nk + nu]      kernel PCs leaf first, then user PCs leaf first

struct prof_cpu (prof[MAX_CPUS])
  u8 *buf      kmalloc(PROF_BUF), PROF_BUF = 2 MiB
  u32 used     bytes of valid records (published with a release store)
  u64 samples, dropped
```

Records vary in length and are packed back to back. A full buffer counts further samples as `dropped`. It never wraps. Stack depth is at most `PROF_KDEPTH` (24) kernel and `PROF_UDEPTH` (24) user frames, so a record is at most 40 + 48×8 = 424 bytes.

### EUPROF1 dump format

Written by `profile.c: profile_dump_file` and `eunuchs.py: EuProfileDump`, and read by `euprof.py: Profile`.

```
header (40 bytes; struct prof_file_hdr; Python "<8sIIQIIQ")
  char magic[8]  "EUPROF1\0"
  u32  version   1
  u32  ncpus
  u64  timebase
  u32  hz        profile_hz (the requested rate, see Limitations)
  u32  rec_hdr   sizeof(struct prof_rec) = 40 (decoder rejects a mismatch)
  u64  dropped   sum over CPUs
per CPU: u64 nbytes, then nbytes of packed records
```

### Embedded symbol table (`.ksyms`)

`gen_ksyms.py` emits four globals into section `.ksyms`, 8-byte aligned:

```
ksyms_num    .dword N
ksyms_addrs  N x .dword   sorted ascending (one entry per address)
ksyms_offs   N x .word    offset of each name in ksyms_names
ksyms_names  N x .asciz
```

Only `nm` types `t`, `T` and `W` are kept, minus names that match `^(\.L|\$|__ksym)`. When several names share an address, the script keeps the one with the smallest `(starts with "__", is local "t", name)`. It therefore prefers real functions over linker markers such as `__kernel_start`, and global symbols over local ones. `kernel.ld` places `.ksyms` after `.stacks`, as the last section before `__kernel_end`, so its size cannot shift any other address.

### Frame records

The kernel and user programs are compiled with `-fno-omit-frame-pointer`. RISC-V GCC stores each frame's record just below `s0`/`fp`:

```
fp - 8   return address
fp - 16  caller's fp
```

`trapentry.S` builds the same kind of record above every trap frame (`FRAME_SIZE` = 288 + 16, matching `TRAP_FRAME_SIZE` 304): `{prev fp, ra = sepc}`, with `s0` pointing just above it. For traps from the kernel, prev fp is the interrupted `s0`, so the unwinder walks straight into the interrupted code. For traps from user mode, prev fp is 0, which ends the chain after one "return address" equal to the user `sepc`. The same entry also emits CFI (`.cfi_signal_frame`, return column 64 = `sepc`) so that GDB can unwind through traps.

### Exception table

`struct extable_entry { uintptr_t insn, fixup; }` pairs are collected in `__ex_table` (inside `.rodata`, between `__ex_table_start` and `__ex_table_end`). `trap.c: extable_lookup(pc)` searches them linearly. Each `safe_*` accessor in `safe.S` adds one entry: label `1:` (the load or store) maps to label `2:` (`li a0,-1; ret`). The user-copy routines (`uaccess.S`) use the same table.

### Log ring

`char log_buf[LOG_BUF_SIZE]` (64 KiB) and `volatile u64 log_head` are deliberately non-static so that GDB can read them. Valid bytes are `[max(0, head - 64K), head)`, at index `pos % 64K`. Every line is formatted by `vklog` as `[sec.usec] cpu L subsys: text\n`. After the ring wraps, the oldest line is partial, and both `dmesg_print` and `eu-dmesg` skip it.

### KTEST descriptors

`KTEST(name)` defines a `static const struct ktest { name, file, fn }` in section `.ktests` (8-byte aligned, `__used`), plus a static function `ktest_fn_<name>(struct ktest_ctx *__kt)`. The linker script gathers them with `KEEP(*(.ktests))` between `__ktests_start` and `__ktests_end`. Tests therefore run in link order, which is the `KDIRS` order and then the wildcard file order within each directory. `struct ktest_ctx` holds the test name and a failure count.

### Lockdep report bookkeeping

For reporting, each dependency edge stores its first occurrence: `struct ld_edge { from, to, cpu, tid, thread[16], from_pc, to_pc }`, in `edges[LD_EDGES]` (1024). The dependency graph is a bitmap `deps[LD_CLASSES][LD_CLASSES/64]` with `LD_CLASSES` = 192 (class 0 means "no class"). Edges beyond 1024 still go into the bitmap but lose their site information.

## How it works

### Entering the monitor

| Entry | Caller | `tf` | `can_continue` |
|---|---|---|---|
| Ctrl-] on the console | `console.c: console_rx_char` (in the UART interrupt handler, which runs on cpu0) | `this_cpu()->irq_tf` | yes |
| Kernel `ebreak` / `debug_break()` | `trap.c: trap_handler`, `EXC_BREAKPOINT`, when `trap_breakpoint_hook` is unset or declines | trap frame; `sepc` is advanced by the instruction length afterwards | yes |
| `dbg break` (`SYS_debug`/`DBG_BREAK`) | `syscall.c: do_debug` | `thread_user_tf(current)` | yes |
| Boot option `break` | `main.c: kmain`, before `pmm_init` | `NULL` | yes |
| Any panic | `panic.c: panic_finish` (unless `panic=shutdown`) | `NULL` for `panic()`, the trap frame for `panic_trap()` | **no** |

A semihosting `ebreak` is the exception to the kernel breakpoint rule. `is_semihost_ebreak` checks that the words before and after the `ebreak` are `0x01f01013` (`slli x0,x0,0x1f`) and `0x40705013` (`srai x0,x0,7`). If they match, the trap handler sets `a0 = -1` and skips the instruction, so a semihosting call without QEMU support simply fails. The ktest `breakpoint_trap_and_resume` uses `trap_breakpoint_hook` to catch an `ebreak` without entering the monitor.

`monitor_enter(reason, tf, can_continue)` then does the following:

1. **Interrupts off.** It saves `intr_enabled()` and calls `intr_off()`.
2. **Takes ownership of the stopped world.** If this CPU already owns the stop (`smp_stopped_by_me()`, which is the case after a panic or on a nested entry), it skips this step. Otherwise it loops `while (!smp_stop_others(&missing)) smp_park_self(tf);`. If another CPU owns the stop, this CPU parks as an ordinary stopped CPU until the owner releases the world, and then takes it. `took_stop` records that this call must release the world later.
   - `smp_stop_others` claims `stop_owner` with a CAS, sends `IPI_STOP` to every CPU that is not already stopped, and waits up to `STOP_TIMEOUT_MS` (200 ms) for each one to set `stopped`. CPUs that do not stop are returned in `missing`. These are CPUs with interrupts off that are not spinning on a lock.
   - A target parks in `cpu_park`, from one of two places. It can park from the IPI handler (`ipi_handle`, which records `stop_tf`). Or it can park from `ipi_poll`, which spinlock spinners and lockdep's raw lock call. In that case `stop_tf` is `NULL` and `stop_pc`/`stop_fp` describe the spin loop's caller. While parked, a CPU still answers TLB-flush requests.
3. **Saves and replaces the monitor's view state** (`mon_tf`, `mon_self_tf`, `mon_view`, `mon_can_continue`), so that a nested entry can restore the outer state on exit. It increments `mon_depth`. At depth 1 it sets `monitor_entry_time = rdtime()`.
4. **Prints a banner**: the reason, `stopped at <sym>` if there is a frame, and any CPUs that did not stop.
5. **Runs the read-eval loop.** `readline` polls `console_getc`, which in turn polls the UART or SBI and waits one tick between empty polls with `cpu_wait_ticks`. It handles DEL/BS, ^U, ^C and `ESC [ A/B` for history. A line that differs from the previous history entry is added to the history. `dispatch` maps `continue` to `c`, then scans `commands[]` linearly with `strcmp`.
6. **On `c`:** it decrements `mon_depth`. At depth 0 it adds the time spent in the monitor to `this_cpu()->last_switch` (if `sched_ready`), so the interrupted thread is not charged for it, and clears `monitor_entry_time`. It restores the saved view state and logs `leaving monitor`. It calls `smp_resume_others()` if it took the stop (this resets every CPU's `last_tick` so the watchdog does not fire, then releases `stop_owner`), and restores interrupts.

Parked CPUs see `stop_owner` go to -1, add the paused time to their own `last_switch`, refresh `last_tick` and return to what they were doing.

**Viewing another CPU.** `cmd_cpu n` requires `cpus[n].stopped`. It sets `mon_view = n` and `mon_tf = cpus[n].stop_tf`, which is `NULL` if that CPU parked from a spin loop. After that, `cmd_regs` prints the frame or explains that a spin-loop park captured no registers. `cmd_bt` uses `cpu_backtrace` from `stop_pc`/`stop_fp`, and `parse_addr("$reg")` reads the viewed frame.

**Address expressions** (`parse_addr`):
- `$name` reads a register from `mon_tf`: `$sepc`/`$pc`, `$stval`, or any ABI register name from `reg_names[]`.
- A string starting with a digit is parsed by `kstrtoul(..., 0)`, so it can be hex or decimal.
- Anything else is treated as `symbol[+off]`, looked up with `ksym_find`. Only text symbols are in the table (see Limitations).

### Recipe: add a monitor command

1. Write a handler in `monitor.c`:
   ```c
   static int cmd_foo(int argc, char **argv)
   {
       if (argc < 2) { kprintf("usage: foo <x>\n"); return MON_STAY; }
       foo_print(kprintf);          /* put the real printer in your subsystem */
       return MON_STAY;
   }
   ```
2. Add `{ "foo", "<x>", "one-line help", cmd_foo }` to `commands[]`. The order of the array is the order `help` prints in. The `args` column is printed with `%-26s`, so keep it short.
3. Follow the monitor's rules:
   - **Never block.** Other CPUs are parked and may hold any lock. Take locks with `spin_lock_debug`/`spin_unlock_debug`. These become a trylock when `panic_depth`, `smp_world_stopped()` or `spin_held_by_me` is true, and the caller must handle `locked == false` (see `cmd_kill`, `thread_backtrace`). Never take a mutex.
   - **Read through `safe_read_*`** whenever the pointer comes from the user or from possibly corrupt data. `hexdump_safe` and `cmd_poke` do this.
   - **Avoid allocating.** `kmalloc` takes ordinary spinlocks (`slab.c` uses `spin_lock_irqsave`), and a parked CPU may hold them.
   - **Write the printer as `xxx_print(print_fn)`** in the owning subsystem. It can then also be a `/proc` file (add it to `procfs.c: root_files[]`) and be used from panics.

### Tracing

**Recording.** `trace(ev, a0, a1, a2)` is a macro. It tests `trace_groups_enabled & trace_event_group[TE_ev]` first, so a disabled event costs one load, one AND and one branch, and its arguments are not evaluated. `trace_record`:
1. saves `sstatus.SIE` and calls `intr_off()`. With interrupts off, the code cannot migrate or be interrupted, so each ring has exactly one writer, its own CPU;
2. picks `trace_bufs[this_cpu()->id]` and the slot `head % 8192`;
3. fills `ts = rdtime()`, `type`, `cpu`, `tid`, `a[0..2]`;
4. increments `head` and restores interrupts.

No atomics are used. A reader on another CPU may see a half-written newest record. The readers deal with this by leaving a margin (see below).

**Strings in records.** `trace_mark("msg", a, b)` stores the *pointer* of a string literal, which is why the `str` decoding exists. `umark` (from `dbg mark`) and `file_open` copy up to 24 bytes into `a[0..2]` (`text24`). `thread_create` and `proc_exec` put 16 name bytes into `a[1..2]` (`name16`).

**Groups at run time.** `trace_set_groups(spec)` parses a comma list of `sched,irq,lock,mm,mark,syscall,proc,blk,fs,net,all,none` and stores the mask. An unknown name returns -1 and leaves the mask unchanged. `trace_init` applies the `trace=` boot option. Events recorded before `trace_init` use the default mask.

**Reading in the kernel** (`trace_print(print, last_n)`, used by `trace show`, `/proc/trace` (200 events) and panics (16 events)). This merges the per-CPU rings by timestamp:
1. For each online CPU it snapshots `head[c]` and sets the lower bound `lo[c] = head - min(head, 8192 - 64)`. The 64-record margin keeps it from reading slots that a running CPU is about to overwrite.
2. Walking backward, it repeatedly takes the CPU whose next-older record has the largest `ts`, `want` times. This finds where each CPU's share of the newest `want` events starts.
3. Walking forward from there, it repeatedly prints the CPU whose next record has the smallest `ts`.

`print_rec` formats `[sec.usec] cN tid name event args...` using `fmt_arg`.

**Dumping** (`trace_dump_file(path)`, from the monitor's `trace dump`, or from `kinit` when `ktest`/`utest` and `tracedump=` are given; `make test` uses `tracedump=build/ktest-trace.bin`):
1. It calls `semihost_open(path, SEMIHOST_MODE_WB)` and returns -1 if that fails (semihosting unavailable).
2. It snapshots up to 256 threads under `spin_lock_debug(&sched_lock)` into a `kmalloc`'d array.
3. It writes the header, the threads, and then each CPU's newest `min(head, 8192 - 256)` records, oldest first, in at most two contiguous writes per CPU (the ring may wrap).

**Host decoding** (`eutrace.py`):
- `load_events()` regex-parses `trace_events.h`. It starts after the `#define TRACE_EVENTS` line and needs each event on one line in exactly the form `X(name, GROUP, "a", "b", "c")`. List order gives the type numbers.
- `Trace.__init__` checks the magic and `rec_size`, reads the threads, concatenates every CPU's records and sorts them by `ts`. It adds names from `thread_create` events for threads that have since exited.
- `text` prints microseconds relative to the first record.
- `perfetto()` builds Chrome trace JSON:
  - pid 1 "CPUs", with track `tid = cpu` for who ran and `100 + cpu` for IRQs;
  - pid 2 "Threads", with one track per tid;
  - each `sched_switch` closes an `X` slice on the CPU track and on the previous thread's track (`args.ended_in_state`);
  - `syscall_enter`/`syscall_exit` pairs become slices on the thread track, named from the syscall table;
  - `irq_entry`/`irq_exit` pairs become slices on the IRQ track;
  - every other event becomes an instant (`ph: "i"`). It goes on the subject thread's track for `sched_wakeup`, `thread_create` and `thread_exit` (arg 0), and on the recording thread's track otherwise;
  - CPU slices that are still open are closed at the last timestamp.
- `make trace-json` runs `eutrace.py perfetto build/trace.bin build/trace.json`.

### Recipe: add a trace event

1. Append a line to `TRACE_EVENTS(X)` in `trace_events.h`, keeping the one-line form, e.g.
   `X(pipe_full, FS, "len", "pid", "")` with a trailing backslash on every line but the last. Appending at the end keeps the numbers of existing events stable. Inserting in the middle renumbers every later event, and old dumps will then decode wrongly.
2. Choose argument names from the decoding table above so that both decoders format them well. If you introduce a new kind of name, add it to **both** `trace.c: fmt_arg` and `eutrace.py: Trace.fmt_arg`.
3. Call `trace(pipe_full, n, p->pid, 0);` at the site. Every argument is cast to `u64`.
4. If the event should be a slice in Perfetto rather than an instant, add a branch to `eutrace.py: Trace.perfetto`.
5. Adding a **group** needs a new bit in `enum trace_group`, a larger `TG_ALL`, an entry in `trace.c: groups[]`, and updates to the help strings in `trace_init` and `cmd_trace`.

### Profiler

**Start** (`profile_start(hz)`, from `profile=` in `profile_boot_option`, from the monitor's `profile start`, or from `SYS_debug DBG_PROFILE`):
1. It clamps `hz` to [`HZ` (100), 5000], with 0 meaning 500.
2. It allocates a 2 MiB `kmalloc` buffer for each online CPU that lacks one. Existing buffers are kept and appended to.
3. It records `started`, calls `timer_set_profile_hz(hz)`, and release-stores `profiling = true`.

`timer_set_profile_hz` sets `per_tick = hz / HZ` (at least 1) and the timer interval to `timebase / (HZ * per_tick)`. Every timer interrupt then calls `profile_sample`, and every `per_tick`-th one is also a scheduler tick (`timer_interrupt`).

**Sampling** (`profile_sample(c->irq_tf)`, in the timer interrupt with interrupts off):
1. If this CPU has no buffer, it returns.
2. **Kernel mode** (`SPP` set):
   - `pcs[0] = sepc`, followed by `backtrace_collect(TF_FP(tf))`, up to 23 more frames. That function uses `safe_read_u64`.
   - Trailing PCs below `KERNEL_BASE` are dropped, because for a trap from user mode the fake frame record's last "return address" is the user `sepc`.
   - If the thread belongs to a process with an `mm`, it scans the kernel PCs for `kernel_trap_entry`. If it finds one, the kernel stack started at a trap from user mode, so it also unwinds the user stack starting at `thread_user_tf(t)`.
3. **User mode:** it unwinds the user stack from the frame.
4. **User unwinding** (`user_stack`) reads `{caller fp, ra}` at `fp - 16` with `uvm_read`. That function translates through the process page table and copies from the direct map. It never faults pages in and never touches user virtual addresses directly. The unwind stops on a misaligned fp, a non-increasing fp, a zero ra, or `PROF_UDEPTH`.
5. **Store.** If the record does not fit, it increments `dropped` and returns. Otherwise it fills the header:
   - `flags`: `PROF_IDLE` if `t == c->idle`, `PROF_USER`, and `PROF_IN_IRQ` if `irq_depth > 1`;
   - `comm` from the process or thread name.
   It copies the header and the PCs, publishes `used` with a release store, and increments `samples`.

**Stop/clear.** `profile_stop` clears `profiling` and resets the timer to `HZ`. `profile_clear` stops and also frees the buffers.

**In-kernel report** (`profile_report`: `profile` in the monitor, `/proc/profile`, `profile report` from the shell):
- It walks every CPU's records up to `used` (acquire load).
- It counts idle, user and kernel samples.
- It counts process names in `comms[64]`. Names are interned with a 17-byte `kmalloc` per new name and freed at the end.
- It counts kernel functions in `funcs[256]`, keyed by the *name pointer* returned from `ksym_lookup(pc - (i ? 1 : 0))`. Self time goes to the leaf only. Total time is counted once per sample even under recursion (the `seen[]` list).
- It prints the top 12 processes and the top 30 functions (`TOP`), each sorted by selection sort. Percentages are of busy (non-idle) samples.

**Dump** (`profile_dump_file`): stops profiling, writes the EUPROF1 header and each CPU's `used` bytes through semihosting, and restarts profiling if it was running.

**Host side** (`euprof.py`):
- `Profile` parses the dump and sorts the samples by time.
- `Symbolizer` uses `eutrace.Elf` for the kernel (`build/kernel.elf`) and, for each `comm`, for `build/user/bin/<comm>`, falling back to `build/user/bin/eutils` for applets. Like the kernel, it subtracts 1 from every PC except the leaf, and drops offsets.
- A stack is laid out root first: `comm; user frames...; [kernel]; kernel frames...`.
- `report` prints the idle/user/kernel split (from all samples), per-CPU busy %, the top 15 processes, and the top 40 functions by self time with their total.
- `folded` writes `stack count` lines.
- `svg` builds a frame tree and writes a 1200-pixel-wide SVG with embedded JavaScript for click-to-zoom. It skips frames narrower than 0.3 pixels.
- `--no-idle`, `--cpu`, `--pid` and `--kernel-only` filter the samples. `make flamegraph` runs all three modes with `--no-idle`.

### Symbol table generation (two-pass link)

From the `Makefile`:
1. `build/ksyms0.S` is generated by `gen_ksyms.py < /dev/null`, which gives an empty table with `ksyms_num = 0`.
2. **Pass 1:** `build/kernel.pass1.elf` is linked from all objects plus `ksyms0.o`.
3. `build/ksyms.S` is generated by `nm -n kernel.pass1.elf | gen_ksyms.py`.
4. **Pass 2:** `build/kernel.elf` is linked from the same objects plus `ksyms.o`.
5. **Check:** the text symbols of both passes (`nm -n | grep -i ' t '`) are compared with `cmp`, and the build fails with `error: text symbols moved between link passes` if they differ.

This works because `.ksyms` is the last section in `kernel.ld`. Its size changes between the passes, but nothing is placed after it. Two lookups use the table:
- `ksym_lookup` binary-searches for the largest address `<= addr`. It returns NULL for an address below the first symbol or at or above `__text_end`.
- `ksym_find` is a linear `strcmp` scan that returns the first match in address order.

### Backtraces

- **`backtrace_collect(fp, pcs, max)`** reads `ra = [fp-8]` and `next = [fp-16]` with `safe_read_u64`. It stops on a zero or misaligned fp, a read fault, `ra == 0`, or `next <= fp` (stacks grow down, so callers are always higher). A corrupt chain therefore ends the walk instead of faulting.
- **`backtrace_print(print, pc, fp)`** prints `#0 pc` and then up to `BACKTRACE_MAX - 1` (31) return addresses. Each return address goes through `format_return`, which:
  - symbolizes `ra - 1`, so a call to a noreturn function at the very end of a function resolves to the caller;
  - prints the offset of `ra` itself;
  - for an address below `USER_TOP` with no kernel symbol, tries `elf_symbolize` on `current_proc()->image` and prints `[user <name>] func+off`.

  Three or more identical consecutive PCs are collapsed into a single `... frame repeated N more times (recursion)` line. If the walk hits the limit, `... (truncated)` is printed.
- **`backtrace_here(print)`** starts at the *caller* of `backtrace_here`. It reads the caller's fp from `current_fp() - 16` and uses `__builtin_return_address(0) - 2` as the pc. `panic()` uses the same trick for the same reason: `ra` itself may lie past the end of a function that calls a noreturn function.
- **Other contexts:**
  - `thread_backtrace` (`sched.c`) unwinds a sleeping thread from its saved `ctx.ra` and `ctx.s[0]`, and a thread running on a parked CPU from that CPU's `stop_pc`/`stop_fp`.
  - `cpu_backtrace` (`smp.c`) handles the monitor's `cpu <n>` view.
  - For user processes, the monitor's `ubt` calls `proc_user_backtrace` with the user frame.
- **Through traps:** the fake record built by `trapentry.S` means a backtrace started in a handler continues past `kernel_trap_entry` into the interrupted function. For a trap from user mode it ends with the user `sepc`.

### Safe memory access

`safe_read_u{8,16,32,64}(addr, out)` and `safe_write_u{8,16,32,64}(addr, val)` are generated by the `SAFE_READ`/`SAFE_WRITE` macros in `safe.S`. Each is a single load or store at label `1:`, plus an `__ex_table` entry that sends faults at that instruction to `2:`, which returns -1.

When a kernel-mode memory fault occurs, `trap_handler` proceeds as follows:
1. It first tries `vm_kernel_fault_fixup` for kernel addresses.
2. For user addresses touched by a registered instruction (the uaccess routines), it tries demand paging with `uvm_fault`.
3. Otherwise, if `extable_lookup(sepc)` finds a fixup, it sets `sepc = fixup` and returns, logging the fixup at `pr_trace` level.

`is_memory_fault` covers load and store access faults, page faults and misaligned accesses, but not instruction faults. Because `SUM` is off outside the uaccess routines, a safe read of a mapped user address also faults and returns -1.

### Semihosting

`semihost_call(op, params)` emits `slli x0,x0,0x1f; ebreak; srai x0,x0,7`, 4-byte aligned with compressed instructions disabled (`.option norvc`), so the three-word pattern is exact. It passes `a0 = op` and `a1 = params`. Only `SYS_OPEN` (0x01), `SYS_CLOSE` (0x02) and `SYS_WRITE` (0x05) are implemented. `semihost_write` converts the host's "bytes not written" result into bytes written, or -1. QEMU resolves paths relative to its working directory, which is the project root under `make`. Semihosting is enabled by `QEMUBASE` (`-semihosting-config enable=on,target=native`) and is **not** enabled in `RROPTS`. In record/replay runs, every dump therefore fails cleanly through the `is_semihost_ebreak` path.

### Panic flow

`panic(fmt, ...)` and `panic_trap(tf)`, both in `panic.c`:

1. **`panic_begin(tf)`:**
   - Interrupts go off.
   - The CPU tries to claim `panic_cpu` with a CAS. If another CPU already holds it, this CPU loops in `smp_park_self(tf)` forever. The first panicking CPU will show it as stopped.
   - It increments `panic_depth`. If the depth exceeds 2, it prints `*** recursive panic; halting ***` and calls `platform_exit(3)`.
   - At depth 1 it stops the other CPUs (`smp_stop_others(&panic_unstopped)`), parking first if the monitor owns the stop on another CPU.
2. **The headline.** For `panic()`, it is the formatted message (256 bytes max) and a backtrace from the caller. For `panic_trap()`, it is the cause name, the mode and the symbolized `sepc`, followed by a cause-specific part:
   - illegal instruction bits;
   - for page and access faults, the address, a `vm_fault_hint` and a page-table walk;
   - otherwise `stval`.

   Then it prints `trap_dump` and a backtrace from the frame.
3. **`panic_context()`:**
   - the current thread and the spinlocks this CPU holds;
   - at depth 1 with several CPUs, every other CPU's thread, its held locks, and either its backtrace from `stop_pc`/`stop_fp` or `did not stop`;
   - at depth 1, the last 16 trace events.
4. **`panic_finish(tf)`:** with `panic=shutdown`, it calls `platform_exit(2)`. Otherwise it calls `monitor_enter("panic", tf, false)`. If the monitor somehow returns, it prints `halting.` and calls `platform_exit(2)`.

All report output goes through `panic_print`, which is `vklog(LOG_ERR, SUBSYS_PANIC)`, so the report is in the log ring for `dmesg`/`eu-dmesg`. While `panic_depth > 0`:
- `vklog` writes without taking `log_lock`;
- `spin_lock_debug` becomes a trylock;
- `spin_unlock_irqrestore` stops panicking about unlocks by a non-owner;
- lockdep and the watchdog are off.

`panic_depth` is never decremented, so the machine stays in this "panicked" mode for the rest of the session.

A kernel stack overflow detected at trap entry goes through `trap.c: stack_overflow` (on the per-CPU emergency stack) into `panic_trap`. An overflow during that handling exits with code 4.

### Log ring and printk

- `vklog(level, subsys, fmt, ap)` returns early if `level > log_threshold[subsys]`.
- Otherwise it formats into a 512-byte stack buffer, makes sure the line ends with a newline, and then, under `log_lock` (skipped when panicking), appends it to the ring with `log_ring_write`. If `level <= console_level`, it also writes the line to the console.
- `kprintf` is separate: it formats straight to `console_putc`, takes no lock and **does not** go to the ring. Monitor output is therefore not in `dmesg`, except the `entering monitor` and `leaving monitor` lines, which use `pr_info`.
- `log_ring_copy` serves `/dev/kmsg`.
- `printk_apply_cmdline` handles `log=` and `quiet`.

### Lockdep reporting

`lockdep_acquire` is called from the lock primitives, and is skipped when lockdep is disabled, `panic_depth` is non-zero, or `monitor_entry_time` is set. Under its own untracked test-and-set lock (`raw_lock`, which spins with `ipi_poll()` so that a stop request still parks the CPU), `add_dep` checks each new edge `held → acquiring` with a breadth-first `find_path` for a path in the opposite direction. If it finds a cycle:
1. It disables lockdep (the graph is now known to be cyclic), increments `lockdep_reports` and drops the raw lock.
2. `report_inversion` prints the acquisition, the held lock, and every link of the reverse chain with its recorded thread, CPU and both sites (from `edges[]`), plus a backtrace of the acquisition. It also emits the `lock_inversion` trace event. All of this happens with `cpu->in_lockdep` set, so that printk's `log_lock` does not recurse into lockdep.
3. With `lockdep=panic`, it then panics.

`lockdep_might_sleep` reports a sleeping operation done while a spinlock is held. In self-test mode (`lockdep_testing`), both functions only count. `lockdep_print` is shared by the monitor and `/proc/lockdep`. `eu-lockdep` reads the same `classes`/`deps` arrays through GDB.

### KTEST

- `ktest_run(filter)` iterates over `__ktests_start..__ktests_end`. It skips tests whose name does not start with `filter` (`strncmp` prefix), runs each remaining test with a fresh `ktest_ctx`, and logs gtest-style `[ RUN ]`, `[ OK ]` and `[ FAILED ]` lines under `SUBSYS_TEST`.
- It ends with `N tests run, ...` and `KTEST RESULT: PASS|FAIL`. Running zero tests counts as FAIL.
- It returns the number of failed tests, or 1 if none ran.
- Failures are reported by `ktest_fail`, which formats into 256 bytes, increments `failures` and logs `file:line: msg`.
- `KT_EXPECT` and `KT_EXPECT_EQ`/`_STREQ` continue after a failure. `KT_ASSERT` returns from the test function.
- A test that panics ends the whole run (with `panic=shutdown`, QEMU exits with code 2).

Tests are started in two ways:
- `kinit` with `ktest[=prefix]`, which then powers off with the combined result (`platform_exit(failed ? 1 : 0)`) and optionally dumps the trace first;
- the monitor's `ktest [prefix]`.

The runner adds no isolation, timeouts or leak checks of its own. Tests that need those (for example `fs_ops_release_buffers_and_inodes`) do them themselves.

### Recipe: add a kernel test

```c
#include <ktest.h>

KTEST(myfeature_handles_empty_input)
{
    struct thing *t = thing_new();
    KT_ASSERT(t != NULL);                 /* stop here if allocation failed */
    KT_EXPECT_EQ(thing_count(t), 0);
    KT_EXPECT_STREQ(thing_name(t), "empty");
    thing_free(t);
}
```

Put it in any kernel `.c` file (by convention `kernel/tests/test_<area>.c`). The `Makefile` wildcard picks up new files under `KDIRS`, and the linker section registers the test. Name tests with a shared prefix so `ktest=myfeature` selects them. Undo any global state you change, as `breakpoint_trap_and_resume` does when it resets `trap_breakpoint_hook`, because later tests run in the same kernel.

### GDB helpers

Startup:
- `make debug` starts QEMU with `-s -S`, halted, with the gdbstub on `:1234`.
- `make gdb` runs `$(GDB) -q -x tools/gdb/init.gdb build/kernel.elf`.
- `init.gdb` sets `riscv:rv64`, turns off confirmation and paging, connects with `target remote localhost:1234`, and sources `tools/gdb/eunuchs.py`. That path is relative, so GDB must start in the project root, as `make` does.

Architecture of `eunuchs.py`:
- **Command classes.** Each command is a subclass of `EuCommand(gdb.Command)`, whose constructor registers `(name, summary)` for `eu-help`. The classes are instantiated by the tuple loop at the bottom of the file.
- **Memory access:** `_read(addr, size)` (inferior memory) and `_u64`.
- **Symbols:**
  - `_static(name)` finds file-static variables first (`gdb.lookup_static_symbol`), then globals. This is how it reaches `zone`, `prof`, `classes`, `nclasses` and so on.
  - `_eval_int(expr)` evaluates a C expression.
  - `_addr(expr)` also accepts function names.
  - `_symbolize(addr)` uses DWARF blocks, falling back to `info symbol` for assembly.
- **Kernel structures:**
  - `_threads()` and `_list_entries(head, type, field)` follow `struct list_head` lists, using the field's `bitpos` (a `container_of`).
  - `_walk_fp(pc, fp)` is a Python copy of `backtrace_collect`.
  - `_running()` maps each CPU's `cur_thread`.
  - `_gdb_thread_of(cpu)` returns `cpus[cpu].hartid + 1`, because QEMU numbers its GDB threads by hart id + 1.
- **Shared code with the trace tool.** `eutrace` is imported from `tools/trace` through `sys.path`. `eu-trace` and `eu-trace-dump` build an EUTRACE1 image straight from `trace_bufs` (`_trace_dump_bytes`) and decode it with the same code as the host tool, using `gdb.current_progspace().filename` as the ELF.
- **Profile dumps.** `eu-profile-dump` packs the EUPROF1 header with `struct.pack` and appends each `prof[c].buf[0:used]`.
- **Breakpoints.** At load time the file sets breakpoints on `panic` and `panic_trap`.

All commands only read memory, except `eu-log`, which writes `log_threshold[]` with `set var`. They therefore work on a wedged or panicked kernel, as long as the gdbstub responds.

### Recipe: add a GDB command

```python
class EuFoo(EuCommand):
    """eu-foo [N]: one-line description (shown by `help eu-foo`)."""

    def __init__(self):
        super().__init__("eu-foo", "short summary for eu-help")

    def invoke(self, arg, from_tty):
        foo = _static("foo_table")              # file-static or global
        for i in range(int(_static("nr_foo"))):
            gdb.write("  %d\n" % int(foo[i]["field"]))
```

Then add `EuFoo` to the tuple in the `for cls in (...)` loop at the end of the file. Use `_static` rather than `parse_and_eval` for file-static variables. Use `_list_entries` for lists. Read raw bytes with `_read` when you need a binary image. These helpers depend on DWARF names, so renaming a struct field in C breaks the command only when someone runs it. Run the command after changing any structure it reads.

### Record/replay

The `Makefile` defines `RROPTS(rr)` as one-CPU QEMU with:
- `-icount shift=auto,rr=record|replay,rrfile=build/rr/replay.bin,rrsnapshot=init`;
- a `snap.qcow2` drive (64M) to hold the initial VM snapshot;
- the disk as `fs.qcow2` (a qcow2 overlay on `base.img`) wrapped in a `blkreplay` driver;
- a user-mode network with `filter-replay`;
- no semihosting, no port forwarding and no pcap.

`rr_prepare` makes the recording self-contained:
1. It removes old images.
2. It copies `build/kernel.elf` to `build/rr/kernel.elf` and `build/fs.img` to `build/rr/base.img`.
3. It creates the two qcow2 files.
4. It writes the boot options to `build/rr/append`.

The targets:
- `record` runs `RROPTS(record)` with `$(APPEND)`.
- `replay` and `replay-debug` (the latter with `-s -S`) run `RROPTS(replay)` with `$(cat build/rr/append)`. They do not depend on the kernel build.
- `replay-gdb` loads `build/rr/kernel.elf` with the usual `init.gdb`, so the GDB helpers match the recorded kernel. Reverse execution comes from QEMU's gdbstub in replay mode.
- `replay-check` records and then replays `panic=shutdown ktest` headless, each under `tools/timeout.py 300`. It requires `KTEST RESULT: PASS` in the recording and byte-identical console logs (`cmp`); on a mismatch it prints the first 20 lines of the `diff`.

## Concurrency and locking

| Data | Writers | Protection / rule |
|---|---|---|
| `trace_bufs[c]` | only CPU `c`, with interrupts off | Single writer, no atomics. Readers use a margin of 64 (`trace_print`) or 256 (`trace_dump_file`) records. The GDB reader assumes a halted machine. |
| `trace_groups_enabled` | monitor, `trace_init` | Plain word. Readers may see the old mask for a while. |
| `prof[c].buf/used` | only CPU `c` in its timer interrupt | `used` is published with a release store and read with an acquire load by `profile_report`. `samples`/`dropped` are plain. |
| `profiling` | `profile_start/stop` | Release store. The timer interrupt checks it without ordering. |
| `profile_report`'s static `funcs[]`/`comms[]` | callers of `profile_report` | procfs serializes its callers with `gen_lock`. The monitor does not, but runs with the world stopped. |
| `stop_owner` | `smp_stop_others` (CAS), `smp_resume_others` | One owner at a time. Others park in `cpu_park` until it is -1. |
| `panic_cpu` | `panic_begin` (CAS) | The first panicking CPU wins. Later CPUs park forever. |
| `log_buf`/`log_head` | `vklog` under `log_lock` (spinlock, interrupts off) | The lock is bypassed once `panic_depth > 0`. Lock-free readers (`dmesg_print`, GDB) may see a line being written. |
| monitor statics | the stop owner only | Only one CPU can be in the monitor. Nesting saves and restores the view state. |
| lockdep `classes/deps/edges` | `add_dep`, `class_of` | `ld_raw` (test-and-set, not tracked by lockdep). Reports are printed after dropping it. `lockdep_print` reads without it. |
| `ksyms_*`, `__ex_table`, `.ktests` | link time | Read-only (`.rodata`, and `.ksyms` after `.stacks`) |

**Rules for code that runs in the monitor or a panic:**
- Interrupts are off and other CPUs are parked wherever they were, possibly holding spinlocks, possibly in the middle of an update. Every print helper that walks a shared structure must use `spin_lock_debug` and tolerate inconsistent data. `cmd_net` says so explicitly when `net_lock` is held.
- Mutexes and sleeping are forbidden. Allocation is risky (see Limitations).
- The monitor's own output uses `kprintf`, which takes no lock.
- lockdep, the hard-lockup watchdog and the hung-thread checks look at `monitor_entry_time`/`smp_world_stopped()`, so the pause does not set them off. `smp_resume_others` and `cpu_park` refresh `last_tick` for the same reason.

**Interrupt context.** `trace()` is safe from any context, including the trap path and NMI-like spots, because it only disables interrupts locally. `profile_sample` runs in the timer interrupt and may only use non-faulting reads: `safe_read_u64` for kernel stacks, and `uvm_read` (a page-table walk plus a direct-map copy) for user stacks. Ctrl-] enters the monitor *from the UART interrupt handler* on cpu0, so commands run with `irq_depth > 0`.

**SMP ordering of stop and panic.** If the monitor owns the stop when another CPU panics, the panicking CPU's `smp_stop_others` fails and that CPU parks. When the monitor continues, the panicking CPU takes the stop and reports normally. If a CPU enters the monitor while a panic on another CPU owns the stop, it parks the same way.

## Interfaces

**Kernel API (other subsystems call these)**

| Function / macro | Header | Notes |
|---|---|---|
| `trace(ev, a0, a1, a2)`, `trace_mark("lit", a, b)` | `trace.h` | Any context. `trace_mark` needs a string literal. |
| `trace_print`, `trace_status`, `trace_clear`, `trace_set_groups`, `trace_dump_file` | `trace.h` | |
| `profile_start/stop/clear/report/dump_file`, `profile_sample`, `profile_boot_option` | `profile.h` | `profile_sample` is called only by `timer.c` |
| `ksym_lookup`, `ksym_find`, `ksym_format`, `ksym_count` | `debug.h` | Text symbols only |
| `backtrace_collect`, `backtrace_print`, `backtrace_here` | `debug.h` | `BACKTRACE_MAX` 32 |
| `hexdump_safe` | `debug.h` | |
| `panic`, `panic_trap`, `panic_print`, `assert`, `BUG_ON`, `BUG` | `debug.h` | `panic_depth` is exported |
| `monitor_enter(reason, tf, can_continue)` | `debug.h` | |
| `debug_break()`, `trap_breakpoint_hook` | `trap.h` | |
| `safe_read_*`, `safe_write_*`, `extable_lookup` | `safe.h` | |
| `semihost_open/write/close`, `SEMIHOST_MODE_WB` (5) | `irq.h` | |
| `KTEST`, `KT_EXPECT`, `KT_ASSERT`, `KT_EXPECT_EQ`, `KT_EXPECT_STREQ`, `ktest_run` | `ktest.h` | |
| `klog`, `vklog`, `pr_*`, `kprintf`, `dmesg_print`, `log_ring_copy`, `log_err_print`, `log_warn_print` | `printk.h` | |

**Boot options handled here:** `break`, `ktest[=prefix]`, `panic=shutdown`, `trace=<groups>`, `tracedump=<path>`, `profile[=hz]`, `lockdep=off|warn|panic`, `log=`, `quiet`.

**`/proc` files backed by this code:** `/proc/trace` (`trace_print`, 200 events), `/proc/profile` (`profile_report`) and `/proc/lockdep` (`lockdep_print`). `/dev/kmsg` is backed by `log_ring_copy`.

**System call:** `SYS_debug` (`syscall.c: do_debug`):
- `DBG_MARK` emits a `umark` trace event with 24 bytes of text and logs it;
- `DBG_BREAK` enters the monitor;
- `DBG_PROFILE` takes a sub-operation: 0 stop, 1 start(hz), 2 clear, 3 dump(path);
- the remaining operations are strace, ps, files, vm and fscrash.

**Monitor commands:** the full list is `commands[]` in `monitor.c` (50 entries). The ones implemented in this subsystem are `help regs csr bt mem poke sym dmesg log trace profile ktest crash cpu c reboot halt`. The others dispatch to their subsystems' printers.

**Host tools and make targets:**
- `make trace-json` and `eutrace.py text|perfetto DUMP [OUT] [--elf]`;
- `make flamegraph` and `euprof.py report|folded|svg PROFILE [OUT] [--kernel --user-dir --no-idle --cpu --pid --kernel-only]`;
- `make debug`/`gdb`, `make record`/`replay`/`replay-debug`/`replay-gdb`/`replay-check`, `make disasm`, `make qemu-log`.

**GDB commands:** `eu-help eu-dmesg eu-log eu-tf eu-platform eu-vm eu-meminfo eu-page eu-mm eu-swap eu-pagecache eu-slab eu-ps eu-cpus eu-lockdep eu-bt eu-trace eu-trace-dump eu-procs eu-bcache eu-files eu-net eu-profile-dump`.

## Debugging and observability

How to debug the debugging tools themselves, and the failure modes to expect:

- **The monitor does not respond to Ctrl-].** Ctrl-] is recognized in the UART interrupt handler (`console_rx_char`), and device interrupts go to cpu0. If cpu0 has interrupts off forever, or the PLIC/UART path is broken, the key never arrives. Use `make gdb` with `eu-cpus`, `eu-ps`, `eu-dmesg` and `eu-trace`, which only read memory. If the problem is the console line discipline on the host, see DEBUGGING.md's note about grc (`command make run`).
- **The monitor hangs right after a command.** The command probably took a lock that a parked CPU holds, or allocated memory. Attach GDB and `bt` on the monitor CPU's GDB thread (`eu-cpus` gives the number). The banner's `cpuN did NOT stop` tells you which CPU might still be running and changing state.
- **`regs` says "parked from a spin loop".** That CPU stopped through `ipi_poll` inside a lock spin, so no trap frame exists. `bt` still works from `stop_pc`/`stop_fp`.
- **Dumps fail** (`dump failed: QEMU semihosting not enabled?`, or `tracedump: semihosting unavailable`). QEMU was started without `-semihosting-config`. This is always the case under `make record`/`replay`, because `RROPTS` does not enable it. Use `eu-trace-dump` or `eu-profile-dump` from GDB instead.
- **Recursive panics.** A panic while printing a panic report (depth 2) prints a reduced report and enters a nested monitor. A third one halts with exit code 3 (`*** recursive panic; halting ***`). A stack overflow while handling a stack overflow halts with code 4. The first report is already in the log ring, so `eu-dmesg` can recover it.
- **Missing output after a panic.** `panic_print` writes to the ring as well as to the console (if `LOG_ERR <= console_level`), so `dmesg` in the post-mortem monitor shows the report even if the console scrolled.
- **Wedged log lock.** Before a panic, `vklog` takes `log_lock` with interrupts off. A CPU that dies holding it blocks all printk on other CPUs, but not `kprintf` (the monitor's output) and not panic output, which bypasses the lock.
- **Symbols look wrong.** Check that the build passed the two-pass check, and that the ELF you symbolize against is the one that ran. For replays that is `build/rr/kernel.elf`. `plat` shows the kernel range and the `ksyms` count, which is 0 if the table is empty.
- **Trace decodes as garbage on the host.** `eutrace.py` parses `trace_events.h` from the tree it runs in. If events were inserted or reordered since the dump, every type number after that point is wrong. A `rec_size` mismatch is reported explicitly.
- **Breakpoints in the kernel.** `debug_break()` enters the monitor, and `c` resumes after the `ebreak` (`trap_handler` adds the instruction length). With GDB attached, use hardware or software breakpoints instead. GDB's software breakpoints are handled by QEMU's gdbstub before the guest sees them.
- **Self-tests.** The `KTEST`s in `kernel/tests/test_debug.c` cover the debug infrastructure: `ksyms_lookup`, `backtrace_walks_frames`, `safe_access_recovers_from_faults`, `breakpoint_trap_and_resume` and `log_ring_and_thresholds`. Also relevant are `test_sched.c: trace_records_context_switches`, and in `test_smp.c`, `stop_the_world_parks_every_other_cpu` and `lockdep_catches_inversions_before_they_deadlock`. Run them with `make run APPEND="ktest=ksyms"` or similar prefixes, or with `ktest <prefix>` in the monitor (see Limitations).
- **Watching the fixup path.** `log=trap:trace` logs every exception-table fixup (`fault at ... fixed up -> ...`).

## Limitations and known issues

- **Only text symbols in `.ksyms`.** `gen_ksyms.py` keeps `t/T/W` only, and `ksym_lookup` rejects addresses at or above `__text_end`. So `sym`, `mem` and `parse_addr` cannot resolve data symbols such as `mem jiffies`, and data addresses print as bare hex.
- **`ksym_find` is a linear scan**, and returns the lowest-addressed of several static functions with the same name.
- **Monitor commands that allocate can wedge the monitor.** `trace dump` (`kmalloc` of 256 × 24 bytes, which is more than 2 KiB and so comes from the page allocator), `profile report` (17-byte name copies), `profile start` (2 MiB per CPU) and `kmalloc`/`kfree` all use ordinary spinlocks. A CPU parked from a spin loop may hold the slab or page-allocator lock.
- **`kill` in the monitor can send a signal without `proc_lock`.** `cmd_kill` calls `signal_send_locked` even when `spin_lock_debug` could not get the lock. `signal_send_locked` allows this while the world is stopped. The intent is to avoid blocking, but it can race with a parked holder that is in the middle of an update.
- **`ktest` from the monitor** runs tests with interrupts off and other CPUs parked, and for Ctrl-] entry, inside an interrupt handler. Tests that sleep, create threads or need other CPUs cannot behave as they do at boot (`ktest=`).
- **Trace dumps from the kernel always omit the oldest 256 records per CPU**, even when the world is stopped, and include at most 256 threads.
- **`trace_clear` races with writers.** It stores `head = 0` from one CPU while other CPUs do non-atomic `head++`, so a concurrent increment can undo the clear for that CPU.
- **Trace dumps are not self-describing.** There is no event table in EUTRACE1, so decoding depends on the source tree's `trace_events.h`. Inserting an event in the middle of the list breaks older dumps.
- **The two trace decoders disagree.** `eutrace.py` lacks the `_ip`, `tcp_*`, `proto`, `cpu` and `ipi` formats that `trace.c: fmt_arg` has, so `net_rx`, `tcp_state`, `ipi_send` and others show raw hex on the host. Its `str` decoding also only resolves strings stored in the kernel ELF.
- **Profiler rate.** `timer_set_profile_hz` gives an actual rate of `HZ * (hz / HZ)`, while `profile_hz` and the EUPROF1 header record the requested `hz`. For example, `profile start 250` samples at 200 Hz but reports 250.
- **`profile_dump_file` leaves profiling stopped if `semihost_open` fails.** It stops profiling first and returns -1 before reaching the restart.
- **The profiler cannot see code running with interrupts off.** There is no NMI on this platform. Such time is charged to the instruction where interrupts are re-enabled.
- **Profile buffers never wrap.** Once 2 MiB per CPU are used, every new sample is dropped. The in-kernel report caps the tables at 256 functions and 64 process names and silently ignores the rest.
- **User frames in other threads' backtraces are symbolized against the wrong program.** `format_return` symbolizes user addresses using `current_proc()`, not the process of the thread being unwound. `bt <tid>` or `cpu <n>; bt` on another process's thread can show the user pc as hex or with names from the wrong program.
- **Only one nested panic is tolerated** in a post-mortem session. `panic_depth` never decreases, and depth 3 halts.
- **The GDB helpers have no tests.** They depend on struct and field names through DWARF (`eu-cpus` also hard-codes 8 held locks, which equals `MAX_HELD_LOCKS` today). A rename shows up only when a command is run. The comment at the end of `eunuchs.py` says it also stops "whenever the kernel ... enters its monitor", but only `panic` and `panic_trap` get breakpoints.
- **Record/replay** works with one CPU only and has no semihosting. `rr_prepare` writes `APPEND` with `echo '$(1)'`, so boot options containing a single quote break it.
- **Stale documentation.** DEBUGGING.md lists "at the end of boot" as a monitor entry path, but `kmain`/`kinit` contain no such call. The only entries are those in the table above.

## Potential improvements

1. **Make trace dumps self-describing.** Append the event table (names, groups, argument names) to EUTRACE1 (version 2), generated from `TRACE_EVENTS(X)`. `eutrace.py` and `eu-trace` would prefer it over parsing `trace_events.h`, so old dumps and replays of old kernels decode correctly after events change. Impact: medium. Effort: small.
2. **Share one decoding table between the kernel and Python.** Bring `eutrace.py: Trace.fmt_arg` up to date with `trace.c: fmt_arg` (`_ip`, `tcp_*`, `proto`, `cpu`, `ipi`), and add a host test that decodes a known dump in both, so `net` and IPI events stop showing as hex in Perfetto. Impact: medium. Effort: small.
3. **No allocation in monitor paths.** Give `trace_dump_file` a static thread-snapshot buffer, intern names in `profile_report` into a static pool, and refuse `profile start` and `kmalloc` while `smp_world_stopped()` (or use a trylock allocation). This removes the main way the monitor can wedge itself. Impact: high. Effort: small.
4. **Monitor commands as a linker section.** Register commands with a `MONITOR_CMD(name, args, help)` macro in a `.moncmds` section, as `KTEST` does, so subsystems can add commands next to their `*_print` functions without editing `monitor.c`'s 50-entry table. Impact: medium. Effort: medium.
5. **Data symbols in `.ksyms`.** Also emit `d/D/b/B/r/R` symbols, with sizes or a separate table, and extend `ksym_lookup`'s range check. Then `sym jiffies`, `mem log_buf` and data addresses in reports become readable. Impact: medium. Effort: small.
6. **Symbolize user frames with the right process.** Pass the target `struct proc *` into `backtrace_print`/`format_return` (from `thread_backtrace`, `cpu_backtrace` and panics of other CPUs) instead of using `current_proc()`. Impact: low. Effort: small.
7. **Fix the profiler rate and dump edge cases.** Store the effective rate `HZ * per_tick` in `profile_hz` (or reject rates that are not multiples of `HZ`), restart profiling on the failure path of `profile_dump_file`, and fix the identical branches in `profile_stop`'s message. Impact: low. Effort: small.
8. **Full-ring trace dumps when stopped.** In `trace_dump_file`, use the full 8192 records when `smp_world_stopped()` or `panic_depth`, and make `trace_clear` per-CPU (each CPU clears its own ring through an IPI, or a generation counter) to remove the clear/increment race. Impact: low. Effort: small.
9. **A GDB helper smoke test.** Add `make gdb-check`: boot with `break`, attach GDB in batch mode, run every `eu-*` command, and fail on Python exceptions. Add it to `make ci` so struct renames are caught at once. Impact: medium. Effort: medium.
10. **Profiler ring mode.** Optionally let the per-CPU buffer wrap (keep the newest samples) so that `profile` at boot followed by `eu-profile-dump` after a hang shows the minutes before the hang, not the first minute of boot. Impact: medium. Effort: medium.
11. **A safer `ktest` in the monitor.** Tag tests that need the scheduler or SMP (a flag in `struct ktest`) and skip them when `ktest_run` is called with the world stopped, or have the monitor spawn a kthread to run the tests after `c`. Impact: low. Effort: medium.
12. **Per-test timing and leak checks in the runner.** Have `ktest_run` record each test's duration and check generic invariants after each test (free pages, slab active counts, held spinlocks, `nheld_mutex`), the way some FS tests do by hand. Impact: medium. Effort: medium.
13. **Semihosting in replays, or a GDB fallback.** Add `-semihosting-config` to `RROPTS` if QEMU's replay supports it. Otherwise make the monitor's `trace dump` and `profile dump` print the matching `eu-*-dump` hint when semihosting fails under `-icount`. Impact: low. Effort: small.
14. **Faster `ksym_find`.** Emit a second index sorted by name in `gen_ksyms.py` and binary-search it. This matters for address parsing in scripts and for any future symbol-based breakpoint command. Impact: low. Effort: small.
