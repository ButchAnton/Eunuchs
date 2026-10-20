/*
 * Disk images on the host (see image.h).
 */
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void random_bytes(uint8_t *p, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, p, n) != (ssize_t)n) {
        for (size_t i = 0; i < n; i++)          /* (no /dev/urandom: anything unique enough) */
            p[i] = (uint8_t)(rand() ^ getpid() ^ i);
    }
    if (fd >= 0)
        close(fd);
}

static void random_guid(uint8_t g[16])
{
    random_bytes(g, 16);
    g[7] = (g[7] & 0x0f) | 0x40;                /* version 4 (byte 7: mixed-endian field 3) */
    g[8] = (g[8] & 0x3f) | 0x80;                /* variant 1 */
}

/* A valid GPT header at lba, with its entries; NULL if not. */
static struct gpt_header *read_gpt(struct image *im, uint64_t lba, struct gpt_entry **entries)
{
    if ((lba + 1) * GPT_SECTOR > im->size)
        return NULL;
    struct gpt_header *h = (struct gpt_header *)(im->data + lba * GPT_SECTOR);
    if (h->signature != GPT_SIGNATURE || h->header_size < GPT_HEADER_SIZE || h->header_size > GPT_SECTOR ||
        h->entry_size != GPT_ENTRY_SIZE || h->num_entries == 0 || h->num_entries > 1024)
        return NULL;
    struct gpt_header copy;
    memcpy(&copy, h, sizeof(copy));
    copy.header_crc = 0;
    if (gpt_crc32(&copy, GPT_HEADER_SIZE) != h->header_crc)
        return NULL;
    uint64_t bytes = (uint64_t)h->num_entries * h->entry_size;
    if (h->entries_lba * GPT_SECTOR + bytes > im->size)
        return NULL;
    *entries = (struct gpt_entry *)(im->data + h->entries_lba * GPT_SECTOR);
    if (gpt_crc32(*entries, (size_t)bytes) != h->entries_crc)
        return NULL;
    return h;
}

static void add_efs_part(struct image *im, struct part *p, bool replay)
{
    uint64_t bytes = (p->last - p->first + 1) * GPT_SECTOR;
    p->is_efs = (p->last + 1) * GPT_SECTOR <= im->size &&
                efs_open(&p->fs, im->data + p->first * GPT_SECTOR, (uint32_t)(bytes / EFS_BSIZE), replay) == 0;
}

static void check_swap(struct image *im, struct part *p)
{
    const struct swap_header *h = (const void *)(im->data + p->first * GPT_SECTOR);
    uint64_t pages = (p->last - p->first + 1) * GPT_SECTOR / SWAP_PAGE;
    p->is_swap = !memcmp(h->magic, SWAP_MAGIC, 8) && h->version == 1 && h->npages >= 2 && h->npages <= pages;
    p->swap_pages = p->is_swap ? h->npages : 0;
}

int image_load(struct image *im, const char *path, bool replay)
{
    memset(im, 0, sizeof(*im));
    im->path = path;
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    im->size = (uint64_t)st.st_size;
    im->data = malloc(im->size ? im->size : 1);
    if (!im->data) {
        fprintf(stderr, "%s: out of memory\n", path);
        close(fd);
        return -1;
    }
    for (uint64_t got = 0; got < im->size;) {
        ssize_t n = read(fd, im->data + got, im->size - got);
        if (n <= 0) {
            fprintf(stderr, "%s: read error\n", path);
            close(fd);
            return -1;
        }
        got += (uint64_t)n;
    }
    close(fd);

    struct gpt_entry *e;
    struct gpt_header *h = read_gpt(im, 1, &e);
    if (!h && im->size >= 2 * GPT_SECTOR)       /* the primary is damaged: the backup */
        h = read_gpt(im, im->size / GPT_SECTOR - 1, &e);
    if (h) {
        im->gpt = true;
        for (uint32_t i = 0; i < h->num_entries && im->nparts < IMAGE_MAX_PARTS; i++) {
            static const uint8_t unused[16];
            if (!memcmp(e[i].type, unused, 16))
                continue;
            struct part *p = &im->parts[im->nparts++];
            gpt_name(&e[i], p->name);
            p->first = e[i].first_lba;
            p->last = e[i].last_lba;
            memcpy(p->type, e[i].type, 16);
            memcpy(p->guid, e[i].guid, 16);
            p->attributes = e[i].attributes;
            p->efs_type = !memcmp(e[i].type, GPT_TYPE_EFS, 16);
            if (p->last >= p->first && (p->last + 1) * GPT_SECTOR <= im->size) {
                if (!memcmp(e[i].type, GPT_TYPE_SWAP, 16))
                    check_swap(im, p);
                else
                    add_efs_part(im, p, replay);
            }
        }
        memcpy(im->disk_guid, h->disk_guid, 16);
        return 0;
    }
    /* a bare efs filesystem */
    struct part *p = &im->parts[0];
    p->first = 0;
    p->last = im->size / GPT_SECTOR - 1;
    add_efs_part(im, p, replay);
    if (!p->is_efs) {
        fprintf(stderr, "%s: not an efs image (and not a GPT disk)\n", path);
        return -1;
    }
    snprintf(p->name, sizeof(p->name), "%.16s", p->fs.sb->label[0] ? p->fs.sb->label : "efs");
    im->nparts = 1;
    return 0;
}

int image_save(struct image *im)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", im->path, (int)getpid());
    struct stat st;
    mode_t mode = stat(im->path, &st) == 0 ? st.st_mode & 0777 : 0644;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", tmp, strerror(errno));
        return -1;
    }
    for (uint64_t done = 0; done < im->size;) {
        ssize_t n = write(fd, im->data + done, im->size - done > (1u << 30) ? (1u << 30) : im->size - done);
        if (n <= 0) {
            fprintf(stderr, "%s: write error: %s\n", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        done += (uint64_t)n;
    }
    if (fsync(fd) < 0 || close(fd) < 0 || rename(tmp, im->path) < 0) {
        fprintf(stderr, "%s: %s\n", im->path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Swap space: the header page; the slots need no initialization. */
static void format_swap(struct image *im, struct part *p)
{
    struct swap_header *h = (void *)(im->data + p->first * GPT_SECTOR);
    memset(h, 0, SWAP_PAGE);
    memcpy(h->magic, SWAP_MAGIC, 8);
    h->version = 1;
    h->npages = (uint32_t)((p->last - p->first + 1) * GPT_SECTOR / SWAP_PAGE);
    snprintf(h->label, sizeof(h->label), "%s", p->name);
    p->is_swap = true;
    p->swap_pages = h->npages;
}

static int format_part(struct image *im, struct part *p)
{
    if (p->is_swap || !memcmp(p->type, GPT_TYPE_SWAP, 16)) {
        format_swap(im, p);
        return 0;
    }
    uint32_t nblocks = (uint32_t)((p->last - p->first + 1) * GPT_SECTOR / EFS_BSIZE);
    int r = efs_format(&p->fs, im->data + p->first * GPT_SECTOR, nblocks, p->name);
    if (r) {
        fprintf(stderr, "%s: partition %s (%llu MiB): %s\n", im->path, p->name,
                (unsigned long long)((p->last - p->first + 1) >> 11), efs_strerror(r));
        return -1;
    }
    p->is_efs = true;
    return 0;
}

/* The protective MBR, both partition tables and both headers, for im->size
   bytes and the partitions in im->parts. */
static void write_gpt(struct image *im)
{
    uint64_t total = im->size / GPT_SECTOR;
    uint8_t *mbr = im->data;
    memset(mbr, 0, GPT_SECTOR);
    uint8_t *pe = mbr + 446;
    pe[4] = 0xee;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;  /* CHS of LBA 1 */
    pe[5] = 0xff; pe[6] = 0xff; pe[7] = 0xff;
    uint32_t start = 1, count = total - 1 > 0xffffffffu ? 0xffffffffu : (uint32_t)(total - 1);
    memcpy(pe + 8, &start, 4);
    memcpy(pe + 12, &count, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;

    struct gpt_entry *ent = (struct gpt_entry *)(im->data + 2 * GPT_SECTOR);
    memset(ent, 0, GPT_NENTRIES * GPT_ENTRY_SIZE);
    for (int i = 0; i < im->nparts; i++) {
        struct part *p = &im->parts[i];
        memcpy(ent[i].type, p->type, 16);
        memcpy(ent[i].guid, p->guid, 16);
        ent[i].first_lba = p->first;
        ent[i].last_lba = p->last;
        ent[i].attributes = p->attributes;
        for (int k = 0; p->name[k] && k < GPT_NAME_LEN; k++)
            ent[i].name[k] = (uint8_t)p->name[k];
    }
    struct gpt_header h;
    memset(&h, 0, sizeof(h));
    h.signature = GPT_SIGNATURE;
    h.revision = GPT_REVISION;
    h.header_size = GPT_HEADER_SIZE;
    h.first_usable = GPT_FIRST_USABLE;
    h.last_usable = total - GPT_ENTRY_SECTORS - 2;
    memcpy(h.disk_guid, im->disk_guid, 16);
    h.num_entries = GPT_NENTRIES;
    h.entry_size = GPT_ENTRY_SIZE;
    h.entries_crc = gpt_crc32(ent, GPT_NENTRIES * GPT_ENTRY_SIZE);
    /* primary */
    memset(im->data + GPT_SECTOR, 0, GPT_SECTOR);
    h.my_lba = 1;
    h.alternate_lba = total - 1;
    h.entries_lba = 2;
    h.header_crc = 0;
    h.header_crc = gpt_crc32(&h, GPT_HEADER_SIZE);
    memcpy(im->data + 1 * GPT_SECTOR, &h, GPT_HEADER_SIZE);
    /* backup: the entries, then the header in the last sector */
    memcpy(im->data + (total - 1 - GPT_ENTRY_SECTORS) * GPT_SECTOR, ent, GPT_NENTRIES * GPT_ENTRY_SIZE);
    memset(im->data + (total - 1) * GPT_SECTOR, 0, GPT_SECTOR);
    h.my_lba = total - 1;
    h.alternate_lba = 1;
    h.entries_lba = total - 1 - GPT_ENTRY_SECTORS;
    h.header_crc = 0;
    h.header_crc = gpt_crc32(&h, GPT_HEADER_SIZE);
    memcpy(im->data + (total - 1) * GPT_SECTOR, &h, GPT_HEADER_SIZE);
}

static int name_ok(const struct image *im, const char *name)
{
    if (strlen(name) > 16 || !name[0]) {
        fprintf(stderr, "%s: partition name '%s' (1-16 characters: it is the efs label too)\n", im->path, name);
        return 0;
    }
    return 1;
}

/* A new partition's identity: its name, type and GUID. */
static void new_part(struct part *p, const char *name, uint64_t first, uint32_t mb)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->first = first;
    p->last = first + (uint64_t)mb * 2048 - 1;
    memcpy(p->type, strcmp(name, SWAP_NAME) ? GPT_TYPE_EFS : GPT_TYPE_SWAP, 16);
    p->efs_type = strcmp(name, SWAP_NAME) != 0;
    random_guid(p->guid);
}

int image_create(struct image *im, const char *path, const struct layout *layout, int n)
{
    memset(im, 0, sizeof(*im));
    im->path = path;
    if (n < 1 || n > IMAGE_MAX_PARTS) {
        fprintf(stderr, "%s: 1 to %d partitions\n", path, IMAGE_MAX_PARTS);
        return -1;
    }
    /* partitions on 1 MiB boundaries after the primary table; the backup at the end */
    uint64_t lba = GPT_ALIGN;
    for (int i = 0; i < n; i++) {
        if (!name_ok(im, layout[i].name))
            return -1;
        new_part(&im->parts[i], layout[i].name, lba, layout[i].mb);
        lba = im->parts[i].last + 1;
    }
    uint64_t total = lba + GPT_ENTRY_SECTORS + 1;
    total = (total + GPT_ALIGN - 1) / GPT_ALIGN * GPT_ALIGN;     /* a whole number of MiB */
    im->size = total * GPT_SECTOR;
    im->data = calloc(1, im->size);
    if (!im->data) {
        fprintf(stderr, "%s: out of memory\n", path);
        return -1;
    }
    im->gpt = true;
    im->nparts = n;
    random_guid(im->disk_guid);
    write_gpt(im);
    for (int i = 0; i < n; i++)
        if (format_part(im, &im->parts[i]))
            return -1;
    return 0;
}

int image_add(struct image *im, const char *name, uint32_t mb)
{
    if (!im->gpt) {
        fprintf(stderr, "%s: not a GPT disk: no room for partitions\n", im->path);
        return -1;
    }
    if (im->nparts == IMAGE_MAX_PARTS || !name_ok(im, name) || !mb) {
        if (im->nparts == IMAGE_MAX_PARTS)
            fprintf(stderr, "%s: already %d partitions\n", im->path, IMAGE_MAX_PARTS);
        return -1;
    }
    for (int i = 0; i < im->nparts; i++) {
        if (!strcmp(im->parts[i].name, name)) {
            fprintf(stderr, "%s: there is a partition %s already\n", im->path, name);
            return -1;
        }
    }
    uint64_t end = GPT_FIRST_USABLE;
    for (int i = 0; i < im->nparts; i++)
        if (im->parts[i].last + 1 > end)
            end = im->parts[i].last + 1;
    uint64_t first = (end + GPT_ALIGN - 1) / GPT_ALIGN * GPT_ALIGN;
    struct part *p = &im->parts[im->nparts];
    new_part(p, name, first, mb);
    uint64_t total = (p->last + 1 + GPT_ENTRY_SECTORS + 1 + GPT_ALIGN - 1) / GPT_ALIGN * GPT_ALIGN;
    uint8_t *data = realloc(im->data, total * GPT_SECTOR);
    if (!data) {
        fprintf(stderr, "%s: out of memory\n", im->path);
        return -1;
    }
    im->data = data;
    im->size = total * GPT_SECTOR;
    /* the old backup table is in the new space: clear everything after the
       last partition */
    memset(im->data + end * GPT_SECTOR, 0, im->size - end * GPT_SECTOR);
    for (int i = 0; i < im->nparts; i++) {      /* the buffer moved: reopen */
        struct part *q = &im->parts[i];
        if (q->is_efs) {
            uint32_t replayed = q->fs.replayed;
            efs_open(&q->fs, im->data + q->first * GPT_SECTOR,
                     (uint32_t)((q->last - q->first + 1) * GPT_SECTOR / EFS_BSIZE), false);
            q->fs.replayed = replayed;
        }
    }
    im->nparts++;
    write_gpt(im);
    return format_part(im, p);
}

struct part *image_part(struct image *im, const char *name)
{
    for (int i = 0; i < im->nparts; i++) {
        struct part *p = &im->parts[i];
        if (!name && !strcmp(p->name, "root") && p->is_efs)
            return p;
        if (name && (!strcmp(p->name, name) ||
                     (p->is_efs && !strncmp(p->fs.sb->label, name, 16) && strlen(name) <= 16)))
            return p;
    }
    if (!name)
        for (int i = 0; i < im->nparts; i++)
            if (im->parts[i].is_efs)
                return &im->parts[i];
    return NULL;
}
