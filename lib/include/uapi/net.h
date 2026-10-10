#ifndef EUNUCHS_UAPI_NET_H
#define EUNUCHS_UAPI_NET_H

/*
 * Sockets and poll: types and constants shared by the kernel and the user
 * libc (Linux values, so the layouts match what C programs expect).
 * Addresses and ports in struct sockaddr_in are in network byte order.
 */

#include <stdint.h>

#define AF_UNSPEC       0
#define AF_INET         2
#define PF_INET         AF_INET

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_NONBLOCK   04000           /* O_NONBLOCK */
#define SOCK_CLOEXEC    02000000        /* O_CLOEXEC */

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7f000001u    /* host byte order */
#define INADDR_BROADCAST 0xffffffffu

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

typedef uint32_t socklen_t;

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_RCVBUF       8
#define SO_SNDBUF       7
#define SO_RCVTIMEO     20

#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40

/* poll */
#define POLLIN          0x001
#define POLLPRI         0x002
#define POLLOUT         0x004
#define POLLERR         0x008
#define POLLHUP         0x010
#define POLLNVAL        0x020

struct pollfd {
    int fd;
    short events;
    short revents;
};

/*
 * Interface configuration (ifconfig): ioctl on any socket.
 * Addresses in host byte order.
 */
#define NET_IOC_GETIF   0x8900          /* struct netif_req, by index */
#define NET_IOC_SETIF   0x8901          /* struct netif_req, by name */

struct netif_req {
    int index;                          /* GETIF: which interface (0..) */
    char name[8];
    uint8_t mac[6];
    uint8_t up;
    uint8_t has_mac;
    uint32_t addr, netmask, gateway;
    uint32_t mtu;
    uint64_t rx_packets, rx_bytes, rx_dropped, tx_packets, tx_bytes, tx_dropped;
};

#endif
