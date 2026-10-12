/* string.h beyond what the kernel shares (lib/string.c). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *stpcpy(char *d, const char *s)
{
    while ((*d = *s++))
        d++;
    return d;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        e[i] = s[i];
    e[i] = 0;
    return d;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return 0;
    }
    char *e = s + strcspn(s, delim);
    if (*e)
        *e++ = 0;
    *save = e;
    return s;
}

char *strtok(char *s, const char *delim)
{
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strsep(char **sp, const char *delim)
{
    char *s = *sp;
    if (!s)
        return 0;
    char *e = s + strcspn(s, delim);
    if (*e)
        *e++ = 0, *sp = e;
    else
        *sp = 0;
    return s;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    return 0;
}

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--)
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

size_t strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n)
        strlcpy(d, s, n);
    return len;
}
