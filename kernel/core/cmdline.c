#define LOG_SUBSYS SUBSYS_BOOT
#include <debug.h>
#include <kstring.h>
#include <fmt.h>
#include <printk.h>

/*
 * Kernel command line: space-separated `key` or `key=value` words.
 * Stored tokenized: each word NUL-terminated in cmdline_buf.
 *
 * Every option the kernel understands is listed in `options` below: boot
 * warns about anything else (usually a typo that would otherwise be
 * silently ignored) and about values outside an option's choices, and the
 * option `help` prints this table. Keep it in sync with docs/DEBUGGING.md.
 */

#define MAX_ARGS 32

static char raw[256];
static char cmdline_buf[256];
static char *words[MAX_ARGS];
static int nwords;

void cmdline_init(const char *bootargs)
{
    strlcpy(raw, bootargs ? bootargs : "", sizeof(raw));
    strlcpy(cmdline_buf, raw, sizeof(cmdline_buf));
    nwords = 0;

    char *p = cmdline_buf;
    while (*p && nwords < MAX_ARGS) {
        while (kisspace(*p))
            *p++ = '\0';
        if (!*p)
            break;
        words[nwords++] = p;
        while (*p && !kisspace(*p))
            p++;
    }
}

const char *cmdline_get(const char *key)
{
    size_t klen = strlen(key);
    for (int i = 0; i < nwords; i++) {
        if (strncmp(words[i], key, klen))
            continue;
        if (words[i][klen] == '\0')
            return "";
        if (words[i][klen] == '=')
            return words[i] + klen + 1;
    }
    return NULL;
}

bool cmdline_has(const char *key)
{
    return cmdline_get(key) != NULL;
}

const char *cmdline_raw(void)
{
    return raw;
}

static const struct {
    const char *key;
    const char *arg;
    const char *choices;        /* '|'-separated allowed values, or NULL */
    const char *help;
} options[] = {
    { "help", "", NULL, "list the boot options" },
    { "log", "=subsys:level,...", NULL, "log thresholds, e.g. log=all:debug,net:trace" },
    { "quiet", "", NULL, "console shows only warnings and errors" },
    { "break", "", NULL, "enter the debug monitor early in boot" },
    { "panic", "=shutdown", "shutdown", "exit QEMU on panic instead of entering the monitor" },
    { "init", "=path", NULL, "program to run as pid 1 (default /bin/init)" },
    { "ktest", "[=prefix]", NULL, "run the kernel unit tests, then power off" },
    { "utest", "[=mode]", NULL, "run the user tests (or one scenario), then power off" },
    { "tracedump", "=host-path", NULL, "after ktest/utest, write the trace to the Mac" },
    { "trace", "=groups", NULL, "trace groups: sched,irq,lock,mm,mark,syscall,proc,blk,fs,net" },
    { "profile", "[=hz]", NULL, "start the sampling profiler at boot (default 500 Hz)" },
    { "nopoison", "", NULL, "don't poison freed pages (faster; hides use-after-free)" },
    { "smp", "=n", NULL, "use at most n CPUs" },
    { "lockdep", "=off|warn|panic", "off|warn|panic", "lock-order checking" },
    { "watchdog", "=seconds", NULL, "hard-lockup watchdog (0 = off)" },
    { "hung", "=seconds", NULL, "report mutex waits longer than this (0 = off)" },
    { "fs.crash", "=point", "after-log|after-commit|mid-install",
      "simulate a power failure in the next commit" },
    { "ip", "=a.b.c.d/bits[,gw]", NULL, "eth0 address" },
    { "net.drop", "=percent", NULL, "drop received packets at random" },
};

static bool in_choices(const char *v, const char *choices)
{
    size_t n = strlen(v);
    for (const char *c = choices; *c;) {
        const char *bar = strchr(c, '|');
        size_t len = bar ? (size_t)(bar - c) : strlen(c);
        if (len == n && !strncmp(c, v, n))
            return true;
        c += len + (bar ? 1 : 0);
    }
    return false;
}

/* Warn about unknown options and bad values; `help` prints the table. */
void cmdline_check(void)
{
    for (int i = 0; i < nwords; i++) {
        char key[32];
        const char *eq = strchr(words[i], '=');
        size_t klen = eq ? (size_t)(eq - words[i]) : strlen(words[i]);
        strlcpy(key, words[i], MIN(klen + 1, sizeof(key)));
        size_t k;
        for (k = 0; k < ARRAY_SIZE(options); k++)
            if (!strcmp(key, options[k].key))
                break;
        if (k == ARRAY_SIZE(options))
            pr_warn("unknown boot option '%s' ignored (boot with 'help' for the list)\n", key);
        else if (options[k].choices && eq && !in_choices(eq + 1, options[k].choices))
            pr_warn("boot option %s: '%s' is not one of %s\n", key, eq + 1, options[k].choices);
    }
    if (cmdline_has("help")) {
        pr_info("boot options (make run APPEND=\"...\"):\n");
        for (size_t k = 0; k < ARRAY_SIZE(options); k++) {
            char opt[48];
            ksnprintf(opt, sizeof(opt), "%s%s", options[k].key, options[k].arg);
            pr_info("  %-30s %s\n", opt, options[k].help);
        }
    }
}
