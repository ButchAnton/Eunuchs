#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>

/* "a.b.c.d" -> network byte order; returns 1 on success (inet_aton), or the
 * address / INADDR_NONE (inet_addr). inet_ntoa uses a static buffer. */
int inet_aton(const char *s, struct in_addr *out);
uint32_t inet_addr(const char *s);
char *inet_ntoa(struct in_addr a);
#define INADDR_NONE 0xffffffffu

#endif
