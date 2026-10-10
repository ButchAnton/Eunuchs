# Build System, Tooling and Testing

Eunuchs is built by one GNU Makefile that cross-compiles the kernel and the user programs with Homebrew's `riscv64-elf-gcc`, links the kernel twice so it can carry its own symbol table, and packs the user programs into an *efs* disk image with a host-side `mkfs`. Testing has four layers: in-kernel unit tests (`KTEST`), a user-space test suite (`/bin/utests`) and stress program, a Python scenario runner that boots QEMU many times against a scratch disk image, and host-native tests run under ASan/UBSan. `make ci` runs all of it from a clean build.

## Responsibilities and scope

This document covers:

- the toolchain and the compiler, assembler and linker flags for the kernel, the user programs and the host tools;
- the build products: the two-pass kernel link, the `.ksyms` table, the linker scripts, the user binaries and the disk image;
- every QEMU command line the Makefile produces (`run`, `debug`, `test`, `stress`, network, record/replay, `qemu-log`);
- the test frameworks (`KTEST`, `utests`, `stress`, host tests), the scenario runner `tools/run_tests.py` and the CI driver `tools/ci.py`;
- how to add tests, how to run a subset, and where to look when something fails.

Out of scope: the efs on-disk format and the fsck rules (see the filesystem document; `tools/mkfs/mkfs.c` appears here only for its build role), the GDB helpers (`tools/gdb/`), and the trace and profile converters (`tools/trace/eutrace.py`, `tools/profile/euprof.py`), which are described in `docs/DEBUGGING.md`.

## Source map

| File | Role |
|---|---|
| `Makefile` | The whole build: toolchain variables, flags, kernel two-pass link, user programs, disk image, QEMU targets, test targets, host tests |
| `kernel/arch/riscv/kernel.ld` | Kernel linker script: high-half link address, page-aligned W^X sections, `.ktests`, `__ex_table`, `.stacks`, `.ksyms` last |
| `user/user.ld` | User linker script: static binaries at `0x10000`, three `PT_LOAD` segments (r-x, r--, rw-) |
| `tools/gen_ksyms.py` | Turns `nm -n` output into `ksyms.S` (the `.ksyms` section); with empty input, an empty table for pass 1 |
| `tools/mkfs/mkfs.c` | Host tool: `mkfs IMAGE SIZE_MB SPEC...` builds `build/fs.img`; `mkfs --check IMAGE` is fsck |
| `tools/run_tests.py` | `make test`: the multi-boot scenario (16 stages), console "expect", network harness, logs and pcaps |
| `tools/ci.py` | `make ci`: clean build, host tests, `make test` on 4 and 1 CPUs, soak, replay check; summary |
| `tools/timeout.py` | Runs a command and kills it after N seconds (exit 124); used because QEMU ignores `SIGALRM` |
| `kernel/include/ktest.h` | `KTEST()` registration macro and the `KT_EXPECT*`/`KT_ASSERT` macros |
| `kernel/debug/ktest.c` | `ktest_run()`: walks `__ktests_start..__ktests_end`, gtest-style output, `KTEST RESULT:` line |
| `kernel/tests/test_*.c` | The 57 kernel unit tests (debug, fs, lib, mm, proc, sched, smp) |
| `kernel/core/main.c` (`kinit`) | Runs `ktest`/`utest` per the boot options, writes `tracedump`, powers off with the result |
| `kernel/arch/riscv/platform.c` (`platform_exit`) | Exit code to QEMU through the `sifive,test0` finisher |
| `user/bin/utests.c` | The 40 user tests plus the multi-boot scenarios (`persist-check`, `crash-*`, `stress[:secs]`) |
| `user/bin/stress.c` | Parallel fork/exec/pipe/file/COW/TCP load generator; `STRESS RESULT:` line |
| `user/bin/exitcode.c`, `user/bin/segv.c`, `user/bin/hello.c` | Helper programs that the user tests exec |
| `tests/host/test_buddy.c` | Host test of `kernel/mm/buddy.c` |
| `tests/host/test_printf.c` | Host test of `lib/printf.c` against the host libc |
| `tests/host/test_tcp.c` | Host test of `kernel/net/tcp.c` (+ `netbuf.c`, `cksum.c`) over a simulated lossy wire |
| `tests/host/test_fsck.py` | Host test: fsck passes a fresh image and catches 7 kinds of injected corruption |
| `.gitignore` | Ignores `build/`, `*.img`, `*.pcap`, `trace*.json` |

## Concepts and data structures

### Toolchain

| Tool | Variable | Default | Used for |
|---|---|---|---|
| Cross compiler | `CROSS`, `CC` | `riscv64-elf-` + `gcc` | kernel and user code, links (via the gcc driver) |
| `nm`, `objdump`, `objcopy` | `NM`, `OBJDUMP`, `$(CROSS)objcopy` | `riscv64-elf-*` | symbol table, `disasm`, stripping user binaries |
| GDB | `GDB` | `riscv64-elf-gdb` | `make gdb`, `make replay-gdb` |
| QEMU | `QEMU` | `qemu-system-riscv64` | everything that boots; `qemu-img` for record/replay |
| Python | `PYTHON` | `python3` | `gen_ksyms.py`, `run_tests.py`, `ci.py`, `timeout.py`, trace/profile tools, `test_fsck.py` |
| Host compiler | `HOSTCC` | `clang` | `mkfs` and the host tests |

Install with `brew install qemu riscv64-elf-gcc riscv64-elf-gdb` (README). The Homebrew GCC must have multilib `libgcc` for both `rv64gc/lp64d` (kernel) and `rv64imac/lp64` (user), because both links pass `-lgcc`. The Makefile is written for GNU Make 3.81, the version macOS ships: user build rules are placed before the generic kernel pattern rules because 3.81 picks the first matching pattern rule, not the most specific one (comment "rules before the generic kernel ones" in `Makefile`).

### Compiler flags and why

**Kernel (`CFLAGS`, `ASFLAGS = CFLAGS -D__ASSEMBLY__`)**

| Flag | Why |
|---|---|
| `-march=rv64gc -mabi=lp64d` | The QEMU `virt` harts are RV64GC |
| `-mcmodel=medany` | PC-relative addressing for globals. The kernel is linked at `0xffffffc080200000` (far from address 0, which `medlow` requires) and its first instructions in `boot.S` run at the physical load address before paging is on; PC-relative code works at both |
| `-ffreestanding -fno-builtin -nostdlib` | No host libc; the kernel has its own `memcpy`, `printf` etc. |
| `-fno-tree-loop-distribute-patterns` | Stops GCC from turning loops into calls to `memset`/`memcpy` (which would recurse inside those functions, or call a function that does not exist) |
| `-fno-pic -fno-stack-protector` | No GOT, no stack-protector runtime |
| `-fno-omit-frame-pointer` | `backtrace_collect()` walks the frame-pointer chain for panics, the monitor, the profiler and `ktest backtrace_walks_frames` |
| `-g -O1` | Debuggable, but optimized enough to be realistic |
| `-Wall -Wextra -Werror -std=gnu11` | Warnings are errors everywhere |
| `-Ikernel/include -Ilib/include` | Kernel headers and the shared `lib/` headers |

Link: `LDFLAGS = -nostdlib -Wl,--no-warn-rwx-segments -T kernel/arch/riscv/kernel.ld`, plus `-lgcc` at the end of the link line.

Avoid libgcc helpers that use lookup tables. `__builtin_ctz` and friends can become libgcc calls, and Homebrew's libgcc is compiled for the `medlow` model, so its table reference cannot reach a high-half kernel at link time. `kernel/proc/signal.c` (`lowest_bit`), `kernel/debug/lockdep.c` (`lowest_bit`) and `kernel/mm/kstack.c` (hand-written popcount loop) have comments saying so. User programs live at `0x10000` and do not have this problem.

**User programs (`UCFLAGS`)**

Same as the kernel except:

| Flag | Why |
|---|---|
| `-march=rv64imac -mabi=lp64` | Integer only. The kernel does not save FP state for user threads and starts every process with `sstatus.FS` = Off (`kernel/proc/proc.c`, exec path); an FP instruction kills the process with a message (`kernel/arch/riscv/trap.c`) |
| `-nostdinc -isystem $(GCC_INC)` | Only Eunuchs's own headers (`user/include`, `lib/include`) plus GCC's freestanding headers (`stdarg.h` etc.) from `$(CC) -print-file-name=include` |
| `-Iuser/include -Ilib/include` | The user libc headers and the shared `uapi/` headers (system call numbers, errno, signals) |

User link: `$(CC) $(UCFLAGS) -nostdlib -static -T user/user.ld -o $@ prog.o $(ULIB) -lgcc`, where `ULIB` is `crt0.o syscall.o libc.o printf.o string.o net.o` from `build/user/lib/`. `lib/printf.c` and `lib/string.c` are compiled twice: into the kernel with `CFLAGS` and into `build/user/lib/` with `UCFLAGS` (explicit rules in the Makefile).

**Host tools and tests**

- `mkfs`: `$(HOSTCC) -O1 -g -Wall -Wextra -Werror -Ikernel/include` (it includes `efs_format.h`, the same header the kernel uses).
- Host tests (`HOST_CFLAGS`): `-g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -Ilib/include -Ikernel/include -Ikernel/mm`. `test_tcp` also gets `-DNET_HOST_TEST`, which compiles out the kernel-only parts of `kernel/include/net.h`, `tcp.c` and `netbuf.c`.

### Build products (all under `build/`)

| Path | What |
|---|---|
| `build/<dir>/<file>.o`, `.d` | Kernel objects mirroring the source tree (`kernel/...`, `lib/...`), with `-MMD -MP` dependency files |
| `build/ksyms0.S`, `ksyms0.o` | Empty symbol table for link pass 1 |
| `build/kernel.pass1.elf` | Pass-1 kernel |
| `build/ksyms.S`, `ksyms.o` | Real symbol table generated from pass 1 |
| `build/kernel.elf` | Final kernel (pass 2); what QEMU boots with `-kernel` |
| `build/pass1.syms`, `pass2.syms` | Text symbols of each pass, compared to prove nothing moved |
| `build/user/lib/*.o` | User runtime |
| `build/user/bin/<prog>` | Full user binaries with DWARF (for GDB) |
| `build/user/stripped/<prog>` | `objcopy --strip-debug` copies; the symbol table stays so the kernel can symbolize user backtraces |
| `build/tools/mkfs` | Host mkfs/fsck |
| `build/fs.img` | The 32 MiB root filesystem image |
| `build/host/test_*` | Host test binaries |
| `build/kernel.asm` | `make disasm` output |
| `build/net.pcap` | Packet capture of every `make run`/`debug`/`qemu-log` |
| `build/qemu.log` | `make qemu-log` output |
| `build/test-fs.img`, `build/test-logs/` | `make test` scratch image, one `.log` per stage, `.pcap` per network stage |
| `build/ktest-trace.bin`, `ktest-trace.json` | Event trace from the first test boot, and its Perfetto conversion |
| `build/stress.img` | `make stress` scratch image |
| `build/rr/` | Record/replay: `kernel.elf`, `base.img`, `fs.qcow2`, `snap.qcow2`, `replay.bin`, `append`, `record.log`, `replay.log` |
| `build/ci/<step>.log` | `make ci` per-step logs |
| `build/profile.bin`, `profile.svg`, `profile.folded`, `trace.bin`, `trace.json` | Written by `profile dump`/`trace dump` via semihosting and converted by `make flamegraph`/`make trace-json` |

### Kernel image layout (`kernel/arch/riscv/kernel.ld`)

`KERNEL_BASE = 0xffffffc000000000`, `KERNEL_PHYS = 0x80200000`. The image is linked at `KERNEL_BASE + KERNEL_PHYS` and loaded (`AT`) at `0x80200000`, where OpenSBI enters it with the MMU off. Entry is `_start`.

| Order | Section | Contents | Symbols |
|---|---|---|---|
| 1 | `.text` | `.text.boot` first, then all text | `__kernel_start`, `__text_end` |
| 2 | `.rodata` (page-aligned) | `.srodata`, `.rodata`; then 8-aligned `.ktests` (`KEEP`), then 8-aligned `__ex_table` (`KEEP`) | `__rodata_start`, `__ktests_start/end`, `__ex_table_start/end`, `__rodata_end` |
| 3 | `.data` + `.bss` (page-aligned) | `.sdata`, `.data`; `.sbss`, `.bss`, `COMMON` | `__data_start`, `__bss_start/end`, `__data_end` |
| 4 | `.stacks` (NOLOAD, 32 KiB-aligned) | boot and emergency stacks; each 32 KiB slot's lower half is a guard | `__stacks_start/end` |
| 5 | `.ksyms` (page-aligned) | symbol table, last so its size cannot move anything else | `__ksyms_start`, `__kernel_end` |

Section boundaries are page-aligned so `vm_init()` can map text r-x, rodata r--, data rw- (W^X); `KTEST(vm_kernel_permissions)` checks that the MMU enforces it.

### User image layout (`user/user.ld`)

Static ELF, entry `_start` (placed first through `.text.start` in `user/lib/crt0.S`). Text starts at `0x10000` in a `PT_LOAD` with flags r-x; `.rodata` page-aligned in an r-- segment; `.data` and `.bss` page-aligned in an rw- segment. `__global_pointer$` is set to `__bss_start`, and `crt0.S` loads `gp` with relaxation disabled. `.comment`, `.riscv.attributes`, `.note*` and `.eh_frame*` are discarded.

### The `.ksyms` table (`tools/gen_ksyms.py`)

`gen_ksyms.py` reads `nm -n` lines, keeps types `t`, `T`, `W`, and skips names that match `^(\.L|\$|__ksym)`. If several names share an address it keeps one, ranked by: not starting with `__` first, then global before local, then alphabetical. Output, in section `.ksyms` (`"a"`, 8-aligned):

```
ksyms_num:   .dword N
ksyms_addrs: .dword addr0, addr1, ...      (sorted)
ksyms_offs:  .word  off0, off1, ...        (offsets into ksyms_names)
ksyms_names: .asciz "name0", "name1", ...
```

The kernel's `ksym_lookup`/`ksym_find`/`ksym_format` (`kernel/debug/ksyms.c`) search this table.

### Disk image contents (`FS_FILES` in the Makefile)

`$(MKFS) build/fs.img $(FS_MB) $(FS_FILES)` with `FS_MB ?= 32`. Spec syntax (from `do_mkfs` in `tools/mkfs/mkfs.c`):

| Spec | Meaning |
|---|---|
| `/path=hostfile` | Copy a host file in (mode 0755, uid/gid 0, from `alloc_inode`) |
| `/path=@/existing` | Hard link to an entry listed earlier |
| `/dir/[:mode[:uid[:gid]]]` | Create a directory, optionally with octal mode and owner |

What goes in:

- every `user/bin/*.c` program as `/bin/<name>` (stripped copy);
- hard links to `/bin/eutils` for `echo mkdir rm rmdir touch ln mv cp wc stat head tail tee chmod chown env id crc32 profile true false yes kill date dmesg free sleep strace su dbg uname clear` (`EUTILS_LINKS`);
- `/etc/motd`, `/etc/passwd` from `user/etc/`, `/www/index.html` from `user/www/`;
- empty `/dev/`, `/proc/`, `/tmp/` (mode 0777), `/www/`, `/home/`, and `/home/guest/` (0755, uid 1000, gid 1000).

The mkfs layout: block 1 superblock, the log from block 2 (`EFS_LOGSIZE + 1` blocks), inode blocks for 1024 inodes, the bitmap, then data. Exit codes: 0 clean, 1 errors found, 2 usage or I/O error.

### Test result protocol

Every automated check relies on a few console strings and the QEMU exit status:

| Producer | Success marker | Exit path |
|---|---|---|
| `ktest_run()` | `KTEST RESULT: PASS` (FAIL if any test failed or no test matched) | |
| `utests` | `UTEST RESULT: PASS` | exit status 0/1 of `/bin/utests` |
| `stress` | `STRESS RESULT: PASS` | exit status 0/1 |
| `kinit` (`kernel/core/main.c`) | `user tests finished with status ...` | `platform_exit(failed ? 1 : 0)` after `ktest`/`utest` |
| `panic_finish` with `panic=shutdown` | `KERNEL PANIC ...` | `platform_exit(2)` |
| `crash_point` (`kernel/fs/log.c`) with `fs.crash=` | `simulating a power failure` | `platform_exit(3)` |
| recursive panic (`kernel/core/panic.c`) | `*** recursive panic; halting ***` | `platform_exit(3)` |
| `mkfs --check` | `clean`; `note: log holds a committed transaction` if the log is non-empty | 0/1/2 |

`platform_exit(code)` writes `0x5555` (pass) or `(code << 16) | 0x3333` (fail) to the `sifive,test0` finisher, which makes QEMU exit with status `code`; it then calls SBI shutdown as a fallback.

### Scenario stage model (`tools/run_tests.py`)

A stage is one QEMU boot (or one fsck run) with a name, a log file and a verdict. There are four kinds:

- **`boot(name, opts, expect_rc=0, expect=(), forbid=())`**: headless boot with `-append "panic=shutdown lockdep=panic <opts> <APPEND>"`, stdin from `/dev/null`, killed after `--timeout` seconds. Passes if the exit status equals `expect_rc`, every `expect` string appears, and no `forbid` string appears.
- **`fsck(name, ...)`**: `mkfs --check build/test-fs.img`, same checks.
- **`interactive(name, steps)`**: boots with only `panic=shutdown lockdep=panic <APPEND>`, waits for `root@eunuchs:/# `, then plays `(send, expect[, prompt])` steps through a small expect class (`Console`). Strings are sent followed by `\r`; `bytes` are raw keys (`^C`, `^Z`) sent after a 0.5 s pause.
- **`network(name, opts, script)`**: like `interactive`, but adds a virtio NIC on QEMU user networking with three host ports picked by `free_port()` and forwarded to guest TCP 80, TCP 5000 and UDP 7, plus a `filter-dump` pcap. Then it runs a Python `script(con, ports)`.

All boots share one scratch image, `build/test-fs.img`, copied from `build/fs.img` at the start. Stages run in order and depend on each other's disk state (for example `persistence` reads a file the first stage wrote).

## How it works

### Kernel build (`make` = `make all`)

1. Compile every `.S` and `.c` in `KDIRS` (`kernel/arch/riscv kernel/core kernel/mm kernel/proc kernel/fs kernel/drivers kernel/net kernel/debug kernel/tests lib`) to `build/<path>.o` with `-MMD -MP`. New files are picked up automatically by `$(wildcard)`.
2. `tools/gen_ksyms.py < /dev/null > build/ksyms0.S` and assemble it: a table with `ksyms_num = 0`.
3. **Pass 1**: link all objects plus `ksyms0.o` into `build/kernel.pass1.elf`.
4. `nm -n kernel.pass1.elf | gen_ksyms.py > build/ksyms.S`; assemble it.
5. **Pass 2**: link all objects plus `ksyms.o` into `build/kernel.elf`.
6. Verify: `nm -n` of both ELFs filtered by `grep -i ' t '` must be identical, otherwise `error: text symbols moved between link passes`. Because `.ksyms` is the last section, its size cannot shift any text address, so this check should never fire. It is there to catch a linker script change that breaks that rule.

### User programs and the disk image

1. `build/user/lib/*.o` from `user/lib/*.{c,S}` and `lib/{printf,string}.c`.
2. Each `user/bin/X.c` becomes `build/user/bin/X.o`, then links to `build/user/bin/X` (`user: $(UPROGS)`).
3. `objcopy --strip-debug` produces `build/user/stripped/X`.
4. `build/tools/mkfs` is built from `tools/mkfs/mkfs.c` (it depends on `kernel/include/efs_format.h`).
5. `build/fs.img` is rebuilt from scratch whenever mkfs, any stripped program, or anything in `user/etc/` or `user/www/` changes. **This discards files created at run time** (Makefile comment on the `$(FSIMG)` rule).

`make all` builds `build/kernel.elf` and `build/fs.img`, so it also builds every user program.

### QEMU command lines

Common base (`QEMUBASE`):

```
qemu-system-riscv64 -machine virt -bios default -nographic -smp $(CPUS) -m $(MEM) \
    -semihosting-config enable=on,target=native -kernel build/kernel.elf
```

`-bios default` is OpenSBI. Semihosting lets the kernel write host files (`tracedump=`, `trace dump`, `profile dump`) relative to QEMU's working directory.

Disk (`DISKOPTS`, a function of the image path):

```
-global virtio-mmio.force-legacy=false -drive file=IMG,if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0
```

Network (`NETOPTS`, used by `run`, `debug`, `qemu-log`):

```
-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$(HTTP_PORT)-:80,hostfwd=tcp:127.0.0.1:$(SHELL_PORT)-:23,hostfwd=udp:127.0.0.1:$(UDP_PORT)-:7 \
-device virtio-net-device,netdev=net0 \
-object filter-dump,id=pcap0,netdev=net0,file=build/net.pcap
```

| Target | Command |
|---|---|
| `run` | `QEMUBASE + DISKOPTS(build/fs.img) + NETOPTS -append "$(APPEND)"` |
| `debug` | as `run` plus `-s -S` (GDB stub on TCP 1234, CPUs halted) |
| `gdb` | `riscv64-elf-gdb -q -x tools/gdb/init.gdb build/kernel.elf` (connects to `localhost:1234`, loads `tools/gdb/eunuchs.py`) |
| `qemu-log` | as `run` plus `-d int,guest_errors,unimp -D build/qemu.log` |
| `test` | `run_tests.py --qemu "QEMU QEMUBASE"`; the runner adds the scratch disk, and a NIC for network stages |
| `stress` | `timeout.py $((STRESS_SECS+60)) QEMU QEMUBASE DISKOPTS(build/stress.img) -append "panic=shutdown lockdep=panic utest=stress:$(STRESS_SECS) $(APPEND)"`, then `mkfs --check build/stress.img` |
| `record` / `replay` / `replay-debug` | `RROPTS` (below) |

`run`, `debug`, `qemu-log` and `record` start with `check_tty`: if stdin is a terminal but stdout is not, make prints a warning that something (for example grc) is piping its output and that console echo will be line-buffered.

### Record and replay (`RROPTS`)

```
-machine virt -bios default -nographic -smp 1 -m $(MEM) -kernel build/rr/kernel.elf \
-icount shift=auto,rr=record|replay,rrfile=build/rr/replay.bin,rrsnapshot=init \
-drive file=build/rr/snap.qcow2,if=none,id=rr \
-global virtio-mmio.force-legacy=false \
-drive file=build/rr/fs.qcow2,if=none,id=img-direct \
-drive driver=blkreplay,if=none,image=img-direct,id=img-blkreplay \
-device virtio-blk-device,drive=img-blkreplay \
-netdev user,id=net0 -device virtio-net-device,netdev=net0 \
-object filter-replay,id=replay,netdev=net0
```

`rr_prepare` (used by `record` and `replay-check`) deletes old `fs.qcow2`, `snap.qcow2` and `replay.bin`, copies `build/kernel.elf` and `build/fs.img` into `build/rr/` (as `kernel.elf` and `base.img`), creates `fs.qcow2` as a qcow2 overlay on `base.img`, creates an empty 64 MiB `snap.qcow2` for the `init` snapshot, and saves the boot options in `build/rr/append`. `replay`, `replay-debug` and `replay-gdb` use only `build/rr/`, so rebuilding does not invalidate a recording. Record/replay is one CPU only, has no semihosting and no host port forwarding.

### Host tests (`make test-host`)

1. Each `tests/host/test_*.c` is compiled with `HOST_CFLAGS` together with the kernel sources listed as its extra prerequisites:
   - `test_printf`: `lib/printf.c`
   - `test_buddy`: `kernel/mm/buddy.c`
   - `test_tcp`: `kernel/net/tcp.c kernel/net/netbuf.c kernel/net/cksum.c` (with `-DNET_HOST_TEST`)
2. Each binary runs in turn (`== build/host/test_X`); the first failure stops the target.
3. `tests/host/test_fsck.py build/tools/mkfs build/user/bin/hello` runs last. It needs a cross-built `hello`, so `test-host` also requires the RISC-V toolchain.

What they check:

- **test_printf**: `ksnprintf` against host `snprintf` for 26 integer formats × 8 values, long/size/intmax/ptrdiff conversions, 7 string formats × 4 strings, `%c`, `%%`, `*` widths (including negative), `%p`, and truncation (return value 11, buffer `"trun"`).
- **test_buddy**: zone of 3000 pages (not a power of two) at PFN `0x80000` with a reserved hole [1500, 1537): initial layout, exhaustion and shuffled free with full coalescing, 200,000 random alloc/free steps with an ownership map that detects overlap and misalignment, error paths (tail free, double free, reserved free, order `MAX_ORDER + 1`).
- **test_tcp**: endpoints A (10.0.0.1) and B (10.0.0.2) over a simulated wire with loss, duplication and delay on a simulated millisecond clock. Scenarios: RFC 1071 checksums; small exchange; bulk both ways; 5% and 20% loss; 10% duplication; 30 ms latency with 5% loss; receiver stall (zero window); 25 randomized seeds (60 KB each way, 10% loss, 5% duplicates, some with 500 ms stalls); simultaneous close (at most 8 segments, both in TIME_WAIT); refused; reset on close with unread data; listen backlog; timeout. At the end it requires `nb_live == 0` (no leaked packet buffers). `test_tcp -v`/`-vv` logs state changes; `test_tcp -v LOSS [SEED DUP STALL]` runs one 60 KB transfer for debugging.
- **test_fsck.py**: builds a 4 MiB image (`/bin/a`, hard link `/bin/b`, `/etc/x`, `/tmp/`) and requires `--check` to pass it, then flips one thing at a time and requires a non-zero exit with a specific message: used block marked free, leaked block, wrong link count, block used twice, orphaned inode, bad `..` entry, bad superblock magic.

### The kernel test run (`ktest`)

`kinit` (`kernel/core/main.c`) runs after the disk, VFS and network are up. With boot option `ktest[=prefix]` it calls `ktest_run(prefix)`, which iterates the `struct ktest` array between `__ktests_start` and `__ktests_end`, skips names that do not start with the prefix (`strncmp`), runs each test in the `kinit` kernel thread with a fresh `struct ktest_ctx`, and logs through `klog(..., SUBSYS_TEST, ...)`:

```
[ RUN      ] name
    file:line: expected: cond          (one per failed check)
[  FAILED  ] name (file)    or    [       OK ] name
N tests run, P passed, F failed
KTEST RESULT: PASS|FAIL
```

`KTEST(name)` emits a `static const struct ktest` into section `.ktests` with `__used`, and `kernel.ld` keeps it with `KEEP`, so registration is automatic: no list to edit. Tests run in link order (the order of `KOBJS`). Zero matching tests count as a failure.

`ktest` can also be run from the debug monitor (`ktest [prefix]`, `cmd_ktest` in `kernel/debug/monitor.c`).

### The user test run (`utest`)

With `utest` (no value) `kinit` spawns `/bin/init -1 /bin/utests`, which runs `utests` once and exits with its status. `utests` runs all 40 entries of its `tests[]` table in order. Each prints `[ RUN      ]`, calls `dbg_mark(name)` (a trace marker), and prints `[       OK ]` or `[  FAILED  ]`. `CHECK(cond)` prints the file and line and marks the test failed. At the end it writes `/home/persist.txt` ("survives reboots") for the next stage and prints `UTEST RESULT:`.

With `utest=<mode>`, `kinit` passes the mode as `argv[1]` and `utests` runs `scenario(mode)` instead of the suite. After `ktest` and/or `utest`, if `tracedump=<path>` is set, `kinit` writes the trace buffer to that host path through semihosting, then calls `platform_exit(failed ? 1 : 0)`.

### The scenario (`make test`, `tools/run_tests.py`)

`make test` depends on the kernel, the image and mkfs, then runs:

```
python3 tools/run_tests.py --qemu "$(QEMU) $(QEMUBASE)" --image build/fs.img \
    --mkfs build/tools/mkfs --timeout $(TEST_TIMEOUT) --append "$(APPEND)"
```

The 16 stages, in order:

| # | Stage | Kind | Options / action | Pass criteria |
|---|---|---|---|---|
| 1 | `kernel+user tests` | boot | `ktest utest tracedump=build/ktest-trace.bin` | rc 0; `KTEST RESULT: PASS`, `UTEST RESULT: PASS`; no `filesystem error` |
| 2 | `fsck after tests` | fsck | | `clean`; no `note: log holds` |
| 3 | `persistence` | boot | `utest=persist-check` | `UTEST RESULT: PASS` |
| 4 | `crash after commit` | boot | `utest=crash-commit-write` | rc 3; `simulating a power failure` |
| 5 | `fsck (committed log)` | fsck | | `clean` and `note: log holds a committed transaction` |
| 6 | `recovery replays` | boot | `utest=crash-commit-check` | `recovering: replaying committed transaction`, `UTEST RESULT: PASS` |
| 7 | `fsck after replay` | fsck | | `clean`; no `note: log holds` |
| 8 | `crash before commit` | boot | `utest=crash-log-write` | rc 3; `simulating a power failure` |
| 9 | `fsck (uncommitted)` | fsck | | `clean`; no `note: log holds` |
| 10 | `recovery discards` | boot | `utest=crash-log-check` | `UTEST RESULT: PASS`; no `recovering: replaying` |
| 11 | `fsck final` | fsck | | `clean` |
| 12 | `smp stress` | boot | `utest=stress` (8 workers, 8 s) | `STRESS RESULT: PASS`, `UTEST RESULT: PASS`; no `LOCKDEP`, `HUNG THREAD`, `filesystem error` |
| 13 | `fsck after stress` | fsck | | `clean`; no `note: log holds` |
| 14 | `interactive shell` | interactive | see below | every expected string within its timeout |
| 15 | `network` | network | `net_basics` | script completes without exception |
| 16 | `network, 5% loss` | network | `net.drop=5`, `net_lossy` | script completes |

Every boot gets `panic=shutdown lockdep=panic` first and `--append` last. Stage 1 does not forbid `LOCKDEP` or `HUNG THREAD`, because the kernel self-tests for lockdep and the hung-thread detector trigger those reports on purpose.

The crash stages use `dbg_fscrash()` from `utests` (`scenario`): write `part1`, arm the crash point (`after-commit` or `after-log`), append `part2`. The kernel stops inside that commit with exit status 3. The `-check` boots require `part1part2` (committed, so it must be replayed) or `part1` (not committed, so it must vanish).

**Interactive shell steps** (stage 14): `echo hello | cat`; `ls / | grep bin > /tmp/x; cat /tmp/x`; `echo abc > /tmp/f; wc -c /tmp/f`; `cat` then `^C` and `echo status=$?` → `status=130`; `sleep 30` then `^Z` → `Stopped  sleep 30`; `jobs`; `kill %1` and `Terminated  sleep 30` at the next prompt; `sleep 1 &` and `wait`; `V=hi sh -c 'echo inner=$V'`; `false || echo or-ok && echo and-ok`; `nosuch` → `command not found`; `segv null` → `Segmentation fault (core dumped)`; `su guest` → prompt `guest@eunuchs:/home/guest$ `, `id` → `uid=1000(guest)`, `touch /etc/nope` → `Permission denied`, `exit`; `cat /proc/self/status | grep pgid`. A step waits for its expected text, then for the next `@eunuchs:` prompt.

**`net_basics`** (stage 15): `ping -c 2 gateway` and `ping -c 2 localhost` with `0% packet loss`; starts `httpd &` and `echod &`; fetches `/` from the host-forwarded HTTP port and requires `Hello from Eunuchs`; fetches `/proc/net/tcp` and requires `ESTABLISHED`; UDP echo of `udp echo test`; 1 MiB from the Mac to `nc -l 5000 | crc32` with a CRC check (`bulk_to_guest`); the guest sends `/bin/sh` to a listener on the Mac (`nc host PORT < /bin/sh`) and the CRC printed by the guest must match; `netstat -s` must mention `retransmits`.

**`net_lossy`** (stage 16, booted with `net.drop=5`, which drops 5% of received packets on every interface including loopback): concatenates several binaries into `/tmp/big`, sends it over loopback with `nc` (our TCP at both ends) and compares CRCs; sends 128 KiB from the Mac (kept short because QEMU's slirp TCP is the sender); `netstat -s` must mention `dropped on purpose`.

After the stages, if `build/ktest-trace.bin` exists the runner converts it with `tools/trace/eutrace.py perfetto` to `build/ktest-trace.json`. It prints `N stages, P passed, F failed: ...` and exits 1 if any stage failed. A failing stage does not stop the run.

### `make stress`

Copies `build/fs.img` to `build/stress.img` with `cp -f`, boots it with `utest=stress:$(STRESS_SECS)` (default 120) under `tools/timeout.py` with `STRESS_SECS + 60` seconds, then fscks `build/stress.img`. `utests` runs `/bin/stress -j 8 -t <secs>`.

`stress` (`user/bin/stress.c`) forks `-j` workers (1–64, default 8; `-t` seconds, default 10; `-n` iterations; `-v` per-worker lines). Each worker loops until the deadline, the iteration limit, or 10 errors:

1. `pipeline`: `/bin/echo <token> | /bin/cat` through `pipe2(O_CLOEXEC)` + `dup2`, checks both exit statuses and the output;
2. `file_io`: writes 1000–16383 bytes to `/tmp/stress.<pid>.<i>`, reads it back, compares, unlinks;
3. `fork_cow`: child checks an 8 KiB array, overwrites it (the parent must not see that), maps, writes and unmaps 4 anonymous pages;
4. `tcp_round`: 16 KiB from a forked sender to a listener on 127.0.0.1, compared byte for byte.

Workers report a `struct result` through a pipe. The parent prints totals, the first 8 lines of `/proc/cpus`, the first line of `/proc/lockdep` and up to 20 lines of `/proc/net/stats`, then `STRESS RESULT: PASS` only if every worker reported, exited 0, and no errors were counted.

### `make replay-check`

`rr_prepare` with `panic=shutdown ktest`; record under `timeout.py 300` with stdin from `/dev/null` into `build/rr/record.log`; replay the same way into `build/rr/replay.log`; require `KTEST RESULT: PASS` in the recording and byte-identical logs (otherwise it prints the first 20 lines of `diff`).

### CI flow (`make ci` → `tools/ci.py`)

1. `shutil.rmtree(build/)`: everything is rebuilt, and old logs, recordings and profiles are deleted.
2. Run each step with `make --no-print-directory -C <root>`, stdout/stderr to `build/ci/<name>.log` (non-alphanumerics replaced by `-`, leading and trailing `-` stripped, so `test (4 CPUs)` → `test--4-CPUs.log`):

| Step | Command | Log |
|---|---|---|
| build | `make -j<cpu_count> all user` | `build.log` |
| host tests | `make test-host` | `host-tests.log` |
| test (4 CPUs) | `make test CPUS=4` | `test--4-CPUs.log` |
| test (1 CPU) | `make test CPUS=1` | `test--1-CPU.log` |
| soak | `make stress CPUS=4 STRESS_SECS=<n>` (default 60, from `--stress-secs` or `$STRESS_SECS`) | `soak.log` |
| replay | `make replay-check` | `replay.log` |

3. Print `ci: <step> ok|FAILED (Ns)` per step (on a TTY a `...` placeholder is overwritten with `\r`). On failure print the last 20 log lines and stop, unless `--keep-going`.
4. Print `ci: X of Y steps passed in Ns` and exit 0 only if every step ran and passed.

`--quick` drops `test (1 CPU)` and `soak`. Pass flags with `make ci CI_FLAGS="--quick --keep-going"`. GNU make exports command-line variables to recipe environments, so `make ci STRESS_SECS=30` reaches `ci.py` through `$STRESS_SECS`.

## Concurrency and locking

- **Parallel make.** `make -jN` is safe for the build: every rule creates its own output directory with `mkdir -p`, and the two link passes are ordered through the `ksyms.S` → `kernel.pass1.elf` dependency. `ci.py` builds with `-j$(os.cpu_count())`. The QEMU targets are serial by nature.
- **One scenario at a time per tree.** `make test` always uses `build/test-fs.img`, `build/test-logs/` and `build/ktest-trace.bin`; `make stress` uses `build/stress.img`; record/replay uses `build/rr/`. Two concurrent runs of the same target in one checkout overwrite each other's files. On a stage timeout `run_tests.py` also runs `pkill -f <path of test-fs.img>`, which kills any QEMU using that image, including one from a concurrent run. Different targets (`make test` and `make stress`) can run side by side.
- **Scratch copies.** `make test` copies `build/fs.img` once at the start; `make stress` copies it each time; `record` and `replay-check` use a qcow2 overlay. `build/fs.img` is only modified by `run`, `debug`, `qemu-log` and the monitor/shell when you use them directly.
- **Ports.** `make run` uses fixed host ports (`HTTP_PORT` 8080, `SHELL_PORT` 2323, `UDP_PORT` 5555) bound to 127.0.0.1; QEMU refuses to start if one is taken. The network test stages call `free_port()` (bind to port 0, read the number, close) three times per stage, so `make test` can run next to a `make run`. There is a small window between closing the probe socket and QEMU binding it.
- **Inside the guest.** The SMP tests rely on `-smp $(CPUS)`. `make test` defaults to 4, and tests that need several CPUs pass trivially on one (comment at the top of `kernel/tests/test_smp.c`).

## Interfaces

### Make targets

| Target | Depends on | What it does |
|---|---|---|
| `all` (default) | kernel, image | Build `build/kernel.elf` and `build/fs.img` (and so all user programs and mkfs) |
| `user` | | Build all user programs |
| `fsck` | mkfs | `build/tools/mkfs --check build/fs.img` |
| `run` | kernel, image | Boot interactively with disk and NIC |
| `debug` | kernel, image | As `run`, halted, GDB stub on :1234 |
| `gdb` | kernel | Attach GDB with `tools/gdb/init.gdb` |
| `qemu-log` | kernel, image | As `run`, with QEMU tracing of interrupts, guest errors and unimplemented features to `build/qemu.log` |
| `disasm` | kernel | `objdump -dS` to `build/kernel.asm` |
| `test` | kernel, image, mkfs | The 16-stage scenario |
| `stress` | kernel, image, mkfs | SMP soak for `STRESS_SECS`, then fsck |
| `test-host` | host tests, mkfs, `build/user/bin/hello` | Host-native tests |
| `ci` | | `tools/ci.py $(CI_FLAGS)` |
| `record` | kernel, image | Interactive one-CPU session recorded into `build/rr/` |
| `replay` | (a recording) | Re-execute the recording |
| `replay-debug` | (a recording) | Replay halted with GDB stub on :1234 |
| `replay-gdb` | | GDB on `build/rr/kernel.elf` |
| `replay-check` | kernel, image | Record and replay `panic=shutdown ktest`; require identical output |
| `flamegraph` | (`build/profile.bin`) | `euprof.py report`, `svg` and `folded`, all with `--no-idle` |
| `trace-json` | (`build/trace.bin`) | `eutrace.py perfetto` → `build/trace.json` |
| `clean` | | `rm -rf build` |

Internal file targets you may name directly: `build/kernel.elf`, `build/kernel.pass1.elf`, `build/fs.img`, `build/tools/mkfs`, `build/user/bin/<prog>`, `build/host/test_<name>`.

### Make variables

| Variable | Default | Effect |
|---|---|---|
| `CROSS` | `riscv64-elf-` | Cross tool prefix |
| `QEMU` | `qemu-system-riscv64` | QEMU binary |
| `PYTHON` | `python3` | Python interpreter |
| `HOSTCC` | `clang` | Compiler for mkfs and host tests |
| `CPUS` | `4` | `-smp` for `run`/`debug`/`test`/`stress`/`qemu-log` (not record/replay, always 1) |
| `MEM` | `128M` | `-m` for every QEMU target |
| `APPEND` | empty | Kernel boot options; for `test`/`stress` appended to every boot after the stage's own options (the first occurrence of a key wins, so it cannot override `panic`, `lockdep`, `ktest` or `utest` there) |
| `TEST_TIMEOUT` | `60` | Seconds per boot/fsck stage in `make test` |
| `STRESS_SECS` | `120` | Duration of `make stress` (`ci.py` uses 60 unless overridden) |
| `FS_MB` | `32` | Disk image size |
| `HTTP_PORT` | `8080` | Host port → guest TCP 80 |
| `SHELL_PORT` | `2323` | Host port → guest TCP 23 (`rshd`) |
| `UDP_PORT` | `5555` | Host port → guest UDP 7 (`echod`) |
| `HOSTFWD` | built from the three ports | Override the whole `hostfwd=` list; `HOSTFWD=` disables forwarding |
| `V` | unset | `V=1` shows full compiler commands instead of `CC file` lines |
| `CI_FLAGS` | empty | Passed to `tools/ci.py` (`--quick`, `--keep-going`, `--stress-secs N`) |

Boot options most used with these targets (the full list is printed by the `help` boot option, from the `options[]` table in `kernel/core/cmdline.c`): `ktest[=prefix]`, `utest[=mode]`, `tracedump=host-path`, `panic=shutdown`, `lockdep=off|warn|panic`, `watchdog=seconds` (default 10), `hung=seconds` (default 30), `smp=n`, `net.drop=percent`, `fs.crash=after-log|after-commit|mid-install`, `log=...`, `trace=...`, `profile[=hz]`.

### Tool command lines

| Tool | Usage |
|---|---|
| `tools/run_tests.py` | `--qemu "CMD"` (required; QEMU without disk), `--image` (required), `--mkfs` (required), `--timeout` (default 60), `--append` (default empty) |
| `tools/ci.py` | `--quick`, `--keep-going`, `--stress-secs N` (default `$STRESS_SECS` or 60); honours `$MAKE` |
| `tools/timeout.py` | `timeout.py SECONDS CMD...`: exit status of CMD, or 124 after killing it |
| `tools/gen_ksyms.py` | stdin: `nm -n` output; stdout: assembly |
| `build/tools/mkfs` | `mkfs IMAGE SIZE_MB [/path=hostfile | /path=@/existing | /dir/[:mode[:uid[:gid]]]]...`; `mkfs --check IMAGE` |
| `/bin/stress` (guest) | `stress [-j workers] [-t seconds] [-n iterations] [-v]` |
| `/bin/utests` (guest) | no argument: the suite; one argument: a scenario |
| `build/host/test_tcp` | `[-v|-vv]`, or `-v LOSS [SEED DUP STALL]` for one transfer |

### Kernel test API (`kernel/include/ktest.h`)

| Macro | Behaviour |
|---|---|
| `KTEST(name) { ... }` | Defines and registers a test; the body sees `__kt` (the context) |
| `KT_EXPECT(cond)` | On false: record a failure (`expected: cond`), continue |
| `KT_ASSERT(cond)` | On false: record a failure (`assert: cond`), `return` from the test |
| `KT_EXPECT_EQ(a, b)` | Compare as `unsigned long long`; prints both values in hex |
| `KT_EXPECT_STREQ(a, b)` | `strcmp`; NULL on either side fails |

`int ktest_run(const char *filter)` returns the number of failed tests (1 if none ran). In `test_mm.c`, `EXPECT_CORRUPTIONS(n, body)` runs `body` with `mm_panic_on_corruption` off and requires exactly `n` new corruption reports.

### Kernel test catalog (57 tests)

**`kernel/tests/test_debug.c`**

| Test | Checks |
|---|---|
| `ksyms_lookup` | `.ksyms` lookups: exact and offset hits for `kmain`, `ksym_find("panic")`, unknown address → NULL, `ksym_format` → `panic+0x10` |
| `backtrace_walks_frames` | Frame-pointer unwinder walks three real frames up to `thread_entry` |
| `safe_access_recovers_from_faults` | `safe_read_u64`/`safe_write_*` return -1 on unmapped addresses (NULL, firmware memory) and count the page faults; work on valid memory |
| `breakpoint_trap_and_resume` | `ebreak` reaches `trap_breakpoint_hook`, the hook can change `a0`, callee-saved `s1` survives, the PC is symbolized |
| `device_tree` | FDT parse: RAM at `0x80000000` ≥ 16 MiB, UART `0x10000000` IRQ 10, timebase 10 MHz, PLIC and test device present, 8 virtio slots |
| `cmdline` | `ktest` present, `panic=shutdown`, unknown option NULL, no prefix matching |
| `log_ring_and_thresholds` | Log ring advances; per-subsystem threshold filters; subsystem and level name lookups |

**`kernel/tests/test_fs.c`**

| Test | Checks |
|---|---|
| `fs_ops_release_buffers_and_inodes` | mkdir/write/read/rename/unlink/rmdir in `/tmp/kt`; no busy buffers or leaked inode references afterwards |
| `fs_sparse_file_and_truncate` | One byte at 5 MiB uses 3–4 blocks; holes read as zeros; `O_TRUNC` returns every block |
| `fs_path_walk` | `namei` of `/`, `/dev/..` (crossing a mount), `/dev/null`, `nameiparent`, `ENOENT`, `ENOTDIR` |
| `fs_no_errors_recorded` | Superblock error count 0; in-memory free count equals the bitmap |

**`kernel/tests/test_lib.c`**

| Test | Checks |
|---|---|
| `printf_basic` | `ksnprintf` precision, `*` width, left justify |
| `printf_truncation` | Return value is the full length; output truncated and terminated |
| `string_funcs` | String/memory functions, including overlapping `memmove` |
| `strtoul` | `kstrtoul` with base 0/10/16, octal, end pointer |

**`kernel/tests/test_mm.c`**

| Test | Checks |
|---|---|
| `pmm_orders_and_alignment` | Orders 0–6 aligned, counted, carry an allocation site, arrive poisoned |
| `pmm_random_stress_no_overlap` | 4 rounds of random alloc/free of 256 tagged blocks; tags intact, free count restored, `pmm_verify_free()` 0 |
| `pmm_detects_double_free` | Second `page_free` reports one corruption |
| `pmm_detects_write_after_free` | A stale write is found by `pmm_verify_free()` and re-poisoned |
| `pmm_rejects_reserved_and_tail_frees` | Freeing a kernel page, a tail page or with the wrong order is reported |
| `kmalloc_basic` | Sizes 1–2048: 8-byte aligned, exact `ksize`, uninit poison, distinct, `kzalloc` zeroed, `kfree(NULL)` |
| `kmalloc_stress` | 6 rounds of random alloc/free of 512 slots with content checks; `slab_verify()` 0 |
| `kmalloc_detects_overflow_by_one` | Writing one byte past the size is reported at `kfree` |
| `kmalloc_detects_underflow` | Writing before the object is reported |
| `kmalloc_detects_double_free` | Second `kfree` reported |
| `kmalloc_detects_use_after_free` | Write after free found by `slab_verify()`, then repaired |
| `kmalloc_detects_bad_pointers` | `kfree` of non-heap pointers (e.g. a `.bss` variable) reported |
| `kmalloc_large` | Large (page-backed) allocations: page aligned, `PG_LARGE`, exact `ksize`, overflow detected |
| `kmem_cache_custom` | A custom cache allocates and frees; freeing a `kmalloc` object into it is reported |
| `leak_report_attributes_sites` | `slab_leak_report` attributes 3 live objects to `leaky_alloc` |
| `vm_kernel_permissions` | Kernel text r-x, rodata r--, data rw-, enforced by the MMU; NULL and firmware unmapped |
| `vm_map_unmap_alias` | Map a page at an alias address, write through it, double map rejected, unmap faults |
| `vm_fault_hints` | Fault hints for NULL, use-after-free poison, uninitialized poison, W^X, non-canonical |

**`kernel/tests/test_proc.c`**

| Test | Checks |
|---|---|
| `vma_add_find_overlap_unmap` | VMA insert, overlap and alignment errors, lookup, hole punching splits a VMA |
| `demand_paging_and_cow` | Demand-zero fault, permission and no-VMA errors, `mm_fork` shares frames COW, child write copies, parent's last-reference write just re-enables W, no page leak |
| `copy_user_rejects_bad_ranges` | `copy_from_user`/`copy_to_user`/`strncpy_from_user` return `-EFAULT` for unmapped, kernel and straddling ranges |
| `elf_load_from_disk` | Loads `/bin/hello`: entry inside a text VMA that is X and not W, `_start` symbolized; junk ELF → `-ENOEXEC` with a reason |

**`kernel/tests/test_sched.c`**

| Test | Checks |
|---|---|
| `thread_runs_and_joins` | Thread runs, `kthread_join` returns its exit code 42 |
| `many_threads_no_leaks` | 24 threads; pages and stacks returned (allowing for the kstack cache) |
| `semaphore_ping_pong` | Two threads strictly alternate through two semaphores |
| `mutex_mutual_exclusion` | 4 threads × 200 increments with `yield()` inside the critical section; total 800, contention seen |
| `timer_preempts_busy_thread` | A spinning thread is preempted by the timer |
| `sleep_duration` | `sleep_ms(40)` takes 40–200 ms |
| `waitqueue_wakes_all` | `wake_up` wakes all three sleepers |
| `stack_high_water_and_guard` | Stack high-water mark ≥ 6000 bytes; the guard page below the stack is unmapped |
| `trace_records_context_switches` | Trace has `thread_create`, `sched_switch` and `thread_exit` for a test thread |
| `preemption_does_not_lose_wakeups` | Regression: preemption between `prepare_to_wait` and the condition check must not sleep forever (hangs if broken) |

**`kernel/tests/test_smp.c`**

| Test | Checks |
|---|---|
| `smp_all_harts_online` | `ncpus_online` matches the DT and `smp=`; every CPU online, has an idle thread and ticks |
| `bound_threads_run_on_every_cpu` | A thread bound to each CPU runs there |
| `spinlock_exclusion_under_contention` | 4 × 20000 non-atomic increments under a spinlock |
| `mutex_exclusion_under_contention` | 4 × 2000 increments under a mutex with yields |
| `ipi_reaches_every_cpu` | A reschedule IPI to each other CPU is counted there |
| `tlb_shootdown_invalidates_every_cpu` | After unmap + `tlb_shootdown()` every CPU faults on the page |
| `stop_the_world_parks_every_other_cpu` | `smp_stop_others` parks all other CPUs and resume releases them (skipped on 1 CPU) |
| `thread_churn_across_cpus` | 25 rounds of 8 short threads; page and slab verifiers clean |
| `lockdep_catches_inversions_before_they_deadlock` | AB/BA inversion, a 3-lock cycle and a mutex under a spinlock each produce one report (skipped if `lockdep=off`) |
| `hung_thread_detector_reports_long_mutex_waits` | With a 50 ms threshold a waiter blocked on a mutex is reported |

### User test catalog (`user/bin/utests.c`, 40 tests, in run order)

| Test | Checks |
|---|---|
| `tcp_loopback_echo` | 200 KB through a forked echo server on 127.0.0.1, `poll`-driven, half-close, exit status |
| `tcp_errors` | `ECONNREFUSED`, `EADDRINUSE`, `ENOTCONN`, reset → `ECONNRESET` then `EPIPE`, `EPROTONOSUPPORT`, `EBADF`, `ENOTSOCK` |
| `nonblocking_sockets` | Non-blocking accept `EAGAIN`, connect `EINPROGRESS` + `POLLOUT` + `SO_ERROR`, recv `EAGAIN` |
| `udp_loopback` | Datagrams, implicit bind on `sendto`, source port, truncated receive, reply |
| `poll_and_select` | `poll` timeouts and readiness, `select`, `POLLHUP`, a sleeping poll woken by another process |
| `tty` | `isatty`, `ENOTTY`, `tcgetattr`/`tcsetattr` ECHO, foreground group, `/dev/tty`, background read → `SIGTTIN` stop |
| `permissions` | uid 0, `chmod`, `umask`, then as uid 1000: `EACCES`, `/tmp` writable, ownership, `EPERM` for chmod/setuid; no exec without x bit |
| `time` | `nanosleep` 30 ms, `time()` after 2020, `gmtime_r` of 2000-02-29 |
| `pipe_basic` | Write/read, EOF, `ESPIPE` on lseek |
| `pipe_bulk_through_fork` | 100,000 bytes through a pipe from a child, weighted checksum |
| `pipe_epipe_and_eintr` | `EPIPE` + `SIGPIPE`; blocking read interrupted by a signal → `EINTR` |
| `cloexec_and_env` | `O_CLOEXEC` and `F_SETFD` honoured by exec; environment passed to children |
| `signal_handler` | Handler runs; registers and stack restored by sigreturn |
| `signal_mask` | Blocked signal stays pending and is delivered on unblock |
| `signal_default_and_ignore` | Default `SIGTERM` kills without core; `SIG_IGN`; `sigaction(SIGKILL)` → `EINVAL` |
| `catch_segv` | A `SIGSEGV` handler runs for a NULL write |
| `sigchld` | `SIGCHLD` delivered once on child exit |
| `stop_continue` | `SIGSTOP` reported via `WUNTRACED`, then `SIGCONT`/`SIGKILL` |
| `eintr` | `sleep_ms` interrupted by a signal returns early with `EINTR` |
| `process_groups` | `setpgid` from both sides, `getpgid`, `getsid`, kill a whole group |
| `file_rw` | Create/`O_EXCL`/append/lseek/overwrite/`fstat`/truncate/`EBADF`/unlink |
| `big_file` | 4.5 MiB file through direct, indirect and double-indirect blocks |
| `dirs` | mkdir, link counts, 100 entries (more than one block), `ENOTEMPTY`, `EISDIR`, rmdir |
| `link_rename` | Hard links, rename over a file, moving a directory across directories updates `..`, `EINVAL`, `EPERM` for directory links |
| `paths` | `.`/`..`/`//` handling, `ENOTDIR`, `ENAMETOOLONG`, relative paths, `getcwd` incl. `/proc/self` and `/dev/..` |
| `fds` | `dup`, `dup2`, shared offsets across fork, `EMFILE` before 40 descriptors |
| `devices_and_proc` | `/dev/null`, `/dev/zero`, `/dev/console`, `/proc/meminfo`, `/proc/self/status`, `/proc/<pid>`, `/dev/kmsg` |
| `exec_copied_program` | Copy `/bin/exitcode` to `/tmp` and exec it |
| `fork_wait_status` | `waitpid` status of `_exit(42)` |
| `many_children` | 10 children, exit codes sum to 55, then `ECHILD` |
| `cow_isolation` | Child sees parent's data and its writes do not leak back |
| `exec_args` | argv with spaces and an empty string reaches `/bin/exitcode` |
| `exec_missing` | exec of a missing file → `ENOENT` |
| `faults_kill_with_signals` | `/bin/segv` null/text/stack/kernel → signal 11, illegal → signal 4 |
| `malloc` | 64 mixed-size allocations, partial frees, contents intact, 300 KB allocation |
| `mmap` | 256 KiB anonymous mapping demand-zero; W+X mapping refused with `EACCES` |
| `bad_pointers_rejected` | `EFAULT` for user NULL-ish and kernel addresses in `write`/`read`/`execv`/`waitpid` |
| `kill` | `SIGKILL` of a sleeping child is prompt; `ESRCH` |
| `sleep_and_ids` | `sleep_ms(50)` ≥ 50 ms; pid and ppid sane |
| `brk` | `sbrk` grow, touch, shrink |

**Scenarios** (`scenario()`, selected with `utest=<mode>`):

| Mode | Does |
|---|---|
| `persist-check` | `/home/persist.txt` contains `survives reboots` (written by the suite); deletes it |
| `crash-commit-write` | Writes `/home/crash1.txt` = `part1`, `dbg_fscrash("after-commit")`, appends `part2`; the machine stops (exit 3) |
| `crash-commit-check` | `/home/crash1.txt` is `part1part2` after recovery |
| `crash-log-write` | Same with `/home/crash2.txt` and `after-log` |
| `crash-log-check` | `/home/crash2.txt` is `part1` (the append vanished) |
| `stress[:secs]` | Runs `/bin/stress -j 8 -t <secs>` (default 8) |

Any other mode prints `unknown mode` and fails.

## Debugging and observability

**Where output goes**

- `make test`: `build/test-logs/<stage name>.log` (file names contain spaces and punctuation, e.g. `network, 5% loss.log`, so quote them), and `build/test-logs/network.pcap`, `build/test-logs/network--5--loss.pcap`. A failing stage prints its reasons (`exit status`, `missing: '...'`, `unexpected: '...'`, or the expect error with the last command sent) and the last 30 (boot/fsck) or 25 (interactive/network) log lines. The network harness writes `[harness: N bytes in T s]` lines into the log.
- `build/ktest-trace.json`: open in ui.perfetto.dev to see what the first test boot did; `utests` marks each test with `dbg_mark(name)`.
- `make ci`: `build/ci/*.log`; the last 20 lines of a failing step are printed.
- `make stress`: on the terminal; `make replay-check`: `build/rr/record.log` and `build/rr/replay.log`.
- `make run`: `build/net.pcap`; `make qemu-log`: `build/qemu.log`.

**Investigating a failing stage**

1. Read the stage log. Look for `[  FAILED  ]` lines (both kernel and user tests use this format), `check failed:`, `expected:`/`assert:`, `KERNEL PANIC`, `LOCKDEP`, `HUNG THREAD` or `*** TIMEOUT`.
2. If the failure is a boot with an unexpected exit status: 1 means a test failed, 2 a panic (`panic=shutdown`), 3 a simulated crash or a recursive panic, `timeout` means a hang that TEST_TIMEOUT cut off.
3. Narrow it down by booting the kernel directly (next item). `make test APPEND="ktest=tlb_"` does **not** work: `cmdline_get()` (`kernel/core/cmdline.c`) returns the first word that matches a key, and the stage's own bare `ktest` comes before `APPEND`, so all tests still run. For the same reason `APPEND` cannot override `panic=` or `lockdep=` in `make test`/`make stress`; it can only add options the stage does not set (for example `log=`, `trace=`, `watchdog=`, `smp=`).
4. Rerun one stage by hand. Copy the image first; `cp` may be aliased to `cp -i`, so use `cp -f` or `command cp`:

```
cp -f build/fs.img /tmp/t.img
command qemu-system-riscv64 -machine virt -bios default -nographic -smp 4 -m 128M \
  -semihosting-config enable=on,target=native -kernel build/kernel.elf \
  -global virtio-mmio.force-legacy=false \
  -drive file=/tmp/t.img,if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
  -append "panic=shutdown lockdep=panic ktest=fs_"
echo "exit status $?"
build/tools/mkfs --check /tmp/t.img
```

   Replace the `-append` value with any stage's options (`utest`, `utest=persist-check`, `utest=stress:30`, ...). Drop `panic=shutdown` to land in the debug monitor on a panic instead of exiting. Keep `panic=shutdown` when running `ktest`: `KTEST(cmdline)` expects it.
5. For an interactive debug session on the real image: `command make run APPEND="ktest=vm_"` or `command make debug` + `make gdb`. Note that `run` uses `build/fs.img` itself, so tests that write files change it.
6. Failures that appear only with several CPUs: compare `make test CPUS=1`. For a deterministic reproduction on one CPU use `command make record APPEND="..."`, then `make replay`, or `make replay-debug` + `make replay-gdb` for reverse execution.
7. Host tests: run the binary directly (`build/host/test_tcp -vv`, `build/host/test_buddy`). ASan/UBSan report on stderr.
8. Network stages: open the stage's `.pcap` in Wireshark; `netstat -s` output is in the log.

**Environment gotchas**

- **grc wraps `make`** in a zsh shell function that pipes its output, so the guest console is line-buffered (typed characters appear after Return). Use `command make run` (also for `debug`, `record`, `qemu-log`). `\make` does not help, because grc uses a function, not an alias. `check_tty` warns when it detects this.
- **`cp` aliased to `cp -i`** in interactive shells: copying over an existing image by hand prompts. The Makefile recipes use `cp -f` and run under `sh`, so they are not affected.
- Ports 8080/2323/5555 in use → `make run` fails to start QEMU; use `HTTP_PORT=...` or `HOSTFWD=`.
- Quit QEMU with `Ctrl-A X`; `Ctrl-]` enters the kernel monitor.
- `make ci` starts by deleting `build/`, including any recording in `build/rr/` and profiles or traces you dumped.

## Limitations and known issues

- **The scenario is all-or-nothing.** `run_tests.py` has no option to run one stage or a range, and stages depend on earlier stages' disk state (stage 3 needs the file from stage 1; the crash checks need the crash writes). A failure early on usually causes more failures later, which can hide the real cause.
- **User tests cannot be filtered.** `utest=<mode>` selects a scenario, not a test; any unknown mode fails. The only way to run one user test is to edit `tests[]`.
- **Some tests can hang instead of failing.** Examples: `t_tcp_loopback_echo` loops forever if `poll` keeps timing out (the `CHECK` records a failure but the `while (got < sizeof(in))` loop continues); `stress.c` `tcp_round` blocks in `accept` forever if the sender's `connect` fails; `KTEST(preemption_does_not_lose_wakeups)` hangs by design. In `make test` these become `*** TIMEOUT` after `TEST_TIMEOUT` seconds with no indication of which check failed.
- **Interactive and network stages have no overall time limit.** They are bounded only by their per-step timeouts (20 s for the first prompt, 10 s per step by default, 60 s for bulk transfers), not by `TEST_TIMEOUT`. On exit `Console.close()` kills QEMU, so those boots never shut down cleanly and are not fscked afterwards.
- **`KT_ASSERT` returns without cleanup.** Several tests leave state behind on an assertion failure: `hung_thread_detector_reports_long_mutex_waits` returns with `hung_m` locked and the hung threshold at 50 ms; `trace_records_context_switches` leaves `trace_groups_enabled` changed; `elf_load_from_disk` leaks the image buffer and the `mm`; `tlb_shootdown_invalidates_every_cpu` leaves the test page mapped. Later tests can then fail for unrelated reasons.
- **`APPEND` cannot override stage options.** `cmdline_get()` returns the first matching word, and stage options precede `APPEND`. The kernel command line also holds at most 256 bytes and 32 words (`cmdline_init`); words beyond that are dropped silently.
- **`KTEST(cmdline)` assumes `panic=shutdown`** on the command line, so a plain `make run APPEND=ktest`, or `ktest` from the monitor, reports a failure there.
- **Running `ktest` from the monitor** runs every test in the monitor's context. Tests that create threads, sleep or wait for other CPUs depend on a running scheduler and on the other CPUs, which the monitor normally stops. Expect only self-contained tests (for example `ktest printf_`, `ktest ksyms`) to be reliable there. (This is inferred from the code, not verified.)
- **Kernel tests are always linked into the kernel.** `kernel/tests` is in `KDIRS`; there is no build option to leave them out.
- **Incomplete rebuild dependencies.** Objects do not depend on the Makefile or on `CFLAGS`/`UCFLAGS`, so changing flags needs `make clean`. `build/fs.img` does not depend on the Makefile, so changing `FS_FILES`, `EUTILS_LINKS` or `FS_MB` does not rebuild the image (touch a user program or `rm build/fs.img`). User `.S` files are compiled without `-MMD` (they include no headers today).
- **Rebuilding the image discards guest data.** Any user-program change rebuilds `build/fs.img` from scratch.
- **Shared scratch paths.** Two `make test` (or two `make stress`, or record/replay-check) runs in one checkout collide on fixed paths, and the timeout path's `pkill -f build/test-fs.img` kills the other run's QEMU.
- **`free_port()` is racy.** The port is released before QEMU binds it.
- **`tracedump=build/ktest-trace.bin` is relative to QEMU's working directory.** `make test` runs from the repository root, so it works; running `run_tests.py` from another directory writes the trace elsewhere, and the runner then silently skips the Perfetto conversion.
- **Exit status 3 has two meanings**: a simulated power failure and a recursive panic both call `platform_exit(3)`. The crash stages also require the `simulating a power failure` text, so this is only a diagnostic ambiguity.
- **Host tests need the cross toolchain**, because `test_fsck.py` uses `build/user/bin/hello` as payload. `test_fsck.py` never deletes its temporary directory.
- **mkfs gives every copied file mode 0755** (`alloc_inode`), including `/etc/passwd`, `/etc/motd` and `/www/index.html`; there is no spec syntax for file modes. `SIZE_MB` is parsed with `atoi` without validation.
- **The pass-1/pass-2 check compares only text symbols** (`grep -i ' t '`), not data or rodata addresses. This is enough as long as `.ksyms` stays last in `kernel.ld`.
- **Record/replay is one CPU only**, with no semihosting and no host port forwarding (`RROPTS`). `rr_prepare` writes the options with `echo '$(1)'`, so an `APPEND` containing a single quote breaks it.
- **The kernel is compiled for `rv64gc/lp64d`.** Nothing in the build stops the compiler from emitting FP instructions in kernel code, and the kernel saves no FP state. Kernel code uses no floating point today; this is a latent hazard, not a current bug.
- **Documentation drift:** `docs/DEBUGGING.md` lists the networking stage before the interactive shell; `run_tests.py` runs the shell first. It also says a kernel test can be added "anywhere"; in fact it must be in a directory listed in `KDIRS`.

## Potential improvements

1. **Stage selection in run_tests.py** — Add `--only NAME[,NAME]` / `--from NAME` and `--list`, and declare each stage's prerequisites (e.g. `persistence` needs `kernel+user tests`) so the runner can run the minimal chain. This would make investigating a failure much faster than a full 16-stage run. Impact: high. Effort: medium.
2. **Per-test filter for utests** — Accept `utest=only:<prefix>` (or treat a mode that matches a test name prefix as a filter) and have `run_tests.py` pass it through. This mirrors `ktest=prefix` and avoids editing `tests[]`. Impact: medium. Effort: small.
3. **Turn hangs into failures** — Bound the loops that can spin forever: break out of `t_tcp_loopback_echo` when `poll` times out, use a timed `poll` before `accept` in `stress.c` `tcp_round`, and add a per-test watchdog in `ktest_run` that logs the running test's name when it exceeds N seconds. Then a timeout names the culprit. Impact: high. Effort: small.
4. **Overall deadline for interactive/network stages** — Wrap `interactive()`/`network()` in the same `--timeout` budget as `boot()` and log `*** TIMEOUT` consistently. After `Console.close()`, run an fsck stage so unclean shutdowns are checked too. Impact: medium. Effort: small.
5. **Cleanup-safe KT_ASSERT** — Provide `KT_ASSERT_GOTO(cond, label)` or a per-test cleanup hook, and convert tests that hold locks or change global settings (`hung_thread_detector_*`, `trace_records_context_switches`, `tlb_shootdown_*`, `elf_load_from_disk`). This stops one failure from causing others. Impact: medium. Effort: small.
6. **Rebuild on flag and manifest changes** — Make objects depend on a generated `build/flags.stamp` (rewritten only when `CFLAGS`/`UCFLAGS` change), and make `$(FSIMG)` depend on the Makefile or a manifest stamp. Removes the "needs make clean" trap. Impact: medium. Effort: small.
7. **Unique scratch paths per run** — Put `test-fs.img`, logs and `ktest-trace.bin` under a per-run directory (or take a lock file), and replace `pkill -f` with killing the specific child PID that `subprocess` already tracks. Concurrent runs in one tree would then be safe. Impact: low. Effort: small.
8. **Machine-readable test results** — Have `run_tests.py` and `ci.py` also write a JSON/JUnit summary (stage, verdict, duration, failing lines), and parse `[  FAILED  ]` names out of the logs into it. Useful for any future hosted CI and for tracking flaky stages. Impact: medium. Effort: small.
9. **Optional exclusion of kernel tests** — A `KTESTS=0` make variable that drops `kernel/tests` from `KDIRS` (and `.ktests` stays empty). This gives a smaller production kernel and checks that nothing outside the tests depends on test code. Impact: low. Effort: small.
10. **Make `KTEST(cmdline)` independent of how it was booted** — Check only properties that hold for any boot (e.g. `cmdline_has("ktest")` when run from `kinit`, unknown option NULL, no prefix match), or skip the `panic=shutdown` check when that option is absent. Then `ktest` from `make run` or the monitor passes cleanly. Impact: low. Effort: small.
11. **File modes in mkfs specs** — Extend `/path=hostfile` with an optional `:mode[:uid[:gid]]` suffix like directories have, and default non-executables to 0644. Images would then match Unix expectations, and permission tests could cover pre-built files. Impact: low. Effort: small.
12. **Kernel compiled without FP** — Build the kernel with `-march=rv64imac_zicsr_zifencei -mabi=lp64` (and a matching libgcc multilib), or clear `sstatus.FS` in the kernel, so any accidental floating-point use fails at compile time or traps immediately instead of silently corrupting user-invisible state. Impact: low. Effort: medium.
13. **Self-contained host tests** — Let `test_fsck.py` generate its payload on the host instead of using `build/user/bin/hello`, and remove its temp directory. `make test-host` would then run without the RISC-V toolchain (useful for quick checks on another machine). Impact: low. Effort: small.
14. **Sanitize stage log names** — Use the same sanitization for `.log` files as for `.pcap` files (no spaces, commas or `%`). They become easier to use from a shell and in scripts. Impact: low. Effort: small.
15. **Let `APPEND` override stage options** — Make `cmdline_get()` return the last matching word (or have `run_tests.py` put `APPEND` before the stage options). Then `make test APPEND="ktest=fs_ lockdep=warn"` would narrow or relax runs as users expect. Today such values are ignored without any message. Impact: medium. Effort: small.
