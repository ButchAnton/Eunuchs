#define LOG_SUBSYS SUBSYS_TEST
#include <fmt.h>
#include <kstring.h>
#include <ktest.h>
#include <printk.h>

extern const struct ktest __ktests_start[], __ktests_end[];

void ktest_fail(struct ktest_ctx *kt, const char *file, int line, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    kt->failures++;
    klog(LOG_ERR, SUBSYS_TEST, "    %s:%d: %s\n", file, line, msg);
}

/*
 * Output is gtest-like and ends with a single "KTEST RESULT: PASS|FAIL" line
 * that scripts can match on.
 */
int ktest_run(const char *filter)
{
    int run = 0, failed = 0;
    size_t flen = filter ? strlen(filter) : 0;

    klog(LOG_INFO, SUBSYS_TEST, "running kernel tests%s%s\n",
         flen ? " matching " : "", flen ? filter : "");

    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (flen && strncmp(t->name, filter, flen))
            continue;
        struct ktest_ctx ctx = { .name = t->name, .failures = 0 };
        klog(LOG_INFO, SUBSYS_TEST, "[ RUN      ] %s\n", t->name);
        t->fn(&ctx);
        run++;
        if (ctx.failures) {
            failed++;
            klog(LOG_ERR, SUBSYS_TEST, "[  FAILED  ] %s (%s)\n", t->name, t->file);
        } else {
            klog(LOG_INFO, SUBSYS_TEST, "[       OK ] %s\n", t->name);
        }
    }

    klog(LOG_INFO, SUBSYS_TEST, "%d tests run, %d passed, %d failed\n", run, run - failed, failed);
    klog(failed ? LOG_ERR : LOG_INFO, SUBSYS_TEST, "KTEST RESULT: %s\n",
         failed || !run ? "FAIL" : "PASS");
    return failed || !run ? (failed ? failed : 1) : 0;
}
