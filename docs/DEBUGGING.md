# Debugging Eunuchs

Quick reference for the kernel's debugging facilities. If you came here because something broke, start with the [cookbook](#cookbook-how-to-debug-) at the end. [ARCHITECTURE.md](ARCHITECTURE.md) explains how the pieces fit.

## Console gotcha: output wrappers

If typed characters appear only after pressing Return, something is piping
make's output. A common culprit is **grc**, whose zsh integration wraps `make` in a shell function.
Run `command make run` to bypass it (`\make` is not enough: grc uses a shell
function, not an alias). `make run` warns
when it detects this.

## Boot options (`make run APPEND="..."`)

| Option | Effect |
|---|---|
| `log=<subsys\|all>:<level>[,...]` | Per-subsystem log threshold, e.g. `log=all:debug,trap:trace` |
| `quiet` | Console shows only warnings/errors (the log ring still gets everything that passes the threshold) |
| `break` | Enter the debug monitor right after early boot |
| `ktest[=prefix]` | Run in-kernel unit tests, then exit QEMU with pass/fail |
| `panic=shutdown` | Exit QEMU on panic (exit code 2) instead of entering the monitor |
| `nopoison` | Turn off free-page poisoning (faster, but hides use-after-free of pages) |
| `trace=<groups>` | Trace groups to record: `sched,irq,lock,mm,mark,syscall,proc,blk,fs,net`, `all`, `none` (default: all but `mm`) |
| `tracedump=<path>` | With `ktest`/`utest`: write the trace to a host file after the tests (`make test` uses this) |
| `utest` | Run the user-space test suite (`/bin/utests`) and power off with its result |
| `init=<path>` | Program to run as pid 1 (default `/bin/init`, which runs the shell `/bin/ush`) |
| `utest=<mode>` | Run one `utests` scenario (`persist-check`, `crash-commit-write`, ...) instead of the suite |
| `fs.crash=<point>` | Stop the machine at `after-log`, `after-commit` or `mid-install` of the next filesystem commit (power-failure testing) |
| `smp=<n>` | Use at most n CPUs (`smp=1`: uniprocessor). `make run CPUS=n` sets how many harts QEMU has (default 4) |
| `lockdep=off\|warn\|panic` | Lock-order checking: report the first inversion and stop checking (default `warn`), panic on it (`make test`), or off |
| `watchdog=<seconds>` | Hard-lockup watchdog: panic if a CPU takes no timer interrupt for this long (default 10, `0` = off) |
| `hung=<seconds>` | Report threads waiting for a mutex longer than this (default 30, `0` = off) |
| `profile[=hz]` | Start the sampling profiler at boot (default 500 Hz): profile the boot, or a test run |
| `help` | List every boot option. Unknown options and bad values get a warning at boot, so typos don't go unnoticed |
| `ip=<a.b.c.d>/<bits>[,<gateway>]` | eth0's address (default `10.0.2.15/24,10.0.2.2`, QEMU's user network) |
| `net.drop=<percent>` | Drop that share of received packets (on `lo` too) at random, to exercise TCP's loss recovery |

Every log line carries the time, the CPU that logged it, and the level: `[    2.622148] 1 E proc: ...` is CPU 1, error, subsystem `proc`.

Levels: `err warn info debug trace` (or `0`–`4`). Subsystems: `core boot panic trap mm sched proc syscall vfs fs blk net drv debug test`.

## Debug monitor

The monitor is entered:
- with **`Ctrl-]`** on the console at any time while the system runs; `c` resumes it
- on any panic or unhandled exception (post-mortem; `c` is refused)
- on `ebreak` / `debug_break()` in kernel code (`c` resumes after the breakpoint)
- with the `break` boot option

The monitor stops the world: interrupts are off and no other thread runs until you `c`.
With several CPUs, the others are parked by an IPI (see [Multiprocessor](#multiprocessor-smp)).
Time spent in it is not charged to the interrupted thread.

Commands: `help regs csr bt mem poke sym dmesg log plat dtb traps ktest crash c reboot halt`,
plus memory: `meminfo pt v2p p2v page slab heapcheck kmalloc kfree`,
threads: `ps bt <tid> sched irq locks trace demo`, CPUs: `cpus cpu <n> lockdep`, network: `net netstat`, and `profile` (see below).
Address arguments accept numbers, `symbol+off`, or `$reg` from the trap frame (`$sepc`, `$sp`, `$a0`, ...).
Use the up/down arrows for history. To exit QEMU, press `Ctrl-^ X`.

`crash null|store|wx|uaf|overflow|stack|illegal|ebreak|panic|assert` deliberately triggers each failure path.
`demo pingpong|spin|deadlock` spawns threads to watch with `ps` and `trace`.

## Disk and filesystems

The root filesystem is **efs** on a virtio disk, `build/fs.img` (volume label `root`). `make` builds it with the host tool `build/tools/mkfs`. It holds the user programs from `user/bin`, `/etc/motd`, empty `/dev`, `/proc`, `/tmp` and `/home`, the toolchain in `/usr`, and the system's own source tree in `/usr/src/eunuchs` (64 MiB in all). The image is rebuilt whenever a program changes, which discards files created at run time.

`make run` (and `debug`, `qemu-log`) also attaches a second disk, `home.img` in the top directory (label `home`, 64 MiB, with `/guest` owned by uid 1000). The kernel mounts it on `/home`, so files kept there survive rebuilds and `make clean`; `make clean-home` deletes it. The kernel picks disks by label, not order: `/` is the disk labelled `root` (or disk 0), `/home` the one labelled `home`. The boot log says which (`mounted efs (disk1) on /home`). `make test` boots with the root disk only.

**The Mac's files**: `make run` (and `debug`, `qemu-log`) also shares the project directory through virtio-9p; the kernel mounts it on `/mnt/host`. It is the real directory, read-write, so what you do there happens on the Mac (`make run HOSTDIR=` shares nothing, `HOSTDIR=dir` another directory). Inside, `ls /mnt/host`, `cat /mnt/host/kernel/core/main.c`, or build: `cd /mnt/host/user/bin && make -f /usr/src/Makefile BIN=/tmp/bin`. `mounts` shows the 9P request and error counts, and `/proc/9p` (or the monitor's `blk`) the transport's. A `9p share "host" not mounted` line at boot means the image lacks `/mnt/host`.

devfs is mounted on `/dev` (`console null zero kmsg disk0 disk1 tty tty0`; `disk1` only when a second disk is attached); procfs is mounted on `/proc`.

**fsck**: `make fsck` checks `build/fs.img` (`build/tools/mkfs --check IMAGE`; run it by hand on `home.img`, with QEMU stopped). It prints the volume label and reports:
- superblock and log problems, and notes a committed log awaiting replay
- `.`/`..` errors, bad names, wrong link counts
- blocks outside the data area or used twice
- blocks used but marked free, or marked used but owned by nobody (leaked)
- orphaned inodes

`make test` runs fsck after every boot that writes.

**/proc** (text, generated at open): `meminfo slabinfo interrupts threads sched procs mounts bcache blk icache trace virtio uptime pipes tty cpus lockdep`, and per process `/proc/<pid>/{status,maps,fds,stack,ustack}` and `/proc/self`. `/dev/kmsg` is a snapshot of the kernel log.

**Monitor**: `mounts` (space, inode use, log statistics: commits, blocks logged/absorbed, commit latency, recovery at mount), `bcache` (hit rate; referenced/pinned buffers and **who took them**), `icache` (cached inodes, references, lock holders), `blk` (I/O counts, latency, in-flight, per disk; every virtio slot), `files <pid>`. These never block, so they work while the monitor has the machine stopped. **GDB**: `eu-bcache`, `eu-files <pid>`.

**Caught automatically:**
- buffer-cache exhaustion panics with the list of held buffers and their call sites (usually a missing `brelse`)
- `log_write` outside a transaction, and transactions that exceed their reservation
- double `iput`, double `file_close`
- on-disk inconsistencies noticed at run time (double free of a block, pointers outside the data area, stale entries pointing at free inodes). These are reported by `fs_error` with a backtrace and counted per mount (`mounts` shows the count).
- the kernel tests assert that no buffers or inode references leak, and that the in-memory free-block count matches the bitmap

**Crash consistency**: every change is logged (write-ahead, xv6 style) and committed atomically; mount replays a committed log. `make test` proves it by stopping the machine right after the commit point (the data must survive recovery) and right before it (the change must vanish). Use `fs.crash=` or `dbg_fscrash()` to experiment.

**Trace events**: `blk_submit`/`blk_done` (sector, length, latency), `file_open` (path tail), `log_commit` (sequence, blocks, µs), in groups `blk` and `fs`.

## User space and processes

The system boots into `init`, which prints `/etc/motd` and runs the shell, **`esh`** (also `/bin/sh`), as a login shell.

**The shell** is zsh-like; [subsystems/12-shell.md](subsystems/12-shell.md) is its manual. In short:
- the language: pipelines, lists, `if`/`while`/`for`/`case`, functions, arrays, `[[ ]]`, `(( ))`, here-documents, and zsh's expansions (`${x:-d}`, `${(U)x}`, `{1..9}`, `**/*.c`);
- job control: `^Z`, `fg`, `bg`, `jobs`, `kill %1`; finished background jobs are reported right away;
- editing: syntax highlighting, gray autosuggestions from history (Right accepts), Up/Down history search by prefix, `^R` incremental search, Tab completion with a menu on the second Tab, `!!`/`!$`/`^a^b`;
- `help` lists builtins and keys; `setopt` shows the options; `/etc/eshrc` and `~/.eshrc` are the start-up files.

Under `make run`, QEMU's monitor escape is `Ctrl-^` (`Ctrl-^ X` quits), so `Ctrl-A` is beginning-of-line in the shell and in emacs.

For debugging scripts and the shell itself: `set -x` traces commands after expansion, `whence -v`/`type` say what a name is, and `strace esh -c '...'` shows the system calls.

**Utilities:**
- files and text: `ls [-l] cat grep echo mkdir rm rmdir touch ln mv cp wc head tail tee stat chmod chown`
- processes: `ps top kill`
- system: `env id su date uname dmesg free sleep yes true false clear`
- debugging: `strace` and `dbg`

**Users**: root (0) and guest (1000) are listed in `/etc/passwd`. `su guest` switches users; `id` shows who you are. Permission bits are enforced on open, create, unlink, rename, exec and directory search. Root bypasses them, except that exec still needs an `x` bit. `/tmp` is world-writable.

**Debugging from the shell:**
- `strace <cmd>`: every system call with decoded arguments (signals by name, modes in octal)
- `dbg break` (stop in the kernel monitor), `dbg mark <text>` (trace marker), `dbg vm`, `dbg files`, `dbg ps`, `dbg fscrash <point>`
- `/proc/<pid>/status` shows credentials, process group and session, stopped state, pending/blocked masks, and the signals caught or ignored

**When a program crashes**, the kernel prints:
- the signal and the cause (`NULL pointer dereference`, `stack overflow (just below the 8192 KiB stack)`, `VMA 'text' is r-x`, `kernel address: ...`, illegal instruction bits)
- the user registers
- a **symbolized user backtrace**, using the program's own ELF symbol table
- the process's memory map (VMAs, resident pages, shared COW pages)

**strace**: `strace <cmd>` in the shell, `strace <pid> [on|off|follow]` in the monitor, or `dbg_strace()` from code. Output looks like `[4 hello] write(1, "hello from pid 4..."..., 49) = 49`, and errors like `= -1 ENOENT (No such file or directory)`.

**Signals** work as in Unix:
- handlers (with `sa_mask`, `SA_NODEFER`, `SA_RESETHAND`, `SA_RESTART`), `sigprocmask`, `sigsuspend`/`pause`
- default actions: terminate, core, ignore, stop, continue
- `SIGCHLD`, process-group `kill`
- interrupted calls fail with `EINTR`, or restart transparently after a stop/continue (`nanosleep` resumes with the remaining time)

Faults become catchable signals (`SIGSEGV`, `SIGILL`, `SIGBUS`). The detailed crash report is printed only when the signal is fatal. A handler that corrupts its stack is reported as "bad signal frame" instead of jumping somewhere random.

**Terminal**: the console tty has a line discipline:
- editing: `DEL`/`^H`, `^U`, `^W`; `^D` means EOF
- echo, with control characters shown as `^X`
- `^C` (`SIGINT`), `^\` (`SIGQUIT`) and `^Z` (`SIGTSTP`) go to the foreground process group
- raw mode via termios (`top` uses it)
- a background job that reads the terminal is stopped with `SIGTTIN`

`Ctrl-]` always goes to the kernel monitor first.

**Monitor**: `tty` (termios flags, foreground group and session, the line being edited, queued input, signals sent), `pipes`, `procs` (now with process group, session, and pending/blocked signal masks), `vm <pid> [va]` (VMAs, or a page-table walk in that address space), `ubt <pid>` (where a process is in user code), `strace <pid>`, `kill <pid>`. `dbg_break()` stops in the monitor with the user registers; `c` continues. An `ebreak` in user code is a SIGTRAP: it stops the program under a debugger (`db`), and otherwise kills it with a fault report.

**Debugging user programs**: `db PROG [ARGS]` (or `db -p PID`) is a gdb-like debugger that runs on Eunuchs, built on `ptrace`: breakpoints, stepping, backtraces, locals and expressions at source level for programs compiled with `tcc -g` (or `make CFLAGS=-g` in `/usr/src`), and symbols plus disassembly for the stripped programs in `/bin`. A traced process shows state `t` in `ps`. See [14-debugger.md](subsystems/14-debugger.md).

**GDB**: `eu-procs`. To debug user code, `add-symbol-file build/user/bin/<prog>` (full DWARF; the copies on the disk image are stripped).

**Safety checks that will bite you (on purpose):** the kernel can only touch user memory through `copy_from_user` / `copy_to_user` / `strncpy_from_user`. `sstatus.SUM` is enabled only inside them, so any other kernel dereference of a user pointer panics with "user-space address accessed from the kernel". Kernel addresses passed in by user programs are rejected with `EFAULT`. User memory is W^X, too: `mmap` or `mprotect` with `PROT_WRITE|PROT_EXEC` fails with `EACCES`; code generated at run time (`tcc -run`) is written first and then made r-x with `mprotect`.

User programs are built for `rv64gc` and may use floating point. The kernel is built without FP; a thread gets zeroed FP registers on its first FP instruction (an illegal-instruction trap the kernel absorbs) and from then on they are saved and restored at context switches, copied by `fork`, and carried in signal frames. In GDB, `info registers float` shows the live user values only while that thread is the one running on the CPU; otherwise look at `((struct thread *)T)->fp`.

Programs compiled on Eunuchs with `tcc` keep a symbol table; compile with `tcc -g` to debug them at source level with `db`. To chase a compile problem, `tcc -vv` shows the search paths and files used, `strace tcc ...` the system calls, and the same compiler runs on the Mac as `build/tcc/xtcc -Bbuild/sysroot/usr/lib/tcc`; disassemble its output with `riscv64-elf-objdump -d`. For an assembly file, compare tcc with GNU as: assemble with both, link each at a fixed address with `riscv64-elf-ld`, and compare the bytes (`tests/host/test_tcc_asm.py` does exactly this; see [13](subsystems/13-toolchain.md#the-assembler)). See [13-toolchain.md](subsystems/13-toolchain.md#debugging-and-observability).

## Threads, scheduling and locks

- `ps` lists every thread with its state, CPU time, context switches, and stack high-water mark. Sleeping threads also show what they wait on (`mutex 'x' held by 5(foo)`, `sem 'y'`, `sleep (12 ms left)`, `join tid 7`) and where they blocked. A preempted thread shows `preempted in <function>`.
- `bt <tid>` gives a full backtrace of any thread, including sleeping ones.
- `sched` shows the run queue, the sleep queue, switch counts and idle %. `irq` shows interrupt counts and time spent per handler. `locks` shows held spinlocks and who is blocked on which mutex.

**Caught automatically**, with names and code sites:

| Bug | Report |
|---|---|
| kernel stack overflow | Detected by guard pages: "KERNEL STACK OVERFLOW" naming the thread and stack range. The report is produced on an emergency stack; recursion is collapsed in the backtrace. |
| mutex deadlock (A↔B, or longer cycles) | Reported when the cycle forms: the chain of threads, the mutexes, and where each was locked |
| recursive mutex / spinlock, unlock by a non-owner | Both lock sites |
| sleeping with a spinlock held, or in an interrupt | The lock and where it was taken |
| a spinlock that never becomes free | The owner CPU and the PC that took it |

Every panic also prints the current thread, the spinlocks held, the other CPUs (thread, locks, backtrace), and the **last 16 trace events**.

## Multiprocessor (SMP)

`make run` boots 4 harts (`CPUS=n` to change; `APPEND="smp=1"` to use only one). OpenSBI picks a random boot hart; it becomes cpu0, and the boot log shows the hart for every CPU. Device interrupts all go to cpu0; every CPU has its own timer tick. There is one run queue shared by all CPUs (oldest runnable thread first). A busy CPU that makes a thread runnable wakes an idle CPU with an IPI.

**Seeing what each CPU does:**
- `cpus` in the monitor (or `cat /proc/cpus`): state, current thread, idle %, context switches, migrations, ticks, IPIs, held spinlocks, and for a stopped CPU, where it stopped.
- When the monitor is entered, all other CPUs park. `cpu <n>` points `regs`, `bt` and `$reg` at the place CPU n stopped (its interrupted registers, or the spin loop it was in). The prompt shows `mon(cpu2)>`. A CPU that did not stop has had interrupts off for 200 ms and isn't spinning on a lock; that itself is a clue.
- `ps` has a CPU column: `*2` is running on cpu2 right now, `2` last ran there. MIGR counts moves between CPUs.
- `sched` shows per-CPU switches and migrations plus the shared queues. `irq` and `/proc/interrupts` show per-CPU counts, including IPIs.
- In traces every event carries its CPU. Perfetto shows one track per CPU, and `ipi_send`, `ipi_recv` and `cpu_stop` events show the IPIs.

**Caught automatically on SMP:**

| Bug | Report |
|---|---|
| lock-order inversion (A→B on one path, B→A on another, or a longer cycle) | `LOCKDEP: possible deadlock`: the acquisition being made, the lock held, and for every link of the reverse chain the thread, CPU and both lock sites that established it. Reported when the second order first occurs, even if it never deadlocks in this run. |
| `mutex_lock` (may sleep) with a spinlock held | `LOCKDEP: mutex_lock at X while holding spinlock 'S'`, even when the mutex was free |
| a CPU with interrupts off for 10 s | `watchdog: hard lockup on cpu2 ... running thread N (holding 'x')`; the panic report then shows where that CPU is if it is spinning |
| a thread waiting for a mutex for 30 s | `HUNG THREAD`: waiter, mutex, owner, both backtraces (and a note when the wait finally ends) |
| a CPU not answering a TLB shootdown | panic naming the CPU and its thread |
| two CPUs panicking at once | the second one parks; the first report shows it |

`lockdep` in the monitor (or `/proc/lockdep`, or `eu-lockdep` in GDB) lists every lock class and the order dependencies seen so far, so you can check that a new lock fits the existing order.

**Kernel rules this relies on** (see `percpu.h`): threads migrate, so code that runs with interrupts on must not keep a `struct cpu *` (or anything read through it) across a point where it could be preempted. Use `current`, `this_cpu_id()` and `in_interrupt()`, which are single loads. The scheduler itself re-reads the CPU after every switch. Interrupts are off before it looks at the CPU at all.

## Networking

`make run` attaches a virtio NIC on QEMU's user-mode network. The guest is 10.0.2.15/24, the Mac is reachable as 10.0.2.2 (`gateway` or `host` in the tools), and QEMU forwards these ports from 127.0.0.1 on the Mac into the guest:

| Mac | Guest | Start in the guest |
|---|---|---|
| `localhost:8080` (`HTTP_PORT`) | tcp 80 | `httpd &` (serves `/www` and any `/proc` file: `curl localhost:8080/proc/net/tcp`) |
| `localhost:2323` (`SHELL_PORT`) | tcp 23 | `rshd &` (a shell over the network: `nc localhost 2323`) |
| `localhost:5555` (`UDP_PORT`) | udp 7 | `echod &` (TCP and UDP echo) |

If a port is taken on the Mac, QEMU refuses to start: use `make run HTTP_PORT=8081`, or `HOSTFWD=` for no forwarding. Programs: `ping`, `nc` (client, `-l` server, `-u` UDP), `ifconfig` (show/set), `netstat` (`-t -u -s -i -a`), `httpd`, `rshd`, `echod`, and `crc32` to check transfers.

The stack lives in `kernel/net/`: Ethernet/ARP, IPv4/ICMP, UDP, TCP, sockets. The `netd` thread handles received packets and protocol timers. Everything is under one mutex, `net_lock`.

**Seeing what happens:**
- **Packets:** every run writes `build/net.pcap` (QEMU's view of the NIC); open it in Wireshark. `log=net:debug` (or `log net debug` in the monitor) logs each packet in the kernel, tcpdump style, on `lo` and `eth0`: `eth0 rx 10.0.2.2:80 > 10.0.2.15:49152 TCP [S.] seq … ack … win … len 0`. `log=net:trace` adds TCP state changes and dropped out-of-window segments (see IMPROVEMENTS.md: the levels used by `tcp.c` are one off). The `net` trace group records `net_rx`/`net_tx` per packet plus `tcp_state` and `tcp_rexmit` events.
- **Connections:** `/proc/net/tcp` (or `netstat`, or monitor `netstat`) lists every TCP connection: state, queued bytes each way, peer window, congestion window, RTO, smoothed RTT, retransmissions, and whether a socket still owns it. Orphans are closing on their own; listener children not yet accepted show `backlog`. `/proc/net/udp` lists datagram and raw sockets.
- **Counters:** `/proc/net/stats` (`netstat -s`, monitor `net`) has every drop reason (bad checksum, fragments, no listener, queue full, unresolved ARP, injected loss), TCP retransmissions split into fast and timeout, duplicate ACKs, and out-of-order segments. It also shows live packet buffers, so you can spot leaks.
- **Interfaces and ARP:** `/proc/net/dev`, `/proc/net/arp`, `ifconfig`.
- **Calls:** `strace` decodes socket addresses (`connect(3, {10.0.2.2:80}, 16)`); a blocked process shows `socket` or `poll` in `ps`.
- **GDB:** `eu-net` lists interfaces, TCP control blocks and sockets from memory, and who holds `net_lock`.

**TCP on the Mac:** `tcp.c` doesn't depend on the rest of the kernel. `build/host/test_tcp` (part of `make test-host`) runs it between two endpoints over a simulated wire with loss, duplication, jitter and latency, with ASan. It checks every byte, the final states, and that nothing leaks. `build/host/test_tcp -vv LOSS SEED DUP STALL` replays one transfer with every state change, retransmission and dropped segment logged. That is how the window-edge, `snd_max`, ACK ping-pong, duplicate-ACK window and TIME_WAIT ACK storm bugs were found and pinned down.

## Tracing (flight recorder)

Events (context switches, wakeups, thread create/exit, interrupts, mutex contention, optional page/kmalloc events, and `trace_mark("msg", a, b)` markers you add) go into an 8192-entry ring buffer per CPU. Recording is always on and costs a few instructions per event.

- Monitor: `trace` (status), `trace show [n]`, `trace groups sched,irq,mm`, `trace clear`, `trace dump [host-path]`.
- `trace dump` writes `build/trace.bin` **on the Mac** via QEMU semihosting. Then:
  ```bash
  make trace-json
  ```
  Open `build/trace.json` in https://ui.perfetto.dev. It has a per-CPU track of which thread ran, a per-thread track of running slices, IRQ slices, and instant events for wakeups, marks and contention.
- `make test` always leaves `build/ktest-trace.json` from the test run (kernel + user tests). System calls show as slices on each thread's track. Process exec/exit and user page faults are events too (groups `syscall`, `proc`).
- Text view:
  ```bash
  python3 tools/trace/eutrace.py text build/trace.bin
  ```
- From GDB, even on a hung kernel: `eu-trace [n]`, or `eu-trace-dump` to write `.bin` + `.json`.

## Profiling

A sampling profiler covers every CPU. While it runs, each CPU's timer fires at the profiling rate (500 Hz by default, up to 5000; the scheduler still ticks at 100 Hz). Every interrupt records the CPU, the thread and process, whether the CPU was idle, and the **whole call stack**: the kernel frames, followed by the user frames of the process underneath. A sample taken in a system call shows both halves, e.g. `stress;main;__syscall;[kernel];syscall_dispatch;do_exit;proc_exit;mm_destroy;...`. Stacks are read with fault-free accessors only, so sampling is safe anywhere. Buffers are 2 MiB per CPU (about a minute at 500 Hz); when one fills up, its CPU counts drops.

**In the guest:**
```
profile run stress -j 4 -t 3      # profile one command, then print the report
profile start [hz] / profile stop / profile report / profile clear
profile dump [host-path]          # write the samples to the Mac (default build/profile.bin)
```
The report (also `/proc/profile`, monitor `profile`) has the idle, user and kernel split, time per process, and the top kernel functions by self and total time.

**On the Mac**, after `profile dump`:
```bash
make flamegraph
```
This prints a report with user functions too, then writes `build/profile.svg` (an interactive flamegraph: click to zoom, hover for counts) and `build/profile.folded` (Brendan Gregg's folded format, for other tools). Kernel frames are symbolized from `build/kernel.elf`, user frames from the unstripped programs in `build/user/bin`. For finer cuts, use `python3 tools/profile/euprof.py report|svg|folded build/profile.bin` with `--cpu N`, `--pid N`, `--kernel-only` or `--no-idle`.

**From GDB**, even on a hung kernel: `eu-profile-dump [path]` writes the same file straight from memory. Boot with `profile`, wait for the hang, attach, dump, and the flamegraph shows what every CPU was doing.

**Caveats:**
- **Interrupts off:** there is no NMI, so a sample can't land while interrupts are disabled. That time is charged to the instruction where they come back on, which makes `spin_unlock_irqrestore` look expensive.
- **Debug features are visible:** page poisoning shows up as `memset`; boot with `nopoison` to measure without it.

Example: the first profile of the SMP stress test showed `unmap_range` at 23% of busy time. Process exit looked up every page of the 8 MiB stack from the root of the page table. Making the walk skip empty tables took it off the chart.

## Deterministic record/replay and reverse debugging

QEMU can record every nondeterministic input to the machine and replay the run exactly: keystrokes, disk completion order, network packets, and timer interrupts (`-icount` ties time to instructions).

```bash
command make record        # like make run (one CPU), recorded into build/rr/
command make replay        # the same run again, instruction for instruction
```
The replay repeats the console output byte for byte, timestamps included. Your keystrokes are ignored; the recorded ones are used. So you can record a failure once and replay it as often as you like. The replay must use the recording's boot options (more logging would change the execution), but you can attach a debugger, which doesn't:

```bash
command make replay-debug  # halted at the start
make replay-gdb            # in another terminal
```
In GDB, besides `break` and `continue`, you can go **backwards**: `reverse-continue` runs back to the previous breakpoint, and `reverse-step`, `reverse-next` and `reverse-stepi` step back. Typical use: the replay reaches a corrupted value; set a watchpoint on it and `reverse-continue` to land on the write that corrupted it.

Limits:
- **One CPU:** QEMU's record/replay requires `-smp 1`, so SMP-only races can't be replayed (use `make test CPUS=1` to see whether a bug needs SMP).
- **Self-contained:** the recording keeps copies of the kernel, a snapshot of the disk and the boot options in `build/rr/`, so you can rebuild meanwhile.
- **Slower:** under `-icount`, idle time passes in real time, so disk-heavy sessions run slower than usual.
- **No semihosting:** there is no trace or profile dump to the Mac in recordings.

`make replay-check` (part of `make ci`) records a boot running the kernel tests, replays it, and requires identical output.

## Memory debugging

**Layout.** User space (lower half, unmapped for now). Below that is a direct map of all RAM at `0xffffffc000000000 + PA`; the kernel image lives inside it with W^X permissions. NULL and firmware memory are unmapped. Physical addresses go through `p2v` before `mem` can read them.

**What gets caught automatically:**

| Bug | How | Report says |
|---|---|---|
| heap overflow / underflow | 8-byte redzones on every object, plus the slack after the requested size | object, cache, requested size, alloc site, exact bad bytes, hexdump |
| use-after-free write | freed objects/pages filled with `0x6b` and checked on reuse | object/page, when freed, where allocated |
| use-after-free read of a pointer | fault address `0x6b6b…` | "likely cause: use after free" |
| uninitialized kmalloc memory | new objects filled with `0x5a` | fault address `0x5a5a…` → "uninitialized" |
| double free / bad free | object/page state tracking | both free sites; interior pointers; reserved memory |
| write to code/rodata | MMU (W^X) | "write to read-only kernel text/rodata" + page-table walk |
| NULL / wild pointers | unmapped | hint + walk of the page table |

By default a corruption report panics into the monitor at the first error. `heapcheck` sweeps all free pages and slab objects on demand.

**Monitor:** `meminfo` (RAM, reserved regions, free blocks by order), `pt` (all mappings) / `pt <va>` (walk), `v2p`, `p2v`, `page <pa|va>` (flags and alloc/free sites of a frame), `slab` (per-cache stats), `slab leaks [prefix]` (live allocations grouped by call site, the leak finder), `heapcheck`.

## GDB

```bash
make debug
```
In a second terminal:
```bash
make gdb
```

GDB stops automatically on `panic` and `panic_trap`. Thanks to CFI in the trap entry, `bt` unwinds through traps (the trap shows as `<signal handler called>`).

QEMU shows each hart as a GDB thread (`info threads`; GDB thread = hart id + 1). `eu-cpus` gives the mapping from kernel CPU numbers to GDB threads. `thread <n>` followed by `bt` shows what that CPU is doing. `eu-bt <tid>` of a running thread tells you which GDB thread to switch to.

| Command | Purpose |
|---|---|
| `eu-help` | list commands |
| `eu-dmesg [N]` | kernel log ring (works even if the kernel is wedged) |
| `eu-log [subsys level]` | show/set log thresholds live |
| `eu-tf [expr]` | pretty-print a `struct trap_frame` (default `tf`) |
| `eu-platform` | hardware discovered from the device tree |
| `eu-vm [va]` | page-table dump, or walk one address |
| `eu-meminfo` | page allocator state and reserved regions |
| `eu-page <addr>` | struct page (flags, alloc/free sites) for a pa or va |
| `eu-slab` | slab cache statistics |
| `eu-ps` | kernel threads, the CPU each runs (or ran) on, and what they wait on |
| `eu-cpus` | every CPU: current thread, held spinlocks, pending IPIs, and its GDB thread number |
| `eu-lockdep` | lock classes and the order dependencies lockdep has recorded |
| `eu-net` | interfaces, TCP control blocks, sockets, and the holder of `net_lock` |
| `eu-profile-dump [path]` | the profiler's samples, from memory (then `make flamegraph`) |
| `eu-bt <tid>` | backtrace of any thread (sleeping ones from their saved context) |
| `eu-trace [n]` | decode recent trace events from memory |
| `eu-trace-dump [path]` | save the trace as `.bin` + Perfetto `.json` |
| `eu-procs` | user processes (including zombies) |

## Tests

- `make test`: the whole scenario (`tools/run_tests.py`), on a scratch copy of the disk image:
  1. kernel tests and the user suite
  2. fsck
  3. a reboot to check persistence
  4. a power failure after the commit point, fsck, then recovery (the data must be present), fsck
  5. a power failure before the commit point, fsck, then recovery (the change must be absent), fsck
  6. an SMP stress run (`/bin/stress`: 8 processes doing fork/exec pipelines, file I/O, copy-on-write forks and loopback TCP connections on all CPUs for 8 s, every result checked), then fsck
  7. an interactive session typed into the shell on the console: pipelines and redirection, `^C`, `^Z` and job control, quoting and variables, `su guest` and a permission denial, command-not-found, and a segfault message
  8. the native toolchain and the editors: `tcc` compiles and runs a program (to a file and with `-run`), `make` rebuilds `cat` from `/usr/src`, and `vi` and `emacs` each edit and save a file
  9. networking through QEMU's user network: ping, HTTP requests from the Mac (including a live `/proc/net/tcp`), UDP echo, 1 MiB each way between the Mac and the guest with CRC checks; then 5% induced packet loss: a transfer over loopback (our TCP at both ends) and one from the Mac, checksummed. Each stage leaves a `.pcap` in `build/test-logs/`.

  The user tests include socket tests over loopback: a 200 KB TCP echo driven by `poll`, error cases (refused, address in use, reset, EPIPE, ENOTSOCK), non-blocking connect and accept, UDP, and `poll`/`select` with pipes. The stress run also makes a TCP connection over loopback in every iteration.

  Every boot uses `lockdep=panic` and the lockup watchdog. The kernel tests include SMP tests: bound threads on every CPU, exclusion under contention, IPIs, a TLB shootdown that must invalidate every CPU's TLB, stop-the-world, and lockdep and hung-thread self-tests.

  Each stage's console output is in `build/test-logs/`; a failing stage prints its last lines. Hung boots are killed after `TEST_TIMEOUT` seconds (60).
- `make stress [STRESS_SECS=120] [CPUS=4]`: the stress run as a soak test for minutes, with `lockdep=panic`, then fsck. It prints per-CPU statistics and lockdep's totals at the end. Run `stress -j N -t SECS -v` from the shell for interactive experiments.
- `make ci`: everything, headless, starting from a clean build:
  - the build (with `-Werror`);
  - the host tests;
  - `make test` on 4 CPUs and on 1;
  - a 60 s SMP soak (`STRESS_SECS`);
  - `make replay-check`.

  Each step's output is in `build/ci/`, and a summary prints at the end. `make ci CI_FLAGS=--quick` skips the 1-CPU run and the soak.
- `make test-host`: host-native tests with ASan/UBSan. It compares `lib/printf.c` against libc, stress-tests the buddy allocator core, runs TCP over a simulated lossy link (see Networking), and checks that fsck detects each kind of injected corruption.
- Add a kernel test with `KTEST(name) { KT_EXPECT(...); }` in any file under a kernel directory listed in `KDIRS` (normally `kernel/tests/`). The linker collects tests automatically.

## Writing debuggable kernel code

- Start each `.c` file that logs with `#define LOG_SUBSYS SUBSYS_xxx`, then use `pr_err/warn/info/debug/trace`.
- Use `assert()` / `BUG_ON()` liberally. They produce a symbolized backtrace and drop into the monitor.
- When debugging code must read memory that might be invalid, use `safe_read_*` / `safe_write_*` (`safe.h`). These return -1 on a fault instead of crashing.
- Frame pointers are mandatory (`-fno-omit-frame-pointer`); don't remove the flag.
- Allocate with `kmalloc`/`kzalloc`/`alloc_pages` directly, not through thin wrappers. Otherwise `slab leaks` attributes everything to the wrapper. If you need a wrapper, pass `__builtin_return_address(0)` to the `*_site` variants.
- Map device registers with `ioremap()`. Unregistered MMIO is unmapped and faults with a hint saying so.

## QEMU-level tracing

`make qemu-log` writes every trap and guest error, as seen by QEMU, to `build/qemu.log`.

**Sampling registers from outside.** When the system is slow or stuck and the kernel's own tools can't say why (a CPU looping with interrupts on, a lost wakeup), ask QEMU: under `make run`, `Ctrl-^ C` switches the console to the QEMU monitor (`Ctrl-^ C` again switches back), and `info registers -a` prints every CPU's registers. Repeating it a few times is a poor man's sampling profiler: look up the `pc`s and `ra`s with `riscv64-elf-addr2line -f -e build/kernel.elf` (or the kernel monitor's `sym` command; for a tcc-built kernel, use its own `kernel.elf`). This is how the tcc epilogue bug was found: CPUs kept showing a `pc` inside one function's epilogue, returning to itself (see [13](subsystems/13-toolchain.md#the-patches-to-tinycc)).

**When the two self-hosting stages differ.** `make selfhost` lists the files that differ between `build/selfhost/stage1` and `stage2`. The object files are kept per source file (`obj/`, `kernel/`), so the first differing `.o` names the source; disassemble both (`riscv64-elf-objdump -d`) and `diff` them. A difference means stage 1's tcc (built by tcc) compiles something differently from `/bin/tcc` (built by GCC): a tcc bug that depends on how tcc itself was compiled. That is how the `fence` operand bug (pointer arithmetic between two string literals that GCC merges) was found. `make tcc-kernel` builds the same kernel on the Mac for a closer look.

## Cookbook: how to debug ...

**A panic.** The report names the cause and decodes the fault: the address, a likely reason ("NULL pointer", "use after free: 0x6b poison", "stack overflow", "write to read-only kernel text"), and the page-table walk. It also shows the registers, a symbolized backtrace, the current thread, the spinlocks held, every other CPU's thread and backtrace, and the last 16 trace events. You land in the monitor: `regs`, `bt`, `ps`, `dmesg`, `trace show 100`, `cpu <n>` + `bt`. If it reproduces on one CPU, `make record` it, then `make replay-debug`, `break panic`, and `reverse-step` to watch it happen.

**A hang.** Press Ctrl-]. Then:
1. `cpus` shows what each CPU is doing. A CPU that didn't stop has had interrupts off for 200 ms.
2. `ps` shows every thread's state and what it waits on, e.g. `mutex 'inode' held by 12(sh)`, `wq 'pipe-read'`, `socket`, `poll`.
3. `locks` lists held spinlocks and mutexes; `bt <tid>` shows a thread's stack.

A deadlock between mutexes is reported automatically when the cycle forms; a lost wakeup shows a thread sleeping on a wait queue whose condition is already true. If the console doesn't respond, `make gdb` and run `eu-ps`, `eu-cpus`, `eu-dmesg` and `eu-trace`. The watchdogs report CPUs stuck with interrupts off (10 s) and mutex waits over 30 s.

**Slowness.** Run `profile run <command>` for a quick report, or `profile dump` and `make flamegraph` for the whole picture with user code. Then:
- **A high idle % on a busy system** means threads are waiting: look at `ps` and the `sched` trace (Perfetto).
- **Time in `spin_unlock_irqrestore` or `intr_on`** is really time spent with interrupts off just before.
- **Network throughput:** check `netstat -s` (retransmissions, duplicate ACKs, out-of-order segments) and `/proc/net/tcp` (window, cwnd, RTO).

**Memory corruption.** The first detection usually names the victim: a redzone report with the object, its allocation site and a hexdump; a `0x6b` page or object (written after free, with its free site); or a `0x5a` pointer (uninitialized). `heapcheck` sweeps everything now; `page <addr>` gives a frame's history; `slab leaks` groups live allocations by site. To find the writer, `make record` the run, replay it under GDB, `watch` the corrupted address and `reverse-continue`.

**A lock problem.** lockdep (`lockdep=panic` in tests) reports any lock-order inversion with the sites that created each order, before it deadlocks. `lockdep` in the monitor lists the order graph. "sleeping with a spinlock held" names the lock and where it was taken.

**A race that needs SMP.** Run `make stress` for minutes; `make test CPUS=1` to see whether it needs more than one CPU; the `cpus`/`cpu <n>` views at the moment it fails; and invariants. The scheduler panics if a CPU's current thread and the code it runs ever disagree (that is how three thread-migration bugs were caught). Trace events carry the CPU, so Perfetto shows the interleaving.

**A user program crashes.** The kernel prints the signal, the faulting address with a diagnosis (NULL, stack overflow, a permission violation), a symbolized user backtrace and the memory map. Use `strace <cmd>` for its system calls, `/proc/<pid>/ustack` for a live process, `dbg_break()` in the program to stop in the monitor, and `ubt <pid>` there.

**A filesystem problem.** Run `make fsck` (or `build/tools/mkfs --check IMAGE`) for the image. In the guest, use `mounts`, `bcache` and `icache`. `fs.crash=after-log|after-commit|mid-install` simulates a power failure at that point of the next commit, to test recovery.

**A network problem.** Use `ping gateway` and `ping localhost`, then `ifconfig` and `netstat -a` (ARP) / `-t` / `-s`. Turn on the per-packet log (`log=net:debug`, or `log net debug` in the monitor) and look at `build/net.pcap` in Wireshark. `net.drop=10` makes losses reproducible. For TCP logic, write a case in `tests/host/test_tcp.c` and debug it on the Mac with `-vv`.
