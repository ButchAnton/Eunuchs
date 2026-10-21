/*
 * eudisk: examine and manage Eunuchs disk images from the Mac.
 *
 * An image is a GPT disk with an efs filesystem in each partition (boot,
 * boot.old, root, home) and swap space (swap; `make` creates disk.img), or a
 * bare efs filesystem.
 * Places in an image are written IMAGE:PATH (the root partition) or
 * IMAGE:PART:PATH, e.g. disk.img:/etc/passwd, disk.img:home:/guest.
 *
 *   eudisk create IMAGE NAME:MB...            a new image, empty filesystems
 *                                             (a partition named swap: swap space)
 *   eudisk add IMAGE NAME:MB                  a new partition at the end
 *   eudisk info [--json] IMAGE                partitions, use, kernels
 *   eudisk ls [-lR] [--json] IMAGE[:PART]:PATH
 *   eudisk cat IMAGE[:PART]:PATH
 *   eudisk get [-r] IMAGE[:PART]:PATH LOCAL   copy out
 *   eudisk put [-r] [-m MODE] [-o UID:GID] LOCAL IMAGE[:PART]:PATH
 *   eudisk mkdir [-p] [-m MODE] [-o UID:GID] IMAGE[:PART]:PATH
 *   eudisk rm [-r] IMAGE[:PART]:PATH
 *   eudisk copy IMAGE[:PART]:PATH IMAGE[:PART]:PATH
 *       a tree from one filesystem to another (or the same), keeping owners,
 *       modes, times and hard links; files that exist are left alone
 *   eudisk fsck IMAGE[:PART]
 *   eudisk kernel [--json] IMAGE              the kernels in the boot slots
 *   eudisk kernel install [--to SLOT] IMAGE KERNEL
 *       the kernel in boot goes to boot.old, KERNEL to boot (nothing if
 *       KERNEL is already in boot); --to writes one slot, no rotation
 *   eudisk kernel rollback IMAGE              swap boot and boot.old
 *   eudisk install IMAGE PART SPEC...         system files (make uses this)
 *
 * install keeps your work: it records what it installed in
 * /var/lib/eudisk/manifest on the partition. A file still as installed is
 * replaced; a file you changed (or one it didn't install) is first copied to
 * /var/backup/DATE-TIME/PATH. For a configuration file (a spec starting with
 * +, like +/etc/passwd=user/etc/passwd) your version stays and the new one
 * is written next to it as PATH.new. Files it installed earlier that are no
 * longer in the specs are removed if unchanged (else left, and reported).
 * Specs: /path=hostfile, /path=@/existing (a hard link), /dir/[:mode:uid:gid].
 *
 * Commands that change an image refuse while QEMU has it open (--force).
 * Exit status: 0 done, 1 failed, 2 usage.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "efs.h"
#include "image.h"

static const char *prog = "eudisk";
static bool force, json;

static void usage(void)
{
    fprintf(stderr,
            "usage: eudisk create IMAGE NAME:MB...\n"
            "       eudisk add IMAGE NAME:MB\n"
            "       eudisk info [--json] IMAGE\n"
            "       eudisk ls [-lR] [--json] IMAGE[:PART]:PATH\n"
            "       eudisk cat IMAGE[:PART]:PATH\n"
            "       eudisk get [-r] IMAGE[:PART]:PATH LOCAL\n"
            "       eudisk put [-r] [-m MODE] [-o UID:GID] LOCAL IMAGE[:PART]:PATH\n"
            "       eudisk mkdir [-p] [-m MODE] [-o UID:GID] IMAGE[:PART]:PATH\n"
            "       eudisk rm [-r] IMAGE[:PART]:PATH\n"
            "       eudisk copy IMAGE[:PART]:PATH IMAGE[:PART]:PATH\n"
            "       eudisk fsck IMAGE[:PART]\n"
            "       eudisk kernel [--json] IMAGE\n"
            "       eudisk kernel install [--to SLOT] IMAGE KERNEL\n"
            "       eudisk kernel rollback IMAGE\n"
            "       eudisk install IMAGE PART SPEC...\n"
            "(commands that change an image refuse while QEMU uses it: --force)\n");
    exit(2);
}

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 1;
}

/* ---- images and places ---- */

struct place {
    char image[PATH_MAX];
    char part[64];              /* "" = the default partition */
    char path[1024];
};

/* IMAGE:PATH or IMAGE:PART:PATH (or just IMAGE[:PART] when no path is needed) */
static void parse_place(const char *arg, struct place *pl)
{
    memset(pl, 0, sizeof(*pl));
    const char *c = strchr(arg, ':');
    if (!c) {
        snprintf(pl->image, sizeof(pl->image), "%s", arg);
        strcpy(pl->path, "/");
        return;
    }
    snprintf(pl->image, sizeof(pl->image), "%.*s", (int)(c - arg), arg);
    c++;
    if (*c == '/' || !*c) {
        snprintf(pl->path, sizeof(pl->path), "%s", *c ? c : "/");
        return;
    }
    const char *c2 = strchr(c, ':');
    if (c2) {
        snprintf(pl->part, sizeof(pl->part), "%.*s", (int)(c2 - c), c);
        snprintf(pl->path, sizeof(pl->path), "%s", c2[1] ? c2 + 1 : "/");
    } else {
        snprintf(pl->part, sizeof(pl->part), "%s", c);
        strcpy(pl->path, "/");
    }
    if (pl->path[0] != '/') {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "/%s", pl->path);
        strcpy(pl->path, tmp);
    }
}

/* Is QEMU running with this image? (ps lists its command line) */
static bool image_in_use(const char *path)
{
    char abs[PATH_MAX];
    if (!realpath(path, abs))
        return false;
    FILE *ps = popen("ps -axww -o command= 2>/dev/null", "r");
    if (!ps)
        return false;
    char line[16384];
    bool used = false;
    const char *base = strrchr(abs, '/') ? strrchr(abs, '/') + 1 : abs;
    while (fgets(line, sizeof(line), ps)) {
        size_t cmdlen = strcspn(line, " \n");          /* the program, not its arguments */
        const char *prog_name = line;
        for (const char *q = line; q < line + cmdlen; q++)
            if (*q == '/')
                prog_name = q + 1;
        if (strncmp(prog_name, "qemu-system", 11))
            continue;
        char *f = line;
        while ((f = strstr(f, "file=")) != NULL) {
            f += 5;
            char arg[PATH_MAX];
            size_t n = strcspn(f, ", \n");
            snprintf(arg, sizeof(arg), "%.*s", (int)n, f);
            char a2[PATH_MAX];
            const char *ab = strrchr(arg, '/') ? strrchr(arg, '/') + 1 : arg;
            if ((realpath(arg, a2) && !strcmp(a2, abs)) || (!strcmp(ab, base) && arg[0] != '/'))
                used = true;
        }
    }
    pclose(ps);
    return used;
}

static int load(struct image *im, const char *path, bool writing)
{
    if (writing && !force && image_in_use(path)) {
        fail("%s is in use by QEMU (stop it first, or --force)", path);
        return -1;
    }
    return image_load(im, path, true);
}

static struct part *find_part(struct image *im, const char *name)
{
    struct part *p = image_part(im, name[0] ? name : NULL);
    if (!p)
        fail("%s: no partition %s", im->path, name[0] ? name : "(efs)");
    else if (!p->is_efs) {
        fail("%s: partition %s %s", im->path, p->name, p->is_swap ? "is swap space, not a filesystem"
                                                                     : "holds no efs filesystem");
        p = NULL;
    }
    return p;
}

static bool modified;           /* image_save at the end */

/* ---- create ---- */

static int cmd_create(int argc, char **argv)
{
    if (argc < 2)
        usage();
    struct stat st;
    if (!force && stat(argv[0], &st) == 0)
        return fail("%s exists (remove it first, or --force)", argv[0]);
    struct layout parts[IMAGE_MAX_PARTS];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        char *c = strchr(argv[i], ':');
        if (!c || n == IMAGE_MAX_PARTS)
            usage();
        *c = 0;
        parts[n].name = argv[i];
        parts[n].mb = (uint32_t)atoi(c + 1);
        if (!parts[n].mb)
            return fail("%s: a size in MiB", argv[i]);
        n++;
    }
    struct image im;
    if (image_create(&im, argv[0], parts, n) || image_save(&im))
        return 1;
    printf("%s: %llu MiB, GPT:", argv[0], (unsigned long long)(im.size >> 20));
    for (int i = 0; i < n; i++)
        printf(" %s (%u MiB)", parts[i].name, parts[i].mb);
    printf("\n");
    return 0;
}

static int cmd_add(int argc, char **argv)
{
    char *c = argc == 2 ? strchr(argv[1], ':') : NULL;
    if (!c)
        usage();
    *c = 0;
    uint32_t mb = (uint32_t)atoi(c + 1);
    if (!mb)
        return fail("%s: a size in MiB", argv[1]);
    struct image im;
    if (load(&im, argv[0], true))
        return 1;
    if (image_add(&im, argv[1], mb) || image_save(&im))
        return 1;
    printf("%s: added %s (%u MiB%s); now %llu MiB\n", argv[0], argv[1], mb,
           strcmp(argv[1], SWAP_NAME) ? "" : ", swap space", (unsigned long long)(im.size >> 20));
    return 0;
}

/* ---- kernels in the boot slots ---- */

struct kinfo {
    bool present, valid;
    uint64_t size;
    char banner[160];           /* "built ... by ..." from the kernel's boot banner */
};

static void kernel_info(const uint8_t *data, size_t len, struct kinfo *k)
{
    memset(k, 0, sizeof(*k));
    k->present = true;
    k->size = len;
    bool elf = len >= 64 && !memcmp(data, "\177ELF", 4) && data[4] == 2 && data[18] == 243 && data[16] == 2;
    static const char key[] = "Eunuchs 0.1 (riscv64), ";
    for (size_t i = 0; i + sizeof(key) < len; i++) {
        if (data[i] == 'E' && !memcmp(data + i, key, sizeof(key) - 1)) {
            const char *s = (const char *)data + i + sizeof(key) - 1;
            size_t n = 0;
            while (i + sizeof(key) - 1 + n < len && s[n] && s[n] != '\n' && n < sizeof(k->banner) - 1)
                n++;
            memcpy(k->banner, s, n);
            break;
        }
    }
    k->valid = elf && k->banner[0];             /* a Eunuchs kernel, not just any program */
}

static int slot_kernel(struct image *im, const char *slot, struct kinfo *k, uint8_t **data, size_t *len)
{
    memset(k, 0, sizeof(*k));
    struct part *p = image_part(im, slot);
    if (!p || !p->is_efs)
        return -ENODEV;
    int ino = efs_lookup(&p->fs, "/kernel.elf");
    if (ino < 0)
        return ino;
    size_t n;
    uint8_t *buf = efs_slurp(&p->fs, (uint32_t)ino, &n);
    if (!buf)
        return -ENOMEM;
    kernel_info(buf, n, k);
    if (data) {
        *data = buf;
        *len = n;
    } else {
        free(buf);
    }
    return 0;
}

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 32)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    }
    putchar('"');
}

static void print_kernels(struct image *im)
{
    static const char *slots[] = { "boot", "boot.old" };
    if (json)
        printf("[");
    for (int i = 0; i < 2; i++) {
        struct kinfo k;
        int r = slot_kernel(im, slots[i], &k, NULL, NULL);
        if (json) {
            printf("%s{\"slot\":", i ? "," : "");
            json_str(slots[i]);
            printf(",\"present\":%s,\"valid\":%s,\"size\":%llu,\"built\":", r == 0 ? "true" : "false",
                   k.valid ? "true" : "false", (unsigned long long)k.size);
            json_str(k.banner);
            printf("}");
            continue;
        }
        if (r == -ENODEV)
            printf("  %-9s (no such partition)\n", slots[i]);
        else if (r < 0)
            printf("  %-9s no kernel\n", slots[i]);
        else
            printf("  %-9s /kernel.elf  %llu KiB  %s%s\n", slots[i], (unsigned long long)(k.size / 1024),
                   k.valid ? "" : "NOT A EUNUCHS KERNEL  ", k.banner[0] ? k.banner : "");
    }
    if (json)
        printf("]");
}

/* Write data as slot:/kernel.elf (replacing it). */
static int write_slot(struct image *im, const char *slot, const uint8_t *data, size_t len)
{
    struct part *p = image_part(im, slot);
    if (!p || !p->is_efs)
        return fail("%s: no %s partition (an image from before the boot slots?)", im->path, slot);
    int ino = efs_lookup(&p->fs, "/kernel.elf");
    int r = ino >= 0 ? efs_replace(&p->fs, (uint32_t)ino, data, len)
                     : (ino = efs_create(&p->fs, "/kernel.elf", EFS_T_FILE, 0755, 0, 0)) < 0
                           ? ino : efs_write(&p->fs, (uint32_t)ino, 0, data, len);
    if (r)
        return fail("%s:%s:/kernel.elf: %s", im->path, slot, efs_strerror(r));
    modified = true;
    return 0;
}

static uint8_t *read_host(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail("%s: %s", path, strerror(errno));
        return NULL;
    }
    size_t cap = 1 << 16, n = 0, got;
    uint8_t *buf = malloc(cap);
    while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap)
            buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    if (!buf)
        fail("%s: out of memory", path);
    *len = n;
    return buf;
}

static int cmd_kernel(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "--json")) {
        json = true;
        argc--, argv++;
    }
    if (argc == 1) {                            /* status */
        struct image im;
        if (load(&im, argv[0], false))
            return 1;
        if (!json)
            printf("%s:\n", argv[0]);
        print_kernels(&im);
        if (json)
            printf("\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[0], "install")) {
        const char *to = NULL;
        argc--, argv++;
        if (argc >= 2 && !strcmp(argv[0], "--to")) {
            to = argv[1];
            argc -= 2, argv += 2;
        }
        if (argc != 2)
            usage();
        size_t len;
        uint8_t *data = read_host(argv[1], &len);
        if (!data)
            return 1;
        struct kinfo nk;
        kernel_info(data, len, &nk);
        if (!nk.valid)
            return fail("%s: not a Eunuchs kernel (a RISC-V ELF executable with its banner)", argv[1]);
        struct image im;
        if (load(&im, argv[0], true))
            return 1;
        if (to) {
            if (write_slot(&im, to, data, len))
                return 1;
            printf("%s: %s:/kernel.elf %s\n", argv[0], to, nk.banner);
        } else {
            struct kinfo cur;
            uint8_t *old = NULL;
            size_t oldlen = 0;
            if (slot_kernel(&im, "boot", &cur, &old, &oldlen) == 0) {
                if (oldlen == len && !memcmp(old, data, len)) {
                    printf("%s: that kernel is already in boot\n", argv[0]);
                    return 0;
                }
                if (write_slot(&im, "boot.old", old, oldlen))
                    return 1;
                printf("%s: boot.old:/kernel.elf %s (was in boot)\n", argv[0], cur.banner);
            }
            if (write_slot(&im, "boot", data, len))
                return 1;
            printf("%s: boot:/kernel.elf %s\n", argv[0], nk.banner);
        }
        return image_save(&im) ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[0], "rollback")) {
        struct image im;
        if (load(&im, argv[1], true))
            return 1;
        struct kinfo a, b;
        uint8_t *da, *db;
        size_t la, lb;
        if (slot_kernel(&im, "boot.old", &b, &db, &lb))
            return fail("%s: no kernel in boot.old to go back to", argv[1]);
        if (slot_kernel(&im, "boot", &a, &da, &la) == 0) {
            if (write_slot(&im, "boot.old", da, la))
                return 1;
        } else {                                /* boot was empty: boot.old moves there */
            efs_unlink(&image_part(&im, "boot.old")->fs, "/kernel.elf");
        }
        if (write_slot(&im, "boot", db, lb))
            return 1;
        printf("%s: boot:/kernel.elf %s; boot.old:/kernel.elf %s\n", argv[1], b.banner, a.banner);
        return image_save(&im) ? 1 : 0;
    }
    usage();
    return 2;
}

/* ---- info ---- */

static int cmd_info(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "--json")) {
        json = true;
        argc--, argv++;
    }
    if (argc != 1)
        usage();
    struct image im;
    if (load(&im, argv[0], false))
        return 1;
    if (json) {
        printf("{\"image\":");
        json_str(argv[0]);
        printf(",\"size\":%llu,\"gpt\":%s,\"in_use\":%s,\"partitions\":[", (unsigned long long)im.size,
               im.gpt ? "true" : "false", image_in_use(argv[0]) ? "true" : "false");
    } else {
        printf("%s: %llu MiB, %s%s\n", argv[0], (unsigned long long)(im.size >> 20),
               im.gpt ? "GPT" : "a bare efs filesystem", image_in_use(argv[0]) ? " (in use by QEMU)" : "");
    }
    for (int i = 0; i < im.nparts; i++) {
        struct part *p = &im.parts[i];
        uint64_t mb = (p->last - p->first + 1) * GPT_SECTOR >> 20;
        struct efs_usage u = { 0 };
        if (p->is_efs)
            efs_usage(&p->fs, &u);
        if (json) {
            printf("%s{\"name\":", i ? "," : "");
            json_str(p->name);
            printf(",\"start\":%llu,\"sectors\":%llu,\"efs\":%s,\"swap\":%s", (unsigned long long)p->first,
                   (unsigned long long)(p->last - p->first + 1), p->is_efs ? "true" : "false",
                   p->is_swap ? "true" : "false");
            if (p->is_swap)
                printf(",\"swap_pages\":%u", p->swap_pages);
            if (p->is_efs) {
                printf(",\"label\":");
                char lab[17];
                snprintf(lab, sizeof(lab), "%.16s", p->fs.sb->label);
                json_str(lab);
                printf(",\"files\":%u,\"dirs\":%u,\"blocks\":%u,\"used_blocks\":%u,\"free_inodes\":%u,"
                       "\"replayed\":%u", u.files, u.dirs, u.data_blocks, u.used_blocks, u.free_inodes,
                       p->fs.replayed);
            }
            printf("}");
            continue;
        }
        if (p->is_swap) {
            printf("  %-9s %5llu MiB  swap space, %u pages\n", p->name, (unsigned long long)mb, p->swap_pages - 1);
            continue;
        }
        if (!p->is_efs) {
            printf("  %-9s %5llu MiB  not efs\n", p->name, (unsigned long long)mb);
            continue;
        }
        printf("  %-9s %5llu MiB  efs, %u files, %u directories, %u%% used (%u of %u blocks)%s\n", p->name,
               (unsigned long long)mb, u.files, u.dirs,
               u.data_blocks ? (unsigned)((uint64_t)u.used_blocks * 100 / u.data_blocks) : 0,
               u.used_blocks, u.data_blocks,
               p->fs.replayed ? " (log holds a committed transaction)" : "");
    }
    if (json) {
        printf("],\"kernels\":");
        print_kernels(&im);
        printf("}\n");
        return 0;
    }
    if (image_part(&im, "boot")) {
        printf("kernels:\n");
        print_kernels(&im);
    }
    return 0;
}

/* ---- ls, cat, get ---- */

static void mode_string(const struct efs_dinode *d, char out[11])
{
    out[0] = d->type == EFS_T_DIR ? 'd' : '-';
    for (int i = 0; i < 9; i++)
        out[1 + i] = d->mode & (0400 >> i) ? "rwxrwxrwx"[i] : '-';
    out[10] = 0;
}

static void list_dir(struct efs *fs, const char *path, uint32_t dir, bool longfmt, bool recursive, bool *first);

static void list_entry(struct efs *fs, const char *name, uint32_t ino, bool longfmt, bool *first,
                       const char *path)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    if (json) {
        printf("%s{\"name\":", *first ? "" : ",");
        *first = false;
        json_str(name);
        printf(",\"path\":");
        json_str(path);
        printf(",\"type\":\"%s\",\"mode\":%u,\"uid\":%u,\"gid\":%u,\"nlink\":%u,\"size\":%llu,"
               "\"mtime\":%llu,\"ino\":%u}", d->type == EFS_T_DIR ? "dir" : "file", d->mode, d->uid, d->gid,
               d->nlink, (unsigned long long)d->size, (unsigned long long)(d->mtime / 1000000000ull), ino);
        return;
    }
    if (!longfmt) {
        printf("%s%s\n", name, d->type == EFS_T_DIR ? "/" : "");
        return;
    }
    char m[11], when[32];
    mode_string(d, m);
    time_t t = (time_t)(d->mtime / 1000000000ull);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
    printf("%s %3u %5u %5u %9llu %s %s%s\n", m, d->nlink, d->uid, d->gid, (unsigned long long)d->size, when,
           name, d->type == EFS_T_DIR ? "/" : "");
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(((const struct efs_dirent *)a)->name, ((const struct efs_dirent *)b)->name);
}

/* A directory's entries but "." and "..", sorted by name. */
static struct efs_dirent *dir_entries(struct efs *fs, uint32_t dir, int *n)
{
    struct efs_dirent de, *v = NULL;
    int cap = 0;
    *n = 0;
    for (uint32_t i = 0; efs_dirent(fs, dir, i, &de); i++) {
        if (!de.ino || !strcmp(de.name, ".") || !strcmp(de.name, ".."))
            continue;
        if (*n == cap)
            v = realloc(v, (cap = cap ? cap * 2 : 32) * sizeof(*v));
        v[(*n)++] = de;
    }
    if (*n)
        qsort(v, *n, sizeof(*v), cmp_names);
    return v;
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void list_dir(struct efs *fs, const char *path, uint32_t dir, bool longfmt, bool recursive, bool *first)
{
    int n;
    struct efs_dirent *v = dir_entries(fs, dir, &n);
    if (recursive && !json)
        printf("%s:\n", path);
    for (int i = 0; i < n; i++) {
        char sub[1100];
        join(sub, sizeof(sub), path, v[i].name);
        list_entry(fs, v[i].name, v[i].ino, longfmt, first, sub);
    }
    if (recursive) {
        for (int i = 0; i < n; i++) {
            if (efs_inode(fs, v[i].ino)->type != EFS_T_DIR)
                continue;
            char sub[1100];
            join(sub, sizeof(sub), path, v[i].name);
            if (!json)
                printf("\n");
            list_dir(fs, sub, v[i].ino, longfmt, recursive, first);
        }
    }
    free(v);
}

static int cmd_ls(int argc, char **argv)
{
    bool longfmt = false, recursive = false;
    for (; argc && argv[0][0] == '-' && argv[0][1]; argc--, argv++) {
        if (!strcmp(argv[0], "--json")) {
            json = true;
            continue;
        }
        for (char *f = argv[0] + 1; *f; f++) {
            if (*f == 'l')
                longfmt = true;
            else if (*f == 'R')
                recursive = true;
            else
                usage();
        }
    }
    if (argc != 1)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    struct image im;
    if (load(&im, pl.image, false))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    int ino = efs_lookup(&p->fs, pl.path);
    if (ino < 0)
        return fail("%s: %s", argv[0], efs_strerror(ino));
    bool first = true;
    if (json)
        printf("[");
    if (efs_inode(&p->fs, (uint32_t)ino)->type == EFS_T_DIR) {
        list_dir(&p->fs, pl.path, (uint32_t)ino, longfmt, recursive, &first);
    } else {
        const char *name = strrchr(pl.path, '/') ? strrchr(pl.path, '/') + 1 : pl.path;
        list_entry(&p->fs, name, (uint32_t)ino, longfmt, &first, pl.path);
    }
    if (json)
        printf("]\n");
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc != 1)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    struct image im;
    if (load(&im, pl.image, false))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    int ino = efs_lookup(&p->fs, pl.path);
    if (ino < 0)
        return fail("%s: %s", argv[0], efs_strerror(ino));
    if (efs_inode(&p->fs, (uint32_t)ino)->type == EFS_T_DIR)
        return fail("%s: is a directory", argv[0]);
    size_t len;
    uint8_t *data = efs_slurp(&p->fs, (uint32_t)ino, &len);
    fwrite(data, 1, len, stdout);
    free(data);
    return 0;
}

static int get_tree(struct efs *fs, const char *from, uint32_t ino, const char *to, bool recursive)
{
    struct efs_dinode *d = efs_inode(fs, ino);
    if (d->type == EFS_T_DIR) {
        if (!recursive)
            return fail("%s: is a directory (use -r)", from);
        if (mkdir(to, 0755) < 0 && errno != EEXIST)
            return fail("%s: %s", to, strerror(errno));
        int n, rc = 0;
        struct efs_dirent *v = dir_entries(fs, ino, &n);
        for (int i = 0; i < n; i++) {
            char a[1100], b[PATH_MAX + 64];
            join(a, sizeof(a), from, v[i].name);
            snprintf(b, sizeof(b), "%s/%s", to, v[i].name);
            rc |= get_tree(fs, a, v[i].ino, b, true);
        }
        free(v);
        return rc;
    }
    size_t len;
    uint8_t *data = efs_slurp(fs, ino, &len);
    FILE *f = fopen(to, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f)) {
        free(data);
        return fail("%s: %s", to, strerror(errno));
    }
    chmod(to, d->mode & 0777);
    free(data);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    bool recursive = false;
    if (argc && !strcmp(argv[0], "-r")) {
        recursive = true;
        argc--, argv++;
    }
    if (argc != 2)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    struct image im;
    if (load(&im, pl.image, false))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    int ino = efs_lookup(&p->fs, pl.path);
    if (ino < 0)
        return fail("%s: %s", argv[0], efs_strerror(ino));
    char to[PATH_MAX + 64];
    struct stat st;
    const char *base = strrchr(pl.path, '/') ? strrchr(pl.path, '/') + 1 : pl.path;
    if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode) && *base)
        snprintf(to, sizeof(to), "%s/%s", argv[1], base);   /* into a directory */
    else
        snprintf(to, sizeof(to), "%s", argv[1]);
    return get_tree(&p->fs, pl.path, (uint32_t)ino, to, recursive);
}

/* ---- put, mkdir, rm ---- */

static int put_mode = -1, put_uid, put_gid;

static int put_tree(struct efs *fs, const char *from, const char *to, bool recursive)
{
    struct stat st;
    if (stat(from, &st) < 0)
        return fail("%s: %s", from, strerror(errno));
    if (S_ISDIR(st.st_mode)) {
        if (!recursive)
            return fail("%s: is a directory (use -r)", from);
        int ino = efs_lookup(fs, to);
        if (ino < 0) {
            ino = efs_create(fs, to, EFS_T_DIR, put_mode >= 0 ? put_mode : st.st_mode & 0777, put_uid, put_gid);
            if (ino < 0)
                return fail("%s: %s", to, efs_strerror(ino));
        } else if (efs_inode(fs, (uint32_t)ino)->type != EFS_T_DIR) {
            return fail("%s: not a directory", to);
        }
        modified = true;
        DIR *d = opendir(from);
        if (!d)
            return fail("%s: %s", from, strerror(errno));
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char a[PATH_MAX + 64], b[1100];
            snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
            join(b, sizeof(b), to, e->d_name);
            rc |= put_tree(fs, a, b, true);
        }
        closedir(d);
        return rc;
    }
    size_t len;
    uint8_t *data = read_host(from, &len);
    if (!data)
        return 1;
    int ino = efs_lookup(fs, to), r;
    if (ino >= 0 && efs_inode(fs, (uint32_t)ino)->type == EFS_T_DIR) {
        free(data);
        return fail("%s: is a directory", to);
    }
    if (ino >= 0) {
        r = efs_replace(fs, (uint32_t)ino, data, len);
    } else {
        ino = efs_create(fs, to, EFS_T_FILE, put_mode >= 0 ? put_mode : st.st_mode & 0777, put_uid, put_gid);
        r = ino < 0 ? ino : efs_write(fs, (uint32_t)ino, 0, data, len);
    }
    free(data);
    if (r)
        return fail("%s: %s", to, efs_strerror(r));
    if (put_mode >= 0)
        efs_inode(fs, (uint32_t)ino)->mode = (uint16_t)put_mode;
    modified = true;
    return 0;
}

static int owner_opt(const char *s)
{
    unsigned u, g;
    if (sscanf(s, "%u:%u", &u, &g) != 2)
        return -1;
    put_uid = (int)u;
    put_gid = (int)g;
    return 0;
}

static int cmd_put(int argc, char **argv)
{
    bool recursive = false;
    for (; argc && argv[0][0] == '-'; argc--, argv++) {
        if (!strcmp(argv[0], "-r"))
            recursive = true;
        else if (!strcmp(argv[0], "-m") && argc > 1)
            put_mode = (int)strtol(argv[1], NULL, 8), argc--, argv++;
        else if (!strcmp(argv[0], "-o") && argc > 1 && !owner_opt(argv[1]))
            argc--, argv++;
        else
            usage();
    }
    if (argc != 2)
        usage();
    struct place pl;
    parse_place(argv[1], &pl);
    struct image im;
    if (load(&im, pl.image, true))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    char to[1100];
    int ino = efs_lookup(&p->fs, pl.path);
    const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (ino >= 0 && efs_inode(&p->fs, (uint32_t)ino)->type == EFS_T_DIR && *base)
        join(to, sizeof(to), pl.path, base);    /* into a directory */
    else
        snprintf(to, sizeof(to), "%s", pl.path);
    int rc = put_tree(&p->fs, argv[0], to, recursive);
    if (modified && image_save(&im))
        return 1;
    return rc;
}

static int cmd_mkdir(int argc, char **argv)
{
    bool parents = false;
    for (; argc && argv[0][0] == '-'; argc--, argv++) {
        if (!strcmp(argv[0], "-p"))
            parents = true;
        else if (!strcmp(argv[0], "-m") && argc > 1)
            put_mode = (int)strtol(argv[1], NULL, 8), argc--, argv++;
        else if (!strcmp(argv[0], "-o") && argc > 1 && !owner_opt(argv[1]))
            argc--, argv++;
        else
            usage();
    }
    if (argc != 1)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    struct image im;
    if (load(&im, pl.image, true))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    int ino = parents ? efs_mkdirs(&p->fs, pl.path)
                      : efs_create(&p->fs, pl.path, EFS_T_DIR, put_mode >= 0 ? put_mode : 0755, put_uid, put_gid);
    if (ino < 0)
        return fail("%s: %s", argv[0], efs_strerror(ino));
    struct efs_dinode *d = efs_inode(&p->fs, (uint32_t)ino);
    if (put_mode >= 0)
        d->mode = (uint16_t)put_mode;
    if (put_uid || put_gid) {
        d->uid = (uint16_t)put_uid;
        d->gid = (uint16_t)put_gid;
    }
    return image_save(&im) ? 1 : 0;
}

static int rm_tree(struct efs *fs, const char *path, bool recursive)
{
    int ino = efs_lookup(fs, path);
    if (ino < 0)
        return fail("%s: %s", path, efs_strerror(ino));
    if (efs_inode(fs, (uint32_t)ino)->type == EFS_T_DIR) {
        if (!recursive)
            return fail("%s: is a directory (use -r)", path);
        int n, rc = 0;
        struct efs_dirent *v = dir_entries(fs, (uint32_t)ino, &n);
        for (int i = 0; i < n; i++) {
            char sub[1100];
            join(sub, sizeof(sub), path, v[i].name);
            rc |= rm_tree(fs, sub, true);
        }
        free(v);
        if (rc)
            return rc;
    }
    int r = efs_unlink(fs, path);
    return r ? fail("%s: %s", path, efs_strerror(r)) : 0;
}

static int cmd_rm(int argc, char **argv)
{
    bool recursive = false;
    if (argc && (!strcmp(argv[0], "-r") || !strcmp(argv[0], "-rf"))) {
        recursive = true;
        argc--, argv++;
    }
    if (argc != 1)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    if (!strcmp(pl.path, "/"))
        return fail("%s: will not remove the root directory", argv[0]);
    struct image im;
    if (load(&im, pl.image, true))
        return 1;
    struct part *p = find_part(&im, pl.part);
    if (!p)
        return 1;
    int rc = rm_tree(&p->fs, pl.path, recursive);
    if (efs_lookup(&p->fs, pl.path) < 0 && image_save(&im))
        return 1;
    return rc;
}

/* ---- copy: a tree from one efs to another, as it is ---- */

struct inomap {
    uint32_t from;
    char *path;                 /* its copy */
};
static struct inomap *imap;
static int nimap, capimap;
static int n_copied, n_skipped;

static int copy_tree(struct efs *src, uint32_t sino, struct efs *dst, const char *to)
{
    struct efs_dinode *sd = efs_inode(src, sino);
    int ino = efs_lookup(dst, to);
    if (sd->type == EFS_T_DIR) {
        if (ino >= 0 && efs_inode(dst, (uint32_t)ino)->type != EFS_T_DIR) {
            fprintf(stderr, "%s: %s exists and is not a directory; not copied\n", prog, to);
            n_skipped++;
            return 1;
        }
        bool created = ino < 0;                 /* an existing directory keeps its owner and mode */
        if (created) {
            ino = efs_create(dst, to, EFS_T_DIR, sd->mode, sd->uid, sd->gid);
            if (ino < 0)
                return fail("%s: %s", to, efs_strerror(ino));
        }
        modified = true;
        int n, rc = 0;
        struct efs_dirent *v = dir_entries(src, sino, &n);
        for (int i = 0; i < n; i++) {
            char sub[1100];
            join(sub, sizeof(sub), to, v[i].name);
            rc |= copy_tree(src, v[i].ino, dst, sub);
        }
        free(v);
        if (created)
            efs_inode(dst, (uint32_t)ino)->mtime = sd->mtime;
        return rc;
    }
    if (ino >= 0) {                             /* never overwrite */
        fprintf(stderr, "%s: %s exists; not copied\n", prog, to);
        n_skipped++;
        return 1;
    }
    for (int i = 0; i < nimap; i++) {           /* another name for a file already copied */
        if (imap[i].from == sino) {
            int r = efs_link(dst, imap[i].path, to);
            modified = true;
            return r ? fail("%s: %s", to, efs_strerror(r)) : 0;
        }
    }
    size_t len;
    uint8_t *data = efs_slurp(src, sino, &len);
    ino = efs_create(dst, to, EFS_T_FILE, sd->mode, sd->uid, sd->gid);
    int r = ino < 0 ? ino : efs_write(dst, (uint32_t)ino, 0, data, len);
    free(data);
    if (r)
        return fail("%s: %s", to, efs_strerror(r));
    struct efs_dinode *dd = efs_inode(dst, (uint32_t)ino);
    dd->mtime = sd->mtime;
    if (nimap == capimap)
        imap = realloc(imap, (capimap = capimap ? capimap * 2 : 64) * sizeof(*imap));
    imap[nimap++] = (struct inomap){ sino, strdup(to) };
    n_copied++;
    modified = true;
    return 0;
}

static int cmd_copy(int argc, char **argv)
{
    if (argc != 2)
        usage();
    struct place a, b;
    parse_place(argv[0], &a);
    parse_place(argv[1], &b);
    static struct image ia, ib;
    if (load(&ia, a.image, false) || load(&ib, b.image, true))
        return 1;
    struct part *pa = find_part(&ia, a.part), *pb = find_part(&ib, b.part);
    if (!pa || !pb)
        return 1;
    int sino = efs_lookup(&pa->fs, a.path);
    if (sino < 0)
        return fail("%s: %s", argv[0], efs_strerror(sino));
    int rc = copy_tree(&pa->fs, (uint32_t)sino, &pb->fs, b.path);
    if (modified && image_save(&ib))
        return 1;
    printf("%s -> %s: %d files copied%s", argv[0], argv[1], n_copied, n_skipped ? "" : "\n");
    if (n_skipped)
        printf(", %d that exist already left alone\n", n_skipped);
    return rc;
}

static int cmd_fsck(int argc, char **argv)
{
    if (argc != 1)
        usage();
    struct place pl;
    parse_place(argv[0], &pl);
    struct image im;
    if (image_load(&im, pl.image, false))
        return 1;
    int errors = 0;
    for (int i = 0; i < im.nparts; i++) {
        struct part *p = &im.parts[i];
        if (pl.part[0] && strcmp(p->name, pl.part))
            continue;
        if (!p->is_efs) {
            printf("%s %s: %s\n", pl.image, p->name, p->is_swap ? "swap space (nothing to check)" : "not efs");
            continue;
        }
        printf("%s %s:\n", pl.image, p->name);
        errors += efs_check(&p->fs, stdout);
    }
    if (pl.part[0] && !image_part(&im, pl.part))
        return fail("%s: no partition %s", pl.image, pl.part);
    return errors ? 1 : 0;
}

/* ---- install: system files, keeping your work ---- */

#define MANIFEST "/var/lib/eudisk/manifest"

struct mrec {
    char *path;
    uint32_t crc;
    uint64_t size;
    char *link;                 /* a hard link: its target */
    bool seen;                  /* in this install's specs */
};

static struct mrec *man;
static int nman, capman;

static struct mrec *man_find(const char *path)
{
    for (int i = 0; i < nman; i++)
        if (!strcmp(man[i].path, path))
            return &man[i];
    return NULL;
}

static struct mrec *man_set(const char *path)
{
    struct mrec *m = man_find(path);
    if (m)
        return m;
    if (nman == capman)
        man = realloc(man, (capman = capman ? capman * 2 : 256) * sizeof(*man));
    m = &man[nman++];
    memset(m, 0, sizeof(*m));
    m->path = strdup(path);
    return m;
}

static void man_load(struct efs *fs)
{
    int ino = efs_lookup(fs, MANIFEST);
    if (ino < 0)
        return;
    size_t len;
    char *text = (char *)efs_slurp(fs, (uint32_t)ino, &len);
    text = realloc(text, len + 1);
    text[len] = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char path[1024], target[1024];
        unsigned crc;
        unsigned long long size;
        if (sscanf(line, "F %x %llu %1023s", &crc, &size, path) == 3) {
            struct mrec *m = man_set(path);
            m->crc = crc;
            m->size = size;
        } else if (sscanf(line, "L %1023s %1023s", path, target) == 2) {
            man_set(path)->link = strdup(target);
        }
    }
    free(text);
}

static int man_save(struct efs *fs)
{
    if (!nman && efs_lookup(fs, MANIFEST) < 0)
        return 0;                               /* nothing to remember */
    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    len += (size_t)snprintf(text, cap, "# what eudisk install put on this filesystem (do not edit)\n");
    for (int i = 0; i < nman; i++) {
        char line[2200];
        int n = man[i].link ? snprintf(line, sizeof(line), "L %s %s\n", man[i].path, man[i].link)
                            : snprintf(line, sizeof(line), "F %08x %llu %s\n", man[i].crc,
                                       (unsigned long long)man[i].size, man[i].path);
        while (len + (size_t)n + 1 > cap)
            text = realloc(text, cap *= 2);
        memcpy(text + len, line, (size_t)n + 1);
        len += (size_t)n;
    }
    int r = efs_mkdirs(fs, "/var/lib/eudisk");
    int ino = r < 0 ? r : efs_lookup(fs, MANIFEST);
    if (r >= 0 && ino == -ENOENT)
        ino = efs_create(fs, MANIFEST, EFS_T_FILE, 0644, 0, 0);
    if (r >= 0 && ino >= 0)
        r = efs_replace(fs, (uint32_t)ino, text, len);
    else
        r = ino < 0 ? ino : r;
    free(text);
    return r;
}

static char backup_dir[64];
static int n_installed, n_replaced, n_backed_up, n_kept, n_removed, n_same, n_dirs;

static int backup(struct efs *fs, const char *path, uint32_t ino)
{
    if (!backup_dir[0]) {
        time_t t = time(NULL);
        strftime(backup_dir, sizeof(backup_dir), "/var/backup/%Y%m%d-%H%M%S", localtime(&t));
    }
    char to[1200];
    snprintf(to, sizeof(to), "%s%s", backup_dir, path);
    char *slash = strrchr(to, '/');
    *slash = 0;
    int r = efs_mkdirs(fs, to);
    *slash = '/';
    if (r < 0)
        return r;
    size_t len;
    uint8_t *data = efs_slurp(fs, ino, &len);
    struct efs_dinode *d = efs_inode(fs, ino);
    int b = efs_create(fs, to, EFS_T_FILE, d->mode, d->uid, d->gid);
    r = b < 0 ? b : efs_write(fs, (uint32_t)b, 0, data, len);
    free(data);
    if (r)
        return r;
    printf("  backed up %s (you changed it) to %s\n", path, to);
    n_backed_up++;
    return 0;
}

static bool same_as_installed(struct efs *fs, const char *path, uint32_t ino)
{
    struct mrec *m = man_find(path);
    if (!m || m->link)
        return false;
    size_t len;
    uint8_t *data = efs_slurp(fs, ino, &len);
    bool same = len == m->size && gpt_crc32(data, len) == m->crc;
    free(data);
    return same;
}

static int install_file(struct efs *fs, const char *path, const char *host, bool config)
{
    size_t len;
    uint8_t *data = read_host(host, &len);
    if (!data)
        return -1;
    struct stat hst;
    uint16_t mode = stat(host, &hst) == 0 && !(hst.st_mode & 0111) ? 0644 : 0755;
    int r = 0, ino = efs_lookup(fs, path);
    uint32_t crc = gpt_crc32(data, len);
    if (ino >= 0 && efs_inode(fs, (uint32_t)ino)->type == EFS_T_DIR) {
        fprintf(stderr, "%s: %s is a directory; not installed\n", prog, path);
        free(data);
        return -1;
    }
    /* A name that is another file's hard link (the last install made it one,
       or you did): replacing it in place would change that file too. It
       goes (yours backed up first), and the file is made afresh. */
    struct mrec *was = man_find(path);
    if (ino >= 0 && efs_inode(fs, (uint32_t)ino)->nlink > 1 && (!was || was->link)) {
        if (!was && (r = backup(fs, path, (uint32_t)ino)) < 0) {
            free(data);
            return r;
        }
        if ((r = efs_unlink(fs, path)) < 0) {
            free(data);
            return r;
        }
        ino = -ENOENT;
    }
    if (ino >= 0) {
        size_t clen;
        uint8_t *cur = efs_slurp(fs, (uint32_t)ino, &clen);
        bool identical = clen == len && !memcmp(cur, data, len);
        free(cur);
        if (identical) {
            n_same++;
        } else if (same_as_installed(fs, path, (uint32_t)ino)) {
            r = efs_replace(fs, (uint32_t)ino, data, len);
            efs_inode(fs, (uint32_t)ino)->mode = mode;
            n_replaced++;
        } else if (config) {
            char newpath[1100];
            snprintf(newpath, sizeof(newpath), "%s.new", path);
            int n = efs_lookup(fs, newpath);
            if (n < 0)
                n = efs_create(fs, newpath, EFS_T_FILE, mode, 0, 0);
            r = n < 0 ? n : efs_replace(fs, (uint32_t)n, data, len);
            if (!r)
                printf("  kept your %s; the new version is %s\n", path, newpath);
            n_kept++;
            free(data);
            man_set(path)->seen = true;         /* the record stays: what was installed */
            return r;
        } else {
            r = backup(fs, path, (uint32_t)ino);
            if (!r)
                r = efs_replace(fs, (uint32_t)ino, data, len);
            efs_inode(fs, (uint32_t)ino)->mode = mode;
            n_replaced++;
        }
    } else {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            r = efs_mkdirs(fs, dir);
        }
        if (r >= 0) {
            ino = efs_create(fs, path, EFS_T_FILE, mode, 0, 0);
            r = ino < 0 ? ino : efs_write(fs, (uint32_t)ino, 0, data, len);
        }
        n_installed++;
    }
    free(data);
    if (r < 0)
        return r;
    struct mrec *m = man_set(path);
    m->crc = crc;
    m->size = len;
    free(m->link);
    m->link = NULL;
    m->seen = true;
    return 0;
}

static int install_link(struct efs *fs, const char *path, const char *target)
{
    int t = efs_lookup(fs, target);
    if (t < 0)
        return t;
    int ino = efs_lookup(fs, path), r = 0;
    if (ino == t) {
        n_same++;
    } else {
        if (ino >= 0) {
            struct mrec *m = man_find(path);
            bool ours = m && ((m->link && !strcmp(m->link, target)) || same_as_installed(fs, path, (uint32_t)ino));
            if (efs_inode(fs, (uint32_t)ino)->type == EFS_T_DIR) {
                fprintf(stderr, "%s: %s is a directory; link not installed\n", prog, path);
                return -1;
            }
            if (!ours && (r = backup(fs, path, (uint32_t)ino)) < 0)
                return r;
            if ((r = efs_unlink(fs, path)) < 0)
                return r;
            n_replaced++;
        } else {
            n_installed++;
        }
        if ((r = efs_link(fs, target, path)) < 0)
            return r;
    }
    struct mrec *m = man_set(path);
    free(m->link);
    m->link = strdup(target);
    m->seen = true;
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    if (argc < 2)
        usage();
    struct image im;
    if (load(&im, argv[0], true))
        return 1;
    struct part *p = find_part(&im, argv[1]);
    if (!p)
        return 1;
    struct efs *fs = &p->fs;
    man_load(fs);
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        char spec[2200];
        bool config = argv[i][0] == '+';
        snprintf(spec, sizeof(spec), "%s", argv[i] + config);
        char *eq = strchr(spec, '=');
        int r;
        if (!eq) {                              /* a directory */
            char *colon = strchr(spec, ':');
            if (colon)
                *colon++ = 0;
            bool existed = efs_lookup(fs, spec) >= 0;
            r = efs_mkdirs(fs, spec);
            if (r >= 0 && !existed)
                n_dirs++;
            if (r >= 0 && colon && !existed) {  /* your directories keep their modes */
                unsigned mode = 0755, uid = 0, gid = 0;
                sscanf(colon, "%o:%u:%u", &mode, &uid, &gid);
                struct efs_dinode *d = efs_inode(fs, (uint32_t)r);
                d->mode = (uint16_t)mode;
                d->uid = (uint16_t)uid;
                d->gid = (uint16_t)gid;
            }
        } else {
            *eq = 0;
            r = eq[1] == '@' ? install_link(fs, spec, eq + 2) : install_file(fs, spec, eq + 1, config);
        }
        if (r < 0) {
            if (r != -1)
                fprintf(stderr, "%s: %s: %s\n", prog, argv[i], efs_strerror(r));
            rc = 1;
        }
    }
    /* what an earlier install put here and these specs no longer name */
    for (int i = 0; i < nman && !rc; i++) {
        struct mrec *m = &man[i];
        if (m->seen)
            continue;
        int ino = efs_lookup(fs, m->path);
        if (ino < 0) {
            m->seen = true;                     /* gone already: forget it */
            m->path[0] = 0;
            continue;
        }
        bool ours = m->link ? efs_lookup(fs, m->link) == ino : same_as_installed(fs, m->path, (uint32_t)ino);
        if (ours) {
            efs_unlink(fs, m->path);
            printf("  removed %s (no longer installed)\n", m->path);
            n_removed++;
            m->path[0] = 0;
        } else {
            printf("  left %s (no longer installed, but you changed it)\n", m->path);
        }
    }
    int k = 0;                                  /* drop forgotten records */
    for (int i = 0; i < nman; i++)
        if (man[i].path[0])
            man[k++] = man[i];
    nman = k;
    bool changed = n_installed || n_replaced || n_kept || n_removed || n_backed_up || n_dirs;
    if (changed) {
        int r = man_save(fs);
        if (r) {
            fprintf(stderr, "%s: %s: %s\n", prog, MANIFEST, efs_strerror(r));
            rc = 1;
        }
        if (image_save(&im))
            return 1;
    }
    printf("%s:%s: %d new, %d updated, %d unchanged", argv[0], p->name, n_installed, n_replaced, n_same);
    if (n_removed)
        printf(", %d removed", n_removed);
    if (n_backed_up)
        printf(", %d of yours backed up in %s", n_backed_up, backup_dir);
    if (n_kept)
        printf(", %d of your configuration files kept", n_kept);
    printf("\n");
    return rc;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--force"))
            force = true;
        else
            break;
    }
    if (i >= argc)
        usage();
    const char *cmd = argv[i];
    argc -= i + 1;
    argv += i + 1;
    /* --force may come after the command too */
    for (int j = 0; j < argc; j++) {
        if (!strcmp(argv[j], "--force")) {
            force = true;
            memmove(argv + j, argv + j + 1, (argc - j - 1) * sizeof(*argv));
            argc--;
            j--;
        }
    }
    if (!strcmp(cmd, "create"))
        return cmd_create(argc, argv);
    if (!strcmp(cmd, "add"))
        return cmd_add(argc, argv);
    if (!strcmp(cmd, "info"))
        return cmd_info(argc, argv);
    if (!strcmp(cmd, "ls"))
        return cmd_ls(argc, argv);
    if (!strcmp(cmd, "cat"))
        return cmd_cat(argc, argv);
    if (!strcmp(cmd, "get"))
        return cmd_get(argc, argv);
    if (!strcmp(cmd, "put"))
        return cmd_put(argc, argv);
    if (!strcmp(cmd, "mkdir"))
        return cmd_mkdir(argc, argv);
    if (!strcmp(cmd, "rm"))
        return cmd_rm(argc, argv);
    if (!strcmp(cmd, "copy"))
        return cmd_copy(argc, argv);
    if (!strcmp(cmd, "fsck"))
        return cmd_fsck(argc, argv);
    if (!strcmp(cmd, "kernel"))
        return cmd_kernel(argc, argv);
    if (!strcmp(cmd, "install"))
        return cmd_install(argc, argv);
    usage();
    return 2;
}
