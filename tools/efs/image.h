/*
 * Disk images on the host: a GPT disk with efs partitions (what make builds:
 * boot, boot.old, root, home), or a bare efs filesystem (older images, test
 * images). The whole image is held in memory; image_save writes it back
 * atomically (a temporary file, then rename), so a failure never leaves a
 * half-written image.
 */
#ifndef EUNUCHS_HOST_IMAGE_H
#define EUNUCHS_HOST_IMAGE_H

#include "efs.h"
#include "gpt_format.h"

#define IMAGE_MAX_PARTS 16

struct part {
    char name[GPT_NAME_LEN + 1];    /* GPT name, or the efs label of a bare image */
    uint64_t first, last;           /* sectors (inclusive) */
    bool efs_type;                  /* GPT type is efs */
    bool is_efs;                    /* holds an efs filesystem (opened in fs) */
    struct efs fs;
};

struct image {
    const char *path;
    uint8_t *data;
    uint64_t size;                  /* bytes */
    bool gpt;
    int nparts;
    struct part parts[IMAGE_MAX_PARTS];
};

struct layout {                     /* image_create */
    const char *name;
    uint32_t mb;
};

/* Load an image (GPT or bare efs) and open its efs filesystems; with replay,
   a committed transaction left in a log is installed (in memory), as the
   kernel does at mount: needed before changing anything. Prints why on
   failure. */
int image_load(struct image *im, const char *path, bool replay);
/* Write it back (temporary file + rename). */
int image_save(struct image *im);
/* A new GPT image with an empty efs filesystem in each partition, labelled
   with the partition's name. */
int image_create(struct image *im, const char *path, const struct layout *parts, int nparts);

/* A partition by GPT name or efs label (NULL: the default: "root", else the
   only/first efs partition). */
struct part *image_part(struct image *im, const char *name);

#endif
