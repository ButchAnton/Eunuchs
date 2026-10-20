/*
 * mkfs / fsck for efs (format: kernel/include/efs_format.h). Host tool, built
 * on the host efs library (tools/efs).
 *
 *   mkfs [--label NAME] IMAGE SIZE_MB [/path=hostfile | /path=@/existing | /dir/[:mode:uid:gid]]...
 *       a bare efs filesystem (the disk image make builds is a GPT disk:
 *       tools/eudisk)
 *   mkfs --check IMAGE
 *       check every efs filesystem in an image: a bare one, or each partition
 *       of a GPT disk
 *
 * The checker validates everything it can derive: superblock layout, the
 * log, directory structure (".", "..", names), link counts, block pointers
 * (range, double use), the free bitmap (leaks and missing marks), and
 * orphaned inodes. It looks at the image as it is: a committed transaction
 * still in the log is reported, not replayed. Exit status: 0 clean, 1 errors
 * found, 2 usage/I/O error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efs.h"
#include "image.h"

static int do_check(const char *path)
{
    struct image im;
    if (image_load(&im, path, false))
        return 2;
    int errors = 0, checked = 0;
    for (int i = 0; i < im.nparts; i++) {
        struct part *p = &im.parts[i];
        if (!p->is_efs) {
            if (im.gpt)
                printf("fsck %s partition %s: %s, skipped\n", path, p->name, p->is_swap ? "swap space" : "not efs");
            continue;
        }
        if (im.gpt)
            printf("fsck %s partition %s:\n", path, p->name);
        else
            printf("fsck %s:\n", path);
        errors += efs_check(&p->fs, stdout);
        checked++;
    }
    if (!checked) {
        fprintf(stderr, "mkfs: %s: no efs filesystem\n", path);
        return 2;
    }
    return errors ? 1 : 0;
}

static int do_mkfs(const char *path, const char *label, int size_mb, int nspecs, char **specs)
{
    uint32_t nblocks = (uint32_t)size_mb * (1024 * 1024 / EFS_BSIZE);
    uint8_t *img = calloc(nblocks, EFS_BSIZE);
    struct efs fs;
    if (!img) {
        fprintf(stderr, "mkfs: out of memory\n");
        return 2;
    }
    int r = efs_format(&fs, img, nblocks, label);
    if (r) {
        fprintf(stderr, "mkfs: %s: %d MiB: %s\n", path, size_mb, efs_strerror(r));
        return 2;
    }
    for (int i = 0; i < nspecs; i++)
        if (efs_apply_spec(&fs, specs[i]))
            return 2;
    FILE *out = fopen(path, "wb");
    if (!out || fwrite(img, EFS_BSIZE, nblocks, out) != nblocks || fclose(out)) {
        fprintf(stderr, "mkfs: %s: write failed\n", path);
        return 2;
    }
    struct efs_usage u;
    efs_usage(&fs, &u);
    printf("mkfs: %s: %u blocks (%d MiB), %u inodes used, %u data blocks used\n", path, nblocks,
           size_mb, u.files + u.dirs, u.used_blocks);
    return 0;
}

int main(int argc, char **argv)
{
    const char *label = "";
    if (argc == 3 && !strcmp(argv[1], "--check"))
        return do_check(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--label")) {
        label = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc >= 3)
        return do_mkfs(argv[1], label, atoi(argv[2]), argc - 3, argv + 3);
    fprintf(stderr, "usage: mkfs [--label NAME] IMAGE SIZE_MB [/path=hostfile | /path=@/existing | /dir/]...\n"
                    "       mkfs --check IMAGE\n");
    return 2;
}
