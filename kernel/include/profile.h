#ifndef EUNUCHS_PROFILE_H
#define EUNUCHS_PROFILE_H

/* Sampling profiler: see kernel/debug/profile.c. */

#include <types.h>

struct trap_frame;

#define PROF_IDLE    1          /* the CPU was idle */
#define PROF_USER    2          /* interrupted in user mode */
#define PROF_IN_IRQ  4          /* interrupted another interrupt handler */

extern volatile bool profiling;
extern u32 profile_hz;

int profile_start(u32 hz);              /* 0: default (500 Hz) */
void profile_stop(void);
void profile_clear(void);               /* stop and free the buffers */
void profile_sample(struct trap_frame *tf);     /* timer interrupt */
void profile_report(int (*print)(const char *fmt, ...));
int profile_dump_file(const char *host_path);   /* semihosting; bytes or -1 */
void profile_boot_option(void);

/* timer.c: run the per-CPU timer at hz (0: back to HZ only) */
void timer_set_profile_hz(u32 hz);

#endif
