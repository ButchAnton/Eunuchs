/*
 * Trace event definitions. Also parsed by tools/trace/eutrace.py and
 * tools/gdb/eunuchs.py, so keep one event per line in this exact form:
 *
 *     X(name, GROUP, "arg0", "arg1", "arg2")
 *
 * Argument names drive decoding: "*tid" -> thread, "*state" -> thread state,
 * "*pc"/"*site" -> kernel symbol, "name16" -> 16-byte string in args 1..2,
 * "text24" -> 24-byte string in args 0..2, "nr" -> system call name,
 * "str" -> pointer to a string in the kernel image, "" -> unused.
 */
#define TRACE_EVENTS(X)                                                    \
    X(sched_switch,    SCHED, "prev_tid", "next_tid", "prev_state")        \
    X(sched_wakeup,    SCHED, "tid", "waker_tid", "")                      \
    X(thread_create,   SCHED, "tid", "name16", "")                         \
    X(thread_exit,     SCHED, "tid", "code", "")                           \
    X(irq_entry,       IRQ,   "cause", "", "")                             \
    X(irq_exit,        IRQ,   "cause", "irq", "")                          \
    X(mutex_contended, LOCK,  "str", "owner_tid", "pc")                    \
    X(page_alloc,      MM,    "pa", "order", "site")                       \
    X(page_free,       MM,    "pa", "order", "site")                       \
    X(kmalloc,         MM,    "ptr", "size", "site")                       \
    X(kfree,           MM,    "ptr", "", "site")                           \
    X(mark,            MARK,  "str", "a", "b")                            \
    X(umark,           MARK,  "text24", "", "")                           \
    X(syscall_enter,   SYSCALL, "nr", "arg0", "arg1")                     \
    X(syscall_exit,    SYSCALL, "nr", "ret", "")                          \
    X(proc_exec,       PROC,  "pid", "name16", "")                        \
    X(proc_exit,       PROC,  "pid", "status", "")                        \
    X(user_fault,      PROC,  "addr", "access", "result")                 \
    X(blk_submit,      BLK,   "sector", "len", "write")                    \
    X(blk_done,        BLK,   "sector", "status", "us")                    \
    X(file_open,       FS,    "text24", "", "")                            \
    X(log_commit,      FS,    "seq", "blocks", "us")                       \
    X(signal_send,     PROC,  "pid", "sig", "from_pid")                    \
    X(signal_deliver,  PROC,  "sig", "handler", "")                        \
    X(proc_stop,       PROC,  "pid", "sig", "")                            \
    X(tty_signal,      PROC,  "sig", "pgrp", "")                           \
    X(ipi_send,        IRQ,   "cpu", "ipi", "")                            \
    X(ipi_recv,        IRQ,   "ipi", "", "")                               \
    X(cpu_stop,        IRQ,   "pc", "", "")                                \
    X(lock_inversion,  LOCK,  "str", "str", "pc")                          \
    X(net_rx,          NET,   "proto", "len", "src_ip")                    \
    X(net_tx,          NET,   "proto", "len", "dst_ip")                    \
    X(net_drop,        NET,   "str", "len", "")                            \
    X(tcp_state,       NET,   "conn", "tcp_from", "tcp_to")                \
    X(tcp_rexmit,      NET,   "conn", "seq", "len")
