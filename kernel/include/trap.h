#ifndef EUNUCHS_TRAP_H
#define EUNUCHS_TRAP_H

#include <types.h>

/* Layout must match trap.S. */
struct trap_frame {
    u64 regs[32];   /* x0..x31; regs[2] is the interrupted sp */
    u64 sepc;
    u64 sstatus;
    u64 scause;
    u64 stval;
};

/* Bytes a trap frame occupies on the stack (frame + fake frame record). */
#define TRAP_FRAME_SIZE 304

#define TF_RA(tf) ((tf)->regs[1])
#define TF_SP(tf) ((tf)->regs[2])
#define TF_FP(tf) ((tf)->regs[8])

extern const char *const reg_names[32];
extern u64 exception_counts[];
extern u64 interrupt_counts[];

void trap_init(void);
const char *trap_cause_name(u64 scause);
void trap_dump(const struct trap_frame *tf, int (*print)(const char *fmt, ...));
void trap_print_counts(int (*print)(const char *fmt, ...));

/*
 * Optional hook for `ebreak` in the kernel. Return true if handled; execution
 * then resumes after the breakpoint. Otherwise the debug monitor is entered.
 */
extern bool (*trap_breakpoint_hook)(struct trap_frame *tf);

/* Deliberately enter the debug monitor from code: `debug_break();` */
static inline void debug_break(void) { asm volatile("ebreak" ::: "memory"); }

#endif
