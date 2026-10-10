/*
 * ifconfig                                   show interfaces
 * ifconfig IFACE ADDR [netmask M] [gw G] [up|down]   configure (root)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <termios.h>          /* ioctl */

static const char *ip(uint32_t a)
{
    struct in_addr x = { htonl(a) };
    return inet_ntoa(x);
}

static uint32_t parse(const char *s)
{
    struct in_addr a;
    if (!inet_aton(s, &a)) {
        printf("ifconfig: bad address %s\n", s);
        return 0;
    }
    return ntohl(a.s_addr);
}

int main(int argc, char **argv)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        printf("ifconfig: socket: %s\n", strerror(errno));
        return 1;
    }
    if (argc >= 2) {
        struct netif_req r;
        memset(&r, 0, sizeof(r));
        int found = 0;
        for (r.index = 0; ioctl(s, NET_IOC_GETIF, &r) == 0; r.index++)
            if (!strcmp(r.name, argv[1])) {
                found = 1;
                break;
            }
        if (!found) {
            printf("ifconfig: no interface %s\n", argv[1]);
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "up"))
                r.up = 1;
            else if (!strcmp(argv[i], "down"))
                r.up = 0;
            else if (!strcmp(argv[i], "netmask") && i + 1 < argc)
                r.netmask = parse(argv[++i]);
            else if (!strcmp(argv[i], "gw") && i + 1 < argc)
                r.gateway = parse(argv[++i]);
            else
                r.addr = parse(argv[i]);
        }
        if (ioctl(s, NET_IOC_SETIF, &r) < 0) {
            printf("ifconfig: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        if (argc == 2)
            return 0;
    }
    struct netif_req r;
    memset(&r, 0, sizeof(r));
    for (r.index = 0;; r.index++) {
        int idx = r.index;
        if (ioctl(s, NET_IOC_GETIF, &r) < 0)
            break;
        printf("%s: flags=<%s%s>  mtu %u\n", r.name, r.up ? "UP" : "DOWN",
               r.has_mac ? "" : ",LOOPBACK", r.mtu);
        printf("        inet %s", ip(r.addr));
        printf("  netmask %s", ip(r.netmask));
        if (r.gateway)
            printf("  gateway %s", ip(r.gateway));
        printf("\n");
        if (r.has_mac)
            printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n", r.mac[0], r.mac[1], r.mac[2],
                   r.mac[3], r.mac[4], r.mac[5]);
        printf("        RX packets %lu  bytes %lu  dropped %lu\n", (unsigned long)r.rx_packets,
               (unsigned long)r.rx_bytes, (unsigned long)r.rx_dropped);
        printf("        TX packets %lu  bytes %lu  dropped %lu\n", (unsigned long)r.tx_packets,
               (unsigned long)r.tx_bytes, (unsigned long)r.tx_dropped);
        r.index = idx;
    }
    return 0;
}
