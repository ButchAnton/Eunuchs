/*
 * virtio-mmio transport (modern, version 2) and split virtqueues.
 * Spec: Virtual I/O Device (VIRTIO) Version 1.2, sections 2.7 and 4.2.
 */
#define LOG_SUBSYS SUBSYS_DRV
#include <debug.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <virtio.h>

#define VIRTIO_MAGIC        0x74726976  /* "virt" */

#define REG_MAGIC           0x000
#define REG_VERSION         0x004
#define REG_DEVICE_ID       0x008
#define REG_DEV_FEATURES    0x010
#define REG_DEV_FEATURES_SEL 0x014
#define REG_DRV_FEATURES    0x020
#define REG_DRV_FEATURES_SEL 0x024
#define REG_QUEUE_SEL       0x030
#define REG_QUEUE_NUM_MAX   0x034
#define REG_QUEUE_NUM       0x038
#define REG_QUEUE_READY     0x044
#define REG_QUEUE_NOTIFY    0x050
#define REG_INT_STATUS      0x060
#define REG_INT_ACK         0x064
#define REG_STATUS          0x070
#define REG_QUEUE_DESC_LO   0x080
#define REG_QUEUE_DESC_HI   0x084
#define REG_QUEUE_AVAIL_LO  0x090
#define REG_QUEUE_AVAIL_HI  0x094
#define REG_QUEUE_USED_LO   0x0a0
#define REG_QUEUE_USED_HI   0x0a4
#define REG_CONFIG          0x100

#define STATUS_ACK          1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4
#define STATUS_FEATURES_OK  8
#define STATUS_FAILED       128

#define VIRTIO_F_VERSION_1  (1ULL << 32)

static inline u32 rd(struct virtio_dev *d, u32 off) { return *(volatile u32 *)(d->regs + off); }
static inline void wr(struct virtio_dev *d, u32 off, u32 v) { *(volatile u32 *)(d->regs + off) = v; }

static const char *device_name(u32 id)
{
    switch (id) {
    case 0: return "(none)";
    case VIRTIO_DEV_NET: return "net";
    case VIRTIO_DEV_BLOCK: return "block";
    case VIRTIO_DEV_9P: return "9p";
    case 3: return "console";
    case 4: return "rng";
    case 16: return "gpu";
    case 18: return "input";
    }
    return "other";
}

uintptr_t dma_addr(const void *va)
{
    uintptr_t a = (uintptr_t)va, pa;
    if (a >= KERNEL_BASE && a < KERNEL_BASE + platform.mem_base + platform.mem_size)
        return __pa(a);
    if (vm_translate(kernel_pagetable, a, &pa, NULL))
        panic("dma_addr: %p is not mapped", va);
    return pa;          /* caller guarantees the buffer does not cross a page */
}

int virtio_find(u32 device_id, u64 want, struct virtio_dev *dev)
{
    return virtio_find_from(0, device_id, want, dev);
}

/* The first device of this type in slot `from` or later (dev->slot tells where). */
int virtio_find_from(int from, u32 device_id, u64 want, struct virtio_dev *dev)
{
    for (int i = from; i < platform.nvirtio; i++) {
        uintptr_t regs = (uintptr_t)ioremap(platform.virtio[i].base, PAGE_SIZE);
        struct virtio_dev d = { .slot = i, .regs = regs, .irq = platform.virtio[i].irq };
        if (rd(&d, REG_MAGIC) != VIRTIO_MAGIC || rd(&d, REG_DEVICE_ID) != device_id)
            continue;
        if (rd(&d, REG_VERSION) != 2) {
            pr_err("virtio%d: legacy (version %u) interface; run QEMU with "
                   "-global virtio-mmio.force-legacy=false\n", i, rd(&d, REG_VERSION));
            return -1;
        }
        d.device_id = device_id;

        wr(&d, REG_STATUS, 0);                              /* reset */
        wr(&d, REG_STATUS, STATUS_ACK);
        wr(&d, REG_STATUS, STATUS_ACK | STATUS_DRIVER);
        wr(&d, REG_DEV_FEATURES_SEL, 0);
        u64 offered = rd(&d, REG_DEV_FEATURES);
        wr(&d, REG_DEV_FEATURES_SEL, 1);
        offered |= (u64)rd(&d, REG_DEV_FEATURES) << 32;
        want |= VIRTIO_F_VERSION_1;
        d.features = offered & want;
        if (!(d.features & VIRTIO_F_VERSION_1)) {
            pr_err("virtio%d: device does not offer VIRTIO_F_VERSION_1\n", i);
            wr(&d, REG_STATUS, STATUS_FAILED);
            return -1;
        }
        wr(&d, REG_DRV_FEATURES_SEL, 0);
        wr(&d, REG_DRV_FEATURES, (u32)d.features);
        wr(&d, REG_DRV_FEATURES_SEL, 1);
        wr(&d, REG_DRV_FEATURES, (u32)(d.features >> 32));
        wr(&d, REG_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
        if (!(rd(&d, REG_STATUS) & STATUS_FEATURES_OK)) {
            pr_err("virtio%d: device rejected features %#lx\n", i, (unsigned long)d.features);
            wr(&d, REG_STATUS, STATUS_FAILED);
            return -1;
        }
        *dev = d;
        pr_info("virtio%d: %s device at %#lx irq %u, features %#lx\n", i,
                device_name(device_id), (unsigned long)platform.virtio[i].base, d.irq,
                (unsigned long)d.features);
        return 0;
    }
    return -1;
}

int virtq_init(struct virtio_dev *dev, struct virtqueue *q, u16 index, u16 num)
{
    wr(dev, REG_QUEUE_SEL, index);
    if (rd(dev, REG_QUEUE_READY))
        return -1;
    u32 max = rd(dev, REG_QUEUE_NUM_MAX);
    if (!max)
        return -1;
    if (num > max)
        num = max;
    if (num > 128)
        num = 128;              /* desc + avail + used fit in one page */

    u8 *page = page_alloc_zeroed();
    if (!page)
        return -1;
    q->index = index;
    q->num = num;
    q->desc = (struct virtq_desc *)page;
    q->avail = (struct virtq_avail *)(page + 16 * num);
    q->used = (struct virtq_used *)(page + ALIGN_UP(16 * num + 6 + 2 * num, 64));
    q->cookies = kzalloc(num * sizeof(void *));
    for (u16 i = 0; i < num; i++)
        q->desc[i].next = i + 1;
    q->free_head = 0;
    q->nfree = num;
    q->last_used = 0;
    spin_init(&q->lock, "virtqueue");
    waitqueue_init(&q->free_wq, "virtq-desc");

    wr(dev, REG_QUEUE_NUM, num);
    u64 d = __pa(q->desc), a = __pa(q->avail), u = __pa(q->used);
    wr(dev, REG_QUEUE_DESC_LO, (u32)d);
    wr(dev, REG_QUEUE_DESC_HI, (u32)(d >> 32));
    wr(dev, REG_QUEUE_AVAIL_LO, (u32)a);
    wr(dev, REG_QUEUE_AVAIL_HI, (u32)(a >> 32));
    wr(dev, REG_QUEUE_USED_LO, (u32)u);
    wr(dev, REG_QUEUE_USED_HI, (u32)(u >> 32));
    wr(dev, REG_QUEUE_READY, 1);
    return 0;
}

void virtio_ready(struct virtio_dev *dev)
{
    wr(dev, REG_STATUS, rd(dev, REG_STATUS) | STATUS_DRIVER_OK);
}

int virtq_add(struct virtqueue *q, const struct virtio_buf *bufs, int n, void *cookie)
{
    unsigned long flags = spin_lock_irqsave(&q->lock);
    if (q->nfree < n) {
        spin_unlock_irqrestore(&q->lock, flags);
        return -1;
    }
    u16 head = q->free_head, idx = head, last = head;
    for (int i = 0; i < n; i++) {
        struct virtq_desc *d = &q->desc[idx];
        d->addr = bufs[i].addr;
        d->len = bufs[i].len;
        d->flags = (bufs[i].device_writes ? VIRTQ_DESC_F_WRITE : 0) |
                   (i + 1 < n ? VIRTQ_DESC_F_NEXT : 0);
        last = idx;
        idx = d->next;
    }
    q->free_head = q->desc[last].next;
    q->nfree -= n;
    q->cookies[head] = cookie;
    q->avail->ring[q->avail->idx % q->num] = head;
    __sync_synchronize();               /* descriptors before the index */
    q->avail->idx++;
    q->submitted++;
    spin_unlock_irqrestore(&q->lock, flags);
    return 0;
}

void virtq_kick(struct virtio_dev *dev, struct virtqueue *q)
{
    __sync_synchronize();
    wr(dev, REG_QUEUE_NOTIFY, q->index);
}

void *virtq_get(struct virtqueue *q, u32 *len)
{
    unsigned long flags = spin_lock_irqsave(&q->lock);
    __sync_synchronize();
    if (q->last_used == q->used->idx) {
        spin_unlock_irqrestore(&q->lock, flags);
        return NULL;
    }
    struct virtq_used_elem e = q->used->ring[q->last_used % q->num];
    q->last_used++;
    void *cookie = q->cookies[e.id];
    q->cookies[e.id] = NULL;
    if (len)
        *len = e.len;

    /* Return the chain to the free list. */
    u16 idx = (u16)e.id, count = 1;
    while (q->desc[idx].flags & VIRTQ_DESC_F_NEXT) {
        idx = q->desc[idx].next;
        count++;
    }
    q->desc[idx].next = q->free_head;
    q->free_head = (u16)e.id;
    q->nfree += count;
    q->completed++;
    spin_unlock_irqrestore(&q->lock, flags);
    wake_up(&q->free_wq);
    if (!cookie)
        pr_err("virtqueue %u: completion for descriptor %u with no request\n", q->index, e.id);
    return cookie;
}

u32 virtio_ack_irq(struct virtio_dev *dev)
{
    u32 st = rd(dev, REG_INT_STATUS);
    wr(dev, REG_INT_ACK, st);
    return st;
}

u32 virtio_config_read32(struct virtio_dev *dev, u32 off)
{
    return rd(dev, REG_CONFIG + off);
}

/* One byte: QEMU fails wider reads that run past the end of a short config space. */
u8 virtio_config_read8(struct virtio_dev *dev, u32 off)
{
    return *(volatile u8 *)(dev->regs + REG_CONFIG + off);
}

void virtio_print(print_fn print)
{
    for (int i = 0; i < platform.nvirtio; i++) {
        uintptr_t regs = (uintptr_t)ioremap(platform.virtio[i].base, PAGE_SIZE);
        struct virtio_dev d = { .regs = regs };
        u32 id = rd(&d, REG_DEVICE_ID);
        print("  virtio%d @ %#lx irq %u: %s (version %u, status %#x)\n", i,
              (unsigned long)platform.virtio[i].base, platform.virtio[i].irq, device_name(id),
              rd(&d, REG_VERSION), rd(&d, REG_STATUS));
    }
}
