/*
 * virtio block device. Requests are synchronous for the caller (it sleeps
 * until the interrupt reports completion) but many can be in flight.
 * Statistics: `blk` in the monitor, /proc/blk; trace events blk_submit/done.
 *
 * Above the disks are volumes, what the filesystems use: each partition of a
 * GPT disk (gpt_format.h; the disk image has boot, boot.old, root, home), or
 * a whole disk without a partition table. blk_rw and blk_capacity take a
 * volume number; blk_disk_rw reaches a whole disk (/dev/disk0).
 */
#define LOG_SUBSYS SUBSYS_BLK
#include <debug.h>
#include <fmt.h>
#include <gpt_format.h>
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

/* Partitions (or whole disks), numbered disk by disk in table order. */
struct volume {
    int disk;
    u64 first, sectors;
    char name[12];              /* disk0p3, or disk1 for a whole disk */
    char label[GPT_NAME_LEN + 1];   /* the GPT partition name; "" for a whole disk */
};
static struct volume volumes[MAX_VOLUMES];
static int nvolumes;

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

int blk_disk_count(void) { return ndisks; }
u64 blk_disk_capacity(int disk) { return disk >= 0 && disk < ndisks ? disks[disk].capacity : 0; }

int blk_disk_rw(int disk, u64 sector, void *buf, u32 len, bool write)
{
    if (disk < 0 || disk >= ndisks)
        return -ENODEV;
    struct disk *d = &disks[disk];
    if (len % 512 || sector + len / 512 > d->capacity) {
        pr_err("blk_disk_rw: bad request disk%d sector %lu len %u (capacity %lu sectors)\n", disk,
               (unsigned long)sector, len, (unsigned long)d->capacity);
        return -EINVAL;
    }
    if (write && d->readonly)
        return -EROFS;
    if (((uintptr_t)buf & (PAGE_SIZE - 1)) + len > PAGE_SIZE && (uintptr_t)buf >= KSTACK_AREA)
        panic("blk_disk_rw: buffer %p crosses a page outside the direct map", buf);

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

static void add_volume(int disk, u64 first, u64 sectors, const char *label, int partno)
{
    if (nvolumes == MAX_VOLUMES) {
        pr_warn("disk%d: more than %d volumes; partition %d ignored\n", disk, MAX_VOLUMES, partno);
        return;
    }
    struct volume *v = &volumes[nvolumes++];
    v->disk = disk;
    v->first = first;
    v->sectors = sectors;
    if (partno)
        ksnprintf(v->name, sizeof(v->name), "disk%dp%d", disk, partno);
    else
        ksnprintf(v->name, sizeof(v->name), "disk%d", disk);
    strlcpy(v->label, label, sizeof(v->label));
}

/* A disk's GPT (the primary table; the backup if that is damaged), as volumes.
   false if the disk has no valid table. */
static bool scan_gpt(int disk, u8 *page)
{
    u64 cap = disks[disk].capacity;
    for (int copy = 0; copy < 2; copy++) {
        u64 lba = copy ? cap - 1 : 1;
        if (cap < 2 * GPT_FIRST_USABLE || blk_disk_rw(disk, lba, page, GPT_SECTOR, false))
            return false;
        struct gpt_header h;
        memcpy(&h, page, sizeof(h));
        u32 want = h.header_crc;
        h.header_crc = 0;
        if (h.signature != GPT_SIGNATURE || h.header_size < GPT_HEADER_SIZE ||
            h.entry_size != GPT_ENTRY_SIZE || h.num_entries == 0 || h.num_entries > 1024 ||
            gpt_crc32(&h, GPT_HEADER_SIZE) != want ||
            h.entries_lba + (u64)h.num_entries * GPT_ENTRY_SIZE / GPT_SECTOR > cap)
            continue;
        /* the entries: check the CRC over all of them, then use them */
        u64 bytes = (u64)h.num_entries * GPT_ENTRY_SIZE;
        u32 crc = 0;
        bool ok = true;
        for (u64 off = 0; off < bytes && ok; off += PAGE_SIZE) {
            u32 n = (u32)MIN((u64)PAGE_SIZE, bytes - off);
            ok = blk_disk_rw(disk, h.entries_lba + off / GPT_SECTOR, page, ALIGN_UP(n, GPT_SECTOR), false) == 0;
            crc = gpt_crc32_update(crc, page, n);
        }
        if (!ok || crc != h.entries_crc) {
            pr_warn("disk%d: GPT %s table: bad entries\n", disk, copy ? "backup" : "primary");
            continue;
        }
        if (copy)
            pr_warn("disk%d: the primary GPT is damaged; using the backup\n", disk);
        int partno = 0;
        for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
            u32 n = (u32)MIN((u64)PAGE_SIZE, bytes - off);
            if (blk_disk_rw(disk, h.entries_lba + off / GPT_SECTOR, page, ALIGN_UP(n, GPT_SECTOR), false))
                break;
            for (u32 i = 0; i < n / GPT_ENTRY_SIZE; i++) {
                const struct gpt_entry *e = (const void *)(page + i * GPT_ENTRY_SIZE);
                static const u8 unused[16];
                partno++;
                if (!memcmp(e->type, unused, 16))
                    continue;
                char name[GPT_NAME_LEN + 1];
                gpt_name(e, name);
                if (e->last_lba < e->first_lba || e->last_lba >= cap) {
                    pr_warn("disk%d: partition %d (%s) is outside the disk; ignored\n", disk, partno, name);
                    continue;
                }
                add_volume(disk, e->first_lba, e->last_lba - e->first_lba + 1, name, partno);
            }
        }
        return true;
    }
    return false;
}

void blk_scan_volumes(void)
{
    u8 *page = page_alloc();
    if (!page)
        panic("blk_scan_volumes: no memory");
    nvolumes = 0;
    for (int d = 0; d < ndisks; d++) {
        int before = nvolumes;
        if (!scan_gpt(d, page))
            add_volume(d, 0, disks[d].capacity, "", 0);
        for (int i = before; i < nvolumes; i++) {
            struct volume *v = &volumes[i];
            pr_info("%s: %lu MiB%s%s%s\n", v->name, (unsigned long)(v->sectors >> 11),
                    v->label[0] ? " \"" : " (whole disk)", v->label, v->label[0] ? "\"" : "");
        }
    }
    page_free(page);
}

int blk_count(void) { return nvolumes; }
u64 blk_capacity(int vol) { return vol >= 0 && vol < nvolumes ? volumes[vol].sectors : 0; }
const char *blk_name(int vol) { return vol >= 0 && vol < nvolumes ? volumes[vol].name : NULL; }
const char *blk_label(int vol) { return vol >= 0 && vol < nvolumes ? volumes[vol].label : NULL; }

int blk_rw(int vol, u64 sector, void *buf, u32 len, bool write)
{
    if (vol < 0 || vol >= nvolumes)
        return -ENODEV;
    struct volume *v = &volumes[vol];
    if (len % 512 || sector + len / 512 > v->sectors) {
        pr_err("blk_rw: bad request %s sector %lu len %u (%lu sectors)\n", v->name, (unsigned long)sector,
               len, (unsigned long)v->sectors);
        return -EINVAL;
    }
    return blk_disk_rw(v->disk, v->first + sector, buf, len, write);
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
        for (int v = 0; v < nvolumes; v++)
            if (volumes[v].disk == i)
                print("  volume %d: %-8s %-9s sectors %lu-%lu (%lu MiB)\n", v, volumes[v].name,
                      volumes[v].label[0] ? volumes[v].label : "-", (unsigned long)volumes[v].first,
                      (unsigned long)(volumes[v].first + volumes[v].sectors - 1),
                      (unsigned long)(volumes[v].sectors >> 11));
    }
}
