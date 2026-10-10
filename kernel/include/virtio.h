#ifndef EUNUCHS_VIRTIO_H
#define EUNUCHS_VIRTIO_H

/*
 * virtio over MMIO (modern interface, virtio 1.x) and split virtqueues.
 * QEMU must run with -global virtio-mmio.force-legacy=false.
 */

#include <spinlock.h>
#include <sched.h>
#include <types.h>

#define VIRTIO_DEV_NET   1
#define VIRTIO_DEV_BLOCK 2

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2      /* device writes this buffer */

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[];
};

struct virtq_used_elem {
    u32 id;
    u32 len;
};

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[];
};

struct virtqueue {
    u16 index;
    u16 num;
    struct virtq_desc *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used *used;
    u16 free_head;
    u16 nfree;
    u16 last_used;
    void **cookies;             /* per head descriptor */
    spinlock_t lock;
    struct waitqueue free_wq;   /* waiters for free descriptors */
    u64 submitted, completed;
};

struct virtio_dev {
    int slot;                   /* index in platform.virtio[] */
    uintptr_t regs;             /* kernel VA */
    u32 irq;
    u32 device_id;
    u64 features;               /* negotiated */
};

struct virtio_buf {
    u64 addr;                   /* physical */
    u32 len;
    bool device_writes;
};

/* Find and initialize the first device of `device_id`; negotiates `want`
 * features (VIRTIO_F_VERSION_1 is always required). */
int virtio_find(u32 device_id, u64 want, struct virtio_dev *dev);
int virtq_init(struct virtio_dev *dev, struct virtqueue *q, u16 index, u16 num);
void virtio_ready(struct virtio_dev *dev);
/* Queue a chain; returns 0, or -1 if not enough free descriptors. */
int virtq_add(struct virtqueue *q, const struct virtio_buf *bufs, int n, void *cookie);
void virtq_kick(struct virtio_dev *dev, struct virtqueue *q);
/* Next completed chain's cookie (and bytes written), or NULL. */
void *virtq_get(struct virtqueue *q, u32 *len);
u32 virtio_ack_irq(struct virtio_dev *dev);
u32 virtio_config_read32(struct virtio_dev *dev, u32 off);
uintptr_t dma_addr(const void *va);     /* physical address of a kernel buffer */
void virtio_print(int (*print)(const char *fmt, ...));

/* virtio_blk.c */
int blk_init(void);
bool blk_present(void);
u64 blk_capacity(void);                 /* in 512-byte sectors */
int blk_rw(u64 sector, void *buf, u32 len, bool write);
void blk_print(int (*print)(const char *fmt, ...));

#endif
