/*
 * pmap: a process's memory, mapping by mapping.
 *
 *   pmap [-q] PID...
 *
 * For each mapping: its address, size, what is resident and in swap, dirty
 * file pages, permissions, kind and what it maps (a file and offset, or the
 * program's text/data, the heap, the stack, anonymous memory); then totals,
 * and the totals by kind. -q: the mappings only. From /proc/PID/maps.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct kind {
    char name[8];
    unsigned long kib, rss, swap;
};

static int pmap(const char *pid, int quiet)
{
    static char buf[16384];
    char path[64], cmd[128] = "";
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid);
    long n = read_file(path, cmd, sizeof(cmd));
    for (long i = 0; i < n - 1; i++)
        if (!cmd[i])
            cmd[i] = ' ';
    snprintf(path, sizeof(path), "/proc/%s/maps", pid);
    if (read_file(path, buf, sizeof(buf)) <= 0) {
        fprintf(stderr, "pmap: %s: no such process\n", pid);
        return 1;
    }
    printf("%s:   %s\n", pid, cmd);
    printf("%-16s %8s %8s %8s %7s %-5s %-6s %s\n", "Address", "Kbytes", "RSS", "Swap", "Dirty", "Mode",
           "Kind", "Mapping");
    unsigned long tk = 0, tr = 0, ts = 0, td = 0;
    struct kind kinds[8];
    int nk = 0;
    for (char *l = buf; *l;) {
        char *e = strchr(l, '\n');
        if (e)
            *e = 0;
        unsigned long start, end, res, swap;
        char prot[8], kind[8], name[32];
        int used = 0;
        if (sscanf(l, " %lx-%lx %7s %7s %lu pg %lu %31s%n", &start, &end, prot, kind, &res, &swap, name,
                   &used) == 7) {
            const char *rest = l + used;
            unsigned long dirty = 0;
            const char *d = strstr(rest, " dirty)");
            if (d) {
                const char *o = d;
                while (o > rest && o[-1] != '(')
                    o--;
                dirty = strtoul(o, 0, 10);
            }
            const char *map = name;
            char filemap[160];
            const char *sh = strstr(rest, "shared+");
            const char *pv = strstr(rest, "private+");
            if (sh || pv) {
                const char *p = sh ? sh : pv;
                snprintf(filemap, sizeof(filemap), "%s", p);
                map = filemap;
            } else if (!strcmp(kind, "elf")) {
                snprintf(filemap, sizeof(filemap), "[%s]", name);
                map = filemap;
            } else if (!strcmp(kind, "heap") || !strcmp(kind, "stack") || !strcmp(kind, "anon")) {
                snprintf(filemap, sizeof(filemap), "[ %s ]", kind);
                map = filemap;
            }
            unsigned long kib = (end - start) / 1024;
            printf("%016lx %8lu %8lu %8lu %7lu %-5s %-6s %s\n", start, kib, res * 4, swap * 4, dirty * 4,
                   prot, kind, map);
            tk += kib;
            tr += res * 4;
            ts += swap * 4;
            td += dirty * 4;
            int k = 0;
            while (k < nk && strcmp(kinds[k].name, kind))
                k++;
            if (k == nk && nk < 8) {
                memset(&kinds[nk], 0, sizeof(kinds[nk]));
                snprintf(kinds[nk].name, sizeof(kinds[nk].name), "%s", kind);
                nk++;
            }
            if (k < nk) {
                kinds[k].kib += kib;
                kinds[k].rss += res * 4;
                kinds[k].swap += swap * 4;
            }
        }
        if (!e)
            break;
        l = e + 1;
    }
    printf("---------------- -------- -------- -------- -------\n");
    printf("%-16s %8lu %8lu %8lu %7lu\n", "total KiB", tk, tr, ts, td);
    if (!quiet && nk > 1) {
        printf("by kind:");
        for (int k = 0; k < nk; k++) {
            printf("  %s %lu (%lu resident", kinds[k].name, kinds[k].kib, kinds[k].rss);
            if (kinds[k].swap)
                printf(", %lu swapped", kinds[k].swap);
            printf(")");
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int quiet = 0, rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) {
            quiet = 1;
            continue;
        }
        if (any++)
            printf("\n");
        rc |= pmap(!strcmp(argv[i], "self") ? "self" : argv[i], quiet);
    }
    if (!any) {
        fprintf(stderr, "usage: pmap [-q] PID...\n");
        return 2;
    }
    return rc;
}
