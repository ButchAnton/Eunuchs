/*
 * vmstat: paging, memory and CPU activity, a line per interval.
 *
 *   vmstat [-w] [DELAY [COUNT]]   lines: the first since boot, then each DELAY seconds
 *   vmstat -s                     every counter, with what it counts
 *
 *   procs   r  runnable processes        b  blocked (uninterruptible) ones
 *   memory  free, page cache, swap cache, swap used (KiB)
 *   swap    si/so  pages read from / written to swap, per second
 *   faults  min/maj  faults per second; maj read a file or swap
 *   reclaim scan  PTEs looked at   free  pages freed, per second
 *   cpu     busy and idle, % of all CPUs
 *
 * -w: wider columns. From /proc/vmstat and /proc/stat.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static unsigned long num(const char *buf, const char *key)
{
    size_t n = strlen(key);
    for (const char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, n) && l[n] == ' ')
            return strtoul(l + n + 1, 0, 10);
    return 0;
}

struct snap {
    unsigned long ms, idle_ms, ncpu, running, blocked;
    unsigned long freep, file, swapcache, swap_used;
    unsigned long faults, major, swap_ins, swap_outs, scanned, freed;
};

static void take(struct snap *s)
{
    char b[4096];
    memset(s, 0, sizeof(*s));
    if (read_file("/proc/stat", b, sizeof(b)) > 0) {
        s->ms = num(b, "uptime_ms");
        s->ncpu = num(b, "cpus");
        for (unsigned long i = 0; i < s->ncpu; i++) {
            char key[16];
            snprintf(key, sizeof(key), "cpu%lu", i);
            const char *l = strstr(b, key);
            if (l && (l = strstr(l, "idle_ms ")))
                s->idle_ms += strtoul(l + 8, 0, 10);
        }
        s->running = num(b, "procs_running");
        s->blocked = num(b, "procs_blocked");
    }
    if (read_file("/proc/vmstat", b, sizeof(b)) > 0) {
        s->freep = num(b, "free") * 4;
        s->file = num(b, "file_pages") * 4;
        s->swapcache = num(b, "swapcache_pages") * 4;
        s->swap_used = num(b, "swap_used") * 4;
        s->faults = num(b, "faults");
        s->major = num(b, "major_faults");
        s->swap_ins = num(b, "swap_ins");
        s->swap_outs = num(b, "swap_outs");
        s->scanned = num(b, "scanned");
        s->freed = num(b, "freed");
    }
}

static int wide;

static void header(void)
{
    if (wide) {
        printf("--procs-- ---------------memory (KiB)--------------- ---swap/s--- -----faults/s---- "
               "--reclaim/s-- ---cpu---\n");
        printf("   r    b       free      cache   swpcache       swpd     si     so    minor  major "
               "    scan  free  busy idle\n");
    } else {
        printf("procs -----------memory (KiB)----------- -swap/s-- --faults/s-- -reclaim- -cpu-\n");
        printf(" r  b    free   cache swpcach    swpd   si   so  minor major  scan free  us id\n");
    }
}

/* One line: rates between a and b (a zeroed: since boot). */
static void line(const struct snap *a, const struct snap *b)
{
    double secs = (b->ms - a->ms) / 1000.0;
    if (secs <= 0)
        secs = 1;
#define R(f) ((double)(b->f - a->f) / secs)
    unsigned long all = (b->ms - a->ms) * (b->ncpu ? b->ncpu : 1);
    unsigned long idle = b->idle_ms - a->idle_ms;
    int id = all ? (int)(idle * 100 / all) : 100;
    if (id > 100)
        id = 100;
    double minor = R(faults) - R(major);
    if (wide)
        printf("%4lu %4lu %10lu %10lu %10lu %10lu %6.0f %6.0f %8.0f %6.0f %8.0f %5.0f  %4d %4d\n",
               b->running, b->blocked, b->freep, b->file, b->swapcache, b->swap_used, R(swap_ins),
               R(swap_outs), minor, R(major), R(scanned), R(freed), 100 - id, id);
    else
        printf("%2lu %2lu %7lu %7lu %7lu %7lu %4.0f %4.0f %6.0f %5.0f %5.0f %4.0f %3d %3d\n", b->running,
               b->blocked, b->freep, b->file, b->swapcache, b->swap_used, R(swap_ins), R(swap_outs), minor,
               R(major), R(scanned), R(freed), 100 - id, id);
#undef R
    fflush(stdout);
}

static const char *const meanings[][2] = {
    { "total", "pages of RAM" },
    { "reserved", "pages never allocatable (firmware, kernel image)" },
    { "free", "pages free" },
    { "min", "free-page mark: user allocations below it reclaim first" },
    { "low", "free-page mark: kswapd wakes below it" },
    { "high", "free-page mark: kswapd stops above it" },
    { "file_pages", "pages of files in the page cache" },
    { "swapcache_pages", "pages in the swap cache" },
    { "dirty", "cached pages newer than their file or swap slot" },
    { "mapped_cache", "cached pages in use (mapped, or being read)" },
    { "swap_total", "swap slots" },
    { "swap_used", "swap slots in use" },
    { "faults", "page faults (user)" },
    { "major_faults", "faults that read a file or swap" },
    { "cache_hits", "page cache lookups that found the page" },
    { "cache_misses", "page cache lookups that read the file" },
    { "swap_outs", "pages written to swap" },
    { "swap_ins", "pages read from swap" },
    { "swap_out_skipped", "swap-cache pages dropped unwritten (nobody needed them)" },
    { "writebacks", "dirty file pages written to their files" },
    { "scanned", "PTEs reclaim looked at" },
    { "unmapped", "pages reclaim took from processes" },
    { "freed", "pages reclaim freed" },
    { "kswapd_runs", "times kswapd reclaimed" },
    { "direct_reclaims", "reclaims by allocating processes" },
    { "alloc_stalls", "allocations that had to wait for memory" },
    { "oom", "allocations that failed: out of memory" },
    { "asid_bits", "address-space ID bits (0: every switch flushes the TLB)" },
};

static int summary(void)
{
    char b[4096];
    if (read_file("/proc/vmstat", b, sizeof(b)) <= 0) {
        perror("vmstat: /proc/vmstat");
        return 1;
    }
    for (char *l = b; *l;) {
        char *e = strchr(l, '\n');
        if (e)
            *e = 0;
        char key[32];
        unsigned long v;
        if (sscanf(l, "%31s %lu", key, &v) == 2) {
            const char *m = "";
            for (size_t i = 0; i < sizeof(meanings) / sizeof(meanings[0]); i++)
                if (!strcmp(meanings[i][0], key))
                    m = meanings[i][1];
            printf("%12lu  %-17s %s\n", v, key, m);
        }
        if (!e)
            break;
        l = e + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-w"))
            wide = 1;
        else if (!strcmp(argv[i], "-s"))
            return summary();
        else {
            fprintf(stderr, "usage: vmstat [-w] [DELAY [COUNT]] | vmstat -s\n");
            return 2;
        }
    }
    double delay = i < argc ? atof(argv[i++]) : 0;
    long count = i < argc ? atol(argv[i]) : delay > 0 ? -1 : 1;
    struct snap zero, a, b;
    memset(&zero, 0, sizeof(zero));
    take(&a);
    zero.ncpu = a.ncpu;
    header();
    line(&zero, &a);                    /* since boot */
    for (long n = 1; count < 0 || n < count; n++) {
        struct timespec ts = { (time_t)delay, (long)((delay - (time_t)delay) * 1e9) };
        nanosleep(&ts, 0);
        take(&b);
        if (n % 20 == 0)
            header();
        line(&a, &b);
        a = b;
    }
    return 0;
}
