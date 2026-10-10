/*
 * nc: netcat.
 *   nc [-u] [-v] host port         connect; stdin -> socket, socket -> stdout
 *   nc -l [-u] [-k] [-v] port      listen (TCP: one connection; -k: keep
 *                                  accepting; UDP: answer the last sender)
 * At the end of stdin the sending side is shut down (FIN) and nc keeps
 * printing until the other side closes too.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int verbose;
static char buf[8192];

static void say(const char *fmt, const char *a, int b)
{
    if (!verbose)
        return;
    char line[160];
    snprintf(line, sizeof(line), fmt, a, b);
    write(2, line, strlen(line));
}

static int write_all(int fd, const char *p, long n)
{
    while (n > 0) {
        long w = write(fd, p, (size_t)n);
        if (w <= 0)
            return -1;
        p += w;
        n -= w;
    }
    return 0;
}

/* Shuttle data between stdin/stdout and a connected socket. */
static int pump(int s, int udp, struct sockaddr_in *peer, int have_peer)
{
    int stdin_open = 1;
    for (;;) {
        struct pollfd p[2] = { { s, POLLIN, 0 }, { 0, POLLIN, 0 } };
        /* UDP has no end-of-stream: after stdin ends, wait a second for replies */
        int n = poll(p, stdin_open ? 2 : 1, !stdin_open && udp ? 1000 : -1);
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            long r = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (r < 0) {
                say("nc: receive: %s%.0d\n", strerror(errno), 0);
                return 1;
            }
            if (r == 0 && !udp)
                return 0;                       /* the other side is done */
            if (udp && !have_peer) {
                *peer = from;
                have_peer = 1;
            }
            write_all(1, buf, r);
        }
        if (stdin_open && (p[1].revents & (POLLIN | POLLHUP))) {
            long r = read(0, buf, sizeof(buf));
            if (r <= 0) {
                stdin_open = 0;
                if (!udp)
                    shutdown(s, SHUT_WR);
                continue;
            }
            long off = 0;
            while (off < r) {
                long w = udp ? (have_peer ? sendto(s, buf + off, (size_t)(r - off), 0,
                                                   (struct sockaddr *)peer, sizeof(*peer))
                                          : (long)(r - off))
                             : send(s, buf + off, (size_t)(r - off), 0);
                if (w < 0) {
                    say("nc: send: %s%.0d\n", strerror(errno), 0);
                    return 1;
                }
                off += w;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int listen_mode = 0, udp = 0, keep = 0;
    const char *host = NULL, *port_s = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *c = argv[i] + 1; *c; c++) {
                if (*c == 'l') listen_mode = 1;
                else if (*c == 'u') udp = 1;
                else if (*c == 'k') keep = 1;
                else if (*c == 'v') verbose = 1;
            }
        } else if (!host && !listen_mode) {
            host = argv[i];
        } else {
            port_s = argv[i];
        }
    }
    if (listen_mode && !port_s)
        port_s = host;
    if (!port_s || (!listen_mode && !host)) {
        printf("usage: nc [-uv] host port | nc -l [-ukv] port\n");
        return 2;
    }
    int port = atoi(port_s);
    int s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s < 0) {
        printf("nc: socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (listen_mode) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
            printf("nc: bind port %d: %s\n", port, strerror(errno));
            return 1;
        }
        if (udp) {
            say("nc: listening on udp port %s%d\n", "", port);
            return pump(s, 1, &a, 0);
        }
        listen(s, 4);
        say("nc: listening on tcp port %s%d\n", "", port);
        do {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            int c = accept(s, (struct sockaddr *)&peer, &pl);
            if (c < 0) {
                printf("nc: accept: %s\n", strerror(errno));
                return 1;
            }
            say("nc: connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            int rc = pump(c, 0, NULL, 0);
            close(c);
            if (!keep)
                return rc;
        } while (keep);
        return 0;
    }
    if (resolve_host(host, &a.sin_addr)) {
        printf("nc: unknown host %s\n", host);
        return 1;
    }
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        printf("nc: connect to %s port %d: %s\n", host, port, strerror(errno));
        return 1;
    }
    say("nc: connected to %s port %d\n", host, port);
    return pump(s, udp, &a, 1);
}
