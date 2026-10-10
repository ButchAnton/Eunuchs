#ifndef _NETDB_H
#define _NETDB_H

#include <netinet/in.h>

/*
 * No DNS client: names are numeric addresses, "localhost", or "host" /
 * "gateway" (the QEMU host, 10.0.2.2). Returns 0 and the address in network
 * byte order, or -1.
 */
int resolve_host(const char *name, struct in_addr *out);

#endif
