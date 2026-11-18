/*
 * init (pid 1): starts the shell and restarts it if it exits; reaps
 * orphaned zombies. `init -1 PROG ARGS...` runs PROG once and exits with its
 * status (used by `make test`; the kernel then powers off).
 *
 * Shutting down: SIGUSR1 asks init to reboot, SIGUSR2 to power off (that is
 * what shutdown, reboot and poweroff send). init stops restarting the shell,
 * hangs up and terminates every other process (SIGHUP, SIGTERM, SIGCONT),
 * gives them a few seconds to exit, kills what is left, syncs and calls
 * reboot(2).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

#define GRACE_MS 5000                       /* after SIGTERM, before SIGKILL */

static volatile int going_down;             /* 0, SIGUSR1 (reboot) or SIGUSR2 (power off) */

static void on_shutdown(int sig)
{
    going_down = sig;
}

static void on_child(int sig)
{
    (void)sig;                              /* only to end sigsuspend */
}

static pid_t start(char **argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        signal(SIGUSR1, SIG_DFL);
        signal(SIGUSR2, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        sigprocmask(SIG_SETMASK, &none, 0);
        execv(argv[0], argv);
        eprintf("init: cannot exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

/* Reap every child that has exited; 1 if none are left. */
static int reap(void)
{
    for (;;) {
        pid_t w = waitpid(-1, 0, WNOHANG);
        if (w < 0)
            return errno == ECHILD;
        if (w == 0)
            return 0;
    }
}

static void shut_down(void)
{
    int rb = going_down == SIGUSR1;
    printf("init: %s\n", rb ? "rebooting" : "powering off");
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    if (kill(-1, SIGHUP) == 0) {
        kill(-1, SIGTERM);
        kill(-1, SIGCONT);                  /* stopped jobs must run to die */
        unsigned long until = uptime_ms() + GRACE_MS;
        while (!reap() && uptime_ms() < until)
            sleep_ms(50);
        if (!reap()) {
            printf("init: killing the processes that are left\n");
            kill(-1, SIGKILL);
            until = uptime_ms() + GRACE_MS;
            while (!reap() && uptime_ms() < until)
                sleep_ms(50);
        }
    }
    sync();
    reboot(rb ? RB_AUTOBOOT : RB_POWER_OFF);
    eprintf("init: reboot: %s\n", strerror(errno));
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    int once = argc > 1 && !strcmp(argv[1], "-1");
    char *dflt[] = { "/bin/esh", "-l", 0 };
    char **prog = argc > (once ? 2 : 1) ? argv + (once ? 2 : 1) : dflt;

    /* The signals that matter are blocked except in sigsuspend, so one that
       comes just before init goes to sleep still wakes it. */
    sigset_t block, none;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGCHLD);
    sigemptyset(&none);
    sigprocmask(SIG_BLOCK, &block, 0);
    signal(SIGUSR1, on_shutdown);
    signal(SIGUSR2, on_shutdown);
    signal(SIGCHLD, on_child);

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
            if (going_down)
                shut_down();
            int st;
            pid_t w = waitpid(-1, &st, WNOHANG);
            if (w == 0 || (w < 0 && errno == EINTR)) {
                sigsuspend(&none);
                continue;
            }
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
        if (going_down)
            shut_down();
    }
}
