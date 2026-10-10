#ifndef EUNUCHS_TRACE_H
#define EUNUCHS_TRACE_H

/*
 * Static tracepoints: a flight recorder of kernel events.
 *
 * Each event is a fixed 40-byte record in a per-CPU ring (lock-free: one
 * writer per CPU, interrupts off while writing). Recording is always compiled
 * in and cheap; groups are enabled at runtime (boot: `trace=sched,irq,mm` or
 * `trace=all|none`; monitor: `trace groups ...`). Default: everything but mm.
 *
 * Reading: `trace show [n]` in the monitor, the last events in every panic,
 * `trace dump` (writes build/trace.bin to the host via semihosting), and
 * eu-trace / eu-trace-dump in GDB. tools/trace/eutrace.py converts a dump to
 * Perfetto/Chrome JSON (open in https://ui.perfetto.dev).
 */

#include <trace_events.h>
#include <types.h>

enum trace_group {
    TG_SCHED = 1 << 0,
    TG_IRQ   = 1 << 1,
    TG_LOCK  = 1 << 2,
    TG_MM    = 1 << 3,
    TG_MARK  = 1 << 4,
    TG_SYSCALL = 1 << 5,
    TG_PROC  = 1 << 6,
    TG_BLK   = 1 << 7,
    TG_FS    = 1 << 8,
    TG_NET   = 1 << 9,
    TG_ALL   = 0x3ff,
};

enum trace_event {
#define X(name, group, a0, a1, a2) TE_##name,
    TRACE_EVENTS(X)
#undef X
    TE_MAX,
};

struct trace_rec {
    u64 ts;         /* rdtime */
    u16 type;
    u16 cpu;
    u32 tid;
    u64 a[3];
};

#define TRACE_RECS 8192         /* per CPU */

struct trace_buf {
    u64 head;                   /* total records ever written */
    struct trace_rec recs[TRACE_RECS];
};

extern u32 trace_groups_enabled;
extern const u32 trace_event_group[TE_MAX];

void trace_record(enum trace_event ev, u64 a0, u64 a1, u64 a2);

#define trace(ev, a0, a1, a2)                                               \
    do {                                                                    \
        if (trace_groups_enabled & trace_event_group[TE_##ev])              \
            trace_record(TE_##ev, (u64)(a0), (u64)(a1), (u64)(a2));         \
    } while (0)

/* Free-form marker; msg must be a string literal (stored by pointer). */
#define trace_mark(msg, a, b) trace(mark, (msg), (a), (b))

void trace_init(void);
void trace_print(int (*print)(const char *fmt, ...), int last_n);
void trace_status(int (*print)(const char *fmt, ...));
void trace_clear(void);
int trace_set_groups(const char *spec);     /* "sched,irq" / "all" / "none" */
int trace_dump_file(const char *path);      /* via semihosting; bytes written or -1 */

#endif
