/*
 * kinstall: install a kernel for the boot loader, keeping the one it
 * replaces. eboot boots /kernel.elf on the boot partition (mounted on /boot)
 * and, if that one is missing or bad, the one on boot.old (/boot.old).
 *
 *   kinstall                 the kernels in the two slots
 *   kinstall [-r] KERNEL     KERNEL to /boot, the kernel there to /boot.old
 *                            (nothing if KERNEL is already in /boot); -r then
 *                            reboots into it
 *   kinstall --rollback      swap the two (the previous kernel boots next)
 *
 * Each copy is written beside its destination, synced and renamed over it:
 * a crash leaves the old kernel or the new one, never part of one. On a disk
 * without boot partitions the slots are /boot/kernel.elf and
 * /boot/kernel.old.elf on the root filesystem, as eboot reads them. The
 * Mac's eudisk kernel install does the same to a disk image.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *slot[2] = { "/boot/kernel.elf", "/boot.old/kernel.elf" };

static int fail(const char *what)
{
    fprintf(stderr, "kinstall: %s: %s\n", what, strerror(errno));
    return 1;
}

/* A whole file, malloc'ed (NULL with errno set). */
static char *slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) == 0 && (buf = malloc(st.st_size ? st.st_size : 1)) != NULL) {
        size_t got = 0;
        long n;
        while (got < (size_t)st.st_size && (n = read(fd, buf + got, st.st_size - got)) > 0)
            got += n;
        *len = got;
    }
    close(fd);
    return buf;
}

/* "built ... by ..." from the kernel's boot banner; 0 if there is none. */
static int built(const char *k, size_t len, char *out, size_t size)
{
    static const char key[] = "Eunuchs 0.1 (riscv64), ";
    snprintf(out, size, "(no banner)");
    for (size_t i = 0; i + sizeof(key) < len; i++) {
        if (k[i] != 'E' || memcmp(k + i, key, sizeof(key) - 1))
            continue;
        const char *s = k + i + sizeof(key) - 1;
        size_t n = 0;
        while (s + n < k + len && s[n] && s[n] != '\n' && n < size - 1)
            n++;
        snprintf(out, size, "%.*s", (int)n, s);
        return 1;
    }
    return 0;
}

/* A RISC-V 64-bit ELF executable with the Eunuchs boot banner in it. */
static int is_kernel(const char *k, size_t len)
{
    char b[128];
    return len >= 64 && !memcmp(k, "\177ELF", 4) && k[4] == 2 && k[16] == 2 && (unsigned char)k[18] == 243 &&
           built(k, len, b, sizeof(b));
}

/* Write data as path: path.new, synced, renamed over path. */
static int put(const char *path, const char *data, size_t len)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return fail(tmp);
    for (size_t done = 0; done < len;) {
        long n = write(fd, data + done, len - done);
        if (n <= 0) {
            fail(tmp);
            close(fd);
            unlink(tmp);
            return 1;
        }
        done += n;
    }
    if (fsync(fd) < 0 || close(fd) < 0 || rename(tmp, path) < 0) {
        fail(path);
        unlink(tmp);
        return 1;
    }
    return 0;
}

/* /boot and /boot.old are partitions of their own, or the root's /boot. */
static void find_slots(void)
{
    struct stat r, b;
    if (stat("/", &r) == 0 && stat("/boot", &b) == 0 && r.st_dev == b.st_dev)
        slot[1] = "/boot/kernel.old.elf";
}

static int status(void)
{
    for (int i = 0; i < 2; i++) {
        size_t len;
        char *k = slurp(slot[i], &len), b[128];
        if (!k) {
            printf("%-22s %s\n", slot[i], strerror(errno));
            continue;
        }
        built(k, len, b, sizeof(b));
        printf("%-22s %5lu KiB  %s%s\n", slot[i], (unsigned long)(len / 1024),
               is_kernel(k, len) ? "" : "NOT A EUNUCHS KERNEL  ", b);
        free(k);
    }
    return 0;
}

static int install(const char *path, int then_reboot)
{
    size_t len, curlen = 0;
    char *k = slurp(path, &len), b[128];
    if (!k)
        return fail(path);
    if (!is_kernel(k, len)) {
        fprintf(stderr, "kinstall: %s: not a Eunuchs kernel (a RISC-V ELF executable with its banner)\n", path);
        return 1;
    }
    char *cur = slurp(slot[0], &curlen);
    if (!cur && errno != ENOENT)
        return fail(slot[0]);
    if (cur && curlen == len && !memcmp(cur, k, len)) {
        printf("kinstall: that kernel is in %s already\n", slot[0]);
    } else {
        if (cur) {
            if (put(slot[1], cur, curlen))
                return 1;
            built(cur, curlen, b, sizeof(b));
            printf("%s: %s (was in %s)\n", slot[1], b, slot[0]);
        }
        if (put(slot[0], k, len))
            return 1;
        built(k, len, b, sizeof(b));
        printf("%s: %s\n", slot[0], b);
    }
    sync();
    if (then_reboot && reboot(RB_AUTOBOOT) < 0)
        return fail("reboot");
    return 0;
}

static int rollback(void)
{
    size_t la, lb;
    char *a = slurp(slot[0], &la), *old = slurp(slot[1], &lb), b[128];
    if (!old)
        return fail(slot[1]);
    if (a && put(slot[1], a, la))
        return 1;
    if (put(slot[0], old, lb))
        return 1;
    if (!a)
        unlink(slot[1]);                /* /boot was empty: the kernel moved, not copied */
    sync();
    built(old, lb, b, sizeof(b));
    printf("%s: %s (was in %s)\n", slot[0], b, slot[1]);
    return 0;
}

int main(int argc, char **argv)
{
    find_slots();
    if (argc == 1)
        return status();
    if (argc == 2 && !strcmp(argv[1], "--rollback"))
        return rollback();
    if (argc == 2 && argv[1][0] != '-')
        return install(argv[1], 0);
    if (argc == 3 && !strcmp(argv[1], "-r"))
        return install(argv[2], 1);
    fprintf(stderr, "usage: kinstall                 the kernels in the boot slots\n"
                    "       kinstall [-r] KERNEL     install KERNEL (-r: and reboot)\n"
                    "       kinstall --rollback      swap the two kernels\n");
    return 2;
}
