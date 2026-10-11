/*
 * init (pid 1): starts the shell and restarts it if it exits; reaps
 * orphaned zombies. `init -1 PROG ARGS...` runs PROG once and exits with its
 * status (used by `make test`; the kernel then powers off).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t start(char **argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        eprintf("init: cannot exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv)
{
    int once = argc > 1 && !strcmp(argv[1], "-1");
    char *dflt[] = { "/bin/esh", "-l", 0 };
    char **prog = argc > (once ? 2 : 1) ? argv + (once ? 2 : 1) : dflt;

    printf("init: pid %d, running %s%s\n", getpid(), prog[0], once ? " once" : "");
    if (!once) {
        int fd = open("/etc/motd", O_RDONLY);
        char buf[512];
        long n;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        if (fd >= 0)
            close(fd);
    }
    for (;;) {
        pid_t child = start(prog);
        for (;;) {
            int st;
            pid_t w = wait(&st);
            if (w < 0)
                break;
            if (w != child)
                continue;                   /* an orphan we adopted */
            if (once)
                return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            printf("init: %s (pid %d) %s %d; restarting\n", prog[0], w,
                   WIFEXITED(st) ? "exited with status" : "killed by signal",
                   WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            break;
        }
        sleep_ms(200);
    }
}
