/*
 * shutdown: bring the system down in an orderly way, now or later.
 *
 *   shutdown [-r | -h | -p] [-k] [TIME] [MESSAGE...]
 *   shutdown -c [MESSAGE...]
 *
 *   -r          reboot (through the boot loader)
 *   -h, -p      power off (the default; Eunuchs has no halted state)
 *   -k          only send the warnings; do not shut down
 *   -c          cancel a pending shutdown
 *   TIME        now (the default), +MINUTES, or HH:MM (today, or tomorrow
 *               if that time has passed)
 *
 * A delayed shutdown goes to the background (its pid is in
 * /tmp/shutdown.pid) and warns on the console as the time comes closer. When
 * the time comes it asks init to shut down (SIGUSR1: reboot, SIGUSR2: power
 * off); init terminates every process, syncs and calls reboot(2). Root only.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PIDFILE "/tmp/shutdown.pid"

static char message[512];

static void usage(void)
{
    eprintf("usage: shutdown [-r | -h | -p] [-k] [TIME] [MESSAGE...]\n"
            "       shutdown -c [MESSAGE...]\n"
            "TIME is now, +MINUTES or HH:MM\n");
    exit(2);
}

/* Write to the console (everyone sees it there), or to stderr without one. */
static void broadcast(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    int fd = open("/dev/console", O_WRONLY);
    write(fd >= 0 ? fd : 2, buf, n);
    if (fd >= 0)
        close(fd);
}

static void warn_users(const char *what, long secs)
{
    char when[64];
    if (secs <= 0)
        snprintf(when, sizeof(when), "NOW");
    else if (secs < 60)
        snprintf(when, sizeof(when), "in %ld second%s", secs, secs == 1 ? "" : "s");
    else {
        long m = (secs + 30) / 60;
        snprintf(when, sizeof(when), "in %ld minute%s", m, m == 1 ? "" : "s");
    }
    broadcast("\n*** System shutdown message from root ***\n"
              "The system is going down for %s %s!\n%s%s",
              what, when, message, *message ? "\n" : "");
}

/* The pid of a pending shutdown, or 0. */
static pid_t pending(void)
{
    char buf[256];
    int fd = open(PIDFILE, O_RDONLY);
    if (fd < 0)
        return 0;
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    pid_t pid = atoi(buf);
    if (pid <= 1 || pid == getpid())
        return 0;
    /* a stale file (from before a reboot) names someone else, or no one */
    snprintf(buf, sizeof(buf), "/proc/%d/cmdline", pid);
    fd = open(buf, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    const char *base = strrchr(buf, '/');
    base = base ? base + 1 : buf;
    return !strcmp(base, "shutdown") ? pid : 0;
}

/* Seconds from now until TIME, or -1 if it isn't one. */
static long parse_time(const char *s)
{
    char *end;
    if (!strcmp(s, "now"))
        return 0;
    if (*s == '+') {
        long m = strtol(s + 1, &end, 10);
        return end == s + 1 || *end || m < 0 ? -1 : m * 60;
    }
    long h = strtol(s, &end, 10);
    if (end == s || *end != ':' || h < 0 || h > 23)
        return -1;
    const char *ms = end + 1;
    long m = strtol(ms, &end, 10);
    if (end != ms + 2 || *end || m < 0 || m > 59)
        return -1;
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    long secs = (h * 3600 + m * 60) - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    if (secs < 0)
        secs += 24 * 3600;                  /* tomorrow */
    return secs;
}

/* When to warn, in seconds before the shutdown (largest first). */
static const long marks[] = { 3600, 1800, 900, 600, 300, 120, 60, 30, 10, 0 };

int main(int argc, char **argv)
{
    int reboot = 0, warn_only = 0, cancel = 0, i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'r': reboot = 1; break;
            case 'h':
            case 'p':
            case 'P': reboot = 0; break;
            case 'k': warn_only = 1; break;
            case 'c': cancel = 1; break;
            default: usage();
            }
        }
    }
    long secs = 0;
    if (!cancel && i < argc) {
        if ((secs = parse_time(argv[i])) < 0) {
            eprintf("shutdown: bad time '%s'\n", argv[i]);
            usage();
        }
        i++;
    }
    for (size_t len = 0; i < argc; i++)
        len += snprintf(message + len, len < sizeof(message) ? sizeof(message) - len : 0, "%s%s",
                        len ? " " : "", argv[i]);

    if (geteuid() != 0) {
        eprintf("shutdown: you must be root\n");
        return 1;
    }

    pid_t other = pending();
    if (cancel) {
        if (!other) {
            eprintf("shutdown: no shutdown is pending\n");
            return 1;
        }
        if (kill(other, SIGTERM) < 0) {
            eprintf("shutdown: cannot cancel pid %d: %s\n", other, strerror(errno));
            return 1;
        }
        unlink(PIDFILE);
        broadcast("\n*** System shutdown message from root ***\n"
                  "The pending shutdown has been cancelled.\n%s%s",
                  message, *message ? "\n" : "");
        return 0;
    }
    if (other && !warn_only) {
        eprintf("shutdown: a shutdown is already pending (pid %d); 'shutdown -c' cancels it\n",
                other);
        return 1;
    }

    const char *what = reboot ? "reboot" : "power off";
    if (secs > 0) {
        pid_t pid = fork();
        if (pid < 0) {
            eprintf("shutdown: fork: %s\n", strerror(errno));
            return 1;
        }
        if (pid > 0) {
            time_t at = time(0) + secs;
            char hm[16];
            strftime(hm, sizeof(hm), "%H:%M", localtime(&at));
            printf("shutdown: %s %s at %s (pid %d); 'shutdown -c' cancels it\n",
                   warn_only ? "warning of a" : "scheduled", what, hm, pid);
            return 0;
        }
        setsid();                           /* out of the shell's jobs */
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        if (!warn_only) {
            int fd = open(PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char buf[16];
                write(fd, buf, snprintf(buf, sizeof(buf), "%d\n", getpid()));
                close(fd);
            }
        }
        unsigned long end = uptime_ms() + secs * 1000;
        warn_users(what, secs);
        for (size_t m = 0; m < sizeof(marks) / sizeof(marks[0]); m++) {
            if (marks[m] >= secs)
                continue;                   /* already past it (or just warned) */
            unsigned long at = end - marks[m] * 1000, now;
            while ((now = uptime_ms()) < at)
                sleep_ms(at - now);
            if (marks[m])
                warn_users(what, marks[m]);
        }
        if (!warn_only)
            unlink(PIDFILE);
    }
    warn_users(what, 0);
    if (warn_only)
        return 0;
    sync();
    if (kill(1, reboot ? SIGUSR1 : SIGUSR2) < 0) {
        eprintf("shutdown: cannot signal init: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
