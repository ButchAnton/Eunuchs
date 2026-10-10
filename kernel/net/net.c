/*
 * Network core: the lock, interfaces, the loopback interface, the netd
 * thread (received packets and protocol timers), statistics, packet
 * logging, and the hooks tcp.c expects. See net.h for the big picture.
 */
#define LOG_SUBSYS SUBSYS_NET
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <net.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <trace.h>
#include <uapi/errno.h>

struct mutex net_lock = MUTEX_INIT(net_lock, "net");
struct netif *netifs[MAX_NETIFS];
int nnetifs;
int net_drop_percent;
struct net_stats net_stats;
extern u64 nb_live;

/* Received packets waiting for netd (filled from interrupt context). */
static spinlock_t rxq_lock = SPINLOCK_INIT("net-rxq");
static LIST_HEAD(rxq);
static int rxq_len;
#define RXQ_MAX 512
static struct waitqueue netd_wq = WAITQUEUE_INIT(netd_wq, "netd");
static struct thread *netd_thread;

/* ---- hooks for the protocols ---- */

u64 net_now_ms(void)
{
    return rdtime() * 1000 / platform.timebase;
}

u32 net_random(void)
{
    static u64 state;
    if (!state)
        state = rdtime() | 1;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (u32)state;
}

void net_log(int level, const char *fmt, ...)
{
    if (level > log_threshold[SUBSYS_NET])
        return;
    va_list ap;
    va_start(ap, fmt);
    vklog(level, SUBSYS_NET, fmt, ap);
    va_end(ap);
}

void tcp_wake_owner(void *owner)
{
    sock_wake(owner);
}

void tcp_trace_state(const struct tcb *t, int from, int to)
{
    trace(tcp_state, (uintptr_t)t, from, to);
}

void tcp_trace_rexmit(const struct tcb *t, u32 seq, u32 len)
{
    trace(tcp_rexmit, (uintptr_t)t, seq, len);
}

const char *ip_str(u32 ip, char *buf)
{
    ksnprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

/* ---- interfaces ---- */

static int lo_xmit(struct netif *dev, struct netbuf *nb)
{
    netif_rx(dev, nb);              /* straight back in, through netd */
    return 0;
}

struct netif loopback_if = {
    .name = "lo", .up = true, .addr = 0x7f000001, .netmask = 0xff000000, .mtu = 1500,
    .xmit = lo_xmit,
};

void netif_register(struct netif *dev)
{
    if (nnetifs < MAX_NETIFS)
        netifs[nnetifs++] = dev;
}

struct netif *netif_by_name(const char *name)
{
    for (int i = 0; i < nnetifs; i++)
        if (!strcmp(netifs[i]->name, name))
            return netifs[i];
    return NULL;
}

/* A packet arrived (driver interrupt, or loopback). Any context. */
void netif_rx(struct netif *dev, struct netbuf *nb)
{
    if (net_drop_percent && (int)(net_random() % 100) < net_drop_percent) {
        net_stats.injected_drops++;
        dev->stats.rx_dropped++;
        trace(net_drop, (uintptr_t)"injected loss", nb->len, 0);
        nb_free(nb);
        return;
    }
    nb->dev = dev;
    unsigned long flags = spin_lock_irqsave(&rxq_lock);
    if (rxq_len >= RXQ_MAX) {
        spin_unlock_irqrestore(&rxq_lock, flags);
        dev->stats.rx_dropped++;
        trace(net_drop, (uintptr_t)"receive queue full", nb->len, 0);
        nb_free(nb);
        return;
    }
    list_add_tail(&nb->node, &rxq);
    rxq_len++;
    spin_unlock_irqrestore(&rxq_lock, flags);
    wake_up(&netd_wq);
}

static struct netbuf *rxq_pop(void)
{
    struct netbuf *nb = NULL;
    unsigned long flags = spin_lock_irqsave(&rxq_lock);
    if (!list_empty(&rxq)) {
        nb = list_first_entry(&rxq, struct netbuf, node);
        list_del(&nb->node);
        rxq_len--;
    }
    spin_unlock_irqrestore(&rxq_lock, flags);
    return nb;
}

/* Something armed a timer (or queued work): make netd look. */
void net_kick(void)
{
    wake_up(&netd_wq);
}

/*
 * netd: process received packets and run the protocol timers. Sleeps until
 * a packet arrives, or for 10 ms while any timer is pending.
 */
static void netd(void *arg)
{
    (void)arg;
    for (;;) {
        mutex_lock(&net_lock);
        bool timers = tcp_timers_pending() || arp_timers_pending();
        mutex_unlock(&net_lock);

        struct wait_entry we = { .t = current, .queued = false };
        prepare_to_wait(&netd_wq, &we);
        if (list_empty(&rxq))
            schedule_until(timers ? rdtime() + ms_to_rdtime(10) : 0);
        finish_wait(&netd_wq, &we);

        mutex_lock(&net_lock);
        struct netbuf *nb;
        int budget = 64;
        while (budget-- && (nb = rxq_pop()) != NULL) {
            struct netif *dev = nb->dev;
            dev->stats.rx_packets++;
            dev->stats.rx_bytes += nb->len;
            net_pkt_log("rx", dev, nb);
            if (dev->has_mac)
                eth_input(dev, nb);
            else
                ip_input(dev, nb);
        }
        tcp_timer();
        arp_timer();
        mutex_unlock(&net_lock);
    }
}

static void parse_ip_option(void)
{
    /* ip=<a.b.c.d>/<bits>[,<gateway>] for eth0 */
    const char *opt = cmdline_get("ip");
    struct netif *eth = netif_by_name("eth0");
    if (!opt || !*opt || !eth)
        return;
    u32 parts[4] = { 0 }, gw[4] = { 0 }, bits = 24;
    const char *p = opt;
    for (int i = 0; i < 4; i++) {
        parts[i] = (u32)kstrtoul(p, &p, 10);
        if (*p == '.')
            p++;
    }
    if (*p == '/')
        bits = (u32)kstrtoul(p + 1, &p, 10);
    if (*p == ',') {
        p++;
        for (int i = 0; i < 4; i++) {
            gw[i] = (u32)kstrtoul(p, &p, 10);
            if (*p == '.')
                p++;
        }
    }
    eth->addr = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
    eth->netmask = bits ? ~0u << (32 - bits) : 0;
    eth->gateway = gw[0] << 24 | gw[1] << 16 | gw[2] << 8 | gw[3];
}

void net_init(void)
{
    netif_register(&loopback_if);
    virtio_net_init();
    parse_ip_option();
    const char *drop = cmdline_get("net.drop");
    if (drop && *drop) {
        net_drop_percent = (int)kstrtoul(drop, NULL, 10);
        pr_warn("net.drop=%d: dropping %d%% of received packets on purpose\n",
                net_drop_percent, net_drop_percent);
    }
    netd_thread = kthread_create("netd", netd, NULL);
    if (!netd_thread)
        panic("cannot start netd");
    kthread_detach(netd_thread);
    for (int i = 0; i < nnetifs; i++) {
        struct netif *d = netifs[i];
        char a[16], m[16], g[16];
        pr_info("%s: %s/%s%s%s%s\n", d->name, ip_str(d->addr, a), ip_str(d->netmask, m),
                d->gateway ? " gateway " : "", d->gateway ? ip_str(d->gateway, g) : "",
                d->up ? "" : " (down)");
    }
}

/* ---- packet log (log=net:debug): one tcpdump-like line per packet ---- */

static void fmt_tcp_flags(char *b, u8 f)
{
    int n = 0;
    b[n++] = '[';
    if (f & 0x02) b[n++] = 'S';
    if (f & 0x01) b[n++] = 'F';
    if (f & 0x04) b[n++] = 'R';
    if (f & 0x08) b[n++] = 'P';
    if (f & 0x10) b[n++] = '.';
    b[n++] = ']';
    b[n] = '\0';
}

void net_pkt_log(const char *dir, struct netif *dev, const struct netbuf *nb)
{
    const u8 *p = nb->data;
    u32 len = nb->len;
    bool tx = dir[0] == 't';
    if (len >= (dev->has_mac ? ETH_HLEN : 20)) {
        u16 type = ETH_P_IP;
        if (dev->has_mac) {
            type = (u16)(p[12] << 8 | p[13]);
            p += ETH_HLEN;
            len -= ETH_HLEN;
        }
        u32 peer = 0, proto = ETH_P_ARP;
        if (type == ETH_P_IP && len >= 20) {
            proto = p[9];
            peer = (u32)(p[tx ? 16 : 12] << 24 | p[tx ? 17 : 13] << 16 |
                         p[tx ? 18 : 14] << 8 | p[tx ? 19 : 15]);
        }
        if (tx)
            trace(net_tx, proto, len, peer);
        else
            trace(net_rx, proto, len, peer);
        p = nb->data;
        len = nb->len;
    }
    if (LOG_DEBUG > log_threshold[SUBSYS_NET])
        return;
    char line[160];
    int n = ksnprintf(line, sizeof(line), "%s %s ", dev->name, dir);
    u16 type = ETH_P_IP;
    if (dev->has_mac) {
        if (len < ETH_HLEN)
            goto short_pkt;
        type = (u16)(p[12] << 8 | p[13]);
        p += ETH_HLEN;
        len -= ETH_HLEN;
    }
    if (type == ETH_P_ARP && len >= 28) {
        char s[16], t[16];
        u32 spa = (u32)(p[14] << 24 | p[15] << 16 | p[16] << 8 | p[17]);
        u32 tpa = (u32)(p[24] << 24 | p[25] << 16 | p[26] << 8 | p[27]);
        if (p[7] == 1)
            ksnprintf(line + n, sizeof(line) - n, "ARP who-has %s tell %s", ip_str(tpa, t),
                      ip_str(spa, s));
        else
            ksnprintf(line + n, sizeof(line) - n,
                      "ARP reply %s is-at %02x:%02x:%02x:%02x:%02x:%02x", ip_str(spa, s),
                      p[8], p[9], p[10], p[11], p[12], p[13]);
    } else if (type == ETH_P_IP && len >= 20) {
        u32 hl = (p[0] & 15) * 4;
        u32 src = (u32)(p[12] << 24 | p[13] << 16 | p[14] << 8 | p[15]);
        u32 dst = (u32)(p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19]);
        u32 tot = (u32)(p[2] << 8 | p[3]);
        const u8 *l4 = p + hl;
        u32 l4len = tot > hl ? tot - hl : 0;
        char s[16], d[16];
        ip_str(src, s);
        ip_str(dst, d);
        if (p[9] == 6 && l4len >= 20 && hl + 20 <= len) {
            char f[10];
            fmt_tcp_flags(f, l4[13]);
            u32 thl = (u32)(l4[12] >> 4) * 4;
            ksnprintf(line + n, sizeof(line) - n,
                      "%s:%u > %s:%u TCP %s seq %u ack %u win %u len %u", s,
                      l4[0] << 8 | l4[1], d, l4[2] << 8 | l4[3], f,
                      (u32)(l4[4] << 24 | l4[5] << 16 | l4[6] << 8 | l4[7]),
                      (u32)(l4[8] << 24 | l4[9] << 16 | l4[10] << 8 | l4[11]),
                      l4[14] << 8 | l4[15], l4len > thl ? l4len - thl : 0);
        } else if (p[9] == 17 && l4len >= 8 && hl + 8 <= len) {
            ksnprintf(line + n, sizeof(line) - n, "%s:%u > %s:%u UDP len %u", s,
                      l4[0] << 8 | l4[1], d, l4[2] << 8 | l4[3], l4len - 8);
        } else if (p[9] == 1 && l4len >= 8 && hl + 8 <= len) {
            ksnprintf(line + n, sizeof(line) - n, "%s > %s ICMP %s id %u seq %u", s, d,
                      l4[0] == 8 ? "echo request" : l4[0] == 0 ? "echo reply" :
                      l4[0] == 3 ? "unreachable" : "type ?",
                      l4[4] << 8 | l4[5], l4[6] << 8 | l4[7]);
        } else {
            ksnprintf(line + n, sizeof(line) - n, "%s > %s proto %u len %u", s, d, p[9], tot);
        }
    } else {
short_pkt:
        ksnprintf(line + n, sizeof(line) - n, "type %#x len %u", type, len);
    }
    pr_debug("%s\n", line);
}

/* ---- debugging ---- */

void net_print_dev(print_fn print)
{
    print("  IFACE  ADDRESS          NETMASK          GATEWAY          MAC                "
          "RX-PKTS   RX-BYTES  RX-DROP   TX-PKTS   TX-BYTES  TX-DROP\n");
    for (int i = 0; i < nnetifs; i++) {
        struct netif *d = netifs[i];
        char a[16], m[16], g[16], mac[20] = "-";
        if (d->has_mac)
            ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", d->mac[0], d->mac[1],
                      d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        print("  %-6s %-16s %-16s %-16s %-17s %9lu %10lu %8lu %9lu %10lu %8lu%s\n", d->name,
              ip_str(d->addr, a), ip_str(d->netmask, m), ip_str(d->gateway, g), mac,
              (unsigned long)d->stats.rx_packets, (unsigned long)d->stats.rx_bytes,
              (unsigned long)d->stats.rx_dropped, (unsigned long)d->stats.tx_packets,
              (unsigned long)d->stats.tx_bytes, (unsigned long)d->stats.tx_dropped,
              d->up ? "" : "  (down)");
    }
}

void net_print_stats(print_fn print)
{
    const struct net_stats *s = &net_stats;
    print("ip: %lu in, %lu out, %lu malformed, %lu fragments dropped, %lu not for us, "
          "%lu no route\n", (unsigned long)s->ip_in, (unsigned long)s->ip_out,
          (unsigned long)s->ip_bad, (unsigned long)s->ip_frags, (unsigned long)s->ip_not_ours,
          (unsigned long)s->ip_no_route);
    print("icmp: %lu in, %lu out, %lu echo requests answered\n", (unsigned long)s->icmp_in,
          (unsigned long)s->icmp_out, (unsigned long)s->icmp_echo_replied);
    print("udp: %lu in, %lu out, %lu to closed ports, %lu bad checksums, %lu queue full\n",
          (unsigned long)s->udp_in, (unsigned long)s->udp_out, (unsigned long)s->udp_no_port,
          (unsigned long)s->udp_bad_cksum, (unsigned long)s->udp_queue_full);
    tcp_print_stats(print);
    print("arp: %lu requests in, %lu replies in, %lu requests out, %lu unresolved\n",
          (unsigned long)s->arp_requests_in, (unsigned long)s->arp_replies_in,
          (unsigned long)s->arp_requests_out, (unsigned long)s->arp_unresolved);
    print("netbufs: %lu live; receive queue %d; %lu packets dropped on purpose (net.drop=%d)\n",
          (unsigned long)nb_live, rxq_len, (unsigned long)s->injected_drops, net_drop_percent);
}
