/*
 * dropcache: empty the page cache, to measure things with it cold.
 *
 *   dropcache
 *
 * Root only. Dirty pages are written to their files first; pages in use
 * (mapped by a process) stay. Writes /proc/drop_caches.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long cached_kib(void)
{
    char b[4096];
    int fd = open("/proc/vmstat", O_RDONLY);
    long n = fd >= 0 ? read(fd, b, sizeof(b) - 1) : -1;
    if (fd >= 0)
        close(fd);
    b[n > 0 ? n : 0] = 0;
    unsigned long kib = 0;
    const char *keys[] = { "\nfile_pages ", "\nswapcache_pages " };
    for (int i = 0; i < 2; i++) {
        const char *l = strstr(b, keys[i]);
        if (l)
            kib += strtoul(l + strlen(keys[i]), 0, 10) * 4;
    }
    return kib;
}

int main(void)
{
    unsigned long before = cached_kib();
    int fd = open("/proc/drop_caches", O_WRONLY);
    if (fd < 0 || write(fd, "1\n", 2) != 2) {
        fprintf(stderr, "dropcache: /proc/drop_caches: %s%s\n", strerror(errno),
                errno == EACCES ? " (root only)" : "");
        return 1;
    }
    close(fd);
    unsigned long after = cached_kib();
    printf("dropcache: %lu KiB dropped; %lu KiB still cached (in use)\n",
           before > after ? before - after : 0, after);
    return 0;
}
