#include <fmt.h>
#include <kstring.h>
#include <ktest.h>

KTEST(printf_basic)
{
    char buf[64];
    ksnprintf(buf, sizeof(buf), "%d %u %x %s %c", -42, 42u, 0xbeef, "hi", 'z');
    KT_EXPECT_STREQ(buf, "-42 42 beef hi z");
    ksnprintf(buf, sizeof(buf), "[%5d|%-5d|%05d]", 7, 7, -7);
    KT_EXPECT_STREQ(buf, "[    7|7    |-0007]");
    ksnprintf(buf, sizeof(buf), "%#lx %p %lu", 0x1234UL, (void *)0x80200000UL, ~0UL);
    KT_EXPECT_STREQ(buf, "0x1234 0x80200000 18446744073709551615");
    ksnprintf(buf, sizeof(buf), "%.3s|%*s|%-*s|", "abcdef", 4, "x", 3, "y");
    KT_EXPECT_STREQ(buf, "abc|   x|y  |");
}

KTEST(printf_truncation)
{
    char buf[8];
    int n = ksnprintf(buf, sizeof(buf), "%s", "0123456789");
    KT_EXPECT_EQ(n, 10);
    KT_EXPECT_STREQ(buf, "0123456");
}

KTEST(string_funcs)
{
    char buf[16];
    KT_EXPECT_EQ(strlen("hello"), 5);
    KT_EXPECT(strcmp("abc", "abd") < 0);
    KT_EXPECT_EQ(strncmp("abcX", "abcY", 3), 0);
    KT_EXPECT_STREQ(strchr("a=b", '='), "=b");
    KT_EXPECT_STREQ(strstr("kernel panic", "pan"), "panic");
    KT_EXPECT_EQ(strlcpy(buf, "overflowing string", 8), 18);
    KT_EXPECT_STREQ(buf, "overflo");

    memcpy(buf, "abcdef", 7);
    memmove(buf + 1, buf, 5);
    KT_EXPECT_STREQ(buf, "aabcde");    /* overlapping copy; NUL at [6] kept */
}

KTEST(strtoul)
{
    const char *end;
    KT_EXPECT_EQ(kstrtoul("0x1f", &end, 0), 0x1f);
    KT_EXPECT_EQ(*end, '\0');
    KT_EXPECT_EQ(kstrtoul("42abc", &end, 10), 42);
    KT_EXPECT_STREQ(end, "abc");
    KT_EXPECT_EQ(kstrtoul("0755", NULL, 0), 0755);
    KT_EXPECT_EQ(kstrtoul("ff", NULL, 16), 255);
}
