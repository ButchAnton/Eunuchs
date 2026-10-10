#ifndef EUNUCHS_PRINTK_H
#define EUNUCHS_PRINTK_H

/*
 * Kernel logging.
 *
 * Every message carries a level and a subsystem. Each subsystem has its own
 * runtime-adjustable threshold (boot: `log=mm:debug,all:info`; monitor: `log`;
 * GDB: `eu-log`). Messages that pass are written to the log ring (`dmesg`,
 * `eu-dmesg`) and to the console if at or below `console_level`.
 *
 * Usage: each .c file that logs defines its subsystem before using pr_*():
 *     #define LOG_SUBSYS SUBSYS_MM
 *     #include <printk.h>
 */

#include <types.h>
#include <stdarg.h>

enum log_level {
    LOG_ERR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE,
    LOG_LEVEL_MAX,
};

#define LOG_SUBSYSTEMS(X) \
    X(CORE, "core")       \
    X(BOOT, "boot")       \
    X(PANIC, "panic")     \
    X(TRAP, "trap")       \
    X(MM, "mm")           \
    X(SCHED, "sched")     \
    X(PROC, "proc")       \
    X(SYSCALL, "syscall") \
    X(VFS, "vfs")         \
    X(FS, "fs")           \
    X(BLK, "blk")         \
    X(NET, "net")         \
    X(DRV, "drv")         \
    X(DEBUG, "debug")     \
    X(TEST, "test")

enum log_subsys {
#define X(id, name) SUBSYS_##id,
    LOG_SUBSYSTEMS(X)
#undef X
    SUBSYS_MAX,
};

#define LOG_BUF_SIZE (64 * 1024)

extern u8 log_threshold[SUBSYS_MAX];
extern u8 console_level;
extern const char *const log_subsys_names[SUBSYS_MAX];
extern const char *const log_level_names[LOG_LEVEL_MAX];

void klog(int level, int subsys, const char *fmt, ...) __printf_like(3, 4);
void vklog(int level, int subsys, const char *fmt, va_list ap);

/* print_fn adapters for multi-line reports (backtraces) at a log level. */
int log_err_print(const char *fmt, ...) __printf_like(1, 2);
int log_warn_print(const char *fmt, ...) __printf_like(1, 2);

/* Unlogged, direct-to-console output (monitor, raw dumps). */
int kprintf(const char *fmt, ...) __printf_like(1, 2);

/* Log ring access. */
void log_ring_write(const char *s, size_t n);
u64 log_ring_head(void);
void dmesg_print(int last_lines);
size_t log_ring_copy(char *buf, size_t size);

int log_subsys_lookup(const char *name);   /* -1 if unknown */
int log_level_lookup(const char *name);    /* accepts names or digits; -1 if unknown */
void printk_apply_cmdline(void);

#define pr_log(level, ...)                                          \
    do {                                                            \
        if ((level) <= log_threshold[LOG_SUBSYS])                   \
            klog((level), LOG_SUBSYS, __VA_ARGS__);                 \
    } while (0)

#define pr_err(...)   pr_log(LOG_ERR, __VA_ARGS__)
#define pr_warn(...)  pr_log(LOG_WARN, __VA_ARGS__)
#define pr_info(...)  pr_log(LOG_INFO, __VA_ARGS__)
#define pr_debug(...) pr_log(LOG_DEBUG, __VA_ARGS__)
#define pr_trace(...) pr_log(LOG_TRACE, __VA_ARGS__)

#endif
