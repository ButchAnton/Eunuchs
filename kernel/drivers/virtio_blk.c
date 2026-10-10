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

static struct virtio_dev dev;
static struct virtqueue vq;
static bool present, readonly;
static u64 capacity;
static struct waitqueue done_wq = WAITQUEUE_INIT(done_wq, "blk-io");
static u64 n_reads, n_writes, sectors_read, sectors_written, n_errors, busy_ticks;
static u64 max_latency, total_latency;

static void blk_irq(void *arg)
{
    (void)arg;
    virtio_ack_irq(&dev);
    struct blk_req *r;
    u32 len;
    bool any = false;
    while ((r = virtq_get(&vq, &len)) != NULL) {
        u64 lat = rdtime() - r->start;
        total_latency += lat;
        if (lat > max_latency)
            max_latency = lat;
        trace(blk_done, r->hdr.sector, r->status, lat * 1000000 / platform.timebase);
        r->done = true;
        any = true;
    }
    if (any)
        wake_up(&done_wq);
}

int blk_init(void)
{
    if (virtio_find(VIRTIO_DEV_BLOCK, VIRTIO_BLK_F_RO, &dev)) {
        pr_info("no virtio block device\n");
        return -1;
    }
    readonly = dev.features & VIRTIO_BLK_F_RO;
    capacity = virtio_config_read32(&dev, 0) | (u64)virtio_config_read32(&dev, 4) << 32;
    if (virtq_init(&dev, &vq, 0, 64)) {
        pr_err("virtio-blk: cannot set up the request queue\n");
        return -1;
    }
    irq_register(dev.irq, "virtio-blk", blk_irq, NULL);
    virtio_ready(&dev);
    present = true;
    pr_info("disk: %lu MiB (%lu sectors)%s, queue of %u descriptors\n",
            (unsigned long)(capacity >> 11), (unsigned long)capacity,
            readonly ? ", read-only" : "", vq.num);
    return 0;
}

bool blk_present(void) { return present; }
u64 blk_capacity(void) { return capacity; }

int blk_rw(u64 sector, void *buf, u32 len, bool write)
{
    if (!present)
        return -ENODEV;
    if (len % 512 || sector + len / 512 > capacity) {
        pr_err("blk_rw: bad request sector %lu len %u (capacity %lu sectors)\n",
               (unsigned long)sector, len, (unsigned long)capacity);
        return -EINVAL;
    }
    if (write && readonly)
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
    wait_event(&vq.free_wq, virtq_add(&vq, bufs, 3, r) == 0);
    virtq_kick(&dev, &vq);
    current->wait_what = "disk I/O";
    wait_event(&done_wq, r->done);

    int err = r->status == VIRTIO_BLK_S_OK ? 0 : -EIO;
    if (err) {
        n_errors++;
        pr_err("disk %s error at sector %lu (status %u)\n", write ? "write" : "read",
               (unsigned long)sector, r->status);
    }
    if (write) {
        n_writes++;
        sectors_written += len / 512;
    } else {
        n_reads++;
        sectors_read += len / 512;
    }
    busy_ticks += rdtime() - r->start;
    kfree(r);
    return err;
}

void blk_print(print_fn print)
{
    u64 tb = platform.timebase, n = n_reads + n_writes;
    if (!present) {
        print("no block device\n");
        return;
    }
    print("virtio-blk: %lu MiB%s, queue %u descriptors (%u free), irq %u\n",
          (unsigned long)(capacity >> 11), readonly ? " read-only" : "", vq.num, vq.nfree, dev.irq);
    print("  reads   %8lu  (%lu KiB)\n", (unsigned long)n_reads, (unsigned long)(sectors_read / 2));
    print("  writes  %8lu  (%lu KiB)\n", (unsigned long)n_writes, (unsigned long)(sectors_written / 2));
    print("  errors  %8lu\n", (unsigned long)n_errors);
    print("  latency avg %lu us, max %lu us\n",
          (unsigned long)(n ? total_latency * 1000000 / tb / n : 0),
          (unsigned long)(max_latency * 1000000 / tb));
    print("  in flight: %lu\n", (unsigned long)(vq.submitted - vq.completed));
}
