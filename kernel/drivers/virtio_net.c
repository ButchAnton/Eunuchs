/*
 * virtio network device (virtio 1.2 section 5.1), as eth0.
 *
 * Queue 0 receives: RX_BUFFERS packet buffers stay posted; the interrupt
 * handler hands each filled one to the stack (netif_rx) and posts a fresh
 * one. Queue 1 transmits: each frame goes out as one descriptor (the
 * 12-byte virtio header is pushed in front of it); finished frames are
 * freed in the interrupt handler. No offloads: we checksum everything.
 *
 * With QEMU's user-mode network (make run), the host is 10.0.2.2, DNS
 * 10.0.2.3, and we are 10.0.2.15/24; QEMU forwards host ports to us
 * (Makefile HOSTFWD) and writes all traffic to build/net.pcap.
 */
#define LOG_SUBSYS SUBSYS_DRV
#include <debug.h>
#include <irq.h>
#include <kstring.h>
#include <mm.h>
#include <net.h>
#include <printk.h>
#include <virtio.h>

#define VIRTIO_NET_F_MAC    (1ULL << 5)
#define VNET_HDR_LEN        12          /* struct virtio_net_hdr with num_buffers */
#define RX_BUFFERS          64

static struct virtio_dev dev;
static struct virtqueue rxq, txq;
static struct netif eth0 = { .name = "eth0", .has_mac = true, .up = true, .mtu = ETH_MTU };
static u64 rx_refill_failures, tx_queue_full;

static void post_rx(void)
{
    struct netbuf *nb = nb_alloc();
    if (!nb) {
        rx_refill_failures++;
        return;
    }
    nb->data = nb->head + NB_HEADROOM - VNET_HDR_LEN;
    struct virtio_buf b = { dma_addr(nb->data), NB_SIZE - (NB_HEADROOM - VNET_HDR_LEN), true };
    if (virtq_add(&rxq, &b, 1, nb)) {
        nb_free(nb);
        rx_refill_failures++;
    }
}

static void net_irq(void *arg)
{
    (void)arg;
    virtio_ack_irq(&dev);
    struct netbuf *nb;
    u32 len;
    int received = 0;
    while ((nb = virtq_get(&rxq, &len)) != NULL) {
        nb->len = len;
        nb_pull(nb, VNET_HDR_LEN);
        netif_rx(&eth0, nb);
        received++;
    }
    for (int i = 0; i < received; i++)
        post_rx();
    if (received)
        virtq_kick(&dev, &rxq);
    while ((nb = virtq_get(&txq, NULL)) != NULL)
        nb_free(nb);
}

static int net_xmit(struct netif *d, struct netbuf *nb)
{
    (void)d;
    u8 *h = nb_push(nb, VNET_HDR_LEN);
    memset(h, 0, VNET_HDR_LEN);
    struct virtio_buf b = { dma_addr(nb->data), nb->len, false };
    if (virtq_add(&txq, &b, 1, nb)) {
        /* Ring full: reclaim finished frames and try once more. */
        struct netbuf *done;
        while ((done = virtq_get(&txq, NULL)) != NULL)
            nb_free(done);
        if (virtq_add(&txq, &b, 1, nb)) {
            tx_queue_full++;
            nb_free(nb);
            return -1;
        }
    }
    virtq_kick(&dev, &txq);
    return 0;
}

int virtio_net_init(void)
{
    if (virtio_find(VIRTIO_DEV_NET, VIRTIO_NET_F_MAC, &dev)) {
        pr_info("no virtio network device (make run attaches one)\n");
        return -1;
    }
    if (virtq_init(&dev, &rxq, 0, RX_BUFFERS) || virtq_init(&dev, &txq, 1, 64)) {
        pr_err("virtio-net: cannot set up the queues\n");
        return -1;
    }
    if (dev.features & VIRTIO_NET_F_MAC) {
        u32 lo = virtio_config_read32(&dev, 0), hi = virtio_config_read32(&dev, 4);
        for (int i = 0; i < 4; i++)
            eth0.mac[i] = (u8)(lo >> (8 * i));
        eth0.mac[4] = (u8)hi;
        eth0.mac[5] = (u8)(hi >> 8);
    } else {
        static const u8 fallback[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        memcpy(eth0.mac, fallback, ETH_ALEN);
    }
    /* QEMU user networking defaults; boot option ip= overrides (net.c). */
    eth0.addr = 0x0a00020f;                     /* 10.0.2.15 */
    eth0.netmask = 0xffffff00;
    eth0.gateway = 0x0a000202;                  /* 10.0.2.2 */
    eth0.xmit = net_xmit;
    for (int i = 0; i < RX_BUFFERS; i++)
        post_rx();
    irq_register(dev.irq, "virtio-net", net_irq, NULL);
    virtio_ready(&dev);
    virtq_kick(&dev, &rxq);
    netif_register(&eth0);
    pr_info("eth0: virtio-net, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", eth0.mac[0], eth0.mac[1],
            eth0.mac[2], eth0.mac[3], eth0.mac[4], eth0.mac[5]);
    return 0;
}
