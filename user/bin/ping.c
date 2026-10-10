/*
 * ping [-c count] [-i interval_ms] [-s size] [-W timeout_ms] host
 * ICMP echo through a raw socket (root only). host: a.b.c.d, localhost,
 * or "host"/"gateway" for the QEMU host (10.0.2.2).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct icmp {
    uint8_t type, code;
    uint16_t sum, id, seq;
    uint64_t sent_us;
    uint8_t data[1400];
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(1, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static uint16_t cksum(const void *p, int len)
{
    const uint8_t *b = p;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(b[i] << 8 | b[i + 1]);
    if (len & 1)
        sum += (uint32_t)b[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);
}

int main(int argc, char **argv)
{
    int count = 4, interval = 1000, size = 56, wait_ms = 1000;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc)
            interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc)
            wait_ms = atoi(argv[++i]);
        else
            host = argv[i];
    }
    struct in_addr dst;
    if (!host || resolve_host(host, &dst)) {
        printf("usage: ping [-c count] [-i ms] [-s size] [-W ms] host\n");
        return 2;
    }
    if (size < 8)
        size = 8;
    if (size > 1400)
        size = 1400;
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        printf("ping: socket: %s\n", strerror(errno));
        return 2;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = dst };
    uint16_t id = (uint16_t)getpid();
    char ip[16];
    strcpy(ip, inet_ntoa(dst));
    printf("PING %s (%s): %d data bytes\n", host, ip, size);

    int sent = 0, received = 0;
    uint64_t min = ~0ULL, max = 0, total = 0;
    for (int seq = 1; seq <= count; seq++) {
        struct icmp m;
        memset(&m, 0, sizeof(m));
        m.type = 8;
        m.id = htons(id);
        m.seq = htons((uint16_t)seq);
        m.sent_us = now_us();
        for (int k = 0; k < size - 8; k++)
            m.data[k] = (uint8_t)k;
        int len = 8 + size;
        m.sum = cksum(&m, len);
        if (sendto(fd, &m, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
            printf("ping: sendto: %s\n", strerror(errno));
        else
            sent++;

        uint64_t deadline = now_us() + (uint64_t)wait_ms * 1000;
        for (;;) {
            int left = (int)((deadline - now_us()) / 1000);
            if ((int64_t)(deadline - now_us()) <= 0)
                break;
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, left) <= 0)
                break;
            struct icmp r;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            long n = recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
            if (n < 16 || r.type != 0 || ntohs(r.id) != id)
                continue;                       /* someone else's ICMP */
            uint64_t rtt = now_us() - r.sent_us;
            received++;
            total += rtt;
            if (rtt < min)
                min = rtt;
            if (rtt > max)
                max = rtt;
            printf("%ld bytes from %s: icmp_seq=%u time=%lu.%03lu ms\n", n, inet_ntoa(from.sin_addr),
                   ntohs(r.seq), (unsigned long)(rtt / 1000), (unsigned long)(rtt % 1000));
            if (ntohs(r.seq) == seq)
                break;
        }
        if (seq < count)
            sleep_ms((unsigned long)interval);
    }
    printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", sent, received,
           sent ? (sent - received) * 100 / sent : 0);
    if (received)
        printf("rtt min/avg/max = %lu.%03lu/%lu.%03lu/%lu.%03lu ms\n", (unsigned long)(min / 1000),
               (unsigned long)(min % 1000), (unsigned long)(total / received / 1000),
               (unsigned long)(total / received % 1000), (unsigned long)(max / 1000),
               (unsigned long)(max % 1000));
    return received ? 0 : 1;
}
