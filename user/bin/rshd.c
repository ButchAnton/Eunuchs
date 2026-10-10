/*
 * rshd [-p port]: a remote shell (default port 23; make run forwards
 * localhost:2323 on the Mac to it: `nc localhost 2323`). Each connection
 * gets /bin/sh -i with the socket as stdin, stdout and stderr. There is no
 * login or encryption: this is for a VM on your own machine.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int port = 23;
    if (argc > 2 && !strcmp(argv[1], "-p"))
        port = atoi(argv[2]);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
        printf("rshd: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("rshd: remote shell on port %d\n", port);
    fflush_stdout();
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        while (waitpid(-1, 0, WNOHANG) > 0)
            ;
        if (c < 0)
            continue;
        printf("rshd: connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        fflush_stdout();
        if (fork() == 0) {
            close(s);
            dup2(c, 0);
            dup2(c, 1);
            dup2(c, 2);
            if (c > 2)
                close(c);
            const char *hello = "Eunuchs remote shell. Type 'exit' to leave.\n";
            write(1, hello, strlen(hello));
            char *sh[] = { "sh", "-i", NULL };
            execv("/bin/sh", sh);
            _exit(127);
        }
        close(c);
    }
}
