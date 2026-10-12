#ifndef EUNUCHS_GPT_FORMAT_H
#define EUNUCHS_GPT_FORMAT_H

/*
 * GUID partition tables (UEFI's GPT), as Eunuchs uses them: the disk image is
 * one GPT disk with an efs filesystem in each partition (boot, boot.old,
 * root, home; tools/eudisk makes them). Shared by the kernel
 * (kernel/drivers/virtio_blk.c), the boot loader (boot/eboot.c) and the host
 * tools, so it uses only <stdint.h> and <stddef.h>.
 *
 * Layout in 512-byte sectors (LBAs): 0 a protective MBR, 1 the header, 2..33
 * the 128 partition entries, partitions (1 MiB aligned), then a copy of the
 * entries and, in the last sector, the backup header. Both copies carry
 * CRC32s; macOS's diskutil and hdiutil read the table.
 */

#include <stddef.h>
#include <stdint.h>

#define GPT_SECTOR          512
#define GPT_SIGNATURE       0x5452415020494645ULL      /* "EFI PART" */
#define GPT_REVISION        0x00010000
#define GPT_HEADER_SIZE     92
#define GPT_NENTRIES        128
#define GPT_ENTRY_SIZE      128
#define GPT_ENTRY_SECTORS   (GPT_NENTRIES * GPT_ENTRY_SIZE / GPT_SECTOR)   /* 32 */
#define GPT_FIRST_USABLE    (2 + GPT_ENTRY_SECTORS)                       /* 34 */
#define GPT_ALIGN           2048                        /* partitions start on 1 MiB */
#define GPT_NAME_LEN        36                          /* UTF-16 code units */

struct gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;        /* of the first header_size bytes, this field 0 */
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc;       /* of num_entries * entry_size bytes */
};

struct gpt_entry {
    uint8_t type[16];           /* all zero: unused */
    uint8_t guid[16];
    uint64_t first_lba;
    uint64_t last_lba;          /* inclusive */
    uint64_t attributes;
    uint16_t name[GPT_NAME_LEN];    /* UTF-16LE, NUL-padded */
};

/* The partition type of an efs filesystem (the GUID 53465545-0000-4e55-8000-4555
   4e554348 in GPT's mixed-endian byte order: "EUFS" ... "EUNUCH"). */
static const uint8_t GPT_TYPE_EFS[16] = {
    0x45, 0x55, 0x46, 0x53, 0x00, 0x00, 0x55, 0x4e,
    0x80, 0x00, 0x45, 0x55, 0x4e, 0x55, 0x43, 0x48,
};

/* The CRC32 (IEEE 802.3, as zlib) that GPT uses. Bitwise: small and slow,
   which is fine for 16 KiB of table. */
static inline uint32_t gpt_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

/* (in pieces: crc = 0, then crc = gpt_crc32_update(crc, piece, n) for each) */
static inline uint32_t gpt_crc32(const void *data, size_t len)
{
    return gpt_crc32_update(0, data, len);
}

/* A partition's name as ASCII (other characters become '?'). */
static inline void gpt_name(const struct gpt_entry *e, char out[GPT_NAME_LEN + 1])
{
    int i = 0;
    for (; i < GPT_NAME_LEN && e->name[i]; i++)
        out[i] = e->name[i] < 128 ? (char)e->name[i] : '?';
    out[i] = 0;
}

_Static_assert(sizeof(struct gpt_entry) == GPT_ENTRY_SIZE, "gpt_entry size");
_Static_assert(offsetof(struct gpt_header, entries_crc) + 4 == GPT_HEADER_SIZE, "gpt_header layout");

#endif
