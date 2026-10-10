#ifndef EUNUCHS_KTEST_H
#define EUNUCHS_KTEST_H

/*
 * In-kernel unit tests.
 *
 *   KTEST(my_feature) {
 *       KT_EXPECT(x == 1);          // records failure, continues
 *       KT_EXPECT_EQ(a, b);
 *       KT_ASSERT(p != NULL);       // records failure, returns
 *   }
 *
 * Run all: boot with `-append ktest` (`make test`); run a subset: `ktest=prefix`,
 * or `ktest [prefix]` from the debug monitor.
 */

#include <types.h>

struct ktest_ctx {
    const char *name;
    int failures;
};

struct ktest {
    const char *name;
    const char *file;
    void (*fn)(struct ktest_ctx *);
};

void ktest_fail(struct ktest_ctx *kt, const char *file, int line, const char *fmt, ...)
    __printf_like(4, 5);
int ktest_run(const char *filter);   /* returns number of failing tests */

#define KTEST(tname)                                                         \
    static void ktest_fn_##tname(struct ktest_ctx *__kt);                    \
    static const struct ktest ktest_desc_##tname                             \
        __used __section(".ktests") __aligned(8) = {                         \
            #tname, __FILE__, ktest_fn_##tname };                            \
    static void ktest_fn_##tname(struct ktest_ctx *__kt __unused)

#define KT_EXPECT(cond)                                                      \
    do {                                                                     \
        if (!(cond))                                                         \
            ktest_fail(__kt, __FILE__, __LINE__, "expected: %s", #cond);     \
    } while (0)

#define KT_ASSERT(cond)                                                      \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ktest_fail(__kt, __FILE__, __LINE__, "assert: %s", #cond);       \
            return;                                                          \
        }                                                                    \
    } while (0)

#define KT_EXPECT_EQ(a, b)                                                   \
    do {                                                                     \
        unsigned long long __a = (unsigned long long)(a);                    \
        unsigned long long __b = (unsigned long long)(b);                    \
        if (__a != __b)                                                      \
            ktest_fail(__kt, __FILE__, __LINE__, "%s == %s (0x%llx != 0x%llx)", \
                       #a, #b, __a, __b);                                    \
    } while (0)

#define KT_EXPECT_STREQ(a, b)                                                \
    do {                                                                     \
        const char *__a = (a), *__b = (b);                                   \
        if (!__a || !__b || strcmp(__a, __b))                                \
            ktest_fail(__kt, __FILE__, __LINE__, "%s == %s (\"%s\" != \"%s\")", \
                       #a, #b, __a ? __a : "(null)", __b ? __b : "(null)");  \
    } while (0)

#endif
