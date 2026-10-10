#ifndef _STRING_H
#define _STRING_H

#include <kstring.h>    /* memset, memcpy, strlen, strcmp, ... (shared with the kernel) */

char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);

#endif
