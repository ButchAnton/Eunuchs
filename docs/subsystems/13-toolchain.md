# The native toolchain: tcc and make

Eunuchs can compile C programs on itself. `/bin/tcc` is TinyCC, a small one-pass C compiler, assembler and linker, built for RISC-V 64 and linked against the Eunuchs libc. The disk image carries what it needs: the headers in `/usr/include`, the C library and start-up files in `/usr/lib`, and tcc's own runtime in `/usr/lib/tcc`. `/bin/make` is a small POSIX-style make written for Eunuchs, and `/usr/src` holds the sources of every program in `/bin` with a Makefile, so the system can rebuild its own user space:

```
root@eunuchs:/# cd /usr/src && make && make install
```

Since the self-hosting work, Eunuchs also builds its own kernel, linked by `kld`, a linker for the kernel's linker script.

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
| `tools/kld/kld.c` | `kld`, the kernel linker (host `build/tools/kld`, guest `/bin/kld`). |
| `user/build.mk`, `kernel/build.mk` | User space and the kernel, built on Eunuchs by tcc (and kld). |
| `tools/selfhost.sh`, `tools/selfhost.py` | `make selfhost`: the two stages in the guest, and their driver on the Mac. |

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

Most are small and marked with "Eunuchs" comments; the fourth is the rewrite of most of the RISC-V assembler (below).

1. `riscv64-link.c` gains support for the absolute relocations `R_RISCV_HI20`, `R_RISCV_LO12_I` and `R_RISCV_LO12_S` (an address split over `lui` and `addi`/a load/a store), marked with "Eunuchs" comments. tcc never emits them itself, but Homebrew's `libgcc.a` is compiled with `-mcmodel=medlow`, which does, and `libtcc1.a` includes libgcc. The addresses fit because programs are linked at `0x10000`.
2. `riscv64-gen.c` (`gfunc_call`): an argument passed on the stack (the 9th and later) was stored through an lvalue that kept the argument's type, `const` included, so a `const`-qualified argument drew the warning "assignment of read-only location". The slot's type now drops `VT_CONSTANT`. This was a warning only; the code was right.
3. `tccdbg.c` (`tcc_debug_end`): before the `DW_LNE_end_sequence` that closes the line program, the address is advanced to the end of the text. Without it the sequence ended at the last row's address, so a last function written on one line (`int f(void) { return 1; }`) covered no addresses and a debugger could not map its code to a line.
4. The assembler: `tccasm.c`, `tccpp.c`, `tcc.h`, `tcctok.h`, `riscv64-asm.c` and `riscv64-tok.h` ([The assembler](#the-assembler)).
5. `tccgen.c`: narrowing a 64-bit value held in a register to `unsigned int` zero-extended it. RISC-V keeps 32-bit values sign-extended in registers, unsigned ones included (the psABI, and tcc's own `lw`, `addw` and comparison code assume it), so a value narrowed this way compared unequal to the same value loaded from memory: `sleep_ms() == (unsigned)-1` was false in tcc-built code (the `utests` `eintr` test failed). It now emits `sext.w`.
6. `tccelf.c`: executables kept `.symtab` and `.strtab` only with `-g` (upstream writes non-allocated sections only then). They are now always kept, as GNU ld does, because the kernel's user backtraces and `elf_symbolize` need them (`ktest elf_load_from_disk` failed on a tcc-built `/bin/hello`). This applies to every program built on Eunuchs.
7. **Fixes found building the kernel** (most were silent miscompilations; `riscv64-gen.c`, `riscv64-asm.c`, `tccgen.c`, `tccpp.c`, `tcc.h`, `include/stdatomic.h`):
   - **The epilogue** did `addi sp, s0, …; ld ra, -8(s0); ld s0, -16(s0)`: it read the saved `ra` and `s0` from below `sp` after freeing the frame. An interrupt at that point (the kernel's trap frame and backtrace record land just below `sp`), or a signal frame in user space, overwrote the saved `ra` with `sepc`, so the function returned to its own epilogue forever. It showed up as cross-CPU wakeups taking a whole 10 ms tick and `cp` running 50 times slower on two CPUs, and was found by sampling the CPUs' registers through the QEMU monitor. Now `ld ra; addi t0, s0, …; ld s0; mv sp, t0; ret`.
   - **`<stdatomic.h>`'s `__atomic_store_n`** stored twice: a plain `*ptr = val`, then the atomic store. A spinlock unlock released the lock, another CPU took it, and the second store released it again (`unlock of spinlock not held`). Now a single store; `__atomic_exchange_n` was added.
   - **Register variables** (`register T x asm("reg")`): the hardware register number was taken as tcc's internal register number (`a0` = 10 became `fa2`, an FP register in the kernel; `s1` crashed tcc with an assertion in its FP register allocator). The variable now lives on the stack, and asm operands that name it use the register; values move directly between the slot and registers outside `a0`–`a7`/`fa0`–`fa7`. `s0` is refused.
   - **Inline asm**: the `"K"` constraint (a 5-bit unsigned immediate) is supported, and the `"r"`/`"f"` allocation loops no longer run one past `a7`/`fa7` into `s2`/`fs2`. The assembler accepts `csrw`/`csrs`/`csrc` with a 5-bit immediate (as `csrrwi`/`csrrsi`/`csrrci`, as gas does).
   - **Constants**: `(void *)0x80200000UL` was sign-extended (the zero-extension of 32-bit unsigned constants applied to `long long` but not to pointers).
   - **Symbol names**: anonymous symbols (the labels of `auipc` pairs) were named `<no name>` in the symbol table (thousands of them, and kernel backtraces showed `<no name>`); now empty.
   - **`parse_fence_operand`** subtracted pointers into two different `"iorw"` string literals (a bug from the assembler work): GCC merges identical literals, tcc does not, so the tcc-built tcc assembled `fence rw,rw` as `fence 0,0`. Found because stage 1 and stage 2 differed.
   - **`SOURCE_DATE_EPOCH`** sets `__DATE__` and `__TIME__` (in UTC), as with GCC, for reproducible builds (the kernel's banner contains its build time).

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
| `/usr/src/eunuchs/` | `EU_SRC`: the kernel, libc, programs, tcc and kld sources and the build scripts | Everything needed to rebuild the system on Eunuchs (see below). |
| `/usr/src/*.c`, `/usr/src/esh/`, `/usr/src/edit/`, `/usr/src/Makefile` | `user/bin/`, `user/usr-src/Makefile` | The program sources and their Makefile. tcc's own sources are not included. |

## How it works

### Building it (on the host)

1. **`build/tcc/xtcc`**: `$(HOSTCC) -O1 -w -DONE_SOURCE=1 -Iports/tcc tcc.c`, a host program that compiles for Eunuchs. It exists only to build tcc's runtime, because those files use tcc-specific code GCC does not accept.

   **Limitation:** `xtcc` stores `long double` constants with only double precision (the low 64 bits of the 128-bit value are zero). On Apple Silicon the host's `long double` is 64 bits, and tcc folds constants in the host type; `/bin/tcc` on Eunuchs keeps full precision. So building `user/build.mk` with `TCC=$PWD/build/tcc/xtcc` on the Mac gives a different `math.o`/`libc.a`, `utests` and `tcc` than the guest build. The regular build uses `xtcc` only for tcc's runtime helpers, which have no such constants, so nothing shipped is affected.
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

### Eunuchs builds its own user space (`user/build.mk`, `make selfhost`)

`user/build.mk` is a Makefile for Eunuchs' own `make`, run in the guest from the shared tree:

```
cd /mnt/host && make -f user/build.mk [OUT=dir] [TCC=compiler]
```

Nothing in it comes from GCC:

1. **The sysroot**, in `OUT/sysroot/usr/lib` (`OUT` defaults to `build/selfhost`): `crt1.o` (`crt0.S` with `-DNO_GP`), empty `crti.o`/`crtn.o` (assembled from standard input, so the file name they record is `-` whatever `OUT` is), and `libc.a` from `user/lib/*.c`, `setjmp.S`, `syscall.S`, `lib/printf.c` and `lib/string.c` with `-DFMT_FLOAT`.
2. **tcc's runtime**, in `OUT/sysroot/usr/lib/tcc`: `libtcc1.a` from tcc's own `lib-arm64.c` (the software `long double` arithmetic), `stdatomic.c`, `builtin.c`, `armflush.c`, `atomic.S` and `alloca.S`, and `runmain.o`. Unlike the regular build's `libtcc1.a`, it contains no libgcc.
3. **Every program**, into `OUT/bin`: each `user/bin/*.c`, `db`, `edit`, `esh`, `tcc` itself (`third_party/tinycc/tcc.c` with `-DONE_SOURCE=1 -Iports/tcc`), and `kld` (`tools/kld/kld.c`).

Headers come from the source tree (`-nostdinc -Iuser/include -Ilib/include -Ithird_party/tinycc/include`), not from `/usr/include`; links use `-B OUT/sysroot/usr/lib/tcc`; archives are made with `tcc -ar`. `make` runs serially.

**`make selfhost`** (on the Mac, `tools/selfhost.py`) proves it works:

1. It boots Eunuchs from a copy of the image (`build/selfhost/boot.img`) with the repository shared on `/mnt/host`.
2. **Stage 1**: `build.mk` with the image's `/bin/tcc` (which GCC built), into `build/selfhost/stage1`.
3. **Stage 2**: `build.mk` again with stage 1's `tcc`, into `build/selfhost/stage2`.
4. Every file of the two stages must be **byte-for-byte identical**: tcc built by tcc builds the same user space as tcc built by GCC, a bootstrap fixed point.
5. The Makefile makes `build/selfhost/fs.img`: the normal image with every `/bin` program and the six libraries (`crt1.o`, `crti.o`, `crtn.o`, `libc.a`, `libtcc1.a`, `runmain.o`) replaced by stage 2's (`SH_FILES`, built by the `sh_file` function from `FS_FILES`; the headers are unchanged), and runs the whole `run_tests.py` scenario on it (`--programs build/selfhost/stage2/bin`).

Both stages take about 37 seconds in the guest with 4 CPUs, boot included; the stages are identical and all 19 test stages pass on the self-built image. Logs: `build/selfhost/stage1.log`, `stage2.log` and `console.log`.

The libc needed one change for this: `math.c` and `stdlib.c` no longer use GCC's floating-point builtins, which tcc lacks. They use `HUGE_VAL` and `NAN` (cast to `long double` where needed) instead of `__builtin_inf`/`infl`/`nan`/`nanl`, and inline-assembly helpers `hw_sqrt`/`hw_sqrtf` (`fsqrt.d`/`fsqrt.s`) instead of `__builtin_sqrt`/`sqrtf`, so both compilers compile the same code.

### kld, the kernel linker (`tools/kld/kld.c`)

tcc's linker cannot follow a linker script, and the kernel needs one (`kernel/arch/riscv/kernel.ld`: a higher-half VMA with a different load address, page-aligned regions, `KEEP`ed tables with start and end symbols, `NOLOAD` stacks). `kld` (about 1,600 lines of C) is a static linker for RISC-V ELF64 relocatable objects, from GCC or tcc, that reads that script itself. It builds on the Mac (`build/tools/kld`, `$(KLD)`) and on Eunuchs (`/bin/kld`, built by `user/build.mk` and installed in the self-hosted image).

```
kld -T script -o kernel.elf [--ksyms] [-M] file.o ...
```

- **The script subset**: `ENTRY`, `OUTPUT_ARCH`, `sym = expr;` and `PROVIDE(sym = expr);` anywhere, `SECTIONS` with `. = expr;`, output sections with `(NOLOAD)` and `AT(expr)`, input statements `*(pattern …)` with `*` wildcards, `KEEP(…)`, and `COMMON`. Expressions: numbers (with `K`/`M`), symbols, `.`, `ALIGN(n)`, `+ - * /` and parentheses.
- **Placement**: the first matching statement takes an input section, as in GNU ld; an allocated input section no statement matches is an error; `.eh_frame`, `.note*`, `.comment` and `.riscv.attributes` are dropped. `AT()` sets the difference between the VMA and the LMA, which carries over to the following sections (GNU ld's rule).
- **Output**: one `PT_LOAD` per allocated output section, the symbol table (locals and globals), and the debug sections with their relocations applied.
- **Relocations**: `32`/`64`, `BRANCH`, `JAL`, `CALL`/`CALL_PLT`, `GOT_HI20`, `PCREL_HI20`/`PCREL_LO12_I`/`S`, `HI20`/`LO12_I`/`S`, `ADD`/`SUB` 8 to 64, `SUB6`, `SET6`/`8`/`16`/`32`, `32_PCREL`, `SET_ULEB128`/`SUB_ULEB128`, `RVC_BRANCH`/`RVC_JUMP`, with range checks. `ALIGN` and `RELAX` are ignored. There is no relaxation and no string merging. tcc reaches globals through the GOT (`auipc` + `ld`); kld rewrites that to `auipc` + `addi`, so there is no GOT. Padding from `R_RISCV_ALIGN` that only relaxation could remove is an error ("build with -mno-relax"), so GCC objects must be compiled with `-mno-relax` to be linked by kld.
- **`--ksyms`** synthesizes the `.ksyms` section (`ksyms_num`, `ksyms_addrs`, `ksyms_offs`, `ksyms_names`) itself, byte-identical to what `tools/gen_ksyms.py` makes from a first link (checked on 929 entries). It lays the image out twice and checks that the text did not move. One link instead of two, and no Python. (Both skip tcc's `L..` local labels.)
- **`-M`** prints a link map.

The GCC-built kernel objects (with `-mno-relax`) linked by kld pass the whole test suite, which validates kld independently of tcc.

### Eunuchs builds its own kernel (`kernel/build.mk`)

```
cd /mnt/host && make -f kernel/build.mk [OUT=dir] [TCC=compiler] [KLD=linker]
```

compiles every `.S` and `.c` file of the kernel's directories (the same `KDIRS` as the Makefile, sorted per directory) with tcc (`-nostdinc -Ikernel/include -Ilib/include -Ithird_party/tinycc/include`, plus `-D__ASSEMBLY__` for `.S`), adds tcc's runtime `atomic.S`, `stdatomic.c` and `builtin.c` (tcc turns `__atomic_*` operations and `__builtin_ctz` and friends into calls), and links with `kld -T kernel/arch/riscv/kernel.ld --ksyms` into `OUT/kernel/kernel.elf`. It runs after `user/build.mk`, which builds `kld`.

Changes to the kernel for tcc (the same code under GCC):
- `stdint.h` moved from `user/include` to `lib/include`, shared by the kernel and the libc (GCC's freestanding one is not available to tcc). It includes the host's (`#include_next`) when not compiling for RISC-V, for the host tests.
- `kernel/include/types.h`, under `__TINYC__`: `#include <stdatomic.h>` for the `__ATOMIC_*` orders and `__atomic_*_n`, and `__sync_synchronize()` as `fence rw, rw`.
- `fdt.c`'s `be32` reads bytes instead of `__builtin_bswap32`, which without Zbb is a libgcc call (`__bswapsi2`). The GCC kernel no longer links `-lgcc` at all.
- `current_fp()` is a macro: as an inline function it was a frame of its own under tcc, which does not inline, and backtraces lost a frame.
- `kernel.ld` places `.data.ro` and `.data.ro.*` (tcc's read-only data) in `.rodata`.
- `exec` accepts up to 2048 argument and environment pointers in 128 KiB, with the vectors allocated rather than on the kernel stack ([05](05-processes-and-signals.md)): the kernel's `kld` command line has about 85 arguments. The terminal's line limit (255 bytes, `LINE_MAX` in `tty.c`) is why the build runs from a script, `tools/selfhost.sh`, rather than one typed command.

**`make selfhost`** builds both user space and the kernel in each stage, **from the image's own sources, without the Mac**: in the guest it runs `cd /usr/src/eunuchs && SOURCE_DATE_EPOCH=<last commit> SH_OUT=/tmp/selfhost sh tools/selfhost.sh` (on the guest's own disk; `SOURCE_DATE_EPOCH` is the last commit's time, `git log -1 --format=%ct`, so `__DATE__`/`__TIME__` are reproducible), and only then uses the share, to hand the results back: `cp -r /tmp/selfhost/stage1 /tmp/selfhost/stage2 /tmp/selfhost/*.log /mnt/host/build/selfhost`. `selfhost.sh` runs `user/build.mk` then `kernel/build.mk`, with `/bin/tcc` for stage 1 and stage 1's `tcc` for stage 2, and exits 1 or 2 if that stage failed. The two stages are byte-identical, kernel included. The Makefile then boots **stage 2's kernel** (`build/selfhost/stage2/kernel/kernel.elf`) with `build/selfhost/fs.img` (stage 2's programs and libraries, plus `/bin/kld`, and the source tree in `/usr/src/eunuchs`, so a self-built system can rebuild itself again) and runs the whole test scenario: 19 of 19 stages pass. Both stages build in about 70 seconds in the guest (faster than reading the sources over 9P).

### The source tree in the image (`/usr/src/eunuchs`)

The image carries everything needed to rebuild the system, at `/usr/src/eunuchs/<same path as in the repository>` (`EU_SRC` in the Makefile, 294 files; `build/fs.img` depends on them): `kernel/`, `lib/`, `user/include`, `user/lib` and `user/bin` (`*.c *.h *.S *.ld`), `kernel/build.mk`, `user/build.mk`, `ports/tcc/config.h`, `tools/kld/kld.c`, `tools/selfhost.sh`, and TinyCC's top-level `*.c *.h *.def`, `include/*.h`, `lib/*.c`, `lib/*.S` and `COPYING` (tcc is LGPL). The older `/usr/src` (the program sources and their Makefile) is unchanged. This is why the default image grew to 64 MiB (`FS_MB`; about 13.4 MB used by 436 files).

To rebuild everything on Eunuchs alone (it needs about 20 MB free):

```
cd /usr/src/eunuchs && sh tools/selfhost.sh
```

The results are in `build/selfhost/` under that directory (`SH_OUT` overrides it): `stage1/` and `stage2/`, each with `sysroot/`, `bin/` and `kernel/kernel.elf`, and the two logs. Install them by hand (`cp` the programs over `/bin`, the libraries over `/usr/lib`); booting the new kernel needs the Mac, since QEMU loads the kernel from a host file. The edit-on-the-Mac, build-in-the-guest workflow still works: run the same script in `/mnt/host`.

**`make tcc-kernel`** does the same on the Mac with `xtcc` and the host `kld`, into `build/tcc-kernel/kernel/kernel.elf`: the quickest way to reproduce a problem in the tcc-built kernel (boot it with `make run`-style options and `-kernel` pointing there, or debug it with GDB).

**Speed**: tcc does almost no optimization, and its kernel runs at about half the speed of GCC's `-O1` kernel (a tcc compile takes 0.59 s against 0.31 s, `cp` 0.23 s against 0.13 s, `ls -l` 0.06 s against 0.03 s), hence `SH_TIMEOUT` (120 s per stage) for its test run. The normal image keeps the GCC kernel.

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
| `build/tools/kld` | The kernel linker, on the host. |
| `make tcc-kernel` | The kernel compiled by `xtcc` and linked by `kld`: `build/tcc-kernel/kernel/kernel.elf`. |
| `make selfhost` | Both self-hosting stages in the guest, then the test suite on stage 2's kernel and programs. |
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
  - **User space: done.** Step 1 (the assembler) and step 2 (`user/build.mk`, above): Eunuchs builds its C library, tcc's runtime and every program, tcc included, and the second generation is identical to the first.
  - **The kernel: done** (step 3, above): built by tcc and linked by kld on Eunuchs, a byte-identical fixed point, and it passes the test suite. Before the kernel work, 46 of its 76 C files compiled with tcc. The blockers: no `stdint.h` for the freestanding kernel build (tcc's headers expect a libc's); the `__ATOMIC_*` constants; the inline-asm `"K"` constraint (a 5-bit CSR immediate); the assembler gaps (`.macro`, `la`, the `fp` register alias, `.dword`, numeric local labels, `.option arch`), now closed: the kernel's `.S` files assemble identically; and a tcc assertion failure in its floating-point register allocator on `kernel/tests/test_debug.c`. Above all, linking: the kernel's linker script (a higher-half VMA different from the load address, page-aligned regions, `KEEP`ed tables with start and end symbols, `NOLOAD` stacks), which tcc's linker cannot express, and the two-pass link whose symbol table is generated by `tools/gen_ksyms.py`, a Python script.
- **The self-built kernel has no DWARF** (it is compiled without `-g`); kld's handling of debug-section relocations has been exercised with GCC's DWARF only.
- **kld does not relax**, so GCC objects need `-mno-relax`.
- **A rebuilt kernel cannot boot itself**: QEMU takes the kernel from a file on the Mac (`-kernel`); there is no boot loader that reads it from the guest's disk.
- **`make`**: no parallel builds (`-j` is ignored), no archive member syntax (`lib.a(obj.o)`), no `$(eval)`/`$(call)`/`define`; `vpath` and `unexport` lines are accepted and ignored.
- **Size**: the image is 64 MiB, with about 13.4 MB used. The sysroot is about 4 MiB of that, mostly `libtcc1.a`, because the regular build includes all of libgcc (the self-built one shows that is unnecessary).

## Potential improvements

1. **Line numbers in fault reports** — Have the kernel (or a user-space helper run on a fault) map pcs to file:line through the DWARF line table that `db` already decodes, for programs built with `-g`. Impact: medium. Effort: medium.
2. **After self-hosting** — Steps 1 (the assembler), 2 (user space) and 3 (the kernel, with kld) are done. Next: build the kernel with `-g` and check kld's debug relocations with tcc's DWARF; relaxation in kld (smaller, faster GCC-built kernels linked by kld); a longer terminal line (`LINE_MAX` is 255); drop libgcc from the regular `libtcc1.a`; and a better code generator, since the tcc kernel runs at half speed. Smaller assembler gaps: `%hi`/`%lo`/`%pcrel_*`, automatic compression, `.cfi` unwind tables, `.irpc`, `\@`. Impact: medium. Effort: large.
3. **`-run` without `mprotect`** — A fallback that writes the program to `/tmp` and `exec`s it, for kernels or environments without `mprotect`. Impact: low. Effort: small.
4. **`make -j`** — Run independent recipes in parallel with `fork` and `wait`; Eunuchs has four CPUs by default. Impact: medium. Effort: medium.
5. **Trim `libtcc1.a`** — Include only the libgcc members that are actually needed (soft `long double`, a few integer helpers) instead of all of them. Impact: low. Effort: small.
6. **More of the GNU make language** — `define`, `$(call)`, `$(eval)`, `vpath`, archive members, so more third-party Makefiles work unchanged. Impact: medium. Effort: medium.
