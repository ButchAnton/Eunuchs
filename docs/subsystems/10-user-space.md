# User space: libc, shell and commands

Everything that runs in U-mode: a small static C library (`user/lib/`), the headers programs compile against (`user/include/` plus the shared ABI in `lib/include/uapi/`), the shell (`esh`, also `/bin/sh`; it has [its own document](12-shell.md)), `init`, the multi-call `eutils` binary, a text editor (`edit`, also `vi` and `emacs`) and about twenty other programs. Every program is a static ELF linked at `0x10000` with the same startup code and the whole libc, and is copied into the efs disk image at build time. The libc is small and debugging-oriented, but covers most of what ordinary C programs expect: system call wrappers that set `errno`, buffered `FILE` stdio, `printf`/`scanf` with floating point, `strto*`, `qsort`, `getopt`, `math.h`, `time.h` with `strftime`, `setjmp`/`sigsetjmp`, POSIX regular expressions, a first-fit `malloc` with magic numbers and poisoning, and thin wrappers for signals, sockets and termios.

## Responsibilities and scope

What this subsystem does:

- **Program startup and exit.** `crt0.S` turns the initial stack built by the kernel's `do_exec` into `main(argc, argv)` with `environ` set; `exit` runs the `atexit` handlers, flushes every stdio stream and calls `SYS_exit`.
- **System call wrappers.** One generic `__syscall` stub (`syscall.S`) plus a C wrapper per call (`libc.c`, `net.c`) that converts `-errno` returns into `-1` and `errno`.
- **Library services.** Buffered stdio (`stdio.c`), formatted output (the kernel's `printf` core, shared through `lib/printf.c`, plus floating-point conversions compiled only for user space), `scanf`, string functions (`lib/string.c`, also shared, and `strings.c`), number conversion, sorting and searching, `getopt`, temporary files, heap allocation, environment handling, directory reading, `math.h`, time conversion and `strftime`, non-local jumps, regular expressions, `poll`/`select`, IPv4 address helpers, termios helpers and the Eunuchs-specific debug hooks (`dbg_*`).
- **The shell.** `esh`, a zsh-like shell with a line editor (`user/bin/esh/`), is described in [12-shell.md](12-shell.md). This document covers only how it fits in.
- **Commands.** `init` (pid 1), file and text tools, the pager, the editor, `make` (with `tcc`, see [13](13-toolchain.md)), process tools, network tools and servers, and test programs (`utests`, `stress`, `segv`, ...).
- **Disk-image contents.** Which files and links go into `build/fs.img` (`Makefile`: `UPROGS`, `EUTILS_LINKS`, `FS_FILES`).

What it does not do:

- No dynamic linking, no shared libraries, no threads (the kernel gives every process exactly one thread). Floating point works (`-march=rv64gc -mabi=lp64d`); the kernel switches FP registers lazily ([03](03-traps-interrupts-syscalls.md#lazy-floating-point-for-user-programs)).
- No locale (everything is the "C" locale, ASCII `ctype`), no wide characters, no time zones (local time is UTC), no `popen`, no constructors.
- No DNS (`resolve_host` knows numeric addresses and three fixed names).
- The kernel side of every mechanism used here (exec, signals, TTY line discipline, sockets, procfs) belongs to other subsystems. This document only describes the user-side contract; for example, the signal frame is built by `kernel/proc/signal.c` (`setup_frame`), and the initial stack by `kernel/proc/proc.c` (`do_exec`).

## Source map

| File | Role |
|---|---|
| `user/lib/crt0.S` | `_start`: sets `gp` and `fp`, computes `argc`/`argv`/`envp` from `sp`, calls `__libc_start`. |
| `user/lib/syscall.S` | `__syscall(nr, a0..a5)` (shuffles arguments into `a7`, `a0`-`a5`, `ecall`) and `__restore_rt` (signal return trampoline). |
| `user/lib/libc.c` | System call wrappers (process, file, credential, time, environment, terminal, signal), `strerror`/`strsignal`, `gmtime_r`, the `malloc` family, `getenv`/`setenv`/`unsetenv`, `exit`, `__libc_start`. |
| `user/lib/stdio.c` | `FILE` streams: `stdin`/`stdout`/`stderr`, `fopen` family, buffered reads and writes, `printf` family, `getline`, `tmpfile`, `perror`; `__stdio_flush_all`. |
| `user/lib/exit.c` | `exit` and `atexit` (32 handlers), alone in their object so that `tcc -run`'s `runmain.o` can supply its own ([13](13-toolchain.md#tcc--run)). |
| `user/lib/scanf.c` | `scanf`, `fscanf`, `sscanf` and their `v` forms. |
| `user/lib/stdlib.c` | `strto*` (integers and floating point), `ato*`, `abs`/`div` families, `qsort`, `bsearch`, `rand`, `abort`, `__assert_fail`, `system`, `putenv`, `realpath`, `mkstemp`/`mktemp`/`mkdtemp`, `getopt`, `reallocarray`. |
| `user/lib/strings.c` | The string functions not shared with the kernel: `strcpy`, `stpcpy`, `strncpy`, `strcat`, `strncat`, `strdup`, `strndup`, `strspn`, `strcspn`, `strpbrk`, `strtok`/`strtok_r`, `strsep`, `memchr`, `memrchr`, `strcasecmp`, `strncasecmp`, `strcoll`, `strxfrm`. |
| `user/lib/math.c` | `math.h`: argument reduction and series in 128-bit `long double` (software), rounded to `double`. |
| `user/lib/time.c` | `gmtime`, `localtime` (= UTC), `timegm`/`mktime`, `difftime`, `clock`, `asctime`/`ctime`, `strftime`, `tzset`. |
| `user/lib/setjmp.S`, `user/lib/sigsetjmp.c` | `setjmp`/`longjmp` (callee-saved integer and FP registers), `sigsetjmp`/`siglongjmp` (optionally the signal mask). |
| `user/lib/unistd2.c` | `ptrace` (glibc-style: `PTRACE_PEEK*` return the word read), `execl`, `execlp`, `execle` (the argument list becomes a vector), `utimensat`, `futimens`, `fsync`, `ftruncate` (`ENOSYS`), `usleep`, `sysconf`, `gethostname`, `ttyname`, `symlink`/`readlink` stubs. |
| `user/lib/regex.c` | POSIX `regcomp`/`regexec`/`regerror`/`regfree`: BRE and ERE, by backtracking. |
| `user/lib/net.c` | Socket wrappers, `poll`, `select` (on top of `ppoll`), `inet_aton`/`inet_addr`/`inet_ntoa`, `resolve_host`. |
| `lib/printf.c` | `kvcbprintf` (callback-based formatter), `kvsnprintf`, `ksnprintf`. Shared with the kernel and host tests (`tests/host/test_printf.c`). The floating-point conversions and `%n` are inside `#ifdef FMT_FLOAT`, defined only for user code. |
| `lib/string.c` | `mem*`, `str*`, `strlcpy`, `memchr_inv`, `kstrtoul`. Shared with the kernel. |
| `lib/include/fmt.h`, `lib/include/kstring.h` | Prototypes for the two shared files; `kisspace`/`kisdigit`/`kisprint` inlines. |
| `lib/include/uapi/syscall.h` | `SYSCALLS` X-macro (number, name, argument count, strace format), `SYS_*` enum, `DBG_*` operations, `PROT_*`/`MAP_*`, `WNOHANG`. |
| `lib/include/uapi/errno.h` | `ERRNOS` X-macro (Linux numbers and messages). |
| `lib/include/uapi/signal.h` | `SIGNALS` X-macro (number, default action, description), `struct sigaction`, `SA_*`, `SIG_*`. |
| `lib/include/uapi/fs.h` | `O_*`, `F_*`, `S_*`, `struct stat`, `struct dirent`, `struct timespec`/`timeval`, `PATH_MAX`, `NAME_MAX`. |
| `lib/include/uapi/net.h` | Socket constants, `struct sockaddr_in`, `struct pollfd`, `POLL*`, `struct netif_req` and the `NET_IOC_*` ioctls. |
| `lib/include/uapi/termios.h` | `struct termios` (Linux layout), flags, `TCGETS`/`TIOC*` ioctl numbers, `struct winsize`. |
| `user/include/*.h` | POSIX-style headers: `unistd.h`, `stdio.h`, `stdlib.h`, `string.h`, `errno.h`, `fcntl.h`, `signal.h`, `dirent.h`, `termios.h`, `time.h`, `poll.h`, `netdb.h`, `eunuchs.h`, `stdint.h`, `inttypes.h`, `limits.h`, `ctype.h`, `assert.h`, `math.h`, `regex.h`, `setjmp.h`, `sys/{mman,select,socket,stat,types,time,wait}.h`, `netinet/in.h`, `arpa/inet.h`. |
| `user/user.ld` | Linker script: text at `0x10000`, page-aligned r-x / r-- / rw- segments. |
| `user/bin/init.c` | pid 1: prints `/etc/motd`, runs and restarts `/bin/esh -l`, reaps orphans; `-1` mode for tests. |
| `user/bin/esh/*.c` | The shell (one program from several files; see [12-shell.md](12-shell.md)). |
| `user/bin/eutils.c` | Multi-call binary: 32 small tools chosen by `argv[0]`. |
| `user/bin/{ls,cat,grep,more,ps,top}.c` | Standalone file, text and process tools. |
| `user/bin/db/*.c` | `db`, the debugger ([14](14-debugger.md)). |
| `user/bin/edit/*.c` | The text editor, one program with a `vi` and an `emacs` personality (see [The editor](#the-editor-vi-and-emacs)). |
| `user/bin/{ping,nc,ifconfig,netstat}.c` | Network clients and configuration. |
| `user/bin/{httpd,rshd,echod}.c` | Network servers. |
| `user/bin/{utests,stress,segv,spin,exitcode,hello}.c` | Test and demonstration programs. |
| `user/etc/motd`, `user/etc/passwd`, `user/etc/group`, `user/etc/hostname`, `user/etc/eshrc` | Message of the day; user database (`root` 0, `guest` 1000, both with `/bin/esh`); group names (for `ps` and `ls`); host name for prompts; the shell's system-wide start-up file. |
| `user/www/index.html` | `httpd`'s default page (links into `/proc`). |
| `Makefile` (`UCFLAGS`, `ULIB`, `UPROGS`, `UMULTI`, `EUTILS_LINKS`, `FS_FILES`) | How user code is compiled, linked, stripped and put in the disk image. `UMULTI` links each directory `user/bin/NAME/` into one program `/bin/NAME`. |

## Concepts and data structures

### Address space of a user program

The linker script `user/user.ld` and the kernel's `do_exec` (`kernel/proc/proc.c`) together give this layout (constants from `kernel/include/proc.h`):

| Address | Contents | Set up by |
|---|---|---|
| `0x0` - `0x10000` | Unmapped (NULL guard). | kernel |
| `0x10000` | `.text` (r-x), starting with `.text.start` so `_start` is the first byte. | `user.ld` |
| next page | `.rodata` (r--), including `.srodata`. | `user.ld` |
| next page | `.data` + `.bss` (rw-). `__bss_start`/`__bss_end` mark the BSS. | `user.ld` |
| `PAGE_ALIGN(end of image)` | Heap, grown by `brk`. `do_brk` refuses to grow it to or past `USER_MMAP_BASE` or into another mapping. | kernel (`mm->brk_start`) |
| `0x2000000000` (`USER_MMAP_BASE`) | Anonymous `mmap` area, grows up. | kernel |
| `0x3ffff00000 - 8 MiB` .. `0x3ffff00000` (`USER_STACK_TOP`) | Stack, 8 MiB, demand-paged, with an unmapped gap below it. | kernel |

The linker discards `.comment`, `.riscv.attributes`, `.note*` and `.eh_frame*`. `__global_pointer$` is set to `__bss_start` with `PROVIDE`, and `_start` loads it into `gp` with relaxation disabled (the `.option norelax` block), so the linker can relax nearby small-data accesses to `gp`-relative ones.

### Initial stack (what `_start` sees)

`do_exec` copies the argument and environment strings to the top of the stack, then writes a vector below them and aligns `sp` down to 16 bytes:

```
USER_STACK_TOP  0x3ffff00000
                argv[0] string            (strings packed downwards, argv first,
                argv[1] string ...         then envp; no alignment between them)
                envp[0] string ...
                (padding to 16 bytes)
sp + 8*(argc+envc+4) .. : AT_NULL pair: 0, 0     <- the only auxv entry
                          NULL
                          envp[envc-1] ... envp[0]
sp + 8*(argc+2)         : envp[0]
sp + 8*(argc+1)         : NULL
sp + 8                  : argv[0] ... argv[argc-1]
sp                      : argc
```

The kernel also sets `a0 = argc`, `a1 = sp + 8` and `a2 = envp`, but `_start` does not rely on them: it reloads `argc` from `0(sp)` and computes `argv = sp + 8` and `envp = argv + argc + 1` itself. The auxiliary vector contains only `AT_NULL`; libc never reads it.

Limits come from `sys_execve`/`copy_strv`: at most `MAX_ARGS` = 32 argument pointers and 32 environment pointers, and `MAX_ARG_LEN` = 4096 bytes of strings in total (both vectors together); more fails with `E2BIG`. The path is copied into a 128-byte buffer. An empty `argv` is replaced by the basename of the path.

### errno and the return convention

The kernel returns `-errno` in `a0` (Linux numbers from `uapi/errno.h`). `ret()` (a `static` copy in both `libc.c` and `net.c`) treats any value in `(-4096, 0)` as an error, stores the negated value in the global `int errno` and returns `-1`. A few wrappers skip `ret()` because the call cannot fail or because the raw value is the result: `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `umask`, `uptime_ms`, `sync`, the `void` `dbg_*` calls, and the internal `brk` path. `mmap` maps an error to `MAP_FAILED`, `getcwd` to `NULL`.

`strerror` and `strsignal` are generated from the `ERRNOS` and `SIGNALS` X-macros, so they always match the kernel's numbering.

### stdio streams

`stdio.c` implements `FILE` (an opaque type in `stdio.h`):

```
struct FILE {
    int fd, flags;          F_READ F_WRITE F_EOF F_ERR F_APPEND
    int mode;               _IOFBF, _IOLBF or _IONBF
    char *buf; size_t size;
    size_t pos, len;        reading: buf[pos..len) unread; writing: buf[0..pos) pending
    int writing;            the buffer holds output
    int ungot;              one ungetc character, or EOF
    int own_buf;
    struct FILE *next;      list of streams opened with fopen/fdopen/tmpfile
};
```

- **The three standard streams** are static: `stdin` (fd 0, fully buffered, `BUFSIZ` = 4096-byte static buffer), `stdout` (fd 1, **always line-buffered**, terminal or not, as Eunuchs programs have always expected; static buffer) and `stderr` (fd 2, unbuffered).
- **Other streams** (`fopen`, `fdopen`, `tmpfile`) are line-buffered if `isatty(fd)`, otherwise fully buffered. Their `BUFSIZ` buffer is allocated on first use; if that `malloc` fails the stream becomes unbuffered. `setvbuf`/`setbuf` change the mode or supply a buffer.
- **Writing.** Bytes are copied into the buffer, which is flushed when full and, for a line-buffered stream, when it contains a `'\n'`. `flush_out` loops until everything is written (retrying `EINTR`); a failed or zero-length `write` sets the error flag and drops the pending bytes.
- **Reading.** `fill` reads up to a buffer's worth. Before reading stdin (or any stream on fd 0) it flushes stdout, so a prompt without a newline appears before input is awaited. `fread` of `BUFSIZ` bytes or more reads straight into the caller's memory. `ungetc` of the character just read backs up within the read buffer, so several characters can be pushed back that way (`scanf` relies on it after reading ahead); otherwise one extra character is held in `ungot`.
- **Switching direction and seeking.** `fseek` accounts for read-ahead and the pushed-back character; `fflush` of an input stream seeks the descriptor back over unread read-ahead. As in ISO C, a `+` stream needs an `fseek` (or `fflush`) between reading and writing.
- **Exit.** `exit` (`exit.c`) runs the `atexit` functions newest first, then calls `__stdio_flush_all` (`fflush(NULL)`: stdout, stderr and every stream on the open list) through a weak reference, so a program that uses no stdio does not pull it in, then `_exit`. stdio also registers `__stdio_flush_all` with `atexit` on its first output, for an `exit` that knows nothing about stdio (the one `tcc -run` links in). `_exit`, `_Exit` and a fatal signal flush nothing; `abort` flushes everything before raising `SIGABRT`.

The formatted-output functions all go through one formatter: `vfprintf` runs `kvcbprintf` into a 256-byte staging sink that is passed to the stream in chunks. `snprintf`/`vsnprintf` remain macros for the shared `ksnprintf`/`kvsnprintf`; `sprintf` is `kvsnprintf` with a 1 GiB limit; `asprintf` allocates. Eunuchs extras kept for older programs: `fflush_stdout()` is `fflush(stdout)`, and `fprintf_fd(fd, ...)` (and the `eprintf(...)` macro, `fprintf_fd(2, ...)`) flushes stdout first, then formats straight to the descriptor with `vdprintf` (no length limit), so error text never overtakes earlier standard output.

### malloc heap

```
struct block {               32 bytes, so user pointers stay 16-byte aligned
    unsigned long magic;     MAGIC_USED 0xa110c8ed or MAGIC_FREE 0xf4eeb10c
    size_t size;             usable bytes after the header (multiple of 16)
    struct block *next;      free-list link (meaningful only when free)
    unsigned long pad;
};
```

- Requests are rounded up to 16 bytes.
- There is one singly linked free list (`free_list`), kept in address order and searched first-fit from the lowest address.
- A free block is split if the remainder can hold a header plus 16 bytes (`sizeof(struct block) + 16` = 48). The remainder takes the block's place in the list.
- `free` (through `free_insert`) puts the block in its place in the list and merges it with the free block right after it and the free block right before it, when they are adjacent in memory. Long-running programs (the shell) therefore reuse freed memory for larger requests instead of fragmenting the heap.
- If nothing fits, the heap grows by `sbrk(max(4096, n + 32))`. The surplus of the new chunk becomes a free block, merged with a free block that ends where the old heap ended.
- The heap is never returned to the kernel.
- `heap_used` and `heap_total` count bytes; `malloc_stats()` prints them.

Debugging checks (all in `libc.c`):

| Check | Where | Reports |
|---|---|---|
| Every block walked on the free list must have `MAGIC_FREE`. | `malloc` | `free list corrupted` |
| Freeing a block whose magic is already `MAGIC_FREE`. | `free` | `double free` (if the block was since merged into a neighbour, its header no longer has a magic and the report is the next row's) |
| Freeing a block whose magic is neither value. | `free` | `free of a pointer malloc did not return` (also catches an overwritten header) |
| `realloc` of a block without `MAGIC_USED`. | `realloc` | `realloc of a bad pointer` |
| Freed memory is filled with `0x6b` (the kernel's freed-page byte). | `free` | A use after free reads `0x6b6b...`, which is easy to spot in a fault report. |

`malloc_die` flushes stdout, prints `malloc: <what> (<ptr>) -- heap corruption or bad pointer` to stderr, emits a `dbg_mark("malloc abort")` trace event and kernel log line, and calls `_exit(134)` (the status a `SIGABRT` death would produce, but delivered as a normal exit).

### Environment

`environ` starts as the kernel-provided `envp` (pointers into the stack). `getenv` scans it linearly. The first `setenv` or `unsetenv` calls `own_environ`, which copies the pointers (at most 63) into the static `env_store[64]` and points `environ` there. `setenv` allocates `"NAME=value"` with `malloc`; `unsetenv` closes the gap by shifting later entries down. Strings removed by `unsetenv` are never freed. `execv` and `execvp` pass the current `environ` to `execve`.

The kernel's `proc_spawn` gives the first process `PATH=/bin`, `HOME=/`, `TERM=vt100`.

### DIR

`opendir` opens the path with `O_RDONLY | O_DIRECTORY` and mallocs a `struct DIR { int fd; int pos, n; struct dirent ents[16]; }`. `readdir` refills `ents` with one `getdents64` call when empty. `struct dirent` is a fixed 64-byte record (`d_ino`, `d_type`, `d_name[59]`), not Linux's variable-length one, so `readdir` simply indexes the array.

## How it works

### Building and linking

`UCFLAGS` (Makefile) compile user code with `-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-builtin -nostdlib -nostdinc -isystem <gcc include> -fno-pic -fno-stack-protector -fno-omit-frame-pointer -g -O1 -Wall -Wextra -Werror -std=gnu11 -Iuser/include -Ilib/include -DFMT_FLOAT`. Three points matter:

- `-fno-omit-frame-pointer` is required: the kernel's user backtraces (`proc_user_backtrace`) and the profiler walk frame records (`{prev fp, ra}` at `fp - 16`).
- `-fno-builtin` and `-fno-tree-loop-distribute-patterns` keep GCC from turning loops into calls to functions libc lacks.
- `-march=rv64gc -mabi=lp64d` is the hard-float ABI: `double` arguments travel in FP registers, and the compiler may use FP registers anywhere. `-DFMT_FLOAT` turns on the floating-point conversions in the shared `lib/printf.c`.

`ULIB` is a list of objects, not an archive: `crt0.o` first, then one object for every `user/lib/*.c` and `user/lib/*.S` (a `wildcard`, so a new libc file needs no Makefile change), then `printf.o` and `string.o`. Every program links all of them, so each binary carries the whole libc (`hello` has about 70 KiB of text) (`-nostdlib -static -T user/user.ld ... -lgcc`). `UPROGS` is `$(wildcard user/bin/*.c)`, so every C file in `user/bin` becomes a program. Each program is then copied with `objcopy --strip-debug` into `build/user/stripped/`; the stripped copy (symbol table kept for kernel backtraces) goes into the disk image, and the full one in `build/user/bin/` is for GDB and the profiler.

### Startup: `_start` to `main`

1. `_start` (`crt0.S`) loads `gp` from `__global_pointer$`, sets `fp = 0` (terminates the frame-pointer chain, which is what stops `proc_user_backtrace`), computes `argc`, `argv`, `envp` from `sp`, and calls `__libc_start`. If that ever returned, `_start` would spin in `1: j 1b`.
2. `__libc_start` (`libc.c`) sets `environ = envp` and calls `exit(main(argc, argv))`. `main` is declared with two arguments; programs use `environ` or `getenv` for the environment.
3. There is no BSS clearing in user space. The kernel's ELF loader (`elf_load`) copies the file bytes of each segment eagerly, and the rest of the segment (the BSS) stays zero.
4. `exit` runs the `atexit` functions (newest first), flushes stdio (`__stdio_flush_all`) and then calls `_exit`, which issues `SYS_exit` and loops forever in case it returns.

### A system call

`read(fd, buf, n)` expands to `ret(__syscall(SYS_read, fd, buf, n, 0, 0, 0))`. `__syscall` moves its first argument into `a7` and shifts the other six down into `a0`-`a5`, then executes `ecall`; the result comes back in `a0`. Because the stub follows the normal calling convention, any wrapper is a single C line. To add a wrapper: make sure the call is in the `SYSCALLS` X-macro in `uapi/syscall.h` (which also makes `strace` decode it), add the C function with `SCn(...)` and `ret()`, and declare it in the matching header.

### Signals

`sigaction` (`libc.c`) copies the caller's `struct sigaction` and always sets `sa_restorer = __restore_rt`, then calls `SYS_rt_sigaction`. The kernel's `setup_frame` refuses to deliver to a handler without a restorer and kills the process ("no restorer"), so a program must install handlers through these wrappers (or set `sa_restorer` itself).

Delivery, from the user's point of view:

1. On the way back to user mode, the kernel pushes a `struct sigframe` (magic `"SIGFRAME"`, all 32 registers, `sepc`, the mask to restore, the signal number, and the FP registers if the program has used floating point) below the interrupted `sp`, aligned to 16 bytes.
2. It sets `a0 = sig`, `ra = __restore_rt`, `sp` = the frame, `pc` = handler.
3. The handler returns into `__restore_rt` (`syscall.S`), which does `li a7, 139` (`SYS_rt_sigreturn`) and `ecall`. The kernel checks the magic, restores registers (FP ones included) and mask, and resumes the interrupted code. A handler may therefore use floating point freely.

The rest of the API:

- `signal(sig, h)` is `sigaction` with `sa_flags = 0` and an empty mask. Unlike glibc's `signal`, it does not set `SA_RESTART`, so slow calls interrupted by the handler fail with `EINTR`.
- `sigprocmask` and `sigsuspend` are direct wrappers. `sigset_t` is `uint32_t`, and `sigemptyset`/`sigaddset`/... are inline bit operations (`SIGBIT(s)` = `1u << s`).
- `pause` reads the current mask with `sigprocmask(SIG_BLOCK, NULL, &cur)` and calls `sigsuspend(&cur)`.
- `raise(sig)` is `kill(getpid(), sig)`.

The kernel honours `SA_RESTART`, `SA_NODEFER` and `SA_RESETHAND` (`signal_deliver`). The comment on `SA_RESTART` in `uapi/signal.h` ("interrupted calls still fail with EINTR") is out of date.

### Formatted output

`printf` → `vfprintf(stdout)` → `kvcbprintf(sink, ...)`. `kvcbprintf` (`lib/printf.c`) parses `%[flags][width][.prec][length]conv` with:

- flags `- + space # 0`;
- width and precision, including `*`;
- lengths `hh h l ll z t j`, and `L` (a `long double` argument, converted to `double` before formatting);
- conversions `d i u o x X p s c %`, and in user space (`FMT_FLOAT`) also `f F e E g G a A` and `n`.

`%p` always prints `0x`. `%s` of `NULL` prints `(null)`. An unknown conversion is printed literally (`%` and the character) so the mistake is visible. `kvsnprintf` always NUL-terminates when `size > 0` and returns the length the output would have had (C99 semantics), so callers that do `n += snprintf(...)` must clamp.

**Floating point** (`#ifdef FMT_FLOAT`; the kernel's copy has none, and neither does the host `test_printf` build):

- `sig_digits` estimates the decimal exponent from the binary one, scales the value by a power of ten in 128-bit `long double` (GCC's software implementation on RISC-V), and extracts up to 17 significant digits, which is enough to round-trip any `double` (`%.17g` of 0.1 is `0.10000000000000001`). The last digit is rounded half to even, where the half is exact in `long double`. Digits asked for beyond the 17th are zeros: `%f` of `1e300` does not print the exact binary value as glibc does, and `%.20f` of 0.1 ends in zeros.
- `%e` has at least two exponent digits; `%g` uses `%e` style when the exponent is below -4 or at least the precision (0 counts as 1), and drops trailing zeros unless `#` is given. The default precision is 6.
- `%a` prints the exact hexadecimal mantissa (`0x1p+0`, subnormals as `0x0.…p-1022`), trailing zero nibbles trimmed when there is no precision; a precision below 13 rounds half up.
- Infinities and NaNs print as `inf`/`nan` (`INF`/`NAN` for the upper-case conversions), with a sign from the sign bit or the `+`/space flags; the `0` flag does not apply to them.
- Output is limited to about 350 digits per conversion, so a huge precision is silently cut.

### Formatted input: `scanf`

`scanf.c` provides `scanf`, `fscanf`, `sscanf` and `vscanf`, `vfscanf`, `vsscanf`, over a small input abstraction (a string, or a `FILE` read with `fgetc`/`ungetc`):

- conversions `d i u o x X c s [...] f F e E g G a A p n %`, with widths and `*` (assignment suppression);
- lengths `hh` and `h`; `l`, `ll`, `j`, `z` and `t` all store a `long`; for floats no length means `float`, `l` `double`, `L` `long double`;
- `%[` with `^`, a leading `]`, and ranges; `%c` reads one character by default and adds no NUL;
- `%i` accepts `0x`, octal `0`, and also a `0b` binary prefix (an extension, from `strtoll` base 0);
- white space in the format skips any white space; other characters must match.

Numbers are collected into a 512-byte buffer and converted with `strtoll`/`strtoull`/`strtold`. The return value is the number of assignments, or `EOF` if input ended before the first conversion.

### Numbers, sorting and other `stdlib.h` services

- **`strtol` family** (`strtol strtoul strtoll strtoull strtoimax strtoumax`, `atoi atol atoll`): base 0 detects `0x` and octal, and `0b` is also accepted; overflow returns the limit with `ERANGE`; `*end` is the start of the string when there were no digits.
- **`strtod` family** (`strtod strtof strtold atof`): decimal and hexadecimal (`0x1.8p1`) forms, `inf`/`infinity` and `nan`/`nan(...)`. The value is built in `long double` from up to 19 significant digits, then rounded; `ERANGE` is set on overflow only.
- **`qsort`**: quicksort with median-of-three pivots, insertion sort for 12 elements or fewer, recursion on the smaller side (stack depth O(log n), worst case O(n²) time). `bsearch` is a binary search.
- **`rand`/`srand`, `random`/`srandom`**: one 64-bit linear congruential generator, `RAND_MAX` = 2³¹ − 1.
- **`getopt`** (declared in `unistd.h`, defined in `stdlib.c`): POSIX behaviour, stopping at the first non-option or `--`; a leading `:` in the option string selects quiet mode. No GNU argument permutation, no `::` optional arguments, no `getopt_long`. Setting `optind = 0` resets it.
- **`system`**: flushes all streams, runs `/bin/sh -c CMD` with `SIGINT`/`SIGQUIT` ignored in the caller while it waits, and returns the wait status.
- **`realpath`**: purely lexical (makes the path absolute with `getcwd`, removes `.`, `..` and repeated `/`), which is exact because efs has no symbolic links; the result must exist.
- **`mkstemp`, `mkdtemp`, `mktemp`**: replace a trailing `XXXXXX` with letters and digits, up to 100 tries with `O_EXCL`. `tmpfile` (stdio) creates and unlinks a file in `/tmp`; `tmpnam` makes `/tmp/tmp<pid>.<n>`.
- **`abort`** flushes all streams, restores the default `SIGABRT` action and raises it. `assert` (`assert.h`, honouring `NDEBUG`) prints `file:line: function: assertion failed: expr` and aborts.
- Also `abs labs llabs div ldiv lldiv reallocarray putenv` (`putenv` copies the string through `setenv`).

### Mathematics: `math.h`

`math.c` computes in 128-bit `long double`, which GCC implements in software on RISC-V, and rounds the result to `double`: arguments are reduced (by `ln 2` for exponentials, modulo π/2 for trigonometry) and Taylor or atanh series summed with enough guard bits to be correct to the last bit or two (`utests` checks agreement to 1e-14). `sqrt` uses the hardware `fsqrt.d`.

- Exponential and logarithmic: `exp exp2 expm1 log log2 log10 log1p pow` (`pow` multiplies exactly for integer exponents up to 64).
- Trigonometric and hyperbolic: `sin cos tan asin acos atan atan2 sinh cosh tanh`.
- Roots and distances: `sqrt cbrt hypot`.
- Rounding and remainders: `floor ceil trunc round lround rint lrint nearbyint modf fmod remainder` (`fmod` is exact).
- Manipulation and comparison: `fabs copysign frexp ldexp scalbn fmin fmax fdim fma` (`fma` is computed in `long double`, not fused).
- `float` versions of `fabs sqrt floor ceil sin cos pow exp log`; classification macros `fpclassify isnan isinf isfinite isnormal signbit`; `HUGE_VAL`, `INFINITY`, `NAN`, and the `M_PI`-style constants.

Because every step is a software `long double` operation, these functions are much slower than a hardware-`double` libm; fine for occasional use, poor for numeric inner loops.

### Time: `time.h`

Local time is UTC: `localtime`/`localtime_r` are `gmtime`/`gmtime_r`, `mktime` is `timegm`, `tzname` is `{"UTC", "UTC"}`, and `tzset` does nothing. `struct tm` has `tm_isdst` (always 0), `tm_gmtoff` (0) and `tm_zone` (`"UTC"`).

- `timegm`/`mktime` normalize out-of-range fields and fill in `tm_wday` and `tm_yday`.
- `strftime` supports `%a %A %b %h %B %c %C %d %D %e %F %H %I %j %k %l %m %M %n %p %P %r %R %s %S %t %T %u %w %U %W %x %X %y %Y %z %Z %%` (`%z` is `+0000`); `E` and `O` modifiers are accepted and ignored, and an unknown conversion is copied literally. It returns 0 if the result does not fit.
- `asctime`, `ctime` (and `_r` forms), `difftime`, and `clock` (microseconds since boot, from `uptime_ms`, so wall time rather than CPU time).
- `time`, `clock_gettime`, `gettimeofday` (also in `sys/time.h`), `nanosleep` and `sleep` are system call wrappers in `libc.c`.

### Non-local jumps: `setjmp.h`

`setjmp` (`setjmp.S`) saves `ra`, `sp`, `s0`–`s11` and the callee-saved FP registers `fs0`–`fs11` in a 28-word `jmp_buf`; `longjmp` restores them and returns its value (1 if given 0). `_setjmp`/`_longjmp` are aliases. `sigsetjmp(env, savemask)` records `savemask` and, if it is non-zero, the current signal mask in two extra words of `sigjmp_buf`, then continues as `setjmp`; `siglongjmp` restores that mask before jumping. Plain `setjmp` does not save the mask.

### Regular expressions: `regex.h`

`regex.c` implements the POSIX interface (`regcomp`, `regexec`, `regerror`, `regfree`). `regcomp` parses the pattern into a small tree (alternation, concatenation, repetition, groups, character sets, anchors, back-references), and `regexec` matches it by **backtracking** with explicit continuations, trying each start position in turn.

- **Syntax.** Basic (BRE, the default) and extended (`REG_EXTENDED`) syntax: `.`, `[...]` with ranges, negation and the classes `[:alpha:]`, `[:digit:]`, `[:alnum:]`, `[:upper:]`, `[:lower:]`, `[:space:]`, `[:blank:]`, `[:punct:]`, `[:print:]`, `[:graph:]`, `[:cntrl:]`, `[:xdigit:]` (no `[= =]` or `[. .]`); `*`; grouping with `\( \)` (BRE) or `( )` (ERE); intervals `\{m,n\}` or `{m,n}`; `+`, `?` and `|` in ERE, and as `\+`, `\?`, `\|` in BRE (the GNU extensions); back-references `\1`–`\9` in both; anchors `^` and `$` (in BRE only at the start and end of a branch). GNU escapes `\< \> \b \B \w \W \s \S \d \D`, plus `\n` and `\t`.
- **Flags.** `REG_ICASE`, `REG_NOSUB`, `REG_NEWLINE` (`.` and negated sets do not match a newline; `^`/`$` also match around one) for `regcomp`; `REG_NOTBOL`, `REG_NOTEOL` and the BSD `REG_STARTEND` (match only within `pmatch[0]`) for `regexec`.
- **Which match.** The leftmost match wins, but among matches at that position the **first** one the backtracker finds is reported, with greedy repetition and alternatives tried in order. That is Perl's rule (leftmost-first), not POSIX's leftmost-longest: `a|ab` on `ab` matches `a`.
- **Limits.** Only groups 1–9 are reported (`pmatch[10..]` are set to -1). Each start position gets at most 2,000,000 matching steps; a pattern that exceeds them (catastrophic backtracking) fails to match there rather than hanging. Matching recurses a few C frames per subject character, so very long subjects use deep stack.
- **Errors.** The standard `REG_E*` codes, with messages from `regerror`.

The editor's searches and substitutions use it; `grep`, `more` and the shell's `[[ =~ ]]` do not (yet).

### Heap allocation

The flow is described under "malloc heap" above. `sbrk(inc)` caches the break in `cur_brk` (fetched with `brk(0)` the first time) and calls `brk(old + inc)`. `brk(addr)` stores whatever the kernel returns, which is the old break if the request was refused, and fails with `ENOMEM` if that differs from `addr`. `calloc` is `malloc` + `memset`; it returns `NULL` if `n * size` overflows. `realloc` returns the same pointer when the block is already big enough. Otherwise it allocates, copies and frees, and never grows in place.

Programs that want big, page-granular memory can use `mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`. The kernel rejects writable+executable mappings with `EACCES` (tested in `utests.c` `t_mmap`).

### poll and select

`poll(fds, n, ms)` converts milliseconds to a `timespec` and calls `ppoll`; a negative timeout passes `NULL` (wait forever). `select` builds a `struct pollfd` array on the stack (descriptors below `FD_SETSIZE` = 64; `nfds` is clamped to 64), polls, and rebuilds the sets: readable means `POLLIN|POLLHUP|POLLERR`, writable means `POLLOUT|POLLERR`, and exceptional means `POLLERR`. It returns the number of bits set. It does not update `*timeout`.

### init

`init` (`user/bin/init.c`) runs as pid 1, started by the kernel's `kinit` (default `/bin/init`, overridable with the `init=` boot option).

1. Program to run: `argv[1..]`, or `/bin/esh -l` (a login shell) if none. `init -1 PROG ARGS...` means "once": the `utest` boot option starts `init -1 /bin/utests [arg]`.
2. Prints `init: pid N, running PROG`, then (not in once mode) copies `/etc/motd` to stdout.
3. Loop: fork and `execv` the program. Then `wait()` repeatedly. Children that are not the program are adopted orphans and are just reaped. When the program exits:
   - in once mode, init returns its status (`128 + signal` if it was killed), and the kernel powers off with that result;
   - otherwise it prints `init: /bin/esh (pid N) exited with status S; restarting`, sleeps 200 ms and starts it again.

### The shell

The shell is `esh`; [12-shell.md](12-shell.md) describes the language, the line editor, and how they are implemented. As a user-space program, what matters here is:

- **One binary, several sources.** It is built from `user/bin/esh/*.c` by the `UMULTI` rule and installed as `/bin/esh`, with `/bin/sh` a hard link to it. Invoked as `sh`, it turns on `shwordsplit` for POSIX scripts.
- **Who runs it.** `init` starts `esh -l` on the console; `su` execs the shell field of `/etc/passwd`; `rshd` runs `esh -i` (prompts without a terminal).
- **The terminal.** It uses the TTY in raw mode while editing and restores the saved modes before running each command. It answers to the kernel's job control: process groups, `tcsetpgrp`, `SIGTTIN`/`SIGTTOU`, `WUNTRACED`. When the terminal answers a cursor-position query, it sets the kernel's window size with `TIOCSWINSZ`, so `top` and other programs see the real size.
- **libc.** It relies on the libc's buffered stdout (flushed before every `fork`), `malloc` (plus its own per-command arena), `opendir`/`readdir`, `poll`, and the termios and signal wrappers.

### The `more` and `less` pager

`more` (`user/bin/more.c`) is also installed as `/bin/less`, and the name it is run as picks the personality:

| | `more` | `less` |
|---|---|---|
| Start | Prints the first screen where the cursor is (no clearing). If everything fits on one screen, it just prints it and exits. | Clears the screen; marks rows past the end with `~`. |
| Prompt | `--More--(42%)`, or `--More--(Next file: x)` (`-d` adds a hint) | the file name on the first screen, then `:`; `(END)` at the end |
| End of the last file | exits | waits (`-E` exits, `-F` exits if it all fit) |
| Several files | `::::::::::::::` headers between them | `:n` / `:p` |

Both keep what they have read in memory, so they scroll backward as well as forward, even when reading a pipe.

**Keys.** `h` shows a summary. A number typed first is a count.
- Forward: Space/`f`/`^F`/PageDown a screen; Return/`j`/`^N`/Down a line; `d`/`^D` half a screen; `s` skips lines.
- Backward: `b`/`^B`/PageUp a screen; `k`/`y`/`^P`/Up a line; `u`/`^U` half a screen.
- Jumps: `g`/`<`/Home to line N (default 1); `G`/`>`/End to the end or line N; `p`/`%` to N percent.
- Search: `/pattern` forward, `?pattern` backward, `n`/`N` repeat it. Matches are shown in reverse video. Patterns are grep's: `c . ^ $ *` and `\c`.
- Marks: `m` plus a letter sets one, `'` plus the letter returns to it.
- Files: `:n` and `:p` go to the next and previous file; `:f`, `=` or `^G` shows where you are.
- `!command` runs a shell command. `r`/`^L` redraws. `-N`, `-i` and `-R` toggle options. `q`, `Q`, `:q` or `ZZ` quits.

**Options.** `-N` line numbers; `-i` case-insensitive searches; `-R` passes color escapes (`ESC[...m`) through, so `ls --color=always | less -R` keeps its colors; `-s` squeezes runs of blank lines; `-F`, `-E` and `-d` as above; `-NUM` lines per screen; `+NUM` starts at a line and `+/PATTERN` at the first match. `-X`, `-c`, `-p`, `-f`, `-u`, `-l`, `-S` and `-K` are accepted and ignored.

**Terminal.**
- Keys are read from `/dev/tty`, so `ps aux | more` works, and the size comes from the terminal (`TIOCGWINSZ`, or `LINES`/`COLUMNS`).
- Long lines wrap, tabs expand to 8 columns, and control characters show as `^X`.
- Only `ICANON` and `ECHO` are turned off, so `^C` quits (status 130) and `^Z` suspends. Either way the terminal modes are restored, and after `fg` the screen is redrawn.
- When standard output is not a terminal, the pager just copies its input (with `::::` headers for several files in `more` style), so `cmd | more | wc -l` works.

**Not available:** regular expressions beyond grep's subset, following a growing file (`F`), an editor (`v`), and the alternate screen: after `less` quits, its last screen stays visible, as with `less -X`.

### The `ls` command

`ls` (`user/bin/ls.c`) behaves like GNU coreutils' `ls`, and BSD's where the letters agree. `ls --help` lists every option.

**Layout.**
- On a terminal it prints names in columns that fit the terminal width (`-C`; `-x` fills across). It uses GNU's layout: as many columns as fit, each as wide as its widest name plus two spaces. Otherwise it prints one name per line (`-1`).
- `-m` separates names with commas.
- `-l` gives the long format:
  - mode, link count, owner, group, size, modification time and name;
  - devices show `major, minor` instead of a size;
  - times older than six months, or in the future, show the year instead of the time;
  - a `total` line gives the blocks (1 KiB units) of the listed files.
- Long-format variants:
  - `-o` leaves out the group, `-g` the owner, and `-n` shows numeric ids;
  - `-h` gives human-readable sizes (1.5K, 23M);
  - `--full-time` and `--time-style=full-iso|long-iso|iso` choose the time format.
- `-i` and `-s` add inode numbers and block counts in any format.

**What is listed.**
- Arguments that are files come first, then the contents of each directory argument. Directories get a `dir:` header when there is more than one argument, or with `-R`.
- `-a` includes names starting with `.`, and `-A` does the same without `.` and `..`.
- `-d` lists directories themselves, and `-R` recurses.
- `-B` hides backup files (`~`), and `-I PAT`/`--hide=PAT` hides names matching a glob.

**Order.**
- By name (byte order), the default.
- `-t` newest first, or by ctime with `-c`.
- `-S` largest first, `-X` by extension, `-v` natural order (`b9` before `b10`), `-U` directory order.
- `-r` reverses any order, and `--group-directories-first` puts directories first.

**Names.**
- `-F` appends `/` to directories, `*` to executables and `|` to FIFOs; `-p` appends only the `/`.
- `--color[=always|auto|never]` (and `-G`) colors names from `LS_COLORS`, using GNU's defaults for the keys it doesn't set:
  - `di` directories (bold blue), `ex` executables (bold green), `cd` devices;
  - `ow` world-writable directories (blue on green, like `/tmp`), `su`/`sg` setuid and setgid files;
  - `*.ext` rules.

  `/etc/eshrc` aliases `ls` to `ls --color=auto`.
- On a terminal, names that need quoting are shown the way the shell would quote them (`'a file'`). That's GNU's `shell-escape` style; `-N` prints names literally. `-Q` uses C-style double quotes, `-b` backslash escapes, and `-q` shows `?` for unprintable characters.

**Exit status:** 0 if everything worked; 1 for a minor problem, such as an unreadable subdirectory; 2 when a command-line argument couldn't be accessed (`ls: cannot access 'x': No such file or directory`).

**Not available:** symbolic links (efs has none, so `-L`/`-H` are accepted and change nothing), access times (`-u` uses the modification time), and time zones (times are UTC).

### The `ps` command

`ps` (`user/bin/ps.c`) follows Linux's procps. An option with a dash is a System V option, one without is a BSD option, and a single command line can mix them, so both habits work: `ps -ef`, `ps -lf`, `ps -ejH`, `ps aux`, `ps axjf`.

**Selection.** With no options, `ps` lists processes that have your effective uid and your terminal (in BSD style, any of yours with a terminal).

| Options | Selects |
|---|---|
| `-e`, `-A`, `ax` | Every process. |
| `a` / `x` | BSD: lift "only mine" / lift "only with a terminal". |
| `-a` | Processes with a terminal, except session leaders. |
| `-d` | Everything except session leaders. |
| `-N`, `--deselect` | The opposite of the rest of the selection. |
| `-p`, `p`, `--pid`, `NNN` | By pid. |
| `--ppid` | By parent. |
| `-s`, `--sid` | By session. |
| `-g` | By session (a number) or group (a name). |
| `-u`, `U`, `--user` / `-U` | By effective / real user (names or numbers). |
| `-G`, `--group` | By real group. |
| `-t`, `t`, `--tty`, `T` | By terminal (`console`, or `-` for none); `T` means my terminal. |
| `-C` | By command name. |
| `r` | Narrows any selection to running processes. |

Lists are comma-separated. A process that matches any of the lists given is selected. `ps` exits with status 1 if it selects nothing, so `ps -p $pid >/dev/null` tests whether a process exists.

**Formats.**

| Options | Columns |
|---|---|
| (default) | `PID TTY TIME CMD`; BSD style: `PID TTY STAT TIME COMMAND` |
| `-f` | `UID PID PPID C STIME TTY TIME CMD` (with arguments) |
| `-F` | `-f` plus `SZ RSS PSR` |
| `-l`, `-lf` | `F S UID PID PPID C PRI NI ADDR SZ WCHAN TTY TIME CMD` (and `STIME` with `-f`) |
| `-j`, `-jf` | `PID PGID SID TTY TIME CMD`; with `-f`, `-f` plus `PGID SID` |
| `-L` | Adds `LWP` (and `NLWP` with `-f`); every process has one thread. |
| `u` | `USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND` |
| `j` | `PPID PID PGID SID TTY TPGID STAT UID TIME COMMAND` |
| `l` | `F UID PID PPID PRI NI VSZ RSS WCHAN STAT TTY TIME COMMAND` |
| `v` | `PID TTY STAT TIME VSZ RSS %MEM COMMAND` |
| `s` | `UID PID PENDING BLOCKED IGNORED CAUGHT STAT TTY TIME COMMAND` |
| `-o`, `o`, `--format LIST` | Your own columns: `ps -o pid,ppid,stat,wchan,args`. `name=HEADER` renames a column (the header runs to the end of the list unless a known column name follows a comma), and `name=` alone hides it. If every header is empty, no header line is printed. |
| `-O`, `O LIST` | `PID`, then `LIST`, then the rest of the default. |

**Other options.**
- `-H` indents commands by depth. `f`/`--forest` draws the tree with `\_` (`ps axjf`, `ps -ef --forest`).
- `--sort` and `k` sort by `[+|-]KEY,...` (`ps aux --sort=-rss`). The default order is by pid.
- `h`/`--no-headers` drops the header line.
- `c` shows the command name instead of the arguments; `n` shows numeric user and group ids.
- `-w`/`w` widens the output to 132 columns, and giving it twice removes the limit (`--width N` sets it). Otherwise output to a terminal is cut at its width.
- `ps -o help` (or `ps L`) lists every column with a description; `--help` prints a summary.

**Columns.**
- **Ids:** `pid ppid pgid sid tpgid lwp nlwp uid ruid user ruser gid rgid group rgroup`. User and group names come from `/etc/passwd` and `/etc/group`.
- **Terminal and state:**
  - `tty`;
  - `s`: the Unix state, `R`, `S`, `D` (an uninterruptible sleep), `T` (stopped), `t` (stopped by its debugger) or `Z` (zombie);
  - `stat`: the state plus BSD flags, `s` (session leader) and `+` (in the terminal's foreground group).
- **Commands and times:**
  - `comm` and `args`: zombies show `[name] <defunct>`, and processes without arguments `[name]`;
  - CPU time: `time`, `times`, `bsdtime`;
  - since the start: `etime`, `etimes`;
  - start time: `stime`, `start`, `lstart`, all in UTC;
  - CPU use: `%cpu`, the CPU time divided by the time since start, as in procps, and `c`, its integer form.
- **Memory:** `%mem`, `rss`, `vsz`, `sz`.
- **Scheduling:** `psr` (the CPU it last ran on) and `wchan` (the kernel's wait reason: `wait4`, `tty read`, `socket`, `pipe-read`...).
- **Signals and mask:** `pending`, `blocked`, `ignored` and `caught` (Linux-style hex masks), and `umask`.
- **Eunuchs only:** `syscalls` (system calls made) and `switches` (times scheduled).
- **Fixed values:** the scheduler has no priorities, so `pri`, `ni` and `cls` show 80, 0 and `TS`. `f` is 4 for a process running as root and 0 otherwise. `addr` is `-`.

**Where the data comes from.**
- `/proc/<pid>/status`, including the fields the kernel adds for `ps`: `ustate`, `start_ms`, `cpu_ms`, `vsz_kib`, `tty`, `tpgid`, `psr`.
- `/proc/<pid>/cmdline` for the arguments.
- `/proc/meminfo` for `%mem`, `uptime_ms()` and `time()` for elapsed and start times.

Only processes are listed. Kernel threads are in `/proc/threads` and the monitor's `ps`.

### The editor: vi and emacs

`edit` (`user/bin/edit/`, about 6,000 lines) is one program with two personalities. The disk image has `/bin/edit` and the hard links `/bin/vi`, `/bin/view`, `/bin/emacs` and `/bin/em`; `main.c` looks at the basename of `argv[0]`: `vi` (also `vim` and `ex`, if you make such links) starts the vi personality, `view` starts it read-only, and every other name, `edit` included, starts the emacs personality.

**The shared core.**

| File | What it does |
|---|---|
| `buffer.c` | A buffer is a growable array of lines (no newline stored), a cursor (line, byte offset), a name and path, and undo/redo stacks. Buffers form a list, so several files can be open. Every edit records an insert or delete; records of one command share a group, which `u`/`C-_` undoes as a unit and redo replays. Loading splits on `\n` (a CR stays visible as `^M`); a file without write permission opens read-only. Saving writes `DIR/.NAME.edit<pid>` and `rename`s it over the file, keeping the old file's mode (new files get 0644); if the temporary file cannot be created, it writes in place. Every saved line ends in `\n`. The syntax is chosen by name: C (`.c .h .cc .cpp .S .js .java`), make (`Makefile`, `makefile`, `*.mk`), Python (`.py`), shell (`.sh .esh`, `eshrc`, `.eshrc`, `esh_profile`), or by a `#!` first line (Python if it mentions `python`, otherwise shell). |
| `term.c` | Opens `/dev/tty` (falling back to fd 0 if it is a terminal), switches to raw mode (no `ICANON`, `ECHO`, `ISIG`, `IEXTEN`, `ICRNL`, `IXON`; `VMIN` 1) and the alternate screen, and restores both on exit. Keys are decoded from escape sequences: arrows, Home/End, PgUp/PgDn, Insert, Delete, Shift-Tab, Ctrl- and Alt-arrows. A lone ESC is recognized when no byte follows within 30 ms; ESC followed quickly by a key is Meta+key. `^Z` restores the terminal and stops the process; on `SIGCONT` the size is re-read and the screen redrawn. The size comes from `TIOCGWINSZ`, overridden by `LINES`/`COLUMNS`. |
| `display.c` | Draws the visible lines with soft wrap (no horizontal scrolling), optional line numbers, tabs expanded, control characters as `^X`, and an optional whitespace view. Syntax highlighting for C (keywords, types, preprocessor lines, strings, numbers, `//` and `/* */` comments, the only state carried across lines), shell and make (`#` comments, `$var`/`$(...)`/`${...}`, make targets) and Python (keywords, `#` comments). Search matches and the visual selection or emacs region are highlighted. The bottom rows are the status or mode line and a message line (errors in red). |
| `prompt.c` | The bottom-line prompt used for `:`, `/`, `M-x`, file names and so on: cursor keys, `^A`/`^E`, `^U`/`^K`/`^W`, a 32-entry history per kind of prompt (Up/Down), and Tab completion of file names, or of command names for `M-x`. Also `run_shell_command`: `fork` and `/bin/sh -c CMD` with stdin from `/dev/null` and stdout and stderr captured through a pipe. |
| `search.c` | Searching and substitution with the libc's POSIX regular expressions (basic syntax, with GNU extensions such as `\+`, `\?`, `\<` and `\>`, and alternation), case folding with "smart case" (insensitive unless the pattern has a capital), wrapping around the end of the buffer. Replacements understand `&` and `\0` (the match), `\1`–`\9` (groups), `\n` (a line break) and `\X` for a literal `X`. |

**vi** (`vi.c`). `vi [-R] [+N | + | +/PATTERN] [FILE...]`; `view` is `vi -R`. Every file named becomes a buffer.

- **Normal mode.** An optional register (`"a`) and count, then a command.
  - Motions: `h j k l`, `w b e W B E ge gE`, `0 ^ $ g_ |`, `gg G H M L`, `{ }`, `%` (brackets), `f F t T ; ,`, `n N * #`, `+ - Enter`, and marks `'x` and `` `x `` (`''` and two backquotes go back to the previous position).
  - Operators `d c y > < gu gU g~`, each with a motion, doubled (`dd`, `guu`, `g~~`) or with a text object: `iw aw iW aW`, quotes `i" a" i' a'` and ``i` a` `` (one line only), brackets `i( a( ib i{ a{ iB i[ a[ i< a<`, and paragraphs `ip ap`.
  - Changes: `x X D C s S r R J gJ ~ p P`, `i a I A o O` (with autoindent), `.` to repeat the last change, `&` to repeat the last `:s`.
  - `u` and `^R` undo and redo; `m` sets a mark (`a`–`z`); `q`*x* records keys into a register and `@`*x* / `@@` replays them; `ZZ`, `ZQ`.
  - Scrolling: `^F ^B ^D ^U ^E ^Y`, `zz zt zb`; `^G` shows the file position, `^L` redraws, `^Z` suspends.
- **Insert mode.** Typing (UTF-8 included), Enter with autoindent, Backspace (joins lines at column 0), `^W`, `^U`, `^R`*x* to insert a register, `^V` for a literal key, Tab (spaces to the next `shiftwidth` with `expandtab`), arrows. `R` is replace mode, where Backspace restores what was overwritten.
- **Visual mode.** `v` (characters) and `V` (lines), `gv` to start again at the previous selection's end, `o` to swap ends, motions and text objects to extend it, and `d x y c s > < ~ u U J r p P D X C S R` to act on it. `:` starts an ex command for `'<,'>`.
- **Registers.** The unnamed register and `"a`–`"z`; `"0` holds the last yank. Yanks and deletes go to the unnamed register as well. Macros are stored in the same registers.
- **Ex commands.** Addresses are numbers, `.`, `$`, `%`, `'x`, `'<`, `'>`, `/pat/`, `?pat?` and `+N`/`-N` offsets, two at most, separated by `,` or `;`.
  - Files: `:w [file]`, `:w!`, `:wq`, `:x`, `:q`, `:q!`, `:wa`, `:qa`, `:wqa`, `:e file`, `:e!`, `:r file`, `:r !cmd`, `:f [name]`.
  - Editing: `:s/pat/rep/[gi]` (any punctuation delimiter; `:s` and `:&` repeat), `:g/pat/cmd` and `:v` (or `:g!`) with `d` or `s` as the command, `:d [x]`, `:y [x]`, `:m addr`, `:t addr` (`:co`), `:j`, `:>`, `:<`, `:=`, `:normal keys`.
  - Shell: `:!cmd` runs a command; `:{range}!cmd` filters the lines through it.
  - Buffers: `:bn`, `:bp`, `:ls` (`:buffers`), `:b N` or `:b name`, `:bd`.
  - Settings: `:set` with `number`/`nu`, `autoindent`/`ai` (on), `ignorecase`/`ic`, `smartcase`/`scs` (on), `hlsearch`/`hls` (on), `list`, `expandtab`/`et` (on), `tabstop`/`ts` (8), `shiftwidth`/`sw` (4) and `syntax`/`syn` (on), with `no` prefixes; `:set` alone shows them. `:noh`, `:syntax on|off`, `:help`, `:version`, `:stop`.
- **Read-only** (`view`, `-R`, or an unwritable file): insert commands warn, and `:w` refuses (E45) unless forced with `!` or given another file name.

**emacs** (`emacs.c`). `emacs [+LINE] [FILE...]` (also `em` and `edit`); with no file it starts in `*scratch*`.

- **Movement.** `C-f C-b C-n C-p` and arrows, `C-a C-e` and Home/End, `M-f M-b` (and Ctrl/Alt-arrows) by word, `M-a M-e` (also `M-{ M-}`) by paragraph, `C-v M-v` and PgDn/PgUp, `M-< M->`, `M-g g` (goto line), `C-l` (recenter: middle, top, bottom).
- **Editing.** `C-d`/Delete, DEL/`C-h`, `M-d`, `M-DEL`, `C-k` (with `C-u N`, N whole lines), `C-o`, `C-t`, `M-t`, `M-u M-l M-c`, `C-q` (quoted insert), `M-\` and `M-SPC`, Tab (indent like the previous line, deeper after `{`, `:` or `(`; spaces only), RET and `C-j` (keep the indentation). Numeric arguments with `C-u` (4, 16, ..., or `C-u` and digits, `-` negating) or `M-` digits.
- **Kill ring.** `C-k`, `C-w`, `M-d`, `M-DEL` and `M-w` save text in a 16-entry ring; consecutive kills append to one entry. `C-y` yanks the newest; `M-y` right after a yank replaces it with the next older entry.
- **Mark and region.** `C-SPC` (or `C-@`) sets the mark, which makes the region active and highlighted; `C-g` deactivates it. `C-x C-x` exchanges point and mark, `C-x h` marks the whole buffer, `C-x C-u`/`C-x C-l` change the region's case. Region commands need an active region.
- **Search and replace.** `C-s`/`C-r` search incrementally (smart case): each key extends the string, `C-s`/`C-r` again finds the next or previous match, and pressing it again after a failing search wraps around the buffer. `C-w` adds the next word, DEL backs up, RET or ESC stops at the match, `C-g` returns to the start, and any other key ends the search and runs. `M-%` is query-replace from point to the end: `y`/SPC replaces, `n`/DEL skips, `!` replaces the rest, `.` replaces and stops, `?` shows help, anything else quits. The whole replacement is one undo step.
- **Files and buffers.** `C-x C-f` find file (prompt starts in the current file's directory, Tab completes), `C-x C-s` save, `C-x C-w` write as, `C-x s` save some buffers, `C-x i` insert file, `C-x C-v` revert, `C-x b` switch buffer, `C-x k` kill buffer, `C-x C-b` list buffers, `C-x Left`/`Right` cycle buffers, `C-x =` show the character and position, `C-x C-c` quit (asking about modified buffers).
- **Undo.** `C-_` (most terminals send it for `C-/` too) and `C-x u` undo, `M-_` redoes. Each command is one undo step, except that typing groups up to about 20 keys.
- **Other.** `M-!` runs a shell command (one-line output goes to the echo area, longer output to a `*Shell Command Output*` buffer), `C-z` and `C-x C-z` suspend, `C-g` cancels.
- **`M-x` commands** (Tab completes the names): `goto-line`, `query-replace`, `query-replace-regexp`, `replace-string`, `replace-regexp`, `save-buffer`, `write-file`, `find-file`, `insert-file`, `kill-buffer`, `switch-to-buffer`, `list-buffers`, `save-some-buffers`, `revert-buffer`, `undo`, `undo-redo`, `set-mark-command`, `exchange-point-and-mark`, `mark-whole-buffer`, `display-line-numbers-mode` (also `linum-mode`), `whitespace-mode`, `font-lock-mode`, `hl-search-mode`, `toggle-case-fold-search`, `count-words`, `what-line`, `shell-command`, `isearch-forward`, `isearch-backward`, `beginning-of-buffer`, `end-of-buffer`, `upcase-region`, `downcase-region`, `kill-region`, `copy-region-as-kill`, `yank`, `delete-trailing-whitespace`, `set-tab-width`, `indent-tabs-mode`, `suspend-emacs`, `save-buffers-kill-emacs`, `kill-emacs`, `help`, `describe-bindings`, `version`. The `-regexp` variants use basic regular expressions, with `&` and `\1`–`\9` in the replacement.
- **Mode line.** `-UUU:**-  NAME  Top|Bot|All|NN%  L<line> C<column>  (C|Shell-script|Makefile|Python|Fundamental)`; `**` means modified and `%%` read-only.

**Under QEMU.** With `-nographic`, as `make run` uses, QEMU multiplexes the serial console and its own monitor and takes `Ctrl-A` as its escape key: `Ctrl-A X` quits QEMU, and `Ctrl-A Ctrl-A` sends one `Ctrl-A` to the guest. So in emacs, type `C-a` as `Ctrl-A Ctrl-A`, or press Home. `Ctrl-]` still enters the kernel monitor: the console takes it before the terminal line discipline, whatever mode the editor has set.

**Not available.** One window per screen (no `C-x 2`, `:split`), no visual block mode, no `fill-paragraph` (`M-q` says so), no keyboard macros in emacs, no syntax highlighting beyond C, shell, make and Python, no wide-character display widths (every UTF-8 character counts as one column), no `SIGWINCH` (the size is re-read only after a suspend), no auto-save, backup files or detection of changes made by others, and undo granularity of a whole command (or a run of typed characters, backspaces included).

### eutils

`eutils` (`user/bin/eutils.c`) is one binary. `main` takes the basename of `argv[0]` and dispatches with a chain of `strcmp`s. The disk image has `/bin/eutils` plus one hard link per name in `EUTILS_LINKS`, made by mkfs's `/path=@/existing` syntax. An unknown name prints `eutils: unknown tool 'X'` and exits with 2. Running `/bin/eutils` directly ends up there too. Some tools (`ln`, `mv`, `cp`, `chmod`, `chown`) are only matched when the argument count is right, so a usage error also produces the "unknown tool" message.

### Network programs

All of them use the BSD socket API from `net.c`, with `resolve_host` for names (`localhost` → 127.0.0.1; `host` or `gateway` → 10.0.2.2; otherwise dotted-quad only).

- **`httpd`**: binds port 80 (`-p`) with `SO_REUSEADDR`, `listen(16)`, and forks a child per connection.
  - The child reads until a blank line (at most 2047 bytes) and parses the method (8-byte buffer) and the path (up to 255 bytes, cut at `?`). Only `GET` and `HEAD` are accepted, and paths containing `..` get 403.
  - `/proc/...` is served straight from procfs; any other path is under the root (`-r`, default `/www`), and `/` means `/index.html`.
  - Directories get an HTML listing. Files are read into a 64 KiB static buffer (larger files are truncated). Responses are HTTP/1.0 with `Connection: close`.
  - It logs one line per request, and the parent reaps children with `WNOHANG` after each `accept`.
- **`rshd`**: port 23 (`-p`). For each connection it forks, `dup2`s the socket onto fds 0-2, writes a greeting and `execv("/bin/esh", {"esh", "-i"})`. There is no authentication and no TTY, so `^C` does not reach remote commands.
- **`echod`**: TCP and UDP on port 7 (`-p`). The parent `poll`s both sockets with a 1 s timeout, answers UDP datagrams itself, and forks a child per TCP connection that echoes until EOF.
- **`nc`**: client (`nc [-u] [-v] host port`) or server (`nc -l [-u] [-k] [-v] port`).
  - `pump` polls the socket and stdin. At EOF on stdin it does `shutdown(SHUT_WR)` (TCP), or for UDP waits up to 1 s for replies.
  - For a UDP listener, replies go to the first sender; stdin read before any datagram has arrived is discarded.
  - `-k` keeps accepting TCP connections, one at a time.
- **`ping`**: raw ICMP socket (root only). It sends echo requests whose payload starts with a send timestamp, matches replies by id, and computes RTT from that timestamp. Options: `-c` count (4), `-i` interval ms (1000), `-s` size (56, clamped to 8..1400), `-W` timeout ms (1000).
- **`ifconfig`**: `NET_IOC_GETIF` by index to list interfaces (addresses in host byte order in `struct netif_req`); `ifconfig IFACE [ADDR] [netmask M] [gw G] [up|down]` looks the interface up by name, edits the request and issues `NET_IOC_SETIF`.
- **`netstat`**: copies `/proc/net/{tcp,udp,dev,arp,stats}` to stdout according to `-t -u -i -a -s` (default `-tu`).

## Concurrency and locking

User processes have one thread each, so the only concurrency inside a process is between the main flow and signal handlers. libc has no locks and none of it is async-signal-safe beyond the bare system call wrappers:

- **`errno` is one global int.** A handler that makes a failing system call overwrites the `errno` the interrupted code was about to read. Handlers should save and restore it.
- **stdio.** Every `FILE` operation in a handler can interleave with, or corrupt, the same stream's buffer in the main program (non-atomic copy-and-advance). `fprintf_fd`/`eprintf` are not safe either: they flush stdout first. Only `write(2)` (with a buffer formatted by `snprintf`) is safe from a handler.
- **Static state in libc**: `strtok`, `gmtime`, `localtime`, `asctime`, `ctime`, `tmpnam` (without a buffer) and `rand` keep static state; use the `_r` forms where they exist.
- **malloc** is not reentrant: the free list is modified without any protection. Never allocate in a handler.
- **Static result buffers**: `inet_ntoa`, `esh`'s `var_get` for computed specials (`RANDOM`, `SECONDS`, ...), and `ps.c`'s `field` return pointers to static storage.

Fork and exec interactions:

- `fork` does not flush any stream. Text that is buffered (on stdout, not yet terminated by `'\n'`; on a fully buffered file, anything) is copied into the child and may be written twice. Programs that fork call `fflush(stdout)` (or `fflush_stdout`, or `fflush(NULL)`) first or only fork after complete lines (`httpd`, `rshd` and `echod` flush explicitly; `sh` flushes its prompt). `system` flushes everything itself.
- `_exit` does not flush. Children that write with `printf` and then `_exit` lose unterminated output; `exit` would also run the parent's `atexit` handlers in the child.
- Descriptors are inherited unless `O_CLOEXEC`/`FD_CLOEXEC` is set (`stress` uses `pipe2(..., O_CLOEXEC)` and relies on `dup2` clearing the flag on the copy). `sh` closes its pipe ends explicitly in both parent and child.
- Ignored signals survive `exec`, while caught ones are reset by the kernel (`signal_exec`). That is why `child_exec` in the shell resets the five signals the interactive shell ignores or catches.
- Job control: the parent and the child both call `setpgid` so the process group exists before either side relies on it. The child also calls `tcsetpgrp` for foreground jobs, which works because `SIGTTOU` is ignored in the shell and only reset in the child after that call.

## Interfaces

### libc API by header

| Header | Contents |
|---|---|
| `unistd.h` | `__syscall`; `read write _exit fork execve execv execvp execl execlp execle pipe pipe2 fcntl getpid getppid brk sbrk sched_yield kill close lseek unlink rmdir link rename chdir getcwd dup dup2 sync fsync ftruncate setpgid getpgid getpgrp setsid getsid getuid geteuid getgid getegid setuid setgid chown access isatty usleep sysconf gethostname ttyname symlink readlink`, `getopt` with `optarg optind opterr optopt`; `STDIN_FILENO`..., `SEEK_*`, `R_OK`..., `_SC_PAGESIZE`, `_SC_CLK_TCK`; Eunuchs extras `sleep_ms(ms)` and `uptime_ms()`. `ftruncate` and `symlink` fail with `ENOSYS` and `readlink` with `EINVAL` (efs has neither). |
| `stdio.h` | `FILE`, `stdin stdout stderr`; `fopen` (modes `r w a`, `+`, `b` ignored, `x` for `O_EXCL`, `e` for `O_CLOEXEC`) `fdopen freopen fclose fflush setvbuf setbuf fileno feof ferror clearerr`; `fgetc getc getchar ungetc fgets getline getdelim fread`; `fputc putc putchar fputs puts fwrite`; `fseek ftell rewind fgetpos fsetpos`; `printf fprintf dprintf sprintf asprintf` and their `v` forms, `snprintf`/`vsnprintf` (macros for `ksnprintf`/`kvsnprintf`); the `scanf` family; `perror remove rename tmpfile tmpnam`; `BUFSIZ` (4096), `EOF`, `_IOFBF _IOLBF _IONBF`; Eunuchs extras `fprintf_fd`, `eprintf(...)`, `fflush_stdout`. No `popen`. |
| `stdlib.h` | `malloc calloc realloc reallocarray free malloc_stats`; `exit _Exit abort atexit system`; `getenv setenv unsetenv putenv environ`; `atoi atol atoll atof strtol strtoul strtoll strtoull strtod strtof strtold`; `abs labs llabs div ldiv lldiv`; `qsort bsearch`; `rand srand random srandom`, `RAND_MAX`; `realpath mkstemp mkdtemp mktemp`; `EXIT_SUCCESS`, `EXIT_FAILURE`, `MB_CUR_MAX` (1). |
| `string.h` | everything in `kstring.h` (`memset memcpy memmove memcmp strlen strnlen strcmp strncmp strchr strrchr strstr strlcpy memchr_inv kstrtoul kisspace kisdigit kisprint`) plus `strcpy stpcpy strncpy strcat strncat strdup strndup strspn strcspn strpbrk strtok strtok_r strsep memchr memrchr strcasecmp strncasecmp strcoll strxfrm strerror`. |
| `errno.h` | `errno`, `strerror` (returns `char *`), all `E*` constants. |
| `fcntl.h` | `open(path, flags, [mode])`, `creat`, `O_*`, `F_*`, `FD_CLOEXEC`, `AT_*`. |
| `sys/stat.h` | `stat fstat mkdir chmod fchmod umask utimensat futimens`, `struct stat` (POSIX `st_mtim`/`st_ctim` timespecs; `st_mtime`, `st_ctime` and `st_atime` are macros for the seconds), `UTIME_NOW`, `UTIME_OMIT`, `S_IS*`. |
| `dirent.h` | `opendir readdir closedir`, fixed-size `struct dirent`, `DT_*`. |
| `sys/wait.h` | `wait waitpid`, `WNOHANG WUNTRACED`, `WIFEXITED WEXITSTATUS WIFSIGNALED WTERMSIG WCOREDUMP WIFSTOPPED WSTOPSIG`. |
| `signal.h` | `sigaction signal sigprocmask sigsuspend pause raise strsignal`, inline `sigemptyset sigfillset sigaddset sigdelset sigismember`, `sigset_t` (32-bit). |
| `termios.h` | `tcgetattr tcsetattr (TCSANOW/TCSADRAIN/TCSAFLUSH) cfmakeraw tcgetpgrp tcsetpgrp isatty ioctl`. |
| `time.h` | `time clock clock_gettime gettimeofday nanosleep sleep`, `gmtime gmtime_r localtime localtime_r timegm mktime difftime asctime asctime_r ctime ctime_r strftime tzset tzname timezone daylight`, `struct tm` (with `tm_isdst`, `tm_gmtoff`, `tm_zone`), `CLOCKS_PER_SEC`. Local time is UTC. |
| `sys/time.h` | `gettimeofday`, `struct timeval`, `struct timezone`. |
| `sys/types.h` | `ssize_t pid_t uid_t gid_t` (16-bit ids) `mode_t off_t ino_t dev_t nlink_t blksize_t blkcnt_t time_t` (64-bit) `suseconds_t clock_t`. |
| `stdint.h`, `inttypes.h` | Exact-, least- and fast-width integer types, `intptr_t`, `intmax_t`, their limits and `INTn_C` macros (LP64); `PRId64`-style format macros, `SCN*` for 32 and 64 bits, `strtoimax`/`strtoumax`. |
| `limits.h` | `CHAR_BIT`, the `char` (unsigned on RISC-V), `short`, `int`, `long`, `long long` limits, `SSIZE_MAX`, `PATH_MAX` (256), `NAME_MAX` (59), `PIPE_BUF`, `ARG_MAX`, `OPEN_MAX` (32), `MB_LEN_MAX`. |
| `ctype.h` | Inline ASCII `isalpha isdigit isalnum isxdigit islower isupper isspace isblank iscntrl isprint isgraph ispunct isascii tolower toupper toascii`. |
| `assert.h` | `assert(e)`, off with `NDEBUG`; may be included again after changing `NDEBUG`. |
| `math.h` | See [Mathematics](#mathematics-mathh). |
| `setjmp.h` | `jmp_buf sigjmp_buf setjmp longjmp _setjmp _longjmp sigsetjmp siglongjmp`. |
| `regex.h` | `regex_t regmatch_t regoff_t`, `regcomp regexec regerror regfree`, `REG_*` flags and errors. |
| `sys/ptrace.h` | `ptrace(request, pid, addr, data)` and, from `uapi/ptrace.h`, the `PTRACE_*` requests, `struct user_regs`, `struct user_fpregs`, `struct ptrace_siginfo`, `struct ptrace_io`, `SI_USER`/`TRAP_*`/`SEGV_*` codes. As in glibc, `PTRACE_PEEKTEXT`/`PEEKDATA` return the word (set `errno` to 0 first to tell a word of -1 from an error); the kernel itself stores it at `*data`. See [14](14-debugger.md). |
| `sys/mman.h` | `mmap munmap mprotect`, `PROT_*`, `MAP_PRIVATE`, `MAP_ANONYMOUS`, `MAP_FAILED`. |
| `poll.h` | `poll(fds, n, timeout_ms)`. |
| `sys/select.h` | `select`, `fd_set` (`FD_SETSIZE` 64), `FD_*` macros. |
| `sys/socket.h` | `socket bind listen accept accept4 connect getsockname getpeername send recv sendto recvfrom setsockopt getsockopt shutdown`, `MSG_NOSIGNAL` (defined here, but not in `uapi/net.h`'s list). |
| `netinet/in.h` | `htons ntohs htonl ntohl` (inline), `TCP_NODELAY`. |
| `arpa/inet.h` | `inet_aton inet_addr inet_ntoa`, `INADDR_NONE`. |
| `netdb.h` | `resolve_host(name, &addr)` (returns 0 or -1; no DNS). |
| `eunuchs.h` | `dbg_mark dbg_break dbg_strace dbg_ps dbg_files dbg_fscrash dbg_vm dbg_profile`, all `SYS_debug` (502) operations. |

### Command reference

Standalone programs (each `user/bin/NAME.c` → `/bin/NAME`):

| Name | Usage | Notes |
|---|---|---|
| `init` | `init [-1] [PROG ARGS...]` | pid 1. Runs `/bin/esh -l` by default and restarts it 200 ms after it exits. `-1`: run once and return its status. |
| `esh` (also `sh`) | `esh`, `esh -c CMDS [NAME ARGS]`, `esh FILE [ARGS]`, `esh -i`, `esh -l` | The shell; see [12-shell.md](12-shell.md). |
| `ls` | `ls [OPTION]... [FILE]...` | GNU-style; see [The `ls` command](#the-ls-command). |
| `cat` | `cat [FILE...]` | stdin with no arguments; 4 KiB reads. |
| `grep` | `grep [-v] [-n] [-i] [-c] PATTERN [FILE...]` | Kernighan & Pike matcher: `c . ^ $ *`. Reads one byte per syscall; lines longer than 1023 bytes are truncated. Exit 0 if any match, 1 if none, 2 for usage errors. |
| `ps` | `ps [options]` | procps-style: dashed options are System V (`-ef`, `-lf`, `-ejH`, `-o pid,args`), undashed are BSD (`aux`, `axjf`), and they mix. See [The `ps` command](#the-ps-command). |
| `more`, `less` | `more [-NiRsFEd] [-NUM] [+NUM \| +/PAT] [FILE...]` | Pager; see [The `more` and `less` pager](#the-more-and-less-pager). |
| `vi`, `view` | `vi [-R] [+N \| + \| +/PAT] [FILE...]` | The editor's vi personality; `view` is read-only. See [The editor](#the-editor-vi-and-emacs). |
| `emacs`, `em`, `edit` | `emacs [+N] [FILE...]` | The same program's emacs personality. |
| `tcc` | `tcc [-c] [-o OUT] [-I DIR] [-D X] [-L DIR] [-l LIB] FILE...`, `tcc -run FILE.c [ARGS]` | TinyCC: compiles, assembles and links C for Eunuchs; see [13](13-toolchain.md). |
| `db` | `db PROG [ARGS]`, `db --args PROG ARGS...`, `db -p PID [PROG]`, with `-ex CMD` and `-batch` | gdb-like source-level debugger on `ptrace`; see [14](14-debugger.md). |
| `make` | `make [-f FILE] [-C DIR] [-nsikBpqd] [VAR=value] [TARGET...]` | POSIX-style make with GNU functions and built-in rules for tcc; see [13](13-toolchain.md#binmake-userbinmakec). |
| `top` | `top` | Raw mode with `VMIN=0`; shows `/proc/uptime`, 10 lines of `/proc/cpus` and 20 of `/proc/threads` every second; `q` or `^C` quits. |
| `ping` | `ping [-c N] [-i MS] [-s SIZE] [-W MS] HOST` | Raw ICMP, root only. |
| `nc` | `nc [-uv] HOST PORT`, `nc -l [-ukv] PORT` | TCP or UDP netcat. |
| `ifconfig` | `ifconfig`, `ifconfig IFACE [ADDR] [netmask M] [gw G] [up\|down]` | Configuring needs root. |
| `netstat` | `netstat [-t] [-u] [-s] [-i] [-a]` | Prints `/proc/net/*`. |
| `httpd` | `httpd [-p PORT] [-r ROOT]` | Default 80 and `/www`; serves `/proc/...` live. `make run` forwards Mac port 8080. |
| `rshd` | `rshd [-p PORT]` | Default 23; unauthenticated `esh -i` per connection. `make run` forwards Mac port 2323. |
| `echod` | `echod [-p PORT]` | TCP and UDP echo, default 7. `make run` forwards Mac UDP port 5555. |
| `utests` | `utests [MODE]` | 46 user-space tests ending in `UTEST RESULT: PASS\|FAIL`. Modes `persist-check`, `crash-commit-write\|check`, `crash-log-write\|check` and `stress[:secs]` are used by `tools/run_tests.py`. |
| `stress` | `stress [-j WORKERS] [-t SECS] [-n ITERS] [-v]` | Defaults: 8 workers, 10 s. Each iteration: an `echo \| cat` pipeline, file I/O in `/tmp`, a COW fork with `mmap`, and 16 KiB over loopback TCP. Prints `STRESS RESULT: PASS\|FAIL`. |
| `segv` | `segv [null\|text\|stack\|illegal\|kernel]` | Crashes on purpose to show the kernel's fault report. |
| `spin` | `spin [MS]` | Busy loop in user mode (default 2000 ms). |
| `exitcode` | `exitcode N [check-args A B]`, `exitcode fdopen FD`, `exitcode env NAME VALUE` | Test helper used by `utests`. |
| `hello` | `hello [ARGS...]` | Prints its pid, parent and arguments. |
| `eutils` | (through the links below) | Multi-call binary. |

`eutils` applets (hard links in `/bin`, list from `EUTILS_LINKS`):

| Name | Usage | Notes |
|---|---|---|
| `echo` | `echo [ARG...]` | No options; no escape processing. |
| `mkdir` | `mkdir [-p] DIR...` | Mode 0755. `-p` (only as the first argument) creates missing parents and accepts an existing directory (`mkdir_p`). Before, `mkdir -p` made a directory named `-p`, which is how the `/usr/src` Makefile's `mkdir -p $(BIN)` left a `-p` behind. |
| `rm` | `rm [-rf] FILE...` | `-r` (or `-R`) removes directories and their contents depth first (`rm_tree`, using `stat`, so it does not follow anything but efs has no symlinks); `-f` ignores missing files; options may be combined (`-rf`, `-fr`) and `--` ends them. Names in a directory are read before any is removed, so the walk does not depend on how the filesystem reorders a directory as it shrinks. There is no `-i`; `rm -r /` is refused. |
| `rmdir` | `rmdir DIR...` | |
| `touch` | `touch [-c] FILE...` | Sets the modification time to now (`utimensat`); creates a missing file (0644) unless `-c`. |
| `ln` | `ln OLD NEW` | Hard links only; exactly two arguments. |
| `mv` | `mv OLD NEW` | `rename`; exactly two arguments; a directory target is not handled. |
| `cp` | `cp FROM TO` | Exactly two arguments; creates `TO` with 0644. |
| `wc` | `wc [-lwc] [FILE...]` | Words are split on space, tab and newline. |
| `stat` | `stat PATH...` | Inode, type, size, blocks, links, mode. |
| `head` | `head [-n N \| -N] [FILE]` | Default 10 lines; reads a byte at a time. |
| `tail` | `tail [-n N \| -N] [FILE]` | Reads the whole input into memory. |
| `tee` | `tee [FILE]` | One file at most. |
| `chmod` | `chmod OCTAL FILE...` | Octal modes only. |
| `chown` | `chown USER[:GID] FILE...` | User by name or uid from `/etc/passwd`; group numeric only (otherwise -1). |
| `env` | `env [NAME=VALUE...] [CMD ARGS...]` | Prints the environment, or runs a command with additions. |
| `id` | `id` | uid, gid, euid, egid; names only for 0 and 1000. |
| `crc32` | `crc32 [FILE...]` | zlib CRC-32 and length (stdin without arguments). |
| `profile` | `profile start [HZ] \| stop \| clear \| report \| dump [HOSTPATH] \| run CMD...` | Kernel sampling profiler through `dbg_profile`; `report` prints `/proc/profile`; `dump` needs semihosting (default `build/profile.bin`). |
| `true`, `false` | | Exit 0 and 1. |
| `yes` | `yes [STRING]` | Forever. |
| `kill` | `kill [-SIG\|-N] PID...`, `kill -l` | Negative pid = process group. Names: `HUP INT QUIT KILL USR1 SEGV USR2 PIPE ALRM TERM CHLD CONT STOP TSTP`. Job specs need the shell's `kill`. |
| `date` | `date` | UTC from `CLOCK_REALTIME` via `gmtime_r`. |
| `dmesg` | `dmesg` | Copies `/dev/kmsg`. |
| `free` | `free` | Copies `/proc/meminfo`. |
| `sleep` | `sleep SECS[.FRACTION]` | Millisecond resolution (three fraction digits). |
| `strace` | `strace CMD [ARGS...]` | `dbg_strace(1)` then `execvp`; the trace goes to the kernel log/console. |
| `su` | `su [USER]` | `setgid`+`setuid` from `/etc/passwd` (only root can switch), `chdir` home, sets `HOME`, `USER`, `LOGNAME` and `SHELL`, execs the user's shell from `/etc/passwd`. No password check. |
| `dbg` | `dbg break \| mark TEXT \| vm \| files \| ps \| fscrash POINT` | Front end for `SYS_debug`. |
| `uname` | `uname` | Prints `Eunuchs 0.1 riscv64`. |
| `clear` | `clear` | ANSI home + clear screen. |

### Files in the disk image

`FS_FILES` in the Makefile, processed in order by `build/tools/mkfs` (image size `FS_MB` = 32 MiB):

| Path | Source |
|---|---|
| `/bin/<prog>` for every `user/bin/*.c` and every directory `user/bin/<prog>/` (so also `/bin/db`, `/bin/edit`, `/bin/esh`, `/bin/make`), and `/bin/tcc` | `build/user/stripped/<prog>` (DWARF stripped, symbols kept) |
| `/bin/sh` | hard link to `/bin/esh` |
| `/bin/less` | hard link to `/bin/more` |
| `/bin/vi`, `/bin/view`, `/bin/emacs`, `/bin/em` | hard links to `/bin/edit` |
| `/mnt/`, `/mnt/host/` | empty; the Mac's shared directory is mounted on `/mnt/host` under `make run` ([06](06-filesystems.md#9pfs-host-directories)) |
| `/usr/include/` | `user/include/` and `lib/include/` (the headers tcc compiles against) |
| `/usr/lib/crt1.o`, `crti.o`, `crtn.o`, `libc.a` | the start-up files and the libc as an archive, from `build/sysroot` |
| `/usr/lib/tcc/` | `libtcc1.a`, `runmain.o` and tcc's own headers |
| `/usr/src/` | the sources of every program (`user/bin/`) and `user/usr-src/Makefile` |
| `/bin/<applet>` for every `EUTILS_LINKS` name | hard link to `/bin/eutils` (`=@/bin/eutils`) |
| `/etc/motd`, `/etc/passwd`, `/etc/group`, `/etc/hostname`, `/etc/eshrc` | `user/etc/` |
| `/dev/`, `/proc/` | empty mount points |
| `/tmp/` | mode 0777 |
| `/www/`, `/www/index.html` | `user/www/index.html` |
| `/home/`, `/home/guest/` | `/home/guest` mode 0755, owned by 1000:1000 |

The image is rebuilt whenever any program changes, which discards files created at run time. Under `make run`, `/home` is a separate disk (`home.img`, holding `/guest` owned by 1000:1000) that is kept across rebuilds; it hides the image's own `/home/guest` ([06](06-filesystems.md#boot-and-mounting)).

`/etc/passwd` has the standard seven fields. `lookup_user` in `eutils` reads the first 1023 bytes (for `su`, `chown` and `id`); `esh` reads it for user names, home directories and `~user`:

```
root:x:0:0:root:/:/bin/esh
guest:x:1000:1000:guest:/home/guest:/bin/esh
```

### Adding a program or an applet

**A new program:**
1. Create `user/bin/foo.c` with `int main(int argc, char **argv)`. Include only headers from `user/include` (and `uapi/`).
2. Run `make`. The wildcard in `UPROGS` builds `build/user/bin/foo`, strips it, and `FS_FILES` puts it at `/bin/foo`.
3. Keep stacks small (8 MiB total), and flush stdio before `fork` or `_exit`. Floating point is fine.

**A new eutils applet:**
1. Add a `if (!strcmp(prog, "foo")) { ... }` branch in `eutils.c` `main`, before the final `else` chain, or as a helper function like `head`/`tail`.
2. Add `foo` to `EUTILS_LINKS` in the Makefile.
3. Update the comment at the top of `eutils.c` and the tables here.

**A new libc function or system call wrapper:** add the code to the file for its area (`libc.c` for system call wrappers, `net.c` for networking, `stdio.c`, `stdlib.c`, `strings.c`, `math.c`, `time.c`, `unistd2.c`, ...; a new file in `user/lib/` is picked up by the `ULIB` wildcard) and the prototype to the right header in `user/include`. A new system call also needs an entry in `SYSCALLS` in `lib/include/uapi/syscall.h` and a kernel implementation. A new shell builtin is a function in `user/bin/esh/builtins.c` plus an entry (with its one-line help) in the `builtins[]` table.

**A program from several files:** put the sources in `user/bin/NAME/`; the `UMULTI` rule links every `.c` there into `/bin/NAME` (`esh` and `edit` are built this way). Extra names are hard links added to `FS_FILES`, as for `vi`.

## Debugging and observability

- **Fault reports.** When a program dies from a fault it does not handle, `user_fault` (`kernel/proc/proc.c`) logs:
  - the signal and the cause, with a diagnosis: `NULL pointer dereference` (address below one page), `kernel address`, `stack overflow (just below the 8192 KiB stack)` (within 1 MiB below the stack), `VMA 'text' is r-x`, or `address not in any mapping`. Illegal instructions show the instruction bits;
  - `pc`/`ra`/`sp`, with `pc` symbolized;
  - a symbolized user backtrace (`proc_user_backtrace`, up to 64 frames, recursion collapsed), which needs the frame pointers that `UCFLAGS` keep;
  - the memory map.

  `segv` shows each kind. The shell then prints the signal description (`esh: segmentation fault (core dumped)  segv null`) and sets `$?` to `128 + sig`.
- **strace.** `strace CMD` (eutils applet), `dbg_strace(1|2)` from code (2 also follows forks), or `strace <pid> [on|off|follow]` in the monitor. One line per call, decoded with the format strings in `SYSCALLS`, e.g. `[4 hello] write(1, "hello ..."..., 49) = 49`; errors show the `E*` name and message. The output goes to the kernel log/console, not the program's stderr.
- **`dbg_*` hooks** (`eunuchs.h`, also the `dbg` applet):
  - `dbg_mark(msg)`: a trace event plus a log line. `utests` marks each test, and `malloc_die` marks aborts;
  - `dbg_break()`: stop in the kernel monitor with this process current;
  - `dbg_ps()`, `dbg_files()`, `dbg_vm()`: dump the process table, this process's descriptors, or its memory map to the console;
  - `dbg_fscrash(point)`: crash-consistency testing;
  - `dbg_profile(op, arg)`: the profiler (`profile run CMD` is the easy way to use it).
- **procfs.** `/proc/<pid>/status` (credentials, pgid/sid, stopped, signal masks and dispositions), `maps`, `fds`, `stack` (kernel stack), `ustack` (symbolized user backtrace of a live process), `cmdline`; `/proc/self`.
- **Monitor** (`Ctrl-]`): `procs`, `ubt <pid>` (user backtrace from the saved trap frame), `vm <pid> [va]`, `files <pid>`, `strace <pid>`, `kill <pid>`, `tty` (foreground group, termios, queued input). `dbg_break()` stops in the monitor; `c` continues. An `ebreak` is a SIGTRAP (fatal unless traced or handled).
- **`db`**: a source-level debugger for user programs, built on `ptrace` ([14](14-debugger.md)).
- **GDB.** `make debug` then `make gdb`; `eu-procs` lists processes. For source-level debugging of a program, `add-symbol-file build/user/bin/<prog>` (no offset is needed because every program is linked at `0x10000`). Since all programs share that address range, a breakpoint on a user address fires in whichever process is running there, so combine it with a condition or use `dbg_break()` in the program instead. `__restore_rt` has no CFI (`.eh_frame` is discarded anyway), so GDB may stop unwinding at a signal handler's caller.
- **Profiling.** `profile run CMD` gives a quick report with user functions symbolized; `profile dump` plus `make flamegraph` on the host symbolizes user frames from `build/user/bin`.
- **Heap.** `malloc_stats()` prints bytes in use and heap size; `/proc/<pid>/maps` shows the `heap` VMA. A `0x6b6b6b6b...` value in a register or a crash address almost always means use after free.
- **Shell.** See [12-shell.md](12-shell.md#debugging-and-observability): `set -x`, `whence -v`, `typeset -p`, `jobs -l`.

## Limitations and known issues

libc:

- **`malloc` reuses freed memory immediately** (first fit from the lowest address), which can hide use-after-free bugs, and the `0x6b` poison is written but never checked. It never shrinks the heap.
- **`setenv` does not check `malloc`'s result,** and `unsetenv` leaks the strings `setenv` allocated.
- **stdio.**
  - Nothing is flushed on `_exit`, a crash or `fork` (see Concurrency).
  - stdout is line-buffered even into a pipe or file, which costs one `write` per line for programs that print a lot.
  - `ungetc` gives multi-character pushback only by backing up over characters still in the read buffer (which is what `scanf`'s read-ahead needs); pushing back a character that is not the one just read works once.
  - Writing to a `+` stream right after reading it to the end of its buffer, without an `fseek` between them, can write stale read-ahead bytes again (`fflush` resets the buffer only when unread data remains). ISO C requires the `fseek`; POSIX allows a `fflush` instead, which takes the broken path.
  - `vfprintf` returns -1 while an earlier error is still set on the stream; `fread` does not check `size * n` for overflow; `freopen` keeps the old buffering mode and does not accept a NULL path.
  - No `popen`/`pclose`, `fmemopen` or `open_memstream`; no wide-character I/O.
- **`execvp`** builds up to 255-byte paths, but the kernel accepts only 128-byte paths in `execve`. It returns immediately on any error other than `ENOENT` (e.g. `EACCES` in an early `PATH` entry), and an empty `PATH` component becomes `/file` instead of the current directory.
- **Not reentrant**: `errno`, stdio buffers, malloc and static result buffers (see Concurrency).
- **Small inaccuracies**: `dup2(fd, fd)` returns `fd` without checking that it is open; `gmtime_r` is wrong for negative times, and so are `timegm` and `mktime`, which normalize through it; `kstrtoul` (the kernel helper still exported through `string.h`) does not detect overflow or set `errno`; `strtod` sets `ERANGE` on overflow but not on underflow; `gethostname` with a zero length reads `(size_t)-1` bytes; `select` does not update the timeout; `fcntl`/`ioctl` always read a third argument even when the caller passed none.
- **Floating point and maths.**
  - `printf` produces at most 17 significant digits (the rest are zeros), so it does not print the exact decimal value of large doubles as glibc does; `%a` with a precision rounds half up.
  - `math.c` is slow (software `long double`). Trigonometric arguments above 2⁶⁰ are first reduced modulo 2π with `fmodl`, which is only as exact as the `long double` value of 2π (no Payne–Hanek reduction), so `sin(1e300)` is not correctly rounded. `log1p` loses about three digits for |x| ≥ 1e-5.
  - `frexpl` halves one step at a time, so `log` of a value near 1e308 takes about a thousand software divisions.
- **Regular expressions.**
  - Leftmost-first, not POSIX leftmost-longest: alternatives are taken in order (`a|ab` on `ab` matches `a`), so results can differ from other systems' `regexec`.
  - Only 9 groups are reported. A pattern that needs more than 2,000,000 backtracking steps at a start position silently fails to match there instead of returning an error, and matching recurses deeply on long subjects.
  - In ERE, an unmatched `)` is taken as a literal rather than an error.
- **Header inconsistencies**: `WUNTRACED`/`WNOHANG` are defined in both `sys/wait.h` and the uapi headers; `MSG_NOSIGNAL` exists only in `sys/socket.h`; the `SA_RESTART` comment in `uapi/signal.h` contradicts the kernel's behaviour.

Commands:

- **`tail -n 0`** prints the whole input: the loop `++lines == n` never matches 0. `tail` also ignores a failed `realloc`.
- **Usage errors in `ln`, `mv`, `cp`, `chmod` and `chown`** produce `eutils: unknown tool 'X'` because the argument-count check is part of the dispatch condition.
- **Commands read one byte per system call**: `head`, `grep` (they could use stdio now), and `esh` when reading commands from a pipe or the `read` builtin (so that input meant for later commands stays unread). `top` writes one byte per call. This is slow, and very visible under `strace`.
- **The editor** (beyond the missing features listed in [its section](#the-editor-vi-and-emacs)):
  - `:{range}w FILE` writes the whole buffer, and `:w !cmd` writes a file named `!cmd` (use `:{range}!cmd` to filter instead). `:g` runs only `d`, `s` and `p` (the default, which reports the count).
  - Macros, `.` and `:normal` cannot drive the `:` and `/` prompts (the prompt reads the terminal directly), so a recorded `:s///` waits for the user on replay. `N.` repeats the whole previous command N times (`3dd` then `2.` deletes six lines). `f`, `t` and `r` take one byte, not one UTF-8 character.
  - Registers `"1`–`"9`, `"_`, `"-` and appending with `"A` are not implemented; any other register name means the unnamed one. Marks do not move when lines are inserted or deleted above them.
  - In emacs, undoing back to the saved text does not clear the modified flag.
- **Missing commands**: there is no `basename` or `dirname` (in esh use `${f##*/}` and `${f%/*}`, or the `:t` and `:h` modifiers), and no `find`, `xargs`, `sed`, `sort`, `diff` or `cmp`.
- **`lookup_user` reads only 1023 bytes** of `/etc/passwd` (so does `ps`'s user and group lookup, with 2047).
- **`httpd`**: files over 64 KiB are truncated; `HEAD` replies say `Content-Length: 0`. The directory-listing loop stops only when `len` reaches `sizeof(body) - 300`, but one entry can add almost 400 bytes, so a listing that long could make `sizeof(body) - len` wrap and overflow `body`. An empty path (`GET  HTTP/1.0`) reads `path[-1]`.
- **`echod`'s header comment** says it logs every connection and datagram; the code logs nothing after start-up. **`eutils.c`'s header comment** omits `crc32` and `profile`.
- **`su` has no password check** (it relies on `setuid` failing for non-root), and **`rshd` has no authentication**. Both are deliberate for a local VM. A remote `esh -i` has no terminal, so there is no `^C`, job control or line editing.

## Potential improvements

1. **Stronger allocator checks** — Neighbouring free blocks are merged now; next, delay reuse (a FIFO quarantine of recently freed blocks, like the kernel slab). Verify the `0x6b` poison on allocation (`memchr_inv`) to report writes after free. Add a tail canary for overruns. This turns malloc from a leak-prone list into a real use-after-free and overflow detector. Impact: high. Effort: medium.
2. **Flush stdio before `fork`** — Have the `fork` wrapper call `fflush(NULL)` first (as `system` already does). Buffered output is otherwise duplicated in the child, a common source of confusing test logs. Impact: medium. Effort: small.
3. **Use stdio in the line readers, and regex in `grep` and `more`** — Rewrite `grep` and `head` on `fgets`/`getline` (they still make one system call per byte), and give `grep` (`-E`, `-F`) and the pager's search the libc's `regcomp`. Impact: medium. Effort: small.
4. **Fix `eutils` argument handling** — Separate "which tool" from "is the usage right" so `cp a` prints a usage message. Make `tail -n 0` print nothing and check `realloc`. Let `mv`/`cp` accept a directory target. Impact: low. Effort: small.
5. **Harden `httpd`** — Bound the directory listing by the remaining space before each `snprintf`, handle an empty path, stream files instead of truncating at 64 KiB, and send the real `Content-Length` for `HEAD`. It is the one program reachable from the host network by default. Impact: medium. Effort: small.
6. **Keep `execvp` and kernel limits consistent** — Return `ENAMETOOLONG` early for paths over the kernel's 128 bytes, keep searching `PATH` after `EACCES` (remembering the error), and treat empty `PATH` components as `.`. Impact: low. Effort: small.
7. **Make signal handlers safer to write** — Save and restore `errno` around handler invocation in a libc trampoline, and document which libc functions are safe in handlers. Optionally add `SA_RESTART` to `signal()` to match BSD/glibc behaviour. Impact: low. Effort: small.
8. **POSIX leftmost-longest matching** — Track the longest match (and POSIX subexpression rules) at each start position, or compile to a Pike VM / NFA with submatch tracking; that also bounds the time, removing the step limit and the deep recursion. Reject unknown character class names. Impact: medium. Effort: medium.
9. **Editor: drive prompts from the key queue** — Let macros, `.` and `:normal` feed the `:` and `/` prompts through the same queue as other keys, honour ranges in `:w` and support `:w !cmd`. Impact: medium. Effort: small.
10. **Editor: the missing big pieces** — Split windows (`C-x 2`/`:split`) sharing buffers; visual block mode; numbered and append registers; `fill-paragraph`/`gq`; emacs keyboard macros (`C-x (`); `SIGWINCH` handling (once the TTY sends it); marks that follow edits; persistent undo across saves (clearing the modified flag at the saved state). Impact: medium. Effort: large.
11. **Editor: safer saving** — `fsync` the temporary file before the `rename` (a no-op in the kernel today, but correct once `fsync` is real), keep the owner, honour the umask for new files, and warn when the file changed on disk since it was read (compare `st_mtime`). Impact: medium. Effort: small.
12. **Unwind info for `__restore_rt` and correct header comments** — Add `.cfi_signal_frame`-style annotations (and keep a minimal `.eh_frame` for it) so GDB can unwind through signal handlers. Fix the stale comments in `uapi/signal.h` (`SA_RESTART`), `echod.c` and `eutils.c`. Impact: low. Effort: small.
