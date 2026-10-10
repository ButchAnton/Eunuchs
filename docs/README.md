# Eunuchs documentation

Documentation for Eunuchs, from the big picture down to the details of each subsystem. Every subsystem document comes from reading the source and points into it (`file.c: function`), so you can go from an explanation straight to the code.

## Start here

| Document | What it covers |
|---|---|
| [../README.md](../README.md) | What Eunuchs is, quick start, make targets |
| [ARCHITECTURE.md](ARCHITECTURE.md) | High-level architecture: how the parts fit, boot to TCP, in one sitting |
| [DEBUGGING.md](DEBUGGING.md) | User guide to every debugging facility, and a cookbook for common failures |
| [IMPROVEMENTS.md](IMPROVEMENTS.md) | Potential improvements across the system, prioritized |
| [PLAN.md](PLAN.md) | The original phase-by-phase plan, with notes on what was built |

## Subsystems in detail

Each document has the same structure:
1. responsibilities and scope;
2. a source map;
3. concepts and data structures;
4. how it works;
5. concurrency and locking;
6. interfaces;
7. debugging and observability;
8. limitations;
9. potential improvements.

| # | Document | Subsystem | Main sources |
|---|---|---|---|
| 1 | [Boot and platform](subsystems/01-boot-and-platform.md) | Boot sequence, device tree, command line, console, kernel log, panic, symbol table | `arch/riscv/boot.S`, `core/main.c`, `core/fdt.c`, `core/printk.c` |
| 2 | [Memory](subsystems/02-memory.md) | Physical pages, slab, page tables, kernel stacks, user address spaces, copy-on-write, user access | `kernel/mm/` |
| 3 | [Traps, interrupts and system calls](subsystems/03-traps-interrupts-syscalls.md) | Trap entry/exit, interrupts, timer, PLIC, the system call table | `arch/riscv/trapentry.S`, `trap.c`, `proc/syscall.c` |
| 4 | [Threads, scheduling and SMP](subsystems/04-threads-scheduling-smp.md) | Threads, scheduler, locks and wait queues, IPIs, TLB shootdown, lockdep, watchdogs | `core/sched.c`, `core/sync.c`, `arch/riscv/smp.c` |
| 5 | [Processes and signals](subsystems/05-processes-and-signals.md) | fork/exec/exit/wait, signals, credentials, process groups and job control | `kernel/proc/` |
| 6 | [Filesystems](subsystems/06-filesystems.md) | VFS, efs and its journal, buffer cache, pipes, poll, devfs, procfs, mkfs/fsck | `kernel/fs/`, `tools/mkfs/` |
| 7 | [Devices and the terminal](subsystems/07-devices-and-tty.md) | virtio transport, block and network drivers, UART, TTY line discipline, RTC | `kernel/drivers/` |
| 8 | [Networking](subsystems/08-networking.md) | Netbufs, interfaces, ARP, IPv4/ICMP, UDP, TCP, sockets | `kernel/net/` |
| 9 | [Debugging internals](subsystems/09-debugging-internals.md) | How the monitor, trace, profiler, backtraces, ktest and GDB helpers are built and extended | `kernel/debug/`, `tools/` |
| 10 | [User space](subsystems/10-user-space.md) | libc, program startup, the shell, every command | `user/` |
| 11 | [Build and testing](subsystems/11-build-and-testing.md) | Toolchain, Makefile, disk image, QEMU setups, test suites, CI | `Makefile`, `tools/`, `tests/` |

## Reading paths

- **New to the code:** ARCHITECTURE.md, then Boot (1), Traps (3) and Threads (4). Those three set up the conventions every other part relies on, especially `tp`/`current`, interrupt state and lock rules.
- **Changing the kernel safely:** read the "Concurrency and locking" section of every subsystem you touch, and Threads/SMP (4) in full. Run `make ci` before you're done.
- **Adding a system call:** Traps (3) for the table and conventions, User space (10) for the libc side, Build and testing (11) to add a test.
- **Adding a driver:** Devices (7) has a walkthrough; Debugging internals (9) shows how to add a monitor command and trace events for it.
- **Chasing a bug:** DEBUGGING.md (its cookbook first), then the subsystem's "Debugging and observability" section.

## Conventions used in the documents

| Notation | Meaning |
|---|---|
| `file.c: func` | A function in that file (paths relative to the repository root, or to `kernel/` where obvious) |
| *monitor* `cmd` | A command in the kernel debug monitor (Ctrl-] in `make run`) |
| `eu-*` | A GDB command from `tools/gdb/eunuchs.py` (`make debug` + `make gdb`) |
| boot option `x=` | A kernel command-line option (`make run APPEND="x=..."`; `help` lists them all) |
| `/proc/...` | A procfs file in the running guest |
