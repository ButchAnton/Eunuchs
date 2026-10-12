# The native toolchain: tcc and make

Eunuchs can compile C programs on itself. `/bin/tcc` is TinyCC, a small one-pass C compiler, assembler and linker, built for RISC-V 64 and linked against the Eunuchs libc. The disk image carries what it needs: the headers in `/usr/include`, the C library and start-up files in `/usr/lib`, and tcc's own runtime in `/usr/lib/tcc`. `/bin/make` is a small POSIX-style make written for Eunuchs, and `/usr/src` holds the sources of every program in `/bin` with a Makefile, so the system can rebuild its own user space:

```
root@eunuchs:/# cd /usr/src && make && make install
```

This document covers how tcc is configured, patched and built, how the sysroot is put together, `tcc -run`, the make program, the problems tcc's dialect causes for our code, and how to debug compile problems.

## Responsibilities and scope

**In scope:**

- The vendored TinyCC (`third_party/tinycc`) and the Eunuchs configuration (`ports/tcc/config.h`).
- The build: the host cross compiler `build/tcc/xtcc`, the native compiler `/bin/tcc`, tcc's runtime library `libtcc1.a`, and the sysroot (`build/sysroot`, the `sysroot` make target) that is copied into the image.
- `tcc -run` and the kernel support it needs (`mprotect`).
- `user/bin/make.c` (`/bin/make`) and `/usr/src` with `user/usr-src/Makefile`.
- The `toolchain and editors` stage of `make test`.

**Out of scope:**

- The C library itself ([10](10-user-space.md)); this document only describes how it is packaged for tcc (`libc.a`, `crt1.o`).
- `mprotect`'s implementation ([02](02-memory.md#user-address-spaces-uvmc)).
- The host build with GCC ([11](11-build-and-testing.md)).
- Debugging the programs tcc builds: `tcc -g` produces DWARF 5, which `db` reads ([14](14-debugger.md)).

## Source map

| File | Role |
|---|---|
| `third_party/tinycc/` | TinyCC, the `mob` branch at commit `43c7708` (version 0.9.28rc), vendored whole. Only `riscv64-link.c` is changed (below). `tcc.c` with `ONE_SOURCE=1` compiles the whole compiler as one translation unit. |
| `third_party/tinycc/lib/` | tcc's runtime sources: `stdatomic.c`, `atomic.S`, `builtin.c`, `alloca.S`, `armflush.c` (into `libtcc1.a`) and `runmain.c` (the start-up code for `tcc -run`). |
| `third_party/tinycc/include/` | tcc's own headers (`stdarg.h`, `stddef.h`, `float.h`, `stdbool.h`, ...), installed in `/usr/lib/tcc/include`. |
| `ports/tcc/config.h` | The configuration, shared by `/bin/tcc` and `xtcc`. |
| `Makefile` (section "TinyCC") | `XTCC`, `/bin/tcc`, the runtime objects, `libtcc1.a`, `libc.a`, `crt1.o`/`crti.o`/`crtn.o`, header copies, `SYS_FILES`, `USR_SRC`, the `sysroot` target. |
| `user/lib/crt0.S` | Also built as `/usr/lib/crt1.o`, with `-DNO_GP`. |
| `user/lib/exit.c` | `exit` and `atexit` in an object of their own, so `tcc -run` can replace them. |
| `user/bin/make.c` | `/bin/make`. |
| `user/usr-src/Makefile` | `/usr/src/Makefile`: rebuilds every program with tcc. |
| `tools/run_tests.py` | The `toolchain and editors` stage. |

## Concepts and data structures

### The configuration (`ports/tcc/config.h`)

| Setting | Value | Why |
|---|---|---|
| `TCC_TARGET_RISCV64` | 1 | Both compilers generate RISC-V 64 code (lp64d). |
| `CONFIG_TCCDIR` | `/usr/lib/tcc` | Default `{B}`, overridable with `-B`. |
| `CONFIG_TCC_SYSINCLUDEPATHS` | `{B}/include:{B}/../../include` | tcc's headers, then `/usr/include`. |
| `CONFIG_TCC_LIBPATHS` | `{B}:{B}/..` | `libtcc1.a`, then `/usr/lib` (`libc.a`). |
| `CONFIG_TCC_CRTPREFIX` | `{B}/..` | `crt1.o`, `crti.o`, `crtn.o` in `/usr/lib`. |
| `CONFIG_TCC_SWITCHES` | `"-static"` | Every link is static: there are no shared libraries. |
| `CONFIG_TCC_ELFINTERP` | `"-"` | No dynamic loader. |
| `CONFIG_TCC_STATIC` | 1 | tcc itself does not use `dlopen`. |
| `CONFIG_RUNMEM_RO` | 1 | `tcc -run` maps generated code r-x and data rw-, never rwx (the kernel enforces W^X). |
| `CONFIG_DWARF_VERSION` | 5 | `-g` emits DWARF 5 (rather than stabs), the format `db` reads. |
| `CONFIG_TCC_BACKTRACE`, `CONFIG_TCC_BCHECK`, `CONFIG_TCC_SEMLOCK` | 0 | No `-b` bounds checking, no run-time backtraces, no locking (tcc runs single-threaded). |

Every path is relative to `{B}`. The same configuration therefore works on the host: `xtcc -Bbuild/sysroot/usr/lib/tcc` finds the staged headers and libraries exactly where `/bin/tcc` finds the installed ones.

### The patches to TinyCC

The first three are small and marked with "Eunuchs" comments; the fourth is the rewrite of most of the RISC-V assembler (below).

1. `riscv64-link.c` gains support for the absolute relocations `R_RISCV_HI20`, `R_RISCV_LO12_I` and `R_RISCV_LO12_S` (an address split over `lui` and `addi`/a load/a store), marked with "Eunuchs" comments. tcc never emits them itself, but Homebrew's `libgcc.a` is compiled with `-mcmodel=medlow`, which does, and `libtcc1.a` includes libgcc. The addresses fit because programs are linked at `0x10000`.
2. `riscv64-gen.c` (`gfunc_call`): an argument passed on the stack (the 9th and later) was stored through an lvalue that kept the argument's type, `const` included, so a `const`-qualified argument drew the warning "assignment of read-only location". The slot's type now drops `VT_CONSTANT`. This was a warning only; the code was right.
3. `tccdbg.c` (`tcc_debug_end`): before the `DW_LNE_end_sequence` that closes the line program, the address is advanced to the end of the text. Without it the sequence ended at the last row's address, so a last function written on one line (`int f(void) { return 1; }`) covered no addresses and a debugger could not map its code to a line.
4. The assembler: `tccasm.c`, `tccpp.c`, `tcc.h`, `tcctok.h`, `riscv64-asm.c` and `riscv64-tok.h` ([The assembler](#the-assembler)).

### The sysroot (`build/sysroot`, copied into the image)

| Image path | From | Contents |
|---|---|---|
| `/usr/include/` | `user/include/`, `lib/include/` | The libc headers and the shared `uapi/` headers, exactly what GCC builds against. |
| `/usr/lib/crt1.o` | `user/lib/crt0.S` with `-DNO_GP` | `_start`. tcc's linker defines no `__global_pointer$` and does not relax code to use `gp`, so the tcc build of `_start` leaves `gp` alone. |
| `/usr/lib/crti.o`, `crtn.o` | an empty assembly file | tcc links them; Eunuchs has no `.init`/`.fini` code. |
| `/usr/lib/libc.a` | every `ULIB` object except `crt0.o`, `strip -g` | The libc as an archive (about 0.9 MiB), so tcc links only the members a program uses. GCC-built programs still link the objects directly. |
| `/usr/lib/tcc/libtcc1.a` | tcc's runtime objects, built by `xtcc`, plus every member of GCC's `libgcc.a`, `strip -g` | tcc's helpers (atomics, `alloca`, builtins) and libgcc's soft `long double` arithmetic, which the libc itself needs (`printf`, `strtod`, `math.c`). About 2.5 MiB. |
| `/usr/lib/tcc/runmain.o` | `third_party/tinycc/lib/runmain.c`, built by `xtcc` | Start-up code for `tcc -run`, with its own `exit`/`atexit`. |
| `/usr/lib/tcc/include/` | `third_party/tinycc/include/` | tcc's compiler headers. |
| `/usr/src/*.c`, `/usr/src/esh/`, `/usr/src/edit/`, `/usr/src/Makefile` | `user/bin/`, `user/usr-src/Makefile` | The program sources and their Makefile. tcc's own sources are not included. |

## How it works

### Building it (on the host)

1. **`build/tcc/xtcc`**: `$(HOSTCC) -O1 -w -DONE_SOURCE=1 -Iports/tcc tcc.c`, a host program that compiles for Eunuchs. It exists only to build tcc's runtime, because those files use tcc-specific code GCC does not accept.
2. **`/bin/tcc`**: `tcc.c` compiled by the cross GCC with `UCFLAGS` (warnings off, `-w`) and linked like any other program against `ULIB`. It is in `UPROGS`, so it is stripped and installed like the rest. It is about 330 KiB.
3. **The runtime**: each `third_party/tinycc/lib/*.{c,S}` that is needed is built by `xtcc -B$(TCC_SRC) -Iuser/include -Ilib/include`. `libtcc1.a` is assembled from those objects plus the members extracted from `$(CC) -march=rv64gc -mabi=lp64d -print-libgcc-file-name`.
4. **The sysroot**: headers are copied, `libc.a` is archived from `ULIB`, `crt1.o` is assembled with `-DNO_GP`, and the empty `crti.o`/`crtn.o` are made. `make sysroot` builds just this.
5. **The image**: `FS_FILES` copies every `SYS_FILES` entry to its path minus the `build/sysroot` prefix, and the `USR_SRC` files to `/usr/src`. `build/fs.img` depends on them, so editing a header or a program's source rebuilds the image.

### Compiling on Eunuchs

`tcc -o hello hello.c` preprocesses, compiles and assembles in one pass straight to machine code (there is no intermediate assembly), then links `crt1.o crti.o hello.o libc.a libtcc1.a crtn.o` statically. `-c` and `-o x.o` produce objects; `tcc -ar` creates archives; `-E` preprocesses only. The result is an ordinary static ELF at `0x10000`, loaded by the same `exec` as GCC's output, and it runs with the same libc. Because tcc itself is a normal process, `strace tcc ...` and `profile run tcc ...` work on it.

### `tcc -run`

`tcc -run file.c [args]` compiles into memory and calls `main` without writing a file:

1. tcc links the program in memory with `runmain.o`, which provides `_runmain`, constructors and destructors, and its own `exit`, `atexit` and `on_exit` (that is why libc's `exit`/`atexit` live alone in `exit.c`: the linker then has no reason to pull libc's in and clash).
2. The code and data are placed in memory from tcc's `malloc`, so on the heap, page-aligned. With `CONFIG_RUNMEM_RO`, tcc calls `mprotect` to make the text pages r-x and the data rw-, never both. The kernel's `vma_protect` splits the heap VMA, marks pages that become writable copy-on-write, and runs `fence.i` on every hart for pages that become executable, so the instruction fetch sees the code just written. `do_brk` starts a new heap VMA when the top of the heap has been re-protected.
3. After `main` returns (or calls `exit`), runmain's exit handlers run; stdio registers its flush with `atexit` on first output, so buffered output is written even though libc's own `exit` is not used. tcc then makes the pages rw- again before freeing them.

Without `mprotect` (before this stage) `-run` was impossible: the heap is never executable, and W^X forbids rwx.

### `/bin/make` (`user/bin/make.c`)

A single-file make of about 2,200 lines, aimed at building C programs with tcc. It also builds and runs on macOS (`st_mtimespec` is used for `st_mtim` under `__APPLE__`), which is handy for testing it against GNU make.

- **Makefiles:** rules with several targets and prerequisites, order-only prerequisites (after `|`), tab-indented recipes with `@` (silent), `-` (ignore errors) and `+` (run even with `-n`), `include` and `-include`, conditionals (`ifeq ifneq ifdef ifndef else endif`), `.PHONY`, `.DEFAULT`, `.SILENT`, `.IGNORE`, `.PRECIOUS`, `.SUFFIXES`, `override`.
- **Variables:** `=`, `:=`, `::=`, `+=`, `?=`; `$(V)`, `${V}`, `$V`; command-line `VAR=value` and the environment; substitution references `$(V:.c=.o)`; automatic variables `$@ $< $^ $+ $? $*` and `$(@D) $(@F) $(<D) $(<F)`; `MAKEFLAGS` is exported to sub-makes.
- **Functions:** `wildcard patsubst subst strip findstring filter filter-out sort dir notdir suffix basename addprefix addsuffix join word words firstword lastword foreach if or and shell origin error warning info`.
- **Rules:** pattern rules (`%.o: %.c`), suffix rules (`.c.o:`), and built-in rules for C: `%.o` from `%.c` (`$(COMPILE.c)`), a program from `%.c` (`$(LINK.c)`) or from `%.o`, and `%.o` from `%.S`. Built-in variables: `CC = tcc`, `AR = tcc -ar`, `ARFLAGS = rcs`, `RM = rm -f`, `SHELL = /bin/sh`, empty `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, `LDLIBS`. Built-in rules are tried after the Makefile's own.
- **Timestamps** are nanoseconds (`st_mtim`), so a file written in the same second as its prerequisite is still seen as newer or older correctly.
- **Execution:** each recipe line runs through `/bin/sh -c` on its own, as POSIX make does.
- **Options:** `-f FILE`, `-C DIR`, `-n`, `-s`, `-i`, `-k`, `-B`, `-p`, `-q`, `-d` (explains why each target is remade); `-j` is accepted and ignored.

### `/usr/src`

`/usr/src/Makefile` builds every top-level `.c` file as a program and `db/`, `edit/` and `esh/` as multi-file programs, all into `/usr/src/bin` with `CC = tcc` and `CFLAGS = -Wall`. `make CFLAGS=-g` builds them with DWARF, so `db` can debug them at source level (the copies in `/bin` are GCC builds stripped of DWARF). `make bin/ls` builds one program; `make install` copies the results over `/bin` (as root); `make clean` removes `bin`. A full rebuild of every program takes about 6 seconds in the guest. `cp` overwrites in place, so the hard links (`/bin/vi`, the `eutils` names) keep pointing at the new files, and replacing a running program (including `cp`'s own `eutils`) is safe because `exec` reads the whole file into memory.

### Building from the Mac's sources

Under `make run` the project directory is mounted on `/mnt/host` ([06](06-filesystems.md#9pfs-host-directories)), so the guest can build the live tree instead of the copy in `/usr/src`:

```
cd /mnt/host/user/bin && make -f /usr/src/Makefile BIN=/tmp/bin
```

builds every program from the current Mac sources in about 7 seconds (the `/usr/src` Makefile's wildcards pick up `*.c` and the `db/`, `edit/`, `esh/` directories from the current directory). With `BIN` under `/mnt/host`, the results appear on the Mac. Edit on the Mac, build and test on Eunuchs, without rebuilding the image.

### The assembler

tcc assembles `.S` files (after running its preprocessor on them) and inline `asm` statements with its own assembler; its C code generator emits machine code directly and never goes through it, so ordinary C was never affected by the assembler's bugs. As vendored, the RISC-V assembler could not handle our assembly and silently produced wrong code for several instructions. It now assembles every `.S` file in the tree (`kernel/arch/riscv/{boot,fp,safe,switch,trapentry,uaccess}.S`, `user/lib/{crt0,setjmp,syscall}.S`) to the same bytes as GNU as after linking, in `xtcc` on the Mac and in `/bin/tcc` on Eunuchs (same source; checked in the guest by writing objects to the 9P share and comparing them on the Mac). This is step 1 of self-hosting.

**Directives added:**
- `.macro NAME params` … `.endm`: parameters with `=default`, comma-separated arguments; `\param` substitution is textual (so `x\n` pastes to `x1`), `\()` separates a parameter from following text, and unknown `\names` pass through (so a `.irp` inside a macro works). Macros are forgotten at the end of each file.
- `.irp`, `.dword` (= `.quad`), `.equ` (= `.set`).
- Data sizes as in gas for RISC-V: `.word` is 4 bytes (vendored tcc made it 2, as on x86, so the `.word 0` meant as a 4-byte illegal instruction in `kernel/debug/monitor.c` and `user/bin/segv.c` was a different instruction when built with tcc), `.half` and `.short` 2, and the explicit `.2byte`, `.4byte`, `.8byte` (tcc's lexer reads `.2byte` as a number, so `tcc_assemble_internal` recognizes those spellings itself), besides `.byte`, `.long`, `.int`, `.quad`.
- `.option arch, …` (accepted and ignored: all of RV64GC is always available); `.option pic`/`nopic` and `push`/`pop` (tracked; they change `la`).
- `.section NAME, "flags", @type` honours the types (`@nobits`, `@note`, `@init_array`, …); with no flags string it infers them as gas does (`.text*` `ax`; `.data*`, `.sdata*`, `.bss*`, `.sbss*` `aw`; `.bss*`/`.sbss*` nobits).
- `.cfi_*` directives are accepted and **ignored**: tcc writes no `.eh_frame` for assembly.
- Backslashes are allowed in assembly source.

**Registers and CSRs:** the alias `fp` for `s0`; every standard S-mode and M-mode CSR by name (`sstatus`, `sepc`, `scause`, `stval`, `sip`, `sie`, `stvec`, `sscratch`, `satp`, `stimecmp`, `mstatus` … `mhartid`, …). An unknown CSR name is now an error; it used to encode CSR 0.

**Instructions added:** `sret`, `mret`, `unimp`, `sfence.vma [rs1[, rs2]]`, `fence.tso`, `flw`/`fsw`, `fmsub`/`fnmsub`/`fnmadd` (`.s`/`.d`), `fsgnjn`/`fsgnjx`, `fmv.x.w`/`fmv.w.x`/`fmv.x.d`/`fmv.d.x`, an optional rounding mode (`rne rtz rdn rup rmm dyn`) on `fadd`/`fsub`/`fmul`/`fdiv`/`fsqrt`/the fused forms and `fcvt`, `fscsr`/`fsrm`/`fsflags` with one or two operands, `jalr rd, rs, imm`, `jalr imm(rs)`, `jr imm(rs)`, `jump sym, temp`, and loads and stores of a symbol (`ld rd, sym`; `sd rs, sym, temp`).

**Pseudo-instructions now mean what they mean in gas:**
- `la` is `lla` (`auipc` + `addi`, `R_RISCV_PCREL_HI20`/`LO12_I`) unless `.option pic` is in effect, when it loads from the GOT with `ld` (it used `lw`, truncating addresses).
- `li` emits gas's minimal sequence (the same constant-loading algorithm); it always used 8 instructions.
- `call` is `auipc ra` + `jalr ra` with `R_RISCV_CALL_PLT`; it was `jalr zero` (so a call never returned) with stray GOT relocations.
- Branches and jumps to labels emit `R_RISCV_BRANCH`/`R_RISCV_JAL`, or are resolved on the spot when the label is local, already defined and in the same section.

**Bugs fixed that made silently wrong code:** a branch to any label became an inverted branch plus an `auipc t0` with no `jalr`; B-type `imm[10:5]` was masked to 5 bits (backward branches and branches of 1 KiB or more were wrong); `j`/`jal` to a `.globl` label was rejected; `srl` was encoded as `xor`, `sraw` as `srlw`, `sraiw` as `srliw`; `bgez` and `blez` were swapped; `lr.*` was encoded as `amoand`, `sc.*` and `amoor` as `amominu`; `fence iorw` lost the i/o bits; `fcvt.d.w`, `fcvt.d.wu` and `fcvt.d.s` had an invalid rounding field; `csrs`/`csrc` and friends with CSR names used CSR 0; the `jal` range check was off by one.

**Compressed instructions** were rewritten: gas syntax (`c.lw rd', off(rs1')`, `c.lwsp rd, off(sp)`, …), correct encodings (`c.sw`/`c.sd` had their register fields swapped, `c.lui` was mis-scaled), labels on `c.j`/`c.beqz`/`c.bnez` (`R_RISCV_RVC_JUMP`/`RVC_BRANCH`), range checks, and the RV32-only `c.flw`, `c.fsw`, `c.flwsp`, `c.fswsp` and `c.jal` rejected (on RV64 those encodings mean `c.ld`, `c.sd`, `c.ldsp`, `c.sdsp` and `c.addiw`).

**Still different from gas:**
- No `%hi()`, `%lo()`, `%pcrel_hi()`/`%pcrel_lo()` operators: use `la`/`lla`, `li` and the symbol forms of loads and stores.
- No automatic compression (only explicit `c.*` instructions are 2 bytes) and no linker relaxation, so tcc's objects are bigger than gas's default output (the comparison test assembles with `-march=rv64g -mno-relax` for that reason).
- No unwind tables from `.cfi_*`, so GDB and `db` cannot unwind through assembly functions in a tcc-built object by CFI (frame pointers still work where the code keeps them).
- No `.irpc`, no `\@` in macros, no `:req`/`:vararg` parameters, no whitespace-separated macro arguments.

**Tests:** `tests/host/test_tcc_asm.py` compares tcc with GNU as on our files, a directives file and thousands of random instructions ([11](11-build-and-testing.md#host-tests-make-test-host)); the `host share (9p)` stage of `make test` assembles and runs `tests/guest/asmtest.S` with tcc on Eunuchs.

**Debugging an assembly difference:** assemble the file with both (`build/tcc/xtcc -c -o a.o f.S`, `riscv64-elf-gcc -c -march=rv64g -mabi=lp64d -mno-relax -o b.o f.S`, with the same `-D`/`-I` options), link each at a fixed address (`riscv64-elf-ld -Ttext=0x80000000`, `--defsym` for undefined symbols), and compare the bytes, or `riscv64-elf-objdump -d` both and `diff`: that is what the test does.

### Dialect problems tcc exposed in our code

tcc accepts most of GNU C, but these differences had to be worked around in the libc and the programs:

- **Inline assembly**: tcc saves and restores every callee-saved register an `asm` statement clobbers, around that statement, so values cannot be passed between `asm` statements in such registers (`utests`' FP tests load and read `fs` registers within one statement). Numeric local labels (`1:` with `1f`/`1b`) used to be unreliable; they work since the assembler work (tested in `tests/guest/asmtest.S`). `fld`/`fsd` do not accept `"m"` operands: pass the address in a register.
- **Predefined macros**: tcc predefines `__linux__` but has no `__builtin_nan`, `__builtin_inf` or `__builtin_huge_val`, so `math.h` has a `__TINYC__` branch that defines `HUGE_VAL`, `INFINITY` and `NAN` with constant expressions (`1e500`, `0.0f / 0.0f`).

## Concurrency and locking

None of its own: tcc and make are ordinary single-threaded processes. Two builds in the same directory race on the same files exactly as they would on Linux.

## Interfaces

### Commands

| Command | Usage |
|---|---|
| `tcc` | `tcc [options] file...`: `-o`, `-c`, `-E`, `-I`, `-D`, `-U`, `-L`, `-l`, `-B`, `-Wall`, `-w`, `-v`/`-vv` (show the search paths and files used), `-run file.c [args]`, `-ar`. See `tcc -h`. |
| `make` | `make [-f FILE] [-C DIR] [-nsikBpqd] [VAR=value] [target...]` |

### Make targets and files on the host

| Target / file | What |
|---|---|
| `make sysroot` | Builds `build/sysroot` (headers, `libc.a`, start-up files, `libtcc1.a`, `runmain.o`). |
| `build/tcc/xtcc` | The host cross compiler. |
| `build/user/bin/tcc` | `/bin/tcc` with symbols (for GDB and the profiler). |

### Kernel interfaces it depends on

`mprotect` (226) for `-run`; `utimensat` is not needed by tcc but `touch` uses it to drive `make`.

## Debugging and observability

- **See what tcc does:** `tcc -vv -o x x.c` prints the include and library search paths and every file it opens; `strace tcc ...` shows the same at the system call level (and the `mprotect` calls of `-run`).
- **Reproduce on the host:** compile the same file with `build/tcc/xtcc -Bbuild/sysroot/usr/lib/tcc -o x x.c`. Same compiler, same configuration, same headers and libraries, but with the host's debugger and sanitizers available for tcc itself. On the guest, `db` can step through a tcc-built program (`tcc -g`) to see what the generated code does. Inspect the output with `riscv64-elf-objdump -d x` and `riscv64-elf-readelf -a x`.
- **A wrong-code suspicion:** compile the file with both GCC (the normal build) and `xtcc` and compare behaviour; disassemble the tcc output around the fault. The kernel's fault report symbolizes tcc-built programs too (they keep a symbol table).
- **`tcc -run` faults:** a SIGSEGV on the first instruction of generated code means the page was not made executable (check `/proc/<pid>/maps`: the heap is split into r-x and rw- VMAs while the program runs).
- **make:** `make -d` explains why each target is remade; `make -n` prints the commands; `make -p` prints the variables and rules.
- **Tests:** the `toolchain and editors` stage of `make test` compiles and runs a program with `tcc -o` and with `tcc -run`, and rebuilds `cat` from `/usr/src` with make.

## Limitations and known issues

- **Debug information is DWARF 5 only**, read by `db`; kernel fault reports and the monitor's `ubt` still show symbols only, not source lines.
- **No shared libraries**: everything is linked statically (`-static` is forced), so every program carries the libc members it uses.
- **Code quality**: tcc does almost no optimization. tcc-built programs (`utests` and `esh` compile cleanly) are noticeably slower than GCC's `-O1` builds, so the image keeps the GCC-built binaries.
- **`tcc -run` needs `mprotect`** and places code in the heap; there is no fallback mode.
- **Self-hosting, measured** (building from `/mnt/host`):
  - **tcc compiles itself** on Eunuchs (`tcc.c` with `ONE_SOURCE`), but its sources are not in `/usr/src`, so that needs the share.
  - **The libc**: every `user/lib/*.c` compiles with tcc, and since the assembler work its `.S` files assemble too, to the same bytes as GNU as. Building the whole libc and `/bin` with tcc in the guest is the next step.
  - **The kernel**: 46 of its 76 C files compile with tcc. The blockers: no `stdint.h` for the freestanding kernel build (tcc's headers expect a libc's); the `__ATOMIC_*` constants; the inline-asm `"K"` constraint (a 5-bit CSR immediate); the assembler gaps (`.macro`, `la`, the `fp` register alias, `.dword`, numeric local labels, `.option arch`), now closed: the kernel's `.S` files assemble identically; and a tcc assertion failure in its floating-point register allocator on `kernel/tests/test_debug.c`. Above all, linking: the kernel's linker script (a higher-half VMA different from the load address, page-aligned regions, `KEEP`ed tables with start and end symbols, `NOLOAD` stacks), which tcc's linker cannot express, and the two-pass link whose symbol table is generated by `tools/gen_ksyms.py`, a Python script.
- **`make`**: no parallel builds (`-j` is ignored), no archive member syntax (`lib.a(obj.o)`), no `$(eval)`/`$(call)`/`define`; `vpath` and `unexport` lines are accepted and ignored.
- **Size**: the sysroot adds about 4 MiB to the 32 MiB image (`libtcc1.a` is most of it, because all of libgcc is included).

## Potential improvements

1. **Line numbers in fault reports** — Have the kernel (or a user-space helper run on a fault) map pcs to file:line through the DWARF line table that `db` already decodes, for programs built with `-g`. Impact: medium. Effort: medium.
2. **Self-hosting, step by step** — (a) **done**: tcc's assembler handles every `.S` file in the tree, byte-identical to GNU as; (b) build the whole libc and every program in `/bin` with tcc on Eunuchs, and run the test suite against them; (c) put `third_party/tinycc` in `/usr/src/tcc` with a Makefile, and check that the second generation is byte-identical to the first; (d) the kernel's C: a freestanding `stdint.h`, `__ATOMIC_*` definitions, the `"K"` constraint, a fix for the register-allocator assertion; (e) a link path without GNU ld: teach tcc's linker the few script features the kernel needs (separate VMA and LMA, alignment, keep and start/end symbols, NOLOAD) or have the kernel relocate itself from a flat image, and rewrite `gen_ksyms` in C. Smaller assembler gaps: `%hi`/`%lo`/`%pcrel_*`, automatic compression, `.cfi` unwind tables, `.irpc`, `\@`. Impact: medium. Effort: large.
3. **`-run` without `mprotect`** — A fallback that writes the program to `/tmp` and `exec`s it, for kernels or environments without `mprotect`. Impact: low. Effort: small.
4. **`make -j`** — Run independent recipes in parallel with `fork` and `wait`; Eunuchs has four CPUs by default. Impact: medium. Effort: medium.
5. **Trim `libtcc1.a`** — Include only the libgcc members that are actually needed (soft `long double`, a few integer helpers) instead of all of them. Impact: low. Effort: small.
6. **More of the GNU make language** — `define`, `$(call)`, `$(eval)`, `vpath`, archive members, so more third-party Makefiles work unchanged. Impact: medium. Effort: medium.
