# Eunuchs — a Unix-like OS in C, on RISC-V, run under QEMU on macOS

## Context

Goal: build a Unix-like OS from scratch in C at `/Users/butch/src/Eunuchs` (currently empty), running on this Apple Silicon Mac via QEMU. Decisions so far:
- **Target**: RISC-V 64 (RV64GC), QEMU `virt` board, OpenSBI firmware (bundled with QEMU) handles M-mode; our kernel runs in S-mode.
- **Scope**: classic Unix core **plus SMP and networking** (virtio-net, small TCP/IP stack, sockets).
- **Top priority**: debugging and observability — it should be easy to see what the kernel internals are doing at any time.

Design principle: build the debugging infrastructure *first* and grow it with every subsystem, so every later milestone comes with its own inspection tools (`/proc` entries, monitor commands, trace events, GDB helpers).

## Toolchain & layout

Host tools (Homebrew): `qemu`, `riscv64-elf-gcc`, `riscv64-elf-gdb`, `e2fsprogs`-style tools not needed (we write our own `mkfs`), `python3` (for host tools/GDB scripts).

```
Eunuchs/
  Makefile, config.mk          # top-level build; `make run`, `make debug`, `make test`
  kernel/
    arch/riscv/               # boot.S, trap.S, sbi.c, csr.h, mmu.c, plic.c, timer.c, smp.c
    core/                     # main.c, panic.c, printk.c, sched.c, proc.c, fork/exec/exit, signal.c, syscall.c
    mm/                       # pmm.c (page allocator), vmm.c (page tables), kmalloc.c (slab), vm_area.c
    fs/                       # vfs.c, path.c, file.c, pipe.c, devfs.c, procfs.c, ext-lite fs (efs/), bcache.c
    drivers/                  # uart16550.c, virtio.c, virtio_blk.c, virtio_net.c, virtio_console.c, rtc
    net/                      # netbuf.c, ether.c, arp.c, ipv4.c, icmp.c, udp.c, tcp.c, socket.c
    debug/                    # ksyms.c, backtrace.c, trace.c, monitor.c, lockdep.c, kasan-lite, ktest.c
    include/
  lib/                        # shared freestanding libc bits (string, printf core) for kernel + user
  user/
    libc/                     # minimal libc: syscalls, stdio, malloc, string, crt0
    bin/                      # init, sh, ls, cat, echo, mkdir, rm, ps, kill, wc, grep, ping, nc, dmesg, strace, top...
  tools/
    mkfs/                     # host-native C tool building disk image (compiled with Apple clang)
    gdb/                      # eunuchs.py — GDB Python extension
    trace/                    # trace decoder -> Perfetto/Chrome JSON; profiler -> flamegraph
    gen_ksyms.py              # symbol table generation
  tests/host/                 # data-structure unit tests compiled natively on macOS
  tests/integration/          # expect-style QEMU boot tests (Python + pexpect)
```

Build flags: `-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -nostdlib -fno-omit-frame-pointer -g -O1 -Wall -Werror`. Frame pointers are mandatory (backtraces). Two-pass link to embed the symbol table.

## Debugging architecture (cross-cutting, introduced in Phase 1 and extended in every phase)

1. **GDB via QEMU gdbstub** — `make debug` launches QEMU with `-s -S` and `riscv64-elf-gdb` with `tools/gdb/eunuchs.py` auto-loaded. Python commands: `eu-ps` (process table), `eu-bt <pid>` (backtrace of a sleeping task from its saved context), `eu-vm <pid>` (page-table walk / VMA list), `eu-runq`, `eu-locks` (held locks per CPU), `eu-dmesg` (dump log ring), `eu-trace` (dump trace ring), `eu-slab`, `eu-files <pid>`, `eu-sock`. Pretty-printers for core structs.
2. **printk + log ring** — log levels and per-subsystem tags (`[sched]`, `[vm]`, `[net]`…), CPU id, timestamp; stored in a ring buffer readable via `dmesg`/`/proc/kmsg`; per-subsystem verbosity runtime-tunable (`/proc/sys/debug/log/<subsys>` and a boot cmdline `log=vm:debug,net:info`).
3. **Symbolic panics & backtraces** — embedded `ksyms` table; `panic()`, `assert()`, page faults in kernel, and `BUG_ON` print registers, `scause/stval`, symbolized backtrace, current process, held locks, last N trace events — then drop into the monitor.
4. **Static tracepoints** — `TRACE(sched_switch, prev, next)` macros writing compact binary records to per-CPU lock-free ring buffers (cheap, always compiled in, runtime-enabled per event). Streamed out over a **dedicated virtio-console/second UART** to a host socket; `tools/trace/` decodes into **Perfetto/Chrome trace JSON** for a visual timeline of context switches, syscalls, IRQs, page faults, block I/O, packets.
5. **In-kernel debug monitor (kdb-style)** — entered on panic, on `Ctrl-]` break on the console, or via `sysrq`. Commands: `ps`, `bt [pid]`, `regs`, `mem <addr> [len]`, `pt <pid> <va>` (walk page table), `vma <pid>`, `runq`, `locks`, `slab`, `trace`, `dmesg`, `irq`, `files <pid>`, `bcache`, `netstat`, `cpu <n>`, `continue`, `reboot`.
6. **procfs** — `/proc/<pid>/{status,maps,fd,stack,syscall}`, `/proc/{meminfo,slabinfo,interrupts,sched,locks,net/*,trace_events}`. Userspace `ps`, `top`, `free`, `netstat` read these.
7. **strace** — per-process syscall tracing flag; kernel logs syscall entry/exit with decoded args; userspace `strace <cmd>`.
8. **Memory debugging** — poison freed pages/objects, slab red-zones + allocation-site tracking (caller PC), leak report (`kmemleak`-lite listing live allocations by site), guard pages under kernel stacks, stack-depth watermarking.
9. **Lock debugging** — spinlocks record owner CPU/PC; lockdep-lite detects lock-order inversions and recursive acquisition; soft-lockup watchdog per CPU (timer notices a CPU not scheduling for N ms → dumps its backtrace via IPI).
10. **Profiler** — timer-sampled PCs + call stacks into a buffer → `tools/trace/` produces flamegraphs.
11. **Determinism & QEMU aids** — `make replay-record` / `make replay` using QEMU `-icount shift=auto,rr=record/replay` so a bug can be re-run identically under GDB with reverse stepping; `make qemu-log` enables `-d int,guest_errors,unimp`.
12. **Tests** — `ktest` in-kernel unit test framework (`make test` boots, runs tests, exits QEMU via SBI shutdown with pass/fail code); host-native unit tests for pure algorithms (allocator, path parsing, TCP state machine, FS layout) under `tests/host` built with Apple clang + ASan/UBSan; integration tests drive the shell over serial with pexpect.

## Step-wise implementation plan

Each phase ends with a runnable, testable system. Phases are ordered so debugging tools exist before the code that needs them.

### Phase 0 — Toolchain & skeleton
- `brew install qemu riscv64-elf-gcc riscv64-elf-gdb`; verify `qemu-system-riscv64 -machine virt` boots OpenSBI.
- Makefile with `run`, `debug`, `gdb`, `test`, `clean`; `git init`; `.gdbinit` loading `tools/gdb/eunuchs.py`.
- **Verify**: `make run` shows OpenSBI banner then jumps to our (empty) kernel at `0x80200000`.

### Phase 1 — Boot, console, and the debug foundation
- `boot.S`: set up stack, clear BSS, jump to `kmain(hartid, dtb)`. Park secondary harts.
- SBI wrappers (console putchar, timer, shutdown, IPI, HSM).
- Minimal DTB parser (memory size, UART, PLIC, virtio MMIO addresses, CLINT timebase).
- UART16550 driver (polling), `kprintf` (shared core in `lib/`), log ring + levels + subsystem tags.
- Trap entry (`trap.S`) saving full register frame; trap dispatcher with decoded `scause`.
- `ksyms` two-pass link, frame-pointer backtrace, `panic()`/`assert()` with symbolized output.
- Debug monitor v1 (`regs`, `mem`, `bt`, `dmesg`, `reboot`) over polled UART.
- GDB script v1 (`eu-dmesg`), `ktest` framework + SBI-shutdown exit codes.
- **Verify**: deliberate `ebreak`/null deref yields symbolized backtrace and monitor prompt; `make test` passes; GDB breakpoint on `kmain` works.

### Phase 2 — Physical & virtual memory
- Page frame allocator (buddy allocator) over DTB-reported RAM; `/proc/meminfo` stub data.
- Sv39 page tables: kernel direct map + kernel image with W^X; enable `satp`.
- Slab `kmalloc/kfree` with red-zones, poisoning, allocation-site tracking, leak report.
- Kernel page-fault handler with clear diagnostics (fault VA, access type, PTE walk dump).
- Monitor: `pt`, `slab`, `meminfo`; GDB: `eu-vm`, `eu-slab`. Host tests for buddy allocator.
- **Verify**: ktest stress alloc/free; use-after-free test triggers poison detection report.

### Phase 3 — Interrupts, timer, and kernel threads (single CPU)
- PLIC driver, interrupt-driven UART RX/TX, SBI timer ticks, `/proc/interrupts`.
- Spinlocks (with owner tracking), interrupt disable nesting.
- Kernel threads, context switch (`swtch.S`), round-robin scheduler, sleep/wakeup (wait queues), mutexes, semaphores.
- Tracepoint infrastructure + trace ring + `sched_switch`, `irq_entry/exit` events; host decoder → Perfetto JSON. Second UART/virtio-console for trace stream.
- Monitor: `ps`, `runq`, `irq`, `trace`; GDB: `eu-ps`, `eu-runq`, `eu-bt <tid>`.
- **Verify**: two kernel threads ping-pong via wait queue; open the exported trace in ui.perfetto.dev and see the switches.

### Phase 4 — User mode, processes, and syscalls
- Process struct (pid, address space, VMAs, fd table, cwd, creds, parent/children), per-process page tables, trampoline for user↔kernel transitions.
- Syscall ABI (`ecall`, a7 = number) with table; `copyin/copyout` with fault-safe user access.
- Embedded initial user binary (`initcode`) before a filesystem exists; ELF loader.
- `fork` (copy-on-write), `exec`, `exit`, `wait`, `getpid`, `sbrk/brk`, `mmap` (anonymous), `sleep`/`nanosleep`.
- Demand paging + COW fault handling; `/proc/<pid>/maps`.
- strace flag + syscall trace events; `/proc/<pid>/syscall`, `/proc/<pid>/stack`.
- Userspace minimal libc (crt0, syscall stubs, printf, malloc).
- **Verify**: user init forks children that print and exit; strace output shows decoded syscalls; COW refcount ktests.

### Phase 5 — Block I/O and filesystem
- virtio-mmio transport + virtio-blk driver (interrupt-driven), buffer cache (LRU, refcounted), block I/O trace events.
- VFS layer: inodes, dentries/path lookup, file objects, mount table; `open/close/read/write/lseek/stat/fstat/dup/dup2/mkdir/rmdir/unlink/link/rename/chdir/getcwd/getdents`.
- On-disk FS "efs": superblock, block & inode bitmaps, inodes with direct/indirect blocks, directories; **write-ahead log** for crash consistency.
- Host `tools/mkfs` (Apple clang) builds `fs.img` with user binaries; host-native tests for FS layout code.
- devfs (`/dev/console`, `/dev/null`, `/dev/zero`, `/dev/disk0`) and full procfs.
- Monitor: `bcache`, `files <pid>`, `inode <n>`; GDB: `eu-files`.
- **Verify**: boot `init` from disk; write file, reboot, read it back; fsck-style consistency check in `mkfs --check`.

### Phase 6 — Unix userland & IPC
- Pipes, `exec` with argv/envp, file descriptors inheritance, `O_CLOEXEC`.
- Signals: `kill`, `sigaction`, `sigprocmask`, `sigreturn`, default actions, SIGCHLD, SIGSEGV on bad user faults, SIGINT/SIGTSTP from TTY.
- TTY layer: line discipline (canonical/raw), echo, `^C`, `^Z`, `^D`, `ioctl` (termios subset), process groups, sessions, controlling terminal, job control.
- Users/permissions: uid/gid, mode bits, `chmod/chown`, `setuid`.
- Time: RTC (goldfish), `time/gettimeofday/clock_gettime`.
- Shell `sh` (pipelines, redirection, background jobs, `fg/bg`, env vars) and coreutils: `ls cat echo cp mv rm mkdir rmdir ln wc grep head tail ps kill sleep env pwd chmod free dmesg strace top`.
- **Verify**: integration test runs `ls / | grep bin > /tmp/x; cat /tmp/x`, `sleep 5 &`, `^C` interrupts `cat`.

### Phase 7 — SMP  ✅ done
_As built: one shared run queue under `sched_lock` (with IPI wakeup of idle CPUs and per-thread CPU binding) instead of per-CPU queues with load balancing. It balances by construction and keeps every scheduling decision under one inspectable lock. The watchdog is a hard-lockup detector (no timer tick for 10 s) plus a hung-mutex detector. `make test` runs on 4 CPUs with `lockdep=panic`; `make stress` is the long soak._
- Bring up all harts via SBI HSM; per-CPU data (`tp` register), per-CPU run queues with load balancing, IPIs (reschedule, TLB shootdown, monitor halt).
- Audit locking across subsystems; lockdep-lite enabled in debug builds; soft-lockup watchdog.
- Per-CPU trace buffers already in place — trace timeline now shows per-CPU tracks.
- Monitor `cpu <n>` switches context to another hart (via IPI-captured register frames); GDB uses QEMU's per-thread vCPU view.
- **Verify**: `-smp 4`; parallel build-like stress test (many fork/exec/pipe) for minutes with lockdep clean; Perfetto shows 4 CPU tracks.

### Phase 8 — Networking  ✅ done
_As built: static address configuration (QEMU's 10.0.2.15) instead of DHCP, `ip=` to change it; no IP fragmentation; TCP with out-of-order reassembly, Reno-style congestion control, RFC 6298 timers; loss injection is the `net.drop=` boot option. `rshd` is the telnetd-style remote shell. `tests/host/test_tcp.c` runs the kernel's TCP on the Mac over a simulated lossy link._
- virtio-net driver, `netbuf` (packet buffers), QEMU user-mode networking (`-netdev user,hostfwd=tcp::5555-:23`) plus a `-netdev socket`/pcap option.
- Ethernet, ARP (with cache), IPv4 (no fragmentation initially), ICMP echo, UDP, TCP (3-way handshake, sliding window, retransmission timers, FIN/RST, TIME_WAIT), DHCP client (or static config matching QEMU's 10.0.2.15).
- BSD sockets API: `socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/close/shutdown`, plus `poll`/`select` across files, pipes, TTYs, sockets.
- Debugging: QEMU `-object filter-dump` writes a **pcap** you open in Wireshark on macOS; per-packet trace events; `/proc/net/{arp,tcp,udp,dev}`; `netstat`; monitor `netstat`; GDB `eu-sock`. Host-native unit tests for the TCP state machine and checksum code.
- Userland: `ping`, `nc`, `ifconfig`, `httpd` (tiny), `telnetd`-style remote shell.
- **Verify**: `ping 10.0.2.2`; `curl localhost:8080` from macOS hits guest `httpd`; `nc` transfer of a large file with checksum match under induced packet loss.

### Phase 9 — Hardening & polish  ✅ done
_As built: sampling profiler with kernel+user stacks on every CPU (`profile`, `make flamegraph`, `eu-profile-dump`); deterministic record/replay with reverse debugging (`make record/replay/replay-debug/replay-check`, one CPU, a QEMU limit); boot option registry (`help`, warnings for unknown options); README.md, docs/ARCHITECTURE.md, a debugging cookbook; `make ci`. The profiler's first finding, per-page lookups in `unmap_range` costing 23% of busy time, was fixed._
- Profiler + flamegraph tool; deterministic record/replay make targets documented.
- Kernel cmdline options (log levels, trace events enabled at boot, `init=`).
- `docs/` with an architecture overview and a "how to debug X" guide per subsystem.
- CI script (`make ci`): host tests + `make test` + integration tests, all headless.

## Verification (end-to-end)
- `make test` — boots QEMU headless, runs ktests, exits with status.
- `make test-host` — native macOS unit tests with ASan/UBSan.
- `make itest` — pexpect integration scenarios (shell, FS persistence, signals, SMP stress, networking).
- `make debug` + `eu-*` GDB commands; `make trace` → open in ui.perfetto.dev; `make pcap` → Wireshark.
- Manual: `make run`, log into the shell, exercise coreutils, `cat /proc/...`, enter the monitor with `Ctrl-]`.

## Reference material
- xv6-riscv (MIT) source and book — closest structural reference for Phases 1–6.
- RISC-V Privileged Spec, SBI spec, virtio 1.x spec, QEMU `virt` board docs, RFC 791/792/793/768/826.
