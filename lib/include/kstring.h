#ifndef EUNUCHS_KSTRING_H
#define EUNUCHS_KSTRING_H

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
/* First byte in [s, s+n) that is not c, or NULL if all bytes equal c. */
void *memchr_inv(const void *s, int c, size_t n);
char *strstr(const char *haystack, const char *needle);
size_t strlcpy(char *dst, const char *src, size_t size);

/* Parse an unsigned integer; base 0 auto-detects 0x / 0 prefixes. */
unsigned long kstrtoul(const char *s, const char **end, int base);

static inline int kisspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int kisdigit(int c) { return c >= '0' && c <= '9'; }
static inline int kisprint(int c) { return c >= 0x20 && c < 0x7f; }

#endif
