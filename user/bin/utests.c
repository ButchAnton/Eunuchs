/*
 * User-space test suite: processes, exec, COW, memory, faults, and syscall
 * argument checking. Ends with "UTEST RESULT: PASS|FAIL"; exit status 0/1.
 */
#include <dirent.h>
#include <errno.h>
#include <eunuchs.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>

static int failures, test_failed;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("    %s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            test_failed = 1;                                                \
        }                                                                   \
    } while (0)

/* Run a program, return its raw wait status. */
static int run(char **argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int st = -1;
    waitpid(pid, &st, 0);
    return st;
}

static void t_fork_wait_status(void)
{
    pid_t pid = fork();
    if (pid == 0)
        _exit(42);
    CHECK(pid > 0);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);
}

static void t_many_children(void)
{
    int sum = 0;
    for (int i = 1; i <= 10; i++)
        if (fork() == 0)
            _exit(i);
    for (int i = 0; i < 10; i++) {
        int st;
        CHECK(wait(&st) > 0);
        sum += WEXITSTATUS(st);
    }
    CHECK(sum == 55);
    CHECK(wait(0) == -1 && errno == ECHILD);
}

static int cow_data[4096];

static void t_cow_isolation(void)
{
    for (int i = 0; i < 4096; i++)
        cow_data[i] = i;
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 4096; i++)
            if (cow_data[i] != i)
                _exit(1);
        for (int i = 0; i < 4096; i++)
            cow_data[i] = -1;               /* private copy */
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    for (int i = 0; i < 4096; i += 97)
        CHECK(cow_data[i] == i);            /* parent's copy untouched */
}

static void t_exec_args(void)
{
    char *argv[] = { "/bin/exitcode", "7", "check-args", "two words", "", 0 };
    int st = run(argv);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

static void t_exec_missing(void)
{
    char *argv[] = { "/bin/no-such-program", 0 };
    CHECK(execv(argv[0], argv) == -1 && errno == ENOENT);
}

static void t_faults_kill_with_signals(void)
{
    char *null[] = { "/bin/segv", "null", 0 };
    char *text[] = { "/bin/segv", "text", 0 };
    char *stack[] = { "/bin/segv", "stack", 0 };
    char *ill[] = { "/bin/segv", "illegal", 0 };
    char *kern[] = { "/bin/segv", "kernel", 0 };
    int st;
    st = run(null);  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 11);
    st = run(text);  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 11);
    st = run(stack); CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 11);
    st = run(ill);   CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 4);
    st = run(kern);  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 11);
}

static void t_malloc(void)
{
    char *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = malloc(1 + i * 37);
        CHECK(p[i] != 0);
        memset(p[i], i, 1 + i * 37);
    }
    for (int i = 0; i < 64; i += 2)
        free(p[i]);
    for (int i = 1; i < 64; i += 2)
        CHECK(memchr_inv(p[i], i, 1 + i * 37) == 0);
    for (int i = 1; i < 64; i += 2)
        free(p[i]);
    char *big = malloc(300000);             /* grows the heap by many pages */
    CHECK(big != 0);
    memset(big, 0x5c, 300000);
    CHECK(big[299999] == 0x5c);
    free(big);
}

static void t_mmap(void)
{
    size_t len = 256 * 1024;
    char *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    for (size_t i = 0; i < len; i += 4096)
        CHECK(m[i] == 0);                   /* demand-zero */
    for (size_t i = 0; i < len; i += 4096)
        m[i] = (char)(i >> 12);
    for (size_t i = 0; i < len; i += 4096)
        CHECK(m[i] == (char)(i >> 12));
    CHECK(munmap(m, len) == 0);
    void *wx = mmap(0, 4096, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(wx == MAP_FAILED && errno == EACCES);
}

static void t_bad_pointers_rejected(void)
{
    CHECK(write(1, (void *)0x10, 5) == -1 && errno == EFAULT);
    /* A kernel address must never be readable through a system call. */
    CHECK(write(1, (void *)0xffffffc080200000UL, 5) == -1 && errno == EFAULT);
    CHECK(read(0, (void *)0xffffffc080200000UL, 1) == -1 && errno == EFAULT);
    char *argv[] = { "/bin/hello", (char *)0x20, 0 };
    CHECK(execv("/bin/hello", argv) == -1 && errno == EFAULT);
    CHECK(waitpid(-1, (int *)0xffffffc080200000UL, WNOHANG) == -1);
}

static void t_kill(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(10000);
        _exit(0);
    }
    sleep_ms(20);
    CHECK(kill(pid, 9) == 0);
    int st;
    unsigned long t0 = uptime_ms();
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == 9);
    CHECK(uptime_ms() - t0 < 1000);
    CHECK(kill(99999, 9) == -1 && errno == ESRCH);
}

static void t_sleep_and_ids(void)
{
    unsigned long t0 = uptime_ms();
    sleep_ms(50);
    CHECK(uptime_ms() - t0 >= 50);
    CHECK(getpid() > 1);
    CHECK(getppid() >= 1);
}

static void t_brk(void)
{
    char *a = sbrk(0);
    CHECK(sbrk(8192) == a);
    a[0] = 1;
    a[8191] = 2;
    CHECK(sbrk(-8192) != (void *)-1);
    CHECK(sbrk(0) == a);
}

/* ---- files ---- */

static int write_file(const char *path, const char *data, int flags)
{
    int fd = open(path, O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0)
        return -1;
    long n = write(fd, data, strlen(data));
    close(fd);
    return n == (long)strlen(data) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return (int)n;
}

static void t_file_rw(void)
{
    char buf[64];
    const char *p = "/tmp/ut_rw";
    unlink(p);
    CHECK(write_file(p, "hello, disk", O_EXCL) == 0);
    CHECK(open(p, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    CHECK(read_file(p, buf, sizeof(buf)) == 11 && !strcmp(buf, "hello, disk"));
    CHECK(write_file(p, "!!", O_APPEND) == 0);
    CHECK(read_file(p, buf, sizeof(buf)) == 13 && !strcmp(buf, "hello, disk!!"));

    int fd = open(p, O_RDWR);
    CHECK(lseek(fd, 7, SEEK_SET) == 7);
    CHECK(write(fd, "DISK", 4) == 4);
    CHECK(lseek(fd, 0, SEEK_END) == 13);
    CHECK(lseek(fd, -20, SEEK_CUR) == -1 && errno == EINVAL);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 13 && S_ISREG(st.st_mode) && st.st_nlink == 1);
    close(fd);
    CHECK(read_file(p, buf, sizeof(buf)) == 13 && !strcmp(buf, "hello, DISK!!"));

    CHECK(write_file(p, "new", O_TRUNC) == 0);
    CHECK(read_file(p, buf, sizeof(buf)) == 3);
    fd = open(p, O_RDONLY);
    CHECK(write(fd, "x", 1) == -1 && errno == EBADF);
    close(fd);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(unlink(p) == 0);
    CHECK(open(p, O_RDONLY) == -1 && errno == ENOENT);
}

static void t_big_file(void)
{
    /* 4.5 MiB: direct, single-indirect and into the double-indirect blocks. */
    const char *p = "/tmp/ut_big";
    static unsigned buf[16384];         /* 64 KiB */
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    for (unsigned chunk = 0; chunk < 72; chunk++) {
        for (unsigned i = 0; i < 16384; i++)
            buf[i] = chunk * 16384 + i;
        if (write(fd, buf, sizeof(buf)) != sizeof(buf)) {
            CHECK(!"short write");
            break;
        }
    }
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 72 * sizeof(buf));
    for (unsigned chunk = 0; chunk < 72; chunk += 7) {
        unsigned v = 0;
        lseek(fd, (long)chunk * sizeof(buf) + 4 * 1234, SEEK_SET);
        CHECK(read(fd, &v, 4) == 4 && v == chunk * 16384 + 1234);
    }
    close(fd);
    CHECK(unlink(p) == 0);
}

static int count_entries(const char *path)
{
    DIR *d = opendir(path);
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)))
        n++;
    if (d)
        closedir(d);
    return n;
}

static void t_dirs(void)
{
    const char *d = "/tmp/ut_dir";
    struct stat st, pst0, pst1;
    char path[64];

    stat("/tmp", &pst0);
    CHECK(mkdir(d, 0755) == 0);
    CHECK(mkdir(d, 0755) == -1 && errno == EEXIST);
    stat("/tmp", &pst1);
    CHECK(pst1.st_nlink == pst0.st_nlink + 1);         /* the new ".." */
    CHECK(stat(d, &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 2);
    CHECK(count_entries(d) == 2);                       /* . and .. */

    for (int i = 0; i < 100; i++) {                     /* > one block of entries */
        snprintf(path, sizeof(path), "%s/f%d", d, i);
        CHECK(write_file(path, "x", 0) == 0);
    }
    CHECK(count_entries(d) == 102);
    CHECK(rmdir(d) == -1 && errno == ENOTEMPTY);
    CHECK(unlink(d) == -1 && errno == EISDIR);
    for (int i = 0; i < 100; i++) {
        snprintf(path, sizeof(path), "%s/f%d", d, i);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(d) == 0);
    CHECK(stat(d, &st) == -1 && errno == ENOENT);
    stat("/tmp", &pst1);
    CHECK(pst1.st_nlink == pst0.st_nlink);
}

static void t_link_rename(void)
{
    char buf[32];
    struct stat st;
    write_file("/tmp/ut_a", "AAA", O_TRUNC);
    CHECK(link("/tmp/ut_a", "/tmp/ut_b") == 0);
    CHECK(stat("/tmp/ut_a", &st) == 0 && st.st_nlink == 2);
    CHECK(unlink("/tmp/ut_a") == 0);
    CHECK(read_file("/tmp/ut_b", buf, sizeof(buf)) == 3 && !strcmp(buf, "AAA"));
    CHECK(stat("/tmp/ut_b", &st) == 0 && st.st_nlink == 1);

    CHECK(rename("/tmp/ut_b", "/tmp/ut_c") == 0);
    CHECK(stat("/tmp/ut_b", &st) == -1);
    write_file("/tmp/ut_d", "DDD", O_TRUNC);
    CHECK(rename("/tmp/ut_d", "/tmp/ut_c") == 0);       /* replaces ut_c */
    CHECK(read_file("/tmp/ut_c", buf, sizeof(buf)) == 3 && !strcmp(buf, "DDD"));

    CHECK(mkdir("/tmp/ut_m1", 0755) == 0);
    CHECK(mkdir("/tmp/ut_m1/sub", 0755) == 0);
    CHECK(rename("/tmp/ut_m1", "/tmp/ut_m1/sub/x") == -1 && errno == EINVAL);
    CHECK(mkdir("/home/ut_m2", 0755) == 0);
    CHECK(rename("/tmp/ut_m1/sub", "/home/ut_m2/sub") == 0);   /* move a directory */
    char cwd[128];
    CHECK(chdir("/home/ut_m2/sub/..") == 0);            /* ".." was updated */
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/home/ut_m2"));
    chdir("/");
    CHECK(link("/tmp/ut_m1", "/tmp/ut_m3") == -1 && errno == EPERM);   /* no dir hard links */
    CHECK(rmdir("/home/ut_m2/sub") == 0 && rmdir("/home/ut_m2") == 0 && rmdir("/tmp/ut_m1") == 0);
    CHECK(unlink("/tmp/ut_c") == 0);
}

static void t_paths(void)
{
    char cwd[128], name[80];
    struct stat st;
    write_file("/tmp/ut_p", "p", O_TRUNC);
    CHECK(stat("/tmp/../tmp/./ut_p", &st) == 0);
    CHECK(stat("//tmp///ut_p", &st) == 0);
    CHECK(stat("/tmp/ut_p/x", &st) == -1 && errno == ENOTDIR);
    memset(name, 'n', 70);
    name[0] = '/';
    name[70] = 0;
    CHECK(stat(name, &st) == -1 && errno == ENAMETOOLONG);
    CHECK(chdir("/tmp") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/tmp"));
    CHECK(stat("ut_p", &st) == 0);                       /* relative */
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    CHECK(chdir("/proc/self") == 0 && getcwd(cwd, sizeof(cwd)) && !strncmp(cwd, "/proc/", 6));
    CHECK(chdir("/dev/..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    CHECK(chdir("/tmp/ut_p") == -1 && errno == ENOTDIR);
    unlink("/tmp/ut_p");
}

static void t_fds(void)
{
    int fd = open("/tmp/ut_fd", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    int fd2 = dup(fd);
    CHECK(fd2 > fd);
    write(fd, "abc", 3);
    CHECK(lseek(fd2, 0, SEEK_CUR) == 3);                /* shared offset */
    CHECK(dup2(fd, 20) == 20);
    pid_t pid = fork();
    if (pid == 0) {
        write(20, "def", 3);                            /* inherited descriptor */
        _exit(0);
    }
    waitpid(pid, 0, 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 6);                 /* the child moved it */
    close(20);
    close(fd2);
    close(fd);
    int fds[40], n = 0;
    while (n < 40 && (fds[n] = open("/dev/null", O_RDONLY)) >= 0)
        n++;
    CHECK(n < 40 && errno == EMFILE);
    while (n--)
        close(fds[n]);
    unlink("/tmp/ut_fd");
}

static void t_devices_and_proc(void)
{
    char buf[256];
    int fd = open("/dev/null", O_RDWR);
    CHECK(write(fd, "gone", 4) == 4 && read(fd, buf, 4) == 0);
    close(fd);
    fd = open("/dev/zero", O_RDONLY);
    memset(buf, 1, 16);
    CHECK(read(fd, buf, 16) == 16 && buf[0] == 0 && buf[15] == 0);
    close(fd);
    struct stat st;
    CHECK(stat("/dev/console", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(read_file("/proc/meminfo", buf, sizeof(buf)) > 0 && strstr(buf, "RAM"));
    char want[32];
    snprintf(want, sizeof(want), "pid %d\n", getpid());
    CHECK(read_file("/proc/self/status", buf, sizeof(buf)) > 0 && strstr(buf, want));
    snprintf(want, sizeof(want), "/proc/%d", getpid());
    CHECK(stat(want, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(open("/proc/meminfo", O_WRONLY) == -1);
    CHECK(read_file("/dev/kmsg", buf, sizeof(buf)) > 0);
}

static void t_exec_copied_program(void)
{
    char buf[4096];
    int in = open("/bin/exitcode", O_RDONLY), out = open("/tmp/ut_exe", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    long n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
    char *argv[] = { "/tmp/ut_exe", "9", 0 };
    int st = run(argv);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
    unlink("/tmp/ut_exe");
}

/* ---- signals ---- */

static volatile int got_sig, sig_count;

static void on_sig(int sig)
{
    got_sig = sig;
    sig_count++;
}

static void catch(int sig, void (*h)(int))
{
    struct sigaction act = { .sa_handler = h };
    sigaction(sig, &act, 0);
}

static void t_signal_handler(void)
{
    got_sig = 0;
    catch(SIGUSR1, on_sig);
    volatile long a = 12345, b = 67890;
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(got_sig == SIGUSR1);
    CHECK(a == 12345 && b == 67890);        /* registers/stack restored by sigreturn */
    signal(SIGUSR1, SIG_DFL);
}

static void t_signal_mask(void)
{
    sigset_t set, old;
    got_sig = 0;
    catch(SIGUSR1, on_sig);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    kill(getpid(), SIGUSR1);
    CHECK(got_sig == 0);                    /* blocked: stays pending */
    sigprocmask(SIG_SETMASK, &old, 0);
    CHECK(got_sig == SIGUSR1);              /* delivered on unblock */
    signal(SIGUSR1, SIG_DFL);
}

static void t_signal_default_and_ignore(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(5000);
        _exit(0);
    }
    sleep_ms(10);
    kill(pid, SIGTERM);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM && !WCOREDUMP(st));

    signal(SIGUSR2, SIG_IGN);
    CHECK(raise(SIGUSR2) == 0);             /* ignored: still alive */
    signal(SIGUSR2, SIG_DFL);
    struct sigaction act = { .sa_handler = on_sig };
    CHECK(sigaction(SIGKILL, &act, 0) == -1 && errno == EINVAL);
}

static void segv_handler(int sig)
{
    _exit(40 + sig);
}

static void t_catch_segv(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        catch(SIGSEGV, segv_handler);
        *(volatile int *)0 = 1;
        _exit(1);
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 40 + SIGSEGV);
}

static void t_sigchld(void)
{
    sig_count = 0;
    catch(SIGCHLD, on_sig);
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    waitpid(pid, 0, 0);
    CHECK(sig_count == 1 && got_sig == SIGCHLD);
    signal(SIGCHLD, SIG_DFL);
}

static void t_stop_continue(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            sleep_ms(10);
    }
    sleep_ms(20);
    kill(pid, SIGSTOP);
    int st;
    CHECK(waitpid(pid, &st, WUNTRACED) == pid);
    CHECK(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
    kill(pid, SIGCONT);
    kill(pid, SIGKILL);
    CHECK(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
}

static void t_eintr(void)
{
    got_sig = 0;
    catch(SIGUSR1, on_sig);
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        sleep_ms(30);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    unsigned long t0 = uptime_ms();
    CHECK(sleep_ms(5000) == (unsigned)-1 && errno == EINTR);
    CHECK(uptime_ms() - t0 < 1000 && got_sig == SIGUSR1);
    waitpid(pid, 0, 0);
    signal(SIGUSR1, SIG_DFL);
}

static void t_process_groups(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        for (;;)
            sleep_ms(10);
    }
    setpgid(pid, pid);                      /* both sides, like a shell */
    sleep_ms(10);
    CHECK(getpgid(pid) == pid);
    CHECK(getpgid(0) != pid);
    CHECK(getsid(0) == getsid(pid));
    CHECK(kill(-pid, SIGTERM) == 0);        /* the whole group */
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    CHECK(setsid() == -1 || 1);             /* allowed to fail if we lead a group */
}

/* ---- pipes, descriptors, environment ---- */

static void t_pipe_basic(void)
{
    int fds[2];
    char buf[16];
    CHECK(pipe(fds) == 0);
    CHECK(write(fds[1], "hello", 5) == 5);
    CHECK(read(fds[0], buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5));
    close(fds[1]);
    CHECK(read(fds[0], buf, sizeof(buf)) == 0);            /* EOF */
    CHECK(lseek(fds[0], 0, SEEK_SET) == -1 && errno == ESPIPE);
    close(fds[0]);
}

static void t_pipe_bulk_through_fork(void)
{
    int fds[2];
    pipe(fds);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        static unsigned char out[100000];
        for (unsigned i = 0; i < sizeof(out); i++)
            out[i] = (unsigned char)(i * 7);
        _exit(write(fds[1], out, sizeof(out)) == sizeof(out) ? 0 : 1);   /* blocks on full */
    }
    close(fds[1]);
    unsigned long total = 0, sum = 0, want = 0;
    unsigned char buf[3000];
    long n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            sum += buf[i] * (total + i + 1);
        total += n;
    }
    for (unsigned long i = 0; i < 100000; i++)
        want += (unsigned char)(i * 7) * (i + 1);
    int st;
    waitpid(pid, &st, 0);
    CHECK(total == 100000 && sum == want);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fds[0]);
}

static void t_pipe_epipe_and_eintr(void)
{
    int fds[2];
    got_sig = 0;
    catch(SIGPIPE, on_sig);
    pipe(fds);
    close(fds[0]);
    CHECK(write(fds[1], "x", 1) == -1 && errno == EPIPE && got_sig == SIGPIPE);
    close(fds[1]);
    signal(SIGPIPE, SIG_DFL);

    pipe(fds);
    catch(SIGUSR1, on_sig);
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        sleep_ms(30);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    char c;
    CHECK(read(fds[0], &c, 1) == -1 && errno == EINTR);    /* empty pipe, interrupted */
    waitpid(pid, 0, 0);
    close(fds[0]);
    close(fds[1]);
    signal(SIGUSR1, SIG_DFL);
}

static void t_cloexec_and_env(void)
{
    int keep = open("/dev/null", O_RDONLY);
    int drop = open("/dev/null", O_RDONLY | O_CLOEXEC);
    char k[8], d[8];
    snprintf(k, sizeof(k), "%d", keep);
    snprintf(d, sizeof(d), "%d", drop);
    char *a1[] = { "/bin/exitcode", "fdopen", k, 0 };
    char *a2[] = { "/bin/exitcode", "fdopen", d, 0 };
    CHECK(WEXITSTATUS(run(a1)) == 0);                      /* inherited */
    CHECK(WEXITSTATUS(run(a2)) == 1);                      /* closed on exec */
    CHECK(fcntl(drop, F_GETFD) == FD_CLOEXEC);
    fcntl(drop, F_SETFD, 0);
    CHECK(WEXITSTATUS(run(a2)) == 0);
    close(keep);
    close(drop);

    CHECK(getenv("PATH") != 0);
    setenv("EU_TEST", "forty two", 1);
    char *a3[] = { "/bin/exitcode", "env", "EU_TEST", "forty two", 0 };
    CHECK(WEXITSTATUS(run(a3)) == 0);                      /* environment passed on */
    unsetenv("EU_TEST");
    CHECK(WEXITSTATUS(run(a3)) == 1);
}

/* ---- terminal, permissions, time ---- */

static void t_tty(void)
{
    int fds[2];
    CHECK(isatty(0) && isatty(1));
    pipe(fds);
    CHECK(!isatty(fds[0]) && errno == ENOTTY);
    close(fds[0]);
    close(fds[1]);
    struct termios t, t2;
    CHECK(tcgetattr(0, &t) == 0 && (t.c_lflag & ICANON) && (t.c_lflag & ECHO));
    t2 = t;
    t2.c_lflag &= ~ECHO;
    CHECK(tcsetattr(0, TCSANOW, &t2) == 0);
    CHECK(tcgetattr(0, &t2) == 0 && !(t2.c_lflag & ECHO));
    tcsetattr(0, TCSANOW, &t);
    CHECK(tcgetpgrp(0) == getpgrp());                       /* we are in the foreground */
    int fd = open("/dev/tty", O_RDWR);
    CHECK(fd >= 0);
    close(fd);

    /* A background process group reading the terminal is stopped with SIGTTIN. */
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        char c;
        read(0, &c, 1);
        _exit(3);
    }
    setpgid(pid, pid);
    int st;
    CHECK(waitpid(pid, &st, WUNTRACED) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

static void t_permissions(void)
{
    CHECK(getuid() == 0 && geteuid() == 0);
    write_file("/tmp/ut_secret", "root only", O_TRUNC);
    CHECK(chmod("/tmp/ut_secret", 0600) == 0);
    struct stat st;
    CHECK(stat("/tmp/ut_secret", &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == 0);
    int old = umask(027);
    CHECK(old == 022);
    write_file("/tmp/ut_um", "x", O_TRUNC);
    CHECK(stat("/tmp/ut_um", &st) == 0 && (st.st_mode & 0777) == 0640);   /* 0644 & ~027 */
    umask(old);

    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;
        if (setuid(1000) < 0 || getuid() != 1000)
            bad |= 1;
        if (open("/tmp/ut_secret", O_RDONLY) != -1 || errno != EACCES)
            bad |= 2;                                   /* 0600 root file */
        if (write_file("/tmp/ut_guest", "mine", O_TRUNC) != 0)
            bad |= 4;                                   /* /tmp is 0777 */
        if (stat("/tmp/ut_guest", &st) || st.st_uid != 1000)
            bad |= 8;
        if (mkdir("/etc/nope", 0755) != -1 || errno != EACCES)
            bad |= 16;                                  /* /etc is root's */
        if (chmod("/tmp/ut_secret", 0777) != -1 || errno != EPERM)
            bad |= 32;                                  /* not the owner */
        if (setuid(0) != -1 || errno != EPERM)
            bad |= 64;                                  /* no way back */
        if (access("/bin/ls", X_OK) != 0)
            bad |= 128;
        _exit(bad);
    }
    int stt;
    waitpid(pid, &stt, 0);
    CHECK(WIFEXITED(stt) && WEXITSTATUS(stt) == 0);
    if (WEXITSTATUS(stt))
        printf("    permission check bits failed: %#x\n", WEXITSTATUS(stt));
    CHECK(chmod("/tmp/ut_um", 0600) == 0);
    char *argv[] = { "/tmp/ut_um", 0 };
    CHECK(execv("/tmp/ut_um", argv) == -1 && errno == EACCES);   /* no x bit, even for root */
    unlink("/tmp/ut_secret");
    unlink("/tmp/ut_um");
    unlink("/tmp/ut_guest");
}

static void t_time(void)
{
    struct timespec a, b, req = { 0, 30 * 1000000 };
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(nanosleep(&req, 0) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
    CHECK(ms >= 30 && ms < 300);
    CHECK(time(0) > 1600000000);                        /* after 2020 */
    time_t t = 951782400;                               /* 2000-02-29 00:00:00 UTC */
    struct tm tm;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 100 && tm.tm_mon == 1 && tm.tm_mday == 29 && tm.tm_wday == 2);
}

/* ---- sockets (over the loopback interface) ---- */

static struct sockaddr_in lo_addr(int port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr = { htonl(INADDR_LOOPBACK) } };
    return a;
}

/* A listening TCP socket on an ephemeral port; returns fd, port in *port. */
static int tcp_listener(int *port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_addr(0);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(s, 4) == 0);
    socklen_t len = sizeof(a);
    CHECK(getsockname(s, (struct sockaddr *)&a, &len) == 0);
    *port = ntohs(a.sin_port);
    CHECK(*port >= 49152);
    return s;
}

static void t_tcp_loopback_echo(void)
{
    int port, l = tcp_listener(&port);
    pid_t pid = fork();
    if (pid == 0) {                             /* echo server: one connection */
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept(l, (struct sockaddr *)&peer, &pl);
        if (c < 0 || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            _exit(2);
        char buf[3000];
        long n;
        while ((n = read(c, buf, sizeof(buf))) > 0)
            if (write(c, buf, (size_t)n) != n)
                _exit(3);
        close(c);
        _exit(n == 0 ? 0 : 4);
    }
    close(l);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_addr(port);
    CHECK(connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
    /* 200 KB through a 32 KB window: send and receive interleaved */
    static unsigned char out[200000], in[200000];
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = (unsigned char)(i * 7 + (i >> 9));
    size_t sent = 0, got = 0;
    while (got < sizeof(in)) {
        struct pollfd p = { s, (short)(POLLIN | (sent < sizeof(out) ? POLLOUT : 0)), 0 };
        CHECK(poll(&p, 1, 5000) == 1);
        if (p.revents & POLLOUT) {
            long n = write(s, out + sent, sizeof(out) - sent > 8192 ? 8192 : sizeof(out) - sent);
            CHECK(n > 0);
            sent += n > 0 ? (size_t)n : 0;
            if (sent == sizeof(out))
                shutdown(s, SHUT_WR);
        }
        if (p.revents & POLLIN) {
            long n = read(s, in + got, sizeof(in) - got);
            CHECK(n > 0);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
    }
    CHECK(got == sizeof(in) && !memcmp(in, out, sizeof(in)));
    char c;
    CHECK(read(s, &c, 1) == 0);                 /* server closed after our FIN */
    close(s);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void t_tcp_errors(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_addr(9);         /* nobody listens on port 9 */
    CHECK(connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == ECONNREFUSED);
    close(s);
    int port, l = tcp_listener(&port);
    int t = socket(AF_INET, SOCK_STREAM, 0);
    a = lo_addr(port);
    CHECK(bind(t, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == EADDRINUSE);
    CHECK(send(t, "x", 1, 0) < 0 && errno == ENOTCONN);
    close(t);
    /* a reset connection: EPIPE on write (SIGPIPE ignored here) */
    signal(SIGPIPE, SIG_IGN);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0);
    int srv = accept(l, 0, 0);
    CHECK(write(c, "unread", 6) == 6);
    sleep_ms(50);
    close(srv);                                 /* unread data: the server resets */
    sleep_ms(50);
    char buf[8];
    long r = read(c, buf, sizeof(buf));
    CHECK(r < 0 && errno == ECONNRESET);
    CHECK(write(c, "x", 1) < 0 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(c);
    close(l);
    CHECK(socket(AF_INET, SOCK_RAW, IPPROTO_UDP) < 0 && errno == EPROTONOSUPPORT);
    CHECK(read(-1, buf, 1) < 0 && errno == EBADF);
    int fds[2];
    pipe(fds);
    CHECK(listen(fds[0], 1) < 0 && errno == ENOTSOCK);
    close(fds[0]);
    close(fds[1]);
}

static void t_nonblocking_sockets(void)
{
    int port, l = tcp_listener(&port);
    fcntl(l, F_SETFL, O_NONBLOCK);
    CHECK(accept(l, 0, 0) < 0 && errno == EAGAIN);
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = lo_addr(port);
    CHECK(connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == EINPROGRESS);
    struct pollfd p = { s, POLLOUT, 0 };
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT));
    int err = -1;
    socklen_t el = sizeof(err);
    CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    char c;
    CHECK(recv(s, &c, 1, 0) < 0 && errno == EAGAIN);
    struct pollfd lp = { l, POLLIN, 0 };
    CHECK(poll(&lp, 1, 2000) == 1);
    int srv = accept(l, 0, 0);
    CHECK(srv >= 0);
    close(srv);
    close(s);
    close(l);
}

static void t_udp_loopback(void)
{
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in aa = lo_addr(0), ba = lo_addr(0);
    CHECK(bind(a, (struct sockaddr *)&aa, sizeof(aa)) == 0);
    socklen_t len = sizeof(aa);
    getsockname(a, (struct sockaddr *)&aa, &len);
    CHECK(sendto(b, "first", 5, 0, (struct sockaddr *)&aa, sizeof(aa)) == 5);
    CHECK(sendto(b, "second", 6, 0, (struct sockaddr *)&aa, sizeof(aa)) == 6);
    char buf[16] = "";
    struct sockaddr_in from;
    len = sizeof(from);
    CHECK(recvfrom(a, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len) == 5 &&
          !memcmp(buf, "first", 5));
    len = sizeof(ba);
    getsockname(b, (struct sockaddr *)&ba, &len);   /* sendto bound it */
    CHECK(from.sin_port == ba.sin_port && ba.sin_port != 0);
    CHECK(recv(a, buf, 3, 0) == 3 && !memcmp(buf, "sec", 3));  /* truncated datagram */
    CHECK(sendto(a, "reply", 5, 0, (struct sockaddr *)&from, sizeof(from)) == 5);
    CHECK(recv(b, buf, sizeof(buf), 0) == 5);
    close(a);
    close(b);
}

static void t_poll_and_select(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    struct pollfd p[2] = { { fds[0], POLLIN, 0 }, { fds[1], POLLOUT, 0 } };
    CHECK(poll(p, 1, 0) == 0);                  /* empty pipe: not readable */
    unsigned long t0 = uptime_ms();
    CHECK(poll(p, 1, 100) == 0);
    unsigned long dt = uptime_ms() - t0;
    CHECK(dt >= 90 && dt < 1000);
    CHECK(poll(p + 1, 1, 0) == 1 && (p[1].revents & POLLOUT));
    write(fds[1], "x", 1);
    CHECK(poll(p, 1, 0) == 1 && (p[0].revents & POLLIN));
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fds[0], &r);
    struct timeval tv = { 0, 0 };
    CHECK(select(fds[0] + 1, &r, 0, 0, &tv) == 1 && FD_ISSET(fds[0], &r));
    close(fds[1]);
    char c;
    read(fds[0], &c, 1);
    CHECK(poll(p, 1, 0) == 1 && (p[0].revents & POLLHUP));     /* writer gone */
    close(fds[0]);
    /* a sleeping poll is woken by data from another process */
    int q[2];
    pipe(q);
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        write(q[1], "y", 1);
        _exit(0);
    }
    struct pollfd w = { q[0], POLLIN, 0 };
    t0 = uptime_ms();
    CHECK(poll(&w, 1, 5000) == 1);
    CHECK(uptime_ms() - t0 < 2000);
    waitpid(pid, 0, 0);
    close(q[0]);
    close(q[1]);
}

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    { "tcp_loopback_echo", t_tcp_loopback_echo },
    { "tcp_errors", t_tcp_errors },
    { "nonblocking_sockets", t_nonblocking_sockets },
    { "udp_loopback", t_udp_loopback },
    { "poll_and_select", t_poll_and_select },
    { "tty", t_tty },
    { "permissions", t_permissions },
    { "time", t_time },
    { "pipe_basic", t_pipe_basic },
    { "pipe_bulk_through_fork", t_pipe_bulk_through_fork },
    { "pipe_epipe_and_eintr", t_pipe_epipe_and_eintr },
    { "cloexec_and_env", t_cloexec_and_env },
    { "signal_handler", t_signal_handler },
    { "signal_mask", t_signal_mask },
    { "signal_default_and_ignore", t_signal_default_and_ignore },
    { "catch_segv", t_catch_segv },
    { "sigchld", t_sigchld },
    { "stop_continue", t_stop_continue },
    { "eintr", t_eintr },
    { "process_groups", t_process_groups },
    { "file_rw", t_file_rw },
    { "big_file", t_big_file },
    { "dirs", t_dirs },
    { "link_rename", t_link_rename },
    { "paths", t_paths },
    { "fds", t_fds },
    { "devices_and_proc", t_devices_and_proc },
    { "exec_copied_program", t_exec_copied_program },
    { "fork_wait_status", t_fork_wait_status },
    { "many_children", t_many_children },
    { "cow_isolation", t_cow_isolation },
    { "exec_args", t_exec_args },
    { "exec_missing", t_exec_missing },
    { "faults_kill_with_signals", t_faults_kill_with_signals },
    { "malloc", t_malloc },
    { "mmap", t_mmap },
    { "bad_pointers_rejected", t_bad_pointers_rejected },
    { "kill", t_kill },
    { "sleep_and_ids", t_sleep_and_ids },
    { "brk", t_brk },
};

/*
 * Multi-boot scenarios driven by tools/run_tests.py:
 *   (default run)     ...also writes /home/persist.txt
 *   persist-check     the file survived a reboot
 *   crash-commit-write / -check: a crash right after the commit point; the
 *                     interrupted append must be present after recovery
 *   crash-log-write / -check: a crash before the commit point; the
 *                     interrupted append must be absent (atomicity)
 */
static int scenario(const char *mode)
{
    char buf[64];
    int ok;
    if (!strcmp(mode, "persist-check")) {
        ok = read_file("/home/persist.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "survives reboots");
        unlink("/home/persist.txt");
    } else if (!strcmp(mode, "crash-commit-write") || !strcmp(mode, "crash-log-write")) {
        int commit = mode[6] == 'c';
        const char *p = commit ? "/home/crash1.txt" : "/home/crash2.txt";
        write_file(p, "part1", O_TRUNC);
        printf("arming crash point and appending\n");
        dbg_fscrash(commit ? "after-commit" : "after-log");
        write_file(p, "part2", O_APPEND);           /* the machine stops in this commit */
        printf("still running?!\n");
        return 1;
    } else if (!strcmp(mode, "crash-commit-check")) {
        ok = read_file("/home/crash1.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "part1part2");
        printf("after recovery: \"%s\" (want \"part1part2\")\n", buf);
        unlink("/home/crash1.txt");
    } else if (!strcmp(mode, "crash-log-check")) {
        ok = read_file("/home/crash2.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "part1");
        printf("after recovery: \"%s\" (want \"part1\")\n", buf);
        unlink("/home/crash2.txt");
    } else if (!strncmp(mode, "stress", 6)) {
        /* stress[:seconds]: the SMP stress test (see stress.c) */
        char *secs = mode[6] == ':' ? (char *)mode + 7 : "8";
        char *argv[] = { "/bin/stress", "-j", "8", "-t", secs, NULL };
        ok = run(argv) == 0;
    } else {
        printf("unknown mode %s\n", mode);
        ok = 0;
    }
    printf("UTEST RESULT: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return scenario(argv[1]);
    int n = sizeof(tests) / sizeof(tests[0]);
    printf("utests: %d tests\n", n);
    for (int i = 0; i < n; i++) {
        test_failed = 0;
        printf("[ RUN      ] %s\n", tests[i].name);
        dbg_mark(tests[i].name);
        tests[i].fn();
        printf("%s %s\n", test_failed ? "[  FAILED  ]" : "[       OK ]", tests[i].name);
        failures += test_failed;
    }
    write_file("/home/persist.txt", "survives reboots", O_TRUNC);
    printf("%d tests, %d passed, %d failed\n", n, n - failures, failures);
    printf("UTEST RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
