/*
 * fincore: how much of each file is in the page cache.
 *
 *   fincore [-v] FILE...
 *
 * Maps each file (without touching its pages) and asks mincore which are in
 * memory. -v draws the file as a strip of 64 cells: # all of that part
 * cached, + some of it, . none.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE 4096

static const char *human(unsigned long b, char *out)
{
    if (b < 1024)
        snprintf(out, 16, "%luB", b);
    else if (b < 1024 * 1024)
        snprintf(out, 16, "%lu.%luK", b / 1024, b % 1024 * 10 / 1024);
    else
        snprintf(out, 16, "%lu.%luM", b / (1024 * 1024), b % (1024 * 1024) * 10 / (1024 * 1024));
    return out;
}

static int fincore(const char *path, int verbose)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "fincore: %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return 1;
    }
    char a[16], b[16];
    unsigned long pages = (st.st_size + PAGE - 1) / PAGE, res = 0;
    unsigned char *vec = NULL;
    if (pages) {
        void *m = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        vec = malloc(pages);
        if (m == MAP_FAILED || !vec || mincore(m, st.st_size, vec) < 0) {
            fprintf(stderr, "fincore: %s: %s\n", path, strerror(errno));
            close(fd);
            free(vec);
            return 1;
        }
        munmap(m, st.st_size);
        for (unsigned long i = 0; i < pages; i++)
            res += vec[i] & 1;
    }
    close(fd);
    printf("%8s %7lu %7lu %8s  %s\n", human(res * PAGE, a), res, pages, human(st.st_size, b), path);
    if (verbose && pages) {
        char strip[65];
        int cells = pages < 64 ? (int)pages : 64;
        for (int c = 0; c < cells; c++) {
            unsigned long lo = pages * c / cells, hi = pages * (c + 1) / cells, in = 0;
            for (unsigned long i = lo; i < hi; i++)
                in += vec[i] & 1;
            strip[c] = in == hi - lo ? '#' : in ? '+' : '.';
        }
        strip[cells] = 0;
        printf("%27s [%s]\n", "", strip);
    }
    free(vec);
    return 0;
}

int main(int argc, char **argv)
{
    int verbose = 0, rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
            continue;
        }
        if (!any++)
            printf("%8s %7s %7s %8s  %s\n", "RES", "PAGES", "OF", "SIZE", "FILE");
        rc |= fincore(argv[i], verbose);
    }
    if (!any) {
        fprintf(stderr, "usage: fincore [-v] FILE...\n");
        return 2;
    }
    return rc;
}
