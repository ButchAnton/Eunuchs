/*
 * top: processes, CPUs, memory and paging, redrawn in place (curses).
 *
 *   top [-d SECONDS] [-b] [-n FRAMES] [-o FIELD]
 *
 * The screen: a CPU bar per CPU (busy since the last refresh); memory
 * (used | page cache) and swap bars; the paging rates (faults, major
 * faults, pages swapped in and out per second); then the processes, the
 * selected one highlighted. Keys:
 *
 *   P M S T N  sort by CPU, memory, swap, CPU time, pid (again: reverse)
 *   Up Down PgUp PgDn Home End   move the selection
 *   k          kill the selected process (asks for the signal)
 *   /          show only commands containing a string (empty: all)
 *   c          command lines or names        t   kernel threads instead
 *   + -        refresh faster or slower      space  refresh now
 *   h ?        help                          q      quit
 *
 * -b: batch mode, plain text frames on standard output (no terminal
 * needed; -n says how many). Data: /proc/stat, /proc/vmstat, /proc/<pid>/
 * status and cmdline, /proc/threads.
 */
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- reading /proc ---- */

static long read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n, total = 0;
    while (total < (long)size - 1 && (n = read(fd, buf + total, size - 1 - total)) > 0)
        total += n;
    close(fd);
    buf[total > 0 ? total : 0] = 0;
    return total;
}

/* The value after "key " at the start of a line. */
static const char *field(const char *buf, const char *key)
{
    size_t n = strlen(key);
    for (const char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, n) && l[n] == ' ')
            return l + n + 1;
    return NULL;
}

static unsigned long num(const char *buf, const char *key)
{
    const char *v = field(buf, key);
    return v ? strtoul(v, 0, 10) : 0;
}

#define MAXCPU 8
#define MAXPROC 256

struct sample {
    unsigned long uptime_ms, idle_ms[MAXCPU];
    int ncpu, procs, running, blocked;
    unsigned long total, freep, file, swapcache, dirty, swap_total, swap_used;
    unsigned long faults, major, swap_ins, swap_outs;
};

struct proc {
    int pid, ppid, uid;
    char state;
    char name[32], cmd[160];
    unsigned long cpu_ms, rss, swap, majflt, minflt, start_ms;
    double cpu;                         /* % since the last refresh */
    unsigned long dmaj;                 /* major faults since then */
};

static struct sample cur, prev;
static struct proc procs[MAXPROC], old[MAXPROC];
static int nprocs, nold;

static void read_sample(struct sample *s)
{
    char buf[4096];
    memset(s, 0, sizeof(*s));
    if (read_file("/proc/stat", buf, sizeof(buf)) > 0) {
        s->uptime_ms = num(buf, "uptime_ms");
        s->ncpu = (int)num(buf, "cpus");
        if (s->ncpu > MAXCPU)
            s->ncpu = MAXCPU;
        for (int i = 0; i < s->ncpu; i++) {
            char key[16];
            snprintf(key, sizeof(key), "cpu%d", i);
            const char *v = field(buf, key);
            if (v && !strncmp(v, "idle_ms ", 8))
                s->idle_ms[i] = strtoul(v + 8, 0, 10);
        }
        s->procs = (int)num(buf, "procs");
        s->running = (int)num(buf, "procs_running");
        s->blocked = (int)num(buf, "procs_blocked");
    }
    if (read_file("/proc/vmstat", buf, sizeof(buf)) > 0) {
        s->total = (num(buf, "total") - num(buf, "reserved")) * 4;     /* (as free counts it) */
        s->freep = num(buf, "free") * 4;
        s->file = num(buf, "file_pages") * 4;
        s->swapcache = num(buf, "swapcache_pages") * 4;
        s->dirty = num(buf, "dirty") * 4;
        s->swap_total = num(buf, "swap_total") * 4;
        s->swap_used = num(buf, "swap_used") * 4;
        s->faults = num(buf, "faults");
        s->major = num(buf, "major_faults");
        s->swap_ins = num(buf, "swap_ins");
        s->swap_outs = num(buf, "swap_outs");
    }
}

static int load_proc(const char *pid, struct proc *p)
{
    char path[64], buf[2048];
    snprintf(path, sizeof(path), "/proc/%s/status", pid);
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return -1;
    memset(p, 0, sizeof(*p));
    p->pid = (int)num(buf, "pid");
    p->ppid = (int)num(buf, "ppid");
    const char *v = field(buf, "uid");
    p->uid = v ? atoi(v) : 0;
    v = field(buf, "ustate");
    p->state = v ? *v : '?';
    v = field(buf, "name");
    if (v) {
        size_t n = strcspn(v, "\n");
        snprintf(p->name, sizeof(p->name), "%.*s", (int)n, v);
    }
    p->cpu_ms = num(buf, "cpu_ms");
    p->rss = num(buf, "rss_kib");
    p->swap = num(buf, "swap_kib");
    p->majflt = num(buf, "maj_flt");
    p->minflt = num(buf, "min_flt");
    p->start_ms = num(buf, "start_ms");
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid);
    long n = read_file(path, p->cmd, sizeof(p->cmd));
    for (long i = 0; i < n - 1; i++)
        if (!p->cmd[i])
            p->cmd[i] = ' ';
    if (n <= 0)
        snprintf(p->cmd, sizeof(p->cmd), "[%s]", p->name);
    return 0;
}

static void read_procs(void)
{
    memcpy(old, procs, sizeof(procs[0]) * nprocs);
    nold = nprocs;
    nprocs = 0;
    DIR *d = opendir("/proc");
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL && nprocs < MAXPROC) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        struct proc *p = &procs[nprocs];
        if (load_proc(de->d_name, p))
            continue;
        unsigned long dt = cur.uptime_ms - prev.uptime_ms;
        for (int i = 0; i < nold; i++) {
            if (old[i].pid == p->pid) {
                if (dt && p->cpu_ms >= old[i].cpu_ms)
                    p->cpu = 100.0 * (double)(p->cpu_ms - old[i].cpu_ms) / (double)dt;
                p->dmaj = p->majflt - old[i].majflt;
                break;
            }
        }
        nprocs++;
    }
    if (d)
        closedir(d);
}

/* ---- users ---- */

static const char *user_name(int uid)
{
    static char cache[16][2][24];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (atoi(cache[i][0]) == uid)
            return cache[i][1];
    static char buf[2048];
    char name[24];
    snprintf(name, sizeof(name), "%d", uid);
    if (read_file("/etc/passwd", buf, sizeof(buf)) > 0) {
        for (char *l = buf; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l)) {
            char *c1 = strchr(l, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c2 && atoi(c2 + 1) == uid) {
                snprintf(name, sizeof(name), "%.*s", (int)(c1 - l), l);
                break;
            }
        }
    }
    if (ncache < 16) {
        snprintf(cache[ncache][0], 24, "%d", uid);
        snprintf(cache[ncache][1], 24, "%s", name);
        return cache[ncache++][1];
    }
    static char tmp[24];
    snprintf(tmp, sizeof(tmp), "%s", name);
    return tmp;
}

/* ---- formatting ---- */

/* KiB as 512K, 40.2M, 1.1G (width 6). */
static const char *human(unsigned long kib, char *b)
{
    if (kib < 1024)
        snprintf(b, 8, "%luK", kib);
    else if (kib < 10 * 1024)
        snprintf(b, 8, "%lu.%luM", kib / 1024, kib % 1024 * 10 / 1024);
    else if (kib < 1024 * 1024)
        snprintf(b, 8, "%luM", kib / 1024);
    else
        snprintf(b, 8, "%lu.%luG", kib / (1024 * 1024), kib % (1024 * 1024) * 10 / (1024 * 1024));
    return b;
}

static void cputime(unsigned long ms, char *b)
{
    unsigned long m = ms / 60000, s = ms / 1000 % 60, h = ms / 10 % 100;
    snprintf(b, 16, "%lu:%02lu.%02lu", m, s, h);
}

/* ---- sorting and selection ---- */

enum { BY_CPU, BY_MEM, BY_SWAP, BY_TIME, BY_PID };
static int sort_by = BY_CPU, reverse_sort;
static char filter[32];
static int show_cmd = 1, show_threads;
static int delay_ms = 1500;
static int sel, top_row;               /* selection and scroll, in the shown list */
static int shown[MAXPROC], nshown;

static int cmp(const void *a, const void *b)
{
    const struct proc *x = &procs[*(const int *)a], *y = &procs[*(const int *)b];
    long d = 0;
    switch (sort_by) {
    case BY_CPU: d = x->cpu > y->cpu ? -1 : x->cpu < y->cpu ? 1 : 0; break;
    case BY_MEM: d = (long)y->rss - (long)x->rss; break;
    case BY_SWAP: d = (long)y->swap - (long)x->swap; break;
    case BY_TIME: d = (long)y->cpu_ms - (long)x->cpu_ms; break;
    case BY_PID: d = x->pid - y->pid; break;
    }
    if (!d)
        d = x->pid - y->pid;
    return (int)(reverse_sort ? -d : d);
}

static void order(void)
{
    nshown = 0;
    for (int i = 0; i < nprocs; i++)
        if (!filter[0] || strstr(procs[i].cmd, filter))
            shown[nshown++] = i;
    qsort(shown, nshown, sizeof(shown[0]), cmp);
    if (sel >= nshown)
        sel = nshown - 1;
    if (sel < 0)
        sel = 0;
}

/* ---- drawing: curses, or plain lines for -b ---- */

static int batch;
static char line[512];                  /* batch: the line being built */

enum { C_OK = 1, C_WARN, C_HOT, C_HEAD, C_CACHE, C_SWAP, C_BAR, C_TITLE, C_DIM };

static void put(int y, int x, attr_t a, const char *s)
{
    if (batch) {
        (void)y;
        size_t len = strlen(line);
        while ((int)len < x && len < sizeof(line) - 1)
            line[len++] = ' ';
        line[len] = 0;
        snprintf(line + len, sizeof(line) - len, "%s", s);
        return;
    }
    attrset(a);
    mvaddnstr(y, x, s, COLS - x > 0 ? COLS - x : 0);
    attrset(A_NORMAL);
}

static void putf(int y, int x, attr_t a, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    put(y, x, a, b);
}

static void end_line(void)
{
    if (batch) {
        printf("%s\n", line);
        line[0] = 0;
    }
}

/*
 * A bar: label [||||||||      text], `width` cells in all. Segments of
 * fractions f[] (summing to <= 1) in colors c[]; with Unicode, eighth blocks
 * make the ends smooth.
 */
static void bar(int y, int x, int width, const char *label, int nseg, const double *f, const int *c,
                const char *text)
{
    put(y, x, A_BOLD | COLOR_PAIR(C_TITLE), label);
    int lx = x + (int)strlen(label);
    put(y, lx, A_BOLD, "[");
    int inner = width - (int)strlen(label) - 2;
    if (inner < 4)
        return;
    int tlen = (int)strlen(text);
    if (batch) {
        char b[256];
        int n = 0;
        for (int s = 0; s < nseg; s++) {
            int cells = (int)(f[s] * inner + 0.5);
            for (int k = 0; k < cells && n < inner && n < 255; k++)
                b[n++] = s == 0 ? '|' : s == 1 ? '+' : '#';
        }
        while (n < inner - tlen && n < 255)
            b[n++] = ' ';
        b[n] = 0;
        put(y, lx + 1, 0, b);
        put(y, lx + 1 + inner - tlen, 0, text);
        put(y, lx + 1 + inner, 0, "]");
        return;
    }
    static const unsigned eighths[] = { 0, 0x258f, 0x258e, 0x258d, 0x258c, 0x258b, 0x258a, 0x2589 };
    int pos = 0;                        /* in eighths of a cell */
    move(y, lx + 1);
    for (int s = 0; s < nseg; s++) {
        int e = (int)(f[s] * inner * 8 + 0.5);
        if (pos + e > inner * 8)
            e = inner * 8 - pos;
        attrset(COLOR_PAIR(c[s]));
        for (; e >= 8; e -= 8, pos += 8)
            addch(ACS_BLOCK);
        if (e > 0) {
            addch(acs_map[11] == '#' ? '#' : eighths[e]);
            pos = (pos / 8 + 1) * 8;
        }
    }
    attrset(A_NORMAL);
    for (int k = pos / 8; k < inner; k++)
        addch(' ');
    put(y, lx + 1 + inner - tlen, A_BOLD, text);
    put(y, lx + 1 + inner, A_BOLD, "]");
}

static int cpu_color(double pct) { return pct < 50 ? C_OK : pct < 85 ? C_WARN : C_HOT; }

static int draw_header(void)
{
    char a[16], b[16], c[16], d[16];
    int y = 0;
    time_t now = time(0);
    struct tm tm;
    gmtime_r(&now, &tm);
    unsigned long up = cur.uptime_ms / 1000;
    putf(y, 0, A_BOLD, "top - %02d:%02d:%02d up %lu:%02lu:%02lu", tm.tm_hour, tm.tm_min, tm.tm_sec,
         up / 3600, up / 60 % 60, up % 60);
    putf(y, 32, 0, "%d processes: %d running, %d blocked;  %d CPUs", cur.procs, cur.running,
         cur.blocked, cur.ncpu);
    end_line();
    y++;
    /* CPUs: two to a row (four on a wide screen, one on a narrow one) */
    int cols = batch ? 80 : COLS;
    int per = cols >= 160 ? 4 : cols >= 60 ? 2 : 1;
    int w = (cols - 1) / per - 1;
    unsigned long dt = cur.uptime_ms - prev.uptime_ms;
    for (int i = 0; i < cur.ncpu; i++) {
        double busy = 0;
        if (dt) {
            unsigned long di = cur.idle_ms[i] - prev.idle_ms[i];
            busy = di > dt ? 0 : 100.0 * (double)(dt - di) / (double)dt;
        }
        char lab[8], t[16];
        snprintf(lab, sizeof(lab), "CPU%d", i);
        snprintf(t, sizeof(t), "%5.1f%%", busy);
        double f = busy / 100;
        int col = cpu_color(busy);
        bar(y, 1 + (i % per) * (w + 1), w, lab, 1, &f, &col, t);
        if (i % per == per - 1 || i == cur.ncpu - 1) {
            end_line();
            y++;
        }
    }
    /* memory: used by programs | page cache, of the total; swap */
    unsigned long used = cur.total - cur.freep, cache = cur.file + cur.swapcache;
    unsigned long prog = used > cache ? used - cache : 0;
    double mf[2] = { cur.total ? (double)prog / cur.total : 0, cur.total ? (double)cache / cur.total : 0 };
    int mc[2] = { C_OK, C_CACHE };
    char t[48];
    snprintf(t, sizeof(t), "%s used + %s cache / %s", human(prog, a), human(cache, b), human(cur.total, c));
    int mw = per >= 2 ? (cols - 1) / 2 - 1 : cols - 2;
    bar(y, 1, mw, "Mem ", 2, mf, mc, t);
    if (per < 2) {
        end_line();
        y++;
    }
    double sf = cur.swap_total ? (double)cur.swap_used / cur.swap_total : 0;
    int sc = C_SWAP;
    if (cur.swap_total)
        snprintf(t, sizeof(t), "%s / %s", human(cur.swap_used, a), human(cur.swap_total, b));
    else
        snprintf(t, sizeof(t), "no swap");
    bar(y, per >= 2 ? 1 + mw + 1 : 1, mw, "Swp ", 1, &sf, &sc, t);
    end_line();
    y++;
    /* paging per second */
    double secs = dt ? dt / 1000.0 : 1;
    putf(y, 1, 0, "Paging: %6.0f faults/s  %5.0f major/s   swap in %5.0f/s  out %5.0f/s   dirty %s",
         (cur.faults - prev.faults) / secs, (cur.major - prev.major) / secs,
         (cur.swap_ins - prev.swap_ins) / secs, (cur.swap_outs - prev.swap_outs) / secs,
         human(cur.dirty, d));
    end_line();
    return y + 1;
}

static const char *const sort_names[] = { "CPU%", "RES", "SWAP", "TIME+", "PID" };

static void draw_procs(int y, int rows)
{
    int cols = batch ? 120 : COLS;
    char head[256];
    snprintf(head, sizeof(head), "%5s %-8s S %5s %5s %6s %6s %6s %9s  %s", "PID", "USER", "CPU%", "MEM%",
             "RES", "SWAP", "MAJ/s", "TIME+", show_cmd ? "COMMAND" : "NAME");
    char padded[512];
    snprintf(padded, sizeof(padded), "%-*s", cols < 500 ? cols : 499, head);
    put(y, 0, A_BOLD | COLOR_PAIR(C_HEAD), padded);
    /* the sort column, marked */
    const char *mark = strstr(head, sort_names[sort_by]);
    if (mark && !batch)
        put(y, (int)(mark - head), A_BOLD | A_UNDERLINE | COLOR_PAIR(C_HEAD), sort_names[sort_by]);
    end_line();
    y++;
    if (sel < top_row)
        top_row = sel;
    if (sel >= top_row + rows)
        top_row = sel - rows + 1;
    for (int r = 0; r < rows && top_row + r < nshown; r++) {
        struct proc *p = &procs[shown[top_row + r]];
        char res[8], sw[8], tm[16], row[512];
        cputime(p->cpu_ms, tm);
        double mem = cur.total ? 100.0 * p->rss / cur.total : 0;
        snprintf(row, sizeof(row), "%5d %-8.8s %c %5.1f %5.1f %6s %6s %6lu %9s  %s", p->pid,
                 user_name(p->uid), p->state, p->cpu, mem, human(p->rss, res),
                 p->swap ? human(p->swap, sw) : "-", p->dmaj, tm, show_cmd ? p->cmd : p->name);
        bool is_sel = !batch && top_row + r == sel;
        attr_t a = is_sel ? A_REVERSE : p->state == 'R' ? A_BOLD : 0;
        snprintf(padded, sizeof(padded), "%-*s", cols < 500 ? cols : 499, row);
        put(y + r, 0, a, padded);
        if (!batch && !is_sel && p->swap)
            put(y + r, 36 + 6 - (int)strlen(sw), COLOR_PAIR(C_SWAP), sw);   /* (the SWAP column) */
        if (!batch && !is_sel && p->cpu >= 50)
            putf(y + r, 17, COLOR_PAIR(cpu_color(p->cpu)) | A_BOLD, "%5.1f", p->cpu);
        end_line();
    }
}

static void draw_threads(int y, int rows)
{
    static char buf[16384];
    read_file("/proc/threads", buf, sizeof(buf));
    char *l = buf;
    for (int r = 0; *l && r < rows + 1; r++) {
        char *e = strchr(l, '\n');
        if (e)
            *e = 0;
        put(y + r, 0, r == 0 ? A_BOLD | COLOR_PAIR(C_HEAD) : 0, l);
        end_line();
        if (!e)
            break;
        l = e + 1;
    }
}

static char message[128];

static void draw_footer(void)
{
    static const char *const keys[][2] = {
        { "h", "Help" }, { "P", "CPU" }, { "M", "Mem" }, { "S", "Swap" }, { "T", "Time" },
        { "N", "PID" }, { "k", "Kill" }, { "/", "Filter" }, { "t", "Threads" }, { "+-", "Delay" },
        { "q", "Quit" },
    };
    move(LINES - 1, 0);
    clrtoeol();
    if (message[0]) {
        put(LINES - 1, 0, A_BOLD, message);
        return;
    }
    int x = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && x < COLS - 8; i++) {
        put(LINES - 1, x, A_BOLD, keys[i][0]);
        x += (int)strlen(keys[i][0]);
        char lab[16];
        snprintf(lab, sizeof(lab), " %-6s", keys[i][1]);
        put(LINES - 1, x, COLOR_PAIR(C_BAR), lab);
        x += (int)strlen(lab) + 1;
    }
    char d[32];
    snprintf(d, sizeof(d), " %.1fs%s%s%s", delay_ms / 1000.0, filter[0] ? " /" : "", filter,
             reverse_sort ? " (reversed)" : "");
    put(LINES - 1, x, COLOR_PAIR(C_BAR), d);
}

static void draw(void)
{
    erase();
    int y = draw_header();
    put(y, 0, 0, "");
    if (show_threads)
        draw_threads(y + 1, LINES - y - 3);
    else
        draw_procs(y + 1, LINES - y - 3);
    draw_footer();
    refresh();
}

static void help(void)
{
    static const char *const text[] = {
        "top: processes, CPUs, memory and paging",
        "",
        "  P M S T N   sort by CPU, memory, swap, time, pid",
        "              (the same key again reverses)",
        "  Up/Down PgUp/PgDn Home/End   select",
        "  k           kill the selected process",
        "  /           filter commands (empty: all)",
        "  c           command lines / names",
        "  t           kernel threads / processes",
        "  + -         refresh faster / slower",
        "  space       refresh now",
        "  q           quit",
        "",
        "Mem: programs (green) and the page cache (blue);",
        "Swp: pages in swap. MAJ/s: faults that read a",
        "file or swap. More: vmstat, free, pmap, fincore.",
        "",
        "             (any key)",
    };
    int n = (int)(sizeof(text) / sizeof(text[0])), w = 54;
    WINDOW *win = newwin(n + 2, w, (LINES - n - 2) / 2 > 0 ? (LINES - n - 2) / 2 : 0,
                         (COLS - w) / 2 > 0 ? (COLS - w) / 2 : 0);
    werase(win);
    wattrset(win, COLOR_PAIR(C_TITLE) | A_BOLD);
    box(win, 0, 0);
    wattrset(win, A_NORMAL);
    for (int i = 0; i < n; i++)
        mvwaddstr(win, i + 1, 2, text[i]);
    refresh();
    delwin(win);
    timeout(-1);
    getch();
}

/* A line of input in the footer; returns 0 if cancelled (Esc). */
static int prompt(const char *q, char *buf, size_t size)
{
    size_t n = strlen(buf);
    timeout(-1);
    curs_set(1);
    for (;;) {
        snprintf(message, sizeof(message), "%s%s", q, buf);
        draw_footer();
        refresh();
        int c = getch();
        if (c == '\n' || c == '\r') {
            break;
        } else if (c == 27) {
            message[0] = 0;
            curs_set(0);
            return 0;
        } else if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            if (n)
                buf[--n] = 0;
        } else if (c >= 32 && c < 127 && n + 1 < size) {
            buf[n++] = (char)c;
            buf[n] = 0;
        }
    }
    message[0] = 0;
    curs_set(0);
    return 1;
}

static void kill_selected(void)
{
    if (!nshown)
        return;
    struct proc *p = &procs[shown[sel]];
    char q[64], sig[8] = "15";
    snprintf(q, sizeof(q), "kill %d (%s) with signal: ", p->pid, p->name);
    if (!prompt(q, sig, sizeof(sig)))
        return;
    int s = atoi(sig);
    if (kill(p->pid, s) < 0)
        snprintf(message, sizeof(message), "kill %d: %s", p->pid, strerror(errno));
}

static void set_sort(int by)
{
    reverse_sort = sort_by == by ? !reverse_sort : 0;
    sort_by = by;
}

static void sample(void)
{
    prev = cur;
    read_sample(&cur);
    read_procs();
    order();
}

static void usage(void)
{
    fprintf(stderr, "usage: top [-d SECONDS] [-b] [-n FRAMES] [-o cpu|mem|swap|time|pid]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int frames = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b"))
            batch = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            delay_ms = (int)(atof(argv[++i]) * 1000);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            const char *o = argv[++i];
            sort_by = !strcmp(o, "mem") ? BY_MEM : !strcmp(o, "swap") ? BY_SWAP : !strcmp(o, "time") ? BY_TIME
                    : !strcmp(o, "pid") ? BY_PID : BY_CPU;
        } else
            usage();
    }
    if (delay_ms < 100)
        delay_ms = 100;

    read_sample(&cur);
    read_procs();
    napms(250);                         /* a first interval for the rates */
    sample();

    if (batch) {
        for (int f = 0; frames < 0 || f < frames; f++) {
            if (f) {
                napms(delay_ms);
                sample();
                printf("\n");
            }
            draw_header();
            line[0] = 0;
            draw_procs(0, nshown);
            fflush(stdout);
        }
        return 0;
    }

    initscr();
    raw();
    noecho();
    keypad(stdscr, true);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_OK, COLOR_GREEN, -1);
        init_pair(C_WARN, COLOR_YELLOW, -1);
        init_pair(C_HOT, COLOR_RED, -1);
        init_pair(C_HEAD, COLOR_BLACK, COLOR_GREEN);
        init_pair(C_CACHE, COLOR_BLUE, -1);
        init_pair(C_SWAP, COLOR_MAGENTA, -1);
        init_pair(C_BAR, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_TITLE, COLOR_CYAN, -1);
        init_pair(C_DIM, 8, -1);
    }
    int frame = 0;
    for (;;) {
        draw();
        if (frames >= 0 && ++frame >= frames)
            break;
        timeout(delay_ms);
        int c = getch();
        if (c == ERR || c == ' ' || c == KEY_RESIZE) {
            if (c == KEY_RESIZE)
                clear();
            sample();
            continue;
        }
        message[0] = 0;
        int page = LINES > 12 ? LINES - 12 : 1;
        switch (c) {
        case 'q': case 'Q': case 3: case 27:
            endwin();
            return 0;
        case 'P': set_sort(BY_CPU); break;
        case 'M': set_sort(BY_MEM); break;
        case 'S': set_sort(BY_SWAP); break;
        case 'T': set_sort(BY_TIME); break;
        case 'N': set_sort(BY_PID); break;
        case KEY_UP: sel--; break;
        case KEY_DOWN: sel++; break;
        case KEY_PPAGE: sel -= page; break;
        case KEY_NPAGE: sel += page; break;
        case KEY_HOME: sel = 0; break;
        case KEY_END: sel = nshown - 1; break;
        case 'k': kill_selected(); break;
        case '/': prompt("filter: ", filter, sizeof(filter)); sel = 0; break;
        case 'c': show_cmd = !show_cmd; break;
        case 't': show_threads = !show_threads; break;
        case '+': case '=': if (delay_ms > 250) delay_ms -= 250; break;
        case '-': delay_ms += 250; break;
        case 'h': case '?': case KEY_F(1): help(); break;
        case 12: clear(); break;        /* ^L */
        default: beep(); break;
        }
        order();
    }
    endwin();
    return 0;
}
