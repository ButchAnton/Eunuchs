/*
 * free: memory and swap, in one table.
 *
 *   free [-k|-m|-g|-h] [-w] [-s SECONDS [-c COUNT]]
 *
 *   total      RAM (less what is reserved: firmware, kernel image)
 *   used       in use by programs and the kernel (not counting the caches)
 *   free       not in use at all
 *   cache      page cache (files) and swap cache: given back when needed
 *   available  free + cache that is clean, roughly what a program could get
 *   Swap       slots in the swap partition: total, used, free
 *
 * Units: KiB (-k, the default), MiB (-m), GiB (-g), or human (-h).
 * -w shows the file and swap caches separately. -s repeats.
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

static char unit = 'k';

static const char *fmt(unsigned long kib, char *b)
{
    switch (unit) {
    case 'm': snprintf(b, 24, "%lu", kib / 1024); break;
    case 'g': snprintf(b, 24, "%lu", kib / (1024 * 1024)); break;
    case 'h':
        if (kib < 1024)
            snprintf(b, 24, "%luKi", kib);
        else if (kib < 1024 * 1024)
            snprintf(b, 24, "%lu.%luMi", kib / 1024, kib % 1024 * 10 / 1024);
        else
            snprintf(b, 24, "%lu.%luGi", kib / (1024 * 1024), kib % (1024 * 1024) * 10 / (1024 * 1024));
        break;
    default: snprintf(b, 24, "%lu", kib); break;
    }
    return b;
}

static int show(int wide)
{
    char buf[4096];
    if (read_file("/proc/vmstat", buf, sizeof(buf)) <= 0) {
        perror("free: /proc/vmstat");
        return 1;
    }
    unsigned long total = (num(buf, "total") - num(buf, "reserved")) * 4;
    unsigned long freek = num(buf, "free") * 4;
    unsigned long file = num(buf, "file_pages") * 4, swc = num(buf, "swapcache_pages") * 4;
    unsigned long dirty = num(buf, "dirty") * 4;
    unsigned long cache = file + swc;
    unsigned long used = total > freek + cache ? total - freek - cache : 0;
    unsigned long avail = freek + (cache > dirty ? cache - dirty : 0);
    unsigned long stotal = num(buf, "swap_total") * 4, sused = num(buf, "swap_used") * 4;
    char a[24], b[24], c[24], d[24], e[24], f[24];
    if (wide) {
        printf("%-6s %12s %12s %12s %12s %12s %12s\n", "", "total", "used", "free", "file cache",
               "swap cache", "available");
        printf("%-6s %12s %12s %12s %12s %12s %12s\n", "Mem:", fmt(total, a), fmt(used, b), fmt(freek, c),
               fmt(file, d), fmt(swc, e), fmt(avail, f));
    } else {
        printf("%-6s %12s %12s %12s %12s %12s\n", "", "total", "used", "free", "cache", "available");
        printf("%-6s %12s %12s %12s %12s %12s\n", "Mem:", fmt(total, a), fmt(used, b), fmt(freek, c),
               fmt(cache, d), fmt(avail, e));
    }
    printf("%-6s %12s %12s %12s\n", "Swap:", fmt(stotal, a), fmt(sused, b), fmt(stotal - sused, c));
    return 0;
}

int main(int argc, char **argv)
{
    int wide = 0;
    double secs = 0;
    long count = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "-m") || !strcmp(argv[i], "-g") ||
            !strcmp(argv[i], "-h"))
            unit = argv[i][1];
        else if (!strcmp(argv[i], "-w"))
            wide = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: free [-k|-m|-g|-h] [-w] [-s SECONDS [-c COUNT]]\n");
            return 2;
        }
    }
    if (secs <= 0)
        return show(wide);
    for (long n = 0; count < 0 || n < count; n++) {
        if (n) {
            struct timespec ts = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
            nanosleep(&ts, 0);
            printf("\n");
        }
        if (show(wide))
            return 1;
        fflush(stdout);
    }
    return 0;
}
