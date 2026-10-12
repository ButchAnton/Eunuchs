/*
 * virtio block device. Requests are synchronous for the caller (it sleeps
 * until the interrupt reports completion) but many can be in flight.
 * Statistics: `blk` in the monitor, /proc/blk; trace events blk_submit/done.
 */
#define LOG_SUBSYS SUBSYS_BLK
#include <debug.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <trace.h>
#include <uapi/errno.h>
#include <virtio.h>

#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1
#define VIRTIO_BLK_S_OK   0
#define VIRTIO_BLK_F_RO   (1ULL << 5)

struct blk_req {
    /* Device-visible part: header (read by device) and status (written). */
    struct {
        u32 type;
        u32 reserved;
        u64 sector;
    } hdr;
    u8 status;
    bool done;
    u64 start;
};

/* One per disk: up to MAX_DISKS, numbered in the order they were found. */
struct disk {
    struct virtio_dev dev;
    struct virtqueue vq;
    bool readonly;
    u64 capacity;
    struct waitqueue done_wq;
    u64 n_reads, n_writes, sectors_read, sectors_written, n_errors, busy_ticks;
    u64 max_latency, total_latency;
};

static struct disk disks[MAX_DISKS];
static int ndisks;

static void blk_irq(void *arg)
{
    struct disk *d = arg;
    virtio_ack_irq(&d->dev);
    struct blk_req *r;
    u32 len;
    bool any = false;
    while ((r = virtq_get(&d->vq, &len)) != NULL) {
        u64 lat = rdtime() - r->start;
        d->total_latency += lat;
        if (lat > d->max_latency)
            d->max_latency = lat;
        trace(blk_done, r->hdr.sector, r->status, lat * 1000000 / platform.timebase);
        r->done = true;
        any = true;
    }
    if (any)
        wake_up(&d->done_wq);
}

int blk_init(void)
{
    int slot = 0;
    while (ndisks < MAX_DISKS) {
        struct disk *d = &disks[ndisks];
        if (virtio_find_from(slot, VIRTIO_DEV_BLOCK, VIRTIO_BLK_F_RO, &d->dev))
            break;
        slot = d->dev.slot + 1;
        d->readonly = d->dev.features & VIRTIO_BLK_F_RO;
        d->capacity = virtio_config_read32(&d->dev, 0) | (u64)virtio_config_read32(&d->dev, 4) << 32;
        if (virtq_init(&d->dev, &d->vq, 0, 64)) {
            pr_err("virtio-blk: cannot set up the request queue\n");
            continue;
        }
        waitqueue_init(&d->done_wq, "blk-io");
        irq_register(d->dev.irq, "virtio-blk", blk_irq, d);
        virtio_ready(&d->dev);
        pr_info("disk%d: %lu MiB (%lu sectors)%s, queue of %u descriptors\n", ndisks,
                (unsigned long)(d->capacity >> 11), (unsigned long)d->capacity,
                d->readonly ? ", read-only" : "", d->vq.num);
        ndisks++;
    }
    if (!ndisks)
        pr_info("no virtio block device\n");
    return ndisks;
}

int blk_count(void) { return ndisks; }
u64 blk_capacity(int disk) { return disk >= 0 && disk < ndisks ? disks[disk].capacity : 0; }

int blk_rw(int disk, u64 sector, void *buf, u32 len, bool write)
{
    if (disk < 0 || disk >= ndisks)
        return -ENODEV;
    struct disk *d = &disks[disk];
    if (len % 512 || sector + len / 512 > d->capacity) {
        pr_err("blk_rw: bad request disk%d sector %lu len %u (capacity %lu sectors)\n", disk,
               (unsigned long)sector, len, (unsigned long)d->capacity);
        return -EINVAL;
    }
    if (write && d->readonly)
        return -EROFS;
    if (((uintptr_t)buf & (PAGE_SIZE - 1)) + len > PAGE_SIZE && (uintptr_t)buf >= KSTACK_AREA)
        panic("blk_rw: buffer %p crosses a page outside the direct map", buf);

    struct blk_req *r = kzalloc(sizeof(*r));
    if (!r)
        return -ENOMEM;
    r->hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    r->hdr.sector = sector;
    r->status = 0xff;
    struct virtio_buf bufs[3] = {
        { dma_addr(&r->hdr), sizeof(r->hdr), false },
        { dma_addr(buf), len, !write },
        { dma_addr(&r->status), 1, true },
    };

    r->start = rdtime();
    trace(blk_submit, sector, len, write);
    wait_event(&d->vq.free_wq, virtq_add(&d->vq, bufs, 3, r) == 0);
    virtq_kick(&d->dev, &d->vq);
    current->wait_what = "disk I/O";
    wait_event(&d->done_wq, r->done);

    int err = r->status == VIRTIO_BLK_S_OK ? 0 : -EIO;
    if (err) {
        d->n_errors++;
        pr_err("disk%d %s error at sector %lu (status %u)\n", disk, write ? "write" : "read",
               (unsigned long)sector, r->status);
    }
    if (write) {
        d->n_writes++;
        d->sectors_written += len / 512;
    } else {
        d->n_reads++;
        d->sectors_read += len / 512;
    }
    d->busy_ticks += rdtime() - r->start;
    kfree(r);
    return err;
}

void blk_print(print_fn print)
{
    u64 tb = platform.timebase;
    if (!ndisks) {
        print("no block device\n");
        return;
    }
    for (int i = 0; i < ndisks; i++) {
        struct disk *d = &disks[i];
        u64 n = d->n_reads + d->n_writes;
        print("disk%d: virtio-blk %lu MiB%s, queue %u descriptors (%u free), irq %u\n", i,
              (unsigned long)(d->capacity >> 11), d->readonly ? " read-only" : "", d->vq.num,
              d->vq.nfree, d->dev.irq);
        print("  reads   %8lu  (%lu KiB)\n", (unsigned long)d->n_reads,
              (unsigned long)(d->sectors_read / 2));
        print("  writes  %8lu  (%lu KiB)\n", (unsigned long)d->n_writes,
              (unsigned long)(d->sectors_written / 2));
        print("  errors  %8lu\n", (unsigned long)d->n_errors);
        print("  latency avg %lu us, max %lu us\n",
              (unsigned long)(n ? d->total_latency * 1000000 / tb / n : 0),
              (unsigned long)(d->max_latency * 1000000 / tb));
        print("  in flight: %lu\n", (unsigned long)(d->vq.submitted - d->vq.completed));
    }
}
