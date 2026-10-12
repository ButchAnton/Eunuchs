#ifndef _STRING_H
#define _STRING_H

#include <kstring.h>    /* memset, memcpy, strlen, strcmp, ... (shared with the kernel) */

char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *stpcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strcspn(const char *s, const char *reject);
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *s, const char *accept);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
char *strsep(char **s, const char *delim);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);
char *strerror(int err);

#endif
