# Threads, scheduling, synchronization and SMP

This subsystem provides kernel threads, a preemptive round-robin scheduler that runs on every CPU, and the locking primitives built on it: spinlocks, wait queues, mutexes and semaphores. It also brings up the secondary harts and provides inter-processor interrupts (IPIs), TLB shootdown and "stop the world" for the debug monitor and panics. A set of debugging aids is built in: lockdep (lock-order checking), a hung-thread detector, a hard-lockup watchdog, and per-CPU state that the monitor, procfs and GDB can display.

## Responsibilities and scope

**In scope:**

- The thread lifecycle: create, start, bind to a CPU, exit, join, detach, and freeing (`kernel/core/sched.c`).
- Scheduling: one global run queue, timer-tick preemption, idle threads, timed sleep, and wakeups (`sched.c`).
- The context switch itself (`kernel/arch/riscv/switch.S`).
- Per-CPU state reached through the `tp` register (`kernel/include/percpu.h`).
- Spinlocks with owner tracking and deadlock timeouts (`kernel/core/spinlock.c`).
- Sleeping primitives: wait queues, `wait_event`, mutexes, semaphores (`kernel/core/sync.c`, macros in `sched.h`).
- SMP: secondary hart bring-up, IPIs, TLB shootdown, stop-the-world (`kernel/arch/riscv/smp.c`).
- Debugging: lockdep (`kernel/debug/lockdep.c`), the hung-thread detector (`sched.c`), the hard-lockup watchdog (`kernel/arch/riscv/timer.c`), and the SMP parts of panic and the monitor.

**Out of scope (owned elsewhere):**

- The trap path that calls into the scheduler (`kernel/arch/riscv/trap.c`, `trapentry.S`). This document covers only how `trap.c` triggers preemption and IPIs.
- Processes, signals and `exit`/`wait4` (`kernel/proc/`). A process has exactly one thread. `proc.c` creates it with `kthread_create_suspended`, detaches it, and ends it with `thread_exit`. Signal delivery wakes sleepers through `wake_interruptible`.
- Kernel stack allocation and the guard pages (`kernel/mm/kstack.c`). The scheduler only calls `kstack_alloc`/`kstack_free`.
- Address-space switching (`vm_activate`, `kernel/mm/uvm.c`). The scheduler calls it on every switch.
- The timer hardware path (`sbi_set_timer`). `timer.c` is covered here only for the tick and the watchdog.

## Source map

| File | Role |
|---|---|
| `kernel/include/sched.h` | `struct thread`, `struct context`, thread states, wait queue, mutex and semaphore types; `wait_event` / `wait_event_interruptible` macros; `can_sleep()` |
| `kernel/core/sched.c` | Thread creation and exit, run queue and sleep queue, `__schedule`, idle loop, timed sleep, `sched_tick`, hung-thread detector, `ps`/`sched`/`bt` output |
| `kernel/arch/riscv/switch.S` | `context_switch` (saves and restores `ra`, `sp`, `s0`–`s11`), `thread_start` trampoline |
| `kernel/include/percpu.h` | `struct cpu`, `MAX_CPUS`, IPI kinds, `this_cpu()`, `current`, `this_cpu_id()`, `in_interrupt()`; `CPU_*` offsets used by assembly |
| `kernel/include/spinlock.h`, `kernel/core/spinlock.c` | Spinlocks (always irqsave), per-CPU held-lock stack, recursion and timeout detection, debug-path variants |
| `kernel/core/sync.c` | Wait queues, mutexes (owner tracking, deadlock-cycle detection), counting semaphores |
| `kernel/include/smp.h`, `kernel/arch/riscv/smp.c` | Secondary bring-up, IPI send/receive/poll, TLB shootdown, stop-the-world, `cpus` / `cpu <n>` output |
| `kernel/include/lockdep.h`, `kernel/debug/lockdep.c` | Lock-class dependency graph, inversion detection, "might sleep" check |
| `kernel/arch/riscv/timer.c` | Per-CPU tick (`HZ` = 100, from `kernel/include/irq.h`), `jiffies`, hard-lockup watchdog |
| `kernel/core/panic.c` | Panic serialization across CPUs, stopping others, reporting other CPUs |
| `kernel/debug/monitor.c` | Monitor entry/exit with stop-the-world; `ps`, `sched`, `cpus`, `cpu`, `bt`, `locks`, `lockdep` commands |
| `kernel/tests/test_sched.c` | KTESTs: join, leaks, semaphores, mutexes, preemption, sleep, wait queues, stack guard, tracing, lost-wakeup regression |
| `kernel/tests/test_smp.c` | KTESTs: harts online, bound threads, spinlock/mutex contention, IPIs, TLB shootdown, stop-the-world, churn, lockdep, hung detector |

## Concepts and data structures

### Per-CPU state: `struct cpu` (`percpu.h`)

There is a static array `struct cpu cpus[MAX_CPUS]` (`MAX_CPUS` = 8), defined in `sched.c`. While in the kernel, the `tp` register always points at the running hart's entry. `boot.S` sets it for cpu0. `secondary_entry` sets it for the other harts. `trapentry.S` swaps it in from `sscratch` on traps from user mode.

The first five fields are accessed from assembly, at fixed offsets checked by `_Static_assert`s at the top of `sched.c`:

| Offset | Field | Use |
|---|---|---|
| 0 | `cur_thread` | The running thread. `current` is `ld 0(tp)`. |
| 8 (`CPU_OVERFLOW_STACK`) | `overflow_stack_top` | Emergency stack for kernel stack overflows |
| 16 (`CPU_OVERFLOW_SP`) | `overflow_sp` | `sp` at an overflowing trap; 0 normally |
| 24 (`CPU_SCRATCH`) | `scratch` | Trap-entry temporary |
| 32 (`CPU_KSTACK_TOP`) | `kstack_top` | Top of the current thread's kernel stack (for traps from user mode). Before a secondary starts, this is its boot `sp`. |

The other important fields, by group:

| Group | Fields | Meaning |
|---|---|---|
| Identity | `hartid`, `id`, `online` | Hart id from firmware; dense CPU index; set once the CPU runs the scheduler |
| Interrupts | `irq_depth`, `irq_tf`, `last_irq`, `irq_count[16]` | Nesting depth (>0 = in an interrupt handler), the interrupted frame, statistics |
| Scheduling | `need_resched`, `idle`, `switch_from`, `prev_dead`, `prev_dead_free`, `last_switch`, `switches`, `migrations` | See "How it works" |
| Lock debugging | `nheld`, `held[MAX_HELD_LOCKS]` (8), `in_lockdep` | Stack of spinlocks held by this CPU, innermost last |
| Tick / watchdog | `next_deadline`, `ticks`, `missed_ticks`, `subticks`, `last_tick` (volatile), `lockup_reported` | `last_tick` is the heartbeat the watchdog checks |
| IPIs | `ipi_pending` (volatile bitmask of `enum ipi_kind`), `ipi_sent[]`, `ipi_received[]`, `tlb_done_gen` | Mailbox and shootdown acknowledgement |
| Stop-the-world | `stopped` (volatile), `stop_tf`, `stop_pc`, `stop_fp` | Where this CPU was when it parked |

The accessors in `percpu.h` are:

- `this_cpu()`: `mv %0, tp`. The pointer is only valid while the thread cannot migrate, which means while interrupts are off.
- `get_current()` / `current`: a single `ld 0(tp)`. It is migration-safe because the load is atomic with respect to preemption.
- `this_cpu_id()`: a single `lw` of `id` relative to `tp`. Migration-safe.
- `in_interrupt()`: a single `lw` of `irq_depth`, `> 0`.

### Threads: `struct thread` (`sched.h`)

| Field(s) | Meaning |
|---|---|
| `ctx` | Callee-saved registers (`struct context`: `ra`, `sp`, `s[12]`). It must be the first field (`_Static_assert` in `sched.c`; `switch.S` uses offsets 0..104). |
| `tid`, `name[16]` | Thread id from `next_tid` (boot thread is 0) and its name |
| `state` | `T_RUNNING` ("R"), `T_RUNNABLE` ("r"), `T_SLEEPING` ("S"), `T_DEAD` ("D") |
| `on_runq` | Linked into the run queue via `run_node` |
| `on_cpu` | Its registers are live on some CPU, or not yet saved. Cleared by the *next* thread in `finish_switch`. |
| `detached` | Nobody will join it; it is freed right after its final switch |
| `exit_done` | (volatile) The post-exit wakeup is finished, so the thread may be freed |
| `kstack`, `kstack_size` | Lowest usable stack address; `KSTACK_SIZE` = 16 KiB. The top `TRAP_FRAME_SIZE` (304) bytes are reserved for the user trap frame (`thread_user_tf`). |
| `proc` | The owning process, or NULL for kernel threads. Selects the page table in `__schedule`. |
| `interruptible` | In an interruptible sleep: `wake_interruptible` (signals) may wake it |
| `fn`, `arg`, `exit_code`, `exit_wq` | Entry point, exit status, and the wait queue `kthread_join` sleeps on |
| `wait_what`, `wait_obj` | What a sleeper waits for, shown by `ps` ("mutex", "sem", "sleep", "join", "start", "poll", or a wait queue name) |
| `blocked_on`, `blocked_since`, `hung_reported` | Mutex wait tracking for deadlock detection and the hung-thread detector |
| `nheld_mutex`, `held_mutex[MAX_HELD_MUTEX]` (8) | Mutexes held, innermost last (lockdep, `locks`) |
| `cpu`, `bound_cpu`, `migrations` | Where it runs or last ran; pinned CPU or -1; migration count |
| `wake_time`, `on_sleepq`, `sleep_node` | Timed sleep: deadline in `rdtime` units, linked into `sleepq` |
| `all_node` | Linked into the global `all_threads` list |
| `runtime`, `switches`, `created`, `create_site` | Accounting and the creator's return address |

**State invariants (under `sched_lock`):**

- A thread is on at most one of: running on a CPU (`on_cpu`), or on the run queue (`on_runq`). There is one transient exception: between `__schedule` putting `prev` on the queue and the next thread's `finish_switch`, `prev` has both set. It cannot be picked up by another CPU in that window because `sched_lock` stays held across the switch (see below).
- `state == T_SLEEPING` with `on_runq` set is legal. This is a **preempted sleeper**: it was preempted between `prepare_to_wait` and `schedule()`. It stays `T_SLEEPING` so that a wakeup still finds it, but it gets CPU time to re-check its condition. Such a thread can even be running and still show "S".
- `T_DEAD` threads are never enqueued. After `finish_switch` they have `on_cpu == false`.
- The idle thread is never on the run queue. `__schedule` marks it `T_RUNNABLE` when it is switched out and `T_RUNNING` when it continues.

### Global scheduler state (`sched.c`)

| Variable | Protected by | Meaning |
|---|---|---|
| `sched_lock` (spinlock "sched") | itself | The scheduler lock |
| `runq` | `sched_lock` | FIFO of runnable threads, shared by all CPUs |
| `sleepq` | `sched_lock` | Timed sleepers sorted by `wake_time` (`sleepq_insert`) |
| `all_threads` | `sched_lock` | Every thread, including dead joinable ones |
| `next_tid` | `sched_lock` | Next thread id |
| `boot_thread` | — | The boot context; becomes thread 0, "idle/0" |
| `name_cache[64]` | `sched_lock` | Names of recently freed threads, for `thread_name()` (trace decoding) |
| `sched_ready` | — | Set at the end of `sched_init`; `__schedule` is a no-op before that |
| `hung_ticks` | — | Hung-thread threshold in `rdtime` ticks (0 = off) |
| `monitor_entry_time` | — | Non-zero while the monitor runs. It freezes accounting and disables lockdep. |

### Synchronization objects

```
spinlock_t   { volatile u32 locked; int cpu; const char *name; uintptr_t owner_pc;
               u64 acquisitions, contentions; u16 ld_class; }
waitqueue    { spinlock_t lock; list_head waiters; const char *name; }
wait_entry   { list_head node; struct thread *t; bool queued; }     (on the waiter's stack)
mutex        { spinlock_t lock; struct thread *owner; uintptr_t owner_pc;
               waitqueue wq; const char *name; u64 contentions; u16 ld_class; }
semaphore    { spinlock_t lock; long count; waitqueue wq; const char *name; }
```

Static initializers: `SPINLOCK_INIT(name)`, `WAITQUEUE_INIT(wq, name)`, `MUTEX_INIT(m, name)`. Runtime initializers: `spin_init`, `waitqueue_init`, `mutex_init`, `sem_init`.

A mutex's internal spinlock and its wait queue's spinlock both take the mutex's name. lockdep therefore puts them in one spinlock class with that name, separate from the mutex class (classes are keyed by kind and name).

### IPI kinds (`percpu.h`)

| Kind | Effect on the target |
|---|---|
| `IPI_RESCHED` | Sets `need_resched`. The trap path then preempts on return. |
| `IPI_TLB_FLUSH` | `sfence.vma`, then publish `tlb_done_gen` |
| `IPI_STOP` | Park in `cpu_park` until the stop owner releases |

### Context switch frame (`switch.S`)

`context_switch(old, new)` stores `ra, sp, s0..s11` at offsets 0..104 of `*old`, loads the same from `*new`, and does `ret`. A new thread has `ctx.ra = thread_start`, `ctx.sp = kstack + KSTACK_SIZE - TRAP_FRAME_SIZE` and `ctx.s[0] = 0` (end of the frame-pointer chain). `thread_start` clears `ra` and jumps to `thread_entry`, so backtraces end cleanly. A sleeping thread's backtrace starts at `ctx.ra` with frame pointer `ctx.s[0]`.

## How it works

### Boot and bring-up

1. `cpu_early_init` (first thing in `kmain`) sets cpu0's `id = 0`, `hartid` and overflow stack.
2. `sched_init` turns the boot context into thread 0, "idle/0". It is bound to cpu0, `T_RUNNING`, `on_cpu`, detached, and runs on `boot_stack`. The unused part of the boot stack is poisoned with `STACK_POISON` (0x57) for high-water reporting. `sched_init` sets `cur_thread`/`idle`, sets `sched_ready`, and calls `hung_init` (boot option `hung=`, default 30 s).
3. `timer_init` (in `kmain` after `sched_init`) computes `interval = timebase / HZ`, reads `watchdog=` (default 10 s), and arms cpu0's timer.
4. `smp_init`:
   - enables the software interrupt (`SIE_SSIE`) on cpu0 and marks cpu0 online;
   - reads `smp=` (capped at `MAX_CPUS`). With `smp<=1`, a single hart, or no SBI HSM extension, it stays uniprocessor;
   - otherwise, for each non-boot hart in `platform.hartids`, calls `start_cpu(id, hart)` with dense ids starting at 1.
5. `start_cpu`:
   - allocates an overflow stack (`kstack_alloc`) and an idle thread (`sched_create_idle`: bound to its CPU, `T_RUNNING`, `on_cpu`, detached, added to `all_threads`);
   - fills in `cpus[id]`, with `kstack_top` = the idle stack top;
   - calls `sbi_hart_start(hartid, __pa(secondary_entry), cpu)`, then waits up to 2000 ms for `c->online`. On failure it stops bringing up more harts.
6. `secondary_entry` (`boot.S`) switches to the early page table, jumps high, switches to `kernel_pagetable`, sets `tp = a1` (the `struct cpu *`), loads `sp` from `CPU_KSTACK_TOP(tp)`, and calls `secondary_main(c)`.
7. `secondary_main` runs `trap_init`, enables IPIs, does `sfence.vma`, and calls `timer_init_cpu`. It sets `last_switch`, atomically increments `ncpus_online`, then release-stores `online = true` and enters `cpu_idle_loop`.
8. `kmain` creates "kinit", detaches it, and calls `cpu_idle_loop` as idle/0.

### Thread creation and start

`create_thread` (behind `kthread_create` and `kthread_create_suspended`):

1. `kzalloc`s the thread and gets a stack from `kstack_alloc`.
2. Sets `bound_cpu = -1`, name, `fn`, `arg`, `created`, `create_site` (the caller's return address), and `exit_wq`. Initializes `ctx` as described above.
3. Under `sched_lock`: assigns the `tid`, adds the thread to `all_threads`, and either calls `enqueue` (running) or sets `T_SLEEPING` with `wait_what = "start"` (suspended).
4. Emits the `thread_create` trace event, with the first 16 bytes of the name packed into two `u64`s.

`kthread_bind(t, cpu)` sets `bound_cpu` under `sched_lock`. It is meant for not-yet-started threads. A `cpu >= ncpus_online` silently becomes -1. `kthread_start(t)` clears `wait_what` and calls `wake_thread`.

A new thread first runs `thread_entry` (via `thread_start`). At that point it holds `sched_lock`, which the previous thread on this CPU took in `__schedule`. `thread_entry` calls `finish_switch` and unlocks, then calls `post_switch` and turns interrupts on. After that it calls `fn(arg)` and finally `thread_exit(0)`.

### `__schedule(bool preempt)`: the core

`schedule()` calls `__schedule(false)` (voluntary). `preempt_schedule()` calls `__schedule(true)` (from the timer path). `yield()` is `schedule()`.

1. Return at once if `!sched_ready`.
2. **Save `SIE`, then `intr_off()`, *then* read `this_cpu()`.** If the order were reversed, the thread could be preempted after reading `tp`, migrate, and then switch out another CPU's current thread.
3. Sanity checks:
   - panic if this CPU holds any spinlock ("schedule() while holding spinlock ...");
   - panic if `irq_depth > 0` and the thread is `T_SLEEPING` ("tried to sleep in interrupt context").
4. Take `sched_lock` and clear `need_resched`.
5. Decide what happens to `prev`:
   - the idle thread: mark it `T_RUNNABLE` (it is never queued);
   - preemption of a `T_SLEEPING` thread: append it to `runq` **without changing its state** (the lost-wakeup rule, below);
   - `T_RUNNING` or `T_RUNNABLE`: `enqueue(prev)`, which sets `T_RUNNABLE`. A voluntary `schedule()` by a thread that was woken in the meantime lands here and is simply requeued;
   - `T_SLEEPING` (voluntary) or `T_DEAD`: leave it off the queue.
6. `next = dequeue(c->id)`: the oldest thread on `runq` whose `bound_cpu` is -1 or this CPU. If there is none, `next` is this CPU's idle thread.
7. If `next == prev`: set `T_RUNNING` if it was `T_RUNNABLE`, unlock with the saved flags, and return.
8. Otherwise:
   - charge `now - last_switch` to `prev->runtime`;
   - emit `sched_switch(prev, next, prev->state)`;
   - mark `next` `T_RUNNING` if it was `T_RUNNABLE` (a preempted sleeper stays `T_SLEEPING`) and set `next->on_cpu`;
   - count a migration if `next->cpu != c->id` (and it has run before);
   - set `cur_thread = next`, `switch_from = prev`, and `kstack_top`;
   - call `vm_activate(next->proc ? next->proc->mm : NULL)` and clear `SSTATUS_SUM` (user access never leaks across threads);
   - call `context_switch(&prev->ctx, &next->ctx)`.
9. **Back in `prev`, possibly on a different CPU:**
   - re-read `c = this_cpu()`. Panic if `c->cur_thread != prev`;
   - `finish_switch(c, c->switch_from)`;
   - release `sched_lock` (flags 0: interrupts stay off);
   - `post_switch(c)`;
   - only then restore interrupts if they were on.

`finish_switch(c, from)` runs in the incoming thread with `sched_lock` held. It clears `from->on_cpu`. If `from` is `T_DEAD`, it records it in `c->prev_dead` and decides, under the lock, whether to free it (`prev_dead_free = from->detached`). A detached dead thread is also removed from `all_threads` there.

`post_switch(c)` runs after `sched_lock` is released, with interrupts still off:
- if the dead predecessor is to be freed, it calls `free_thread`, which saves the name in `name_cache` and calls `kstack_free` and `kfree`;
- otherwise it calls `wake_up(&dead->exit_wq)` and then release-stores `exit_done = true`.

Interrupts must stay off until `post_switch` is done. Otherwise the thread could be preempted, resume on another CPU, and process *that* CPU's `prev_dead`.

### Sleeping and waking

**Wait queues** (`sync.c`):

- `prepare_to_wait` takes the queue lock, links the entry if not queued, then takes `sched_lock` and sets `T_SLEEPING`. It sets `wait_what`/`wait_obj` if they are still unset.
- `finish_wait` unlinks the entry and sets `T_RUNNING` under `sched_lock`, then clears `wait_what`/`wait_obj`.
- `wait_event(wq, cond)` is the loop `prepare_to_wait; if (cond) break; schedule();`, then `finish_wait`. The condition is re-checked after every wakeup.
- `wait_event_interruptible` also sets `current->interruptible`. It returns -4 (`-EINTR`) if `thread_signal_pending(current)` becomes true.
- `do_wake` (behind `wake_up` and `wake_up_one`) holds the queue lock. For each entry it unlinks it and calls `wake_thread`. `wake_up_one` stops after the first entry for which `wake_thread` returned true. Every entry it passes over (whose thread was not sleeping) is unlinked as well.
- `add_wait_queue` / `remove_wait_queue` only link and unlink, without touching the state (for `poll`).

**`wake_locked(t)`** (under `sched_lock`): returns false unless `t` is `T_SLEEPING`. It removes `t` from `sleepq` and emits `sched_wakeup`. Then:
- if `t->on_cpu`, it only sets `T_RUNNABLE`: the thread is still on its way into `schedule()`, which will requeue it;
- otherwise it calls `enqueue(t)`.

`wake_thread` and `wake_interruptible` (which also requires `t->interruptible`) wrap it with the lock.

**`enqueue(t)`** sets `T_RUNNABLE`. If `t` is not already queued, it appends `t` to `runq` and calls `kick_idle_cpu(t)`:

- a bound thread gets an `IPI_RESCHED` sent to its CPU (if that is not this CPU, the CPU is online, and it has no resched IPI pending);
- otherwise, if this CPU is busy (not running its idle thread) and more than one CPU is online, the first online idle CPU without `need_resched` and without a pending resched IPI gets one. Only one CPU is kicked per enqueue.
- an idle CPU never kicks others. Its own idle loop calls `schedule()` after every interrupt.

**Timed sleep:**

- `sleep_ms(ms)` loops until `rdtime()` reaches the deadline. Each iteration does the following under `sched_lock`: set `interruptible`, check for a pending signal (and stop if there is one), `sleepq_insert`, set `T_SLEEPING` and `wait_what = "sleep"`. Then it calls `schedule()`. A pending signal ends the sleep early; other early wakeups loop. `sleep_ms(0)` is `yield()`.
- `sched_tick` (every scheduler tick on every CPU) wakes all `sleepq` entries whose `wake_time <= now`. It sets `need_resched` if this CPU is not idle or the run queue is non-empty. On cpu0, once per `HZ` ticks, it also calls `hung_thread_check`.

**Multi-source waits** (`poll`, `netd`):

- `set_current_sleeping(what)` marks the caller `T_SLEEPING` and interruptible *before* it checks its sources. A wakeup in between therefore makes the thread runnable, and the following sleep returns at once.
- `schedule_until(deadline)` inserts the thread into `sleepq` if it is still sleeping and a deadline is given, calls `schedule()`, and removes it from `sleepq` afterwards.
- `set_current_running()` ends the wait.

### Preemption

`timer_interrupt` (`timer.c`) reprograms the SBI timer, updates `last_tick` (the heartbeat), and optionally takes a profile sample. On every `per_tick`-th interrupt (1 unless the profiler raised the rate) it also:
- increments `jiffies` (cpu0 only) and `ticks`;
- calls `watchdog_check`;
- calls `sched_tick`.

`handle_interrupt` in `trap.c` calls `preempt_schedule()` on the way out if all of these hold:
- `need_resched` is set;
- `irq_depth == 0`;
- `sched_ready`;
- the interrupted context had interrupts enabled (`SSTATUS_SPIE`).

Code that runs with interrupts off (which includes every spinlock holder) is therefore never preempted. On the way back to user mode, `return_to_user_checks` calls `yield()` if `need_resched` is set. The effective time slice is one tick (10 ms at `HZ` = 100). The trap exit restores the *live* `tp`, not the saved one, because a preempted kernel context may resume on another CPU.

### The lost-wakeup rule (why preemption never sleeps a thread)

Consider a thread preempted inside `wait_event` after `prepare_to_wait` set `T_SLEEPING` but before it calls `schedule()`. If `__schedule(true)` took it off the CPU as a sleeper, a wakeup that had already happened (and been consumed) would be lost forever. So a preempted `T_SLEEPING` thread is requeued with its state unchanged. It only sleeps when it calls `schedule()` itself after rechecking its condition. `test_sched.c: preemption_does_not_lose_wakeups` is the regression test: the condition is already true and the wakeup already spent, then the waiter is "preempted" right after `prepare_to_wait`. With the bug, the test hangs.

The other half is the `on_cpu` check in `wake_locked`. A wakeup that arrives while the target is between "set `T_SLEEPING`" and the switch only marks it `T_RUNNABLE`, and `__schedule` requeues it instead of parking it.

### Exit, join and detach

`thread_exit(code)`:
1. Panics on the idle thread.
2. Turns interrupts off, so there is no preemption between `T_DEAD` and the final switch.
3. Records `exit_code` and sets `T_DEAD` under `sched_lock`.
4. Calls `schedule()` and never returns.

The next thread on that CPU then runs `finish_switch` and `post_switch`.

`kthread_join(t)`:
1. Panics on self-join and on detached threads.
2. Waits on `t->exit_wq` until `t->state == T_DEAD && !t->on_cpu`.
3. Spins in `wait_exit_done` until `exit_done`. The exiting CPU may still be inside `post_switch`, calling `wake_up(&t->exit_wq)`.
4. Unlinks `t` from `all_threads` and frees it.

`kthread_detach(t)` sets `detached` under `sched_lock`:
- if `t` has not yet passed `finish_switch`, `finish_switch` will see `detached` and free it;
- if `t` is already `T_DEAD && !on_cpu`, `finish_switch` decided "announce, don't free", so `kthread_detach` unlinks it, waits for `exit_done`, and frees it itself.

The decision is made exactly once, under the lock, so a race between `detach` and the final switch cannot double-free or leak.

A thread that is never joined or detached stays in `all_threads` as `T_DEAD` forever. `ps` shows it as "(exited; awaiting join)".

### Idle loop

`cpu_idle_loop`:
1. Panic if this CPU's `cur_thread` or `current` is not its idle thread.
2. `intr_off()`.
3. If `runq` is empty and `need_resched` is clear, `wfi`.
4. `intr_on()`. Pending interrupts are taken here.
5. `schedule()`.

The check-then-`wfi` with interrupts off has no lost-wakeup window. `wfi` returns on any pending enabled interrupt even with `SIE` clear, and the interrupt is taken at step 4.

### Mutexes

`mutex_lock(m)`:
1. Panics in interrupt context, and on recursive locking (names both sites).
2. Calls `lockdep_might_sleep` and `lockdep_acquire(LD_MUTEX)`.
3. Tries `trylock_at`: under `m->lock`, take the mutex if `owner == NULL`, recording `owner`, `owner_pc` and pushing onto the thread's `held_mutex[]`.
4. On contention:
   - bumps `contentions` and emits `mutex_contended`;
   - sets `blocked_since`, `blocked_on`, `wait_what = "mutex"` and `wait_obj`;
   - runs `check_deadlock`, which follows owner → `blocked_on` → owner for up to 16 links and panics with "MUTEX DEADLOCK detected" and the full chain if it leads back to the caller;
   - then `wait_event(&m->wq, trylock_at(m, pc))`.
5. Clears `blocked_on`. If the hung detector had reported this wait, it logs "finally got mutex".

`mutex_unlock` panics unless the caller is the owner. Under `m->lock` it clears the owner and pops `held_mutex`, then calls `wake_up_one(&m->wq)`. There is no handoff. The woken waiter competes with any other thread calling `trylock_at` (barging).

`mutex_trylock` does not call lockdep (a trylock cannot deadlock), but it does push onto `held_mutex[]`, so later acquisitions record edges from it.

### Semaphores

- `sem_trydown`: decrement under `s->lock` if `count > 0`.
- `sem_down`: `sem_trydown`, else `wait_what = "sem"` and `wait_event(&s->wq, sem_trydown(s))`.
- `sem_up`: increment, then `wake_up_one`.

There is no lockdep and no owner tracking for semaphores.

### Spinlocks

`spin_lock_irqsave(l)`:
1. Saves `SIE` and disables interrupts.
2. Panics on recursion: `l` is locked and `l->cpu` is this CPU (only when `panic_depth == 0`).
3. Calls `lockdep_acquire(LD_SPIN)`.
4. Spins on `__atomic_exchange_n(&l->locked, 1, ACQUIRE)`. On the first failed attempt it bumps `contentions`. **Every iteration calls `ipi_poll()`**. After `SPIN_TIMEOUT` (100,000,000) iterations it panics with "deadlock: spinlock 'x' held by cpu N at <site> never released".
5. Records `cpu`, `owner_pc` and `acquisitions`, and pushes onto `cpu->held[]`.

`spin_unlock_irqrestore(l, flags)` panics if `l` is not held by this CPU (unless already panicking). Otherwise it pops `held[]`, clears the owner, release-stores `locked = 0`, and re-enables interrupts if `flags` says so. Locks may be released in any order. `pop_held` searches the stack.

`spin_trylock_irqsave` makes one exchange attempt and skips lockdep. `spin_lock_debug` / `spin_unlock_debug` are for printing code (`ps`, procfs, trace names): they take the lock normally, but only *try* it when panicking, when the world is stopped, or when this CPU already holds it. The caller then prints an unlocked snapshot.

### IPIs

- `smp_send_ipi(cpu, kind)` atomically ORs the bit into the target's `ipi_pending` (SEQ_CST), counts it, emits `ipi_send`, and calls `sbi_send_ipi(1, hartid)`.
- `ipi_handle(tf)` (software interrupt from `handle_interrupt`) clears `sip.SSIP` and atomically takes all pending bits. Then:
  - `RESCHED` sets `need_resched`;
  - `TLB_FLUSH` calls `tlb_flush_ack`;
  - `STOP` calls `cpu_park(tf, …)`.
- `ipi_poll()` is called from every spin loop with interrupts off: spinlocks, the lockdep raw lock, shootdown waits, and parked CPUs (TLB only). It services only `TLB_FLUSH` and `STOP`. `RESCHED` stays pending for the real interrupt. When it parks, it records its caller's pc and frame pointer (`CALLER_FP()`), so the monitor shows the spin loop's location.

### TLB shootdown

Only kernel mappings need it. A user page table is live on one CPU at a time, and `vm_activate` flushes whenever `satp` changes.

`tlb_shootdown()`:
1. Disables interrupts and flushes locally.
2. If more than one CPU is online:
   - atomically increments `tlb_gen` to `g` and sets its own `tlb_done_gen = g`;
   - sends `IPI_TLB_FLUSH` to every CPU whose `tlb_done_gen < g`;
   - spins until every CPU's `tlb_done_gen >= g`, calling `ipi_poll` meanwhile. It panics after `SHOOTDOWN_TIMEOUT_MS` (3000 ms) with "TLB shootdown: cpu N did not respond".

On the target, `tlb_flush_ack` reads `tlb_gen` (acquire) *before* its `sfence.vma`, then publishes that generation. Its flush therefore covers every change made before that generation. Because waiters poll, two CPUs shooting down at the same time each service the other's request and cannot deadlock. A parked CPU also services TLB flushes in `cpu_park`.

The main user is `kstack.c: unmap_pages`, used when a freed stack does not fit in the stack cache. `vm_unmap` callers on the kernel page table must call it too.

### Stop-the-world

A single global `stop_owner` (-1 when nobody owns it) serializes the debug monitor and panics.

- `smp_stop_others(&missing)`:
  - claims `stop_owner` with a CAS (or already owns it) and returns false if another CPU owns it;
  - sends `IPI_STOP` to every CPU not already `stopped`;
  - waits up to `STOP_TIMEOUT_MS` (200 ms, measured from the start for all CPUs together);
  - returns the CPUs that did not park as a bitmask in `missing`.
- `cpu_park(tf, pc, fp)`:
  - records `stop_tf`/`stop_pc`/`stop_fp` (from the trap frame for an IPI, from pc/fp for a spin loop) and emits `cpu_stop`;
  - release-stores `stopped = true`, then spins while `stop_owner >= 0 && stop_owner != me`, servicing TLB flushes;
  - on release, adds the stopped time to `last_switch` (not charged as CPU time), refreshes `last_tick` (not a lockup), and clears `stopped`.
- `smp_resume_others` (owner only) refreshes every CPU's `last_tick`, then releases `stop_owner`.
- `smp_park_self(tf)` lets a CPU that lost the race for `stop_owner` park until it is free.

A CPU spinning on a spinlock parks from `ipi_poll`. A CPU with interrupts off that is *not* spinning cannot park. It is reported as missing: "did NOT stop" in the monitor, "did not stop: interrupts off and not spinning on a lock" in panics.

**Monitor** (`monitor_enter`): interrupts off. Unless this CPU already owns the stop (as during a panic), it loops `smp_stop_others` / `smp_park_self` until it owns it. On the outermost entry it sets `monitor_entry_time`. On exit it adds the time spent to this CPU's `last_switch`, clears `monitor_entry_time`, and calls `smp_resume_others` if it took the stop.

**Panic** (`panic_begin`):
1. Interrupts off.
2. CAS on `panic_cpu`: a second CPU that panics parks forever (`smp_park_self` in a loop).
3. `panic_depth > 2` halts with "recursive panic".
4. On the first panic, loops `smp_stop_others` / `smp_park_self`. If the monitor holds the world on another CPU, the panic waits for it.

`panic_other_cpus` then prints, for every other CPU: its thread, interrupt context, held spinlocks, and either a backtrace from `stop_pc`/`stop_fp` or "did not stop". `panic_finish` exits QEMU with `panic=shutdown`, or enters the monitor.

### Hung-thread detector

`hung_thread_check` runs once a second from cpu0's tick, and directly from tests. It only *tries* `sched_lock`; if the lock is busy, it waits for the next second. It looks for the first thread with all of:
- `blocked_on` set;
- `T_SLEEPING`;
- not yet reported;
- waiting longer than `hung_ticks`.

It reports "HUNG THREAD: N (name) has waited X ms for mutex 'm'", the owner and its lock site, the waiter's backtrace, and either the owner's backtrace (if it is not on a CPU) or "owner is running on cpuN". All of this is printed while holding `sched_lock`, so neither thread can move. Each wait is reported once (`hung_reported`). Only mutex waits are covered.

### Hard-lockup watchdog

`watchdog_check` runs on every scheduler tick on every CPU. It is skipped when any of these hold:
- `lockup_ticks == 0` (`watchdog=0`);
- fewer than 2 CPUs online;
- the world is stopped;
- the monitor is active;
- a panic is in progress.

For every other online, non-stopped CPU whose `last_tick` is older than `lockup_ticks`, it CAS-sets `lockup_reported` and panics: "watchdog: hard lockup on cpuN: no timer interrupt for X ms (interrupts off); it is running thread T (name); holding 'a', 'b'". The panic's stop-the-world then shows where that CPU is, if it is spinning.

### lockdep

Classes are `(kind, name)` pairs, with up to `LD_CLASSES` = 192 (class 0 means "none"). Each lock caches its class id in `ld_class` on first use. `deps[a]` is a bitmap of classes `b` acquired while `a` was held. The first `LD_EDGES` = 1024 edges are also recorded with thread, CPU and both sites.

`lockdep_acquire` runs before every `spin_lock_irqsave` and `mutex_lock`. It is skipped when lockdep is disabled, during a panic, or in the monitor. With interrupts off and `cpu->in_lockdep` set as a recursion guard, it adds an edge from every spinlock this CPU holds, and from every mutex the current thread holds (unless in an interrupt handler), to the new class.

`add_dep(from, to)`:
- skips `from == to` (same-class nesting is not checked) and edges that already exist;
- under a raw test-and-set lock that lockdep does not track (spinning with `ipi_poll`), runs a BFS from `to` looking for `from` (`find_path`);
- if it finds a path, reports "LOCKDEP: possible deadlock: lock order inversion" with each link's recorded sites and a backtrace, then either panics (`lockdep=panic`) or disables lockdep globally (default `warn`, "further checking disabled"). In `lockdep_testing` mode it only counts.

`lockdep_might_sleep` (from `mutex_lock`) reports "LOCKDEP: mutex_lock at X while holding spinlock 'y' ...: it may sleep", even when the mutex is uncontended.

## Concurrency and locking

These are the rules every kernel programmer working in Eunuchs must follow. Most violations are detected at run time. The detection is noted after each rule.

### Rule 1: Per-CPU data and migration

- **`current`, `this_cpu_id()` and `in_interrupt()` are always safe.** Each is a single `tp`-relative load, so preemption cannot split it.
- **`this_cpu()` (a `struct cpu *`) may only be held while interrupts are off.** With interrupts on, the thread can be preempted after reading `tp` and resume on another CPU, where the pointer refers to someone else's state. Interrupt handlers, spinlock holders and the scheduler may keep it. Example: `thread_entry` drops `c` before `intr_on()` ("from here on we may migrate: no more `c`").
- **After any call that can switch threads, re-read `this_cpu()`.** This includes `context_switch`, `schedule()`, `yield()`, `sleep_ms`, `mutex_lock`, `wait_event` and `preempt_schedule()`. `__schedule` does this after `context_switch` and checks that `c->cur_thread == prev`; a mismatch panics with "resumed on cpuN, whose current thread is ...".
- **In `__schedule`, interrupts go off *before* the CPU state is read.** Interrupts stay off until `post_switch` has finished with that CPU's `prev_dead`.
- `spin_held_by_me` reads the CPU id with one load. This is safe because a thread never migrates while it holds a spinlock.

### Rule 2: Spinlocks

- Every spinlock acquisition disables interrupts (irqsave). There is no non-irqsave variant. Holding a spinlock therefore also means "not preemptible, not migrating".
- **Never sleep, schedule or take a mutex while holding a spinlock.** `__schedule` panics ("schedule() while holding spinlock"), and `lockdep_might_sleep` reports `mutex_lock` under a spinlock even when uncontended. `sem_down` and `wait_event` are only caught when they actually reach `schedule()`.
- **No recursion.** Taking a lock this CPU already holds panics and names both sites.
- **Unlock only on the CPU that locked.** This is checked by `spin_unlock_irqrestore`.
- **Keep critical sections short.** Spinners are bounded by `SPIN_TIMEOUT` (1e8 iterations) and then panic. Interrupts off for 10 s triggers the watchdog. Interrupts off for 3 s while another CPU is shooting down TLBs panics the shootdown, unless the CPU is spinning (spinners poll IPIs).
- A CPU holds at most `MAX_HELD_LOCKS` (8) spinlocks in its debug stack. Deeper nesting still works, but the extra locks are not shown and not checked by lockdep.
- In debug and printing code, use `spin_lock_debug` / `spin_unlock_debug` or `spin_trylock_irqsave`. This keeps a wedged or stopped kernel from deadlocking the monitor.

### Rule 3: Sleeping

- `can_sleep()` is false before `sched_ready`, in interrupt handlers, and with interrupts disabled (which includes the monitor).
- `mutex_lock` in interrupt context panics. Sleeping in interrupt context panics in `__schedule`.
- **Always sleep through a condition loop.** `wait_event` / `wait_event_interruptible` mark the thread sleeping *before* testing the condition, and re-test after every wakeup. A waker must make the condition true *before* calling `wake_up`. Both sides synchronize through the wait queue's lock: if the waker's `wake_up` takes the lock after the waiter queued itself, it finds the waiter; if before, the waiter's later lock acquisition sees the condition. Wakeups can be spurious (`sleep_ms` loops; `wake_up_one` may skip entries), so never assume a wakeup means the condition holds.
- For waits on several sources, call `set_current_sleeping()` before checking anything, then `schedule_until()`, then `set_current_running()`. See `fs/poll.c` and `net/net.c`.
- Interruptible sleepers set `interruptible` and check `thread_signal_pending` after taking `sched_lock` (or after `prepare_to_wait` took it). `wake_interruptible` checks the flag under the same lock, so a signal cannot fall in between.
- **Never call `__schedule` with preempt=true from code that wants to sleep.** Preemption never puts a thread to sleep (see the lost-wakeup rule).

### Rule 4: Mutexes

- Only the owner may unlock; a foreign or double unlock panics. Recursive locking panics.
- A direct deadlock cycle through `blocked_on` links (up to 16 deep) panics at the moment it forms. Inversions that have not yet deadlocked are lockdep's job.
- Mutexes are not fair. Waiters are woken one at a time and compete with new lockers.
- A thread that exits while holding a mutex is not detected at exit. Its waiters show up later in the hung-thread detector.

### Lock order

| Order (outer → inner) | Where |
|---|---|
| wait queue lock → `sched_lock` | `prepare_to_wait`, `finish_wait`, `do_wake` (via `wake_thread`) |
| `sched_lock` → printk's `log_lock` | `hung_thread_check` prints while holding `sched_lock`; `pr_debug`s inside `create_thread` run *after* unlocking |
| `sched_lock` → (none: IPI send) | `kick_idle_cpu` sends IPIs while holding `sched_lock`; `smp_send_ipi` takes no lock |
| `mutex.lock`, `semaphore.lock` → nothing | Released before any wait queue operation |
| `kstack_lock` taken after `sched_lock` is released | `post_switch` → `free_thread` → `kstack_free` |

`sched_lock` is taken last in the scheduler paths. Never take a wait queue lock, a mutex or anything that may call `wake_up` while holding `sched_lock`. lockdep enforces the order it has seen. `post_switch` exists precisely so that freeing and `wake_up(exit_wq)` happen after `sched_lock` is dropped.

### `sched_lock` held across `context_switch`

`sched_lock` is acquired by the outgoing thread and released by the incoming one: in `__schedule` after `context_switch` returns, or in `thread_entry` for a new thread. Both run on the same CPU, so the per-CPU `held[]` stack and `l->cpu` stay consistent. This guarantees that a thread requeued by CPU A cannot be dequeued by CPU B before A has saved its registers. B needs `sched_lock` to dequeue, and A releases it only after `context_switch` has stored `prev->ctx`. `on_cpu` records the same fact for code that sees the thread outside this window (`kthread_join`, `wake_locked`, `running_now`).

### What `sched_lock` protects

`runq`, `sleepq`, `all_threads`, `next_tid`, `name_cache`, and for every thread: `state`, `on_runq`, `on_cpu`, `on_sleepq`, `wake_time`, `bound_cpu`, `detached`, and `cpu->cur_thread`. The `interruptible` flag is written without the lock in some paths (`wait_event_interruptible`, after `schedule()` returns in `sleep_ms`), but every reader that matters (`wake_interruptible`) runs under it.

Fields written without any lock (by their own thread only) and read racily by debug code: `wait_what`, `wait_obj`, `blocked_on`, `blocked_since`, `hung_reported`, `held_mutex[]`, `runtime`. `hung_thread_check` explicitly tolerates `blocked_since` being newer than its `now`.

### Interrupt-context rules

- Interrupt handlers may: take spinlocks, call `wake_up` / `wake_up_one` / `wake_thread`, call `sem_up`, and send IPIs.
- They may not: call `mutex_lock`, `sem_down`, `wait_event` or `sleep_ms`.
- Mutexes held by the interrupted thread are not counted by lockdep in handlers (`irq_depth > 0`).
- Preemption happens only at the outermost interrupt exit, and only if the interrupted context had interrupts enabled.
- PLIC device interrupts all go to cpu0. Timer and software interrupts are per CPU.

### Memory ordering notes

- Spinlocks use an acquire exchange and a release store. The lock/unlock pair is the barrier for everything in this subsystem.
- IPI bits are set with SEQ_CST `or`, and taken with an ACQ_REL `exchange` / `fetch_and`.
- `exit_done` uses release/acquire, and so do `online`, `stopped`, `stop_owner` and `tlb_done_gen`.
- `tlb_flush_ack` must load `tlb_gen` *before* `sfence.vma`. Do not reorder these.

## Interfaces

### Thread API (`sched.h`)

| Function | Notes |
|---|---|
| `kthread_create(name, fn, arg)` | Runnable at once; returns NULL on out-of-memory |
| `kthread_create_suspended(name, fn, arg)` | `T_SLEEPING`, `wait_what = "start"`, until `kthread_start` |
| `kthread_bind(t, cpu)` | Before start; an out-of-range CPU becomes unbound |
| `kthread_start(t)` | |
| `kthread_join(t)` | Returns the exit code, frees `t`; panics on self or detached |
| `kthread_detach(t)` | Freed automatically after exit |
| `thread_exit(code)` | `__noreturn`; panics on the idle thread |
| `thread_find(tid)` | Caller holds `sched_lock` or the world is stopped |
| `thread_name(tid)` | Also knows the last 64 freed threads |
| `thread_user_tf(t)` | The user trap frame at the top of the stack |
| `sched_create_idle(cpu)`, `cpu_idle_loop()`, `sched_init()`, `cpu_early_init()` | Boot only |

### Scheduling and sleeping

`schedule()`, `preempt_schedule()`, `yield()`, `sleep_ms(ms)`, `set_current_sleeping(what)`, `set_current_running()`, `schedule_until(deadline)`, `ms_to_rdtime(ms)`, `wake_thread(t)`, `wake_interruptible(t)`, `sched_tick()`, `can_sleep()`.

### Synchronization

- Wait queues: `waitqueue_init`, `WAITQUEUE_INIT`, `prepare_to_wait`, `finish_wait`, `wake_up`, `wake_up_one`, `add_wait_queue`, `remove_wait_queue`, `wait_event`, `wait_event_interruptible`.
- Mutexes: `mutex_init`, `MUTEX_INIT`, `mutex_lock`, `mutex_trylock`, `mutex_unlock`.
- Semaphores: `sem_init`, `sem_down`, `sem_trydown`, `sem_up`.
- Spinlocks: `spin_init`, `SPINLOCK_INIT`, `spin_lock_irqsave`, `spin_trylock_irqsave`, `spin_unlock_irqrestore`, `spin_held_by_me`, `spin_is_locked`, `spin_lock_debug`, `spin_unlock_debug`, `spin_print_held`, `spin_print_held_cpu`.

### SMP (`smp.h`)

`smp_init`, `smp_send_ipi`, `ipi_handle`, `ipi_poll`, `tlb_shootdown`, `smp_stop_others`, `smp_resume_others`, `smp_park_self`, `smp_stopped_by_me`, `smp_world_stopped`, `cpus_print`, `cpu_backtrace`, `timer_init_cpu`, `watchdog_check`. Globals: `ncpus_online`, `cpus[]`, `tlb_shootdowns`, `ipi_names[]`.

### Debug hooks

`hung_thread_check()`, `hung_set_threshold_ms(ms)` (returns the old value; for tests), `lockdep_init`, `lockdep_acquire`, `lockdep_might_sleep`, `lockdep_print`, `lockdep_enabled`, `lockdep_reports`, `lockdep_testing`.

### Boot options

| Option | Default | Effect |
|---|---|---|
| `smp=<n>` | all harts (max 8) | Use at most n CPUs; `smp=1` is uniprocessor |
| `watchdog=<s>` | 10 | Hard-lockup threshold; 0 = off |
| `hung=<s>` | 30 | Hung mutex-wait threshold; 0 = off |
| `lockdep=off\|warn\|panic` | warn | `make test` uses panic |
| `panic=shutdown` | — | Exit QEMU on panic instead of entering the monitor |

### /proc files

`/proc/threads` (`thread_list_print`), `/proc/sched` (`sched_print`), `/proc/cpus` (`cpus_print`), `/proc/lockdep` (`lockdep_print`), `/proc/interrupts` (`irq_print`, includes IPI counts).

### Monitor commands

| Command | What it shows |
|---|---|
| `ps` | Every thread: TID, name, state, CPU (`*` = running now), CPU-ms, switches, migrations, stack high-water, wait reason and "where" (first non-scheduler frames, or "preempted in ...") |
| `sched` | Per-CPU current thread, switches, migrations in, `need_resched`, idle %; the run queue; the sleep queue with time left; `jiffies` |
| `cpus` | Per-CPU hart, state (this-cpu / stopped / no-stop! / idle / running), thread, idle %, switches, migrations, ticks, IPIs in, stop location, held spinlocks; IPI sent/received totals; TLB shootdowns |
| `cpu <n>` | Switch the view to CPU n: `regs` / `bt` / `$reg` then refer to where it parked |
| `bt [tid]` | Backtrace of a thread (saved context, or the parked CPU's location if it is running) |
| `locks` | Held spinlocks per CPU, held mutexes per thread, threads blocked on mutexes |
| `lockdep` | Classes and the dependency graph |

## Debugging and observability

### Trace events (`kernel/include/trace_events.h`)

| Event | Group | Arguments |
|---|---|---|
| `sched_switch` | SCHED | prev tid, next tid, prev state |
| `sched_wakeup` | SCHED | tid, waker tid |
| `thread_create` | SCHED | tid, name (16 bytes) |
| `thread_exit` | SCHED | tid, code |
| `mutex_contended` | LOCK | mutex name, owner tid, pc |
| `lock_inversion` | LOCK | the two class names, pc |
| `ipi_send` | IRQ | target CPU, kind |
| `ipi_recv` | IRQ | kind |
| `cpu_stop` | IRQ | pc where the CPU parked |

Each CPU records into its own ring, and the Perfetto export has one track per CPU. Use `trace groups sched,lock,irq` in the monitor to focus on scheduling.

### GDB (`tools/gdb/eunuchs.py`)

- `eu-ps`: threads, state, CPU and wait reason. Works on a halted or wedged kernel.
- `eu-cpus`: each CPU's GDB thread, current thread, `irq_depth`, `stopped`, `ipi_pending`, held spinlocks. Use `thread <n>` then `bt` to see a CPU.
- `eu-bt TID`: backtrace from a sleeping thread's saved context.
- `eu-lockdep`: the dependency graph.
- `eu-trace`, `eu-trace-dump`: recent events, or a Perfetto file.

### Failure messages and what they mean

| Message | Meaning / first step |
|---|---|
| `schedule() while holding spinlock 'x' (taken at S)` | A sleeping call (or `yield`) inside a spinlock section. Fix the caller at S. |
| `thread N (name) tried to sleep in interrupt context` | A handler called something that blocks |
| `mutex_lock('m') in interrupt context` | Same, for a mutex |
| `recursive mutex_lock('m') ...: locked at A, again at B` | Self-deadlock |
| `mutex_unlock('m') at S by thread N, but the owner is ...` | Unbalanced or foreign unlock |
| `MUTEX DEADLOCK detected:` + chain, then `mutex deadlock (cycle above)` | A real cycle of mutex waiters |
| `recursive spinlock: 'x' already held by this CPU` | Self-deadlock on a spinlock |
| `unlock of spinlock 'x' not held by this CPU` | Double unlock, or unlock on the wrong CPU (it migrated, so interrupts must have been re-enabled) |
| `deadlock: spinlock 'x' held by cpu N at S never released` | `SPIN_TIMEOUT` reached; the holder at S is stuck or forgot to unlock |
| `LOCKDEP: possible deadlock: lock order inversion` | Two code paths take two lock classes in opposite orders; the report lists every link's sites |
| `LOCKDEP: mutex_lock at X while holding spinlock 'y'` | A sleeping lock under a spinlock |
| `HUNG THREAD: N (name) has waited X ms for mutex 'm'` | A lost unlock, or a deadlock through something other than mutexes (for example a wait queue). Read both backtraces. |
| `watchdog: hard lockup on cpuN: no timer interrupt for X ms` | That CPU has had interrupts off for `watchdog=` seconds. The panic report shows where, if it is spinning. |
| `TLB shootdown: cpuN did not respond within 3000 ms` | That CPU had interrupts off for a long time without spinning on a lock |
| `scheduler: thread N resumed on cpuM, whose current thread is ...` | Corrupted CPU state or a migration-safety violation |
| `idle loop of cpuN is running as thread T` | Same family: `tp` or `cur_thread` is wrong |
| `dead thread N was scheduled again` | A `T_DEAD` thread was put back on the queue |
| `cpuN did NOT stop (interrupts off?)` / `did not stop: interrupts off and not spinning on a lock` | Stop-the-world could not park that CPU. Its data may change while you look at it. |

### Techniques

- **A thread hangs.** Run `ps`. Look at its wait reason. For a mutex, the holder is named; run `bt <holder>`. For a wait queue, check whether the condition is already true. If it is, a waker skipped `wake_up` or set the condition after waking (a lost wakeup).
- **A CPU is unresponsive.** Run `cpus`. "no-stop!" or "did NOT stop" means interrupts are off without spinning. Use `eu-cpus` in GDB and `thread <n>` / `bt`.
- **Lock problems.** `locks` shows who holds what. `lockdep` / `/proc/lockdep` show the order graph. The default `warn` mode stops checking after the first report, so look in `dmesg` for that first report.
- **Performance.** `sched` shows idle % and migrations per CPU. `ps` shows CPU-ms and migrations per thread. A Perfetto trace shows each CPU's timeline.
- **Tests.** `make test` runs the KTESTs in `test_sched.c` and `test_smp.c` with `-smp 4` and `lockdep=panic`. Tests that need several CPUs pass trivially on one.

## Limitations and known issues

- **One global run queue and one global `sched_lock`.** Every wakeup, every tick, every switch and every sleep-queue update on every CPU takes the same lock. This is simple and easy to inspect, but it will not scale beyond the 8 CPUs that `MAX_CPUS` allows.
- **No CPU affinity in placement.** `dequeue` takes the oldest eligible thread regardless of where it last ran, so threads migrate freely (`migrations` counts them). No priorities, no `nice`, no real-time classes. The time slice is fixed at one tick (10 ms).
- **The idle loop can busy-spin.** `cpu_idle_loop` uses `wfi` only if `runq` is completely empty. If the queue holds only threads bound to *other* CPUs, an idle CPU loops on `schedule()`, taking `sched_lock` each time, until those threads run.
- **Preempted sleepers look asleep.** A thread preempted inside `wait_event` stays `T_SLEEPING` while it is on the run queue or even running, so `ps` shows "S" for it. The idle thread shows "r" whenever it is switched out.
- **The mutex is unfair and has no handoff.** A waker's `wake_up_one` competes with new lockers, so a waiter can starve under heavy contention.
- **Hung detection covers only mutexes.** Waits on semaphores, wait queues, `join` and `poll` are never reported. Only the first hung thread found per second is reported.
- **The watchdog needs two CPUs.** With `smp=1`, or if every CPU has interrupts off, no lockup is detected. There is no soft-lockup detection: a thread spinning with interrupts on and never scheduling is preempted normally, but a kernel loop that keeps interrupts on and never sleeps is never flagged.
- **Stop-the-world is best-effort.** A CPU with interrupts off that is not spinning on a spinlock (or in another `ipi_poll` loop) cannot be parked. The monitor and panic proceed after 200 ms with that CPU still running.
- **lockdep is "lite".**
  - Classes are keyed by name, so different locks with the same name merge, and nesting two locks of one class is never checked.
  - Trylocks add no edges.
  - At most 192 classes and 1024 recorded edges. Further classes are not checked; further edges are checked but carry no site information.
  - In `warn` mode the first inversion disables all further checking.
  - Only 8 held spinlocks per CPU and 8 held mutexes per thread are visible.
- **`lockdep_might_sleep` is only called by `mutex_lock`.** `sem_down` and `wait_event` under a spinlock are caught only when they actually call `schedule()`.
- **Spin timeout is counted in iterations, not time.** `SPIN_TIMEOUT` = 1e8 loop iterations, each with an `ipi_poll`. The real time it represents depends on the host.
- **Spinlocks are plain test-and-set.** Each spin iteration does an atomic exchange, with no backoff and no fairness.
- **The hung report holds `sched_lock` while printing.** `hung_thread_check` prints two backtraces to the polled console with `sched_lock` held and interrupts off on cpu0, which stalls scheduling on every CPU for the duration.
- **A failed hart start leaks.** If `start_cpu` fails after `sched_create_idle` succeeded, the idle thread stays in `all_threads` as a `T_RUNNING` "idle/N" that never runs, and the overflow stack is not freed.
- **A thread that is never joined or detached stays forever** as `T_DEAD`, together with its 16 KiB stack.
- **Nested waits clobber multi-source sleeps.** If code called after `set_current_sleeping` blocks on a mutex or wait queue, `finish_wait` sets `T_RUNNING` and clears `wait_what`. The following `schedule_until` then returns immediately. This is safe (a spurious wakeup) but costs one extra loop.
- **Racy statistics.** `tlb_shootdowns++` and `spinlock.contentions++` are non-atomic updates from several CPUs. These counters are approximate.
- **The tick runs on idle CPUs.** Every CPU takes 100 timer interrupts per second even when idle (no tickless idle).

## Potential improvements

1. **Make the idle loop check for eligible work** — `cpu_idle_loop` checks `list_empty(&runq)`, so a queue holding only threads bound to other CPUs makes idle CPUs spin through `schedule()` and hammer `sched_lock`. Keep a count of runnable unbound threads (or per-CPU bound counts) and `wfi` when nothing is eligible for this CPU. Impact: medium. Effort: small.
2. **Test-and-test-and-set or ticket spinlocks** — `spin_lock_irqsave` does an `amoswap` on every iteration. Spinning on a plain load first cuts coherence traffic; a ticket lock adds FIFO fairness. Keep `ipi_poll` in the loop. Impact: medium. Effort: small.
3. **Soft-lockup detector** — `last_switch` is already tracked per CPU. In `sched_tick`, warn when a non-idle thread has run for more than N seconds without switching while other work is queued. This catches kernel loops that keep interrupts on, which the hard-lockup watchdog cannot see (and it works with `smp=1`). Impact: medium. Effort: small.
4. **Extend the hung-thread detector beyond mutexes** — record `blocked_since` in `prepare_to_wait` and `sem_down` as well, and report long uninterruptible waits on wait queues and semaphores (excluding expected long waits such as `join` or idle daemons via a flag). Many real hangs are lost wakeups on wait queues. Impact: high. Effort: medium.
5. **Check held locks at thread exit** — in `thread_exit`, panic or warn if `nheld_mutex > 0`, naming each mutex and its `owner_pc`. Today the leak only surfaces later as a hung waiter. Impact: medium. Effort: small.
6. **Fix the `start_cpu` failure path** — on `sbi_hart_start` failure or timeout, remove the idle thread from `all_threads`, free it and the overflow stack, and leave `cpus[id]` clean. Today a phantom "idle/N" appears in `ps`. Impact: low. Effort: small.
7. **Cache-affine placement** — let `dequeue` prefer a thread whose `cpu` equals this CPU (for example, scan the first few entries), or add per-CPU queues with stealing. This reduces the migrations that `sched` reports. It would require revisiting the single-lock design. Impact: medium. Effort: large.
8. **Split `sched_lock`** — the sleep queue and `all_threads`/`name_cache` don't need the run-queue lock. Separate locks (with a documented order) would shorten `sched_tick` and `ps` hold times. Impact: medium. Effort: medium.
9. **Mutex handoff or FIFO fairness** — when waiters exist, `mutex_unlock` could hand ownership directly to the first waiter, which prevents starvation under barging. Keep `owner_pc` and lockdep semantics intact. Impact: low. Effort: medium.
10. **`wait_event_timeout`** — there is no generic timed wait on a wait queue. `poll` and `netd` hand-roll it with `set_current_sleeping` / `schedule_until`. A macro built on `sleepq_insert` would remove that duplication. Impact: medium. Effort: small.
11. **Move printing out of `sched_lock` in `hung_thread_check`** — copy the waiter's and owner's `ctx.ra`/`ctx.s[0]` (with a reference so they can't be freed) and print after unlocking, so a report doesn't stall scheduling on every CPU. Impact: low. Effort: medium.
12. **Better lockdep classes** — key classes by the lock's initialization site, or allow an explicit class key, instead of by name. This avoids false merges (a mutex's internal spinlock and its wait-queue lock already share a class). Optionally keep checking after the first report in `warn` mode, de-duplicating per edge. Impact: medium. Effort: medium.
13. **Time-based spin timeout** — replace the iteration count `SPIN_TIMEOUT` with an `rdtime` deadline, checked every few thousand iterations. Then the "deadlock: spinlock never released" threshold means the same thing on every host. Impact: low. Effort: small.
14. **Show preempted sleepers distinctly** — add a flag set by the `preempt && T_SLEEPING` branch of `__schedule`, and display it in `ps`/`sched`, so a thread that is actually runnable is not mistaken for a sleeper. Impact: low. Effort: small.
15. **Validate `kthread_bind`** — warn when `cpu >= ncpus_online` instead of silently unbinding, and refuse binding a thread that is already running or queued. Impact: low. Effort: small.
