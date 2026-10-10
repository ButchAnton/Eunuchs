/*
 * echod [-p port]: echo server on TCP and UDP (default port 7). Each TCP
 * connection is served by a child process; UDP datagrams are answered by
 * the parent. Logs one line per connection/datagram.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int port = 7;
    if (argc > 2 && !strcmp(argv[1], "-p"))
        port = atoi(argv[2]);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    int t = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(t, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(t, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(t, 8) < 0 ||
        bind(u, (struct sockaddr *)&a, sizeof(a)) < 0) {
        printf("echod: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("echod: echoing on tcp and udp port %d\n", port);
    fflush_stdout();
    char buf[4096];
    for (;;) {
        while (waitpid(-1, 0, WNOHANG) > 0)
            ;
        struct pollfd p[2] = { { t, POLLIN, 0 }, { u, POLLIN, 0 } };
        if (poll(p, 2, 1000) <= 0)
            continue;
        if (p[1].revents & POLLIN) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            long n = recvfrom(u, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n >= 0)
                sendto(u, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
        }
        if (p[0].revents & POLLIN) {
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            int c = accept(t, (struct sockaddr *)&peer, &pl);
            if (c < 0)
                continue;
            if (fork() == 0) {
                close(t);
                close(u);
                long n;
                while ((n = read(c, buf, sizeof(buf))) > 0)
                    if (write(c, buf, (size_t)n) != n)
                        break;
                _exit(0);
            }
            close(c);
        }
    }
}
