/*
 * memhog: use more memory than the machine has, and check that every byte
 * comes back. A test of swapping (make test's swap stage runs `memhog 80` on
 * a 64 MiB machine) and a way to watch it: `vmstat` in the monitor, /proc/vmstat,
 * /proc/swaps, /proc/<pid>/maps (the swap column).
 *
 *   memhog [-q] [-h SECONDS] MB
 *
 * Maps MB MiB of anonymous memory, writes a pattern that depends on each
 * word's address into all of it, then checks it all. Then forks: the child
 * checks everything again and rewrites a quarter of the pages (copies on
 * write, possibly of swapped-out pages shared with the parent); the parent
 * waits and checks that its own pages are unchanged. Prints the paging
 * counters that changed. Exit status 0 if every word was right. -h waits
 * that long after filling, before checking (to try swapoff meanwhile);
 * -q says less.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096
#define WORDS (PAGE / 8)

static unsigned long pattern(unsigned long page, unsigned long word, unsigned long gen)
{
    unsigned long x = (page * WORDS + word) * 0x9e3779b97f4a7c15UL ^ gen;
    return x ^ (x >> 29);
}

static void fill(unsigned long *mem, unsigned long pages, unsigned long gen, unsigned long step)
{
    for (unsigned long p = 0; p < pages; p += step)
        for (unsigned long w = 0; w < WORDS; w++)
            mem[p * WORDS + w] = pattern(p, w, gen);
}

/* Wrong words, reporting the first few. */
static unsigned long check(const char *who, unsigned long *mem, unsigned long pages, unsigned long gen,
                           unsigned long gen2, unsigned long step2)
{
    unsigned long bad = 0;
    for (unsigned long p = 0; p < pages; p++) {
        unsigned long g = step2 && p % step2 == 0 ? gen2 : gen;
        for (unsigned long w = 0; w < WORDS; w++) {
            unsigned long want = pattern(p, w, g), got = mem[p * WORDS + w];
            if (got != want && bad++ < 5)
                printf("memhog: %s: page %lu word %lu: %#lx, want %#lx\n", who, p, w, got, want);
        }
    }
    return bad;
}

static long counter(const char *name)
{
    static char buf[2048];
    int fd = open("/proc/vmstat", O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    size_t len = strlen(name);
    for (char *l = buf; *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : l + strlen(l))
        if (!strncmp(l, name, len) && l[len] == ' ')
            return atol(l + len + 1);
    return -1;
}

int main(int argc, char **argv)
{
    int quiet = 0, hold = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (!strcmp(argv[i], "-h") && i + 1 < argc)
            hold = atoi(argv[++i]);
        else
            break;
    }
    if (i != argc - 1 || atoi(argv[i]) <= 0) {
        fprintf(stderr, "usage: memhog [-q] [-h SECONDS] MB\n");
        return 2;
    }
    unsigned long mb = atoi(argv[i]), pages = mb * 256;
    static const char *names[] = { "swap_outs", "swap_ins", "major_faults", "unmapped", "direct_reclaims" };
    long before[5];
    for (int i = 0; i < 5; i++)
        before[i] = counter(names[i]);

    unsigned long *mem = mmap(0, pages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("memhog: mmap");
        return 1;
    }
    if (!quiet)
        printf("memhog: filling %lu MiB\n", mb);
    fill(mem, pages, 1, 1);
    if (hold) {
        printf("memhog: %lu MiB filled; checking in %d s\n", mb, hold);
        fflush(stdout);
        sleep_ms((unsigned long)hold * 1000);
    }
    unsigned long bad = check("parent", mem, pages, 1, 0, 0);

    pid_t pid = fork();
    if (pid < 0) {
        perror("memhog: fork");
        return 1;
    }
    if (pid == 0) {
        unsigned long b = check("child", mem, pages, 1, 0, 0);
        fill(mem, pages, 2, 4);                 /* a quarter of it, copied on write */
        b += check("child after writing", mem, pages, 1, 2, 4);
        _exit(b ? 1 : 0);
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("memhog: the child failed (status %#x)\n", st);
        bad++;
    }
    bad += check("parent after the child", mem, pages, 1, 0, 0);
    munmap(mem, pages * PAGE);

    printf("memhog: %s: %lu MiB, %lu wrong words;", bad ? "FAILED" : "ok", mb, bad);
    for (int i = 0; i < 5; i++)
        printf(" %s %ld", names[i], counter(names[i]) - before[i]);
    printf("\n");
    return bad ? 1 : 0;
}
