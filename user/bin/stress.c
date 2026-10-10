/*
 * stress: a parallel "build-like" load for SMP testing. Each worker process
 * repeatedly
 *   - runs a pipeline  /bin/echo <token> | /bin/cat  and checks the output
 *     (fork, exec, pipes, dup2, wait),
 *   - writes, reads back and deletes a file in /tmp (filesystem, log, bcache),
 *   - forks a child that checks copy-on-write memory and maps/unmaps pages
 *     (COW faults, mmap, exit status),
 *   - sends 16 KB to itself over TCP on the loopback interface (sockets,
 *     the network stack and its netd thread, with a forked sender),
 * verifying every result, until the time or iteration limit. With several
 * CPUs all of this runs in parallel; the kernel's lockdep (make test boots
 * with lockdep=panic), lockup watchdog and heap checks watch for trouble.
 *
 * usage: stress [-j workers] [-t seconds] [-n iterations] [-v]
 * Exit status 0 if every check passed.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

struct result {
    int worker, iters, errors;
    unsigned long ms;
};

static int errors, verbose;

#define FAIL(...)                                                           \
    do {                                                                    \
        printf("stress[%d]: ", getpid());                                   \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
        errors++;                                                           \
    } while (0)

/* Pipe ends are close-on-exec; dup2 makes the ones a child needs inheritable. */
static pid_t spawn(char **argv, int in, int out)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (in >= 0)
            dup2(in, 0);
        if (out >= 0)
            dup2(out, 1);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void pipeline(int w, int i)
{
    char token[48], want[52], buf[128];
    snprintf(token, sizeof(token), "w%d-i%d-t%lu", w, i, uptime_ms());
    snprintf(want, sizeof(want), "%s\n", token);
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) {
        FAIL("pipe2 failed");
        return;
    }
    char *echo[] = { "/bin/echo", token, NULL };
    char *cat[] = { "/bin/cat", NULL };
    pid_t p1 = spawn(echo, -1, a[1]);
    pid_t p2 = spawn(cat, a[0], b[1]);
    close(a[0]);
    close(a[1]);
    close(b[1]);
    int n = 0, r;
    while (n < (int)sizeof(buf) - 1 && (r = read(b[0], buf + n, sizeof(buf) - 1 - n)) > 0)
        n += r;
    buf[n] = '\0';
    close(b[0]);
    int s1 = -1, s2 = -1;
    if (p1 < 0 || p2 < 0)
        FAIL("fork failed");
    if (p1 > 0 && waitpid(p1, &s1, 0) != p1)
        FAIL("waitpid(echo) failed");
    if (p2 > 0 && waitpid(p2, &s2, 0) != p2)
        FAIL("waitpid(cat) failed");
    if (s1 != 0 || s2 != 0)
        FAIL("pipeline exit statuses %#x %#x", s1, s2);
    if (strcmp(buf, want))
        FAIL("pipeline output \"%s\", expected \"%s\"", buf, token);
}

static unsigned char fbuf[16384], rbuf[16384];

static void file_io(int w, int i)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/stress.%d.%d", getpid(), i);
    int size = 1000 + (i * 977 + w * 131) % (int)(sizeof(fbuf) - 1000);
    for (int k = 0; k < size; k++)
        fbuf[k] = (unsigned char)(k * 7 + i + w);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("create %s failed", path);
        return;
    }
    if (write(fd, fbuf, size) != size)
        FAIL("write %s failed", path);
    close(fd);
    fd = open(path, O_RDONLY);
    int n = fd >= 0 ? read(fd, rbuf, sizeof(rbuf)) : -1;
    if (fd >= 0)
        close(fd);
    if (n != size || memcmp(fbuf, rbuf, size))
        FAIL("%s read back %d bytes (wrote %d)%s", path, n, size,
             n == size ? ", contents differ" : "");
    if (unlink(path))
        FAIL("unlink %s failed", path);
}

static unsigned char cow[8192];

static void fork_cow(int i)
{
    unsigned char v = (unsigned char)i;
    memset(cow, v, sizeof(cow));
    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        for (size_t k = 0; k < sizeof(cow); k++)
            bad |= cow[k] != v;
        memset(cow, v + 1, sizeof(cow));        /* COW: the parent must not see this */
        char *m = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            bad = 1;
        } else {
            for (int k = 0; k < 4; k++)
                m[k * 4096] = (char)k;
            for (int k = 0; k < 4; k++)
                bad |= m[k * 4096] != (char)k;
            munmap(m, 4 * 4096);
        }
        _exit(bad ? 3 : 0);
    }
    if (pid < 0) {
        FAIL("fork failed");
        return;
    }
    int st = -1;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        FAIL("COW child status %#x", st);
    for (size_t k = 0; k < sizeof(cow); k++) {
        if (cow[k] != v) {
            FAIL("parent memory changed by the child at %d", (int)k);
            break;
        }
    }
}

static unsigned char net_out[16384], net_in[16384];

static void tcp_round(int i)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr = { htonl(INADDR_LOOPBACK) } };
    socklen_t len = sizeof(a);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1) ||
        getsockname(l, (struct sockaddr *)&a, &len)) {
        FAIL("tcp listen: %s", strerror(errno));
        if (l >= 0)
            close(l);
        return;
    }
    for (size_t k = 0; k < sizeof(net_out); k++)
        net_out[k] = (unsigned char)(k * 13 + i);
    pid_t pid = fork();
    if (pid == 0) {
        close(l);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)))
            _exit(2);
        size_t off = 0;
        while (off < sizeof(net_out)) {
            long n = write(s, net_out + off, sizeof(net_out) - off);
            if (n <= 0)
                _exit(3);
            off += (size_t)n;
        }
        _exit(0);
    }
    int c = accept(l, 0, 0);
    size_t got = 0;
    long n = 0;
    while (c >= 0 && got < sizeof(net_in) && (n = read(c, net_in + got, sizeof(net_in) - got)) > 0)
        got += (size_t)n;
    if (got != sizeof(net_in) || memcmp(net_in, net_out, got))
        FAIL("tcp loopback: got %d of %d bytes%s", (int)got, (int)sizeof(net_in),
             got == sizeof(net_in) ? " (corrupted)" : "");
    if (c >= 0)
        close(c);
    close(l);
    int st = -1;
    waitpid(pid, &st, 0);
    if (st != 0)
        FAIL("tcp sender status %#x", st);
}

static void worker(int w, int iters, unsigned long deadline, int report)
{
    unsigned long t0 = uptime_ms();
    int i = 0;
    while ((!iters || i < iters) && (!deadline || uptime_ms() < deadline) && errors < 10) {
        pipeline(w, i);
        file_io(w, i);
        fork_cow(i);
        tcp_round(i);
        i++;
    }
    struct result r = { w, i, errors, uptime_ms() - t0 };
    write(report, &r, sizeof(r));
    _exit(errors ? 1 : 0);
}

static void show_file(const char *path, int max_lines)
{
    char buf[2048];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    for (char *p = buf; *p && max_lines; p++) {
        if (*p == '\n' && --max_lines == 0)
            p[1] = '\0';
    }
    printf("%s", buf);
}

int main(int argc, char **argv)
{
    int jobs = 8, secs = 10, iters = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-j") && i + 1 < argc)
            jobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else {
            printf("usage: stress [-j workers] [-t seconds] [-n iterations] [-v]\n");
            return 2;
        }
    }
    if (jobs < 1 || jobs > 64)
        jobs = 8;
    printf("stress: %d workers, %s%d %s\n", jobs, iters ? "" : "for ", iters ? iters : secs,
           iters ? "iterations each" : "seconds");

    int rep[2];
    if (pipe(rep)) {
        printf("stress: pipe failed\n");
        return 1;
    }
    unsigned long t0 = uptime_ms(), deadline = iters ? 0 : t0 + (unsigned long)secs * 1000;
    for (int w = 0; w < jobs; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(rep[0]);
            worker(w, iters, deadline, rep[1]);
        }
        if (pid < 0)
            FAIL("cannot fork worker %d", w);
    }
    close(rep[1]);

    int total = 0, bad = 0, reported = 0;
    struct result r;
    while (read(rep[0], &r, sizeof(r)) == sizeof(r)) {
        reported++;
        total += r.iters;
        bad += r.errors;
        if (verbose || r.errors)
            printf("  worker %d: %d iterations in %lu ms, %d errors\n", r.worker, r.iters, r.ms,
                   r.errors);
    }
    int st, failed_exits = 0;
    while (wait(&st) > 0)
        failed_exits += !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    unsigned long ms = uptime_ms() - t0;

    printf("stress: %d iterations (%d pipelines = %d execs, %d files, %d COW forks, "
           "%d TCP connections) in %lu ms, %d errors\n", total, total, 2 * total, total, total,
           total, ms, bad + errors);
    show_file("/proc/cpus", 8);
    show_file("/proc/lockdep", 1);
    show_file("/proc/net/stats", 20);
    int ok = reported == jobs && !failed_exits && !bad && !errors;
    printf("STRESS RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
