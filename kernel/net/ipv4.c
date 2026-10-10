/*
 * IPv4 (RFC 791) and ICMP (RFC 792).
 *
 * Routing: 127/8 and our own addresses go to the loopback interface; the
 * eth0 subnet directly; everything else to eth0's gateway. No forwarding,
 * no options, no fragmentation: fragments are dropped and counted
 * ("fragments dropped" in /proc/net/stats), and we always send with DF set
 * and at most the MTU.
 *
 * ICMP: echo requests are answered here; every ICMP packet is also offered
 * to raw ICMP sockets (that is how ping sees its replies).
 */
#define LOG_SUBSYS SUBSYS_NET
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <net.h>
#include <printk.h>
#include <uapi/errno.h>

struct ip_hdr {
    u8 vhl, tos;
    u16 len, id, frag;
    u8 ttl, proto;
    u16 sum;
    u32 src, dst;
} __attribute__((packed));

struct icmp_hdr {
    u8 type, code;
    u16 sum;
    u16 id, seq;
} __attribute__((packed));

static u16 next_id;

bool ip_is_local(u32 a)
{
    if ((a >> 24) == 127)
        return true;
    for (int i = 0; i < nnetifs; i++)
        if (netifs[i]->addr == a && a)
            return true;
    return false;
}

struct netif *ip_route(u32 dst, u32 *next_hop)
{
    *next_hop = dst;
    if (ip_is_local(dst))
        return &loopback_if;
    for (int i = 0; i < nnetifs; i++) {
        struct netif *d = netifs[i];
        if (d == &loopback_if || !d->up || !d->addr)
            continue;
        if (dst == 0xffffffff || (dst & d->netmask) == (d->addr & d->netmask))
            return d;
    }
    for (int i = 0; i < nnetifs; i++) {
        struct netif *d = netifs[i];
        if (d != &loopback_if && d->up && d->gateway) {
            *next_hop = d->gateway;
            return d;
        }
    }
    return NULL;
}

/* The address we send from to reach dst (0 if unroutable). */
u32 ip_source_for(u32 dst)
{
    u32 hop;
    struct netif *d = ip_route(dst, &hop);
    if (!d)
        return 0;
    if (d == &loopback_if)
        return (dst >> 24) == 127 ? 0x7f000001 : dst;   /* talking to ourselves */
    return d->addr;
}

int ip_output(u32 src, u32 dst, u8 proto, struct netbuf *nb)
{
    u32 hop;
    struct netif *dev = ip_route(dst, &hop);
    if (!dev) {
        net_stats.ip_no_route++;
        nb_free(nb);
        return -ENETUNREACH;
    }
    if (!src)
        src = ip_source_for(dst);
    if (nb->len + sizeof(struct ip_hdr) > dev->mtu) {
        nb_free(nb);
        return -EMSGSIZE;
    }
    struct ip_hdr *h = nb_push(nb, sizeof(*h));
    h->vhl = 0x45;
    h->tos = 0;
    h->len = hton16((u16)nb->len);
    h->id = hton16(next_id++);
    h->frag = hton16(0x4000);                   /* don't fragment */
    h->ttl = 64;
    h->proto = proto;
    h->sum = 0;
    h->src = hton32(src);
    h->dst = hton32(dst);
    h->sum = inet_cksum(h, sizeof(*h));
    net_stats.ip_out++;
    if (dev == &loopback_if) {
        net_pkt_log("tx", dev, nb);
        dev->stats.tx_packets++;
        dev->stats.tx_bytes += nb->len;
        return dev->xmit(dev, nb);
    }
    return eth_output(dev, nb, hop, ETH_P_IP);
}

void ip_input(struct netif *dev, struct netbuf *nb)
{
    net_stats.ip_in++;
    const struct ip_hdr *h = (const struct ip_hdr *)nb->data;
    u32 hl = (h->vhl & 15) * 4;
    if (nb->len < sizeof(*h) || (h->vhl >> 4) != 4 || hl < sizeof(*h) || hl > nb->len ||
        ntoh16(h->len) < hl || ntoh16(h->len) > nb->len || inet_cksum(h, hl) != 0) {
        net_stats.ip_bad++;
        dev->stats.rx_errors++;
        nb_free(nb);
        return;
    }
    if (ntoh16(h->frag) & 0x3fff) {             /* MF set or an offset: a fragment */
        net_stats.ip_frags++;
        nb_free(nb);
        return;
    }
    u32 dst = ntoh32(h->dst);
    if (!ip_is_local(dst) && dst != 0xffffffff &&
        dst != ((dev->addr & dev->netmask) | ~dev->netmask)) {
        net_stats.ip_not_ours++;
        nb_free(nb);
        return;
    }
    nb->src_ip = ntoh32(h->src);
    nb->dst_ip = dst;
    nb->proto = h->proto;
    nb->len = ntoh16(h->len);                   /* drop Ethernet padding */
    nb_pull(nb, hl);
    switch (nb->proto) {
    case 1: {
        struct netbuf *copy = nb_clone(nb);
        if (copy)
            raw_input(copy);
        icmp_input(nb);
        break;
    }
    case 6:
        tcp_input(nb);
        break;
    case 17:
        udp_input(nb);
        break;
    default:
        nb_free(nb);
        break;
    }
}

void icmp_input(struct netbuf *nb)
{
    net_stats.icmp_in++;
    if (nb->len < sizeof(struct icmp_hdr) || inet_cksum(nb->data, nb->len) != 0) {
        nb_free(nb);
        return;
    }
    struct icmp_hdr *ic = (struct icmp_hdr *)nb->data;
    if (ic->type == 8 && ic->code == 0) {       /* echo request: answer in place */
        ic->type = 0;
        ic->sum = 0;
        ic->sum = inet_cksum(nb->data, nb->len);
        net_stats.icmp_out++;
        net_stats.icmp_echo_replied++;
        u32 from = nb->dst_ip == 0xffffffff ? 0 : nb->dst_ip;
        ip_output(from, nb->src_ip, 1, nb);
        return;
    }
    nb_free(nb);
}
