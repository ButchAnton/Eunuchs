/*
 * netstat [-t] [-u] [-s] [-i] [-a]
 *   (default: TCP and UDP sockets) -t TCP, -u UDP/raw, -s protocol
 *   statistics, -i interfaces, -a ARP table. Reads /proc/net/.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void show(const char *title, const char *path)
{
    printf("%s\n", title);
    fflush_stdout();
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("  (cannot open %s)\n", path);
        return;
    }
    char buf[2048];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
}

int main(int argc, char **argv)
{
    int t = 0, u = 0, s = 0, i = 0, a = 0;
    for (int k = 1; k < argc; k++)
        for (const char *c = argv[k] + (argv[k][0] == '-'); *c; c++) {
            if (*c == 't') t = 1;
            else if (*c == 'u') u = 1;
            else if (*c == 's') s = 1;
            else if (*c == 'i') i = 1;
            else if (*c == 'a') a = 1;
        }
    if (!t && !u && !s && !i && !a)
        t = u = 1;
    if (t)
        show("TCP connections:", "/proc/net/tcp");
    if (u)
        show("UDP and raw sockets:", "/proc/net/udp");
    if (i)
        show("Interfaces:", "/proc/net/dev");
    if (a)
        show("ARP table:", "/proc/net/arp");
    if (s)
        show("Statistics:", "/proc/net/stats");
    return 0;
}
