# The debugger: ptrace and db

Programs on Eunuchs can be debugged on Eunuchs. The kernel implements `ptrace` (`kernel/proc/ptrace.c`), the Unix interface through which one process (the tracer) stops, inspects and changes another (the tracee). On top of it, `db` (`user/bin/db/`) is a gdb-like debugger: breakpoints, stepping by line or instruction, backtraces, local variables and C expressions at source level, read from the DWARF that `tcc -g` (or GCC) produces, plus a RISC-V disassembler for code without debug information.

```
root@eunuchs:/tmp# tcc -g -o d d.c
root@eunuchs:/tmp# db d
Reading symbols from ./d...
(db) break add
Breakpoint 1 at 0x10074: file /tmp/d.c, line 3.
(db) run
Breakpoint 1, add (a=2, b=3) at /tmp/d.c:3
3           return a + b;
(db) finish
Run till exit from #0  add (a=2, b=3) at /tmp/d.c:3
Value returned is $1 = 5
```

The first half of this document covers the kernel side, the second `db`.

## Responsibilities and scope

**In scope:**

- `ptrace` (system call 117): tracing stops, register, FP-register and memory access, resuming with or without a signal, attaching and detaching, and the changes this makes to signals, `exec`, faults, `wait4` and `exit`.
- The kernel's breakpoint behaviour: a user `ebreak` is a SIGTRAP.
- `db`: its command language, breakpoints and stepping, unwinding, the DWARF reader, expressions and printing, the disassembler.
- The libc wrapper `ptrace()` and `<sys/ptrace.h>`.

**Out of scope:**

- Debugging the kernel: the monitor, GDB and the trace tools ([09](09-debugging-internals.md), [DEBUGGING.md](../DEBUGGING.md)). `dbg_break()` (`SYS_debug`/`DBG_BREAK`) is still how a user program enters the kernel monitor.
- Producing debug information: `tcc -g` and its patches are in [13](13-toolchain.md).
- Hardware debug features: QEMU's RISC-V harts have no single-step trap usable from S-mode, and the kernel uses no debug triggers, so there are no hardware breakpoints or watchpoints.

## Source map

| File | Role |
|---|---|
| `kernel/proc/ptrace.c` | `do_ptrace` (the requests), `ptrace_stop` (the tracing stop), `ptrace_tracer_exit`, `ptrace_io` (bulk memory copies), attach permission (`may_attach`). |
| `lib/include/uapi/ptrace.h` | Request numbers, `struct user_regs`, `struct user_fpregs`, `struct ptrace_siginfo`, `struct ptrace_io`, `si_code` values. Shared by the kernel, libc and `db`. |
| `kernel/include/proc.h` | The tracing fields of `struct proc`; `ptrace_stop`, `ptrace_tracer_exit`, `uvm_peek`, `uvm_poke`. |
| `kernel/proc/signal.c` | `signal_deliver` calls `ptrace_stop`; SIGCONT cannot end a tracing stop, SIGKILL does. |
| `kernel/proc/proc.c` | SIGTRAP after a traced `exec`; `wait4` (`reportable`, `tracee_reportable`, `find_reportable`); exit notifications to a tracer; `user_fault` records siginfo and leaves traced processes alive. |
| `kernel/mm/uvm.c` | `uvm_peek`, `uvm_poke`, `debug_page`. |
| `kernel/arch/riscv/trap.c` | `EXC_BREAKPOINT` from user mode → `user_fault(tf, "ebreak", SIGTRAP)`. |
| `kernel/fs/procfs.c` | State `t` for a process in a tracing stop. |
| `user/lib/unistd2.c`, `user/include/sys/ptrace.h` | The libc `ptrace()`. |
| `user/bin/db/db.h` | Shared types and the file list. |
| `user/bin/db/main.c` (1,450 lines) | Command line, the command loop and every command. |
| `user/bin/db/target.c` (820) | The debugged process: run, attach, registers, memory, breakpoints, software single-step, unwinding, step/next/finish/until. |
| `user/bin/db/dwarf.c` (1,690) | DWARF 2–5: units, abbreviations, forms, line tables, functions, scopes, types, location lists and expressions. |
| `user/bin/db/expr.c` (1,120) | C expressions: parser, evaluator against the stopped process, gdb-style printing. |
| `user/bin/db/disas.c` (420) | RV64GC disassembler. |
| `user/bin/db/elf.c` (180) | ELF sections and the symbol table. |
| `user/bin/utests.c: t_ptrace` | The kernel interface's test. |

## Concepts and data structures

### The interface (`lib/include/uapi/ptrace.h`)

`ptrace(request, pid, addr, data)`, system call 117. Request numbers are Linux's where Linux has them:

| Request | Number | Effect |
|---|---|---|
| `PTRACE_TRACEME` | 0 | The caller's parent becomes its tracer (the debugger forks, the child calls this, then `exec`s). `EPERM` if already traced or no parent. |
| `PTRACE_PEEKTEXT`, `PEEKDATA` | 1, 2 | Read the 8-byte word at `addr`. The kernel stores it at `*data`; the libc wrapper returns it, as glibc does (clear `errno` first to tell -1 from an error). |
| `PTRACE_POKETEXT`, `POKEDATA` | 4, 5 | Write the word `data` at `addr`, whatever the page protection. |
| `PTRACE_CONT` | 7 | Resume; `data` is the signal to deliver (0: none). |
| `PTRACE_KILL` | 8 | Send SIGKILL (the tracee need not be stopped). |
| `PTRACE_SINGLESTEP` | 9 | Always `EIO`: RISC-V has no trace trap, so debuggers step in software. |
| `PTRACE_GETREGS`, `SETREGS` | 12, 13 | `struct user_regs { u64 r[32]; }`: `r[0]` is the pc, `r[1..31]` are x1..x31 (Linux's `user_regs_struct` order). |
| `PTRACE_GETFPREGS`, `SETFPREGS` | 14, 15 | `struct user_fpregs { u64 f[32]; u64 fcsr; }`. Reading from a process that never used FP gives zeros; writing gives it FP state (`fp_used`, `FS` = Initial). |
| `PTRACE_ATTACH` | 16 | Trace a running process; it is sent SIGSTOP. |
| `PTRACE_DETACH` | 17 | Stop tracing and resume with signal `data`. |
| `PTRACE_GETSIGINFO` | 0x4202 | `struct ptrace_siginfo { i32 si_signo, si_code; u64 si_addr; }`: why it stopped. |
| `PTRACE_READ`, `PTRACE_WRITE` | 0x4e00, 0x4e01 | Eunuchs: bulk copy, `data` points to `struct ptrace_io { addr; buf; len; }` (at most 1 MiB per call). A debugger reads a whole function or string in one call instead of one word at a time. |

`si_code` values: `SI_USER` (0, sent with `kill`), `TRAP_BRKPT` (1, an `ebreak`; `si_addr` is its pc), `TRAP_EXEC` (5, Eunuchs: after `exec`; `si_addr` is the entry point), `SEGV_MAPERR` (1) and `SEGV_ACCERR` (2) with the faulting address, `ILL_ILLOPC` (1), `BUS_ADRALN` (1). For a stop whose signal is not the last fault the kernel recorded, `si_code` is `SI_USER` and `si_addr` 0.

Every request except `TRACEME`, `ATTACH` and `KILL` needs a tracee that the caller traces and that is in a tracing stop; otherwise it fails with `ESRCH`, as on Linux. An unknown request is `EIO`.

### Process state (`struct proc`)

| Field | Meaning |
|---|---|
| `tracer` | The tracing process, or NULL. Not inherited by `fork`. |
| `ptrace_stopped` | In a tracing stop: only the tracer (or SIGKILL) ends it. Also sets the ordinary `stopped`/`stop_sig`. |
| `ptrace_resume_sig` | The signal the tracer chose when resuming. |
| `ptrace_exit_report` | Exited, and a tracer that is not the parent has not been told yet. |
| `si_signo`, `si_code`, `si_addr` | The last fault or trap and its details, for `PTRACE_GETSIGINFO`. |

All change under `proc_lock`. `ps` and `/proc/<pid>/status` show the state letter `t` for a tracing stop.

## How it works: the kernel

### The tracing stop

A traced process stops **whenever a signal is about to be delivered to it** (except SIGKILL). `signal_deliver` dequeues the signal and, before looking at its action, calls `ptrace_stop(p, sig)`:

1. Under `proc_lock`: if there is no tracer any more, or SIGKILL is pending, return `sig` unchanged. Otherwise set `ptrace_stopped`, `stopped`, `stop_sig = sig`, clear `stop_reported`, send SIGCHLD to the tracer and wake its `child_wq`.
2. Sleep on `stop_wq` (`wait_what = "traced"`) until the tracer clears `ptrace_stopped` or SIGKILL arrives.
3. Return `ptrace_resume_sig`: the signal the tracer chose.

`signal_deliver` then drops the signal if the result is 0, or delivers the result (normally the same signal, but the tracer may substitute another) through the usual action; a substituted signal that is blocked goes back to pending. Ignored signals stop the tracee too. SIGCONT does not end a tracing stop; SIGKILL does, and the process dies.

Where the stops come from:

- **Signals** sent by anyone, including `kill` from the shell (`si_code = SI_USER`).
- **`ebreak`**: `user_exception` calls `user_fault(tf, "ebreak", SIGTRAP)` with `sepc` still on the `ebreak`, so a debugger sees the pc of the breakpoint. (`dbg_break()`, not `ebreak`, enters the kernel monitor; untraced programs with no SIGTRAP handler are killed with a fault report.)
- **Faults**: `user_fault` records `si_signo`, `si_code` and `si_addr` and, for a traced process, posts the signal instead of killing, so the debugger sees `Program received signal SIGSEGV` with the address. If the tracer resumes with signal 0, the faulting instruction runs again.
- **`exec`**: a successful `exec` by a traced process sends SIGTRAP with `TRAP_EXEC` and the entry point, so the debugger gets control before the first instruction.
- **`PTRACE_ATTACH`** sends SIGSTOP.

### Waiting for a tracee (`sys_wait4`)

`find_reportable` looks at the caller's children first (`reportable`): a zombie, a job-control stop if `WUNTRACED` was given, or a tracing stop if the caller is the tracer, **without** `WUNTRACED`. A parent that is not the tracer never sees its child's tracing stops. Then it scans every process the caller traces without being its parent (`tracee_reportable`): their tracing stops, and their exit, which is reported to the tracer once (the tracer gets the pid and exit status, and `tracer` is cleared) while the real parent still reaps the zombie. As in Linux, the tracer hears of the exit first: until it has, the real parent's `wait4` passes over the zombie, and it is woken when the tracer has seen it (or exits). `has_zombie` uses the same scan, so a tracer sleeping in `wait4` is woken for tracing stops.

### Registers and memory

The tracee sits in `ptrace_stop` with its user registers in its trap frame (`thread_user_tf`) and its FP registers in `thread->fp`: `ptrace_stop` saves them there before it announces the stop and loads them back as it returns, so the tracer never sees stale values and `SETFPREGS` takes effect even if the tracee never sleeps. `GETREGS`/`SETREGS` copy the trap frame (`r[0]` ↔ `sepc`); `GETFPREGS`/`SETFPREGS` copy `thread->fp`. They happen under `proc_lock`, which keeps the tracee and its `mm` alive; the copies to and from the tracer's memory happen with the lock dropped, since they can fault.

Memory goes through `uvm_peek_locked` and `uvm_poke_locked` ([02](02-memory.md#user-address-spaces-uvmc)) with the tracee's `mm->lock` held. `lock_tracee_mm` finds the stopped tracee under `proc_lock` and takes its `mm->lock` with `mutex_trylock` (a mutex cannot be waited for under a spinlock); if reclaim has it, it drops `proc_lock`, sleeps 1 ms and tries again. The lock also keeps the mm alive, since `exit` takes it before freeing.

- The page is faulted in if a VMA covers it (a swapped-out page is read back), so untouched pages can be read.
- `uvm_poke` ignores the VMA's protection (breakpoints go into r-x text), but first breaks sharing: a page whose frame has other users (text after `fork` is shared without COW marks) is copied, so a breakpoint never appears in another process.
- Writing an executable page runs `fence.i` on this hart and on every hart through SBI, so the instruction fetch sees the new code wherever the tracee runs next.
- `PTRACE_READ`/`WRITE` work a page at a time through a kernel bounce page, locking the tracee's mm for each page. A read error is reported as `EIO`.

### Attaching, detaching, exits

- **`PTRACE_ATTACH`**: refused (`EPERM`) for oneself, init, a process already traced, a process without an `mm`, or, for non-root callers, a process whose real, effective and saved uids and real and effective gids are not all the caller's effective ids (`may_attach`). Otherwise it sets `tracer` and sends SIGSTOP; the tracer then waits for the stop.
- **`PTRACE_DETACH`**: clears `tracer` and resumes with the given signal.
- **Tracer exit** (`ptrace_tracer_exit`, called from `proc_exit` under `proc_lock`): every tracee loses its tracer, and one in a tracing stop is resumed with no signal.
- **Tracee exit**: the parent is notified as usual; a tracer that is not the parent also gets SIGCHLD, and `wait4` reports the exit to it once.

### Kernel limitations

- **No single-step** (`PTRACE_SINGLESTEP` is `EIO`), no hardware breakpoints or watchpoints, no `PTRACE_O_*` options: no fork, vfork, clone or exec events, and a traced process's children are not traced.
- **No `PTRACE_SYSCALL`** (stops at system call entry and exit) and no `PTRACE_PEEKUSER`/`POKEUSER`, `GETEVENTMSG`, `SEIZE`, `INTERRUPT` or `LISTEN`.
- **Memory access can allocate under `mm->lock`**: `debug_page` makes untouched pages resident (and a breakpoint's private copy) with the tracee's `mm->lock` held, the one place a user page is allocated under that lock (faults drop it first). Reclaim only ever trylocks other mms, so this cannot deadlock, but direct reclaim may run from there.
- **`uvm_peek` allocates**: reading an untouched page of the tracee makes it resident (a zeroed page).

## How it works: db

### Starting

```
db PROG [ARGS...]          db --args PROG ARGS...          db -p PID [PROG]
   [-ex CMD]... [-batch]
```

- The program is found as given if the name contains `/`, else in the current directory, else in `$PATH` (`/bin`). With `-p` and no program, the name is taken from `/proc/PID/cmdline` and searched the same way.
- `Reading symbols from X...`; without DWARF it adds `(No debugging symbols found in X: compile with tcc -g for source-level debugging)` and works at the symbol and instruction level.
- `-ex CMD` (up to 16, also spelled `-x`) runs commands at startup, each echoed as `(db) CMD`. `-batch` then kills (or detaches from) the program and exits 0. `make test` uses this.
- `run` forks; the child calls `PTRACE_TRACEME` and `execv`s the program (arguments split on blanks, no shell, no redirection); db waits for the `exec` SIGTRAP and continues.
- db ignores SIGINT itself; the child has the default action, so `^C` stops the program (a tracing stop, reported as SIGINT) and returns to the prompt.
- An empty line repeats `step`, `next`, `stepi`, `nexti`, `continue`, `until`, `finish`, `up`, `down`, `x` and `list` (the last two continue where they stopped).

### Commands

| Group | Commands |
|---|---|
| Running | `run`/`r [ARGS]`, `start [ARGS]` (temporary breakpoint at `main`), `continue`/`c [N]` (ignore the current breakpoint N-1 more times), `kill`/`k`, `attach PID`, `detach`, `quit`/`q`, `file PROG` |
| Stepping | `step`/`s [N]`, `next`/`n [N]`, `stepi`/`si [N]`, `nexti`/`ni [N]`, `finish`/`fin`, `until`/`u [LOC]`, `advance LOC` |
| Breakpoints | `break`/`b [LOC] [if COND]`, `tbreak LOC`, `delete`/`d [N...]`, `disable [N...]`, `enable [N...]`, `condition N [COND]`, `ignore N COUNT` |
| Stack | `backtrace`/`bt`/`where [full] [N]` (`full` adds each frame's locals), `frame`/`f [N]`, `up [N]`, `down [N]` |
| Data | `print`/`p[/FMT] EXPR`, `output EXPR`, `x[/NFU] [ADDR]`, `display[/FMT] EXPR` (shown at each stop where it can be evaluated), `undisplay [N]`, `set var X = E` (or `set X = E`), `ptype EXPR`, `whatis EXPR` |
| info | `info breakpoints`, `locals`, `args`, `registers [R]`, `all-registers` (also `float`: f0–f31 as double and hex, and fcsr), `frame`, `line [LOC]`, `symbol ADDR`, `functions [S]`, `variables [S]`, `display`, `source`, `program` |
| Source | `list`/`l [LOC \| N,M \| -]`, `disassemble`/`disas [FUNC \| START,END \| START,+LEN]` |
| Other | `shell CMD`/`!CMD`, `echo TEXT`, `help` |

**Locations** (`LOC`): `*EXPR` (an address), `FILE:LINE` (by full path, base name or path suffix), `LINE` (in the current file: the one at the pc, else the last listed, else `main`'s), `FUNCTION` (after its prologue, from DWARF), or an ELF function symbol (its exact address, for code without DWARF).

**Formats** for `print` and `x`: `x d u o t c a f s i`; sizes for `x`: `b h w g` (default `x/1xw`, remembered between uses). `x/i` disassembles; `x/s` prints a string.

### Breakpoints

A breakpoint is a 2-byte `c.ebreak` (0x9002) written over the first two bytes of the instruction, with the original two bytes saved. Breakpoints are **in memory only while the program runs**: all are inserted (`PTRACE_WRITE`) just before each `PTRACE_CONT` and removed at every stop, so memory reads, `x` and `disassemble` always see the real code. Resuming from a pc that has a breakpoint first steps over it. On a hit the pc is already on the breakpoint (the kernel leaves `sepc` on the `ebreak`); db counts the hit, evaluates the condition (a false condition does not count) and applies the ignore count, and silently continues if it should not stop.

### Single-stepping in software

`next_pcs` decodes the instruction at the pc and lists every possible next pc (at most 4): both targets of a conditional branch (`B`-type, `c.beqz`, `c.bnez`), the target of `jal`, `c.j`, and of `jalr`, `c.jr`, `c.jalr` (from the register value, low bit cleared), and otherwise pc+2 or pc+4 for compressed or full instructions. A `jal`/`jalr` writing `ra` or `t0`, or `c.jalr`, is a call. db plants temporary `c.ebreak`s there (no user breakpoints during a raw step), continues, waits, and removes them.

- **`stepi`** is one such step. **`nexti`** on a call runs to the return address with breakpoints in place; if it arrives with `sp` below the starting `sp`, it is a deeper recursive call, and it keeps going.
- **`step`/`next`** single-step instruction by instruction (one ptrace round trip each, up to 1,000,000 instructions) until one of: the pc is the starting frame's return address with `sp` ≥ the starting CFA (the function returned: stop in the caller, mid-line); the entry of another function (`step` runs on to its first line after the prologue, the second line-table row whose line differs; `next` steps over the call by running to the return address, using the recursion check above); another function not at its entry; a statement row (`is_stmt`) at its exact address with a different line or file; a backward jump to a different line of the same file (a loop head). `step` into code with no line information runs out of it again.
- **`finish`** runs to frame 1's pc and accepts the stop only when `sp` ≥ frame 0's CFA (so a recursive call of the same function does not stop it early), then prints `Value returned is $N = ...` from `a0` (or `fa0` for floating-point types).
- **`until LOC` / `advance LOC`** run to an address with user breakpoints active; plain `until` is `next`.

### Unwinding

The stack is unwound through frame pointers (`-fno-omit-frame-pointer` for GCC builds; tcc always keeps them): the return address at `[s0 - 8]`, the caller's `s0` at `[s0 - 16]`, at most 64 frames. The innermost frame needs care, because the pc may be in a prologue or epilogue where `s0` is not yet (or no longer) this function's: `frame_is_set_up` scans up to 96 bytes from the function start for `addi s0,sp,N` (or `c.addi4spn s0`) before the pc, and up to 32 bytes after the pc for `ld s0` (still set up) or `ret` (already torn down). When the frame is not set up, the CFA is `sp`, the caller's pc is `ra` and its `s0` is the current `s0`; otherwise the CFA is `s0`. The walk stops when the pc leaves `.text` or `s0` is 0, misaligned or not increasing.

### The DWARF reader (`dwarf.c`)

- **Units**: versions 2 to 5 (`tcc -g` emits 5, GCC 5 by default), 32- and 64-bit DWARF, address size 8. DWARF 5 unit types and every DWARF 5 form, including the indexed ones (`strx*`, `addrx*`, `loclistx`, `rnglistx`) with `str_offsets_base`, `addr_base`, `loclists_base` and `rnglists_base`, `line_strp`, `implicit_const` and `data16`.
- **Sections**: `.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str`, `.debug_line_str`, `.debug_str_offsets`, `.debug_addr`, `.debug_loclists` and `.debug_loc`, `.debug_rnglists` and `.debug_ranges`. Not `.debug_frame` or `.eh_frame`.
- **Line tables**: versions 2–5 (DWARF 5 directory and file entry formats), split into sequences sorted by address; a pc maps to the last row at or below it in its sequence. Source files are found at the recorded path, then by base name in the current directory, then in `/usr/src`.
- **Functions and globals**: the subprograms with an address range and the variables with a location or constant value that are direct children of a unit.
- **Types**: base types (signedness and char-ness from the encoding), pointers, typedefs, structs and unions (member offsets, bit-fields in both DWARF 4 and 5 styles), arrays of up to 8 dimensions, enums, and function types; qualifiers are dropped.
- **Scopes**: the parameters and variables of the function at the pc and of the lexical blocks containing it, innermost first, then the unit's globals, then all others. Inlined subroutines are not shown.
- **tcc's parameter order**: tcc lists a function's parameters last to first; for units whose `DW_AT_producer` starts with `"tcc "`, db reverses them so frames print `add (a=2, b=3)`.
- **Locations**: DWARF 5 location lists (all `DW_LLE` kinds) and DWARF 4 `.debug_loc`, so GCC `-O1` code, where a variable moves between registers and the stack, prints wherever a list entry covers the pc. The evaluator handles constants, stack operations, arithmetic and logic, `deref`, `reg*`/`breg*`/`regx`/`bregx`, `fbreg`, `call_frame_cfa`, `implicit_value`, `stack_value` and the first `piece`.
- **Frame base**: `DW_OP_reg8` (tcc: the frame pointer) gives the frame's `s0`; `DW_OP_call_frame_cfa` (GCC) gives the CFA from the unwinder, which is `s0` when the frame is set up and `sp` otherwise. Registers other than `sp` and `s0` are known in the innermost frame only, so in outer frames register-allocated variables show as optimized out.

### Expressions and printing (`expr.c`)

C expressions over the program's variables, registers (`$pc`, `$sp`, `$fp`, `$ra`, `$a0`..., `$x0`–`$x31`, `$f0`–`$f31` and `$fa0`-style names, assignable) and literals: arithmetic, comparisons, logic, bit operations, shifts, unary `- ! ~ * &`, `[]`, `.`, `->`, `?:`, assignment, `sizeof`, pointer arithmetic, and casts to built-in arithmetic and pointer types (`(char *)`, `(uint32_t)`, ...). Names are looked up in the selected frame's scopes, then as functions, then as ELF symbols (a data symbol without debug information is read as `char`, `int` or `long` by its size). Values print the way gdb prints them: `{a = 1, b = {...}}` for structs, arrays with `<repeats N times>` for runs of 10 or more, strings for `char` arrays and pointers, `(type *) 0x...` and `<symbol+offset>` for pointers, `65 'A'` for chars, enumerator names for enums, and `%.17g` for floating point.

### The disassembler (`disas.c`)

RV64 I, M, A (with `.aq`/`.rl`), F, D, Zicsr (with names for `fflags`, `frm`, `fcsr`, `cycle`, `time`, `instret`), `fence`, `fence.i`, `ecall`, `ebreak`, `sret`, `wfi`, and the compressed set, in objdump style (compressed instructions without the `c.` prefix), with the usual pseudo-instructions (`li`, `mv`, `ret`, `j`, `beqz`, `nop`, `sext.w`, `fmv.d`, ...) and branch targets as `0x... <symbol+offset>`. Unknown encodings print as `.word`/`.short`.

## Concurrency and locking

The kernel side's rules are in `ptrace.c`'s header comment: `tracer`, `ptrace_stopped` and `ptrace_resume_sig` change under `proc_lock`; a stopped tracee does not touch its registers or memory, and the tracer accesses them under `proc_lock`, which also keeps the tracee alive; copies to and from the tracer's own memory are made with the lock dropped. `db` is a single-threaded process that only touches the tracee while it is stopped.

## Interfaces

| Interface | Where |
|---|---|
| `ptrace` (117) | [03](03-traps-interrupts-syscalls.md) system call table; this document |
| `long ptrace(int request, pid_t pid, void *addr, void *data)` | `<sys/ptrace.h>`, `user/lib/unistd2.c` |
| `db` | `/bin/db`; sources in `/usr/src/db/` |
| State `t` | `/proc/<pid>/status` (`ustate`), `ps` |

## Debugging and observability

- **Kernel**: `log=proc:debug` logs attaches and every tracing stop (`pid N (name) stopped for its tracer (pid M) by SIGx`); `trace=proc` records `proc_stop` events; `ps` shows `t` and the wait reason `traced`; `strace db ...` shows every `ptrace` call with its request.
- **Source-level debugging of system programs**: the programs in `/bin` are GCC builds stripped of DWARF, so db works with their symbols and disassembly only. Rebuild them with debug information on the guest (`cd /usr/src && make CFLAGS=-g bin/ls`) and debug `bin/ls`, or debug the full GCC build `build/user/bin/<prog>` from the host with GDB.
- **When variables show as optimized out**: in an outer frame, only `sp`- and `s0`-based locations can be read; select frame 0 or step to where the variable lives in memory. In a prologue or epilogue, frame-base-relative variables of GCC code can be wrong (the CFA comes from heuristics, not CFI); step past the prologue.
- **When a stop makes no sense**: `info program`, `info frame` and `x/8i $pc` show where the program is; `info breakpoints` shows hit counts and conditions.

## Limitations and known issues

- **No watchpoints**, no `catch`, `handle` or `signal` commands, no calling functions from expressions, no `jump`/`return`, no convenience variables, and `$N` history values cannot be used in later expressions.
- **Signals are always passed on.** After a stop by a signal, the next resume delivers it again (except SIGTRAP, SIGINT and SIGSTOP); there is no way to discard it.
- **Fork**: breakpoints are in memory while the program runs, and `fork` copies them, so a child forked by the debugged program inherits the `c.ebreak`s; the child is not traced, so the first breakpoint it reaches kills it with SIGTRAP. There is no fork following. (`exec` in the child replaces the image, so a child that execs at once is safe.)
- **Stepping is slow**: `step`/`next` take one ptrace round trip per instruction, and give up silently after 1,000,000 instructions. Stepping through an `lr`/`sc` loop may never succeed, since each trap can cancel the reservation.
- **Unwinding needs frame pointers**: no CFI, so code compiled without frame pointers, or a pc inside an unusual prologue, gives wrong or short backtraces.
- **Expressions**: casts only to built-in types (not to structs, unions or typedefs); `&&`, `||` and `?:` evaluate both sides; no string literals, `++`, `--` or compound assignment.
- **Typedefs of structs, unions, enums and arrays** print under the aggregate's name (`struct node`), not the typedef's: the type is shared rather than copied, because a copy taken while a self-referential struct is still being read would never get its members.
- **System programs** have no DWARF in the image (see above).

## Potential improvements

1. **Watchpoints through page protection** — Write-protect the page holding the watched object (a kernel-side `PTRACE` request, since `mprotect` from the tracer does not reach the tracee), take the fault, single-step the store in software with the page writable, compare the value, and re-protect. Slow, but needs no hardware triggers. Impact: medium. Effort: medium.
2. **Fork and exec events** — `PTRACE_SETOPTIONS` with `TRACEFORK`/`TRACEEXEC`-style options: new children start traced (and stopped), so db can remove its breakpoints from them or follow them. Fixes the inherited-breakpoint problem. Impact: medium. Effort: medium.
3. **Separate debug files for `/bin`** — Install the DWARF of the GCC builds (`objcopy --only-keep-debug`) under `/usr/lib/debug/bin/`, with a `.gnu_debuglink`, and have db load them, so every system program debugs at source level. Impact: medium. Effort: small.
4. **CFI unwinding** — Read `.debug_frame` (GCC) and synthesize rules for tcc's fixed prologue, so backtraces are exact in prologues, epilogues and leaf functions, and callee-saved registers (and the variables in them) are known in outer frames. Impact: medium. Effort: medium.
5. **Richer expressions** — Casts to struct, union and typedef types, function calls in the inferior (set up the registers and a return breakpoint), `$N` history and convenience variables, and `handle`/`signal` to control signal delivery. Impact: low. Effort: medium.
6. **`PTRACE_SYSCALL`** — Stops at system call entry and exit, which would let `strace` be a user program and db `catch syscall`. Impact: low. Effort: small.
