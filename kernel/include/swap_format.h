#ifndef EUNUCHS_SWAP_FORMAT_H
#define EUNUCHS_SWAP_FORMAT_H

/*
 * Swap space: a GPT partition named "swap" (type GPT_TYPE_SWAP), where the
 * kernel keeps user pages it has pushed out of memory (kernel/mm/swap.c).
 * Page 0 is this header; pages 1 .. npages-1 are slots of one page each.
 * Nothing else is on disk: which slot holds what lives only in memory, so
 * swap starts empty at every boot. Shared by the kernel and the host tools
 * (tools/efs/image.c writes the header when it makes the partition).
 */

#include <stdint.h>

#define SWAP_MAGIC       "EUSWAP01"
#define SWAP_PAGE        4096
#define SWAP_NAME        "swap"

struct swap_header {
    char magic[8];              /* SWAP_MAGIC, not NUL-terminated */
    uint32_t version;           /* 1 */
    uint32_t npages;            /* pages in the partition, header included */
    char label[16];
};

/* The partition type: the GUID 57535545-5041-4e55-8000-45554e554348 in GPT's
   mixed-endian byte order ("EUSWAPUN" ... "EUNUCH"). */
static const uint8_t GPT_TYPE_SWAP[16] = {
    0x45, 0x55, 0x53, 0x57, 0x41, 0x50, 0x55, 0x4e,
    0x80, 0x00, 0x45, 0x55, 0x4e, 0x55, 0x43, 0x48,
};

#endif
