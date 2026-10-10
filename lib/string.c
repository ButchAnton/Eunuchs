#include <kstring.h>

#define WORD sizeof(unsigned long)
#define ALIGNED(p) (((unsigned long)(p) & (WORD - 1)) == 0)

static inline unsigned long repeat_byte(int c)
{
    return (unsigned long)(unsigned char)c * (~0UL / 0xff);
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n && !ALIGNED(d)) {
        *d++ = (unsigned char)c;
        n--;
    }
    unsigned long w = repeat_byte(c);
    for (; n >= WORD; n -= WORD, d += WORD)
        *(unsigned long *)d = w;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memchr_inv(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n && !ALIGNED(p)) {
        if (*p != (unsigned char)c)
            return (void *)p;
        p++, n--;
    }
    unsigned long w = repeat_byte(c);
    for (; n >= WORD; n -= WORD, p += WORD)
        if (*(const unsigned long *)p != w)
            break;
    for (; n; n--, p++)
        if (*p != (unsigned char)c)
            return (void *)p;
    return NULL;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (ALIGNED(d) && ALIGNED(s)) {
        for (; n >= WORD; n -= WORD, d += WORD, s += WORD)
            *(unsigned long *)d = *(const unsigned long *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncmp(haystack, needle, n))
            return (char *)haystack;
    return n ? NULL : (char *)haystack;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

unsigned long kstrtoul(const char *s, const char **end, int base)
{
    while (kisspace(*s))
        s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0' && s[1]) {
            base = 8;
            s++;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base)
            break;
        v = v * base + d;
    }
    if (end)
        *end = s;
    return v;
}
