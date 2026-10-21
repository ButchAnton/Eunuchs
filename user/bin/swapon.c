/*
 * swapon, swapoff: turn swap space on or off.
 *
 *   swapon               the swap space in use, and how much of it
 *   swapon [VOLUME]      use it ("swap", "disk0p5" or "/dev/disk0p5")
 *   swapoff [VOLUME]     stop: every page in swap comes back into memory
 *                        first (fails if they don't fit)
 *
 * Without a volume, the partition named "swap". Root only, except showing.
 * The kernel uses the swap partition from boot on (boot option noswap: not).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/swap.h>
#include <unistd.h>

static int show(void)
{
    char b[1024];
    int fd = open("/proc/swaps", O_RDONLY);
    long n = fd >= 0 ? read(fd, b, sizeof(b) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0) {
        perror("swapon: /proc/swaps");
        return 1;
    }
    b[n] = 0;
    if (!strncmp(b, "no swap", 7)) {
        printf("no swap space in use\n");
        return 0;
    }
    char vol[32], label[32];
    unsigned long size, used, peak;
    const char *l = strchr(b, '\n');
    if (l && sscanf(l + 1, "%31s %31s %lu %lu %lu", vol, label, &size, &used, &peak) == 5) {
        printf("%-10s %-8s %10s %10s %10s %5s\n", "VOLUME", "LABEL", "SIZE", "USED", "PEAK", "USE%");
        printf("%-10s %-8s %9luK %9luK %9luK %4lu%%\n", vol, label, size, used, peak,
               size ? used * 100 / size : 0);
        const char *rest = strchr(l + 1, '\n');
        if (rest)
            printf("%s", rest + 1);
    } else {
        printf("%s", b);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int off = !strcmp(prog, "swapoff");
    if (argc > 2 || (argc == 2 && argv[1][0] == '-' && strcmp(argv[1], "-a"))) {
        fprintf(stderr, "usage: %s [VOLUME]\n", prog);
        return 2;
    }
    if (!off && argc == 1)
        return show();
    const char *vol = argc == 2 && strcmp(argv[1], "-a") ? argv[1] : "swap";
    if ((off ? swapoff(vol) : swapon(vol, 0)) < 0) {
        const char *why = errno == ENOMEM ? " (the pages in swap don't fit in memory)"
                        : errno == EBUSY ? (off ? " (still in use)" : " (swap is on already)")
                        : errno == EINVAL ? (off ? " (not the swap in use)" : " (no swap header there)")
                        : errno == EPERM ? " (root only)" : "";
        fprintf(stderr, "%s: %s: %s%s\n", prog, vol, strerror(errno), why);
        return 1;
    }
    return off ? 0 : show();
}
