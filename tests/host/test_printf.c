/*
 * Host-native test of lib/printf.c: compare ksnprintf against the host libc's
 * snprintf for a wide range of formats. Built with ASan/UBSan.
 */
#include <fmt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(...)                                                          \
    do {                                                                    \
        char want[256], got[256];                                           \
        int wn = snprintf(want, sizeof(want), __VA_ARGS__);                 \
        int gn = ksnprintf(got, sizeof(got), __VA_ARGS__);                  \
        checks++;                                                           \
        if (wn != gn || strcmp(want, got)) {                                \
            failures++;                                                     \
            printf("FAIL %s:%d  fmt %s\n  want [%s] (%d)\n  got  [%s] (%d)\n", \
                   __FILE__, __LINE__, #__VA_ARGS__, want, wn, got, gn);    \
        }                                                                   \
    } while (0)

int main(void)
{
    int ints[] = { 0, 1, -1, 42, -42, 1000000, INT_MAX, INT_MIN };
    const char *ifmts[] = {
        "%d", "%i", "%5d", "%-5d|", "%05d", "%+d", "% d", "%.3d", "%8.3d",
        "%-8.3d|", "%+05d", "%.0d", "%x", "%X", "%#x", "%#X", "%08x", "%#08x",
        "%o", "%#o", "%u", "%#.0o", "%#.0x", "%hhd", "%hd", "%hhx",
    };
    for (size_t f = 0; f < sizeof(ifmts) / sizeof(ifmts[0]); f++)
        for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++)
            CHECK(ifmts[f], ints[i]);

    long longs[] = { 0, LONG_MAX, LONG_MIN, -123456789012L };
    for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
        CHECK("%ld", longs[i]);
        CHECK("%lx", (unsigned long)longs[i]);
        CHECK("%lu", (unsigned long)longs[i]);
        CHECK("%#020lx", (unsigned long)longs[i]);
        CHECK("%lld", (long long)longs[i]);
        CHECK("%llu", (unsigned long long)longs[i]);
        CHECK("%zu", (size_t)longs[i]);
        CHECK("%zx", (size_t)longs[i]);
        CHECK("%jd", (intmax_t)longs[i]);
        CHECK("%td", (ptrdiff_t)longs[i]);
    }

    const char *strs[] = { "", "a", "hello", "a longer string here" };
    const char *sfmts[] = { "%s", "%10s", "%-10s|", "%.3s", "%10.3s", "%-10.3s|", "%.0s" };
    for (size_t f = 0; f < sizeof(sfmts) / sizeof(sfmts[0]); f++)
        for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
            CHECK(sfmts[f], strs[i]);

    CHECK("%c%c%c", 'a', 'b', 'c');
    CHECK("%5c|%-5c|", 'x', 'y');
    CHECK("100%%");
    CHECK("%*d|%-*d|%.*s", 6, 42, 6, 42, 2, "abcdef");
    CHECK("%*d", -6, 42);
    CHECK("%p", (void *)0x80200000UL);
    CHECK("mixed %s=%d (%#x) [%-6s]", "val", 255, 255, "ok");

    /* Truncation semantics. */
    char small[5];
    int n = ksnprintf(small, sizeof(small), "%s", "truncate me");
    checks++;
    if (n != 11 || strcmp(small, "trun")) {
        failures++;
        printf("FAIL truncation: n=%d buf=[%s]\n", n, small);
    }

    printf("printf: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
