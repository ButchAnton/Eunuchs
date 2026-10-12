# Eunuchs

A small Unix-like operating system for 64-bit RISC-V, written in C from scratch and run under QEMU on macOS. It has processes, a journaling filesystem, a shell, job control, SMP and a TCP/IP stack. Above all, it makes its own internals easy to see: almost every subsystem comes with its own way to inspect it while it runs.

```
root@eunuchs:/# ping -c 1 gateway
64 bytes from 10.0.2.2: icmp_seq=1 time=1.749 ms
root@eunuchs:/# profile run stress -j 4 -t 3
profile: stopped, 500 Hz, 6000 samples over 3027 ms on 4 CPUs, 0 dropped
  kernel functions (of busy samples):   self   total
    memset                                16.4%    16.4%
    ...
root@eunuchs:/# ^]
=== Eunuchs debug monitor: console break (Ctrl-]) ===
on cpu0; other CPUs stopped ('cpus', 'cpu <n>')
mon> netstat
```

## Quick start

```bash
brew install qemu riscv64-elf-gcc riscv64-elf-gdb
```

The guest is RISC-V, so the host can be Apple Silicon or Intel. On Intel Macs Homebrew often has no `qemu` bottle; if `qemu-system-riscv64` is missing or breaks after a `brew upgrade`, reinstall the local formula: `brew reinstall local/qemu/qemu`.

```bash
command make run
```

`command` bypasses shell wrappers around `make` (such as grc) that buffer the console. Inside, `help` lists the shell's builtins and keys; `Ctrl-]` enters the kernel debug monitor (`help` there too, `c` to continue); `Ctrl-^ X` quits QEMU.

| Target | What it does |
|---|---|
| `make run` | Boot with 4 CPUs, the disk image, a persistent `/home` disk (`home.img`), this directory shared on `/mnt/host`, and a NIC (`CPUS=`, `MEM=`, `APPEND="boot options"`, `HOSTDIR=`) |
| `make debug` / `make gdb` | Boot halted, then attach GDB with the `eu-*` helpers |
| `make test` | The whole test scenario, headless (kernel and user tests, crash recovery, SMP stress, shell, networking) |
| `make test-host` | Native tests on the Mac with ASan/UBSan (allocator, printf, TCP over a lossy link, fsck) |
| `make stress` | SMP soak test (`STRESS_SECS=120`) |
| `make ci` | Everything above from a clean build, with a summary |
| `make record` / `make replay` | Record a session, then replay it instruction for instruction |
| `make replay-debug` + `make replay-gdb` | Replay under GDB with reverse execution |
| `make flamegraph` | Profile report and an interactive SVG flamegraph from `profile dump` |
| `make trace-json` | Event trace for ui.perfetto.dev from `trace dump` |
| `make selfhost` | Eunuchs rebuilds its own user space with tcc, twice; the stages must be identical, and the test suite then runs on the self-built image |
| `make fsck` | Check the disk image |
| `make clean-home` | Delete `home.img`, so the next `make run` starts with an empty `/home` |

## What's inside

- **Hardware:** QEMU `virt` with RV64GC, OpenSBI firmware, the kernel in S-mode; up to 8 harts, virtio block (up to four disks) and network devices. User programs may use floating point (switched lazily by the kernel, which itself uses none).
- **Memory:** a buddy page allocator and a slab allocator, both with poisoning, redzones, allocation sites and leak reports. Sv39 page tables with a W^X kernel, guard pages under kernel stacks, demand paging and copy-on-write `fork`.
- **Processes:** ELF executables, `fork`/`exec`/`wait`, signals with handlers, process groups and sessions, a TTY line discipline with job control, users and permissions, and Linux system call numbers.
- **Files:** a VFS, *efs* (an on-disk filesystem with a write-ahead log that survives power failures), devfs, and procfs with a lot of kernel state. The root filesystem is rebuilt with the programs; `/home` lives on a second disk that survives rebuilds; and the Mac's project directory appears on `/mnt/host` through virtio-9p (read-write: it *is* your checkout; `make run HOSTDIR=` turns the share off, `HOSTDIR=dir` shares another directory).
- **SMP:** every hart runs threads from a shared run queue; IPIs handle reschedules, TLB shootdowns and stopping the world.
- **Network:** Ethernet, ARP, IPv4, ICMP, UDP and TCP (with retransmission, fast retransmit, reassembly and congestion control), BSD sockets, and `poll`/`select`.
- **User land:** `esh`, a zsh-like shell: functions, arrays, `[[ ]]`, zsh's expansions and job control, with a line editor that does syntax highlighting, history autosuggestions, prefix and incremental history search, and Tab completion with menus ([manual](docs/subsystems/12-shell.md)). Plus about 60 commands including `ps`, `top`, `strace`, `grep`, `nc`, `ping`, `httpd` and `rshd`, a pager (`more`/`less`) and a text editor that is both `vi` (with ex commands, visual mode, registers and macros) and `emacs` (kill ring, incremental search, `M-x`). Programs are built against a small C library of our own: buffered stdio, `printf` and `scanf` with floating point, `math.h`, `strftime`, `setjmp`, POSIX regular expressions. And Eunuchs can build its own programs: `tcc` (TinyCC) compiles C on the guest, with `tcc -run` for scripts-in-C, and a small `make` rebuilds everything in `/usr/src` ([details](docs/subsystems/13-toolchain.md)). Eunuchs builds its own user space: the C library, tcc's runtime and every program, tcc included, compiled on Eunuchs by tcc from the shared source tree, reproduce themselves byte for byte (`make selfhost`). `db`, a gdb-like debugger built on a new `ptrace`, debugs them at source level from their DWARF: breakpoints, stepping, backtraces, locals and expressions ([details](docs/subsystems/14-debugger.md)).
- **Debugging:** the [debugging guide](docs/DEBUGGING.md) covers the kdb-style monitor, symbolized panics, GDB helpers, the event trace (Perfetto), the sampling profiler (flamegraphs), lockdep, watchdogs, `strace`, packet logs and captures, fault injection, and deterministic replay with reverse debugging.

## Documents

- [docs/README.md](docs/README.md): the documentation index, with reading paths.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the system is put together, subsystem by subsystem, linking to [detailed documents for each subsystem](docs/subsystems/).
- [docs/IMPROVEMENTS.md](docs/IMPROVEMENTS.md): potential improvements, prioritized.
- [docs/DEBUGGING.md](docs/DEBUGGING.md): every debugging facility, and a cookbook for common failures.
- [docs/PLAN.md](docs/PLAN.md): the plan it was built from, phase by phase.
