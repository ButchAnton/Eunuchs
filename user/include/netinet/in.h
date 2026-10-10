#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <uapi/net.h>

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x)
{
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#define TCP_NODELAY 1

#endif
