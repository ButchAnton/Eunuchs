# User space: libc, shell and commands

Everything that runs in U-mode: a small static C library (`user/lib/`), the headers programs compile against (`user/include/` plus the shared ABI in `lib/include/uapi/`), the shell (`esh`, also `/bin/sh`; it has [its own document](12-shell.md)), `init`, the multi-call `eutils` binary and about twenty other programs. Every program is a static ELF linked at `0x10000` with the same startup code and the whole libc, and is copied into the efs disk image at build time. The libc is deliberately small and debugging-oriented: system call wrappers that set `errno`, one line-buffered stdout, a first-fit `malloc` with magic numbers and poisoning, and thin wrappers for signals, sockets and termios.

## Responsibilities and scope

What this subsystem does:

- **Program startup and exit.** `crt0.S` turns the initial stack built by the kernel's `do_exec` into `main(argc, argv)` with `environ` set; `exit` flushes stdout and calls `SYS_exit`.
- **System call wrappers.** One generic `__syscall` stub (`syscall.S`) plus a C wrapper per call (`libc.c`, `net.c`) that converts `-errno` returns into `-1` and `errno`.
- **Library services.** Formatted output (the kernel's `printf` core, shared through `lib/printf.c`), string functions (`lib/string.c`, also shared), heap allocation, environment handling, directory reading, time conversion, `poll`/`select`, IPv4 address helpers, termios helpers and the Eunuchs-specific debug hooks (`dbg_*`).
- **The shell.** `esh`, a zsh-like shell with a line editor (`user/bin/esh/`), is described in [12-shell.md](12-shell.md). This document covers only how it fits in.
- **Commands.** `init` (pid 1), file and text tools, process tools, network tools and servers, and test programs (`utests`, `stress`, `segv`, ...).
- **Disk-image contents.** Which files and links go into `build/fs.img` (`Makefile`: `UPROGS`, `EUTILS_LINKS`, `FS_FILES`).

What it does not do:

- No dynamic linking, no shared libraries, no threads (the kernel gives every process exactly one thread), no floating point (`-march=rv64imac`; the kernel kills a process that executes an FP instruction).
- No `FILE *` stdio, no `fopen`, no `scanf`, no locale, no `qsort`, no `atexit`, no constructors.
- No DNS (`resolve_host` knows numeric addresses and three fixed names).
- The kernel side of every mechanism used here (exec, signals, TTY line discipline, sockets, procfs) belongs to other subsystems. This document only describes the user-side contract; for example, the signal frame is built by `kernel/proc/signal.c` (`setup_frame`), and the initial stack by `kernel/proc/proc.c` (`do_exec`).

## Source map

| File | Role |
|---|---|
| `user/lib/crt0.S` | `_start`: sets `gp` and `fp`, computes `argc`/`argv`/`envp` from `sp`, calls `__libc_start`. |
| `user/lib/syscall.S` | `__syscall(nr, a0..a5)` (shuffles arguments into `a7`, `a0`-`a5`, `ecall`) and `__restore_rt` (signal return trampoline). |
| `user/lib/libc.c` | Most of libc: process, file, credential, time, environment, terminal, signal wrappers; stdio; `strcpy`/`strcat`/`strtol`/`atoi`; `malloc` family; `exit`, `__libc_start`. |
| `user/lib/net.c` | Socket wrappers, `poll`, `select` (on top of `ppoll`), `inet_aton`/`inet_addr`/`inet_ntoa`, `resolve_host`. |
| `lib/printf.c` | `kvcbprintf` (callback-based formatter), `kvsnprintf`, `ksnprintf`. Shared with the kernel and host tests (`tests/host/test_printf.c`). |
| `lib/string.c` | `mem*`, `str*`, `strlcpy`, `memchr_inv`, `kstrtoul`. Shared with the kernel. |
| `lib/include/fmt.h`, `lib/include/kstring.h` | Prototypes for the two shared files; `kisspace`/`kisdigit`/`kisprint` inlines. |
| `lib/include/uapi/syscall.h` | `SYSCALLS` X-macro (number, name, argument count, strace format), `SYS_*` enum, `DBG_*` operations, `PROT_*`/`MAP_*`, `WNOHANG`. |
| `lib/include/uapi/errno.h` | `ERRNOS` X-macro (Linux numbers and messages). |
| `lib/include/uapi/signal.h` | `SIGNALS` X-macro (number, default action, description), `struct sigaction`, `SA_*`, `SIG_*`. |
| `lib/include/uapi/fs.h` | `O_*`, `F_*`, `S_*`, `struct stat`, `struct dirent`, `struct timespec`/`timeval`, `PATH_MAX`, `NAME_MAX`. |
| `lib/include/uapi/net.h` | Socket constants, `struct sockaddr_in`, `struct pollfd`, `POLL*`, `struct netif_req` and the `NET_IOC_*` ioctls. |
| `lib/include/uapi/termios.h` | `struct termios` (Linux layout), flags, `TCGETS`/`TIOC*` ioctl numbers, `struct winsize`. |
| `user/include/*.h` | POSIX-style headers: `unistd.h`, `stdio.h`, `stdlib.h`, `string.h`, `errno.h`, `fcntl.h`, `signal.h`, `dirent.h`, `termios.h`, `time.h`, `poll.h`, `netdb.h`, `eunuchs.h`, `sys/{mman,select,socket,stat,wait}.h`, `netinet/in.h`, `arpa/inet.h`. |
| `user/user.ld` | Linker script: text at `0x10000`, page-aligned r-x / r-- / rw- segments. |
| `user/bin/init.c` | pid 1: prints `/etc/motd`, runs and restarts `/bin/esh -l`, reaps orphans; `-1` mode for tests. |
| `user/bin/esh/*.c` | The shell (one program from several files; see [12-shell.md](12-shell.md)). |
| `user/bin/eutils.c` | Multi-call binary: 32 small tools chosen by `argv[0]`. |
| `user/bin/{ls,cat,grep,ps,top}.c` | Standalone file, text and process tools. |
| `user/bin/{ping,nc,ifconfig,netstat}.c` | Network clients and configuration. |
| `user/bin/{httpd,rshd,echod}.c` | Network servers. |
| `user/bin/{utests,stress,segv,spin,exitcode,hello}.c` | Test and demonstration programs. |
| `user/etc/motd`, `user/etc/passwd`, `user/etc/hostname`, `user/etc/eshrc` | Message of the day; user database (`root` 0, `guest` 1000, both with `/bin/esh`); host name for prompts; the shell's system-wide start-up file. |
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

### stdout buffer

`libc.c` has one static buffer: `outbuf[512]` and `outlen`. All of `printf`, `vprintf`, `putchar` and `puts` go through `out_char`, which appends one byte and calls `fflush_stdout` when the byte is `'\n'` or the buffer is full. So stdout is line-buffered whether or not it is a terminal.

`fflush_stdout` is called explicitly by `exit`, `getchar` (so a prompt appears before input is read), and `fprintf_fd` when its target is fd 1. It is **not** called by `_exit`, on a fatal signal, or before `fork`.

`fprintf_fd(fd, ...)` (and the `eprintf(...)` macro, which is `fprintf_fd(2, ...)`) is unbuffered: it formats into a 512-byte stack buffer and issues one `write`. Output longer than 511 bytes is truncated. `snprintf`/`vsnprintf` are macros for the shared `ksnprintf`/`kvsnprintf`.

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

`UCFLAGS` (Makefile) compile user code with `-march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding -fno-builtin -nostdlib -nostdinc -isystem <gcc include> -fno-pic -fno-stack-protector -fno-omit-frame-pointer -g -O1 -Wall -Wextra -Werror -std=gnu11 -Iuser/include -Ilib/include`. Two points matter:

- `-fno-omit-frame-pointer` is required: the kernel's user backtraces (`proc_user_backtrace`) and the profiler walk frame records (`{prev fp, ra}` at `fp - 16`).
- `-fno-builtin` and `-fno-tree-loop-distribute-patterns` keep GCC from turning loops into calls to functions libc lacks.

`ULIB` is a list of objects, not an archive: `crt0.o syscall.o libc.o printf.o string.o net.o`. Every program links all of them (`-nostdlib -static -T user/user.ld ... -lgcc`). `UPROGS` is `$(wildcard user/bin/*.c)`, so every C file in `user/bin` becomes a program. Each program is then copied with `objcopy --strip-debug` into `build/user/stripped/`; the stripped copy (symbol table kept for kernel backtraces) goes into the disk image, and the full one in `build/user/bin/` is for GDB and the profiler.

### Startup: `_start` to `main`

1. `_start` (`crt0.S`) loads `gp` from `__global_pointer$`, sets `fp = 0` (terminates the frame-pointer chain, which is what stops `proc_user_backtrace`), computes `argc`, `argv`, `envp` from `sp`, and calls `__libc_start`. If that ever returned, `_start` would spin in `1: j 1b`.
2. `__libc_start` (`libc.c`) sets `environ = envp` and calls `exit(main(argc, argv))`. `main` is declared with two arguments; programs use `environ` or `getenv` for the environment.
3. There is no BSS clearing in user space. The kernel's ELF loader (`elf_load`) copies the file bytes of each segment eagerly, and the rest of the segment (the BSS) stays zero.
4. `exit` calls `fflush_stdout` then `_exit`, which issues `SYS_exit` and loops forever in case it returns.

### A system call

`read(fd, buf, n)` expands to `ret(__syscall(SYS_read, fd, buf, n, 0, 0, 0))`. `__syscall` moves its first argument into `a7` and shifts the other six down into `a0`-`a5`, then executes `ecall`; the result comes back in `a0`. Because the stub follows the normal calling convention, any wrapper is a single C line. To add a wrapper: make sure the call is in the `SYSCALLS` X-macro in `uapi/syscall.h` (which also makes `strace` decode it), add the C function with `SCn(...)` and `ret()`, and declare it in the matching header.

### Signals

`sigaction` (`libc.c`) copies the caller's `struct sigaction` and always sets `sa_restorer = __restore_rt`, then calls `SYS_rt_sigaction`. The kernel's `setup_frame` refuses to deliver to a handler without a restorer and kills the process ("no restorer"), so a program must install handlers through these wrappers (or set `sa_restorer` itself).

Delivery, from the user's point of view:

1. On the way back to user mode, the kernel pushes a `struct sigframe` (magic `"SIGFRAME"`, all 32 registers, `sepc`, the mask to restore, the signal number) below the interrupted `sp`, aligned to 16 bytes.
2. It sets `a0 = sig`, `ra = __restore_rt`, `sp` = the frame, `pc` = handler.
3. The handler returns into `__restore_rt` (`syscall.S`), which does `li a7, 139` (`SYS_rt_sigreturn`) and `ecall`. The kernel checks the magic, restores registers and mask, and resumes the interrupted code.

The rest of the API:

- `signal(sig, h)` is `sigaction` with `sa_flags = 0` and an empty mask. Unlike glibc's `signal`, it does not set `SA_RESTART`, so slow calls interrupted by the handler fail with `EINTR`.
- `sigprocmask` and `sigsuspend` are direct wrappers. `sigset_t` is `uint32_t`, and `sigemptyset`/`sigaddset`/... are inline bit operations (`SIGBIT(s)` = `1u << s`).
- `pause` reads the current mask with `sigprocmask(SIG_BLOCK, NULL, &cur)` and calls `sigsuspend(&cur)`.
- `raise(sig)` is `kill(getpid(), sig)`.

The kernel honours `SA_RESTART`, `SA_NODEFER` and `SA_RESETHAND` (`signal_deliver`). The comment on `SA_RESTART` in `uapi/signal.h` ("interrupted calls still fail with EINTR") is out of date.

### Formatted output

`printf` → `vprintf` → `kvcbprintf(out_char, NULL, fmt, ap)`. `kvcbprintf` (`lib/printf.c`) parses `%[flags][width][.prec][length]conv` with:

- flags `- + space # 0`;
- width and precision, including `*`;
- lengths `hh h l ll z t j`;
- conversions `d i u o x X p s c %`.

`%p` always prints `0x`. `%s` of `NULL` prints `(null)`. An unknown conversion is printed literally (`%` and the character) so the mistake is visible. There is no floating point. `kvsnprintf` always NUL-terminates when `size > 0` and returns the length the output would have had (C99 semantics), so callers that do `n += snprintf(...)` must clamp.

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
- **stdout.** `printf`, `puts` and `putchar` in a handler can interleave with, or corrupt, the main program's `outbuf`/`outlen` (non-atomic append-and-increment). `write(2)` or `fprintf_fd`, which only uses its own stack buffer, is safe from a handler.
- **malloc** is not reentrant: the free list is modified without any protection. Never allocate in a handler.
- **Static result buffers**: `inet_ntoa`, `esh`'s `var_get` for computed specials (`RANDOM`, `SECONDS`, ...), and `ps.c`'s `field` return pointers to static storage.

Fork and exec interactions:

- `fork` does not flush stdout. Text that is buffered but not yet terminated by `'\n'` is copied into the child and may be printed twice. Programs that fork call `fflush_stdout` first or only fork after complete lines (`httpd`, `rshd` and `echod` flush explicitly; `sh` flushes its prompt).
- `_exit` does not flush. Children that write with `printf` and then `_exit` lose unterminated output.
- Descriptors are inherited unless `O_CLOEXEC`/`FD_CLOEXEC` is set (`stress` uses `pipe2(..., O_CLOEXEC)` and relies on `dup2` clearing the flag on the copy). `sh` closes its pipe ends explicitly in both parent and child.
- Ignored signals survive `exec`, while caught ones are reset by the kernel (`signal_exec`). That is why `child_exec` in the shell resets the five signals the interactive shell ignores or catches.
- Job control: the parent and the child both call `setpgid` so the process group exists before either side relies on it. The child also calls `tcsetpgrp` for foreground jobs, which works because `SIGTTOU` is ignored in the shell and only reset in the child after that call.

## Interfaces

### libc API by header

| Header | Contents |
|---|---|
| `unistd.h` | `__syscall`; `read write _exit fork execve execv execvp pipe pipe2 fcntl getpid getppid brk sbrk sched_yield kill close lseek unlink rmdir link rename chdir getcwd dup dup2 sync setpgid getpgid getpgrp setsid getsid getuid geteuid getgid getegid setuid setgid chown access`; Eunuchs extras `sleep_ms(ms)` and `uptime_ms()`. Types `ssize_t`, `pid_t`, `uid_t`/`gid_t` (16-bit). |
| `stdio.h` | `printf vprintf fprintf_fd puts putchar getchar fflush_stdout`, `snprintf`/`vsnprintf` (macros for `ksnprintf`/`kvsnprintf`), `eprintf(...)`, `EOF`. No `FILE`. |
| `stdlib.h` | `malloc calloc realloc free malloc_stats exit atoi strtol getenv setenv unsetenv environ`. |
| `string.h` | everything in `kstring.h` (`memset memcpy memmove memcmp strlen strnlen strcmp strncmp strchr strrchr strstr strlcpy memchr_inv kstrtoul kisspace kisdigit kisprint`) plus `strcpy strcat`. No `memchr`, `strncpy`, `strdup`. |
| `errno.h` | `errno`, `strerror`, all `E*` constants. |
| `fcntl.h` | `open(path, flags, [mode])`, `creat`, `O_*`, `F_*`, `FD_CLOEXEC`, `AT_*`. |
| `sys/stat.h` | `stat fstat mkdir chmod fchmod umask`, `struct stat` (`st_mtime`/`st_ctime` in ns), `S_IS*`. |
| `dirent.h` | `opendir readdir closedir`, fixed-size `struct dirent`, `DT_*`. |
| `sys/wait.h` | `wait waitpid`, `WNOHANG WUNTRACED`, `WIFEXITED WEXITSTATUS WIFSIGNALED WTERMSIG WCOREDUMP WIFSTOPPED WSTOPSIG`. |
| `signal.h` | `sigaction signal sigprocmask sigsuspend pause raise strsignal`, inline `sigemptyset sigfillset sigaddset sigdelset sigismember`, `sigset_t` (32-bit). |
| `termios.h` | `tcgetattr tcsetattr (TCSANOW/TCSADRAIN/TCSAFLUSH) cfmakeraw tcgetpgrp tcsetpgrp isatty ioctl`. |
| `time.h` | `time clock_gettime gettimeofday nanosleep sleep gmtime_r`, `struct tm` (no `tm_isdst`), `time_t` (64-bit). |
| `sys/mman.h` | `mmap munmap`, `PROT_*`, `MAP_PRIVATE`, `MAP_ANONYMOUS`, `MAP_FAILED`. |
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
| `ls` | `ls [-l] [PATH...]` | `-l`: mode, links, size, inode (not owner). One name per line, in directory order. |
| `cat` | `cat [FILE...]` | stdin with no arguments; 4 KiB reads. |
| `grep` | `grep [-v] [-n] [-i] [-c] PATTERN [FILE...]` | Kernighan & Pike matcher: `c . ^ $ *`. Reads one byte per syscall; lines longer than 1023 bytes are truncated. Exit 0 if any match, 1 if none, 2 for usage errors. |
| `ps` | `ps` | PID, PPID, state, wait channel and name from `/proc/<pid>/status` (first 511 bytes). |
| `top` | `top` | Raw mode with `VMIN=0`; shows `/proc/uptime`, 10 lines of `/proc/cpus` and 20 of `/proc/threads` every second; `q` or `^C` quits. |
| `ping` | `ping [-c N] [-i MS] [-s SIZE] [-W MS] HOST` | Raw ICMP, root only. |
| `nc` | `nc [-uv] HOST PORT`, `nc -l [-ukv] PORT` | TCP or UDP netcat. |
| `ifconfig` | `ifconfig`, `ifconfig IFACE [ADDR] [netmask M] [gw G] [up\|down]` | Configuring needs root. |
| `netstat` | `netstat [-t] [-u] [-s] [-i] [-a]` | Prints `/proc/net/*`. |
| `httpd` | `httpd [-p PORT] [-r ROOT]` | Default 80 and `/www`; serves `/proc/...` live. `make run` forwards Mac port 8080. |
| `rshd` | `rshd [-p PORT]` | Default 23; unauthenticated `esh -i` per connection. `make run` forwards Mac port 2323. |
| `echod` | `echod [-p PORT]` | TCP and UDP echo, default 7. `make run` forwards Mac UDP port 5555. |
| `utests` | `utests [MODE]` | 40 user-space tests ending in `UTEST RESULT: PASS\|FAIL`. Modes `persist-check`, `crash-commit-write\|check`, `crash-log-write\|check` and `stress[:secs]` are used by `tools/run_tests.py`. |
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
| `mkdir` | `mkdir DIR...` | Mode 0755. |
| `rm` | `rm FILE...` | `unlink` only; no `-r`/`-f`. |
| `rmdir` | `rmdir DIR...` | |
| `touch` | `touch FILE...` | Creates if missing (0644); does not update times. |
| `ln` | `ln OLD NEW` | Hard links only; exactly two arguments. |
| `mv` | `mv OLD NEW` | `rename`; exactly two arguments; a directory target is not handled. |
| `cp` | `cp FROM TO` | Exactly two arguments; creates `TO` with 0644. |
| `wc` | `wc [-lwc] [FILE...]` | Words are split on space, tab and newline. |
| `stat` | `stat PATH...` | Inode, type, size, blocks, links, mode. |
| `head` | `head [-n N] [FILE]` | Default 10 lines; reads a byte at a time. |
| `tail` | `tail [-n N] [FILE]` | Reads the whole input into memory. |
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
| `/bin/<prog>` for every `user/bin/*.c` and every directory `user/bin/<prog>/` | `build/user/stripped/<prog>` (DWARF stripped, symbols kept) |
| `/bin/sh` | hard link to `/bin/esh` |
| `/bin/<applet>` for every `EUTILS_LINKS` name | hard link to `/bin/eutils` (`=@/bin/eutils`) |
| `/etc/motd`, `/etc/passwd`, `/etc/hostname`, `/etc/eshrc` | `user/etc/` |
| `/dev/`, `/proc/` | empty mount points |
| `/tmp/` | mode 0777 |
| `/www/`, `/www/index.html` | `user/www/index.html` |
| `/home/`, `/home/guest/` | `/home/guest` mode 0755, owned by 1000:1000 |

The image is rebuilt whenever any program changes, which discards files created at run time.

`/etc/passwd` has the standard seven fields. `lookup_user` in `eutils` reads the first 1023 bytes (for `su`, `chown` and `id`); `esh` reads it for user names, home directories and `~user`:

```
root:x:0:0:root:/:/bin/esh
guest:x:1000:1000:guest:/home/guest:/bin/esh
```

### Adding a program or an applet

**A new program:**
1. Create `user/bin/foo.c` with `int main(int argc, char **argv)`. Include only headers from `user/include` (and `uapi/`).
2. Run `make`. The wildcard in `UPROGS` builds `build/user/bin/foo`, strips it, and `FS_FILES` puts it at `/bin/foo`.
3. Keep stacks small (8 MiB total), avoid floating point, and flush stdout before `fork` or `_exit`.

**A new eutils applet:**
1. Add a `if (!strcmp(prog, "foo")) { ... }` branch in `eutils.c` `main`, before the final `else` chain, or as a helper function like `head`/`tail`.
2. Add `foo` to `EUTILS_LINKS` in the Makefile.
3. Update the comment at the top of `eutils.c` and the tables here.

**A new libc function or system call wrapper:** add the code to `libc.c` (or `net.c` for networking) and the prototype to the right header in `user/include`. A new system call also needs an entry in `SYSCALLS` in `lib/include/uapi/syscall.h` and a kernel implementation. A new shell builtin is a function in `user/bin/esh/builtins.c` plus an entry (with its one-line help) in the `builtins[]` table.

**A program from several files:** put the sources in `user/bin/NAME/`; the `UMULTI` rule links every `.c` there into `/bin/NAME`.

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
- **procfs.** `/proc/<pid>/status` (credentials, pgid/sid, stopped, signal masks and dispositions), `maps`, `fds`, `stack` (kernel stack), `ustack` (symbolized user backtrace of a live process); `/proc/self`.
- **Monitor** (`Ctrl-]`): `procs`, `ubt <pid>` (user backtrace from the saved trap frame), `vm <pid> [va]`, `files <pid>`, `strace <pid>`, `kill <pid>`, `tty` (foreground group, termios, queued input). An `ebreak` in user code also stops in the monitor; `c` continues.
- **GDB.** `make debug` then `make gdb`; `eu-procs` lists processes. For source-level debugging of a program, `add-symbol-file build/user/bin/<prog>` (no offset is needed because every program is linked at `0x10000`). Since all programs share that address range, a breakpoint on a user address fires in whichever process is running there, so combine it with a condition or use `dbg_break()` in the program instead. `__restore_rt` has no CFI (`.eh_frame` is discarded anyway), so GDB may stop unwinding at a signal handler's caller.
- **Profiling.** `profile run CMD` gives a quick report with user functions symbolized; `profile dump` plus `make flamegraph` on the host symbolizes user frames from `build/user/bin`.
- **Heap.** `malloc_stats()` prints bytes in use and heap size; `/proc/<pid>/maps` shows the `heap` VMA. A `0x6b6b6b6b...` value in a register or a crash address almost always means use after free.
- **Shell.** See [12-shell.md](12-shell.md#debugging-and-observability): `set -x`, `whence -v`, `typeset -p`, `jobs -l`.

## Limitations and known issues

libc:

- **`malloc` reuses freed memory immediately** (first fit from the lowest address), which can hide use-after-free bugs, and the `0x6b` poison is written but never checked. It never shrinks the heap.
- **`setenv` does not check `malloc`'s result,** and `unsetenv` leaks the strings `setenv` allocated.
- **stdio.** Only stdout is buffered, and not flushed on `_exit`, crashes or `fork`. `printf`'s `write` result is ignored, so a short write to a pipe or socket loses data silently. `fprintf_fd` truncates at 511 bytes and does not flush stdout first unless it writes to fd 1, so stderr text can appear before earlier stdout text that has no newline.
- **`execvp`** builds up to 255-byte paths, but the kernel accepts only 128-byte paths in `execve`. It returns immediately on any error other than `ENOENT` (e.g. `EACCES` in an early `PATH` entry), and an empty `PATH` component becomes `/file` instead of the current directory.
- **Not reentrant**: `errno`, the stdout buffer, malloc and static result buffers (see Concurrency).
- **Small inaccuracies**: `dup2(fd, fd)` returns `fd` without checking that it is open; `gmtime_r` is wrong for negative times; `strtol`/`kstrtoul` do not detect overflow or set `errno`; `select` does not update the timeout; `fcntl`/`ioctl` always read a third argument even when the caller passed none.
- **Header inconsistencies**: `WUNTRACED`/`WNOHANG` are defined in both `sys/wait.h` and the uapi headers; `MSG_NOSIGNAL` exists only in `sys/socket.h`; the `SA_RESTART` comment in `uapi/signal.h` contradicts the kernel's behaviour.

Commands:

- **`tail -n 0`** prints the whole input: the loop `++lines == n` never matches 0. `tail` also ignores a failed `realloc`.
- **Usage errors in `ln`, `mv`, `cp`, `chmod` and `chown`** produce `eutils: unknown tool 'X'` because the argument-count check is part of the dispatch condition.
- **Commands read one byte per system call**: `head`, `grep`, and `esh` when reading commands from a pipe or the `read` builtin (so that input meant for later commands stays unread). `top` writes one byte per call. This is slow, and very visible under `strace`.
- **`ps` reads only 511 bytes** of each `status` file; **`lookup_user` reads only 1023 bytes** of `/etc/passwd`.
- **`httpd`**: files over 64 KiB are truncated; `HEAD` replies say `Content-Length: 0`. The directory-listing loop stops only when `len` reaches `sizeof(body) - 300`, but one entry can add almost 400 bytes, so a listing that long could make `sizeof(body) - len` wrap and overflow `body`. An empty path (`GET  HTTP/1.0`) reads `path[-1]`.
- **`echod`'s header comment** says it logs every connection and datagram; the code logs nothing after start-up. **`eutils.c`'s header comment** omits `crc32` and `profile`.
- **`su` has no password check** (it relies on `setuid` failing for non-root), and **`rshd` has no authentication**. Both are deliberate for a local VM. A remote `esh -i` has no terminal, so there is no `^C`, job control or line editing.

## Potential improvements

1. **Stronger allocator checks** — Neighbouring free blocks are merged now; next, delay reuse (a FIFO quarantine of recently freed blocks, like the kernel slab). Verify the `0x6b` poison on allocation (`memchr_inv`) to report writes after free. Add a tail canary for overruns. This turns malloc from a leak-prone list into a real use-after-free and overflow detector. Impact: high. Effort: medium.
2. **Flush stdout before `fork` and on fatal paths** — Provide `fork()` that calls `fflush_stdout` first, and flush in `malloc_die`. That avoids duplicated and lost output, which is otherwise a common source of confusing test logs. Impact: medium. Effort: small.
3. **Make `printf` handle short writes** — Loop in `fflush_stdout` until the buffer is fully written, or until a real error occurs. Output to sockets (`rshd`) and full pipes is otherwise silently truncated. Impact: medium. Effort: small.
4. **Buffered input for line readers** — Add a small `read`-ahead buffer helper (or a minimal `FILE`/`fgets`) and use it in `grep` and `head`. Today they make one system call per byte, which dominates their run time and their `strace` output. Impact: medium. Effort: medium.
5. **Fix `eutils` argument handling** — Separate "which tool" from "is the usage right" so `cp a` prints a usage message. Make `tail -n 0` print nothing and check `realloc`. Let `mv`/`cp` accept a directory target. Impact: low. Effort: small.
6. **Harden `httpd`** — Bound the directory listing by the remaining space before each `snprintf`, handle an empty path, stream files instead of truncating at 64 KiB, and send the real `Content-Length` for `HEAD`. It is the one program reachable from the host network by default. Impact: medium. Effort: small.
7. **Keep `execvp` and kernel limits consistent** — Return `ENAMETOOLONG` early for paths over the kernel's 128 bytes, keep searching `PATH` after `EACCES` (remembering the error), and treat empty `PATH` components as `.`. Impact: low. Effort: small.
8. **Make signal handlers safer to write** — Save and restore `errno` around handler invocation in a libc trampoline, and document which libc functions are safe in handlers. Optionally add `SA_RESTART` to `signal()` to match BSD/glibc behaviour. Impact: low. Effort: small.
9. **Unwind info for `__restore_rt` and correct header comments** — Add `.cfi_signal_frame`-style annotations (and keep a minimal `.eh_frame` for it) so GDB can unwind through signal handlers. Fix the stale comments in `uapi/signal.h` (`SA_RESTART`), `echod.c` and `eutils.c`. Impact: low. Effort: small.
