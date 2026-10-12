# Processes, program execution, signals, credentials and job control

This subsystem turns kernel threads into Unix processes. A `struct proc` wraps exactly one kernel thread and adds an address space, a program image, a file table, signal state, credentials and job-control identity (process group, session, controlling terminal). The code implements `fork` (copy-on-write), `execve` (static RISC-V ELF), `exit`/`wait4` with zombies and reparenting to init, Linux-numbered signals with handlers on user-stack frames, system-call restart, stop/continue, and real/effective/saved uid and gid with setuid executables.

## Responsibilities and scope

What this subsystem does:

- Keeps the global process list, pids, and parent/child links (`kernel/proc/proc.c`).
- Creates processes: the first ones from the kernel (`proc_spawn`) and all others by `fork` (`sys_fork`).
- Replaces a process image (`do_exec`): it loads the file, maps the ELF segments (`elf.c`) and builds the initial user stack with argv and envp.
- Handles termination: `proc_exit`, zombies, reparenting, `wait4`, and kernel-side waiting (`proc_wait_kernel`).
- Turns fatal user faults into signals and prints a diagnosis (`user_fault`, `proc_user_backtrace`).
- Sends signals, delivers them, runs handlers through signal frames and `rt_sigreturn`, applies default actions, implements stop and continue, and restarts system calls (`signal.c`, plus the return-to-user hook in `kernel/arch/riscv/trap.c`).
- Manages process groups and sessions (`setpgid`, `setsid`, `getpgid`, `getsid` in `signal.c`) and the job-control side of the console TTY (`kernel/drivers/tty.c`).
- Manages credentials (`setuid`, `setgid`, `umask`) and the permission-related file system calls `chmod`, `chown` and `access` (`cred.c`). `cred.c` also contains the time system calls (`clock_gettime`, `gettimeofday`, `nanosleep`). This document covers only the restart behaviour of `nanosleep`.

What it does not do (and where that lives):

- Address-space mechanics (VMAs, demand paging, the COW fault itself, `mm_fork`): `kernel/mm/uvm.c`. This document only describes how `fork` and `exec` use them.
- Threads, the run queue, wait queues and the context switch: `kernel/core/sched.c`, `kernel/core/sync.c`. A process has exactly one thread (`struct proc.thread`).
- The trap entry and system-call dispatch: `kernel/arch/riscv/trap.c`, `kernel/proc/syscall.c`. These are covered here only where they affect signals and restarts.
- File descriptors (`files_fork`, `files_exec`, `files_close_all` in `kernel/fs/file.c`) and the permission check itself (`vfs_permission` in `kernel/fs/vfs.c`).
- The TTY line discipline. Only the job-control parts of `tty.c` are covered here.

## Source map

| File | Role |
|---|---|
| `kernel/include/proc.h` | `struct proc`, `struct ksigaction`, `struct exec_image`, the user layout constants (`USER_TOP`, `USER_STACK_TOP`, ...), the wait-status encoders, `ERESTARTSYS`, and the prototypes for everything below. |
| `kernel/proc/proc.c` | The process list and `proc_lock`, pid allocation, `exec_image` loading and refcounting, `do_exec`/`sys_execve`, `proc_spawn`, `sys_fork`, `proc_exit`, `sys_wait4`, `proc_wait_kernel`, `proc_user_backtrace`, `user_fault`, and `proc_list_print` (the `procs` table). |
| `kernel/proc/elf.c` | `elf_load` (validates and eagerly loads a static ET_EXEC RISC-V ELF64 file) and `elf_symbolize` (looks up a symbol in the program's `.symtab` for user backtraces). |
| `kernel/proc/signal.c` | Sending (`signal_send*`), delivery (`signal_deliver`), the signal frame and `rt_sigreturn`, stop/continue (`do_stop`), the restart hook (`syscall_restart_check`), `kill`, `rt_sigaction`, `rt_sigprocmask`, `rt_sigsuspend`, and `setpgid`/`getpgid`/`getsid`/`setsid`. |
| `kernel/proc/cred.c` | `get*id`, `setuid`, `setgid`, `umask`, `fchmod(at)`, `fchownat`, `faccessat`, plus the time calls (`nanosleep` with its restart deadline). |
| `kernel/proc/syscall.c` | `syscall_dispatch` (records `-ERESTARTSYS` for the restart logic), `do_exit`, `do_getpid`/`do_getppid`, the `fork`/`execve`/`wait4` wrappers, and strace. |
| `lib/include/uapi/signal.h` | Signal numbers, names and default actions (the `SIGNALS` X-macro), `NSIG`, `SIGBIT`, `SA_*` flags, `struct sigaction`, `SIG_BLOCK`/`UNBLOCK`/`SETMASK`, `WUNTRACED`. It is shared with libc. |
| `lib/include/uapi/syscall.h` | System-call numbers and strace formats, `WNOHANG`, and `DBG_*` operations (including `DBG_STRACE`). |
| `kernel/drivers/tty.c` | Job control on the console: `tty_input` (`^C`, `^\`, `^Z`), `job_control` (SIGTTIN), and `tty_ioctl` (`TIOCGPGRP`, `TIOCSPGRP`, `TIOCSCTTY`, `TIOCNOTTY`). `tty_set_ctty` is also here. |
| `kernel/fs/procfs.c` | `/proc/<pid>/{status,maps,fds,stack,ustack,cmdline}` (`gen_pid`, `gen_pid_locked`), `/proc/procs`, `/proc/self`. |
| `kernel/arch/riscv/trap.c` | `user_exception` (faults lead to `user_fault`) and `return_to_user_checks` (calls `signal_deliver`, then `syscall_restart_check`). |
| `user/lib/libc.c`, `user/lib/syscall.S`, `user/lib/crt0.S` | The user side: `sigaction` inserts `__restore_rt` as `sa_restorer`. `__restore_rt` issues `rt_sigreturn` (139). `_start` reads argc/argv/envp from the stack. |
| `kernel/tests/test_proc.c` | KTESTs: VMA add/find/split, demand paging and COW refcounts, `copy_*_user` rejecting bad ranges, `elf_load` and `elf_symbolize` on `/bin/hello`, and rejection of a junk ELF. |
| `user/bin/utests.c` | User tests: `fork_wait_status`, `many_children`, `cow_isolation`, `exec_args`, `exec_missing`, `faults_kill_with_signals`, `bad_pointers_rejected`, `kill`, `signal_handler`, `signal_mask`, `signal_default_and_ignore`, `catch_segv`, `sigchld`, `stop_continue`, `eintr`, `process_groups`, `tty` (SIGTTIN), `permissions`, `exec_copied_program`, `cloexec_and_env`. |

## Concepts and data structures

### `struct proc` (`kernel/include/proc.h`)

The process is allocated with `kzalloc` in `proc_alloc`, so every field not named below starts at zero.

| Group | Fields | Notes |
|---|---|---|
| Identity | `pid`, `name[16]` | Pids come from `next_pid++` (starts at 1, never reused, no wrap handling). `name` is the basename of the program, set by `do_exec`. |
| Lifecycle | `state` (`P_ALIVE`, `P_ZOMBIE`), `exit_status`, `thread` | `thread` is NULL once the process is a zombie. `exit_status` uses the `wait4` encoding (see below). |
| Family | `parent`, `children`, `sibling`, `all_node` | `all_node` links into the global `all_procs`. `parent` is NULL for processes started by `proc_spawn`, and for orphans after init itself has exited. |
| Waiting | `child_wq`, `exit_wq`, `stop_wq` | The parent sleeps on `child_wq` in `wait4`. Kernel waiters (`proc_wait_kernel`) sleep on `exit_wq`. A stopped process sleeps on its own `stop_wq`. |
| Memory and image | `mm`, `exe`, `image`, `image_size` | `exe` is a refcounted `struct exec_image`; `image`/`image_size` mirror `exe->data`/`exe->size` for the symbolizer. |
| Files | `files[32]`, `fd_flags[32]`, `cwd` | Owned by `kernel/fs/file.c`. |
| Statistics and debug | `syscalls`, `started` (rdtime), `signals_received`, `strace` (0 off, 1 on, 2 follow forks), `strace_buf[192]` | |
| Signals | `sig_pending`, `sig_blocked`, `saved_blocked`, `restore_blocked`, `sigactions[32]` | Bit masks are `u32` (`NSIG` = 32, `SIGBIT(s)` = `1u << s`). |
| Stop/continue | `stopped`, `stop_sig`, `stop_reported`, `cont_seq` | See "Stop and continue". |
| Restart | `syscall_restart`, `restart_a0`, `nanosleep_restarting`, `nanosleep_deadline` | See "System-call restart". |
| Credentials | `uid`, `euid`, `suid`, `gid`, `egid`, `sgid`, `umask` | All `u16`. There are no supplementary groups. |
| Job control | `pgid`, `sid`, `ctty` | `ctty` points to the controlling `struct tty` (only the console exists). |
| Tracing | `tracer`, `ptrace_stopped`, `ptrace_resume_sig`, `ptrace_exit_report`, `si_signo`, `si_code`, `si_addr` | The debugger tracing this process, a tracing stop in progress, the signal the tracer passes on, whether a non-parent tracer must still be told of the exit, and why the last fault or trap signal was sent (`PTRACE_GETSIGINFO`). Changed under `proc_lock`. See [14](14-debugger.md). |

### `struct exec_image`

```c
struct exec_image { int refcnt; void *data; size_t size; char path[64]; };
```

This is the whole executable file, read into one `kmalloc` buffer (`image_load`, which calls `vfs_read_all`). It is shared by reference across `fork` (`sys_fork` increments `refcnt` atomically) and dropped by `image_put` on exec and exit. It is used only to symbolize user addresses (`elf_symbolize`). The segments themselves are copied into user pages at exec time, so a running program costs its file size in kernel memory plus its resident pages. `vfs_read_all` rejects files over 16 MiB. In practice `kmalloc` cannot allocate more than one 4 MiB buddy block (`MAX_ORDER` 10), so larger programs fail with `ENOMEM`.

### `struct ksigaction` and the signal table

```c
struct ksigaction { uintptr_t handler; u64 flags; uintptr_t restorer; u32 mask; };
```

`handler` is `SIG_DFL` (0), `SIG_IGN` (1) or a user address. `restorer` must be set: libc's `sigaction` always stores `__restore_rt` there. Without one, `setup_frame` fails and the process is killed.

`lib/include/uapi/signal.h` defines the signals with Linux numbers and a default-action letter:

| Action | Meaning | Signals |
|---|---|---|
| `T` | terminate | HUP, INT, KILL, USR1, USR2, PIPE, ALRM, TERM, and any number not in the table |
| `C` | terminate with `WCOREFLAG` (no core file is written) | QUIT, ILL, TRAP, ABRT, BUS, FPE, SEGV |
| `I` | ignore | CHLD, WINCH |
| `S` | stop | STOP, TSTP, TTIN, TTOU |
| `K` | continue | CONT |

`UNBLOCKABLE` is `SIGKILL | SIGSTOP`, and `STOP_SIGS` is the four stop signals (`signal.c`). Supported `sa_flags` are `SA_RESTART`, `SA_NODEFER` and `SA_RESETHAND`. `SA_NOCLDSTOP` is defined but never read. There is no `SA_SIGINFO` and no `SA_ONSTACK`.

`struct sigaction` (uapi) is this kernel's own layout, not Linux's: `{ sa_handler, sa_flags, sa_restorer, u32 sa_mask, u32 sa_pad }`.

### Wait status encoding (`proc.h`)

| Event | Encoding |
|---|---|
| `exit(code)` | `WEXITSTATUS_ENC(code)` = `(code & 0xff) << 8` |
| killed by signal | `WSIGNAL_ENC(sig)` = `sig & 0x7f`, plus `WCOREFLAG` (0x80) for `C` signals and for fatal faults |
| stopped | `WSTOPPED_ENC(sig)` = `(sig << 8) \| 0x7f` |

### The signal frame (`signal.c: struct sigframe`)

The frame is pushed on the user stack at `ALIGN_DOWN(sp - sizeof(frame), 16)`:

```
offset  size  field
     0     8  magic = 0x5349474652414d45 ("SIGFRAME")
     8   256  regs[32]   (x0..x31 from the trap frame)
   264     8  sepc       (where to resume)
   272     4  blocked    (mask to restore on rt_sigreturn)
   276     4  sig
   280     8  fp_valid   (1 if the FP registers below are meaningful)
   288   256  fp.f[32]   (f0..f31)
   544     8  fp.fcsr
   552        (total)
```

The handler is entered with `a0 = sig`, `ra = restorer`, `sp = frame`, and `sepc = handler`. If the interrupted thread had used floating point (`fp_used`), `setup_frame` saves the live FP registers (`fp_save_regs`, with interrupts off) and copies them into the frame with `fp_valid = 1`; otherwise `fp_valid = 0` and the FP area is zeroes. The handler runs with the same FP registers and `FS` as the interrupted code.

### User address layout (`proc.h`)

| Constant | Value | Use |
|---|---|---|
| `USER_MIN_ADDR` | `0x10000` | Lowest allowed ELF segment address (NULL guard below). |
| `USER_MMAP_BASE` | `0x2000000000` | Start of anonymous `mmap`. `brk` must stay below it. |
| `USER_STACK_TOP` | `0x3ffff00000` | Top of the stack VMA. |
| `USER_STACK_SIZE` | 8 MiB | The stack VMA is `[TOP - 8 MiB, TOP)`, demand-paged. |
| `USER_TOP` | `0x4000000000` | End of the Sv39 lower half. |

### Process lifecycle

```
 proc_alloc ──► P_ALIVE, linked into all_procs (+ parent's children)
     │            fork: mm_fork, thread created, creds/files/signals copied,
     │                  kthread_start
     │            spawn: thread runs user_entry → do_exec → user_return
     ▼
  running ◄──► stopped (do_stop: sleeps on stop_wq until SIGCONT/SIGKILL)
     │
     │ exit(), fatal signal, fatal fault, failed spawn exec
     ▼
 proc_exit: mm and files released, image dropped, children reparented,
            state = P_ZOMBIE, thread = NULL, SIGCHLD + wake parent
     │
     ▼
 reaped: sys_wait4 (parent) or proc_wait_kernel (kernel) →
         proc_unlink + kfree, under/after proc_lock
```

Invariants:

- Only the reaper frees a `struct proc`, and it unlinks the proc under `proc_lock` first. So any code holding `proc_lock` can use any proc it found in `all_procs` (`proc.c` header comment).
- `p->mm`, `p->exe`/`p->image` and `p->thread` change only under `proc_lock`. The old object is destroyed after the lock is dropped (`do_exec`, `set_image`, `proc_exit`). A reader holding the lock therefore sees either the old object, still alive, or the new one.
- A zombie has `thread == NULL` and `mm == NULL`, and its files are closed. Only `pid`, `pgid`, `exit_status` and the list links matter.
- After `proc_exit` drops `proc_lock`, nothing touches `p` again: `current->proc` was set to NULL inside the lock.

## How it works

### Process creation from the kernel: `proc_spawn`

`kinit` (`kernel/core/main.c`) calls `proc_spawn("/bin/init", argv)`, or the binary named by the `init=` boot option. With the `utest` option it runs `init -1 /bin/utests [arg]` and then `proc_wait_kernel`.

1. Copies the path and up to `MAX_ARGS` (32) arguments into a `struct spawn_args` (512 bytes of string space). If no arguments are given, argv[0] is the basename.
2. `proc_alloc(basename, NULL)`: no parent.
3. Creates a suspended kernel thread running `user_entry`. If this process is pid 1 it records `initproc`.
4. Opens `/dev/console` three times as fds 0–2 (it panics if `/dev` is not mounted), sets `cwd` to the root, `umask` to 022, `pgid = sid = pid` (a new session), and calls `tty_set_ctty`, which makes the console this process's controlling terminal and the foreground group.
5. Links thread and proc, detaches the thread, and starts it.
6. `user_entry` runs `do_exec` with the fixed environment `PATH=/bin HOME=/ TERM=vt100`. On failure it logs `cannot start <path>: <errno>` and calls `proc_exit(WEXITSTATUS_ENC(127))`. On success it calls `user_return` with the prepared trap frame.

### `fork` (`proc.c: sys_fork`)

1. `proc_alloc(parent->name, parent)`: the child gets a pid and is linked into `all_procs` and the parent's `children` right away, under `proc_lock`.
2. `mm_fork(parent->mm)` (`uvm.c`): copies the VMA list. For every present page it clears `PTE_W` and sets `PTE_COW` in the parent's PTE (if it was writable or already COW), copies the PTE to the child, and takes a page reference (`page_get`). The parent's TLB is flushed. A later write fault in either process goes through `uvm_fault`: if the page's refcount is 1 it is simply made writable again; otherwise the page is copied and the old one is released (`cow_copies++`).
3. Creates a suspended thread running `fork_child_entry`.
4. Shares the program image (`exe->refcnt++`). Copies `strace` only when it is 2 (follow). Copies all credentials and `umask`. Then `files_fork` (dups every descriptor, copies `fd_flags` and `cwd`) and `signal_fork` (copies `sigactions`, `sig_blocked`, `pgid`, `sid` and `ctty`; clears `sig_pending`).
5. Copies the parent's user trap frame to the child's kernel stack and sets the child's `a0 = 0`. `syscall_dispatch` has already advanced `sepc` past the `ecall`. If the parent has used floating point, it saves its live FP registers (interrupts off) and gives the child a copy with `fp_used` set; the child loads them in `thread_entry` when it first runs. The copied `sstatus` carries the parent's `FS`.
6. Starts the child and returns the child's pid to the parent.

On failure (no `mm` or no thread) the child is unlinked under `proc_lock` and freed, and the call returns `-ENOMEM`.

### `execve` (`sys_execve`, `do_exec`)

`sys_execve`:
- copies the path (up to 128 bytes);
- copies argv and envp with `copy_strv` into one 4 KiB (`MAX_ARG_LEN`) kmalloc buffer, with at most `MAX_ARGS` = 32 pointers each (more gives `-E2BIG`; a string that overflows the buffer gives `-E2BIG`);
- uses the basename of the path if argv is empty, then calls `do_exec`.

`do_exec` builds everything new first and commits only at the end, so a failure leaves the caller untouched:

1. **Load the image.** `image_load(path)`:
   - `namei`, then under `ilock` checks that the inode is a regular file and that `vfs_permission(ip, X_OK)` passes. Root still needs at least one x bit (`vfs.c: vfs_permission`).
   - Records the file's `mode`, `uid` and `gid` (for setuid).
   - Reads the whole file with `vfs_read_all(path)`, which looks the path up again.
   - `EISDIR` is reported as `EACCES`.
2. **Build the address space.**
   - `mm_create`, then `elf_load` (below). An ELF error is logged as `exec <path>: <reason>` and returns `-ENOEXEC`.
   - Adds the 8 MiB `stack` VMA.
   - Sets `brk_start = brk = PAGE_ALIGN(end of the highest segment)`.
3. **Build the initial stack** with `uvm_write` into the new, not yet active `mm`. The strings are copied downward from `USER_STACK_TOP`, argv first, then envp. The vector is then written at a 16-byte-aligned `sp`:

   ```
   high  USER_STACK_TOP
         argv[0] string, argv[1] string, ... (first copied = highest)
         envp[0] string, ...
         (padding from ALIGN_DOWN(.., 16))
         0, 0                  AT_NULL (no other auxv entries)
         NULL
         envp[envc-1] ... envp[0]
         NULL
         argv[argc-1] ... argv[0]
   sp -> argc
   low
   ```

4. **Commit.**
   - Swaps `p->mm` under `proc_lock`, then `vm_activate(mm)` and `mm_destroy(old)`.
   - Sets the process and thread name to the basename.
   - `set_image` swaps `exe` under `proc_lock` and drops the old reference.
   - `signal_exec` resets every handler to `SIG_DFL` except those set to `SIG_IGN`. `sig_blocked` and `sig_pending` are kept.
   - Applies setuid/setgid: if the file mode has `S_ISUID`, `euid = suid = file uid`; the same for `S_ISGID` with gids.
   - `files_exec` closes `FD_CLOEXEC` descriptors and clears all fd flags.
5. **Prepare the trap frame.**
   - Zeroes it, then sets `sepc = entry`, `sp`, `a0 = argc`, `a1 = sp + 8` (argv) and `a2 = envp`.
   - `sstatus` gets user mode (`SPP = 0`), `SPIE` set, `SUM` clear, and FS (bits 13–14) = 0, so the FPU is off, and `current->fp_used` is cleared. The new program's first floating-point instruction traps and gets fresh zeroed FP registers ([lazy floating point](03-traps-interrupts-syscalls.md#lazy-floating-point-for-user-programs)); nothing of the old image's FP state is visible.
   - Emits the `proc_exec` trace event, then returns `argc`. The dispatcher stores that in `a0`, which matches what was just put in the frame.

`crt0.S: _start` reads `argc` from `0(sp)` and computes `argv = sp + 8` and `envp = argv + argc + 1`.

`strace` stays set across exec (`DBG_STRACE`: "inherited by exec").

#### `elf_load` (`elf.c`)

- Checks:
  - the `\x7fELF` magic;
  - 64-bit little-endian;
  - `EM_RISCV` (243);
  - `ET_EXEC` (static only; there is no dynamic loader);
  - that the program headers lie inside the file.
- For each `PT_LOAD` with `memsz > 0`:
  - checks `filesz <= memsz` and that the file range is inside the image;
  - checks that `[vaddr, vaddr+memsz)` lies within `[USER_MIN_ADDR, USER_STACK_TOP - USER_STACK_SIZE)`;
  - refuses segments that are both writable and executable (W^X);
  - adds a `VMA_ELF` named `text`, `data` or `rodata`. `-EEXIST` means that two segments share a page, and the error message asks for page-aligned segments.
- Loads eagerly: `uvm_populate` maps a zeroed page for every page of the segment and copies the file bytes, so bss stays zero.
- Fails if no segment was loaded, or if the entry point is not in an executable VMA.
- Errors are returned as text in `err` and are logged by `do_exec`.

`elf_symbolize` scans every `SHT_SYMTAB` section for `STT_FUNC` symbols and untyped global symbols (assembly labels). It returns the closest symbol at or below the address that still contains it (if it has a size), together with the offset.

### Exit (`proc_exit`)

`proc_exit(status)` is called from `exit` (`do_exit`), from fatal default actions in `signal_deliver`, from `user_fault`, from `do_rt_sigreturn` via `user_fault`, and from `user_entry`.

1. It panics if called from a kernel thread. It also panics if the process is init and no kernel waiter has registered (`init (pid 1) exited with status ...`).
2. Detaches the address space under `proc_lock` (`p->mm = NULL`), switches to the kernel page table (`vm_activate(NULL)`), then `mm_destroy`. The order matters: if the thread is preempted during the destroy, the scheduler must not reactivate a page table that is being freed (`sched.c` calls `vm_activate(next->proc->mm)`).
3. `files_close_all` (descriptors and cwd) and `set_image(p, NULL)`.
4. Under `proc_lock`:
   - Reparents every child to `initproc`. If the exiting process *is* init, the children get `parent = NULL`. If any reparented child is already a zombie, init is woken.
   - Sets `exit_status` and `P_ZOMBIE`, and sets `thread = NULL`.
   - Sends `SIGCHLD` to the parent (`signal_send_locked`). With the default action it is discarded, because `CHLD` is `I`. It then wakes the parent's `child_wq`.
   - Wakes `exit_wq` for kernel waiters.
   - Sets `current->proc = NULL`.
5. Unlocks and calls `thread_exit(status)`. The thread was detached at creation, so the scheduler frees it.

### Waiting (`sys_wait4`)

The `pid` argument follows POSIX: -1 means any child, > 0 one child, 0 the caller's process group, < -1 group `-pid` (`child_matches`). The options are `WNOHANG` (1) and `WUNTRACED` (2). `rusage` is ignored.

Loop:
1. Under `proc_lock`, `find_reportable` scans the children. A child is *reportable* if it is a zombie, or if it is stopped and not yet reported and either `WUNTRACED` was given (a job-control stop) or it is in a tracing stop and the caller is its tracer (no `WUNTRACED` needed). A tracing stop is never reported to a parent that is not the tracer. It then scans `all_procs` for processes the caller traces without being their parent (`tracee_reportable`): their tracing stops, and their exit, which is reported to the tracer once (`ptrace_exit_report`) while the real parent still reaps the zombie.
2. Stopped child (or tracee): sets `stop_reported = true` and returns the pid with `WSTOPPED_ENC(stop_sig)`. The child stays in the list.
3. Zombie of a traced non-child: clears `ptrace_exit_report` and `tracer` and returns its pid and status without reaping it. Zombie child: `proc_unlink` under the lock, then (unlocked) reads `exit_status`, `kfree`s the proc, copies the status out and returns the pid. If the status copy faults, the child has already been reaped and the call returns `-EFAULT`.
4. No matching child: `-ECHILD`. With `WNOHANG`: 0.
5. Otherwise it sleeps with `wait_event_interruptible(&p->child_wq, has_zombie(...))`. `has_zombie` re-scans under `proc_lock`, so a wakeup between the scan and the sleep is not lost. If a signal interrupts the wait, it returns `-ERESTARTSYS`.

`proc_wait_kernel(p)` is the kernel-side version for processes that have no parent. It sets `init_has_kernel_waiter` when waiting for init, waits on `exit_wq` until `P_ZOMBIE`, unlinks and frees the proc, and clears `initproc`.

### Sending signals (`signal_send_locked`)

All senders end up here, with `proc_lock` held. The function panics with `signal_send_locked without proc_lock` unless the lock is held or the world is stopped by the monitor.

1. Validates `sig` (0 ≤ sig < 32; 0 only checks existence). A target that is NULL or not `P_ALIVE` gives `-ESRCH`, so signalling a zombie fails.
2. Emits the `signal_send` trace event and a `pr_debug` line.
3. **SIGCONT or SIGKILL:** increments `cont_seq`, clears pending stop signals, and if the process is stopped, clears `stopped` and wakes `stop_wq`.
4. **A stop signal:** clears a pending SIGCONT.
5. **Discard on send:** a signal whose handler is `SIG_IGN`, or `SIG_DFL` with default action `I` or `K`, is dropped unless it is SIGKILL or SIGSTOP. SIGCONT has already continued the process in step 3.
6. Otherwise sets the pending bit, increments `signals_received`, and calls `wake(p)`. That calls `wake_interruptible(thread)`, which wakes the thread only if it is in an interruptible sleep, and wakes `stop_wq`.

Wrappers:
- `signal_send` takes the lock.
- `signal_send_pgrp` sends to every live process with a matching `pgid`. It returns `-ESRCH` if none accepted the signal.
- `signal_notify_parent` sends SIGCHLD and wakes `child_wq`. It is used by `do_stop`.

`kill(pid, sig)` (`do_kill`):
- `pid > 0`: one process.
- `pid == 0`: the caller's own group.
- `pid < -1`: group `-pid`.
- `pid == -1`: returns `-EINVAL` (not supported).

There is **no permission check**: any process may signal any other.

### Delivery (`signal_deliver`) and the return-to-user path

Every trap that returns to user mode ends in `trap.c: return_to_user_checks`:

```c
if (this_cpu()->need_resched) yield();
if (signal_pending(current_proc())) { intr_on(); signal_deliver(tf); }
syscall_restart_check(tf);
intr_off();
```

`signal_pending(p)` is `sig_pending & (~sig_blocked | UNBLOCKABLE)`.

`signal_deliver` loops while a signal is deliverable:

1. Under `proc_lock`: picks the **lowest-numbered** deliverable signal, clears its pending bit, and snapshots `cont_seq`.
   - **Traced:** if the process has a tracer and the signal is not SIGKILL, it first enters a *tracing stop* (`ptrace_stop`, [14](14-debugger.md)), before the action is even looked at (so ignored signals stop it too). The tracer's `PTRACE_CONT` or `PTRACE_DETACH` chooses what happens next: signal 0 drops the signal and the loop continues; any other number is delivered instead, through the normal steps below (if it is blocked, it goes back to pending).
2. If the action is `SIG_IGN`, it continues with the next signal. SIGKILL and SIGSTOP always use the default action.
3. Default actions:
   - `I` and `K`: nothing.
   - `S`: `do_stop` (below), then continue the loop.
   - `C`: `proc_exit(WSIGNAL_ENC(sig) | WCOREFLAG)`.
   - Anything else: `proc_exit(WSIGNAL_ENC(sig))`.
4. Handler:
   - If the system call that just ran returned `-ERESTARTSYS` (`p->syscall_restart`), the flag is cleared. With `SA_RESTART`, `a0` is restored to `restart_a0` and `sepc -= 4`, so the call runs again after the handler. Without it, `a0 = -EINTR`. This adjustment is made **before** the frame is built, so the frame holds the post-handler state.
   - The mask to restore is `saved_blocked` if a `sigsuspend` is in progress (`restore_blocked`), otherwise the current `sig_blocked`.
   - `setup_frame` writes the frame. If there is no restorer, or the user stack cannot be written (for example after a stack overflow), the log says `pid N (name): cannot deliver SIGx: ...; killing it` and the process exits with `SIGSEGV | WCOREFLAG`.
   - Blocks `act->mask` and, unless `SA_NODEFER`, the signal itself. SIGKILL and SIGSTOP are never blocked.
   - `SA_RESETHAND` resets the action to `SIG_DFL`.
   - **Returns after one handler.** Any other pending signals are delivered on the next return to user mode, normally the `rt_sigreturn` call.
5. If the loop ends without running a handler and `restore_blocked` is set, the `sigsuspend` mask is restored here.

`rt_sigreturn` (`do_rt_sigreturn`):
- Reads the frame at the current `sp`. If the copy fails or the magic is wrong, it logs `rt_sigreturn with a bad signal frame at ... (magic ...)` and calls `user_fault(tf, "bad signal frame", SIGSEGV)`.
- Otherwise it restores x1..x31 and `sepc`, and sets `sig_blocked = frame.blocked` (minus the unblockable signals).
- If the frame has `fp_valid` set and the thread uses FP, it copies the frame's FP registers into `current->fp` and loads them (interrupts off), undoing whatever the handler did to them. A handler can therefore change the interrupted code's FP registers by editing the frame, as with the integer registers.
- It returns `frame.regs[10]`, because `syscall_dispatch` writes the return value into `a0`.

### System-call restart (`ERESTARTSYS`)

`ERESTARTSYS` (512) is kernel-internal. Blocking calls return it when a signal interrupts them:
- `wait4`, `nanosleep`, and the TTY read (including the `job_control` SIGTTIN path);
- pipe read/write (`fs/pipe.c`);
- socket waits (`net/socket.c`).

`syscall_dispatch` then sets `p->syscall_restart = true` and `p->restart_a0 = args[0]`. The original `a0` is needed because `a0` is overwritten with the return value; `a1`–`a5` are never modified.

What happens next depends on the signal:

| Outcome of `signal_deliver` | Effect |
|---|---|
| A handler with `SA_RESTART` | `sepc -= 4`, `a0 = restart_a0`: the call runs again after the handler returns. |
| A handler without `SA_RESTART` | `a0 = -EINTR`. |
| No handler ran (stop/continue, or an ignored signal) | `syscall_restart_check` restores `a0`, rewinds `sepc` by 4 (the `ecall`), and re-executes the call transparently. |
| The process dies | Not relevant. |

`nanosleep` keeps its deadline across a transparent restart: before returning `-ERESTARTSYS` it saves `nanosleep_deadline`. `syscall_restart_check` sets `nanosleep_restarting` when `a7 == SYS_nanosleep`, and the re-run sleeps only until the old deadline (`cred.c: do_nanosleep`).

`sleep_ms` (500) and `rt_sigsuspend` return `-EINTR` directly and are never restarted.

### Stop and continue (`do_stop`, `cont_seq`)

A stop signal with the default action reaches `do_stop(p, sig, cont_seq)` **after** it has been dequeued with the lock dropped. A SIGCONT or SIGKILL may arrive in that window. Such a signal increments `cont_seq`, so `do_stop` re-checks under `proc_lock`: if `p->cont_seq` differs from the value seen at dequeue time, the stop is cancelled. This is the "SIGCONT must win over an earlier stop" rule.

Otherwise `do_stop`:
- sets `stopped`, `stop_sig` and `stop_reported = false`;
- notifies the parent (SIGCHLD plus a `child_wq` wakeup, which lets `wait4(WUNTRACED)` report it);
- emits `proc_stop`;
- sleeps uninterruptibly on `stop_wq` until `!p->stopped` or SIGKILL is pending.

SIGCONT and SIGKILL clear `stopped` in `signal_send_locked`, except that SIGCONT cannot end a *tracing* stop (`ptrace_stopped`): only the tracer can, or SIGKILL, which wakes the tracee so it dies. Other signals sent to a stopped process stay pending until it continues; `wake` wakes `stop_wq`, but the wait condition is not met.

There is no `WCONTINUED` and no SIGCHLD on continue.

### Tracing (`ptrace`)

A process can be traced by a debugger (`PTRACE_TRACEME` by the child of the debugger before it execs, or `PTRACE_ATTACH`). The details are in [14-debugger.md](14-debugger.md); the effects on this subsystem are:

- **Signals**: every signal except SIGKILL turns into a tracing stop first (above). The stop sets `stopped`, `stop_sig` and `ptrace_stopped`, sends SIGCHLD to the tracer, and sleeps on `stop_wq` with `wait_what = "traced"`.
- **`exec`**: after a successful exec, a traced process is sent SIGTRAP with `si_code = TRAP_EXEC` and `si_addr` = the entry point, so the debugger gets control before the first instruction.
- **Faults**: `user_fault` never kills a traced process directly; the signal goes through a tracing stop.
- **`wait4`**: the tracer sees tracing stops and the tracee's exit (above).
- **`exit`**: a tracer that exits releases its tracees (`ptrace_tracer_exit`): they lose their tracer, and one in a tracing stop is resumed with no signal. A tracee whose tracer is not its parent also notifies the tracer of its exit.
- **State**: `/proc/<pid>/status` and `ps` show `t` for a tracing stop (`T` for a job-control stop).

### Faults (`user_fault`)

`trap.c: user_exception` sends the following to `user_fault(tf, what, sig)`:

| Trap | Signal |
|---|---|
| Page fault that `uvm_fault` could not resolve | SIGSEGV; SIGBUS for out-of-memory |
| Illegal instruction | SIGILL |
| Misaligned access | SIGBUS |
| Breakpoint (`ebreak`) | SIGTRAP (`sepc` left on the `ebreak`) |
| Anything else | SIGSEGV |

Then:

- It records `si_signo`, `si_addr` (the pc for a breakpoint or illegal instruction, else `stval`) and `si_code` (`SEGV_ACCERR` if a VMA covers the address, otherwise 1: `SEGV_MAPERR`, `TRAP_BRKPT`, `ILL_ILLOPC` or `BUS_ADRALN`), for `PTRACE_GETSIGINFO`.
- If the process is traced, or has a handler for the signal and the signal is not blocked (`signal_handled`), `user_fault` posts the signal and returns. A traced process then stops for its debugger in `signal_deliver` instead of dying. The handler runs on the way out, and if it returns, the faulting instruction is executed again.
- Otherwise it prints a report and calls `proc_exit(WSIGNAL_ENC(sig) | WCOREFLAG)`. The report contains:
  - the signal, the cause and address, and a diagnosis: `NULL pointer dereference` (addr < 4096), `kernel address: ...`, `stack overflow (just below the N KiB stack)` (within 1 MiB below the stack VMA), `VMA '<name>' is r-x`, or `address not in any mapping`;
  - pc/ra/sp with the symbol;
  - a user backtrace;
  - the memory map (`mm_print`).

`proc_user_backtrace(print, p, pc, fp)` walks the frame-pointer chain. Each record is `{prev fp, ra}` at `fp - 16`, read with `uvm_read`, which never faults pages in. The walk stops after 64 frames, or when the frame pointer is zero, misaligned or does not increase. Repeated identical pcs (recursion) are collapsed. Symbols come from `elf_symbolize` on `p->image`.

### Process groups, sessions and the controlling terminal

- **`setpgid(pid, pgid)`** (`setpgid_locked`). `pid` 0 means the caller, and `pgid` 0 means the target's own pid. The target must be the caller or one of its children (`-ESRCH`), and in the caller's session but not a session leader (`-EPERM`). When joining an existing group, a process with that `pgid` in the same session must exist (`-EPERM`; zombies count). The `EACCES` "child has already exec'd" rule is not implemented.
- **`getpgid`, `getsid`**: read under `proc_lock`.
- **`setsid`**: fails with `-EPERM` if the caller is already a group leader (`pgid == pid`). Otherwise `sid = pgid = pid` and `ctty = NULL`.
- **Inheritance**: `fork` copies `pgid`, `sid` and `ctty` (`signal_fork`); `exec` keeps them.

TTY side (`kernel/drivers/tty.c`, a single `console_tty` that has `sid` and `pgrp` fields):

| Mechanism | Behaviour |
|---|---|
| `tty_set_ctty(p)` | Called by `proc_spawn`. Sets `p->ctty = &console_tty`, `tty.sid = p->sid`, `tty.pgrp = p->pgid`. |
| `TIOCSCTTY` | Allowed only for a session leader. It takes over the console unconditionally (sets ctty, `tty.sid`, `tty.pgrp`). |
| `TIOCNOTTY` | Clears `p->ctty`. |
| `TIOCGPGRP` / `tcgetpgrp` | Returns `tty.pgrp`. |
| `TIOCSPGRP` / `tcsetpgrp` | Requires `p->ctty == tty` and `p->sid == tty.sid` (else `-ENOTTY`). It does **not** check that the group exists, and sends no SIGTTOU to background callers. |
| `^C`, `^\`, `^Z` (with `ISIG`) | `tty_input`, called from the UART interrupt, flushes input (unless `NOFLSH`), echoes, and after dropping `tty->lock` calls `signal_send_pgrp(tty.pgrp, SIGINT/SIGQUIT/SIGTSTP, 0)`. Emits the `tty_signal` trace event. |
| Background read | `job_control` (from `tty_read`): if the caller's ctty is this tty and its `pgid` differs from `tty.pgrp`, then it returns `-EIO` when SIGTTIN is ignored or blocked; otherwise it sends SIGTTIN to the caller's whole group and returns `-ERESTARTSYS`. After a `fg`, the read restarts transparently. |
| Background write | Not checked: there is no `TOSTOP` and no SIGTTOU. |

The shell (`esh`, [12-shell.md](12-shell.md#job-control)) is the main user:
- it ignores SIGTSTP, SIGTTIN and SIGTTOU, and catches SIGINT and SIGCHLD (without `SA_RESTART`);
- it calls `setpgid` on both sides of `fork`;
- it moves jobs to the foreground with `tcsetpgrp` and continues them with `kill(-pgid, SIGCONT)`;
- it collects stops with `waitpid(-1, ..., WUNTRACED)`, and relies on `SIGCHLD` to report background jobs while the user is typing.

### Credentials and permissions (`cred.c`)

| Call | Rule |
|---|---|
| `setuid(u)` | If `euid == 0`: `uid = euid = suid = u`. Else if `u` is `uid` or `suid`: `euid = u`. Else `-EPERM`. |
| `setgid(g)` | Same rule, but the privilege test is still `euid == 0`. |
| `umask(m)` | Stores `m & 0777` and returns the old value. Spawned processes start with 022. |
| `chmod`, `fchmod` | Root or the owner only (`-EPERM`). The filesystem must have `write_inode`. Sets `mode & 07777` inside a filesystem transaction. |
| `chown` (`fchownat`) | Root only. -1 leaves a field unchanged. Setuid/setgid bits are not cleared. |
| `access` (`faccessat`) | `vfs_permission` with the requested bits. This uses the **effective** ids (see `vfs.c: creds`), not the real ones as POSIX `access` requires. |
| exec | `S_ISUID`/`S_ISGID` set `euid`/`suid` (`egid`/`sgid`) to the file's owner. |

`vfs_permission` (`vfs.c`):
- Root passes every check, except that executing a regular file needs at least one x bit.
- Other users get the owner bits if `euid` matches the file's uid, else the group bits if `egid` matches, else the "other" bits.
- Only `AT_FDCWD` is accepted as the directory fd of the `*at` calls.

Kernel threads (no proc) act as root.

## Concurrency and locking

### `proc_lock`

`proc_lock` is a global spinlock (`proc.c`), always taken with `spin_lock_irqsave`, because `tty_input` runs in the UART interrupt and reaches `signal_send_pgrp`. It protects:

- `all_procs`, `parent`, `children` and `sibling`;
- `state`, `stopped`, `stop_sig`, `stop_reported` and `cont_seq`;
- `sig_pending` (all writers hold it: senders, the dequeue in `signal_deliver`, and `rt_sigaction` when it clears a pending bit for `SIG_IGN`; the last one is unlocked, see below);
- `pgid`, `sid` (`setpgid`, `setsid`);
- the pointers `mm`, `exe`/`image`/`image_size`, `thread` and `cwd` (`files_close_all`).

**Keep-alive rule.** A proc is freed only by `sys_wait4` or `proc_wait_kernel`, after `proc_unlink` under `proc_lock`. Anything that finds a proc via `proc_find` or the list while holding the lock may use it, and its `mm`/image, until it unlocks. This is what `do_kill`, `do_getpgid`, the procfs generators (`gen_pid` runs `gen_pid_locked` entirely under the lock), `do_getppid` (the parent may be exiting) and the monitor rely on. Objects that are swapped out (an old `mm`, an old image) are destroyed only after the swap has been published under the lock.

**Lock order:**
- `proc_lock` → wait-queue locks and `sched_lock` (`wake_up`, `wake_interruptible` are called with it held).
- procfs also takes `mm->vma_lock` (in `mm_print`) and `file_lock` (in `files_print_fds`) inside `proc_lock`.
- `tty_input` drops `tty->lock` *before* signalling, so `tty->lock` and `proc_lock` are never nested.

**Never sleep under `proc_lock`.** This is why procfs resolves the cwd path before it takes the lock (`gen_pid`, case 3), and why `mm_destroy`, `image_put` and `files_close_all` run outside it.

### Owner-only fields (no lock)

Some fields are written only by the owning thread and read elsewhere without a lock:
- `sig_blocked`, `saved_blocked`, `restore_blocked`, `sigactions[]`;
- `syscall_restart`, `restart_a0`, `nanosleep_*`;
- credentials, `umask`, `name`, `ctty`;
- the descriptor table.

The cross-CPU readers are `signal_pending`/`thread_signal_pending` (reads `sig_blocked`), `signal_send_locked` (reads `sigactions[sig].handler`), `job_control` and procfs. They tolerate a word-sized stale read: at worst, a signal is classified as ignored or deliverable based on the previous action. The comment in `proc.c` that says `proc_lock` protects "signal state" means `sig_pending` and the stop state, not these fields.

### Lost-wakeup avoidance

- **`wait4`**: `wait_event_interruptible` queues the waiter before it re-evaluates `has_zombie`. `proc_exit` and `do_stop` change state and wake `child_wq` inside `proc_lock`.
- **Signals vs. sleeps**: `wake_interruptible` wakes a thread only if `t->interruptible` is set, and it checks this under `sched_lock`. `wait_event_interruptible` and `sleep_ms` set `interruptible` and then re-check `thread_signal_pending`, so a signal that arrived just before the sleep is seen.
- **Stop**: `do_stop` waits on `stop_wq` with the condition `!p->stopped || SIGKILL pending`, and senders wake `stop_wq`.

### SMP notes

- Delivery happens only on the target's own CPU, on its way back to user mode. A sender does not IPI a target that is running in user mode on another CPU. The target notices the signal at its next trap, at the latest at the next timer tick (100 Hz).
- `fork` flushes only the local TLB after it write-protects the parent's PTEs. This is enough because the scheduler switches `satp` (with `sfence.vma`) whenever a CPU switches to a thread with a different `mm`, or to a kernel thread (`sched.c` → `vm_activate`). A single-threaded process's `mm` is therefore never live in another CPU's TLB.
- `exe->refcnt` uses atomics, because fork and exit on different CPUs may touch a shared image.
- `user_fault` and the resulting `proc_exit` run with interrupts **disabled**: `user_exception` calls `intr_off()` after `uvm_fault`, and the hardware disabled them on trap entry for the other causes. `signal_deliver`, by contrast, runs with `intr_on()`.

## Interfaces

### Kernel functions used by other subsystems

| Function | Used by |
|---|---|
| `proc_spawn`, `proc_wait_kernel` | `kernel/core/main.c` (init, utests) |
| `proc_exit` | syscalls, signals, faults |
| `proc_find`, `proc_nth`, `proc_nth_pid`, `proc_exists` | procfs, monitor (`proc_find` needs `proc_lock` or a stopped world) |
| `proc_list_print` | `procs` monitor command, `/proc/procs`, `DBG_PS` |
| `proc_user_backtrace` | `user_fault`, `ubt` monitor command, `/proc/<pid>/ustack` |
| `user_fault` | `trap.c`, `do_rt_sigreturn` |
| `signal_send`, `signal_send_locked`, `signal_send_pgrp` | tty, pipes and sockets (SIGPIPE), monitor `kill`, `proc_exit`, `user_fault` |
| `signal_pending`, `thread_signal_pending` | `sleep_ms`, `wait_event_interruptible`, `poll.c`, `nanosleep` |
| `signal_deliver`, `syscall_restart_check` | `trap.c: return_to_user_checks` |
| `signal_handled`, `signal_name` | faults, strace (`k` format), procfs |
| `tty_set_ctty` | `proc_spawn` |
| `elf_load`, `elf_symbolize` | exec, backtraces, KTESTs |

### System calls

Numbers are from `lib/include/uapi/syscall.h`.

| # | Name | Handler |
|---:|---|---|
| 93 | exit | `syscall.c: do_exit` |
| 101 | nanosleep | `cred.c: do_nanosleep` (restartable) |
| 117 | ptrace | `ptrace.c: do_ptrace` ([14](14-debugger.md)) |
| 129 | kill | `signal.c: do_kill` |
| 133 | rt_sigsuspend | `do_rt_sigsuspend` (always `-EINTR`) |
| 134 | rt_sigaction | `do_rt_sigaction` (`-EINVAL` for KILL/STOP) |
| 135 | rt_sigprocmask | `do_rt_sigprocmask` (`u32` mask) |
| 139 | rt_sigreturn | `do_rt_sigreturn` |
| 144 / 146 | setgid / setuid | `cred.c` |
| 154–157 | setpgid, getpgid, getsid, setsid | `signal.c` |
| 166 | umask | `cred.c` |
| 172 / 173 | getpid / getppid | `syscall.c` |
| 174–177 | getuid, geteuid, getgid, getegid | `cred.c` |
| 48, 52, 53, 54 | faccessat, fchmod, fchmodat, fchownat | `cred.c` |
| 220 | fork | `sys_fork` (Linux's 220 is `clone`; here it is a plain fork) |
| 221 | execve | `sys_execve` |
| 260 | wait4 | `sys_wait4` (`WNOHANG`, `WUNTRACED`) |
| 500 | sleep_ms | `syscall.c` (returns `-EINTR`, not restarted) |
| 502 | debug | `DBG_STRACE`, `DBG_PS`, `DBG_BREAK`, `DBG_VM`, ... |
| 29 | ioctl (tty) | `TIOCGPGRP`, `TIOCSPGRP`, `TIOCSCTTY`, `TIOCNOTTY` |

### /proc files

| File | Contents |
|---|---|
| `/proc/procs` | `proc_list_print`: PID, PPID, PGID, SID, name, state (`zombie`, `T` for stopped, or the thread state), TID, syscalls, RSS, strace, SIGPEND, SIGBLK, age. |
| `/proc/<pid>/status` | pid, ppid, name, state, tid, wait reason, context switches, syscalls, rss_kib, strace, uid/euid/gid/egid/umask, pgid, sid, stopped, sig_pending, sig_blocked, sig_caught, sig_ignored; then, for `ps`, `ustate` (the Unix state letter: R, S, D for an uninterruptible sleep, T, Z), `start_ms` (start time, ms since boot), `cpu_ms` (CPU time including the current slice, `thread_cpu_ticks`), `vsz_kib` (sum of the VMAs), `tty` (`console` or `-`), `tpgid` (the terminal's foreground group), `psr` (the CPU it last ran on). |
| `/proc/<pid>/cmdline` | The arguments of the last `execve`, each followed by a NUL (as on Linux), kept in `proc.cmdline` (256 bytes, truncated) and copied by `fork`. |
| `/proc/<pid>/maps` | `mm_print` (VMAs, resident and COW pages). |
| `/proc/<pid>/fds` | The cwd path, then the descriptors. |
| `/proc/<pid>/stack` | Kernel backtrace of the process's thread. |
| `/proc/<pid>/ustack` | `proc_user_backtrace` from the saved user trap frame. |
| `/proc/self` | Resolves to the caller's pid directory. |

Each file is generated in full at `open` time into a 64 KiB buffer (`SEQ_MAX`).

### Boot options

- `init=<path>`: the first program (default `/bin/init`).
- `utest[=arg]`: runs `init -1 /bin/utests [arg]` under `proc_wait_kernel` and exits QEMU with the result.
- `log=proc:debug`: enables the `pr_debug` lines of this subsystem (`SUBSYS_PROC`).

### Monitor commands (`kernel/debug/monitor.c`)

`procs`, `vm <pid> [va]`, `ubt <pid>`, `strace <pid> [on|off|follow]`, `kill <pid>`, `files <pid>`, `tty`.

The monitor's `kill` sends SIGKILL with `spin_lock_debug`, so it works even if the lock holder is parked. It "takes effect when it runs".

## Debugging and observability

- **Process table**: `procs` (monitor), `ps` (the user command, which reads `/proc/<pid>/status`), `/proc/procs`, or GDB `eu-procs` (includes zombies). A stopped process shows `T`. The SIGPEND and SIGBLK columns show the masks in hex: bit *n* is signal *n*.
- **One process**: `/proc/<pid>/status` lists caught and ignored signals by name and shows `waiting <reason>` (e.g. `wait4`, `stopped`, `sigsuspend`, `tty read`, `sleep`).
- **Where is it?** `ubt <pid>` and `/proc/<pid>/ustack` show user code. `/proc/<pid>/stack` and `ps` in the monitor show the kernel side.
- **System calls**: `strace <cmd>` in the shell, `strace <pid> [on|off|follow]` in the monitor, or `dbg_strace()`. Signals are printed by name (format `k`), and `execve` arguments are decoded (format `a`). A restarted call shows up as two entries: the first returns `-1 E? (...)` with errno 512, which `errno_name` does not know, and the second is the re-run.
- **Trace events** (the `PROC` group in `kernel/include/trace_events.h`): `proc_exec(pid, name16)`, `proc_exit(pid, status)`, `user_fault(addr, access, result)`, `signal_send(pid, sig, from_pid)`, `signal_deliver(sig, handler)`, `proc_stop(pid, sig)`, `tty_signal(sig, pgrp)`.
- **Logs** (`log=proc:debug`): `signal SIGx -> pid N (name) from pid M`, `pid N (name) exits with status 0x..`, `pid N exec <path>: entry ..., K args`, `pid N (name) stopped by SIGx`, `pid N (name): <what> at ... -> SIGx handler`, `exec <path>: <errno>`.

**Messages and what they mean:**

| Message | Meaning |
|---|---|
| `pid N (name) killed by SIGSEGV: <cause>: <what> at 0x.. -- <diagnosis>` and the lines after it | A fatal user fault. Read the diagnosis, the symbolized backtrace and the memory map. |
| `exec <path>: <reason>` (warning) | `elf_load` rejected the file: not ELF, wrong machine, not static, W^X segment, segment outside user space, shared page, bad entry point. |
| `cannot start <path>: <errno>` | `proc_spawn`'s exec failed. The process exits with status 127. |
| `init (pid 1) exited with status 0x..` (panic) | Init died with no kernel waiter. |
| `pid N: cannot deliver SIGx: user stack not writable (stack overflow?)` / `no restorer` | Handler setup failed. The process is killed with SIGSEGV. |
| `rt_sigreturn with a bad signal frame at ... (magic ...)` | The handler moved `sp` or overwrote its frame. |
| `signal_send_locked without proc_lock` (panic) | A caller forgot the lock. |
| `vm_activate: page table ... is not a live page table` (panic) | Usually an `mm` was destroyed while still reachable. Check the ordering rules in `proc_exit`/`do_exec`. |
| `user-mode trap but the current thread has no process` (panic) | A thread returned to user mode after `proc_exit` cleared `current->proc`. |

**Tests:**
- `make test` runs the kernel tests (`ktest`, including `test_proc.c`) and the user tests (`utest`, `utests.c`) through `tools/run_tests.py`. `make stress` runs `utest=stress:<secs>` (parallel fork/exec/pipe/COW load) with `lockdep=panic`.
- `/bin/segv {null,text,stack,illegal,kernel}` deliberately produces each kind of fault report.
- `/bin/exitcode` checks argv and env passing.

## Limitations and known issues

- **No permission check on `kill`** (`do_kill`): an unprivileged process (for example after `setuid(1000)`) can SIGKILL root's processes, including init.
- **Exec TOCTOU** (`image_load`): permission, mode and owner are taken from one path lookup, and the bytes come from a second lookup (`vfs_read_all(path)`). If the path is renamed in between, the process can run different contents with the first file's setuid identity.
- **ELF bounds checks can overflow**: `ph->offset + ph->filesz`, `ph->vaddr + ph->memsz` and `eh->phoff + phnum * 56` are unchecked 64-bit sums, and `phentsize` is not validated. `elf_symbolize` does not bounds-check the symbol table, the string table offsets or the name termination against `size`. A crafted ELF can make the kernel read past the image buffer, at exec or when a fault report is printed.
- **Lost signal during fork**: the child is visible in `all_procs` (with `pgid == 0` and no thread) before `signal_fork` runs, and `signal_fork` sets `sig_pending = 0`. A `kill(childpid, ...)` that lands in this window is silently lost.
- **`proc_spawn` error path**: if `kthread_create_suspended` fails, the proc is `kfree`d while still linked in `all_procs` (`proc_alloc` already added it). That leaves a dangling list entry.
- **Init race at boot**: in `utest` mode, `init_has_kernel_waiter` is set only when `proc_wait_kernel` starts. If init exits before that (for example `/bin/init` fails to exec), `proc_exit` panics.
- **Orphans of init**: when init itself exits (utest mode), its children get `parent = NULL` and are never reaped.
- **No orphaned-process-group rules**: there is no SIGHUP/SIGCONT to orphaned stopped groups, and SIGTTIN is still sent to an orphaned background group. Such a group can stop forever.
- **No SIGCHLD semantics beyond notification**: `SA_NOCLDSTOP` is ignored, `SIG_IGN`/`SA_NOCLDWAIT` do not auto-reap, and there is no `WCONTINUED` and no SIGCHLD on continue.
- **Signals**: 31 numbers in a `u32` mask, no real-time or queued signals (a second instance of a pending signal is merged), no `siginfo`, no `sigaltstack` (so a SIGSEGV handler cannot run after a stack overflow), `kill(-1)` is unsupported, and signalling a zombie returns `ESRCH` (Linux returns 0).
- **FP state across handlers is not fully symmetric**: if a handler is the first code in the thread to use floating point, the frame has `fp_valid = 0`, so `rt_sigreturn` leaves the handler's FP register values (and `FS` on) for the interrupted code. That code had never used FP, so it is harmless, but it is not a strict restore. Every frame is 552 bytes, FP area included, even for programs that never use FP.
- **TTY job control is partial**: only one tty; no SIGTTOU or `TOSTOP` for writes or `tcsetpgrp` from the background; `TIOCSPGRP` accepts any number; `TIOCSCTTY` steals the console from another session; a session leader's exit does not detach the terminal or send SIGHUP; `tty.pgrp` is read and written without a lock.
- **Credentials**: `u16` ids, no supplementary groups, no `setreuid`/`setresuid`/`setfsuid`; `access()` checks effective rather than real ids; `chown` does not clear setuid/setgid bits; there is no `nosuid` handling.
- **Restart corner cases**:
  - `rt_sigreturn` returns the restored `a0` through the normal dispatch path. If an asynchronously interrupted context happened to have `a0 == -512`, `syscall_dispatch` would treat it as `-ERESTARTSYS` and rewind `sepc`.
  - A `nanosleep` restarted by an `SA_RESTART` handler sleeps the full time again, because only `syscall_restart_check` sets `nanosleep_restarting`.
  - `sigsuspend` returns `EINTR` even when the only signal was a stop/continue.
- **Exec costs and limits**:
  - The whole file is kept in kernel memory for every running image (shared only across `fork`), and every segment is copied eagerly. Text pages are not shared between processes that exec the same file.
  - At most 32 argv and 32 envp entries in 4 KiB total, and no auxv beyond `AT_NULL`.
  - Programs larger than one 4 MiB `kmalloc` cannot be loaded.
- **Pids are never reused**, and `next_pid` is a plain `int`. procfs inode numbers (`PID_BASE + pid * 8`, `u32`) overflow after about 5×10⁸ pids.
- **Linear scans**: `proc_find` is O(n), `proc_nth` is O(n²), and a full `/proc` readdir calls `proc_nth_pid` once per entry. All of it runs under the global `proc_lock` with interrupts disabled.
- **Fatal-fault path with interrupts off**: `user_fault` → `proc_exit` (address-space teardown, closing files) runs with interrupts disabled. This lengthens interrupt latency.

## Potential improvements

1. **Add a permission check to `kill`** — In `do_kill`/`signal_send_pgrp`, allow the signal only if the sender's euid is 0 or its real/effective uid matches the target's real or saved uid. In-kernel senders (tty, monitor, `proc_exit`) would bypass the check. This closes an obvious hole now that setuid exists. Impact: high. Effort: small.
2. **Exec from the checked inode** — Make `image_load` read the file through the inode it already locked and permission-checked (an inode-based `vfs_read_all`), instead of looking the path up again. This removes the setuid TOCTOU. Impact: high. Effort: small.
3. **Harden the ELF parser** — Use overflow-safe range checks (`offset <= size && filesz <= size - offset`, and the same for `vaddr`/`memsz` and `phoff`), validate `phentsize`, and bounds-check every section, symbol and string access in `elf_symbolize`, including NUL termination. Add junk-ELF KTESTs for each case. Impact: high. Effort: small.
4. **Close the fork visibility window** — In `sys_fork`, set `pgid`, `sid`, creds and `sig_*` before the child is linked, or link it last under `proc_lock` once it is fully built. Also fix the `proc_spawn` failure path to unlink before `kfree`. Impact: medium. Effort: small.
5. **Fix init-waiter registration** — Let `proc_spawn` (or a flag argument) mark a process as kernel-waited before its thread starts, and reparent init's orphans to a kernel reaper instead of NULL. This removes the boot panic race and the leak. Impact: medium. Effort: small.
6. **Implement SIGCHLD and orphan semantics** — Honour `SA_NOCLDSTOP`, auto-reap when SIGCHLD is `SIG_IGN` or `SA_NOCLDWAIT`, add `WCONTINUED` and a SIGCHLD on continue. Send SIGHUP+SIGCONT to newly orphaned stopped groups, and return `EIO` instead of SIGTTIN for orphaned groups in `job_control`. Impact: medium. Effort: medium.
7. **Complete TTY job control** — Add `TOSTOP`/SIGTTOU on background writes and `tcsetpgrp`. Validate `TIOCSPGRP` against existing groups in the session. Refuse `TIOCSCTTY` while another live session owns the tty. Clear `tty.sid`/`pgrp` and send SIGHUP when the session leader exits. Protect `pgrp`/`sid` with `tty->lock`. Impact: medium. Effort: medium.
8. **Make the restart state explicit** — Keep a per-thread "in syscall" flag, and have `do_rt_sigreturn` bypass the `-ERESTARTSYS` check (for example by returning through a path that skips it), so restored user registers can never trigger a restart. Move the `nanosleep` deadline handling into the `SA_RESTART` path too. Impact: medium. Effort: small.
9. **Interrupt-enabled fatal-fault path** — Call `intr_on()` in `user_fault` before reporting and `proc_exit`, as `return_to_user_checks` does for `signal_deliver`. Address-space teardown and file closing should not run with interrupts masked. Impact: medium. Effort: small.
10. **Document and enforce owner-only signal fields** — Clarify in `proc.c` and `proc.h` which fields are `proc_lock`-protected and which are owner-only. Use `READ_ONCE`/`WRITE_ONCE`-style accessors for `sig_blocked` and `sigactions[].handler`, which are read across CPUs. Have `rt_sigaction` clear pending bits under `proc_lock`. Impact: low. Effort: small.
11. **Kick remote targets** — When `signal_send_locked` targets a thread that is running in user mode on another CPU, send it a reschedule IPI so the signal is delivered promptly rather than at the next tick. Impact: low. Effort: small.
12. **Pid hash and pid reuse** — Replace the linear `proc_find` and O(n²) `proc_nth` with a pid hash or sorted list, and recycle pids below a maximum. This makes `/proc` listings and `kill` scale, and avoids the procfs inode overflow. Impact: low. Effort: medium.
13. **Share program text** — Cache `exec_image` objects per inode, and map read-only segments from shared, refcounted pages instead of copying them eagerly in `elf_load`. Use a page list rather than one large `kmalloc`, which also lifts the 4 MiB program limit. Impact: medium. Effort: large.
14. **Fuller credential model** — Add `setreuid`/`setresuid` (and the gid versions), supplementary groups, clearing of setuid/setgid bits on `chown` and on writes by non-owners, and real-id semantics for `access()`. Impact: low. Effort: medium.
15. **`sigaltstack` and `SA_SIGINFO`** — Let a SIGSEGV handler run after a stack overflow, and pass the fault address to handlers, using the `stval` that `user_fault` already has. Impact: low. Effort: medium.
