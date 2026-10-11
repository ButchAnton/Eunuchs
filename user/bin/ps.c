/*
 * ps: report processes, in the style of Linux procps: options with a dash
 * are System V (ps -ef, ps -lf, ps -o pid,args), options without one are
 * BSD (ps aux, ps axjf), and the two can be mixed.
 *
 * Selection
 *   (none)        processes with my effective uid on my terminal
 *   -e -A, ax     everything                 a   all users with a terminal
 *   -a            with a terminal, minus session leaders   x  include no-terminal
 *   -d            all but session leaders    -N  invert the selection
 *   -p/p/--pid L  by pid    --ppid L  by parent    -s/--sid L  by session
 *   -u/U/--user L by effective user   -U L  by real user   -G L  by real group
 *   -g L          by session or group  -t/t/--tty L  by terminal   T  my terminal
 *   -C L          by command name      r   only running processes
 * Output
 *   -f full  -F extra full  -l long  -j jobs  -L threads   (combine: -lf, -jf)
 *   u user   j jobs   l long   v memory   s signals   (BSD formats)
 *   -o/o/--format FMT  your own columns (pid,comm=NAME,...); -O/O FMT adds to the default
 *   -H, f, --forest     process tree      --sort/k [+-]KEY,...  sort
 *   h, --no-headers     no header         w, -w  wider (twice: unlimited)
 *   c   command name instead of arguments n   numeric user ids
 *   -o help (or ps L)   list the column names
 *
 * Data comes from /proc/<pid>/status and /proc/<pid>/cmdline. Times are UTC.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- process data ---- */

struct pinfo {
    int pid, ppid, pgid, sid, tpgid, tid, psr;
    int uid, euid, gid, egid, umask;
    char state;                 /* R S D T Z */
    char tty[16];
    char comm[32];
    char args[512];
    char wchan[32];
    unsigned long start_ms, cpu_ms, rss_kib, vsz_kib, syscalls, switches;
    unsigned sig_pending, sig_blocked, sig_caught, sig_ignored;
    int selected;
    int depth;                  /* forest */
    int printed;
};

static struct pinfo *procs;
static int nprocs;
static unsigned long now_ms, mem_kib;
static time_t now_wall;

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        eprintf("ps: out of memory\n");
        exit(1);
    }
    return p;
}

static long read_all(const char *path, char *buf, size_t size)
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

/* The value of "key value" in a status file, or NULL. */
static const char *field(char *buf, const char *key)
{
    static char val[256];
    size_t n = strlen(key);
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (!strncmp(line, key, n) && (line[n] == ' ' || line[n] == '\n' || !line[n])) {
            const char *v = line[n] == ' ' ? line + n + 1 : "";
            size_t len = next ? (size_t)(next - v) : strlen(v);
            if (len >= sizeof(val))
                len = sizeof(val) - 1;
            memcpy(val, v, len);
            val[len] = 0;
            return val;
        }
        line = next ? next + 1 : 0;
    }
    return 0;
}

static long num(char *buf, const char *key, long dflt)
{
    const char *v = field(buf, key);
    return v && *v ? strtol(v, 0, 0) : dflt;
}

static const char *const signames[] = {
    0, "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE",
    "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT",
    "SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGURG", "SIGXCPU",
    "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO", "SIGPWR", "SIGSYS",
};

/* "SIGINT SIGCHLD" -> a mask with bit (signal - 1) set, like Linux. */
static unsigned sigmask_of(const char *list)
{
    unsigned m = 0;
    while (list && *list) {
        while (*list == ' ')
            list++;
        const char *e = list;
        while (*e && *e != ' ')
            e++;
        for (int s = 1; s < 32; s++)
            if ((size_t)(e - list) == strlen(signames[s]) && !strncmp(list, signames[s], e - list))
                m |= 1u << (s - 1);
        list = e;
    }
    return m;
}

static int load(const char *pidstr, struct pinfo *p)
{
    char path[64], buf[2048];
    snprintf(path, sizeof(path), "/proc/%s/status", pidstr);
    if (read_all(path, buf, sizeof(buf)) <= 0)
        return -1;
    memset(p, 0, sizeof(*p));
    p->pid = num(buf, "pid", 0);
    p->ppid = num(buf, "ppid", 0);
    p->pgid = num(buf, "pgid", 0);
    p->sid = num(buf, "sid", 0);
    p->tpgid = num(buf, "tpgid", -1);
    p->tid = num(buf, "tid", p->pid);
    p->psr = num(buf, "psr", 0);
    p->start_ms = num(buf, "start_ms", 0);
    p->cpu_ms = num(buf, "cpu_ms", 0);
    p->rss_kib = num(buf, "rss_kib", 0);
    p->vsz_kib = num(buf, "vsz_kib", 0);
    p->syscalls = num(buf, "syscalls", 0);
    p->switches = num(buf, "switches", 0);
    p->sig_pending = num(buf, "sig_pending", 0) & 0xffffffff;
    /* status shows bit (sig) set; Linux masks use bit (sig - 1) */
    p->sig_pending >>= 1;
    p->sig_blocked = (num(buf, "sig_blocked", 0) & 0xffffffff) >> 1;
    p->sig_caught = sigmask_of(field(buf, "sig_caught"));
    p->sig_ignored = sigmask_of(field(buf, "sig_ignored"));
    const char *v = field(buf, "uid");
    if (v) {
        /* "uid 0 euid 0 gid 0 egid 0 umask 022" */
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "uid %s", v);
        char *t = tmp;
        for (int k = 0; k < 5; k++) {
            char *key = t;
            char *sp = strchr(key, ' ');
            if (!sp)
                break;
            *sp = 0;
            char *val = sp + 1;
            char *end = strchr(val, ' ');
            if (end)
                *end = 0;
            int x = (int)strtol(val, 0, !strcmp(key, "umask") ? 8 : 10);
            if (!strcmp(key, "uid")) p->uid = x;
            else if (!strcmp(key, "euid")) p->euid = x;
            else if (!strcmp(key, "gid")) p->gid = x;
            else if (!strcmp(key, "egid")) p->egid = x;
            else if (!strcmp(key, "umask")) p->umask = x;
            if (!end)
                break;
            t = end + 1;
        }
    }
    v = field(buf, "ustate");
    p->state = v && *v ? v[0] : '?';
    if (num(buf, "stopped", 0) && p->state != 'Z')
        p->state = 'T';
    v = field(buf, "tty");
    snprintf(p->tty, sizeof(p->tty), "%s", v && strcmp(v, "-") ? v : "?");
    v = field(buf, "name");
    snprintf(p->comm, sizeof(p->comm), "%s", v ? v : "?");
    v = field(buf, "waiting");
    snprintf(p->wchan, sizeof(p->wchan), "%s", v && p->state != 'R' ? v : "-");

    snprintf(path, sizeof(path), "/proc/%s/cmdline", pidstr);
    long n = read_all(path, p->args, sizeof(p->args));
    if (n > 0) {
        for (long i = 0; i < n - 1; i++)
            if (!p->args[i])
                p->args[i] = ' ';
    } else {
        snprintf(p->args, sizeof(p->args), "[%s]", p->comm);
    }
    if (p->state == 'Z')
        snprintf(p->args, sizeof(p->args), "[%s] <defunct>", p->comm);
    return 0;
}

static void load_all(void)
{
    int cap = 64;
    procs = xmalloc(cap * sizeof(*procs));
    DIR *d = opendir("/proc");
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        if (nprocs == cap) {
            cap *= 2;
            procs = realloc(procs, cap * sizeof(*procs));
            if (!procs) {
                eprintf("ps: out of memory\n");
                exit(1);
            }
        }
        if (load(de->d_name, &procs[nprocs]) == 0)
            nprocs++;
    }
    if (d)
        closedir(d);
    now_ms = uptime_ms();
    now_wall = time(0);
    char buf[1024];
    if (read_all("/proc/meminfo", buf, sizeof(buf)) > 0) {
        char *t = strstr(buf, "total");
        if (t) {
            /* "total    N pages  K KiB" */
            char *p = strstr(t, "pages");
            if (p)
                mem_kib = strtol(p + 5, 0, 10);
        }
    }
}

/* ---- users and groups ---- */

static const char *id_name(const char *file, int id, int numeric)
{
    static char out[4][32];
    static int slot;
    char *r = out[slot++ % 4];
    snprintf(r, 32, "%d", id);
    if (numeric)
        return r;
    char buf[2048];
    if (read_all(file, buf, sizeof(buf)) <= 0)
        return r;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *c1 = strchr(line, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : 0;
        if (c2 && atoi(c2 + 1) == id) {
            snprintf(r, 32, "%.*s", (int)(c1 - line), line);
            return r;
        }
        line = next;
    }
    return r;
}

static int name_id(const char *file, const char *name)
{
    if (name[0] >= '0' && name[0] <= '9')
        return atoi(name);
    char buf[2048];
    if (read_all(file, buf, sizeof(buf)) <= 0)
        return -1;
    size_t n = strlen(name);
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *c2 = strchr(line, ':');
        c2 = c2 ? strchr(c2 + 1, ':') : 0;
        if (c2 && !strncmp(line, name, n) && line[n] == ':')
            return atoi(c2 + 1);
        line = next;
    }
    return -1;
}

/* ---- formatting values ---- */

static int numeric_ids;

static void fmt_clock(char *b, size_t n, unsigned long ms, int with_days)
{
    unsigned long s = ms / 1000;
    unsigned long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    s %= 60;
    if (with_days && d)
        snprintf(b, n, "%lu-%02lu:%02lu:%02lu", d, h, m, s);
    else
        snprintf(b, n, "%02lu:%02lu:%02lu", d * 24 + h, m, s);
}

static void fmt_elapsed(char *b, size_t n, unsigned long ms)
{
    unsigned long s = ms / 1000;
    unsigned long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    s %= 60;
    if (d)
        snprintf(b, n, "%lu-%02lu:%02lu:%02lu", d, h, m, s);
    else if (h)
        snprintf(b, n, "%02lu:%02lu:%02lu", h, m, s);
    else
        snprintf(b, n, "%02lu:%02lu", m, s);
}

static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static time_t started_at(struct pinfo *p)
{
    return now_wall - (time_t)((now_ms - p->start_ms) / 1000);
}

/* style: 0 STIME (HH:MM or MonDD), 1 START (HH:MM:SS or Mon DD), 2 lstart */
static void fmt_start(char *b, size_t n, struct pinfo *p, int style)
{
    time_t t = started_at(p);
    struct tm tm;
    gmtime_r(&t, &tm);
    int recent = now_ms - p->start_ms < 24UL * 3600 * 1000;
    if (style == 2)
        snprintf(b, n, "%s %s %2d %02d:%02d:%02d %d", days[tm.tm_wday % 7], months[tm.tm_mon % 12],
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
    else if (recent && style == 1)
        snprintf(b, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else if (recent)
        snprintf(b, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else
        snprintf(b, n, style == 1 ? " %s %02d" : "%s%02d", months[tm.tm_mon % 12], tm.tm_mday);
}

/* %CPU times 10 */
static unsigned long pcpu10(struct pinfo *p)
{
    unsigned long el = now_ms > p->start_ms ? now_ms - p->start_ms : 1;
    return p->cpu_ms * 1000 / (el ? el : 1);
}

/* ---- columns ---- */

enum { AL, AR };

struct colspec {
    const char *names;          /* space-separated aliases; the first is canonical */
    const char *header;
    int width, align;
    const char *help;
};

static const struct colspec specs[] = {
    { "pid", "PID", 5, AR, "process id" },
    { "ppid", "PPID", 5, AR, "parent's process id" },
    { "pgid pgrp", "PGID", 5, AR, "process group" },
    { "sid sess session", "SID", 5, AR, "session" },
    { "tpgid", "TPGID", 5, AR, "foreground group of the terminal" },
    { "lwp tid spid", "LWP", 5, AR, "thread id" },
    { "nlwp thcount", "NLWP", 4, AR, "number of threads" },
    { "uid euid", "UID", 5, AR, "effective user id" },
    { "ruid", "RUID", 5, AR, "real user id" },
    { "user euser uname", "USER", 8, AL, "effective user name" },
    { "ruser", "RUSER", 8, AL, "real user name" },
    { "gid egid", "GID", 5, AR, "effective group id" },
    { "rgid", "RGID", 5, AR, "real group id" },
    { "group egroup", "GROUP", 8, AL, "effective group name" },
    { "rgroup", "RGROUP", 8, AL, "real group name" },
    { "tty tt tname", "TTY", 8, AL, "controlling terminal" },
    { "stat", "STAT", 4, AL, "state with BSD flags (s session leader, + foreground)" },
    { "s state", "S", 1, AL, "state: R running, S sleeping, D uninterruptible, T stopped, Z zombie" },
    { "comm ucomm ucmd", "COMMAND", 8, AL, "command name" },
    { "args cmd command", "COMMAND", 7, AL, "command with arguments" },
    { "etime", "ELAPSED", 7, AR, "elapsed time since start, [[dd-]hh:]mm:ss" },
    { "etimes", "ELAPSED", 7, AR, "elapsed seconds" },
    { "time cputime", "TIME", 8, AR, "CPU time, [dd-]hh:mm:ss" },
    { "times cputimes", "TIME", 6, AR, "CPU seconds" },
    { "bsdtime", "TIME", 6, AR, "CPU time, m:ss" },
    { "stime start_time", "STIME", 5, AR, "start time (hh:mm, or MonDD)" },
    { "start", "START", 8, AR, "start time (hh:mm:ss, or Mon dd)" },
    { "lstart", "STARTED", 24, AL, "start date and time" },
    { "pcpu %cpu", "%CPU", 4, AR, "CPU use over the process's life" },
    { "c cp", "C", 2, AR, "CPU use, integer percent" },
    { "pmem %mem", "%MEM", 4, AR, "resident memory as a percentage of RAM" },
    { "rss rssize rsz", "RSS", 5, AR, "resident memory, KiB" },
    { "vsz vsize", "VSZ", 6, AR, "virtual memory, KiB" },
    { "sz", "SZ", 5, AR, "virtual memory, pages" },
    { "psr cpu", "PSR", 3, AR, "processor it last ran on" },
    { "wchan", "WCHAN", 6, AL, "what a sleeping process waits for" },
    { "pri", "PRI", 3, AR, "priority (all processes share one)" },
    { "ni nice", "NI", 3, AR, "nice value (always 0)" },
    { "cls class", "CLS", 3, AL, "scheduling class (TS)" },
    { "f flags flag", "F", 1, AR, "flags: 4 = running as root" },
    { "addr", "ADDR", 4, AL, "(not available: -)" },
    { "pending sig", "PENDING", 8, AR, "pending signals (hex mask)" },
    { "blocked sigmask", "BLOCKED", 8, AR, "blocked signals (hex mask)" },
    { "ignored sigignore", "IGNORED", 8, AR, "ignored signals (hex mask)" },
    { "caught sigcatch", "CAUGHT", 8, AR, "caught signals (hex mask)" },
    { "umask", "UMASK", 5, AR, "file creation mask" },
    { "syscalls nsys", "SYSCALLS", 8, AR, "system calls made (Eunuchs)" },
    { "switches nvcsw", "SWITCH", 6, AR, "times scheduled (Eunuchs)" },
    { 0, 0, 0, 0, 0 },
};

struct col {
    const struct colspec *spec;
    char header[32];
    int width;
};

static struct col cols[64];
static int ncols;
static int forest, forest_art, show_comm_only;

static const struct colspec *find_spec(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; specs[i].names; i++) {
        const char *s = specs[i].names;
        while (*s) {
            const char *e = strchr(s, ' ');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len == n && !strncmp(s, name, n))
                return &specs[i];
            s += len + (e ? 1 : 0);
        }
    }
    return 0;
}

static int is(const struct colspec *s, const char *canon)
{
    size_t n = strlen(canon);
    return !strncmp(s->names, canon, n) && (s->names[n] == ' ' || !s->names[n]);
}

/* The text of one cell. */
static void cell(struct pinfo *p, const struct colspec *s, char *b, size_t n)
{
    if (is(s, "pid")) snprintf(b, n, "%d", p->pid);
    else if (is(s, "ppid")) snprintf(b, n, "%d", p->ppid);
    else if (is(s, "pgid")) snprintf(b, n, "%d", p->pgid);
    else if (is(s, "sid")) snprintf(b, n, "%d", p->sid);
    else if (is(s, "tpgid")) snprintf(b, n, "%d", p->tpgid);
    else if (is(s, "lwp")) snprintf(b, n, "%d", p->pid);
    else if (is(s, "nlwp")) snprintf(b, n, "1");
    else if (is(s, "uid")) snprintf(b, n, "%d", p->euid);
    else if (is(s, "ruid")) snprintf(b, n, "%d", p->uid);
    else if (is(s, "user")) snprintf(b, n, "%s", id_name("/etc/passwd", p->euid, numeric_ids));
    else if (is(s, "ruser")) snprintf(b, n, "%s", id_name("/etc/passwd", p->uid, numeric_ids));
    else if (is(s, "gid")) snprintf(b, n, "%d", p->egid);
    else if (is(s, "rgid")) snprintf(b, n, "%d", p->gid);
    else if (is(s, "group")) snprintf(b, n, "%s", id_name("/etc/group", p->egid, numeric_ids));
    else if (is(s, "rgroup")) snprintf(b, n, "%s", id_name("/etc/group", p->gid, numeric_ids));
    else if (is(s, "tty")) snprintf(b, n, "%s", p->tty);
    else if (is(s, "stat")) {
        int k = 0;
        b[k++] = p->state;
        if (p->pid == p->sid && k < (int)n - 1)
            b[k++] = 's';
        if (p->tpgid > 0 && p->pgid == p->tpgid && k < (int)n - 1)
            b[k++] = '+';
        b[k] = 0;
    } else if (is(s, "s")) snprintf(b, n, "%c", p->state);
    else if (is(s, "comm") || is(s, "args")) {
        const char *text = is(s, "comm") || show_comm_only ? p->comm : p->args;
        int k = 0;
        if (forest && p->depth > 0) {
            if (forest_art) {
                k += snprintf(b + k, n - k, " ");
                for (int i = 1; i < p->depth && k < (int)n; i++)
                    k += snprintf(b + k, n - k, "    ");
                if (k < (int)n)
                    k += snprintf(b + k, n - k, "\\_ ");
            } else {
                for (int i = 0; i < p->depth && k < (int)n; i++)
                    k += snprintf(b + k, n - k, "  ");
            }
        }
        if (k < (int)n)
            snprintf(b + k, n - k, "%s", text);
    } else if (is(s, "etime")) fmt_elapsed(b, n, now_ms - p->start_ms);
    else if (is(s, "etimes")) snprintf(b, n, "%lu", (now_ms - p->start_ms) / 1000);
    else if (is(s, "time")) fmt_clock(b, n, p->cpu_ms, 1);
    else if (is(s, "times")) snprintf(b, n, "%lu", p->cpu_ms / 1000);
    else if (is(s, "bsdtime")) snprintf(b, n, "%lu:%02lu", p->cpu_ms / 60000, p->cpu_ms / 1000 % 60);
    else if (is(s, "stime")) fmt_start(b, n, p, 0);
    else if (is(s, "start")) fmt_start(b, n, p, 1);
    else if (is(s, "lstart")) fmt_start(b, n, p, 2);
    else if (is(s, "pcpu")) {
        unsigned long c = pcpu10(p);
        snprintf(b, n, "%lu.%lu", c / 10, c % 10);
    } else if (is(s, "c")) snprintf(b, n, "%lu", pcpu10(p) / 10);
    else if (is(s, "pmem")) {
        unsigned long m = mem_kib ? p->rss_kib * 1000 / mem_kib : 0;
        snprintf(b, n, "%lu.%lu", m / 10, m % 10);
    } else if (is(s, "rss")) snprintf(b, n, "%lu", p->rss_kib);
    else if (is(s, "vsz")) snprintf(b, n, "%lu", p->vsz_kib);
    else if (is(s, "sz")) snprintf(b, n, "%lu", p->vsz_kib / 4);
    else if (is(s, "psr")) snprintf(b, n, "%d", p->psr);
    else if (is(s, "wchan")) snprintf(b, n, "%s", p->wchan);
    else if (is(s, "pri")) snprintf(b, n, "80");
    else if (is(s, "ni")) snprintf(b, n, "0");
    else if (is(s, "cls")) snprintf(b, n, "TS");
    else if (is(s, "f")) snprintf(b, n, "%d", p->euid == 0 ? 4 : 0);
    else if (is(s, "addr")) snprintf(b, n, "-");
    else if (is(s, "pending")) snprintf(b, n, "%08x", p->sig_pending);
    else if (is(s, "blocked")) snprintf(b, n, "%08x", p->sig_blocked);
    else if (is(s, "ignored")) snprintf(b, n, "%08x", p->sig_ignored);
    else if (is(s, "caught")) snprintf(b, n, "%08x", p->sig_caught);
    else if (is(s, "umask")) snprintf(b, n, "%04o", p->umask);
    else if (is(s, "syscalls")) snprintf(b, n, "%lu", p->syscalls);
    else if (is(s, "switches")) snprintf(b, n, "%lu", p->switches);
    else snprintf(b, n, "?");
}

/* Sort keys compare numerically when both cells are numbers. */
static int numeric_key(const struct colspec *s)
{
    return s->align == AR && !is(s, "time") && !is(s, "etime") && !is(s, "bsdtime") &&
           !is(s, "stime") && !is(s, "start") && !is(s, "pending") && !is(s, "blocked") &&
           !is(s, "ignored") && !is(s, "caught");
}

static long sort_value(struct pinfo *p, const struct colspec *s)
{
    if (is(s, "time") || is(s, "times") || is(s, "bsdtime")) return (long)p->cpu_ms;
    if (is(s, "etime") || is(s, "etimes") || is(s, "stime") || is(s, "start") || is(s, "lstart"))
        return -(long)p->start_ms;
    if (is(s, "pcpu") || is(s, "c")) return (long)pcpu10(p);
    if (is(s, "pmem") || is(s, "rss")) return (long)p->rss_kib;
    char b[64];
    cell(p, s, b, sizeof(b));
    int hex = is(s, "pending") || is(s, "blocked") || is(s, "ignored") || is(s, "caught");
    return strtol(b, 0, hex ? 16 : 10);
}

/* ---- options ---- */

static void usage(int full)
{
    printf("usage: ps [options]\n");
    if (!full) {
        printf("Try 'ps --help' for more, and 'ps -o help' for the column names.\n");
        return;
    }
    printf("Like procps: options with '-' are System V, options without are BSD.\n\n"
           "Examples:  ps   ps -ef   ps -lf   ps -ejH   ps aux   ps axjf   ps -o pid,stat,args\n\n"
           "Selection:\n"
           "  -e, -A, ax     every process          a     all users (with a terminal)\n"
           "  -a             terminal, not session leaders    x   include processes without one\n"
           "  -d             all but session leaders          -N, --deselect   invert\n"
           "  -p, p, --pid LIST    by process id    --ppid LIST   by parent\n"
           "  -s, --sid LIST       by session       -t, t, --tty LIST   by terminal (T: mine)\n"
           "  -u, U, --user LIST   by user          -U LIST   by real user\n"
           "  -G LIST    by real group   -g LIST  by session or group   -C LIST  by command name\n"
           "  r          only running processes      NNN   a pid (BSD)\n"
           "Output:\n"
           "  -f full   -F extra full   -l long   -j jobs   -L with threads   -y  (accepted)\n"
           "  u user-oriented   j jobs   l long   v memory   s signals\n"
           "  -o, o, --format FMT   columns: pid,comm,args (name=HEADER renames)\n"
           "  -O, O FMT    the default columns plus FMT\n"
           "  -H, f, --forest   tree    --sort, k [+|-]KEY,...   sort (- for descending)\n"
           "  h, --no-headers   no header line   w, -w  wide (twice: unlimited)\n"
           "  c   command name only    n   numeric user and group ids\n"
           "  -o help, L   list the columns\n");
}

static void list_columns(void)
{
    for (int i = 0; specs[i].names; i++)
        printf("%-24s %-8s %s\n", specs[i].names, specs[i].header, specs[i].help);
}

/* Selection criteria. */
struct idlist {
    int v[64];
    int n;
};
static struct idlist sel_pid, sel_ppid, sel_sid, sel_euid, sel_ruid, sel_rgid, sel_sess_or_gid;
static char sel_tty[8][16];
static int nsel_tty;
static char sel_cmd[16][32];
static int nsel_cmd;
static int any_list;            /* an explicit selection list was given */
static int all, all_with_tty, no_tty_ok, not_leaders, terminal_not_leaders, running_only;
static int negate, only_mine_tty;
static int bsd_style, sysv_style;

/* formats */
static int fmt_full, fmt_extra, fmt_long, fmt_jobs, fmt_threads;
static char bsd_fmt;            /* u j l v s, or 0 */
static char user_fmt[1024];
static char extra_fmt[512];
static int no_header, width_level;
static char sort_spec[256];

static void add_ids(struct idlist *l, const char *arg, const char *file)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", arg);
    for (char *t = buf; t && *t;) {
        char *e = t + strcspn(t, ", ");
        char c = *e;
        *e = 0;
        if (*t && l->n < 64) {
            int id = file ? name_id(file, t) : atoi(t);
            if (id < 0) {
                eprintf("ps: %s: no such %s\n", t, !strcmp(file, "/etc/group") ? "group" : "user");
                exit(1);
            }
            l->v[l->n++] = id;
        }
        t = c ? e + 1 : 0;
    }
    any_list = 1;
}

static void add_strs(char (*v)[16], int *n, int max, const char *arg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", arg);
    for (char *t = buf; t && *t;) {
        char *e = t + strcspn(t, ", ");
        char c = *e;
        *e = 0;
        if (*t && *n < max)
            snprintf(v[(*n)++], 16, "%s", t);
        t = c ? e + 1 : 0;
    }
    any_list = 1;
}

static void add_cmds(const char *arg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", arg);
    for (char *t = buf; t && *t;) {
        char *e = t + strcspn(t, ",");
        char c = *e;
        *e = 0;
        if (*t && nsel_cmd < 16)
            snprintf(sel_cmd[nsel_cmd++], 32, "%s", t);
        t = c ? e + 1 : 0;
    }
    any_list = 1;
}

static void append_fmt(char *dst, size_t size, const char *add)
{
    size_t n = strlen(dst);
    snprintf(dst + n, size - n, "%s%s", n ? "," : "", add);
}

/* Option value: rest of this argument or the next one. */
static const char *optval(const char *rest, int *i, int argc, char **argv, const char *opt)
{
    if (*rest)
        return rest;
    if (*i + 1 < argc)
        return argv[++*i];
    eprintf("ps: option %s needs an argument\n", opt);
    usage(0);
    exit(1);
}

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--", 2)) {
            const char *name = a + 2, *eq = strchr(name, '=');
            char key[32];
            snprintf(key, sizeof(key), "%.*s", eq ? (int)(eq - name) : (int)strlen(name), name);
            const char *val = eq ? eq + 1 : "";
#define LONGVAL() (eq ? val : optval("", &i, argc, argv, a))
            if (!strcmp(key, "help")) { usage(1); exit(0); }
            else if (!strcmp(key, "version")) { printf("ps (Eunuchs) 1.0, procps-style\n"); exit(0); }
            else if (!strcmp(key, "pid")) add_ids(&sel_pid, LONGVAL(), 0);
            else if (!strcmp(key, "ppid")) add_ids(&sel_ppid, LONGVAL(), 0);
            else if (!strcmp(key, "sid")) add_ids(&sel_sid, LONGVAL(), 0);
            else if (!strcmp(key, "user")) add_ids(&sel_euid, LONGVAL(), "/etc/passwd");
            else if (!strcmp(key, "group")) add_ids(&sel_rgid, LONGVAL(), "/etc/group");
            else if (!strcmp(key, "tty")) add_strs(sel_tty, &nsel_tty, 8, LONGVAL());
            else if (!strcmp(key, "sort")) snprintf(sort_spec, sizeof(sort_spec), "%s", LONGVAL());
            else if (!strcmp(key, "format")) append_fmt(user_fmt, sizeof(user_fmt), LONGVAL());
            else if (!strcmp(key, "forest")) forest = forest_art = 1;
            else if (!strcmp(key, "no-headers") || !strcmp(key, "no-heading")) no_header = 1;
            else if (!strcmp(key, "headers")) no_header = 0;
            else if (!strcmp(key, "deselect")) negate = 1;
            else if (!strcmp(key, "width") || !strcmp(key, "cols") || !strcmp(key, "columns")) {
                width_level = -atoi(LONGVAL());
            } else {
                eprintf("ps: unknown option: %s\n", a);
                usage(0);
                exit(1);
            }
#undef LONGVAL
            continue;
        }
        if (a[0] == '-') {
            sysv_style = 1;
            for (const char *c = a + 1; *c; c++) {
                const char *rest = c + 1;
                switch (*c) {
                case 'e': case 'A': all = 1; break;
                case 'a': terminal_not_leaders = 1; break;
                case 'd': not_leaders = 1; break;
                case 'N': negate = 1; break;
                case 'f': fmt_full = 1; break;
                case 'F': fmt_full = fmt_extra = 1; break;
                case 'l': fmt_long = 1; break;
                case 'j': fmt_jobs = 1; break;
                case 'L': case 'T': fmt_threads = 1; break;
                case 'H': forest = 1; break;
                case 'w': width_level++; break;
                case 'y': case 'c': case 'M': case 'Z': break;
                case 'p': case 'q': add_ids(&sel_pid, optval(rest, &i, argc, argv, "-p"), 0); goto next;
                case 's': add_ids(&sel_sid, optval(rest, &i, argc, argv, "-s"), 0); goto next;
                case 'u': add_ids(&sel_euid, optval(rest, &i, argc, argv, "-u"), "/etc/passwd"); goto next;
                case 'U': add_ids(&sel_ruid, optval(rest, &i, argc, argv, "-U"), "/etc/passwd"); goto next;
                case 'G': add_ids(&sel_rgid, optval(rest, &i, argc, argv, "-G"), "/etc/group"); goto next;
                case 'g': {
                    const char *v = optval(rest, &i, argc, argv, "-g");
                    if (v[0] >= '0' && v[0] <= '9')
                        add_ids(&sel_sess_or_gid, v, 0);
                    else
                        add_ids(&sel_rgid, v, "/etc/group");
                    goto next;
                }
                case 't': add_strs(sel_tty, &nsel_tty, 8, optval(rest, &i, argc, argv, "-t")); goto next;
                case 'C': add_cmds(optval(rest, &i, argc, argv, "-C")); goto next;
                case 'o': append_fmt(user_fmt, sizeof(user_fmt), optval(rest, &i, argc, argv, "-o")); goto next;
                case 'O': append_fmt(extra_fmt, sizeof(extra_fmt), optval(rest, &i, argc, argv, "-O")); goto next;
                default:
                    eprintf("ps: unknown option: -%c\n", *c);
                    usage(0);
                    exit(1);
                }
            }
        next:
            continue;
        }
        /* BSD: a bare number is a pid */
        if (a[0] >= '0' && a[0] <= '9') {
            add_ids(&sel_pid, a, 0);
            continue;
        }
        bsd_style = 1;
        for (const char *c = a; *c; c++) {
            const char *rest = c + 1;
            switch (*c) {
            case 'a': all_with_tty = 1; break;
            case 'x': no_tty_ok = 1; break;
            case 'r': running_only = 1; break;
            case 'T': only_mine_tty = 1; break;
            case 'u': case 'j': case 'l': case 'v': case 's': bsd_fmt = *c; break;
            case 'f': forest = forest_art = 1; break;
            case 'w': width_level++; break;
            case 'h': no_header = 1; break;
            case 'c': show_comm_only = 1; break;
            case 'n': numeric_ids = 1; break;
            case 'S': case 'e': case 'm': break;
            case 'L': list_columns(); exit(0);
            case 'p': add_ids(&sel_pid, optval(rest, &i, argc, argv, "p"), 0); goto bnext;
            case 'U': add_ids(&sel_euid, optval(rest, &i, argc, argv, "U"), "/etc/passwd"); goto bnext;
            case 't': add_strs(sel_tty, &nsel_tty, 8, optval(rest, &i, argc, argv, "t")); goto bnext;
            case 'o': append_fmt(user_fmt, sizeof(user_fmt), optval(rest, &i, argc, argv, "o")); goto bnext;
            case 'O': append_fmt(extra_fmt, sizeof(extra_fmt), optval(rest, &i, argc, argv, "O")); goto bnext;
            case 'k': snprintf(sort_spec, sizeof(sort_spec), "%s", optval(rest, &i, argc, argv, "k")); goto bnext;
            default:
                eprintf("ps: unknown option: %c\n", *c);
                usage(0);
                exit(1);
            }
        }
    bnext:;
    }
}

/* ---- building the column list ---- */

static void add_col(const char *spec_text)
{
    char name[48], hdr[32];
    const char *eq = strchr(spec_text, '=');
    snprintf(name, sizeof(name), "%.*s", eq ? (int)(eq - spec_text) : (int)strlen(spec_text), spec_text);
    if (!strcmp(name, "help") || !strcmp(name, "L")) {
        list_columns();
        exit(0);
    }
    const struct colspec *s = find_spec(name);
    if (!s) {
        eprintf("ps: unknown column: %s (ps -o help lists them)\n", name);
        exit(1);
    }
    if (ncols == 64)
        return;
    struct col *c = &cols[ncols++];
    c->spec = s;
    if (eq)
        snprintf(hdr, sizeof(hdr), "%s", eq + 1);
    else
        snprintf(hdr, sizeof(hdr), "%s", s->header);
    snprintf(c->header, sizeof(c->header), "%s", hdr);
    c->width = 0;
}

/* "pid,tty,comm=NAME" (a "name=" header takes the rest of the list, as in procps) */
static void add_cols(const char *list)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", list);
    char *t = buf;
    while (*t) {
        while (*t == ',' || *t == ' ')
            t++;
        if (!*t)
            break;
        char *e = t;
        while (*e && *e != ',' && *e != ' ' && *e != '=')
            e++;
        if (*e == '=') {
            char *end = strchr(e, ',');
            /* a header ends at the next comma only if a known column follows it */
            while (end) {
                char nm[48];
                const char *n2 = end + 1;
                size_t l = strcspn(n2, ",= ");
                snprintf(nm, sizeof(nm), "%.*s", (int)l, n2);
                if (find_spec(nm))
                    break;
                end = strchr(end + 1, ',');
            }
            if (end)
                *end = 0;
            add_col(t);
            if (!end)
                break;
            t = end + 1;
            continue;
        }
        char c = *e;
        *e = 0;
        add_col(t);
        if (!c)
            break;
        t = e + 1;
    }
}

static void build_format(void)
{
    if (*user_fmt) {
        add_cols(user_fmt);
        return;
    }
    const char *f;
    if (bsd_fmt == 'u')
        f = "user,pid,%cpu,%mem,vsz,rss,tname,stat,start,bsdtime,args";
    else if (bsd_fmt == 'j')
        f = "ppid,pid,pgid,sid,tname,tpgid,stat,uid,bsdtime,args";
    else if (bsd_fmt == 'l')
        f = "f,uid,pid,ppid,pri,ni,vsz,rss,wchan,stat,tname,bsdtime,args";
    else if (bsd_fmt == 'v')
        f = "pid,tname,stat,bsdtime,vsz,rss,%mem,args";
    else if (bsd_fmt == 's')
        f = "uid,pid,pending,blocked,ignored,caught,stat,tname,bsdtime,args";
    else if (fmt_long && fmt_full)
        f = fmt_threads ? "f,s,user=UID,pid,ppid,lwp,c,pri,ni,addr,sz,wchan,stime,tty,time,args"
                        : "f,s,user=UID,pid,ppid,c,pri,ni,addr,sz,wchan,stime,tty,time,args";
    else if (fmt_long)
        f = fmt_threads ? "f,s,uid,pid,ppid,lwp,c,pri,ni,addr,sz,wchan,tty,time,comm=CMD"
                        : "f,s,uid,pid,ppid,c,pri,ni,addr,sz,wchan,tty,time,comm=CMD";
    else if (fmt_jobs && fmt_full)
        f = "user=UID,pid,ppid,pgid,sid,c,stime,tty,time,args=CMD";
    else if (fmt_jobs)
        f = "pid,pgid,sid,tty,time,comm=CMD";
    else if (fmt_extra)
        f = fmt_threads ? "user=UID,pid,ppid,lwp,c,nlwp,sz,rss,psr,stime,tty,time,args=CMD"
                        : "user=UID,pid,ppid,c,sz,rss,psr,stime,tty,time,args=CMD";
    else if (fmt_full)
        f = fmt_threads ? "user=UID,pid,ppid,lwp,c,nlwp,stime,tty,time,args=CMD"
                        : "user=UID,pid,ppid,c,stime,tty,time,args=CMD";
    else if (fmt_threads)
        f = "pid,lwp,tty,time,comm=CMD";
    else if (bsd_style && !sysv_style)
        f = "pid,tname,stat,bsdtime,args";
    else
        f = "pid,tty,time,comm=CMD";
    if (*extra_fmt) {
        /* -O: pid, the extra columns, then the default's other columns */
        add_col("pid");
        add_cols(extra_fmt);
        const char *rest = strchr(f, ',');
        if (!strncmp(f, "pid,", 4))
            add_cols(rest + 1);
        else
            add_cols(f);
        return;
    }
    add_cols(f);
}

/* ---- selection ---- */

static int in(struct idlist *l, int v)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == v)
            return 1;
    return 0;
}

static int tty_match(const char *want, const char *have)
{
    if (!strcmp(want, "-") || !strcmp(want, "?"))
        return !strcmp(have, "?");
    if (!strncmp(want, "/dev/", 5))
        want += 5;
    if (!strcmp(want, "tty") || !strcmp(want, "tty0") || !strcmp(want, "ttyS0"))
        want = "console";
    return !strcmp(want, have);
}

static void select_procs(void)
{
    /* who am I */
    char my_tty[16] = "?";
    int my_euid = geteuid();
    char buf[2048];
    if (read_all("/proc/self/status", buf, sizeof(buf)) > 0) {
        const char *v = field(buf, "tty");
        if (v && strcmp(v, "-"))
            snprintf(my_tty, sizeof(my_tty), "%s", v);
    }
    int by_flags = all || all_with_tty || no_tty_ok || not_leaders || terminal_not_leaders ||
                   only_mine_tty;
    for (int i = 0; i < nprocs; i++) {
        struct pinfo *p = &procs[i];
        int s = 0;
        /* explicit lists: a process matching any of them is selected */
        if (in(&sel_pid, p->pid) || in(&sel_ppid, p->ppid) || in(&sel_sid, p->sid) ||
            in(&sel_euid, p->euid) || in(&sel_ruid, p->uid) || in(&sel_rgid, p->gid) ||
            in(&sel_sess_or_gid, p->sid))
            s = 1;
        for (int k = 0; k < nsel_tty && !s; k++)
            s = tty_match(sel_tty[k], p->tty);
        for (int k = 0; k < nsel_cmd && !s; k++)
            s = !strcmp(sel_cmd[k], p->comm);
        int has_tty = strcmp(p->tty, "?") != 0;
        if (all)
            s = 1;
        if (not_leaders && p->pid != p->sid)
            s = 1;
        if (terminal_not_leaders && has_tty && p->pid != p->sid)
            s = 1;
        if (only_mine_tty && !strcmp(p->tty, my_tty) && has_tty)
            s = 1;
        if (bsd_style && (all_with_tty || no_tty_ok)) {
            /* BSD: a lifts "only mine", x lifts "only with a terminal" */
            int mine_ok = all_with_tty || p->euid == my_euid;
            int tty_ok = no_tty_ok || has_tty;
            if (mine_ok && tty_ok)
                s = 1;
        }
        if (!any_list && !by_flags) {
            /* the default: mine, on my terminal */
            if (bsd_style && !sysv_style)
                s = p->euid == my_euid && has_tty;
            else
                s = p->euid == my_euid && !strcmp(p->tty, my_tty);
        }
        if (running_only && p->state != 'R')        /* r narrows any selection */
            s = 0;
        p->selected = negate ? !s : s;
    }
}

/* ---- sorting and the forest ---- */

struct sortkey {
    const struct colspec *s;
    int desc;
};
static struct sortkey keys[8];
static int nkeys;

static void parse_sort(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", sort_spec);
    for (char *t = buf; t && *t;) {
        char *e = strchr(t, ',');
        if (e)
            *e = 0;
        int desc = 0;
        if (*t == '-' || *t == '+') {
            desc = *t == '-';
            t++;
        }
        const struct colspec *s = find_spec(t);
        if (!s) {
            eprintf("ps: unknown sort key: %s (ps -o help lists them)\n", t);
            exit(1);
        }
        if (nkeys < 8) {
            keys[nkeys].s = s;
            keys[nkeys].desc = desc;
            nkeys++;
        }
        t = e ? e + 1 : 0;
    }
}

static int cmp(struct pinfo *a, struct pinfo *b)
{
    for (int k = 0; k < nkeys; k++) {
        const struct colspec *s = keys[k].s;
        int r;
        if (numeric_key(s) || is(s, "time") || is(s, "etime") || is(s, "bsdtime") ||
            is(s, "stime") || is(s, "start") || is(s, "pcpu") || is(s, "pmem")) {
            long x = sort_value(a, s), y = sort_value(b, s);
            r = x < y ? -1 : x > y;
        } else {
            char x[512], y[512];
            cell(a, s, x, sizeof(x));
            cell(b, s, y, sizeof(y));
            r = strcmp(x, y);
        }
        if (r)
            return keys[k].desc ? -r : r;
    }
    return a->pid < b->pid ? -1 : a->pid > b->pid;
}

static void sort_procs(struct pinfo **v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cmp(v[j - 1], v[j]) > 0; j--) {
            struct pinfo *t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

static struct pinfo **order;
static int norder;

static void add_tree(struct pinfo *p, int depth, struct pinfo **sel, int nsel)
{
    if (p->printed || depth > 64)
        return;
    p->printed = 1;
    p->depth = depth;
    order[norder++] = p;
    for (int i = 0; i < nsel; i++)
        if (sel[i]->ppid == p->pid && sel[i] != p)
            add_tree(sel[i], depth + 1, sel, nsel);
}

static void build_order(void)
{
    struct pinfo **sel = xmalloc((nprocs + 1) * sizeof(*sel));
    int nsel = 0;
    for (int i = 0; i < nprocs; i++)
        if (procs[i].selected)
            sel[nsel++] = &procs[i];
    sort_procs(sel, nsel);
    order = xmalloc((nsel + 1) * sizeof(*order));
    if (!forest) {
        memcpy(order, sel, nsel * sizeof(*sel));
        norder = nsel;
        return;
    }
    for (int i = 0; i < nsel; i++) {
        int parent_selected = 0;
        for (int k = 0; k < nsel; k++)
            if (sel[k]->pid == sel[i]->ppid && sel[k] != sel[i])
                parent_selected = 1;
        if (!parent_selected)
            add_tree(sel[i], 0, sel, nsel);
    }
    for (int i = 0; i < nsel; i++)     /* anything left (cycles) */
        if (!sel[i]->printed)
            add_tree(sel[i], 0, sel, nsel);
}

/* ---- output ---- */

static int term_width(void)
{
    if (width_level < 0)
        return -width_level;           /* --width N */
    if (width_level >= 2)
        return 0;                       /* unlimited */
    if (width_level == 1)
        return 132;
    if (!isatty(1))
        return 0;
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 0)
        return atoi(c);
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        return ws.ws_col;
    return 80;
}

static void emit(char *line, int width)
{
    int n = strlen(line);
    while (n && line[n - 1] == ' ')
        n--;
    if (width && n > width)
        n = width;
    line[n] = 0;
    printf("%s\n", line);
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    build_format();
    if (*sort_spec)
        parse_sort();
    load_all();
    select_procs();
    build_order();

    /* column widths: at least the header and the minimum, at most the longest value */
    char cellbuf[600];
    for (int c = 0; c < ncols; c++) {
        int w = strlen(cols[c].header);
        int padded = c + 1 < ncols || cols[c].spec->align == AR;   /* a last text column runs free */
        if (cols[c].spec->width > w && padded)
            w = cols[c].spec->width;
        if (padded)
            for (int i = 0; i < norder; i++) {
                cell(order[i], cols[c].spec, cellbuf, sizeof(cellbuf));
                int l = strlen(cellbuf);
                if (l > w)
                    w = l;
            }
        cols[c].width = w;
    }
    int width = term_width();
    int any_header = 0;
    for (int c = 0; c < ncols; c++)
        any_header |= cols[c].header[0] != 0;

    char *line = xmalloc(8192);
    if (!no_header && any_header) {
        int k = 0;
        for (int c = 0; c < ncols && k < 8000; c++) {
            const char *h = cols[c].header;
            int last = c + 1 == ncols;
            if (cols[c].spec->align == AR)
                k += snprintf(line + k, 8192 - k, "%s%*s", c ? " " : "", cols[c].width, h);
            else
                k += snprintf(line + k, 8192 - k, "%s%-*s", c ? " " : "", last ? 0 : cols[c].width, h);
        }
        emit(line, width);
    }
    for (int i = 0; i < norder; i++) {
        int k = 0;
        for (int c = 0; c < ncols && k < 8000; c++) {
            cell(order[i], cols[c].spec, cellbuf, sizeof(cellbuf));
            int last = c + 1 == ncols;
            if (cols[c].spec->align == AR)
                k += snprintf(line + k, 8192 - k, "%s%*s", c ? " " : "", cols[c].width, cellbuf);
            else
                k += snprintf(line + k, 8192 - k, "%s%-*s", c ? " " : "", last ? 0 : cols[c].width,
                              cellbuf);
        }
        emit(line, width);
    }
    return norder ? 0 : 1;
}
