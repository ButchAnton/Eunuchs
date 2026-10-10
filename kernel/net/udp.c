/*
 * UDP (RFC 768) and raw ICMP sockets.
 *
 * Received datagrams queue on their socket (at most UDP_QUEUE_MAX; beyond
 * that they are dropped and counted, "queue full" in /proc/net/stats).
 * Raw ICMP sockets get a copy of every ICMP message (from the ICMP header
 * on; the sender is in the address returned by recvfrom).
 */
#define LOG_SUBSYS SUBSYS_NET
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <net.h>
#include <printk.h>
#include <uapi/errno.h>

#define UDP_QUEUE_MAX 64

struct udp_hdr {
    u16 sport, dport, len, sum;
} __attribute__((packed));

LIST_HEAD(all_sockets);

/* Most specific match: connected (4-tuple) beats bound address beats wildcard. */
struct socket *udp_lookup(u32 laddr, u16 lport, u32 raddr, u16 rport)
{
    struct socket *s, *best = NULL;
    int best_score = -1;
    list_for_each_entry(s, &all_sockets, node) {
        if (s->type != SOCK_DGRAM || !s->bound || s->lport != lport)
            continue;
        if (s->laddr && s->laddr != laddr)
            continue;
        if (s->connected && (s->raddr != raddr || s->rport != rport))
            continue;
        int score = (s->connected ? 2 : 0) + (s->laddr ? 1 : 0);
        if (score > best_score) {
            best = s;
            best_score = score;
        }
    }
    return best;
}

static void enqueue(struct socket *s, struct netbuf *nb)
{
    if (s->rxq_len >= UDP_QUEUE_MAX || s->shut_rd) {
        net_stats.udp_queue_full++;
        nb_free(nb);
        return;
    }
    list_add_tail(&nb->node, &s->rxq);
    s->rxq_len++;
    sock_wake(s);
}

void udp_input(struct netbuf *nb)
{
    net_stats.udp_in++;
    if (nb->len < sizeof(struct udp_hdr)) {
        nb_free(nb);
        return;
    }
    const struct udp_hdr *h = (const struct udp_hdr *)nb->data;
    u32 len = ntoh16(h->len);
    if (len < sizeof(*h) || len > nb->len ||
        (h->sum && l4_cksum(nb->src_ip, nb->dst_ip, 17, nb->data, len) != 0)) {
        net_stats.udp_bad_cksum++;
        nb_free(nb);
        return;
    }
    nb->len = len;
    nb->src_port = ntoh16(h->sport);
    struct socket *s = udp_lookup(nb->dst_ip, ntoh16(h->dport), nb->src_ip, nb->src_port);
    if (!s) {
        net_stats.udp_no_port++;
        nb_free(nb);
        return;
    }
    nb_pull(nb, sizeof(*h));
    s->rx_bytes += nb->len;
    enqueue(s, nb);
}

int udp_output(struct socket *s, u32 dst, u16 dport, const void *data, u32 len)
{
    if (len > ETH_MTU - 20 - sizeof(struct udp_hdr))
        return -EMSGSIZE;
    u32 src = s->laddr ? s->laddr : ip_source_for(dst);
    if (!src)
        return -ENETUNREACH;
    struct netbuf *nb = nb_alloc();
    if (!nb)
        return -ENOBUFS;
    struct udp_hdr *h = nb_put(nb, sizeof(*h) + len);
    memcpy(h + 1, data, len);
    h->sport = hton16(s->lport);
    h->dport = hton16(dport);
    h->len = hton16((u16)(sizeof(*h) + len));
    h->sum = 0;
    u16 sum = l4_cksum(src, dst, 17, h, sizeof(*h) + len);
    h->sum = sum ? sum : 0xffff;                /* 0 means "no checksum" */
    net_stats.udp_out++;
    s->tx_bytes += len;
    int r = ip_output(src, dst, 17, nb);
    return r < 0 ? r : (int)len;
}

void raw_input(struct netbuf *nb)
{
    struct socket *s;
    list_for_each_entry(s, &all_sockets, node) {
        if (s->type != SOCK_RAW || s->proto != nb->proto)
            continue;
        if (s->connected && s->raddr != nb->src_ip)
            continue;
        struct netbuf *c = nb_clone(nb);
        if (c) {
            s->rx_bytes += c->len;
            enqueue(s, c);
        }
    }
    nb_free(nb);
}

void udp_print(print_fn print)
{
    struct socket *s;
    print("  ID TYPE LOCAL                 REMOTE                QUEUED  RX-BYTES  TX-BYTES  PID\n");
    list_for_each_entry(s, &all_sockets, node) {
        if (s->type == SOCK_STREAM)
            continue;
        char la[16], ra[16], l[24], r[24];
        ksnprintf(l, sizeof(l), "%s:%u", ip_str(s->laddr, la), s->lport);
        ksnprintf(r, sizeof(r), "%s:%u", s->connected ? ip_str(s->raddr, ra) : "*", s->rport);
        print("  %2lu %-4s %-21s %-21s %6d %9lu %9lu %4d\n", (unsigned long)s->id,
              s->type == SOCK_RAW ? "raw" : "udp", l, r, s->rxq_len,
              (unsigned long)s->rx_bytes, (unsigned long)s->tx_bytes, s->owner_pid);
    }
}
