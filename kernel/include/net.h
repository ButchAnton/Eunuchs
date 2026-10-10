#ifndef EUNUCHS_NET_H
#define EUNUCHS_NET_H

/*
 * The network stack (kernel/net/): packet buffers, interfaces (eth0 on
 * virtio-net, lo), Ethernet + ARP, IPv4 + ICMP, UDP, TCP, and sockets.
 *
 * Execution: received frames are queued by the driver's interrupt handler
 * (or by the loopback interface) and processed by the `netd` kernel thread,
 * which also runs the protocol timers. System calls run the transmit side
 * in the caller's thread.
 *
 * Locking: all protocol state (interfaces, ARP table, sockets, TCP control
 * blocks) is protected by one mutex, net_lock. Sleeping socket calls drop
 * it while they wait. The receive queue has its own spinlock (the driver
 * fills it from interrupt context).
 *
 * Addresses and ports are kept in host byte order everywhere inside the
 * stack and converted only when headers are built or parsed.
 *
 * This header is also compiled on the host by tests/host/test_tcp.c, which
 * runs tcp.c against a simulated lossy link; keep the parts tcp.c needs free
 * of other kernel headers.
 *
 * Debugging: /proc/net/{dev,arp,tcp,udp,stats}; monitor `net`, `netstat`;
 * boot options log=net:debug (one tcpdump-style line per packet),
 * net.drop=<percent> (random loss), ip=<addr>/<bits>,<gateway>;
 * trace events net_rx/net_tx/tcp_state/tcp_rexmit; every `make run`
 * writes the raw traffic to build/net.pcap (open it in Wireshark).
 */

#include <list.h>
#include <types.h>

/* ---- byte order (the CPU is little endian, the wire big endian) ---- */

static inline u16 ntoh16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static inline u32 ntoh32(u32 x)
{
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}
#define hton16 ntoh16
#define hton32 ntoh32

/* ---- packet buffers ---- */

#define NB_SIZE      2048
#define NB_HEADROOM  128        /* room for virtio + Ethernet + IP + TCP headers */

struct netif;

struct netbuf {
    struct list_head node;
    u8 *head;                   /* the allocation (NB_SIZE bytes) */
    u8 *data;                   /* first byte of the packet */
    u32 len;                    /* bytes from data */
    struct netif *dev;          /* receive: interface it arrived on */
    u32 src_ip, dst_ip;         /* receive: from the IP header */
    u16 src_port;               /* receive (UDP): sender's port */
    u8 proto;
};

struct netbuf *nb_alloc(void);          /* data at head + NB_HEADROOM, len 0 */
void nb_free(struct netbuf *nb);
void *nb_put(struct netbuf *nb, u32 n);  /* append n bytes, returns their start */
void *nb_push(struct netbuf *nb, u32 n); /* prepend a header */
void nb_pull(struct netbuf *nb, u32 n);  /* strip a header */
struct netbuf *nb_clone(const struct netbuf *nb);

/* ---- checksums (cksum.c) ---- */

u32 cksum_add(u32 sum, const void *data, u32 len);     /* 16-bit one's complement sum */
u16 cksum_finish(u32 sum);
u16 inet_cksum(const void *data, u32 len);
/* TCP/UDP checksum over the pseudo-header plus `len` bytes of segment. */
u16 l4_cksum(u32 src, u32 dst, u8 proto, const void *seg, u32 len);

/* ---- hooks the protocols call (net.c in the kernel, stubs in host tests) ---- */

int ip_output(u32 src, u32 dst, u8 proto, struct netbuf *nb);  /* consumes nb */
u64 net_now_ms(void);
u32 net_random(void);
void net_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(const void *p);

/* ---- TCP (tcp.c) ---- */

enum tcp_state {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSING, TCP_TIME_WAIT,
    TCP_CLOSE_WAIT, TCP_LAST_ACK,
};

#define TCP_POLL_IN     1       /* data, EOF or a connection to accept */
#define TCP_POLL_OUT    2       /* room in the send buffer */
#define TCP_POLL_ERR    4
#define TCP_POLL_HUP    8

#define TCP_BUF_SIZE    (32 * 1024)     /* per-connection send and receive buffers */
#define TCP_MSS_DEFAULT 536
#define TCP_MSS_LOCAL   1460

struct tcb;

struct tcp_stats {
    u64 active_opens, passive_opens, accepts, resets_sent, resets_rcvd;
    u64 segs_in, segs_out, bytes_in, bytes_out;
    u64 retransmits, fast_retransmits, timeouts, dup_acks, out_of_order, bad_cksum;
    u64 no_listener, backlog_full;
};
extern struct tcp_stats tcp_stats;

/* `owner` is passed back to tcp_wake_owner() (the socket); NULL = orphan. */
struct tcb *tcp_connect(u32 laddr, u16 lport, u32 raddr, u16 rport, void *owner, int *err);
struct tcb *tcp_listen(u32 laddr, u16 lport, int backlog, void *owner);
struct tcb *tcp_accept(struct tcb *listener);   /* an established child, or NULL */
void tcp_set_owner(struct tcb *t, void *owner);
int tcp_send(struct tcb *t, const void *buf, u32 n);   /* bytes taken, or -errno */
int tcp_recv(struct tcb *t, void *buf, u32 n, bool peek);  /* bytes, 0 = EOF, -EAGAIN, -errno */
void tcp_shutdown(struct tcb *t);                       /* no more sending (FIN) */
void tcp_close(struct tcb *t);                          /* the owner lets go */
u32 tcp_poll(struct tcb *t);
enum tcp_state tcp_get_state(const struct tcb *t);
int tcp_get_error(const struct tcb *t);
void tcp_get_addrs(const struct tcb *t, u32 *laddr, u16 *lport, u32 *raddr, u16 *rport);
bool tcp_port_in_use(u32 laddr, u16 lport);
void tcp_input(struct netbuf *nb);      /* nb->data at the TCP header; consumes nb */
void tcp_timer(void);                   /* every few ms from netd */
bool tcp_timers_pending(void);
const char *tcp_state_name(enum tcp_state s);
void tcp_print(int (*print)(const char *fmt, ...));
void tcp_print_stats(int (*print)(const char *fmt, ...));
/* hooks */
void tcp_wake_owner(void *owner);
void tcp_trace_state(const struct tcb *t, int from, int to);
void tcp_trace_rexmit(const struct tcb *t, u32 seq, u32 len);

#ifndef NET_HOST_TEST
/* ======== kernel-only part ======== */

#include <fs.h>
#include <sched.h>
#include <uapi/net.h>

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806

struct netif_stats {
    u64 rx_packets, rx_bytes, rx_dropped, rx_errors;
    u64 tx_packets, tx_bytes, tx_dropped;
};

struct netif {
    char name[8];
    u8 mac[ETH_ALEN];
    bool has_mac;               /* Ethernet (eth0) vs. lo */
    bool up;
    u32 addr, netmask, gateway;
    u32 mtu;
    /* Transmit a frame (Ethernet header first; for lo the IP header). Consumes nb. */
    int (*xmit)(struct netif *dev, struct netbuf *nb);
    struct netif_stats stats;
};

#define MAX_NETIFS 2
extern struct netif *netifs[MAX_NETIFS];
extern int nnetifs;
extern struct netif loopback_if;

extern struct mutex net_lock;
extern int net_drop_percent;

struct net_stats {
    u64 ip_in, ip_out, ip_bad, ip_frags, ip_not_ours, ip_no_route;
    u64 icmp_in, icmp_out, icmp_echo_replied;
    u64 udp_in, udp_out, udp_no_port, udp_bad_cksum, udp_queue_full;
    u64 arp_requests_in, arp_replies_in, arp_requests_out, arp_unresolved;
    u64 injected_drops;
};
extern struct net_stats net_stats;

/* net.c */
void net_init(void);
void net_kick(void);                    /* wake netd: a timer may have been armed */
void netif_register(struct netif *dev);
void netif_rx(struct netif *dev, struct netbuf *nb);   /* from drivers, any context */
struct netif *netif_by_name(const char *name);
void net_pkt_log(const char *dir, struct netif *dev, const struct netbuf *nb);
const char *ip_str(u32 ip, char *buf);                 /* buf: 16 bytes */
void net_print_dev(int (*print)(const char *fmt, ...));
void net_print_stats(int (*print)(const char *fmt, ...));

/* ether.c: Ethernet and ARP */
void eth_input(struct netif *dev, struct netbuf *nb);
int eth_output(struct netif *dev, struct netbuf *nb, u32 next_hop, u16 type);
void arp_timer(void);
bool arp_timers_pending(void);
void arp_print(int (*print)(const char *fmt, ...));

/* ipv4.c */
void ip_input(struct netif *dev, struct netbuf *nb);
struct netif *ip_route(u32 dst, u32 *next_hop);
u32 ip_source_for(u32 dst);
bool ip_is_local(u32 addr);

/* udp.c and raw (ICMP) sockets */
struct socket;
void udp_input(struct netbuf *nb);
int udp_output(struct socket *s, u32 dst, u16 dport, const void *data, u32 len);
void raw_input(struct netbuf *nb);      /* a copy of every ICMP packet */
void icmp_input(struct netbuf *nb);

/* socket.c */
struct socket {
    int type;                   /* SOCK_STREAM / SOCK_DGRAM / SOCK_RAW */
    int proto;
    u64 id;
    u32 laddr, raddr;
    u16 lport, rport;
    bool bound, connected, listening;
    bool shut_rd, shut_wr;
    int error;                  /* pending error (SO_ERROR) */
    struct tcb *tcb;            /* stream sockets */
    struct list_head rxq;       /* datagram/raw: queued netbufs */
    int rxq_len;
    struct waitqueue wq;        /* readers, writers, accept, connect, poll */
    struct list_head node;      /* all_sockets */
    u64 rx_bytes, tx_bytes;
    int owner_pid;
};
extern struct list_head all_sockets;
void sock_wake(struct socket *s);
bool sock_port_in_use(int type, u32 laddr, u16 lport);
struct socket *udp_lookup(u32 laddr, u16 lport, u32 raddr, u16 rport);
void udp_print(int (*print)(const char *fmt, ...));
void sockets_print(int (*print)(const char *fmt, ...));
extern const struct chr_ops sock_ops;

/* virtio_net.c */
int virtio_net_init(void);

#endif /* !NET_HOST_TEST */
#endif
