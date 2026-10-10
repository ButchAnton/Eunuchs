#define LOG_SUBSYS SUBSYS_CORE
#include <console.h>
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <platform.h>
#include <percpu.h>
#include <printk.h>
#include <riscv.h>
#include <spinlock.h>

const char *const log_subsys_names[SUBSYS_MAX] = {
#define X(id, name) [SUBSYS_##id] = name,
    LOG_SUBSYSTEMS(X)
#undef X
};

const char *const log_level_names[LOG_LEVEL_MAX] = {
    "err", "warn", "info", "debug", "trace",
};

u8 log_threshold[SUBSYS_MAX] = {
#define X(id, name) [SUBSYS_##id] = LOG_INFO,
    LOG_SUBSYSTEMS(X)
#undef X
};
u8 console_level = LOG_TRACE;

/*
 * Log ring. Exported (non-static) so GDB's eu-dmesg can read it from a halted
 * or wedged kernel: bytes [max(0, head - size), head) are valid, at index
 * (pos % LOG_BUF_SIZE).
 */
char log_buf[LOG_BUF_SIZE];
volatile u64 log_head;

/* Serializes ring + console output; bypassed once panicking. */
static spinlock_t log_lock = SPINLOCK_INIT("log");

void log_ring_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        log_buf[(log_head + i) % LOG_BUF_SIZE] = s[i];
    log_head += n;
}

u64 log_ring_head(void)
{
    return log_head;
}

static char ring_at(u64 pos)
{
    return log_buf[pos % LOG_BUF_SIZE];
}

/* Copy the valid part of the ring (oldest first) into buf. */
size_t log_ring_copy(char *buf, size_t size)
{
    u64 head = log_head;
    u64 start = head > LOG_BUF_SIZE ? head - LOG_BUF_SIZE : 0;
    if (start > 0) {                    /* skip the partial oldest line */
        while (start < head && log_buf[start % LOG_BUF_SIZE] != '\n')
            start++;
        start++;
    }
    size_t n = 0;
    for (u64 p = start; p < head && n < size; p++)
        buf[n++] = log_buf[p % LOG_BUF_SIZE];
    return n;
}

void dmesg_print(int last_lines)
{
    u64 head = log_head;
    u64 start = head > LOG_BUF_SIZE ? head - LOG_BUF_SIZE : 0;

    /* After wraparound the oldest line is partial; skip it. */
    if (start > 0) {
        while (start < head && ring_at(start) != '\n')
            start++;
        start++;
    }

    if (last_lines > 0) {
        u64 pos = head;
        int lines = 0;
        while (pos > start) {
            if (ring_at(pos - 1) == '\n' && pos != head && ++lines == last_lines)
                break;
            pos--;
        }
        start = pos;
    }

    for (u64 p = start; p < head; p++)
        console_putc(ring_at(p));
}

static u64 timebase(void)
{
    return platform.timebase ? platform.timebase : 10000000;
}

void vklog(int level, int subsys, const char *fmt, va_list ap)
{
    if (level > log_threshold[subsys])
        return;

    char line[512];
    u64 t = rdtime(), tb = timebase();
    int n = ksnprintf(line, sizeof(line), "[%5lu.%06lu] %d %c %s: ",
                      (unsigned long)(t / tb),
                      (unsigned long)((t % tb) * 1000000 / tb),
                      this_cpu_id(), "EWIDT"[level], log_subsys_names[subsys]);
    n += kvsnprintf(line + n, sizeof(line) - n, fmt, ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;   /* truncated */
    if (line[n - 1] != '\n') {
        line[n++] = '\n';
        line[n] = '\0';
    }

    bool locked = panic_depth == 0;
    unsigned long flags = locked ? spin_lock_irqsave(&log_lock) : 0;
    log_ring_write(line, n);
    if (level <= console_level)
        console_write(line, n);
    if (locked)
        spin_unlock_irqrestore(&log_lock, flags);
}

int log_err_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_ERR, SUBSYS_DEBUG, fmt, ap);
    va_end(ap);
    return 0;
}

int log_warn_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_WARN, SUBSYS_DEBUG, fmt, ap);
    va_end(ap);
    return 0;
}

/* Console output from user programs: serialized with kernel log lines. */
void console_write_locked(const char *s, size_t n)
{
    bool locked = panic_depth == 0;
    unsigned long flags = locked ? spin_lock_irqsave(&log_lock) : 0;
    console_write(s, n);
    if (locked)
        spin_unlock_irqrestore(&log_lock, flags);
}

void klog(int level, int subsys, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(level, subsys, fmt, ap);
    va_end(ap);
}

static void console_out(char c, void *arg)
{
    (void)arg;
    console_putc(c);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvcbprintf(console_out, NULL, fmt, ap);
    va_end(ap);
    return n;
}

int log_subsys_lookup(const char *name)
{
    for (int i = 0; i < SUBSYS_MAX; i++)
        if (!strcmp(name, log_subsys_names[i]))
            return i;
    return -1;
}

int log_level_lookup(const char *name)
{
    if (kisdigit(name[0]) && !name[1])
        return name[0] - '0' < LOG_LEVEL_MAX ? name[0] - '0' : -1;
    for (int i = 0; i < LOG_LEVEL_MAX; i++)
        if (!strcmp(name, log_level_names[i]))
            return i;
    return -1;
}

/*
 * Boot options:
 *   log=<subsys|all>:<level>[,...]   e.g. log=all:debug,trap:trace
 *   quiet                            console shows warnings and errors only
 */
void printk_apply_cmdline(void)
{
    if (cmdline_has("quiet"))
        console_level = LOG_WARN;

    const char *spec = cmdline_get("log");
    if (!spec)
        return;

    char buf[128];
    strlcpy(buf, spec, sizeof(buf));
    for (char *item = buf; item && *item;) {
        char *next = strchr(item, ',');
        if (next)
            *next++ = '\0';
        char *colon = strchr(item, ':');
        int level = colon ? log_level_lookup(colon + 1) : -1;
        if (colon)
            *colon = '\0';
        if (level < 0) {
            pr_warn("log=: bad entry '%s'\n", item);
        } else if (!strcmp(item, "all")) {
            for (int i = 0; i < SUBSYS_MAX; i++)
                log_threshold[i] = level;
        } else {
            int s = log_subsys_lookup(item);
            if (s < 0)
                pr_warn("log=: unknown subsystem '%s'\n", item);
            else
                log_threshold[s] = level;
        }
        item = next;
    }
}
