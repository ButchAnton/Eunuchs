#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int cat(int fd, const char *name)
{
    char buf[4096];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write(1, buf, n) != n)
            return 1;
    if (n < 0) {
        eprintf("cat: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2)
        return cat(0, "stdin");
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            eprintf("cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        rc |= cat(fd, argv[i]);
        close(fd);
    }
    return rc;
}
