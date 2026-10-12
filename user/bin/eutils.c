/*
 * eutils: one binary, many small tools, chosen by argv[0] (the disk image
 * has /bin/<tool> as hard links to it):
 *   echo mkdir rm rmdir touch ln mv cp wc stat head tail tee chmod chown env
 *   id true false yes kill date dmesg free sleep strace su dbg uname clear
 */
#include <errno.h>
#include <eunuchs.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *prog;

static int fail(const char *what)
{
    eprintf("%s: %s: %s\n", prog, what, strerror(errno));
    return 1;
}

static int copy(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return fail(from);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return fail(to);
    }
    char buf[4096];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) {
            rc = fail(to);
            break;
        }
    close(in);
    close(out);
    return rc;
}

static int wc_flags = 7;    /* 1 lines, 2 words, 4 bytes */

static int wc(int fd)
{
    char buf[4096];
    long n, lines = 0, words = 0, bytes = 0;
    int inword = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (long i = 0; i < n; i++) {
            lines += buf[i] == '\n';
            int sp = buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t';
            words += !sp && !inword;
            inword = !sp;
        }
    }
    if (wc_flags & 1) printf("%7ld", lines);
    if (wc_flags & 2) printf(" %7ld", words);
    if (wc_flags & 4) printf(" %7ld", bytes);
    printf("\n");
    return 0;
}

static int open_in(const char *path)
{
    if (!path || !strcmp(path, "-"))
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fail(path);
    return fd;
}

static int head(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) {
        n = atoi(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        n = atoi(argv[1] + 1);          /* -NUM, the traditional form */
        i = 2;
    }
    int fd = open_in(i < argc ? argv[i] : 0);
    if (fd < 0)
        return 1;
    char c;
    while (n > 0 && read(fd, &c, 1) == 1) {
        putchar(c);
        if (c == '\n')
            n--;
    }
    return 0;
}

static int tail(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) {
        n = atoi(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        n = atoi(argv[1] + 1);          /* -NUM, the traditional form */
        i = 2;
    }
    int fd = open_in(i < argc ? argv[i] : 0);
    if (fd < 0)
        return 1;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    long r;
    while ((r = read(fd, buf + len, cap - len)) > 0) {
        len += r;
        if (len == cap)
            buf = realloc(buf, cap *= 2);
    }
    size_t start = len;
    long lines = 0;
    while (start > 0) {
        if (buf[start - 1] == '\n' && start != len && ++lines == n)
            break;
        start--;
    }
    write(1, buf + start, len - start);
    return 0;
}

static int tee(int argc, char **argv)
{
    int fd = argc > 1 ? open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (argc > 1 && fd < 0)
        return fail(argv[1]);
    char buf[512];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
        if (fd >= 0)
            write(fd, buf, n);
    }
    return 0;
}

static int lookup_user(const char *name, int *uid, int *gid, char *home, size_t hlen,
                       char *shell, size_t slen)
{
    char buf[1024];
    int fd = open("/etc/passwd", O_RDONLY);
    long n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *f[7];
        int k = 0;
        for (char *p = line; k < 7; k++) {
            f[k] = p;
            p = strchr(p, ':');
            if (!p)
                break;
            *p++ = 0;
        }
        if (k >= 5 && (!strcmp(f[0], name) || (*name >= '0' && *name <= '9' && atoi(name) == atoi(f[2])))) {
            *uid = atoi(f[2]);
            *gid = atoi(f[3]);
            snprintf(home, hlen, "%s", k >= 5 ? f[5] : "/");
            if (shell)
                snprintf(shell, slen, "%s", k >= 6 && f[6][0] ? f[6] : "/bin/sh");
            return 0;
        }
        line = next;
    }
    return -1;
}

static const char *user_name(int uid)
{
    return uid == 0 ? "root" : uid == 1000 ? "guest" : "?";
}

static int signum(const char *s)
{
    if (*s >= '0' && *s <= '9')
        return atoi(s);
    if (!strncmp(s, "SIG", 3))
        s += 3;
    static const struct { const char *n; int s; } names[] = {
        { "HUP", 1 }, { "INT", 2 }, { "QUIT", 3 }, { "KILL", 9 }, { "USR1", 10 }, { "SEGV", 11 },
        { "USR2", 12 }, { "PIPE", 13 }, { "ALRM", 14 }, { "TERM", 15 }, { "CHLD", 17 },
        { "CONT", 18 }, { "STOP", 19 }, { "TSTP", 20 },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].n))
            return names[i].s;
    return -1;
}

/*
 * profile start [hz] | stop | report | clear | dump [host-path] | run CMD [ARGS...]
 * The kernel's sampling profiler (kernel and user stacks, all CPUs).
 * `run` profiles one command from start to finish and prints the report;
 * `dump` writes the samples to the Mac (default build/profile.bin) for
 * `make flamegraph`.
 */
static int profile_cmd(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "report";
    if (!strcmp(sub, "start"))
        return dbg_profile(1, argc > 2 ? atoi(argv[2]) : 0) ? fail("profile") : 0;
    if (!strcmp(sub, "stop"))
        return dbg_profile(0, 0);
    if (!strcmp(sub, "clear"))
        return dbg_profile(2, 0);
    if (!strcmp(sub, "dump")) {
        const char *path = argc > 2 ? argv[2] : "build/profile.bin";
        if (dbg_profile(3, (long)path))
            return fail("profile dump (semihosting not enabled?)");
        printf("samples written to %s on the host; run `make flamegraph` there\n", path);
        return 0;
    }
    if (!strcmp(sub, "run") && argc > 2) {
        dbg_profile(2, 0);
        dbg_profile(1, 0);
        pid_t pid = fork();
        if (pid == 0) {
            execvp(argv[2], argv + 2);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        dbg_profile(0, 0);
        argv[1] = "report";
        argc = 2;
        sub = "report";
    }
    if (!strcmp(sub, "report")) {
        int fd = open("/proc/profile", O_RDONLY);
        char buf[1024];
        long n;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        return 0;
    }
    printf("usage: profile start [hz] | stop | report | clear | dump [host-path] | run CMD...\n");
    return 2;
}

/* crc32 [file...]: CRC-32 (as zlib/Python's zlib.crc32) and length; stdin without args */
static int crc32_cmd(int argc, char **argv)
{
    static unsigned table[256];
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    int rc = 0;
    for (int i = argc > 1 ? 1 : 0; i < argc; i++) {
        int fd = argc > 1 ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) {
            rc = fail(argv[i]);
            continue;
        }
        unsigned crc = 0xffffffffu;
        unsigned long total = 0;
        static unsigned char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (long k = 0; k < n; k++)
                crc = table[(crc ^ buf[k]) & 255] ^ (crc >> 8);
            total += (unsigned long)n;
        }
        printf("%08x %lu%s%s\n", ~crc, total, argc > 1 ? " " : "", argc > 1 ? argv[i] : "");
        if (fd)
            close(fd);
    }
    return rc;
}

int main(int argc, char **argv)
{
    prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int rc = 0;

    if (!strcmp(prog, "head"))
        return head(argc, argv);
    if (!strcmp(prog, "tail"))
        return tail(argc, argv);
    if (!strcmp(prog, "tee"))
        return tee(argc, argv);
    if (!strcmp(prog, "true"))
        return 0;
    if (!strcmp(prog, "false"))
        return 1;
    if (!strcmp(prog, "yes")) {
        for (;;)
            printf("%s\n", argc > 1 ? argv[1] : "y");
    }
    if (!strcmp(prog, "crc32"))
        return crc32_cmd(argc, argv);
    if (!strcmp(prog, "profile"))
        return profile_cmd(argc, argv);
    if (!strcmp(prog, "uname")) {
        printf("Eunuchs 0.1 riscv64\n");
        return 0;
    }
    if (!strcmp(prog, "clear")) {
        printf("\033[H\033[2J");
        return 0;
    }
    if (!strcmp(prog, "dmesg") || !strcmp(prog, "free")) {
        const char *path = prog[0] == 'd' ? "/dev/kmsg" : "/proc/meminfo";
        int fd = open(path, O_RDONLY);
        char buf[1024];
        long n;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        return fd < 0 ? fail(path) : 0;
    }
    if (!strcmp(prog, "sleep")) {
        if (argc < 2) {
            eprintf("usage: sleep SECONDS[.fraction]\n");
            return 2;
        }
        char *dot = strchr(argv[1], '.');
        long ms = atoi(argv[1]) * 1000L;
        if (dot) {
            long frac = 0, scale = 100;
            for (char *p = dot + 1; *p >= '0' && *p <= '9' && scale; p++, scale /= 10)
                frac += (*p - '0') * scale;
            ms += frac;
        }
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
        return nanosleep(&ts, 0) < 0;
    }
    if (!strcmp(prog, "date")) {
        static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
                                      "Sep", "Oct", "Nov", "Dec" };
        time_t t = time(0);
        struct tm tm;
        gmtime_r(&t, &tm);
        printf("%s %s %2d %02d:%02d:%02d UTC %d\n", days[tm.tm_wday], mons[tm.tm_mon], tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
        return 0;
    }
    if (!strcmp(prog, "id")) {
        printf("uid=%d(%s) gid=%d euid=%d egid=%d\n", getuid(), user_name(getuid()), getgid(),
               geteuid(), getegid());
        return 0;
    }
    if (!strcmp(prog, "env")) {
        int i = 1;
        for (; i < argc && strchr(argv[i], '='); i++) {
            char *eq = strchr(argv[i], '=');
            *eq = 0;
            setenv(argv[i], eq + 1, 1);
        }
        if (i == argc) {
            for (char **e = environ; *e; e++)
                printf("%s\n", *e);
            return 0;
        }
        execvp(argv[i], argv + i);
        return fail(argv[i]);
    }
    if (!strcmp(prog, "strace")) {
        if (argc < 2) {
            eprintf("usage: strace CMD [ARGS...]   (output goes to the kernel log/console)\n");
            return 2;
        }
        dbg_strace(1);
        execvp(argv[1], argv + 1);
        return fail(argv[1]);
    }
    if (!strcmp(prog, "su")) {
        int uid, gid;
        char home[128];
        const char *who = argc > 1 ? argv[1] : "root";
        char shell[128];
        if (lookup_user(who, &uid, &gid, home, sizeof(home), shell, sizeof(shell)) < 0) {
            eprintf("su: no user %s in /etc/passwd\n", who);
            return 1;
        }
        if (setgid(gid) < 0 || setuid(uid) < 0)
            return fail("setuid (only root may switch users)");
        chdir(home);
        setenv("HOME", home, 1);
        setenv("USER", who, 1);
        setenv("LOGNAME", who, 1);
        setenv("SHELL", shell, 1);
        unsetenv("HISTFILE");
        char *sh[] = { shell, 0 };
        execv(sh[0], sh);
        return fail(shell);
    }
    if (!strcmp(prog, "kill")) {
        int sig = SIGTERM, i = 1;
        if (argc > 1 && !strcmp(argv[1], "-l")) {
            for (int s = 1; s < 32; s++)
                if (strcmp(strsignal(s), "Unknown signal"))
                    printf("%2d %s\n", s, strsignal(s));
            return 0;
        }
        if (argc > 1 && argv[1][0] == '-') {
            sig = signum(argv[1] + 1);
            i = 2;
            if (sig < 0) {
                eprintf("kill: unknown signal %s\n", argv[1]);
                return 2;
            }
        }
        if (i == argc) {
            eprintf("usage: kill [-SIG] PID...   (negative PID: a process group)\n");
            return 2;
        }
        for (; i < argc; i++) {
            char *end;
            long pid = strtol(argv[i], &end, 10);
            if (*end || end == argv[i]) {
                eprintf("kill: %s: not a process id (job specs like %%1 need the shell's kill)\n",
                        argv[i]);
                rc = 1;
            } else if (kill((int)pid, sig) < 0) {
                rc = fail(argv[i]);
            }
        }
        return rc;
    }
    if (!strcmp(prog, "chmod") && argc > 2) {
        int mode = (int)strtol(argv[1], 0, 8);
        for (int i = 2; i < argc; i++)
            if (chmod(argv[i], mode) < 0)
                rc = fail(argv[i]);
        return rc;
    }
    if (!strcmp(prog, "chown") && argc > 2) {
        int uid, gid = -1;
        char home[128], *colon = strchr(argv[1], ':');
        if (colon) {
            *colon = 0;
            gid = atoi(colon + 1);
        }
        int g;
        if (lookup_user(argv[1], &uid, &g, home, sizeof(home), 0, 0) < 0) {
            eprintf("chown: unknown user %s\n", argv[1]);
            return 1;
        }
        for (int i = 2; i < argc; i++)
            if (chown(argv[i], uid, gid) < 0)
                rc = fail(argv[i]);
        return rc;
    }
    if (!strcmp(prog, "dbg")) {
        const char *op = argc > 1 ? argv[1] : "";
        if (!strcmp(op, "break"))
            dbg_break();
        else if (!strcmp(op, "mark"))
            dbg_mark(argc > 2 ? argv[2] : "mark");
        else if (!strcmp(op, "vm"))
            dbg_vm();
        else if (!strcmp(op, "files"))
            dbg_files();
        else if (!strcmp(op, "ps"))
            dbg_ps();
        else if (!strcmp(op, "fscrash") && argc > 2)
            return dbg_fscrash(argv[2]) < 0 ? fail("fscrash") : 0;
        else {
            eprintf("usage: dbg break | mark TEXT | vm | files | ps | fscrash POINT\n");
            return 2;
        }
        return 0;
    }

    if (!strcmp(prog, "echo")) {
        for (int i = 1; i < argc; i++)
            printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        printf("\n");
    } else if (!strcmp(prog, "mkdir")) {
        for (int i = 1; i < argc; i++)
            if (mkdir(argv[i], 0755) < 0)
                rc = fail(argv[i]);
    } else if (!strcmp(prog, "rm")) {
        for (int i = 1; i < argc; i++)
            if (unlink(argv[i]) < 0)
                rc = fail(argv[i]);
    } else if (!strcmp(prog, "rmdir")) {
        for (int i = 1; i < argc; i++)
            if (rmdir(argv[i]) < 0)
                rc = fail(argv[i]);
    } else if (!strcmp(prog, "touch")) {
        int i = 1, no_create = 0;
        if (i < argc && !strcmp(argv[i], "-c"))
            no_create = 1, i++;
        for (; i < argc; i++) {
            if (utimensat(AT_FDCWD, argv[i], 0, 0) == 0)
                continue;               /* it exists: now it's new */
            if (errno != ENOENT || no_create) {
                if (errno != ENOENT)
                    rc = fail(argv[i]);
                continue;
            }
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
            if (fd < 0)
                rc = fail(argv[i]);
            else
                close(fd);
        }
    } else if (!strcmp(prog, "ln") && argc == 3) {
        if (link(argv[1], argv[2]) < 0)
            rc = fail(argv[2]);
    } else if (!strcmp(prog, "mv") && argc == 3) {
        if (rename(argv[1], argv[2]) < 0)
            rc = fail(argv[1]);
    } else if (!strcmp(prog, "cp") && argc == 3) {
        rc = copy(argv[1], argv[2]);
    } else if (!strcmp(prog, "wc")) {
        int i = 1;
        if (argc > 1 && argv[1][0] == '-') {
            wc_flags = 0;
            for (char *f = argv[1] + 1; *f; f++)
                wc_flags |= *f == 'l' ? 1 : *f == 'w' ? 2 : *f == 'c' ? 4 : 0;
            i = 2;
        }
        if (i == argc)
            return wc(0);
        for (; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0)
                rc = fail(argv[i]);
            else {
                wc(fd);
                close(fd);
            }
        }
    } else if (!strcmp(prog, "stat")) {
        for (int i = 1; i < argc; i++) {
            struct stat st;
            if (stat(argv[i], &st) < 0) {
                rc = fail(argv[i]);
                continue;
            }
            printf("%s: inode %u, %s, %lu bytes, %lu blocks, %u links, mode %o\n", argv[i],
                   st.st_ino, S_ISDIR(st.st_mode) ? "directory" : S_ISCHR(st.st_mode) ? "device" : "file",
                   (unsigned long)st.st_size, (unsigned long)st.st_blocks, st.st_nlink,
                   st.st_mode & 07777);
        }
    } else {
        eprintf("eutils: unknown tool '%s'\n", prog);
        return 2;
    }
    return rc;
}
