/*
 * virtio 9P transport: a channel to a 9P server, which for QEMU's
 * virtio-9p-device is a directory on the host (-fsdev local,...). Each
 * request is one descriptor chain: the T-message for the device to read,
 * then a buffer it writes the R-message into. Callers sleep until the
 * interrupt reports their reply; many requests can be in flight.
 *
 * The 9P protocol itself (9P2000.L) is in kernel/fs/ninep.c.
 */
#define LOG_SUBSYS SUBSYS_DRV
#include <debug.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <printk.h>
#include <uapi/errno.h>
#include <virtio.h>

#define VIRTIO_9P_F_MOUNT_TAG (1ULL << 0)

struct p9req {
    bool done;
    u32 len;                    /* bytes of reply the device wrote */
};

struct p9dev {
    struct virtio_dev dev;
    struct virtqueue vq;
    char tag[P9_TAG_MAX + 1];   /* the mount tag (-device ...,mount_tag=NAME) */
    struct waitqueue done_wq;
    u64 requests, bytes_out, bytes_in;
};

static struct p9dev devs[MAX_9P];
static int ndevs;

static void p9_irq(void *arg)
{
    struct p9dev *d = arg;
    virtio_ack_irq(&d->dev);
    struct p9req *r;
    u32 len;
    bool any = false;
    while ((r = virtq_get(&d->vq, &len)) != NULL) {
        r->len = len;
        r->done = true;
        any = true;
    }
    if (any)
        wake_up(&d->done_wq);
}

/* The mount tag is in the device's config space: le16 length, then bytes. */
static void read_tag(struct p9dev *d)
{
    u32 len = virtio_config_read8(&d->dev, 0) | (u32)virtio_config_read8(&d->dev, 1) << 8;
    if (len > P9_TAG_MAX)
        len = P9_TAG_MAX;
    for (u32 i = 0; i < len; i++)
        d->tag[i] = (char)virtio_config_read8(&d->dev, 2 + i);
    d->tag[len] = 0;
}

int p9_init(void)
{
    int slot = 0;
    while (ndevs < MAX_9P) {
        struct p9dev *d = &devs[ndevs];
        if (virtio_find_from(slot, VIRTIO_DEV_9P, VIRTIO_9P_F_MOUNT_TAG, &d->dev))
            break;
        slot = d->dev.slot + 1;
        if (virtq_init(&d->dev, &d->vq, 0, 64)) {
            pr_err("virtio-9p: cannot set up the request queue\n");
            continue;
        }
        read_tag(d);
        waitqueue_init(&d->done_wq, "9p-rpc");
        irq_register(d->dev.irq, "virtio-9p", p9_irq, d);
        virtio_ready(&d->dev);
        pr_info("9p%d: host share \"%s\"\n", ndevs, d->tag);
        ndevs++;
    }
    return ndevs;
}

int p9_count(void) { return ndevs; }
const char *p9_tag(int i) { return i >= 0 && i < ndevs ? devs[i].tag : ""; }

/*
 * Send a T-message and wait for the R-message. Both buffers must be kernel
 * memory in the direct map (kmalloc), each physically contiguous. Returns
 * the reply's length, or -errno.
 */
long p9_transact(int i, const void *tx, u32 txlen, void *rx, u32 rxcap)
{
    if (i < 0 || i >= ndevs)
        return -ENODEV;
    struct p9dev *d = &devs[i];
    struct p9req *r = kzalloc(sizeof(*r));
    if (!r)
        return -ENOMEM;
    struct virtio_buf bufs[2] = {
        { dma_addr(tx), txlen, false },
        { dma_addr(rx), rxcap, true },
    };
    wait_event(&d->vq.free_wq, virtq_add(&d->vq, bufs, 2, r) == 0);
    virtq_kick(&d->dev, &d->vq);
    current->wait_what = "9P reply";
    wait_event(&d->done_wq, r->done);
    long len = r->len;
    kfree(r);
    d->requests++;
    d->bytes_out += txlen;
    d->bytes_in += len;
    return len;
}

void p9_print(int (*print)(const char *fmt, ...))
{
    for (int i = 0; i < ndevs; i++)
        print("9p%d: tag \"%s\", %lu requests, %lu bytes out, %lu bytes in\n", i, devs[i].tag,
              (unsigned long)devs[i].requests, (unsigned long)devs[i].bytes_out,
              (unsigned long)devs[i].bytes_in);
}
