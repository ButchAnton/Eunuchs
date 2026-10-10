/*
 * Ethernet framing and ARP (RFC 826).
 *
 * The ARP table maps IPv4 next hops to MAC addresses. A packet for an
 * unresolved address waits on its entry (a few per entry) while requests go
 * out once a second; after three unanswered requests the waiting packets are
 * dropped (counted as "unresolved" in /proc/net/stats). Entries expire after
 * ARP_TTL_MS and are refreshed by any ARP packet from the host.
 *
 * Debugging: /proc/net/arp and `net` in the monitor show the table;
 * log=net:debug logs every ARP packet.
 */
#define LOG_SUBSYS SUBSYS_NET
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <net.h>
#include <printk.h>
#include <uapi/errno.h>

#define ARP_ENTRIES     32
#define ARP_MAX_PENDING 8
#define ARP_RETRY_MS    1000
#define ARP_TRIES       3
#define ARP_TTL_MS      (10 * 60 * 1000)

struct eth_hdr {
    u8 dst[ETH_ALEN], src[ETH_ALEN];
    u16 type;
} __attribute__((packed));

struct arp_pkt {
    u16 htype, ptype;
    u8 hlen, plen;
    u16 op;
    u8 sha[ETH_ALEN];
    u32 spa;
    u8 tha[ETH_ALEN];
    u32 tpa;
} __attribute__((packed));

enum { ARP_FREE, ARP_PENDING, ARP_RESOLVED };

struct arp_entry {
    u32 ip;
    u8 mac[ETH_ALEN];
    int state;
    struct netif *dev;
    u64 updated, next_try;
    int tries;
    struct list_head pending;       /* netbufs waiting for the answer */
    int npending;
};

static struct arp_entry arp_table[ARP_ENTRIES];
static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void xmit(struct netif *dev, struct netbuf *nb, const u8 *dst, u16 type)
{
    struct eth_hdr *e = nb_push(nb, ETH_HLEN);
    memcpy(e->dst, dst, ETH_ALEN);
    memcpy(e->src, dev->mac, ETH_ALEN);
    e->type = hton16(type);
    net_pkt_log("tx", dev, nb);
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += nb->len;
    if (dev->xmit(dev, nb))
        dev->stats.tx_dropped++;
}

static void arp_send(struct netif *dev, u16 op, const u8 *dst_mac, u32 tpa)
{
    struct netbuf *nb = nb_alloc();
    if (!nb)
        return;
    struct arp_pkt *a = nb_put(nb, sizeof(*a));
    a->htype = hton16(1);
    a->ptype = hton16(ETH_P_IP);
    a->hlen = ETH_ALEN;
    a->plen = 4;
    a->op = hton16(op);
    memcpy(a->sha, dev->mac, ETH_ALEN);
    a->spa = hton32(dev->addr);
    memcpy(a->tha, op == 2 ? dst_mac : (const u8 *)"\0\0\0\0\0\0", ETH_ALEN);
    a->tpa = hton32(tpa);
    if (op == 1)
        net_stats.arp_requests_out++;
    xmit(dev, nb, op == 2 ? dst_mac : bcast, ETH_P_ARP);
}

static struct arp_entry *arp_find(u32 ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].state != ARP_FREE && arp_table[i].ip == ip)
            return &arp_table[i];
    return NULL;
}

static void drop_pending(struct arp_entry *e)
{
    while (!list_empty(&e->pending)) {
        struct netbuf *nb = list_first_entry(&e->pending, struct netbuf, node);
        list_del(&nb->node);
        nb_free(nb);
        net_stats.arp_unresolved++;
    }
    e->npending = 0;
}

static struct arp_entry *arp_new(u32 ip, struct netif *dev)
{
    struct arp_entry *victim = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (e->state == ARP_FREE) {
            victim = e;
            break;
        }
        if (e->state == ARP_RESOLVED && (!victim || e->updated < victim->updated))
            victim = e;                         /* evict the oldest resolved one */
    }
    if (!victim)
        return NULL;
    if (victim->state != ARP_FREE)
        drop_pending(victim);
    memset(victim, 0, sizeof(*victim));
    list_init(&victim->pending);
    victim->ip = ip;
    victim->dev = dev;
    return victim;
}

static void arp_resolved(struct arp_entry *e, const u8 *mac)
{
    memcpy(e->mac, mac, ETH_ALEN);
    e->state = ARP_RESOLVED;
    e->updated = net_now_ms();
    while (!list_empty(&e->pending)) {
        struct netbuf *nb = list_first_entry(&e->pending, struct netbuf, node);
        list_del(&nb->node);
        xmit(e->dev, nb, e->mac, ETH_P_IP);     /* only IP waits for ARP */
    }
    e->npending = 0;
}

int eth_output(struct netif *dev, struct netbuf *nb, u32 next_hop, u16 type)
{
    u32 bcast_ip = (dev->addr & dev->netmask) | ~dev->netmask;
    if (next_hop == 0xffffffff || next_hop == bcast_ip) {
        xmit(dev, nb, bcast, type);
        return 0;
    }
    struct arp_entry *e = arp_find(next_hop);
    u64 now = net_now_ms();
    if (e && e->state == ARP_RESOLVED && now - e->updated < ARP_TTL_MS) {
        xmit(dev, nb, e->mac, type);
        return 0;
    }
    if (!e || e->state == ARP_RESOLVED) {       /* unknown or stale: ask */
        if (!e)
            e = arp_new(next_hop, dev);
        if (!e) {
            nb_free(nb);
            return -ENOBUFS;
        }
        list_init(&e->pending);
        e->state = ARP_PENDING;
        e->tries = 1;
        e->next_try = now + ARP_RETRY_MS;
        arp_send(dev, 1, NULL, next_hop);
    }
    if (e->npending == ARP_MAX_PENDING) {       /* drop the oldest waiting packet */
        struct netbuf *old = list_first_entry(&e->pending, struct netbuf, node);
        list_del(&old->node);
        nb_free(old);
        e->npending--;
        net_stats.arp_unresolved++;
    }
    list_add_tail(&nb->node, &e->pending);
    e->npending++;
    return 0;
}

static void arp_input(struct netif *dev, struct netbuf *nb)
{
    if (nb->len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)nb->data;
    if (ntoh16(a->htype) != 1 || ntoh16(a->ptype) != ETH_P_IP || a->hlen != ETH_ALEN ||
        a->plen != 4)
        return;
    u32 spa = ntoh32(a->spa), tpa = ntoh32(a->tpa);
    u16 op = ntoh16(a->op);
    if (op == 1)
        net_stats.arp_requests_in++;
    else
        net_stats.arp_replies_in++;
    bool for_us = dev->addr && tpa == dev->addr;
    struct arp_entry *e = arp_find(spa);
    if (e)
        arp_resolved(e, a->sha);                /* refresh what we know */
    else if (for_us && (e = arp_new(spa, dev)) != NULL)
        arp_resolved(e, a->sha);
    if (op == 1 && for_us)
        arp_send(dev, 2, a->sha, spa);
}

void eth_input(struct netif *dev, struct netbuf *nb)
{
    if (nb->len < ETH_HLEN) {
        dev->stats.rx_errors++;
        nb_free(nb);
        return;
    }
    const struct eth_hdr *e = (const struct eth_hdr *)nb->data;
    if (memcmp(e->dst, dev->mac, ETH_ALEN) && memcmp(e->dst, bcast, ETH_ALEN)) {
        nb_free(nb);                            /* not for us */
        return;
    }
    u16 type = ntoh16(e->type);
    nb_pull(nb, ETH_HLEN);
    if (type == ETH_P_ARP) {
        arp_input(dev, nb);
        nb_free(nb);
    } else if (type == ETH_P_IP) {
        ip_input(dev, nb);
    } else {
        nb_free(nb);
    }
}

void arp_timer(void)
{
    u64 now = net_now_ms();
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (e->state != ARP_PENDING || now < e->next_try)
            continue;
        if (e->tries >= ARP_TRIES) {
            char s[16];
            pr_debug("arp: no answer for %s; dropping %d waiting packets\n", ip_str(e->ip, s),
                     e->npending);
            drop_pending(e);
            e->state = ARP_FREE;
            continue;
        }
        e->tries++;
        e->next_try = now + ARP_RETRY_MS;
        arp_send(e->dev, 1, NULL, e->ip);
    }
}

bool arp_timers_pending(void)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].state == ARP_PENDING)
            return true;
    return false;
}

void arp_print(print_fn print)
{
    u64 now = net_now_ms();
    print("  ADDRESS          MAC                IFACE  STATE     AGE(s) WAITING\n");
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (e->state == ARP_FREE)
            continue;
        char s[16];
        print("  %-16s %02x:%02x:%02x:%02x:%02x:%02x  %-6s %-9s %6lu %d\n", ip_str(e->ip, s),
              e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
              e->dev ? e->dev->name : "?", e->state == ARP_PENDING ? "resolving" : "ok",
              (unsigned long)((now - e->updated) / 1000), e->npending);
    }
}
