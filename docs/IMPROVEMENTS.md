# Potential improvements

A prioritized list of what could be better in Eunuchs, gathered while documenting every subsystem from its source. It has four parts:

1. Bugs found during that review, by severity: [section 1](#1-bugs-found-while-documenting).
2. The improvements most worth doing, ranked: [section 2](#2-top-recommendations).
3. Patterns that repeat across subsystems: [section 3](#3-cross-cutting-themes).
4. Improvements by subsystem, with links to their detailed write-ups: [section 4](#4-improvements-by-subsystem). Larger directions for the system as a whole close the list: [section 5](#5-larger-directions).

**Legend:**
- **Impact** (H/M/L): how much it matters for correctness, robustness, security or usefulness.
- **Effort** (S/M/L): small is under a day, medium a few days, large a week or more.
- ✔ **verified**: confirmed by reading the code during the review. Everything else is a finding from reading the code that has not been reproduced by a test yet.

Each subsystem document in [subsystems/](subsystems/) ends with its full list, giving the reasoning and exact code locations for every item; this page summarizes and ranks them.

## 1. Bugs found while documenting

None of these show up in `make test` or `make ci` today. Most need an unusual input, a race window, or a configuration the tests don't use. That is also an argument for the testing improvements in section 2.

### 1.1 Security

The system is a single-user teaching OS running in a VM, but it does have users and permissions, and these defeat them.

| # | Bug | Where | Consequence |
|---|---|---|---|
| S1 | `kill` has no permission check ✔ | `proc/signal.c: do_kill`, `signal_send_locked` | Any user can signal any process, including root's |
| S2 | exec checks one file and runs another ✔ | `proc/proc.c: image_load` (permission and setuid bits from `namei(path)`, bytes from a second `vfs_read_all(path)`) | A rename between the two lookups runs different contents with the first file's setuid identity |
| S3 | ELF parser trusts the file | `proc/elf.c: elf_load` (unchecked 64-bit sums, `phentsize`), `elf_symbolize` (no bounds) | A crafted executable makes the kernel read past its buffer |
| S4 | Debug system call is unprivileged | `proc/syscall.c: do_debug` (`DBG_BREAK`, `DBG_FSCRASH`, `DBG_PROFILE`) | Any user can stop the machine, arm a simulated power failure, or write files on the host through semihosting |
| S5 | `strace` buffer overflow | `proc/syscall.c: strace_enter` (`len += ksnprintf` adds the untruncated length, so `size - len` wraps) | A long `execve` argument list under strace writes past `struct proc` |
| S6 | Device-tree parser trusts the blob | `core/fdt.c: fdt_parse`, `fdt_walk` | A malformed DTB reads past its end (the DTB comes from QEMU, so this is hardening) |

### 1.2 Hangs and deadlocks

| # | Bug | Where | Consequence |
|---|---|---|---|
| H2 | Retry loop on safe accesses to user memory (likely) | `arch/riscv/trap.c: trap_handler`: the copy-on-write/demand-paging retry matches every exception-table entry, including `safe_read_*` | A `safe_read` of a mapped user address (monitor `mem 0x10000` after a user breakpoint, a backtrace into user space) faults forever |
| H3 | netd can miss its wakeup | `net/net.c: netd`: checks for pending timers, drops `net_lock`, then waits; a `net_kick` in between is lost | TCP timers stall until the next packet arrives |
| H4 | Any trap after a stack overflow halts | `arch/riscv/trap.c: stack_overflow`: `overflow_sp` is never cleared | The overflow report's own `safe_read` faults turn into "overflow while handling overflow" and QEMU exits |
| H6 | Monitor commands allocate memory | `debug/monitor.c` → `trace_dump_file`, `profile_report`, `profile_start`, `kmalloc` | If a parked CPU holds a slab lock, the monitor wedges |
| H7 | Tests that hang instead of failing | `user/bin/utests.c: t_tcp_loopback_echo` (poll loop never ends), `user/bin/stress.c: tcp_round` (`accept` waits forever) | A failure shows up only as a timeout, with no hint of which check failed |
| H8 | Persist probes and orphans never give up | `net/tcp.c: retransmit_timeout` | A closed socket whose peer vanished with a zero window lives forever |
| H9 | Idle CPUs spin on unrunnable work | `core/sched.c: cpu_idle_loop`: waits only when the run queue is completely empty | Threads bound to another CPU keep idle CPUs busy taking `sched_lock` |

### 1.3 Memory or data corruption

| # | Bug | Where | Consequence |
|---|---|---|---|
| C1 | A double free can go undetected | `mm/buddy.c: free_block`, `mm/pmm.c: free_pages_site`: a page merged into a lower buddy keeps flags 0, so freeing it again passes every check | Two allocations later overlap |
| C2 | Inode reuse race | `fs/vfs.c: iput` vs `efs.c: efs_alloc_inode` | A new file can be written back as a free inode |
| C3 | Use after free | `fs/vfs.c: vfs_read_all` reads `ip->type` after `iunlockput` on an error path | Reads freed memory |
| C4 | Allocation state updated without a lock | `fs/efs.c`: `alloc_lock` exists but is never taken; `bhint`, `nfree_*` race | Wrong free counts, and racing allocation hints on SMP |
| C5 | I/O errors are cached as valid data | `fs/bcache.c: bread` marks failed reads valid; `log.c` ignores `bwrite` errors; no virtio flush | Corrupt data, and a commit can follow a failed log write |
| C6 | Transaction size isn't enforced | `fs/efs.c: free_ind`, `log.c` (`MAXOPBLOCKS` never checked per operation) | Deleting a very large file on a bigger image panics with "transaction too big" |
| C7 | Missing memory barrier in virtqueues | `drivers/virtio.c: virtq_get` (no fence between reading `used->idx` and the ring entry) | Could read a stale completion on real weakly ordered hardware |
| C8 | Lost receive buffers | `drivers/virtio_net.c: post_rx` (a failed refill is never retried) | The receive ring shrinks for good under memory pressure |
| C9 | Restart after `rt_sigreturn` | `proc/syscall.c: syscall_dispatch` + `signal.c: do_rt_sigreturn` | If the restored `a0` is -512, the interrupted context is rewound and changed |
| C11 | `httpd` directory listing can overflow its buffer | `user/bin/httpd.c: handle` | One long entry wraps `sizeof(body) - len` |
| C12 | Process freed while still listed | `proc/proc.c: proc_spawn` failure path frees a proc still in `all_procs` | Dangling list entry (needs an allocation failure) |
| C13 | Wrong FIN acknowledgement in simultaneous open | `net/tcp.c: ack_advance` | `fin_was_acked` set too early (rare path) |

### 1.4 Wrong behavior

- **Signals:**
  - `kill` of a running process waits for its next trap (no IPI).
  - `SA_RESTART` restarts `nanosleep` for its full time.
  - `SA_NOCLDSTOP` is ignored.
  - A signal sent to a child in the middle of `fork` is lost (`signal_fork` clears pending).
- **Init:** if init exits before `kinit` registers as its waiter, the kernel panics. Orphans of init are never reaped.
- **TCP:**
  - Karn's rule is defeated after a timeout (`retransmit_timeout` restarts RTT timing).
  - RSTs are sent at `snd_nxt` instead of `snd_max`.
  - ACK/RST information is dropped when our window is zero.
  - `shutdown(SHUT_WR)` in SYN_SENT is ignored.
  - ECONNRESET is reported as EPIPE, and `SO_ERROR` doesn't clear.
- **Network logging:** `net_log` levels don't match the kernel's log levels (TCP state changes need `log=net:trace`; level 5 can't print).
- **Terminal:** `O_NONBLOCK`, `VTIME`, `VMIN > 1`, `OPOST`/`ONLCR` and SIGTTOU are not implemented. `TIOCSPGRP`/`TIOCSCTTY` are not validated.
- **Filesystem:**
  - `fsync`/`sync` return before in-flight operations commit.
  - Pipes ignore `O_NONBLOCK`.
  - `getdents64` truncates 59-byte names.
  - `access()` uses effective ids.
- **Profiler:**
  - The real sampling rate is `HZ * (hz / HZ)`, but the requested rate is recorded (250 → 200 Hz).
  - A failed dump leaves profiling off.
  - User frames in other threads' backtraces are symbolized against the wrong program.
- **Tests and build:**
  - `make test APPEND=...` cannot override stage options (`cmdline_get` takes the first match), and the command line is silently cut at 32 words or 256 bytes.
  - `fs.img` doesn't depend on the Makefile, and objects don't depend on compiler flags.
- **Statistics:** many counters are updated without atomics on SMP (`tlb_shootdowns`, lock contentions, block statistics, `nb_live`, slab large-allocation counters), so they are approximate.

### 1.5 Documentation and code hygiene

These were all reported by the reviews:
- stale comments: the monitor's "end of boot" entry, `console_getc` "Phase 3", the UART's "console ring", `.ksyms` "after .bss", the devfs list, `trap.h` "trap.S", `SA_RESTART` in `uapi/signal.h`, and `echod`'s logging claim;
- dead code: `uart_disable_rx_irq`, `sockets_print`, the `FIONREAD` define, the `quiet` variable in `test_tcp.c`;
- duplicated constants: `WNOHANG`/`WUNTRACED`, and `hartids[8]` sized by coincidence with `MAX_CPUS`;
- four places that assume a 10 MHz timebase while `timer_init` has no fallback.

The stale statements in `docs/` were fixed during the review; the comments in the source are still to do.

### 1.6 Fixed since the review

- **H5, spurious end-of-file on the terminal.** The TTY input queue now stores `^D` as a marker entry (`Q_EOF`) in line with the text, and one counter of line ends is kept right by every push and pop, in every mode ([07](subsystems/07-devices-and-tty.md#tty-read-tty_read)).
- **Flaky `tcp_loopback_echo`.** A blocking TCP `send` returned a short count when the send buffer filled, so the test's echo server (and `echod`) gave up on a "short" `write` and reset the connection, about one boot in seven. Blocking sends now wait until all the data is queued, as on Linux. The test's client uses a non-blocking socket for its `poll` loop, and its server loops on `write`.
- **mkfs file modes.** mkfs now takes file modes from the host: 0755 for executables, 0644 for everything else (`/etc/passwd` included).
- **The kernel could have used floating point.** It was compiled for `rv64gc` while saving no FP state. It is now built for `rv64imac_zicsr_zifencei`/`lp64`, so the compiler cannot emit an FP instruction in kernel code; `fp.S` is the only kernel code that touches the FP registers ([11](subsystems/11-build-and-testing.md#compiler-flags-and-why)).
- **Editor and libc bugs found while documenting them**: `vi -R` argument order, `:g` without a command, `:noh`, out-of-range addresses, `:wq` with other modified buffers, `:%m0`, `:r !cmd` with no output, pasting macro registers, prompt histories, emacs `C-x C-c`'s prompt, `M-` digit arguments and `M-%` with an empty replacement; `fscanf` losing read-ahead (now `ungetc` backs up in the buffer); prefix-matched regex class names; `round`, NaN and huge arguments in `math.c`; misplaced declarations in `unistd.h`.
- **H1, transactions and inode locks in both orders.** `vfs_open` (`O_TRUNC`), `chmod_inode`, `do_fchownat` and `do_utimensat` took the inode lock before `fs_begin_op`, which could deadlock waiting for log space. They now begin the transaction first, as writes do ([06](subsystems/06-filesystems.md)).
- **C10 and the old shell's wrong behavior.** The shell was replaced by [esh](subsystems/12-shell.md). It expands each command when it runs, applies redirections to builtins, has its own state per `source`, and frees per-command memory. It has no fixed word limits, and doesn't use process groups when not interactive.

## 2. Top recommendations

Ranked by value for effort. The first group is mostly small fixes for the bugs above. The second makes future bugs cheaper to find. The third extends the system.

**Fix (small, high value):**

| Rank | Improvement | Impact | Effort | Bugs |
|---:|---|:-:|:-:|---|
| 1 | Permission checks: `kill`, `SYS_debug`, `TIOCSCTTY`/`TIOCSPGRP` | H | S | S1, S4 |
| 2 | Exec from the already-checked inode; harden the ELF parser and symbolizer | H | S | S2, S3 |
| 3 | One lock order for transactions (`fs_begin_op` before `ilock` everywhere): done, see 1.6 | H | S | H1 |
| 4 | Make double frees of coalesced pages detectable (poison and flag every page of a free block) | H | S | C1 |
| 5 | Limit the uaccess retry to `copy_*_user` instructions (a separate exception table or flag) | H | S | H2 |
| 6 | Fix `strace_enter` bounds; clear `overflow_sp` after reporting | H | S | S5, H4 |
| 7 | Line-discipline unit tests (`KTEST`s feeding `tty_input`), now that the accounting is fixed | M | S | |
| 8 | Fix netd's lost kick (check for work after `prepare_to_wait`, under the lock) | H | S | H3 |
| 9 | Fix the inode reuse race and the `vfs_read_all` use-after-free; take `alloc_lock` | H | S | C2–C4 |
| 10 | No allocation in monitor paths (preallocate, or refuse while the world is stopped) | H | S | H6 |

**Make bugs cheaper to find:**

| Rank | Improvement | Impact | Effort |
|---:|---|:-:|:-:|
| 11 | Turn hangs into failures: per-test watchdog in `ktest_run`, bounded loops in user tests and stress, overall stage time limits | H | S |
| 12 | Stage and test selection: `run_tests.py --only/--from/--list`, `utest=only:<prefix>`, let `APPEND` override stage options | H | M |
| 13 | Propagate I/O errors through the buffer cache and log; add virtio flushes and a real `fsync` | H | M |
| 14 | Wider deadlock detection: hung-thread reports for wait queues and semaphores (not just mutexes), a held-mutex check at `thread_exit`, a soft-lockup detector | H | M |
| 15 | Fault-injection coverage like `net.drop` for the disk (`blk.fail=`), allocators (`kmalloc.fail=`) and log size, run in CI | M | M |
| 16 | Machine-readable test results (JSON/JUnit) plus a `make gdb-check` smoke test of the `eu-*` commands | M | S |

**Extend:**

| Rank | Improvement | Impact | Effort |
|---:|---|:-:|:-:|
| 17 | Shell: regular expressions for `[[ =~ ]]`, extended globbing and glob qualifiers, `SIGWINCH` from the console | M | M |
| 18 | TCP: NewReno fast recovery, delayed ACKs and Nagle, window scaling with configurable buffers | M | M |
| 19 | User-space C library: malloc with checked poison, canaries and delayed reuse; flush before `fork`; POSIX leftmost-longest regex; `grep -E` on the libc regex | H | M |
| 20 | Scheduler: per-CPU run queues, or at least last-CPU affinity; split `sched_lock`; ticket spinlocks | M | L |

## 3. Cross-cutting themes

Patterns that showed up in several independent reviews. Fixing them as patterns, with a helper, a rule and a check, is cheaper than fixing each instance.

1. **Silent truncation and fixed tables.** Examples:
   - the command line (32 words, 256 bytes) and the `log=` spec (128 bytes);
   - reserved regions, harts and virtio devices (8 each);
   - ppoll wait registrations (128) and the environment (63 entries);
   - `getdents64` names, `httpd` files over 64 KiB;
   - the ARP and lockdep class tables.

   Several of these drop data with no message; a dropped reserved region could even hand firmware memory to the allocator. A `WARN_ONCE`-style helper that reports the first truncation per site would make all of them visible at boot or at first use.
2. **Statistics without synchronization.** Counters are updated from several CPUs without atomics. A tiny `stat_inc()` (relaxed atomic add), or per-CPU counters summed on read, fixes them all at once.
3. **Missing privilege checks around debugging features.** The debug system call, `DBG_FSCRASH` and semihosting dumps are all reachable from any user. One rule ("debug operations that affect the machine require euid 0") plus a check in `do_debug` covers them.
4. **Error paths that are rarely exercised.** Failure paths that leak memory, leave dangling list entries or report the wrong errno: `start_cpu`, `proc_spawn`, `virtq_init`, `vma_unmap`, `copy_strv`, and `KT_ASSERT` returning without cleanup. Fault injection (recommendation 15) would exercise them.
5. **Documentation drift.** The reviews found about a dozen stale comments. A short "invariants and assumptions" block at the top of each source file, kept in sync with the subsystem documents, and a docs review step in `make ci` would help (for example, grepping the docs for file and function names that no longer exist).
6. **Lock rules that the tools can't see.** lockdep checks spinlocks and mutexes, but not log space (H1), the netd wait (H3) or the stop-the-world interactions (H6). Each could be annotated, or checked by a lockdep "pseudo-lock" (Linux models such waits as locks for exactly this reason).

## 4. Improvements by subsystem

Condensed. Each document's last section has the detail, code locations and reasoning.

### [Boot and platform](subsystems/01-boot-and-platform.md#potential-improvements)
- Bounds-check the device-tree parser (M/S).
- Honor `status = "disabled"`, every `reg` entry and every memory node (M/M).
- Warn whenever a fixed table overflows (M/S).
- Don't hold `log_lock` with interrupts off across polled UART output (M/M).
- Structured log records with sequence numbers, and a streaming `/dev/kmsg` (M/M).
- Symbol sizes (and optionally data symbols) in `.ksyms` (M/S).
- Smaller items (L/S):
  - validate numeric boot options;
  - one timebase helper that fails early if the timebase is missing;
  - a fallback when SBI SRST is missing, and the SBI DBCN early console;
  - harden secondary-hart bring-up;
  - a build id instead of `__DATE__`.

### [Memory](subsystems/02-memory.md#potential-improvements)
- Detectable double frees (H/S).
- Restrict the uaccess retry (M/S).
- Emit the declared `mm` trace events (M/S).
- Clear `SUM` on kernel trap entry (M/S).
- Allocate slab pages outside the cache lock (M/S).
- Word-at-a-time user copies (M/M).
- ASIDs and ranged TLB shootdowns (M/M).
- Per-CPU page and object caches (M/L).
- Host tests for the slab and `uvm` logic (M/M).
- Smaller items:
  - failure-atomic `vma_unmap` and `vma_protect`, remote `fence.i` only to harts that ran the process, robust `ioremap`, atomic statistics (L/S);
  - freeing empty user page tables and reusing `mmap` holes, and a boot option to slim down slab debugging (L/M).

### [Traps, interrupts and system calls](subsystems/03-traps-interrupts-syscalls.md#potential-improvements)
- `strace_enter` bounds (H/S).
- Recover after a stack-overflow report (H/S).
- Keep `rt_sigreturn` out of restart logic (M/S).
- Privilege checks in `SYS_debug` (M/S).
- Sstc timer and tickless idle (M/M).
- Spread device interrupts across CPUs, with PLIC contexts from the device tree (M/M).
- Save FP registers only when `sstatus.FS` says they are dirty (L/S).
- Smaller items (L/S): `TF_*` offsets generated from C; prompt signal delivery to running processes; `missed_ticks` and per-syscall statistics; a sorted exception table.

### [Threads, scheduling and SMP](subsystems/04-threads-scheduling-smp.md#potential-improvements)
- Hung-thread detection beyond mutexes (H/M).
- An idle loop that only wakes for work it can run (M/S).
- Ticket or test-and-test-and-set spinlocks (M/S).
- A soft-lockup detector (M/S).
- A held-mutex check at exit (M/S).
- A generic `wait_event_timeout` (M/S).
- Split `sched_lock` (M/M).
- Better lockdep classes, and keep checking in warn mode (M/M).
- Per-CPU run queues or last-CPU affinity (M/L).
- Smaller items: fix the `start_cpu` failure path; validate `kthread_bind`; a time-based spin timeout; mutex fairness; print hung reports without holding `sched_lock`.

### [Processes and signals](subsystems/05-processes-and-signals.md#potential-improvements)
- `kill` permission check (H/S).
- Exec from the checked inode (H/S).
- ELF hardening (H/S).
- Fix the fork visibility window and the `proc_spawn` failure path (M/S).
- Fix init waiting and the reaping of init's orphans (M/S).
- Explicit restart state (M/S).
- Interrupts on during fatal faults (M/S).
- SIGCHLD and orphaned-group semantics: `SA_NOCLDSTOP`, `WCONTINUED`, SIGHUP (M/M).
- Complete TTY job control (M/M).
- Shared program text instead of one 4 MiB `kmalloc` per image (M/L).
- Smaller items: an IPI so signals reach running targets promptly; a pid hash with pid reuse; `setresuid` and supplementary groups; `sigaltstack` and `SA_SIGINFO`.

### [Filesystems](subsystems/06-filesystems.md#potential-improvements)
- Propagate I/O errors (H/M).
- One transaction lock order (H/S).
- Inode reuse race (H/S).
- `vfs_read_all` use-after-free (M/S).
- Use `alloc_lock` (M/S).
- Bound transaction size (M/M).
- A real `fsync`/`sync` (M/M).
- Ordered-mode journaling so file data isn't logged (H/L).
- An inode cache with hashing (M/M).
- Validate geometry at mount and in fsck; let fsck replay a committed log (M/S).
- A mount table (`/etc/fstab` or a `mount` call) instead of the two fixed labels and the fixed `/mnt/<tag>` 9P mounts, `/dev` nodes for every disk, and a real `st_dev` per filesystem (M/M).
- 9pfs: reclaim nodes and fids (never freed today); attribute or dentry caching; map host uids; symbolic links; names longer than 59 bytes; `Tfsync` for `fsync` (M/M).
- Pipe `O_NONBLOCK` and atomic writes (M/S).
- Smaller items: `getdents64` names; harden `vfs_getcwd`; root-only `DBG_FSCRASH`; `eu-files` for pipes.

### [Devices and the terminal](subsystems/07-devices-and-tty.md#potential-improvements)
- `VMIN`/`VTIME` and `O_NONBLOCK` (M/M).
- Retry receive-buffer refills and export the driver counters (M/S).
- virtqueue barrier, config generation, reset wait (M/S).
- Per-request block completion (M/S).
- Block flush at log commit (M/S).
- SIGTTOU and ioctl validation (M/S).
- Interrupt-driven, buffered UART output (M/M).
- Line-discipline ktests (M/S).
- Smaller items: `OPOST` and SIGWINCH; `/dev/diskN` for every disk; `EVENT_IDX` and batched kicks; scatter-gather I/O; a driver registry; `_POSIX_VDISABLE`.

### [Networking](subsystems/08-networking.md#potential-improvements)
- netd lost kick (H/S).
- Karn's rule after timeouts (M/S).
- RST at `snd_max` (M/S).
- Bounded persist probing and orphan lifetime (M/S).
- No `copy_from_user` under `net_lock` (M/S).
- ACK processing with a zero window (M/M).
- Socket API gaps: `SHUT_WR`, `SO_ERROR`, `FIONREAD`, `SO_RCVTIMEO` (M/M).
- NewReno (M/M).
- Delayed ACKs and Nagle (M/M).
- Window scaling (M/M).
- Smaller items: align log levels; generate and honor ICMP errors; buffer-health counters; better demultiplexing and TIME_WAIT reuse; RFC 6528 initial sequence numbers and RFC 5961 challenge ACKs; dead code.

### [Debugging internals](subsystems/09-debugging-internals.md#potential-improvements)
- No allocation in the monitor (H/S).
- Self-describing trace dumps (M/S).
- A Python decoder that matches `trace.c` (M/S).
- Data symbols (M/S).
- Monitor commands registered through a linker section (M/M).
- `make gdb-check` (M/M).
- A ring mode for the profiler (M/M).
- Per-test timing and leak checks in `ktest` (M/M).
- Smaller items: profiler rate and dump fixes; user symbolization per process; full trace dumps while stopped; semihosting in replays; a sorted symbol index.

### [User space](subsystems/10-user-space.md#potential-improvements)
- A malloc with real checks: delayed reuse, checked poison, canaries (H/M).
- Flush stdio before `fork` (M/S).
- `grep` and `head` on stdio, and `regcomp` in `grep` (`-E`, `-F`) and the pager (M/S).
- Payne–Hanek argument reduction for `sin`/`cos`/`tan` of huge arguments (L/S).
- POSIX leftmost-longest regular expressions without a step limit (M/M).
- Harden `httpd` (M/S).
- The editor:
  - macros, `.` and `:normal` through the `:` and `/` prompts; ranges in `:w` and `:w !cmd` (M/S);
  - split windows, visual block mode, numbered and append registers, `fill-paragraph`, emacs keyboard macros (M/L);
  - safer saving: `fsync`, owner and umask, warn when the file changed on disk (M/S);
  - smaller: `SIGWINCH`, marks that follow edits, wide-character widths, more syntax modes (e.g. Markdown, assembly) (L/M).
- Smaller items: a `basename` (and `dirname`) command, which does not exist yet; eutils arguments; consistent `execvp` limits; safer signal handlers; unwind information for `__restore_rt`.

### [The shell](subsystems/12-shell.md#potential-improvements)
- Regular expressions for `[[ =~ ]]` (M/S now that libc has `regcomp`).
- Extended globbing and glob qualifiers (M/M).
- More completion: program arguments, hosts, pids (M/M).
- vi mode and `bindkey` (M/M).
- `SIGWINCH` from the console (L/S).
- Smaller items: cache parsed functions; shared history between running shells; wide characters; time zones; process substitution.

### [The native toolchain](subsystems/13-toolchain.md#potential-improvements)
- Line numbers from `-g` DWARF in kernel fault reports (M/M).
- Self-hosting roadmap (M/L): steps 1 (tcc's assembler) and 2 (the user space builds itself, a byte-identical fixed point, `make selfhost`) are done. Next, step 3: the kernel's C (blocked by `stdint.h`, `__ATOMIC_*`, the `"K"` constraint and a register-allocator assertion) and its link (the linker script and the Python `gen_ksyms` two-pass link). Also: the libc's and tcc's sources in `/usr/src`, so the image rebuilds everything without the share; no libgcc in the regular build's `libtcc1.a`; `xtcc` with full `long double` precision on the Mac (it folds constants in the host's 64-bit `long double`). Remaining assembler gaps: `%hi`/`%lo`/`%pcrel_*`, automatic compression, `.cfi` unwind tables, `.irpc`, `\@`.
- `make -j` (M/M); more GNU make (`define`, `$(call)`, `$(eval)`, `vpath`) (M/M).
- `tcc -run` without `mprotect` (a fallback through `/tmp` and `exec`) (L/S); a smaller `libtcc1.a` (L/S).

### [The debugger](subsystems/14-debugger.md#potential-improvements)
- Watchpoints through page protection (write-protect the page, single-step the faulting store, compare) (M/M).
- Fork and exec events (`PTRACE_O_TRACEFORK`/`TRACEEXEC`-style options), so `db` can follow children and keep breakpoints out of them (M/M).
- Separate debug files for `/bin` (`build/user/bin` DWARF installed under `/usr/lib/debug`) so the system programs debug at source level (M/S).
- CFI (`.debug_frame`/`.eh_frame`) unwinding instead of frame-pointer heuristics (M/M).
- Casts to struct and union types, and function calls, in `db` expressions (L/M).
- Do `ptrace` memory copies without holding `proc_lock` with interrupts off (M/S).

### [Build and testing](subsystems/11-build-and-testing.md#potential-improvements)
- Stage selection (H/M).
- Hangs become failures (H/S).
- A per-test `utest` filter (M/S).
- Overall time limits and fsck after the interactive and network stages (M/S).
- A cleanup-aware `KT_ASSERT` (M/S).
- Stamp files so flag and file-list changes trigger rebuilds (M/S).
- Machine-readable results (M/S).
- `APPEND` overrides stage options (M/S).
- A test stage with a second (home) disk (M/S).
- Smaller items: unique scratch paths; owners and modes for files in mkfs specs; host tests that don't need the cross toolchain.

## 5. Larger directions

Bigger projects that would each change what the system can do. They are listed roughly in the order they build on each other.

**Done since the review:**
- **Floating point for user programs.** User code is built for `rv64gc`/`lp64d`. The kernel enables FP per thread on its first FP instruction, saves and restores the registers at context switches of threads that use them, copies them at `fork`, resets them at `exec`, and carries them in signal frames ([03](subsystems/03-traps-interrupts-syscalls.md#lazy-floating-point-for-user-programs)).
- **A real C library.** Buffered `FILE` stdio, `printf` with floating point, `scanf`, `strto*`, `qsort`, `getopt`, `math.h`, `strftime`, `setjmp`/`sigsetjmp`, and POSIX regular expressions (BRE and ERE) ([10](subsystems/10-user-space.md#stdio-streams)).
- **Several disks and a persistent `/home`.** Up to four virtio disks, a buffer cache keyed by disk, a log per filesystem, volume labels, and `home.img`, which survives rebuilds ([06](subsystems/06-filesystems.md#boot-and-mounting)).
- **A native C toolchain.** TinyCC runs on Eunuchs (`/bin/tcc`, with `tcc -run` through the new `mprotect`), with a sysroot in `/usr`, a POSIX `make`, and the sources of every program in `/usr/src`, rebuilt in about 6 seconds in the guest ([13](subsystems/13-toolchain.md)).
- **tcc's assembler fixed and completed** (self-hosting step 1): macros, `.irp`, section types, gas-compatible `la`/`li`/`call`, every S- and M-mode CSR, the missing instructions, rewritten compressed instructions, and a dozen silent mis-encodings fixed; checked byte for byte against GNU as by a host test ([13](subsystems/13-toolchain.md#the-assembler)).
- **Eunuchs builds its own user space** (self-hosting step 2). `user/build.mk`, run by Eunuchs' `make` in the guest, builds the libc, tcc's runtime (without libgcc) and all 26 programs, tcc included, with tcc; `make selfhost` builds twice and requires identical results, then runs the test suite on the self-built image. Two tcc bugs found on the way (zero-extension of narrowed unsigned values, missing symbol tables without `-g`) are fixed ([13](subsystems/13-toolchain.md#eunuchs-builds-its-own-user-space-userbuildmk-make-selfhost)).
- **The Mac's files in the guest.** A virtio-9p transport and a 9P2000.L client (9pfs) mount the project directory on `/mnt/host` under `make run`: edit on the Mac, build and run on Eunuchs ([06](subsystems/06-filesystems.md#9pfs-host-directories)).
- **A native debugger.** `ptrace` in the kernel (tracing stops on signal delivery, register, FP and memory access, `exec` and breakpoint SIGTRAPs) and `db`, a gdb-like source-level debugger with its own DWARF 4/5 reader, software single-stepping and an RV64GC disassembler ([14](subsystems/14-debugger.md)). A user `ebreak` is now a SIGTRAP instead of freezing the machine in the monitor.
- **A text editor.** `edit`, with a vi personality (operators, text objects, visual mode, registers, macros, ex commands) and an emacs one (kill ring, incremental search, query-replace, `M-x`) ([10](subsystems/10-user-space.md#the-editor-vi-and-emacs)).

| Direction | What it involves | Why |
|---|---|---|
| **User threads** (`clone`, futex, per-thread signal masks, TLS via `tp`) | Many threads per `mm`: per-thread kernel state, an `mm` lock for real, TLB shootdowns for user mappings, thread-directed signals | The single-thread-per-process assumption is everywhere (mm locking, signals, `current_proc`); this is the largest structural change |
| **Page cache and file-backed `mmap`** | Cache file pages instead of whole-program `kmalloc` images; demand paging from disk; shared text; `msync` | Removes the 4 MiB program limit, makes `exec` cheap, enables shared libraries |
| **Dynamic linking** (an `ld.so`, a shared libc) | Needs the page cache and `mmap` of files | Smaller binaries; a more realistic user space |
| **A real filesystem alongside efs** (ext2, read-only first) | A second on-disk filesystem type through the VFS | Read disk images made elsewhere (exchanging files with the Mac is now done by 9pfs) |
| **Networking breadth:** DHCP, a DNS resolver, IPv6 | DHCP and DNS are small user programs on top of UDP; IPv6 is a larger stack addition | Talk to the outside world by name; QEMU's user network provides DHCP and DNS |
| **Per-CPU scheduling and a better scheduler** | Per-CPU run queues with load balancing, priorities and `nice`, maybe CFS-like fairness | Scales beyond a handful of harts; gives interactive priority |
| **Power and time:** tickless idle, the Sstc timer, `CLOCK_MONOTONIC_RAW` | Timer rework | Lower host CPU use while idle (QEMU spins less), finer timing |
| **SMP record/replay** | QEMU limits record/replay to one CPU; an in-kernel deterministic SMP scheduler mode (`smp.deterministic`) could make races replayable | Make SMP races reproducible, which is the hardest class of bug here |
| **Security model** | Capabilities instead of `euid == 0` checks, per-process resource limits, W^X for user `mprotect` | Today's model is root/non-root only |
| **Hardware beyond QEMU virt** | Device-tree-driven drivers, other boards (VisionFive 2), the Sv48 paging mode | Real hardware tests weak memory ordering and the "QEMU only" assumptions noted above |
