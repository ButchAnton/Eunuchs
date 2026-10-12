# Traps, interrupts, timers and system calls

Every entry into the Eunuchs kernel after boot goes through one assembly trap vector, `kernel_trap_entry`. It saves a full register frame and calls `trap_handler`, which sorts the trap into an interrupt (timer, PLIC device interrupt, IPI), a system call, a user fault, or a kernel fault. On the way back to user mode the same path reschedules, delivers signals and restarts interrupted system calls. This document covers that path, the per-CPU timer tick and lockup watchdog, the PLIC driver, kernel stack overflow detection, the fake frame records that let backtraces cross traps, and the system call ABI and table.

## Responsibilities and scope

This subsystem:

- installs the trap vector (`stvec`, direct mode) on every hart;
- saves and restores a `struct trap_frame` for traps from user and kernel mode, keeps `tp`/`sscratch` consistent, and builds a fake frame record plus CFI so backtraces (the kernel's and GDB's) continue through traps;
- detects kernel stack overflow at trap entry and reports it from a per-CPU emergency stack;
- dispatches interrupts: the supervisor timer (`timer.c`), external interrupts through the PLIC (`plic.c`), and software interrupts (IPIs, handled in `smp.c`);
- programs the periodic tick through SBI, keeps `jiffies`, and runs the hard-lockup watchdog;
- dispatches `ecall` system calls through a table generated from `lib/include/uapi/syscall.h`, and implements strace;
- handles user exceptions (demand paging, copy-on-write, enabling floating point on a thread's first FP instruction, fatal faults that become signals) and kernel exceptions (lazy kernel page-table sync, exception-table fixups, breakpoints, otherwise panic);
- runs the "return to user" checks: preemption, signal delivery, system call restart;
- provides `cpu_wait_ticks` (a `wfi`-based sleep for code that polls with interrupts off) and semihosting calls (host file I/O for trace and profile dumps).

It does **not** own:

- the scheduler (`kernel/core/sched.c`: `schedule`, `preempt_schedule`, `sched_tick`) — it only calls into it;
- IPIs and stop-the-world (`kernel/arch/riscv/smp.c`: `ipi_handle`, `cpu_park`) — it only routes software interrupts there;
- page-fault resolution (`kernel/mm/uvm.c: uvm_fault`, `kernel/mm/vmm.c: vm_kernel_fault_fixup`) and user-memory access (`uaccess.S`, `safe.S` provide the exception-table entries);
- signal semantics (`kernel/proc/signal.c`); only its interaction with the trap return path is described here;
- the system call handlers themselves, which live with their subsystems (`fs/file.c`, `net/socket.c`, `proc/*.c`). The table below lists where.
- panic reporting and the monitor (`kernel/core/panic.c`, `kernel/debug/monitor.c`), which consume trap frames.

## Source map

| File | Role |
|---|---|
| `kernel/arch/riscv/trapentry.S` | The trap vector `kernel_trap_entry`: stack selection, overflow check, frame save/restore, fake frame record, CFI, `sret`. Also `user_return` (first entry to user mode). |
| `kernel/arch/riscv/trap.c` | `trap_init`, `trap_handler` (the C dispatcher), `handle_interrupt`, `user_exception`, `return_to_user_checks`, `stack_overflow`, `extable_lookup`, semihosting `ebreak` recognition, `trap_dump`, `trap_print_counts`, cause names, exception counters. |
| `kernel/include/trap.h` | `struct trap_frame`, `TRAP_FRAME_SIZE`, `TF_RA/TF_SP/TF_FP`, `trap_breakpoint_hook`, `debug_break()`. |
| `kernel/include/riscv.h` | CSR accessors, `sstatus`/`sie` bits, `enum exc_code`, `enum irq_code`, `rdtime`, `intr_on/off/enabled`, `current_fp`. |
| `kernel/include/percpu.h` | `struct cpu` and the `CPU_*` offsets used by `trapentry.S`. |
| `kernel/include/mm.h` | `KSTACK_SLOT`, `KSTACK_SIZE`, `KSTACK_GUARD_BIT` (overflow detection). |
| `kernel/arch/riscv/timer.c` | Per-CPU periodic tick through SBI, `jiffies`, missed-tick accounting, profiler rate, hard-lockup watchdog. |
| `kernel/arch/riscv/plic.c` | PLIC setup, `irq_register`, claim/complete loop `plic_handle`, `irq_print` (`/proc/interrupts`, monitor `irq`). |
| `kernel/include/irq.h` | PLIC/timer/semihosting prototypes, `MAX_IRQS` (64), `HZ` (100). |
| `kernel/arch/riscv/idle.c` | `cpu_wait_ticks`: `wfi` with only the timer enabled, for polling loops with interrupts off. |
| `kernel/arch/riscv/semihost.c` | QEMU semihosting (`SYS_OPEN`, `SYS_WRITE`, `SYS_CLOSE`). |
| `kernel/proc/syscall.c` | `syscall_dispatch`, the system call table, strace, `errno_name`, the simple system calls (`exit`, `brk`, `mmap`, `debug`, ...). |
| `lib/include/uapi/syscall.h` | The `SYSCALLS(X)` X-macro: numbers, names, argument counts, strace formats. `SYS_debug` operations, `mmap` flags. |
| `lib/include/uapi/errno.h` | The `ERRNOS(X)` X-macro (Linux values, names, descriptions). |
| `user/lib/syscall.S` | User `__syscall` stub and the `__restore_rt` signal trampoline. |
| `user/lib/crt0.S` | User `_start`: sets `gp`, ends the frame chain, calls `__libc_start`. |
| `kernel/proc/signal.c` | `signal_deliver`, `syscall_restart_check`, `do_rt_sigreturn` (only the trap-return interaction is covered here). |
| `kernel/arch/riscv/boot.S` | Defines `boot_stack` and `overflow_stack` in guard-padded slots (section `.stacks`). |
| `kernel/arch/riscv/fp.S` | `fp_save_regs` / `fp_load_regs`: copy the user FP registers (`f0`–`f31`, `fcsr`) between the hardware and a `struct fpstate`. The only kernel code with FP instructions (it enables the D extension for itself). |

## Concepts and data structures

### The trap frame

`struct trap_frame` (`kernel/include/trap.h`) must match the offsets hard-coded in `trapentry.S`:

```c
struct trap_frame {
    u64 regs[32];   /* x0..x31; regs[2] is the interrupted sp */
    u64 sepc;       /* offset 256 */
    u64 sstatus;    /* offset 264 */
    u64 scause;     /* offset 272 */
    u64 stval;      /* offset 280 */
};
#define TRAP_FRAME_SIZE 304   /* frame + 16-byte fake frame record */
```

On the stack the frame is followed by a 16-byte fake frame record. The whole block is 304 bytes (`FRAME_SIZE` in `trapentry.S`, `TRAP_FRAME_SIZE` in C; `kernel/core/sched.c` has `_Static_assert(sizeof(struct trap_frame) + 16 == TRAP_FRAME_SIZE)`).

```
 higher addresses
 +------------------------+  <- s0 (fp) set by trap entry = sp + 304
 | fake ra   = sepc       |  sp + 296   (fp - 8)
 | fake prev fp           |  sp + 288   (fp - 16): interrupted s0, or 0 for user traps
 +------------------------+
 | stval                  |  sp + 280
 | scause                 |  sp + 272
 | sstatus                |  sp + 264
 | sepc                   |  sp + 256
 | x31 (t6)               |  sp + 248
 | ...                    |
 | x2  (sp, interrupted)  |  sp + 16
 | x1  (ra)               |  sp + 8
 | x0  (always 0)         |  sp + 0
 +------------------------+  <- sp passed to trap_handler (a0)
```

Field notes:

- `regs[2]` is the stack pointer at the time of the trap (user `sp` for user traps; `sp + FRAME_SIZE` for kernel traps — corrected by `stack_overflow` if the entry switched to the emergency stack).
- `regs[4]` (`tp`) holds the user's `tp` for user traps. For kernel traps it is overwritten on return with the *live* `tp` (see below).
- `sepc`, `sstatus` are written back to the CSRs on return, so handlers change the resume point and mode by editing the frame (skipping `ecall`/`ebreak`, exec, signal delivery, `rt_sigreturn`).
- `sstatus.SPP` in the frame says where the trap came from (0 = user). `sstatus.SPIE` says whether the interrupted code had interrupts enabled; preemption relies on it.

Helper macros: `TF_RA(tf)`, `TF_SP(tf)`, `TF_FP(tf)` (`regs[1]`, `regs[2]`, `regs[8]`).

**Where frames live.** A trap from user mode always puts its frame at the very top of the current thread's kernel stack. `kernel/core/sched.c: thread_user_tf` computes it as `kstack + kstack_size - TRAP_FRAME_SIZE`; this is how `fork`, `execve`, `rt_sigreturn`, `DBG_BREAK`, procfs and the profiler find a thread's user registers. A trap from kernel mode pushes its frame on whatever stack was in use.

### Per-CPU fields used by the trap path

`struct cpu` (`kernel/include/percpu.h`). The first five fields have fixed offsets because `trapentry.S` uses them; `sched.c` checks them with `_Static_assert`s.

| Field | Offset / macro | Use |
|---|---|---|
| `cur_thread` | 0 | `current` is `ld 0(tp)`. |
| `overflow_stack_top` | `CPU_OVERFLOW_STACK` (8) | Emergency stack for kernel stack overflows. cpu0: `overflow_stack` in `boot.S` (set in `cpu_early_init`); others: a `kstack_alloc()` stack (`smp.c: start_cpu`). |
| `overflow_sp` | `CPU_OVERFLOW_SP` (16) | 0 normally; the `sp` at an overflowing trap otherwise. Never reset. |
| `scratch` | `CPU_SCRATCH` (24) | Spill slot for `t0` during entry. |
| `kstack_top` | `CPU_KSTACK_TOP` (32) | Top of the running thread's kernel stack, updated at every context switch (`sched.c`) — where user traps put their frame. |
| `irq_depth` | — | > 0 inside `handle_interrupt`; read by `in_interrupt()`. |
| `irq_tf` | — | Frame of the context the current interrupt interrupted (used by the profiler). |
| `need_resched` | — | Set by `sched_tick` and the resched IPI; consumed on interrupt exit and on return to user. |
| `in_overflow` | — | Set once an overflow is being reported; a second one halts the machine. |
| `irq_count[16]` | — | Interrupts taken, by `scause` code. |
| `last_irq` | — | PLIC source being handled (for the `irq_exit` trace event). |
| `next_deadline`, `ticks`, `subticks`, `missed_ticks`, `last_tick`, `lockup_reported` | — | Timer and watchdog state (below). |

### The `sscratch` convention

| Mode | `sscratch` | `tp` |
|---|---|---|
| User | this CPU's `struct cpu *` | user's `tp` |
| Kernel | 0 | this CPU's `struct cpu *` |

At entry, `csrrw tp, sscratch, tp` swaps the two. If the new `tp` is non-zero the trap came from user mode; otherwise the entry swaps back (`csrrw tp, sscratch, zero`). This works whatever value the user put in `tp`.

### Kernel stacks and the guard bit

All kernel stacks — thread stacks from `kstack_alloc`, `boot_stack`, `overflow_stack`, and the secondary CPUs' emergency stacks — are 16 KiB (`KSTACK_SIZE`) in the upper half of a 32 KiB-aligned slot (`KSTACK_SLOT`) whose lower half is an unmapped guard (`kernel/include/mm.h`). So every valid kernel `sp` has bit 14 (`KSTACK_GUARD_BIT`) set, and an `sp` with bit 14 clear is in the guard. Trap entry tests `sp - (FRAME_SIZE + 256)`: if that address would land in the guard, the trap switches to the emergency stack.

### Fake frame records and CFI

The kernel is built with frame pointers. GCC stores `{prev fp, ra}` at `fp - 16` and `fp - 8`. Trap entry writes the same record above the frame (prev fp = interrupted `s0`, ra = `sepc`) and sets `s0 = sp + FRAME_SIZE`. Consequences:

- `kernel/debug/backtrace.c: backtrace_collect` walks from `trap_handler` through the trap into the interrupted function, with no special casing. For user traps the prev fp is 0, which ends the chain after the user `pc`; `format_return` then symbolizes it against the process image as `[user name] sym+off`.
- `kernel/core/sched.c: thread_where` and `kernel/debug/profile.c` recognize the `kernel_trap_entry` symbol in a chain to say "preempted in ..." or to switch to user frames.

For GDB, `kernel_trap_entry` carries DWARF CFI: `.cfi_signal_frame` (so GDB shows `<signal handler called>` and does not subtract 1 from the pc), `.cfi_return_column 64` with `.cfi_offset 64` pointing at the saved `sepc`, and `.cfi_offset` rules for `sp` and every saved GPR. The CFI rules only take effect after `.Lsave_csrs` (once the frame is complete).

### Exception and interrupt codes

`kernel/include/riscv.h`:

| `enum exc_code` | Value | Handling (from user / from kernel) |
|---|---|---|
| `EXC_INSN_MISALIGNED` | 0 | SIGBUS / panic |
| `EXC_INSN_ACCESS` | 1 | SIGSEGV / panic |
| `EXC_ILLEGAL_INSN` | 2 | first FP instruction: enable FP for the thread; otherwise SIGILL / panic |
| `EXC_BREAKPOINT` | 3 | SIGTRAP (a tracer stops the process) / semihosting fallback, `trap_breakpoint_hook`, or monitor |
| `EXC_LOAD_MISALIGNED`, `EXC_STORE_MISALIGNED` | 4, 6 | SIGBUS / exception-table fixup or panic |
| `EXC_LOAD_ACCESS`, `EXC_STORE_ACCESS` | 5, 7 | SIGSEGV / exception-table fixup or panic |
| `EXC_ECALL_U` | 8 | system call |
| `EXC_ECALL_S` | 9 | — / panic |
| `EXC_INSN_PAGE_FAULT`, `EXC_LOAD_PAGE_FAULT`, `EXC_STORE_PAGE_FAULT` | 12, 13, 15 | `uvm_fault`, else signal / kernel page-table sync, uaccess paging, fixup, or panic |
| `EXC_SW_CHECK`, `EXC_HW_ERROR` | 18, 19 | SIGSEGV / panic |

| `enum irq_code` | Value | Handler |
|---|---|---|
| `IRQ_S_SOFT` | 1 | `smp.c: ipi_handle` |
| `IRQ_S_TIMER` | 5 | `timer.c: timer_interrupt` |
| `IRQ_S_EXT` | 9 | `plic.c: plic_handle` |
| `IRQ_COUNTER` | 13 | not expected; masked on first occurrence |

### Interrupt enables

| `sie` bit | Set by | CPUs |
|---|---|---|
| `SIE_STIE` (timer) | `timer.c: timer_init_cpu` | all |
| `SIE_SSIE` (IPI) | `smp.c: secondary_main`, `smp_init` | all |
| `SIE_SEIE` (external) | `plic.c: plic_init` | boot CPU (cpu0) only |

### The system call table

`lib/include/uapi/syscall.h` defines one line per call:

```c
X(64,  write,       3, "dbu")
```

= number, name, argument count, and an strace format string (one letter per argument: `d` signed, `u` unsigned, `x` hex, `p` pointer, `s` user string, `b` user buffer whose length is the next argument, `a` argv array, `k` signal, `o` octal, `n` `struct sockaddr_in`). The same macro produces:

- `enum { SYS_<name> = nr }` for the kernel and libc;
- prototypes `long do_<name>(const u64 *a)` and a sparse table `table[SYSCALL_MAX]` (512 entries) in `kernel/proc/syscall.c`;
- strace decoding in the kernel, and `tools/trace/eutrace.py` parses the file (keep one entry per line).

Numbers follow Linux/RISC-V where an equivalent exists; 500 and up are Eunuchs-specific. One deviation: 220 is `fork` with no arguments (Linux uses 220 for `clone`).

`lib/include/uapi/errno.h` similarly defines `ERRNOS(X)` with Linux values; `errno_name` and the static `errno_desc` in `syscall.c` are generated from it. `ERESTARTSYS` (512) is kernel-internal (`kernel/include/proc.h`) and never reaches user space.

### Per-process system call state

In `struct proc` (`kernel/include/proc.h`):

| Field | Meaning |
|---|---|
| `syscalls` | Count of system calls made (shown by `ps`/`procs`). |
| `strace` | 0 off, 1 on (kept across exec), 2 on and inherited by forked children. |
| `strace_buf[192]` | The formatted call, built at entry and printed with the result at exit. |
| `syscall_restart`, `restart_a0` | The last call returned `-ERESTARTSYS`; the original `a0` to restore if it is re-run. |
| `nanosleep_restarting`, `nanosleep_deadline` | A restarted `nanosleep` sleeps only to the original deadline. |

## How it works

### Trap entry (`trapentry.S: kernel_trap_entry`)

Hardware has already set `sepc`, `scause`, `stval`, copied `SIE` to `SPIE`, cleared `SIE` (interrupts are off for the whole handler unless C code turns them on), and set `SPP`.

**From user mode** (`.Lfrom_user`):

1. `tp` = `struct cpu *` (swapped from `sscratch`); `sscratch` now holds the user `tp`.
2. Spill `t0` to `cpu->scratch`, save user `sp` in `t0`, load `sp = cpu->kstack_top - FRAME_SIZE`, store the user `sp` at `regs[2]`, reload `t0`.
3. Save x1, x3–x31 (`SAVE_GPRS`), then overwrite `regs[4]` with the user `tp` from `sscratch` and set `sscratch = 0` (now "in kernel").
4. Fake frame record prev fp = 0.

**From kernel mode:**

1. Swap back so `tp` is the CPU again and `sscratch` stays 0.
2. Spill `t0` to `cpu->scratch`; compute `sp - (FRAME_SIZE + 256)` and test bit `KSTACK_GUARD_BIT`. If clear, the frame (plus 256 bytes of headroom for the handler) would reach the guard: store `sp` in `cpu->overflow_sp` and load `sp = cpu->overflow_stack_top`.
3. Push the frame, `SAVE_GPRS`, store `sp + FRAME_SIZE` as the interrupted `sp`, prev fp = interrupted `s0`.

**Common** (`.Lsave_csrs`): store `regs[0] = 0`, `sepc`, `sstatus`, `scause`, `stval`; fake ra = `sepc`; `s0 = sp + FRAME_SIZE`; `call trap_handler(sp)`.

### Trap return (`.Ltrap_return`)

1. Write the frame's `sepc` and `sstatus` back to the CSRs (handlers may have changed them). The saved `sstatus` has `SIE = 0`, so interrupts stay off until `sret` sets `SIE = SPIE`.
2. If `SPP` = 0 (going to user): `sscratch = tp` (the live CPU pointer — the thread may have migrated while in the kernel).
3. If `SPP` = 1 (going back into the kernel): overwrite the saved `tp` with the live one. The interrupted thread may have been preempted inside this trap and resumed on another CPU; restoring the old `tp` would point it at the wrong `struct cpu`.
4. Restore x1, x3–x31, then `sp` last, and `sret`.

`user_return(tf)` jumps straight to `.Ltrap_return` with `sp = tf`. `kernel/proc/proc.c` uses it, with interrupts off, to enter user mode for the first time after `exec` (`user_entry`) and in a forked child (`fork_child_entry`). The frames they use were built by `do_exec` (`sstatus` with `SPP`, `SIE`, `SUM` and `FS` cleared and `SPIE` set; `a0` = argc, `a1` = argv, `a2` = envp) or copied from the parent (`sys_fork`, with `a0 = 0`).

### `trap_handler` (`kernel/arch/riscv/trap.c`)

```
trap_handler(tf)
  if this_cpu()->overflow_sp: stack_overflow(tf)            -- never returns
  if interrupt:
      handle_interrupt(tf, code)
      if from user: return_to_user_checks(tf)
      return
  exception_counts[code]++ (atomic)
  if from user: user_exception(tf, code); return_to_user_checks(tf); return
  kernel exception:
      page fault on a kernel address and vm_kernel_fault_fixup()  -> return (retry)
      memory fault:
          uaccess instruction (extable entry), user address, load/store page fault,
            and uvm_fault() == 0                                   -> return (retry)
          extable fixup exists                                     -> sepc = fixup; return
      breakpoint:
          semihosting sequence                                     -> a0 = -1, skip; return
          trap_breakpoint_hook handled it, or monitor_enter()      -> skip ebreak; return
      panic_trap(tf)
```

### Interrupts (`handle_interrupt`)

1. Count the interrupt in `cpu->irq_count[code]`, increment `irq_depth`, save the outer `irq_tf` and set `irq_tf = tf`, clear `last_irq`, emit trace event `irq_entry`.
2. Dispatch: `timer_interrupt`, `plic_handle`, or `ipi_handle(tf)`. Any other code is masked in `sie` with a warning (`unexpected <cause> (scause=...); masked`).
3. Emit `irq_exit` (with the PLIC source), restore `irq_tf`, decrement `irq_depth`.
4. **Preemption:** if `need_resched` is set, `irq_depth == 0`, `sched_ready`, and the frame's `SPIE` is set, call `preempt_schedule()`. `SPIE` set means the interrupted code had interrupts on, so it holds no spinlock (spinlocks disable interrupts). This makes the kernel fully preemptible wherever interrupts are enabled, including inside system calls. Traps from user mode always have `SPIE` set.

Handlers run with interrupts off and do not nest. The thread switched to by `preempt_schedule` runs on its own stack; this trap frame stays on the preempted thread's stack until it is scheduled again, possibly on another CPU.

### The timer (`kernel/arch/riscv/timer.c`)

- `timer_init` (boot CPU, from `kmain`): `interval = platform.timebase / HZ` (HZ = 100; QEMU's 10 MHz timebase gives 100,000 ticks), reads the `watchdog=<seconds>` boot option (default 10, 0 = off), sets `timer_running`, calls `timer_init_cpu`. Secondary CPUs call `timer_init_cpu` from `secondary_main`.
- `timer_init_cpu`: `next_deadline = now + interval`, `sbi_set_timer(next_deadline)`, enable `SIE_STIE`.
- `timer_interrupt`, per CPU:
  1. Advance `next_deadline` by `interval`. If it is already in the past, add the number of skipped intervals to `missed_ticks` and rearm at `now + interval` (no catch-up burst).
  2. `sbi_set_timer` (this also clears the pending timer interrupt), record `last_tick = now` (watchdog heartbeat).
  3. If the profiler is on, `profile_sample(c->irq_tf)`.
  4. Only every `per_tick`-th interrupt is a scheduler tick: then `jiffies++` (cpu0 only), `c->ticks++`, `watchdog_check()`, `sched_tick()` (wakes expired sleepers and sets `need_resched` unless the CPU is idle with an empty run queue).
- `timer_set_profile_hz(hz)` (called by the profiler): `per_tick = hz / HZ` (at least 1), `interval = timebase / (HZ * per_tick)`. So the profiling rate is rounded down to a multiple of 100 Hz and the scheduler still ticks at 100 Hz. `hz = 0` restores the normal rate.
- `timer_rearm`: restore the tick after `cpu_wait_ticks` borrowed the timer.

Time itself comes from `rdtime` everywhere (`uptime_ms`, `clock_gettime(CLOCK_MONOTONIC)`, sleep deadlines), so lost ticks do not skew time. `jiffies` is only a tick counter.

**Hard-lockup watchdog** (`watchdog_check`, every scheduler tick on every CPU): skipped if `lockup_ticks` is 0, fewer than 2 CPUs are online, the world is stopped, the monitor is active (`monitor_entry_time`), or a panic is in progress. For each other CPU that is online and not stopped, if `now - last_tick >= watchdog seconds`, claim `lockup_reported` with a CAS (one report per CPU) and panic: `watchdog: hard lockup on cpuN: no timer interrupt for M ms (interrupts off); it is running thread T (name); holding 'lock', ...`. The panic stops the other CPUs; a stuck CPU spinning on a lock answers the stop IPI from its spin loop, so the report shows where it is.

### Idle waiting (`kernel/arch/riscv/idle.c: cpu_wait_ticks`)

Used by polling code that runs with interrupts off (the console input loop of the monitor, `kernel/core/console.c`). It saves `sie`, enables only `SIE_STIE`, sets the SBI timer to `now + ticks`, and executes `wfi`. `wfi` wakes on a pending interrupt that is enabled in `sie` even when `sstatus.SIE` is off, so no trap is taken. Only the timer is enabled so that a device interrupt left pending in the PLIC does not turn the wait into a busy loop. Afterwards it hands the timer back (`timer_rearm`, or `sbi_set_timer(~0)` before the tick is running) and restores `sie` and `SIE`.

The normal idle loop (`kernel/core/sched.c: cpu_idle_loop`) is separate: interrupts off, `wfi` if the run queue is empty and `need_resched` is clear, interrupts on (the pending interrupt is taken there), `schedule()`.

### Device interrupts (`kernel/arch/riscv/plic.c`)

- `plic_init` (boot CPU): context = `2 * platform.boot_hart + 1` (the S-mode context of the boot hart on QEMU `virt`). Maps only three pages with `ioremap`: the priority page, the page with this context's enable bits, and the context's threshold/claim page. Sets the threshold to 0 and enables `SIE_SEIE` on this CPU only. All device interrupts therefore go to cpu0: there are no claim races and a handler never runs concurrently with itself.
- `irq_register(irq, name, handler, arg)`: fails if there is no PLIC, `irq` is 0 or ≥ `MAX_IRQS` (64), or already registered. Stores the descriptor, sets priority 1, and sets the enable bit. Callers: UART (`kernel/core/console.c`), virtio-blk, virtio-net.
- `plic_handle`: loop claim → if registered, call the handler and accumulate `count` and `time` (rdtime ticks) → else count `spurious` → complete, until the claim register reads 0. `cpu->last_irq` records the source for tracing.

### System calls

**User side.** `user/lib/syscall.S: __syscall(nr, a0..a5)` shifts its arguments down one register (`a7 = nr`), executes `ecall` and returns `a0`. `user/lib/libc.c: ret()` turns results in `(-4096, 0)` into `errno` and -1. `__restore_rt` (`li a7, 139; ecall`) is the `sa_restorer` that libc installs for every handler.

**Kernel side** (`kernel/proc/syscall.c: syscall_dispatch`, reached from `user_exception` on `EXC_ECALL_U`):

1. `nr = a7`; copy `a0..a5` into a local `args[6]` (handlers receive a pointer to this copy, not to the frame).
2. `sepc += 4` (resume after the `ecall`), `p->syscalls++`.
3. **`intr_on()`** — system calls run with interrupts enabled and are preemptible.
4. Trace event `syscall_enter(nr, arg0, arg1)`.
5. If `nr` is out of range or has no handler: `pr_warn("[pid name] unknown system call N")`, result `-ENOSYS`. Otherwise call `table[nr].fn(args)`, wrapped in `strace_enter`/`strace_exit` if `p->strace` was set *before* the call (a call that toggles strace is logged consistently).
6. Trace event `syscall_exit(nr, ret)`.
7. If `ret == -ERESTARTSYS`: `p->syscall_restart = true`, `p->restart_a0 = args[0]`.
8. `a0 = ret`.

Handlers that need the full user register state use `thread_user_tf(current)`: `fork` copies it, `execve` replaces it (and returns `argc`, which becomes the new program's `a0`), `rt_sigreturn` restores it, `DBG_BREAK` passes it to the monitor.

`exit` never returns; strace prints it at entry as `= ?`.

### User exceptions (`user_exception`)

| Cause | Action |
|---|---|
| `ecall` | `syscall_dispatch` |
| instruction/load/store page fault | `intr_on`, `uvm_fault(p->mm, stval, FAULT_EXEC/READ/WRITE)`, `intr_off`, trace `user_fault`. On failure `user_fault(tf, reason, sig)`: `-ENOMEM` → "out of memory", SIGBUS; `-EACCES` → "permission denied", SIGSEGV; else "no mapping", SIGSEGV. |
| breakpoint | `user_fault(tf, "ebreak", SIGTRAP)` with `sepc` still on the `ebreak`. A traced process stops for its debugger ([14](14-debugger.md)); an untraced one with a SIGTRAP handler gets it; otherwise the program is killed with the usual fault report, as on other Unixes. (Until the debugger stage this entered the kernel monitor; `dbg_break()`, i.e. `SYS_debug`/`DBG_BREAK`, is now the way into the monitor from user code.) |
| illegal instruction | If `sstatus.FS` is off in the trap frame and the thread has not used floating point yet (`!current->fp_used`), this is the program's first FP instruction: zero `current->fp`, set `fp_used`, load the zeroed registers (`fp_load_regs`), set `FS` = Initial in the frame, and return so the instruction is retried. Otherwise SIGILL "illegal instruction". See [lazy floating point](#lazy-floating-point-for-user-programs). |
| misaligned | SIGBUS "misaligned access". |
| anything else | SIGSEGV "bad access". |

`kernel/proc/proc.c: user_fault` either queues the signal (if the program has a handler and has not blocked it — delivered on the way out) or prints a diagnosis (NULL dereference, kernel address, user stack overflow, VMA permissions), a symbolized user backtrace and the memory map, and kills the process with the signal and core flag.

### Lazy floating point for user programs

The kernel is built without floating point (`-march=rv64imac_zicsr_zifencei -mabi=lp64`), so the FP registers only ever hold user state. User programs are built for `rv64gc`/`lp64d` (see [build and testing](11-build-and-testing.md)). FP state is per thread and enabled on first use:

1. `do_exec` starts every program with `sstatus.FS` = Off in its trap frame and `current->fp_used = false`.
2. The program's first FP instruction traps as an illegal instruction. `user_exception` sees `FS` off and `fp_used` false, zeroes `current->fp` (so the thread never sees another process's registers), sets `fp_used`, loads the zeroed state with `fp_load_regs`, sets `FS` = Initial (`SSTATUS_FS_INITIAL`) in the trap frame, and returns. `sret` retries the instruction, which now runs.
3. From then on the hardware moves `FS` to Dirty as the program writes FP registers. The scheduler saves and restores the registers around every context switch of an `fp_used` thread ([04](04-threads-scheduling-smp.md)); `fork` copies them and signal frames carry them ([05](05-processes-and-signals.md)).

`fp_save_regs` and `fp_load_regs` (`fp.S`) set `sstatus.FS` to Initial in the *kernel's* `sstatus` before touching the registers (FP instructions trap when `FS` is Off, in S-mode too). The user's `FS` lives in `tf->sstatus` and takes effect at `sret`. `struct fpstate` is `{u64 f[32]; u64 fcsr;}` (`kernel/include/sched.h`); `fsd`/`fld` store the full 64 bits of each register.

### Kernel exceptions

1. **Lazy kernel page-table sync.** A page fault on an address ≥ `KERNEL_BASE` calls `vm_kernel_fault_fixup`, which copies a top-level entry that another CPU added to the kernel page table after this process's page table copied the kernel half. Returns true if it fixed something; the instruction is retried.
2. **User access from `copy_from_user`/`copy_to_user`.** If the faulting instruction is in the exception table (`__ex_table`, filled by `uaccess.S` and `safe.S`), the address is below `USER_TOP`, it is a load/store page fault, and `uvm_fault` succeeds, the access is retried. This is how demand paging and copy-on-write work transparently inside the kernel. Note: this runs with interrupts still off.
3. **Fixup.** Otherwise, for any memory fault whose `sepc` has an exception-table entry, set `sepc` to the fixup (logged at `trace` level: `fault at ... (addr ...) fixed up -> ...`). The fixups make `copy_*_user` return `-EFAULT` and `safe_read_*`/`safe_write_*` return -1. `extable_lookup` is a linear scan.
4. **Breakpoints.** If the `ebreak` is the semihosting sequence (`slli x0,x0,0x1f` = `0x01f01013` before, `srai x0,x0,7` = `0x40705013` after, read with `safe_read_u32`), semihosting is not enabled in QEMU: return `a0 = -1` and skip 4 bytes. Otherwise call `trap_breakpoint_hook` (used by the `breakpoint_trap_and_resume` ktest) or enter the monitor, then skip the instruction (`insn_length` handles `c.ebreak`).
5. **Everything else:** `panic_trap(tf)` (`kernel/core/panic.c`) prints `KERNEL PANIC on cpuN (hart H): unhandled <cause> in kernel mode at <sym>`, the faulting address with a hint and a page-table walk (for page and access faults), the registers (`trap_dump`), a backtrace, and the panic context.

### Kernel stack overflow

1. A function overruns its 16 KiB stack and touches the guard: store/load page fault in kernel mode. (Or any trap arrives with fewer than 560 bytes left: `FRAME_SIZE + 256`.)
2. Trap entry sees bit 14 clear in `sp - 560`, saves `sp` in `cpu->overflow_sp`, and switches to the emergency stack.
3. `trap_handler` sees `overflow_sp != 0` and calls `stack_overflow`: if `in_overflow` is already set it prints `*** kernel stack overflow while handling a stack overflow; halting` and calls `platform_exit(4)`. Otherwise it sets `in_overflow`, puts the real `sp` into `tf->regs[2]`, prints `KERNEL STACK OVERFLOW: sp=... in thread T (name), stack lo-hi` and a hint, and calls `panic_trap`. The backtrace collapses runs of identical frames ("frame repeated N more times (recursion)").

`overflow_sp` is never cleared, so after an overflow every later trap on that CPU goes to `stack_overflow` and halts (see Limitations).

The monitor command `crash stack` triggers this path on purpose.

### Return to user mode, signals and restart

`return_to_user_checks(tf)` runs after every trap from user mode (interrupt, system call or exception), with interrupts off on entry:

1. `if (this_cpu()->need_resched) yield();`
2. `if (signal_pending(current_proc())) { intr_on(); signal_deliver(tf); }` — may stop the process or make it exit.
3. `syscall_restart_check(tf);`
4. `intr_off();` — keeps interrupts off from here to `sret`. An IPI or tick that arrives now stays pending and is taken right after `sret`, since S-mode interrupts are always enabled in U-mode.

How the pieces in `kernel/proc/signal.c` cooperate with the dispatcher:

- A blocking handler interrupted by a signal returns `-ERESTARTSYS` (pipes, TTY reads, sockets, `wait4`, `nanosleep`). The dispatcher records `syscall_restart` and the original `a0`.
- `signal_deliver` loops over deliverable signals (lowest number first). Ignored signals and default actions `I`/`K` are skipped; stop signals run `do_stop` (sleeps until SIGCONT); terminate/core actions call `proc_exit`. For a signal with a handler:
  - if `syscall_restart` is set, clear it, and either rewind (`a0 = restart_a0`, `sepc -= 4`) if the handler has `SA_RESTART`, or set `a0 = -EINTR`;
  - `setup_frame` pushes a `struct sigframe` (magic `"SIGFRAME"`, all GPRs, `sepc`, the old mask, signal) on the user stack, 16-byte aligned, and sets `sp`, `ra = sa_restorer`, `a0 = sig`, `sepc = handler`. Failure (no restorer, or unwritable stack) kills the process with SIGSEGV;
  - update the blocked mask, honor `SA_NODEFER` and `SA_RESETHAND`, and return: **only one handler is set up per return to user**.
  - If no handler ran and `rt_sigsuspend` left `restore_blocked` set, the saved mask is restored.
- `syscall_restart_check`: if `syscall_restart` is still set (no handler consumed it — the signal was ignored, stopped-and-continued, or the call simply returned `-ERESTARTSYS`), restore `a0`, `sepc -= 4` (the `ecall` is always 4 bytes) so the call runs again, and for `nanosleep` set `nanosleep_restarting` so the rerun sleeps to the original deadline.
- The handler returns to `__restore_rt`, whose `ecall` enters `do_rt_sigreturn`: it reads the frame at the current `sp`, checks the magic (else `rt_sigreturn with a bad signal frame` and SIGSEGV), restores `x1..x31` and `sepc` and the mask, and returns the saved `a0` so that the dispatcher's `a0 = ret` keeps it.

Because the checks run at every return from user-mode traps, a signal sent to a process that is computing in user mode on another CPU is noticed at that CPU's next trap — at worst its next timer tick (10 ms).

### Semihosting (`kernel/arch/riscv/semihost.c`)

`semihost_call(op, params)` emits the magic sequence `slli x0,x0,0x1f; ebreak; srai x0,x0,7` (assembled with `norvc`, 4-byte aligned) with `a0 = op`, `a1 = params`. With QEMU's `-semihosting-config enable=on,target=native`, QEMU performs host I/O relative to its working directory. Without it, the `ebreak` traps and `trap_handler` returns -1. Wrappers: `semihost_open(path, mode)` (`SEMIHOST_MODE_WB` = 5), `semihost_write` (returns bytes written or -1), `semihost_close`. Used for `trace dump` and profile dumps.

## Concurrency and locking

- **Interrupts off during trap handling.** Hardware clears `SIE` on entry. The trap path turns interrupts on only in: `syscall_dispatch` (whole system call), the user page-fault path around `uvm_fault`, and `return_to_user_checks` around `signal_deliver`. Interrupt handlers themselves always run with interrupts off and never nest; `irq_depth > 0` only for their duration. Exceptions can nest inside interrupt handlers (e.g. a `safe_read` fixup).
- **Preemption rule.** An interrupt preempts only if the interrupted context had `SPIE` set. Spinlocks (`spin_lock_irqsave`) disable interrupts, so code holding one is never preempted; `__schedule` additionally panics if called with spinlocks held, or if an interrupt handler tries to sleep.
- **Per-CPU data.** `tp` is the CPU pointer in the kernel. Code with interrupts on may migrate between reading `tp` and using it, so it must use single-load accessors (`current`, `this_cpu_id()`, `in_interrupt()`). The trap code reads `this_cpu()` only with interrupts off, and `return_to_user_checks` re-reads it after every point where it could have switched. Trap return always uses the live `tp`, never the saved one.
- **`cpu->scratch`, `overflow_sp`, `irq_tf`, `irq_count`, timer fields** are touched only by their own CPU with interrupts off; no locks.
- **`exception_counts[]`** is global and updated with `__atomic_add_fetch(RELAXED)`. Readers (`trap_print_counts`) do not lock.
- **PLIC.** Only cpu0 has `SEIE`, so `plic_handle` and every device handler run on cpu0 only. `irqs[]` is written by `irq_register` at boot (no lock; the enable-bit read-modify-write is unlocked too) and read by `plic_handle` and `irq_print`. Device handlers synchronize with process-context code through their own spinlocks.
- **Timer.** `interval` and `per_tick` are written by `timer_set_profile_hz` with atomic stores and read with plain loads in `timer_interrupt` (aligned 64-/32-bit loads, so not torn). `lockup_reported` is claimed by CAS so only one CPU reports a given lockup.
- **System call state** (`syscall_restart`, `restart_a0`, `nanosleep_*`, `strace_buf`) belongs to the process's only thread and is touched only by it. `sig_pending` is changed under `proc_lock` by senders and by `signal_deliver`'s dequeue.
- **FP registers.** The live FP registers belong to the thread running on the CPU. The first-use path in `user_exception` runs with interrupts off; once `fp_used` is set, a preemption before `sret` saves and restores them like any other switch.
- **SMP and traps.** A thread preempted inside a kernel trap may resume on another CPU; its trap frame stays on its own kernel stack, and `kstack_top` is updated at every switch so the next user trap lands on the right stack.

## Interfaces

### Functions for other subsystems

| Function | Where | Purpose |
|---|---|---|
| `trap_init()` | `trap.c` | Install `stvec`. Called by `kmain` and `secondary_main`. |
| `trap_cause_name(scause)` | `trap.c` | Human-readable cause. |
| `trap_dump(tf, print)` | `trap.c` | Print `sepc`, `ra`, `scause`, `stval`, decoded `sstatus` and all registers. |
| `trap_print_counts(print)` | `trap.c` | Exception counts and per-CPU interrupt counts (monitor `traps`). |
| `extable_lookup(pc)` | `trap.c` (declared in `safe.h`) | Fixup address for a faulting instruction, or 0. |
| `trap_breakpoint_hook` | `trap.h` | Optional hook for kernel `ebreak`. |
| `debug_break()` | `trap.h` | Inline `ebreak`: enter the monitor from code. |
| `user_return(tf)` | `trapentry.S` | Enter user mode with a prepared frame (interrupts off). |
| `thread_user_tf(t)` | `sched.c` | A thread's user trap frame. |
| `plic_init()`, `irq_register(irq, name, fn, arg)`, `irq_print(print)` | `plic.c` | Device interrupts. `irq_register` returns 0 or -1. |
| `timer_init()`, `timer_init_cpu()`, `timer_rearm()`, `timer_set_profile_hz(hz)`, `watchdog_check()` | `timer.c` | Tick. |
| `jiffies`, `timer_running` | `timer.c` | Tick count (cpu0), tick started. |
| `cpu_wait_ticks(ticks)` | `idle.c` | Sleep with `wfi` for polling loops. |
| `semihost_open/write/close` | `semihost.c` | Host file I/O. |
| `syscall_dispatch(tf)`, `syscall_name(nr)`, `errno_name(err)` | `syscall.c` | Dispatch; names for trace tools, procfs and logs. |
| `ms_to_ticks(ms, timebase)` | `irq.h` | Milliseconds to timebase ticks. |

### System calls

ABI: `ecall` with `a7` = number, `a0`–`a5` = arguments; result in `a0`, `-errno` on failure. All handlers have the signature `long do_<name>(const u64 *a)`.

| Nr | Name | Handler | Notes |
|---:|---|---|---|
| 17 | `getcwd` | `kernel/fs/file.c: do_getcwd` | Returns the length including NUL. |
| 23 | `dup` | `kernel/fs/file.c: do_dup` | Lowest free descriptor. |
| 24 | `dup3` | `kernel/fs/file.c: do_dup3` | Only `O_CLOEXEC` honored; `EINVAL` if old == new. |
| 25 | `fcntl` | `kernel/fs/file.c: do_fcntl` | `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD/SETFD`, `F_GETFL/SETFL` (`O_APPEND`, `O_NONBLOCK`). |
| 29 | `ioctl` | `kernel/fs/file.c: do_ioctl` | Forwarded to the file's `cops->ioctl` (TTY); else `ENOTTY`. |
| 34 | `mkdirat` | `kernel/fs/file.c: do_mkdirat` | Directory fd must be `AT_FDCWD` unless the path is absolute (all `*at` path calls). |
| 35 | `unlinkat` | `kernel/fs/file.c: do_unlinkat` | `AT_REMOVEDIR` for directories. |
| 37 | `linkat` | `kernel/fs/file.c: do_linkat` | Hard link. |
| 38 | `renameat` | `kernel/fs/file.c: do_renameat` | |
| 48 | `faccessat` | `kernel/proc/cred.c: do_faccessat` | `AT_FDCWD` only; checks with effective credentials via `vfs_permission`. |
| 49 | `chdir` | `kernel/fs/file.c: do_chdir` | |
| 52 | `fchmod` | `kernel/proc/cred.c: do_fchmod` | |
| 53 | `fchmodat` | `kernel/proc/cred.c: do_fchmodat` | `AT_FDCWD` only. |
| 54 | `fchownat` | `kernel/proc/cred.c: do_fchownat` | `AT_FDCWD` only. |
| 56 | `openat` | `kernel/fs/file.c: do_openat` | `O_CLOEXEC` sets `FD_CLOEXEC`. |
| 57 | `close` | `kernel/fs/file.c: do_close` | |
| 59 | `pipe2` | `kernel/fs/file.c: do_pipe2` | `O_CLOEXEC` only. |
| 61 | `getdents64` | `kernel/fs/file.c: do_getdents64` | Fixed-size `struct dirent` records. |
| 62 | `lseek` | `kernel/fs/file.c: do_lseek` | `ESPIPE` on pipes and devices; procfs snapshots are seekable. |
| 63 | `read` | `kernel/fs/file.c: do_read` | May return `-ERESTARTSYS` internally (pipes, TTY, sockets). |
| 64 | `write` | `kernel/fs/file.c: do_write` | |
| 73 | `ppoll` | `kernel/fs/poll.c: do_ppoll` | Up to 64 fds (`POLL_MAX_FDS`); the signal-mask argument is not used; interrupted → `EINTR`. |
| 79 | `fstatat` | `kernel/fs/file.c: do_fstatat` | Flags ignored. |
| 80 | `fstat` | `kernel/fs/file.c: do_fstat` | Pipes report `S_IFIFO`. |
| 81 | `sync` | `kernel/fs/file.c: do_sync` | `vfs_sync_all`: waits for every filesystem's next log commit (efs `log_sync`). |
| 82 | `fsync` | `kernel/fs/file.c: do_fsync` | `vfs_sync` of the file's filesystem (`EBADF` for a bad fd). |
| 88 | `utimensat` | `kernel/proc/cred.c: do_utimensat` | Sets the modification time; a NULL path means the fd (`futimens`); `AT_FDCWD` only otherwise; `UTIME_NOW`/`UTIME_OMIT`; flags ignored. |
| 93 | `exit` | `kernel/proc/syscall.c: do_exit` | `proc_exit(WEXITSTATUS_ENC(code))`; never returns. |
| 101 | `nanosleep` | `kernel/proc/cred.c: do_nanosleep` | Fills the remainder; `-ERESTARTSYS` on a signal, restarted to the original deadline. |
| 113 | `clock_gettime` | `kernel/proc/cred.c: do_clock_gettime` | `CLOCK_REALTIME` (RTC) and `CLOCK_MONOTONIC` (`rdtime`) only. |
| 117 | `ptrace` | `kernel/proc/ptrace.c: do_ptrace` | Linux request numbers plus Eunuchs `PTRACE_READ`/`PTRACE_WRITE`; `PEEK*` store the word at `*data`; `PTRACE_SINGLESTEP` → `EIO`. See [14](14-debugger.md). |
| 124 | `sched_yield` | `kernel/proc/syscall.c: do_sched_yield` | |
| 129 | `kill` | `kernel/proc/signal.c: do_kill` | pid > 0, 0 (own group), < -1 (group); `kill(-1)` → `EINVAL`. |
| 133 | `rt_sigsuspend` | `kernel/proc/signal.c: do_rt_sigsuspend` | 32-bit mask; always returns `EINTR`. |
| 134 | `rt_sigaction` | `kernel/proc/signal.c: do_rt_sigaction` | SIGKILL/SIGSTOP → `EINVAL`. |
| 135 | `rt_sigprocmask` | `kernel/proc/signal.c: do_rt_sigprocmask` | 32-bit mask. |
| 139 | `rt_sigreturn` | `kernel/proc/signal.c: do_rt_sigreturn` | Restores the frame; returns the saved `a0`. |
| 142 | `reboot` | `kernel/proc/syscall.c: do_reboot` | Root only (`EPERM`). Calls `vfs_sync_all` first, so every completed write is on disk. `RB_AUTOBOOT` (0x01234567): cold reboot through SBI SRST (OpenSBI, eboot, the kernel again); `RB_POWER_OFF` (0x4321fedc): shut down; anything else `EINVAL`. Never returns on success. |
| 144 | `setgid` | `kernel/proc/cred.c: do_setgid` | |
| 146 | `setuid` | `kernel/proc/cred.c: do_setuid` | Root sets all three ids; others may set euid to real or saved. |
| 154 | `setpgid` | `kernel/proc/signal.c: do_setpgid` | |
| 155 | `getpgid` | `kernel/proc/signal.c: do_getpgid` | |
| 156 | `getsid` | `kernel/proc/signal.c: do_getsid` | |
| 157 | `setsid` | `kernel/proc/signal.c: do_setsid` | |
| 166 | `umask` | `kernel/proc/cred.c: do_umask` | Returns the old mask. |
| 169 | `gettimeofday` | `kernel/proc/cred.c: do_gettimeofday` | From the RTC; timezone ignored. |
| 172 | `getpid` | `kernel/proc/syscall.c: do_getpid` | |
| 173 | `getppid` | `kernel/proc/syscall.c: do_getppid` | Reads the parent under `proc_lock`. |
| 174–177 | `getuid`, `geteuid`, `getgid`, `getegid` | `kernel/proc/cred.c` | |
| 198 | `socket` | `kernel/net/socket.c: do_socket` | `AF_INET` only; stream (TCP), datagram (UDP), raw (ICMP, euid 0 only). |
| 200 | `bind` | `kernel/net/socket.c: do_bind` | |
| 201 | `listen` | `kernel/net/socket.c: do_listen` | |
| 202 | `accept` | `kernel/net/socket.c: do_accept` | `accept4` with flags 0. |
| 203 | `connect` | `kernel/net/socket.c: do_connect` | |
| 204 | `getsockname` | `kernel/net/socket.c: do_getsockname` | |
| 205 | `getpeername` | `kernel/net/socket.c: do_getpeername` | |
| 206 | `sendto` | `kernel/net/socket.c: do_sendto` | |
| 207 | `recvfrom` | `kernel/net/socket.c: do_recvfrom` | |
| 208 | `setsockopt` | `kernel/net/socket.c: do_setsockopt` | Accepts and ignores `SO_REUSEADDR/RCVBUF/SNDBUF`, `TCP_NODELAY`; else `ENOPROTOOPT`. |
| 209 | `getsockopt` | `kernel/net/socket.c: do_getsockopt` | `SOL_SOCKET` only (e.g. `SO_ERROR`). |
| 210 | `shutdown` | `kernel/net/socket.c: do_shutdown` | |
| 214 | `brk` | `kernel/proc/syscall.c: do_brk` | Returns the new break, or the old one on failure (Linux style). Grows/shrinks the heap VMA, or starts a new one if `mprotect` changed the top of the heap. |
| 215 | `munmap` | `kernel/proc/syscall.c: do_munmap` | |
| 220 | `fork` | `kernel/proc/syscall.c: do_fork` → `proc.c: sys_fork` | No arguments (Linux 220 is `clone`). Child gets `a0 = 0`. |
| 221 | `execve` | `kernel/proc/syscall.c: do_execve` → `proc.c: sys_execve` | On success returns `argc` into the new image's `a0`. |
| 222 | `mmap` | `kernel/proc/syscall.c: do_mmap` | Anonymous private only; `PROT_WRITE\|PROT_EXEC` → `EACCES`; address hint ignored; a guard page between mappings. |
| 226 | `mprotect` | `kernel/proc/syscall.c: do_mprotect` → `uvm.c: vma_protect` | Page-aligned start; the whole range must be mapped (`ENOMEM`); `PROT_WRITE\|PROT_EXEC` → `EACCES`; splits VMAs; `fence.i` on every hart when execute is added. |
| 242 | `accept4` | `kernel/net/socket.c: do_accept4` | |
| 260 | `wait4` | `kernel/proc/syscall.c: do_wait4` → `proc.c: sys_wait4` | `rusage` ignored; `-ERESTARTSYS` when interrupted. |
| 500 | `sleep_ms` | `kernel/proc/syscall.c: do_sleep_ms` | Eunuchs-specific. `EINTR` if a signal is pending afterwards (not restarted). |
| 501 | `uptime_ms` | `kernel/proc/syscall.c: do_uptime_ms` | `rdtime * 1000 / timebase`. |
| 502 | `debug` | `kernel/proc/syscall.c: do_debug` | Debug operations, below. |

`SYS_debug` operations (`a0`), wrapped by `dbg_*` in `user/include/eunuchs.h`:

| Op | Value | Effect |
|---|---:|---|
| `DBG_MARK` | 0 | Up to 24 bytes of user string → `umark` trace event and an info log line. |
| `DBG_BREAK` | 1 | Enter the kernel monitor with the caller's user frame. |
| `DBG_STRACE` | 2 | `a1`: 0 off, 1 on, 2 on and follow forks. |
| `DBG_PS` | 3 | Print the process table on the console. |
| `DBG_FILES` | 4 | Print the caller's open files. |
| `DBG_VM` | 5 | Print the caller's memory map. |
| `DBG_FSCRASH` | 6 | Arm a simulated crash at `after-log`, `after-commit` or `mid-install` of the next commit. |
| `DBG_PROFILE` | 7 | `a1`: 0 stop, 1 start (`a2` = Hz), 2 clear, 3 dump to the host file named by `a2` (semihosting). |

### Boot options

| Option | Effect |
|---|---|
| `watchdog=<seconds>` | Hard-lockup watchdog threshold (default 10, `0` = off). |
| `log=trap:trace` | Log every exception-table fixup. `log=syscall:<level>` controls strace output (logged at `info`). |
| `trace=<groups>` | `irq` (irq_entry/exit, IPIs), `syscall` (syscall_enter/exit), `proc` (`user_fault`, signals). |

### /proc and monitor

- `/proc/interrupts` → `irq_print`.
- Monitor: `traps`, `irq`, `regs`, `bt`, `cpus`, `cpu <n>`, `strace <pid> [on|off|follow]`, `trace ...`, `crash stack|illegal|ebreak|...`.

## Debugging and observability

**Counters.**
- `traps` (monitor): exception counts by cause (all CPUs) and interrupt counts per CPU.
- `irq` (monitor) and `/proc/interrupts`: timer, IPI and external counts per CPU, then each PLIC source with its count, total handler time in microseconds and handler symbol, plus spurious claims.

**strace.** `strace <cmd>` in the shell (`user/bin/eutils.c` calls `dbg_strace(1)` and execs), `strace <pid> on|off|follow` in the monitor, or `dbg_strace()` from code. Lines go to the kernel log at `info` under subsystem `syscall`:

```
[3 sh] write(1, "hello\n", 6) = 6
[3 sh] execve("/bin/nope", ["nope"], 0x0) = -1 ENOENT (No such file or directory)
```

Strings are cut at 40 bytes, buffers at 32, argv at 6 elements of 24 bytes; `brk`/`mmap` results are shown in hex, `execve` success as 0.

**Trace events** (`kernel/include/trace_events.h`): `irq_entry(cause)`, `irq_exit(cause, irq)`, `syscall_enter(nr, arg0, arg1)`, `syscall_exit(nr, ret)`, `user_fault(addr, access, result)`, `signal_deliver(sig, handler)`, `ipi_send`, `ipi_recv`. In Perfetto, IRQs and system calls show as slices.

**Registers and backtraces.** In the monitor, `regs` shows the trap frame via `trap_dump`; `bt` walks through traps thanks to the fake frame records; `cpu <n>` switches to another CPU's stop frame. In GDB (`make gdb`), `bt` unwinds through traps using the CFI (the trap shows as `<signal handler called>`); `eu-tf [expr]` pretty-prints a `struct trap_frame`; breakpoints are preset on `panic` and `panic_trap`. `make qemu-log` records every trap as QEMU sees it in `build/qemu.log`.

**Typical messages.**

| Message | Meaning |
|---|---|
| `KERNEL PANIC on cpuN (hart H): unhandled <cause> in kernel mode at <sym>` | A kernel exception with no fixup. Read the faulting address, hint, page-table walk and backtrace. |
| `KERNEL STACK OVERFLOW: sp=... in thread T (name), stack lo-hi` | A kernel stack ran into its guard (or a trap arrived with < 560 bytes left). Look for recursion ("frame repeated") or big local arrays. |
| `*** kernel stack overflow while handling a stack overflow; halting` | A second trap on a CPU after an overflow (including, today, any faulting `safe_read` during the report). QEMU exits with status 4. |
| `watchdog: hard lockup on cpuN: no timer interrupt for M ms (interrupts off); it is running thread T (name); holding '...'` | That CPU had interrupts off for the watchdog period. |
| `unexpected <cause> (scause=...); masked` | An interrupt code other than timer, external or software arrived; it is now disabled on that CPU. |
| `[pid name] unknown system call N` | `a7` outside the table. The call returns `ENOSYS`. |
| `pid N (name) killed by SIGSEGV: <cause>: no mapping at 0x... -- NULL pointer dereference` | From `user_fault`; followed by the user backtrace and memory map. |
| `fault at ... (addr ...) fixed up -> ...` | (`log=trap:trace`) an exception-table fixup happened, e.g. `EFAULT` from a bad user pointer. |
| `irq_register(N, name) failed` | No PLIC, IRQ 0 or ≥ 64, or already registered. |

**Tests.** `kernel/tests/test_debug.c`: `safe_access_recovers_from_faults` (fixups, counts two load page faults), `breakpoint_trap_and_resume` (`trap_breakpoint_hook`), `backtrace_walks_frames`. `user/bin/utests.c` checks `EINTR` from interrupted `sleep_ms` and pipe reads.

## Limitations and known issues

- **FP state is saved eagerly once used.** `FS` is never checked for Dirty: every switch away from an `fp_used` thread saves all 32 registers and `fcsr`, and every switch back loads them, even if the thread has not touched FP since. A thread that used FP once pays this until it execs.
- **A genuinely illegal instruction costs two traps** in a program that has not used FP: the first is taken as "first FP use" (the opcode is not inspected) and enables FP; the retry then faults with `FS` on and gets SIGILL.
- **`SYS_debug` has no permission checks.** Any process can stop the machine (`DBG_BREAK`), arm a simulated filesystem crash (`DBG_FSCRASH`), and start the profiler and write its output to a host path of its choice through semihosting (`DBG_PROFILE` op 3).
- **After a kernel stack overflow, the CPU cannot take another trap.** `overflow_sp` is never cleared and `stack_overflow` halts on the second call, so a faulting `safe_read` during the overflow report or later in the monitor (e.g. `mem` at a bad address) exits QEMU with the misleading "overflow while handling an overflow" message.
- **Overflow margin is a heuristic.** Entry needs 560 bytes (`FRAME_SIZE + 256`) below `sp`. A trap that arrives with less is reported as an overflow even if the thread would not have overflowed; a handler that needs more than 256 bytes past the frame on a nearly full stack overflows inside the handler (still caught, by the guard page).
- **All device interrupts on cpu0.** Device handlers and the claim loop run with interrupts off on cpu0, so a busy device adds latency to cpu0's tick and to everything scheduled there. The PLIC context is assumed to be `2 * hart + 1` (true on QEMU `virt`), not read from the device tree. There is no `irq_unregister`, no per-IRQ priority, and no masking of an individual source.
- **Periodic tick on every CPU, even idle ones**, through an SBI call per interrupt (no Sstc `stimecmp`, no tickless idle).
- **Profiling rate is rounded down to a multiple of 100 Hz** (`timer_set_profile_hz`).
- **The watchdog needs two CPUs.** With `smp=1` (or one hart) there is no hard-lockup detection. A CPU cannot report its own lockup (no NMI).
- **`missed_ticks` is counted but never shown.** Ticks swallowed by `cpu_wait_ticks` are not counted at all.
- **`jiffies` advances only on cpu0** and stalls when cpu0 has interrupts off.
- **One signal handler per return to user.** Other pending signals wait for the next trap (e.g. the handler's first system call or the next tick). Signals sent to a process running in user mode on another CPU are noticed at its next trap (≤ 10 ms); no IPI is sent.
- **`nanosleep` is restarted after an `SA_RESTART` handler** with its full original duration; Linux returns `EINTR` for `nanosleep` whenever a handler runs. (A restart without a handler correctly sleeps only to the old deadline.)
- **`rt_sigreturn` goes through the restart logic.** If the restored `a0` happens to equal `-ERESTARTSYS` (-512), `syscall_dispatch` sets `syscall_restart`, and `syscall_restart_check` then rewinds `sepc` by 4 and clobbers `a0` in the restored context. Possible, for example, after an `SA_RESTART` handler interrupts `wait4(-512, ...)`, which is restarted with `a0 = -512`.
- **strace buffer overrun.** In `strace_enter`, `len` is advanced by the return value of `ksnprintf`, which is the untruncated length. When an argument separator (`", "`) pushes `len` past `sizeof(strace_buf)` (192) in the middle of an argument, the next call is given `size - len`, which wraps to a huge `size_t`, and writes past the end of `p->strace_buf` (the last field of `struct proc`). Long `execve` argument lists under strace can trigger it.
- **`__restore_rt` has no fallback** after its `ecall`. If `rt_sigreturn` fails and the process survives (it has a SIGSEGV handler), execution falls through into whatever follows `__restore_rt`.
- **Kernel uaccess faults resolve with interrupts off** (`uvm_fault`, possibly a page allocation and a 4 KiB copy for COW), unlike user faults which turn interrupts on.
- **`extable_lookup` is a linear scan**, done twice per kernel memory fault.
- **The `ppoll` signal mask, `wait4` `rusage`, `fstatat` flags, and `*at` directory fds** (other than `AT_FDCWD`) are ignored or rejected.
- **Assembly constants are hand-maintained.** `trapentry.S` defines its own `FRAME_SIZE` and `TF_*` offsets; only the total size and the `struct cpu` offsets are checked by static asserts.
- **CFI for user traps** describes the user frame as the caller of `kernel_trap_entry`; GDB will try to unwind into user code using kernel symbols.

## Potential improvements

1. **Clamp `len` in `strace_enter`** — After every `ksnprintf`/`put_escaped`, use `len = MIN(len, size)` (or a small append helper), so a long argument list cannot write past `p->strace_buf`. Memory-corruption bug triggered just by tracing. Impact: high. Effort: small.
2. **Let traps work after a stack overflow** — In `trap_handler`, treat a trap as a new overflow only if the emergency-stack switch happened for *this* trap (e.g. compare `regs[2]`/`sp` with the emergency stack, or clear `overflow_sp` in `stack_overflow` once it is copied into the frame), so `safe_read` fixups, `bt` and `mem` work in the overflow report and the monitor. Impact: high. Effort: small.
3. **Keep `rt_sigreturn` out of the restart logic** — Skip the `-ERESTARTSYS` check in `syscall_dispatch` for `SYS_rt_sigreturn` (or have handlers signal a restart through a flag instead of the return value). Removes a rare context corruption. Impact: medium. Effort: small.
4. **Permission checks for `SYS_debug`** — Require euid 0 (or a boot option) for `DBG_BREAK`, `DBG_FSCRASH` and `DBG_PROFILE`, and restrict profile dump paths to `build/`. Today any user process can halt the machine or write host files. Impact: medium. Effort: small.
5. **Asm offsets generated from C** — Generate `FRAME_SIZE`, `TF_SEPC`... and `CPU_*` from `offsetof` at build time (an `asm-offsets.c`), or at least add `_Static_assert`s for each `TF_*` offset. Prevents silent frame-layout drift. Impact: low. Effort: small.
6. **Sstc timer and tickless idle** — Use `stimecmp` when the hart has Sstc (no SBI `ecall` per tick), and stop the periodic tick on idle CPUs, programming the next sleeper's deadline instead. Less overhead and fewer wakeups, especially at profiling rates up to 5000 Hz. Impact: medium. Effort: medium.
7. **Spread device interrupts and read PLIC contexts from the device tree** — Parse `interrupts-extended` to find each hart's S-mode context, add per-IRQ CPU affinity and `irq_unregister`/mask, and enable `SEIE` on more than cpu0. Removes the cpu0 bottleneck and the `2 * hart + 1` assumption. Impact: medium. Effort: medium.
8. **Faster signal delivery to running processes** — When `signal_send` targets a process that is running on another CPU, send `IPI_RESCHED` so the signal is seen at once rather than at the next tick; and loop `return_to_user_checks` until neither `need_resched` nor a deliverable signal remains. Impact: low. Effort: small.
9. **Linux-compatible `nanosleep` interruption** — Return a distinct internal code (like `ERESTART_RESTARTBLOCK`) so a handler always yields `EINTR` with the remaining time, and only handler-less interruptions restart. Impact: low. Effort: small.
10. **`__restore_rt` safety net** — Follow its `ecall` with an `unimp` (or a call to `_exit`), so a failed `rt_sigreturn` cannot fall through into unrelated code. Impact: low. Effort: small.
11. **Show `missed_ticks` and per-CPU exceptions** — Add missed ticks to `/proc/interrupts`/`cpus`, count ticks lost in `cpu_wait_ticks`, and add per-syscall counts (and optionally latency) to a `/proc/syscalls` file. Cheap observability for timer and syscall behavior. Impact: low. Effort: small.
12. **Save FP state only when dirty** — Check `sstatus.FS` (or `tf->sstatus`) for Dirty before `fp_save_regs` and set it back to Clean afterwards, and turn `FS` off again for a thread that has not used FP for a while, so FP-using threads that are mostly integer stop paying 33 loads and stores per switch. Impact: low. Effort: small.
13. **Resolve kernel uaccess faults with interrupts on when allowed** — In `trap_handler`, enable interrupts around `uvm_fault` if the frame's `SPIE` was set, matching the user-fault path and cutting interrupt latency during COW copies. Impact: low. Effort: small.
14. **Sorted exception table** — Sort `__ex_table` at build or boot and binary-search it in `extable_lookup` (called twice per kernel memory fault). Small today, but grows with every new uaccess/safe accessor. Impact: low. Effort: small.
