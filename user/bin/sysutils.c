/*
 * sysutils: process, user and system tools in one binary, chosen by argv[0]
 * (the disk image has /bin/<tool> as hard links to it):
 *   timeout nohup time whoami groups hostname tty printenv uptime cal
 *   pgrep pkill pidof killall watch
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static const char *prog;

static void die_usage(const char *usage)
{
    eprintf("usage: %s %s\n", prog, usage);
    exit(prog[0] == 't' && prog[1] == 'i' && prog[2] == 'm' && prog[3] == 'e' && prog[4] == 'o' ? 125 : 2);
}

static long read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = 0, r;
    while (n < (long)size - 1 && (r = read(fd, buf + n, size - 1 - n)) > 0)
        n += r;
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

/* ---- signals by name ---- */

static const struct { const char *name; int num; } signals[] = {
#define X(name, num, act, desc) { #name, num },
    SIGNALS(X)
#undef X
};

static int sig_number(const char *s)
{
    if (isdigit(*s))
        return atoi(s);
    if (!strncasecmp(s, "SIG", 3))
        s += 3;
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        if (!strcasecmp(signals[i].name + 3, s))
            return signals[i].num;
    return -1;
}

static const char *sig_name(int n)
{
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        if (signals[i].num == n)
            return signals[i].name + 3;
    return "?";
}

/* "1.5", "2m", "1h", "1d": seconds as milliseconds, or -1 */
static long duration_ms(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return -1;
    double mul = 1;
    if (*end == 's') mul = 1, end++;
    else if (*end == 'm') mul = 60, end++;
    else if (*end == 'h') mul = 3600, end++;
    else if (*end == 'd') mul = 86400, end++;
    if (*end)
        return -1;
    return (long)(v * mul * 1000 + 0.5);
}

/* ================================================================ timeout */

static pid_t child;
static volatile int got_sig;

static void forward(int sig)
{
    got_sig = sig;
    if (child > 0)
        kill(child, sig);
}

static int status_of(int st)
{
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int timeout_main(int argc, char **argv)
{
    int sig = SIGTERM, preserve = 0, i = 1;
    long kill_after = -1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--preserve-status")) preserve = 1;
        else if (!strcmp(argv[i], "--foreground")) ;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            if ((sig = sig_number(argv[++i])) <= 0) {
                eprintf("timeout: %s: invalid signal\n", argv[i]);
                return 125;
            }
        } else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
            if ((kill_after = duration_ms(argv[++i])) < 0) {
                eprintf("timeout: invalid time interval '%s'\n", argv[i]);
                return 125;
            }
        } else
            die_usage("[-s SIGNAL] [-k DURATION] [--preserve-status] DURATION COMMAND [ARG...]");
    }
    if (argc - i < 2)
        die_usage("[-s SIGNAL] [-k DURATION] [--preserve-status] DURATION COMMAND [ARG...]");
    long ms = duration_ms(argv[i]);
    if (ms < 0) {
        eprintf("timeout: invalid time interval '%s'\n", argv[i]);
        return 125;
    }
    signal(SIGINT, forward);
    signal(SIGTERM, forward);
    signal(SIGHUP, forward);
    child = fork();
    if (child < 0) {
        eprintf("timeout: fork: %s\n", strerror(errno));
        return 125;
    }
    if (child == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        execvp(argv[i + 1], argv + i + 1);
        int e = errno;
        eprintf("timeout: failed to run command '%s': %s\n", argv[i + 1], strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    unsigned long start = uptime_ms(), deadline = start + ms;
    int timed_out = 0, st;
    for (;;) {
        pid_t w = waitpid(child, &st, WNOHANG);
        if (w == child)
            break;
        unsigned long now = uptime_ms();
        if (ms && !timed_out && now >= deadline) {
            timed_out = 1;
            kill(child, sig);
            if (sig != SIGKILL && sig != SIGCONT)
                kill(child, SIGCONT);
            if (kill_after >= 0)
                deadline = now + kill_after;
            continue;
        }
        if (timed_out && kill_after >= 0 && now >= deadline) {
            kill(child, SIGKILL);
            kill_after = -1;
        }
        sleep_ms(10);
    }
    if (timed_out && !preserve)
        return 124;
    return status_of(st);
}

/* ================================================================ nohup */

static int nohup_main(int argc, char **argv)
{
    if (argc < 2)
        die_usage("COMMAND [ARG...]");
    int first = argc > 2 && !strcmp(argv[1], "--") ? 2 : 1;
    if (isatty(0)) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, 0);
            close(fd);
        }
    }
    int out_tty = isatty(1);
    if (out_tty) {
        const char *name = "nohup.out";
        char alt[512];
        int fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0 && getenv("HOME")) {
            snprintf(alt, sizeof(alt), "%s/nohup.out", getenv("HOME"));
            name = alt;
            fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0600);
        }
        if (fd < 0) {
            eprintf("nohup: failed to open '%s': %s\n", name, strerror(errno));
            return 125;
        }
        eprintf("nohup: ignoring input and appending output to '%s'\n", name);
        dup2(fd, 1);
        close(fd);
    }
    if (isatty(2))
        dup2(1, 2);
    signal(SIGHUP, SIG_IGN);
    execvp(argv[first], argv + first);
    int e = errno;
    eprintf("nohup: failed to run command '%s': %s\n", argv[first], strerror(e));
    return e == ENOENT ? 127 : 126;
}

/* ================================================================ time */

static void fmt_secs(const char *label, unsigned long ms, int posix)
{
    if (posix)
        eprintf("%s %lu.%02lu\n", label, ms / 1000, ms % 1000 / 10);
    else
        eprintf("%s\t%lum%lu.%03lus\n", label, ms / 60000, ms / 1000 % 60, ms % 1000);
}

static int time_main(int argc, char **argv)
{
    int i = 1, posix = 0;
    if (i < argc && !strcmp(argv[i], "-p"))
        posix = 1, i++;
    if (i >= argc)
        die_usage("[-p] COMMAND [ARG...]");
    unsigned long t0 = uptime_ms();
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        eprintf("time: fork: %s\n", strerror(errno));
        return 125;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        execvp(argv[i], argv + i);
        int e = errno;
        eprintf("time: cannot run %s: %s\n", argv[i], strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    /* No rusage from wait4: sample the child's CPU time in /proc while it
       runs (its own time only, not its children's). */
    char path[64], buf[2048];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    unsigned long cpu = 0, poll = 1;
    int st;
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (read_file(path, buf, sizeof(buf)) > 0) {
            char *c = strstr(buf, "\ncpu_ms ");
            if (c)
                cpu = strtoul(c + 8, 0, 10);
        }
        sleep_ms(poll);
        if (poll < 20)
            poll *= 2;
    }
    unsigned long real = uptime_ms() - t0;
    if (!posix)
        eprintf("\n");
    fmt_secs("real", real, posix);
    fmt_secs("cpu ", cpu, posix);
    if (WIFSIGNALED(st))
        eprintf("time: command terminated by signal %d\n", WTERMSIG(st));
    return status_of(st);
}

/* ================================================================ users */

static const char *user_name(unsigned uid)
{
    static char b[16];
    struct passwd *pw = getpwuid(uid);
    if (pw)
        return pw->pw_name;
    snprintf(b, sizeof(b), "%u", uid);
    return b;
}

static int whoami_main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
        die_usage("");
    puts(user_name(geteuid()));
    return 0;
}

static int groups_main(int argc, char **argv)
{
    int rc = 0;
    for (int i = argc > 1 ? 1 : 0; i < argc; i++) {
        const char *name;
        unsigned gid;
        char who[64];
        if (argc > 1) {
            struct passwd *pw = getpwnam(argv[i]);
            if (!pw) {
                eprintf("groups: '%s': no such user\n", argv[i]);
                rc = 1;
                continue;
            }
            snprintf(who, sizeof(who), "%s", pw->pw_name);
            gid = pw->pw_gid;
            name = who;
            printf("%s : ", name);
        } else {
            snprintf(who, sizeof(who), "%s", user_name(geteuid()));
            name = who;
            gid = getegid();
        }
        struct group *g = getgrgid(gid);
        if (g)
            printf("%s", g->gr_name);
        else
            printf("%u", gid);
        setgrent();
        while ((g = getgrent())) {
            if (g->gr_gid == gid)
                continue;
            for (char **m = g->gr_mem; *m; m++)
                if (!strcmp(*m, name)) {
                    printf(" %s", g->gr_name);
                    break;
                }
        }
        endgrent();
        putchar('\n');
    }
    return rc;
}

static int hostname_main(int argc, char **argv)
{
    int i = 1, shortname = 0;
    if (i < argc && !strcmp(argv[i], "-s"))
        shortname = 1, i++;
    if (i < argc) {
        /* setting it: /etc/hostname is where the name lives */
        const char *name = argv[i];
        if (!*name || strlen(name) > 63 || strchr(name, '/')) {
            eprintf("hostname: invalid name '%s'\n", name);
            return 1;
        }
        int fd = open("/etc/hostname", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            eprintf("hostname: /etc/hostname: %s\n", strerror(errno));
            return 1;
        }
        dprintf(fd, "%s\n", name);
        close(fd);
        return 0;
    }
    char name[256];
    if (gethostname(name, sizeof(name)) < 0) {
        eprintf("hostname: %s\n", strerror(errno));
        return 1;
    }
    if (shortname && strchr(name, '.'))
        *strchr(name, '.') = 0;
    puts(name);
    return 0;
}

static int tty_main(int argc, char **argv)
{
    int silent = argc > 1 && !strcmp(argv[1], "-s");
    char *t = ttyname(0);
    if (!silent)
        puts(t ? t : "not a tty");
    return t ? 0 : 1;
}

static int printenv_main(int argc, char **argv)
{
    if (argc < 2) {
        for (char **e = environ; *e; e++)
            puts(*e);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (v)
            puts(v);
        else
            rc = 1;
    }
    return rc;
}

/* ================================================================ uptime */

static long stat_field(const char *buf, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : 0)
        if (!strncmp(p, key, kl) && p[kl] == ' ')
            return atol(p + kl + 1);
    return -1;
}

static int uptime_main(int argc, char **argv)
{
    int pretty = 0, since = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) pretty = 1;
        else if (!strcmp(argv[i], "-s")) since = 1;
        else die_usage("[-p | -s]");
    }
    unsigned long secs = uptime_ms() / 1000;
    time_t now = time(0);
    if (since) {
        time_t boot = now - secs;
        char b[32];
        strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", localtime(&boot));
        puts(b);
        return 0;
    }
    unsigned long d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60;
    if (pretty) {
        printf("up");
        int any = 0;
        if (d) printf(" %lu day%s", d, d == 1 ? "" : "s"), any = 1;
        if (h) printf("%s %lu hour%s", any ? "," : "", h, h == 1 ? "" : "s"), any = 1;
        if (m || !any) printf("%s %lu minute%s", any ? "," : "", m, m == 1 ? "" : "s");
        putchar('\n');
        return 0;
    }
    char clock[16], buf[2048];
    strftime(clock, sizeof(clock), "%H:%M:%S", localtime(&now));
    printf(" %s up ", clock);
    if (d)
        printf("%lu day%s, ", d, d == 1 ? "" : "s");
    if (h)
        printf("%2lu:%02lu, ", h, m);
    else
        printf("%lu min, ", m);
    if (read_file("/proc/stat", buf, sizeof(buf)) > 0)
        printf(" %ld processes, %ld running, %ld cpu%s\n", stat_field(buf, "procs"),
               stat_field(buf, "procs_running"), stat_field(buf, "cpus"),
               stat_field(buf, "cpus") == 1 ? "" : "s");
    else
        putchar('\n');
    return 0;
}

/* ================================================================ cal */

static const char *const month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December" };

static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int m, int y)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && leap(y));
}

/* 0 = Sunday (proleptic Gregorian) */
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* The 8 lines of a month (title, weekdays, 6 weeks), each 20 wide. */
static void month_lines(int m, int y, int with_year, int today, char lines[8][64])
{
    char title[32];
    if (with_year)
        snprintf(title, sizeof(title), "%s %d", month_names[m - 1], y);
    else
        snprintf(title, sizeof(title), "%s", month_names[m - 1]);
    int pad = (20 - (int)strlen(title)) / 2;
    snprintf(lines[0], 64, "%*s%s%*s", pad, "", title, 20 - pad - (int)strlen(title), "");
    snprintf(lines[1], 64, "Su Mo Tu We Th Fr Sa");
    int first = weekday(y, m, 1), n = month_days(m, y), day = 1;
    for (int w = 0; w < 6; w++) {
        char *l = lines[2 + w];
        int o = 0;
        for (int dow = 0; dow < 7; dow++) {
            int show = (w > 0 || dow >= first) && day <= n;
            if (show && day == today)
                o += snprintf(l + o, 64 - o, "%s\033[7m%2d\033[0m", dow ? " " : "", day++);
            else if (show)
                o += snprintf(l + o, 64 - o, "%s%2d", dow ? " " : "", day++);
            else
                o += snprintf(l + o, 64 - o, "%s  ", dow ? " " : "");
        }
    }
}

static void put_trimmed(const char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        n--;
    fwrite(s, 1, n, stdout);
    putchar('\n');
}

static int cal_main(int argc, char **argv)
{
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, whole_year = 0;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-y"))
        whole_year = 1, i++;
    if (argc - i == 1) {
        y = atoi(argv[i]);
        whole_year = 1;
    } else if (argc - i == 2) {
        m = atoi(argv[i]);
        y = atoi(argv[i + 1]);
    } else if (argc - i != 0) {
        die_usage("[-y] [[MONTH] YEAR]");
    }
    if (y < 1 || y > 9999 || m < 1 || m > 12) {
        eprintf("cal: invalid date\n");
        return 1;
    }
    int hl = isatty(1);
    int cur_y = tm.tm_year + 1900, cur_m = tm.tm_mon + 1;
    char lines[3][8][64];
    if (!whole_year) {
        month_lines(m, y, 1, hl && y == cur_y && m == cur_m ? tm.tm_mday : 0, lines[0]);
        for (int l = 0; l < 8; l++)
            put_trimmed(lines[0][l]);
        return 0;
    }
    char title[16];
    snprintf(title, sizeof(title), "%d", y);
    printf("%*s\n\n", 32 + (int)strlen(title) / 2, title);
    for (int q = 0; q < 4; q++) {
        for (int k = 0; k < 3; k++) {
            int mm = q * 3 + k + 1;
            month_lines(mm, y, 0, hl && y == cur_y && mm == cur_m ? tm.tm_mday : 0, lines[k]);
        }
        for (int l = 0; l < 8; l++) {
            char row[256];
            snprintf(row, sizeof(row), "%s  %s  %s", lines[0][l], lines[1][l], lines[2][l]);
            put_trimmed(row);
        }
        if (q < 3)
            putchar('\n');
    }
    return 0;
}

/* ================================================================ pgrep, pkill, pidof, killall */

struct proc {
    int pid, ppid, uid;
    char name[32];
    char cmd[512];                      /* argv, space separated */
};

static struct proc *procs;
static int nprocs;

static void load_procs(void)
{
    DIR *d = opendir("/proc");
    if (!d) {
        eprintf("%s: /proc: %s\n", prog, strerror(errno));
        exit(3);
    }
    struct dirent *e;
    int cap = 0;
    char path[64], buf[2048];
    while ((e = readdir(d))) {
        if (!isdigit(e->d_name[0]))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        if (read_file(path, buf, sizeof(buf)) <= 0)
            continue;
        if (strstr(buf, "\nstate zombie"))
            continue;
        if (nprocs == cap) {
            cap = cap ? cap * 2 : 64;
            procs = realloc(procs, cap * sizeof(*procs));
        }
        struct proc *p = &procs[nprocs];
        memset(p, 0, sizeof(*p));
        p->pid = atoi(e->d_name);
        p->ppid = stat_field(buf, "ppid");
        char *u = strstr(buf, "\nuid ");
        if (u) {
            char *eu = strstr(u, " euid ");
            p->uid = eu ? atoi(eu + 6) : atoi(u + 5);
        }
        char *nm = strstr(buf, "\nname ");
        if (nm) {
            nm += 6;
            size_t l = strcspn(nm, "\n");
            snprintf(p->name, sizeof(p->name), "%.*s", (int)l, nm);
        }
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        long n = read_file(path, p->cmd, sizeof(p->cmd));
        while (n > 0 && !p->cmd[n - 1])
            n--;
        for (long k = 0; k < n; k++)
            if (!p->cmd[k])
                p->cmd[k] = ' ';
        if (n <= 0)
            snprintf(p->cmd, sizeof(p->cmd), "%s", p->name);
        nprocs++;
    }
    closedir(d);
}

/* argv[0]'s last component, as pidof and killall match it */
static const char *cmd_base(const struct proc *p, char *b, size_t n)
{
    size_t l = strcspn(p->cmd, " ");
    snprintf(b, n, "%.*s", (int)l, p->cmd);
    char *s = strrchr(b, '/');
    return s ? s + 1 : b;
}

static int pgrep_main(int argc, char **argv)
{
    int is_kill = !strcmp(prog, "pkill");
    int list = 0, full = 0, exact = 0, icase = 0, newest = 0, oldest = 0, invert = 0, count = 0, echo = 0;
    int sig = SIGTERM, uid = -1, ppid = -1, i = 1;
    const char *delim = "\n";
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (is_kill && (isdigit(a[1]) || isupper(a[1]))) {
            if ((sig = sig_number(a + 1)) < 0) {
                eprintf("pkill: unknown signal %s\n", a + 1);
                return 2;
            }
            continue;
        }
        for (const char *f = a + 1; *f; f++) {
            switch (*f) {
            case 'l': list = 1; break;
            case 'a': list = 2; break;
            case 'f': full = 1; break;
            case 'x': exact = 1; break;
            case 'i': icase = 1; break;
            case 'n': newest = 1; break;
            case 'o': oldest = 1; break;
            case 'v': invert = 1; break;
            case 'c': count = 1; break;
            case 'e': echo = 1; break;
            case 'd':
            case 'u':
            case 'P':
            case 's': {
                const char *v = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : 0;
                if (!v)
                    die_usage("[-flaxinovc] [-d DELIM] [-u USER] [-P PPID] PATTERN");
                if (*f == 'd')
                    delim = v;
                else if (*f == 'P')
                    ppid = atoi(v);
                else if (*f == 's') {
                    if ((sig = sig_number(v)) < 0) {
                        eprintf("pkill: unknown signal %s\n", v);
                        return 2;
                    }
                } else {
                    struct passwd *pw = getpwnam(v);
                    uid = pw ? (int)pw->pw_uid : atoi(v);
                }
                f += strlen(f) - 1;
                break;
            }
            default:
                die_usage(is_kill ? "[-SIGNAL] [-flxinoe] [-u USER] [-P PPID] PATTERN"
                                  : "[-flaxinovc] [-d DELIM] [-u USER] [-P PPID] PATTERN");
            }
        }
    }
    if (i + 1 != argc && !(i == argc && (uid >= 0 || ppid >= 0)))
        die_usage(is_kill ? "[-SIGNAL] [-flxinoe] [-u USER] [-P PPID] PATTERN"
                          : "[-flaxinovc] [-d DELIM] [-u USER] [-P PPID] PATTERN");
    regex_t re;
    int have_re = i < argc;
    if (have_re) {
        char pat[1024];
        snprintf(pat, sizeof(pat), exact ? "^(%s)$" : "%s", argv[i]);
        if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0))) {
            eprintf("%s: invalid pattern '%s'\n", prog, argv[i]);
            return 2;
        }
    }
    load_procs();
    int self = getpid(), parent = getppid();
    int *hits = calloc(nprocs + 1, sizeof(int)), nhits = 0;
    for (int k = 0; k < nprocs; k++) {
        struct proc *p = &procs[k];
        if (p->pid == self)
            continue;
        if (is_kill && p->pid == parent)
            continue;                   /* never the shell that ran us */
        int m = 1;
        if (have_re)
            m = regexec(&re, full ? p->cmd : p->name, 0, 0, 0) == 0;
        if (uid >= 0 && p->uid != uid)
            m = 0;
        if (ppid >= 0 && p->ppid != ppid)
            m = 0;
        if (m != invert)
            hits[nhits++] = k;
    }
    if ((newest || oldest) && nhits) {
        int best = hits[0];
        for (int k = 1; k < nhits; k++)
            if (newest ? procs[hits[k]].pid > procs[best].pid : procs[hits[k]].pid < procs[best].pid)
                best = hits[k];
        hits[0] = best;
        nhits = 1;
    }
    if (count) {
        printf("%d\n", nhits);
        return nhits ? 0 : 1;
    }
    int rc = nhits ? 0 : 1;
    for (int k = 0; k < nhits; k++) {
        struct proc *p = &procs[hits[k]];
        if (is_kill) {
            if (kill(p->pid, sig) < 0) {
                eprintf("pkill: killing pid %d failed: %s\n", p->pid, strerror(errno));
                continue;
            }
            if (echo)
                printf("%s killed (pid %d)\n", p->name, p->pid);
            continue;
        }
        if (k)
            fputs(delim, stdout);
        if (list == 2)
            printf("%d %s", p->pid, p->cmd);
        else if (list)
            printf("%d %s", p->pid, p->name);
        else
            printf("%d", p->pid);
    }
    if (!is_kill && nhits)
        putchar('\n');
    return rc;
}

static int pidof_main(int argc, char **argv)
{
    int single = 0, i = 1, found = 0;
    const char *omit = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-s")) single = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) omit = argv[++i];
        else die_usage("[-s] [-o PID] NAME...");
    }
    if (i >= argc)
        die_usage("[-s] [-o PID] NAME...");
    load_procs();
    int self = getpid();
    for (; i < argc; i++) {
        /* newest first, like Linux */
        for (int k = nprocs - 1; k >= 0; k--) {
            struct proc *p = &procs[k];
            char b[512];
            if (p->pid == self || (omit && p->pid == atoi(omit)))
                continue;
            if (strcmp(p->name, argv[i]) && strcmp(cmd_base(p, b, sizeof(b)), argv[i]))
                continue;
            printf("%s%d", found ? " " : "", p->pid);
            found = 1;
            if (single)
                break;
        }
        if (single && found)
            break;
    }
    if (found)
        putchar('\n');
    return found ? 0 : 1;
}

static int killall_main(int argc, char **argv)
{
    int sig = SIGTERM, quiet = 0, verbose = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-l")) {
            for (size_t k = 0; k < sizeof(signals) / sizeof(signals[0]); k++)
                printf("%s%s", signals[k].name + 3, k + 1 < sizeof(signals) / sizeof(signals[0]) ? " " : "\n");
            return 0;
        }
        if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-s") && i + 1 < argc) {
            if ((sig = sig_number(argv[++i])) < 0) {
                eprintf("killall: unknown signal %s\n", argv[i]);
                return 1;
            }
        } else if ((sig = sig_number(a + 1)) < 0) {
            eprintf("killall: unknown signal %s\n", a + 1);
            return 1;
        }
    }
    if (i >= argc)
        die_usage("[-q] [-v] [-SIGNAL | -s SIGNAL] NAME...  |  -l");
    load_procs();
    int self = getpid(), rc = 0;
    for (; i < argc; i++) {
        int n = 0;
        for (int k = 0; k < nprocs; k++) {
            struct proc *p = &procs[k];
            char b[512];
            if (p->pid == self)
                continue;
            if (strcmp(p->name, argv[i]) && strcmp(cmd_base(p, b, sizeof(b)), argv[i]))
                continue;
            if (kill(p->pid, sig) < 0) {
                if (!quiet)
                    eprintf("killall: %s(%d): %s\n", argv[i], p->pid, strerror(errno));
                continue;
            }
            if (verbose)
                printf("Killed %s(%d) with signal %d (%s)\n", argv[i], p->pid, sig, sig_name(sig));
            n++;
        }
        if (!n) {
            if (!quiet)
                eprintf("%s: no process found\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* ================================================================ watch */

static volatile int stop_watch;

static void on_int(int sig)
{
    (void)sig;
    stop_watch = 1;
}

static int watch_main(int argc, char **argv)
{
    long interval = 2000;
    int no_title = 0, i = 1, exit_on_change = 0, count = -1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "-t")) no_title = 1;
        else if (!strcmp(argv[i], "-g")) exit_on_change = 1;
        else if (!strncmp(argv[i], "-n", 2) && (argv[i][2] || i + 1 < argc)) {
            const char *v = argv[i][2] ? argv[i] + 2 : argv[++i];
            interval = duration_ms(v);
            if (interval < 0) {
                eprintf("watch: bad interval '%s'\n", v);
                return 1;
            }
            if (interval < 100)
                interval = 100;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            count = atoi(argv[++i]);    /* Eunuchs: stop after COUNT runs */
        } else
            die_usage("[-t] [-g] [-n SECS] [-c COUNT] COMMAND [ARG...]");
    }
    if (i >= argc)
        die_usage("[-t] [-g] [-n SECS] [-c COUNT] COMMAND [ARG...]");
    char cmd[2048];
    size_t o = 0;
    for (int k = i; k < argc && o < sizeof(cmd) - 1; k++)
        o += snprintf(cmd + o, sizeof(cmd) - o, "%s%s", k > i ? " " : "", argv[k]);
    signal(SIGINT, on_int);
    char host[64];
    gethostname(host, sizeof(host));
    char *prev = 0;
    for (int run = 0; !stop_watch && (count < 0 || run < count); run++) {
        /* capture the output, then draw it all at once */
        int fds[2];
        if (pipe(fds) < 0)
            return 1;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            close(fds[0]);
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            close(fds[1]);
            execl("/bin/sh", "sh", "-c", cmd, (char *)0);
            _exit(127);
        }
        close(fds[1]);
        char *out = 0;
        size_t n = 0, cap = 0;
        char b[1024];
        long r;
        while ((r = read(fds[0], b, sizeof(b))) > 0) {
            if (n + r + 1 > cap)
                out = realloc(out, cap = (n + r + 1) * 2);
            memcpy(out + n, b, r);
            n += r;
        }
        close(fds[0]);
        waitpid(pid, 0, 0);
        if (out)
            out[n] = 0;
        fputs("\033[H\033[2J", stdout);
        if (!no_title) {
            time_t now = time(0);
            char when[32];
            strftime(when, sizeof(when), "%a %b %e %H:%M:%S %Y", localtime(&now));
            printf("Every %ld.%lds: %s    %s: %s\n\n", interval / 1000, interval % 1000 / 100, cmd, host, when);
        }
        if (out)
            fwrite(out, 1, n, stdout);
        fflush(stdout);
        int changed = prev && out && strcmp(prev, out);
        free(prev);
        prev = out;
        if (exit_on_change && changed)
            break;
        if (count >= 0 && run + 1 >= count)
            break;
        for (long t = 0; t < interval && !stop_watch; t += 50)
            sleep_ms(50);
    }
    free(prev);
    return 0;
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(prog, "timeout")) return timeout_main(argc, argv);
    if (!strcmp(prog, "nohup")) return nohup_main(argc, argv);
    if (!strcmp(prog, "time")) return time_main(argc, argv);
    if (!strcmp(prog, "whoami")) return whoami_main(argc, argv);
    if (!strcmp(prog, "groups")) return groups_main(argc, argv);
    if (!strcmp(prog, "hostname")) return hostname_main(argc, argv);
    if (!strcmp(prog, "tty")) return tty_main(argc, argv);
    if (!strcmp(prog, "printenv")) return printenv_main(argc, argv);
    if (!strcmp(prog, "uptime")) return uptime_main(argc, argv);
    if (!strcmp(prog, "cal")) return cal_main(argc, argv);
    if (!strcmp(prog, "pgrep") || !strcmp(prog, "pkill")) return pgrep_main(argc, argv);
    if (!strcmp(prog, "pidof")) return pidof_main(argc, argv);
    if (!strcmp(prog, "killall")) return killall_main(argc, argv);
    if (!strcmp(prog, "watch")) return watch_main(argc, argv);
    eprintf("sysutils: run me as one of: timeout nohup time whoami groups hostname tty printenv "
            "uptime cal pgrep pkill pidof killall watch\n");
    return 2;
}
