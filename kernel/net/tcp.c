/*
 * TCP (RFC 793/9293, with RFC 6298 retransmission timing and RFC 5681
 * congestion control in its simplest form).
 *
 * One control block (struct tcb) per connection or listener:
 *   - send ring: bytes from snd_una on, snd_len of them (sent or not);
 *     the FIN, once the owner shuts down, sits just after the data
 *   - receive ring: in-order data not yet read, followed by room for the
 *     rest of the window. Out-of-order data is written straight to its place
 *     in the ring and remembered as a range (up to OOO_RANGES); when the gap
 *     before it fills, rcv_nxt jumps over it. Each out-of-order arrival is
 *     answered with a duplicate ACK, which drives the peer's fast retransmit.
 *   - one retransmission timer: on expiry, go back to snd_una and resend
 *     (go-back-N), doubling the RTO; with a zero window it becomes the
 *     persist timer and probes with one byte
 *   - listeners keep their not-yet-accepted children on a list
 *
 * Ownership: the socket layer owns a tcb through `owner`. When it lets go
 * (tcp_close) the connection finishes on its own (FIN handshake, TIME_WAIT)
 * and is freed when it reaches CLOSED. A tcb with an owner is never freed
 * here; the owner sees CLOSED and an error instead.
 *
 * Every call happens with net_lock held. This file depends only on net.h,
 * so tests/host/test_tcp.c runs it on the Mac against a simulated lossy link.
 *
 * Debugging: /proc/net/tcp (every connection: sequence numbers, windows,
 * RTO, retransmits), /proc/net/stats, tcp_state/tcp_rexmit trace events,
 * log=net:debug.
 */
#include <net.h>
#include <uapi/errno.h>

#ifdef NET_HOST_TEST
#include <string.h>
#else
#include <kstring.h>
#include <printk.h>
#endif

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define RTO_INITIAL_MS   1000
#define RTO_MIN_MS       200
#define RTO_MAX_MS       30000
#define MAX_RETRIES      10       /* data/FIN retransmissions before giving up */
#define MAX_SYN_RETRIES  5
#define TIME_WAIT_MS     2000     /* 2*MSL, kept short for a teaching system */
#define FIN_WAIT_2_MS    30000    /* orphans waiting for the peer's FIN */

#define OOO_RANGES       8

#define SEQ_LT(a, b)  ((i32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((i32)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((i32)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((i32)((a) - (b)) >= 0)

struct tcp_hdr {
    u16 sport, dport;
    u32 seq, ack;
    u8 off;                     /* data offset in words, upper 4 bits */
    u8 flags;
    u16 win, sum, urg;
} __attribute__((packed));

struct tcb {
    struct list_head node;      /* all tcbs */
    u32 id;
    enum tcp_state state;
    u32 laddr, raddr;
    u16 lport, rport;
    void *owner;                /* the socket, or NULL */
    int error;                  /* -errno to report: reset, refused, timed out */

    /* send side */
    u32 iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    u32 snd_max;                /* highest seq ever sent: snd_nxt rewinds on timeouts */
    u8 *sndbuf;
    u32 snd_head, snd_len;      /* ring: snd_len bytes from snd_una at snd_head */
    bool fin_queued;            /* owner is done sending: FIN after the data */
    bool fin_sent;              /* snd_nxt is past the FIN */
    bool fin_was_acked;
    u16 mss;                    /* peer's maximum segment size */
    u32 cwnd, ssthresh;
    int dupacks;

    /* receive side */
    u32 irs, rcv_nxt;
    u8 *rcvbuf;
    u32 rcv_head, rcv_len;      /* ring: rcv_len unread bytes from rcv_head */
    bool fin_rcvd;
    u32 last_adv_wnd;
    struct { u32 start, end; } ooo[OOO_RANGES];     /* received beyond rcv_nxt, sorted */
    int nooo;

    /* timers (net_now_ms) */
    u64 rexmit_at, tw_at, fw2_at;
    u32 rto, srtt, rttvar;      /* ms; srtt == 0: no sample yet */
    bool rtt_timing;
    u32 rtt_seq;
    u64 rtt_start;
    int retries;

    /* listeners: children not yet accepted; children: their listener */
    struct list_head children;
    struct list_head child_node;
    struct tcb *parent;
    int backlog, nchildren;

    u64 retransmits, segs_in, segs_out, bytes_in, bytes_out;
};

struct tcp_stats tcp_stats;
static LIST_HEAD(tcbs);
static u32 next_id = 1;

static const char *const state_names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
    "FIN_WAIT_2", "CLOSING", "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK",
};

const char *tcp_state_name(enum tcp_state s)
{
    return (unsigned)s < sizeof(state_names) / sizeof(state_names[0]) ? state_names[s] : "?";
}

/* ---- helpers ---- */

static u32 min32(u32 a, u32 b) { return a < b ? a : b; }

static u32 rcv_space(const struct tcb *t)
{
    return TCP_BUF_SIZE - t->rcv_len;
}

static u16 adv_window(const struct tcb *t)
{
    return (u16)min32(rcv_space(t), 65535);
}

static void ring_write(u8 *ring, u32 pos, const u8 *src, u32 n)
{
    pos %= TCP_BUF_SIZE;
    u32 first = min32(n, TCP_BUF_SIZE - pos);
    memcpy(ring + pos, src, first);
    memcpy(ring, src + first, n - first);
}

static void ring_read(const u8 *ring, u32 pos, u8 *dst, u32 n)
{
    pos %= TCP_BUF_SIZE;
    u32 first = min32(n, TCP_BUF_SIZE - pos);
    memcpy(dst, ring + pos, first);
    memcpy(dst + first, ring, n - first);
}

static void set_state(struct tcb *t, enum tcp_state s)
{
    if (t->state == s)
        return;
    net_log(4 /* debug */, "tcp %u %u.%u.%u.%u:%u: %s -> %s\n", t->id, t->raddr >> 24,
            (t->raddr >> 16) & 255, (t->raddr >> 8) & 255, t->raddr & 255, t->rport,
            tcp_state_name(t->state), tcp_state_name(s));
    tcp_trace_state(t, t->state, s);
    t->state = s;
    if (s == TCP_TIME_WAIT)
        t->tw_at = net_now_ms() + TIME_WAIT_MS;
}

static void wake(struct tcb *t)
{
    if (t->owner)
        tcp_wake_owner(t->owner);
    else if (t->parent && t->parent->owner)
        tcp_wake_owner(t->parent->owner);     /* accept() may be waiting */
}

static struct tcb *tcb_new(u32 laddr, u16 lport, u32 raddr, u16 rport)
{
    struct tcb *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->sndbuf = kmalloc(TCP_BUF_SIZE);
    t->rcvbuf = kmalloc(TCP_BUF_SIZE);
    if (!t->sndbuf || !t->rcvbuf) {
        kfree(t->sndbuf);
        kfree(t->rcvbuf);
        kfree(t);
        return NULL;
    }
    t->id = next_id++;
    t->laddr = laddr;
    t->lport = lport;
    t->raddr = raddr;
    t->rport = rport;
    t->mss = TCP_MSS_DEFAULT;
    t->rto = RTO_INITIAL_MS;
    t->iss = net_random();
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    list_init(&t->children);
    list_init(&t->child_node);
    list_add_tail(&t->node, &tcbs);
    return t;
}

static void tcb_free(struct tcb *t)
{
    list_del(&t->node);
    if (t->parent) {
        list_del(&t->child_node);
        t->parent->nchildren--;
    }
    kfree(t->sndbuf);
    kfree(t->rcvbuf);
    kfree(t);
}

/* CLOSED: free it unless someone still holds it. */
static void maybe_free(struct tcb *t)
{
    if (t->state == TCP_CLOSED && !t->owner)
        tcb_free(t);
}

/* ---- output ---- */

/*
 * Build and send one segment. Data comes from the send ring at offset
 * (seq - snd_una). Returns 0 or -ENOBUFS.
 */
static int send_segment_win(struct tcb *t, u32 seq, u8 flags, u32 len, bool same_window);

static int send_segment(struct tcb *t, u32 seq, u8 flags, u32 len)
{
    return send_segment_win(t, seq, flags, len, false);
}

static int send_segment_win(struct tcb *t, u32 seq, u8 flags, u32 len, bool same_window)
{
    struct netbuf *nb = nb_alloc();
    if (!nb)
        return -ENOBUFS;
    bool syn = flags & TCP_SYN;
    u32 hlen = sizeof(struct tcp_hdr) + (syn ? 4 : 0);
    struct tcp_hdr *h = nb_put(nb, hlen);
    h->sport = hton16(t->lport);
    h->dport = hton16(t->rport);
    h->seq = hton32(seq);
    h->ack = hton32(flags & TCP_ACK ? t->rcv_nxt : 0);
    h->off = (u8)((hlen / 4) << 4);
    h->flags = flags;
    if (!same_window || adv_window(t) < t->last_adv_wnd)
        t->last_adv_wnd = adv_window(t);
    h->win = hton16((u16)t->last_adv_wnd);
    h->sum = 0;
    h->urg = 0;
    if (syn) {
        u8 *opt = (u8 *)(h + 1);
        opt[0] = 2;                         /* MSS option */
        opt[1] = 4;
        opt[2] = TCP_MSS_LOCAL >> 8;
        opt[3] = TCP_MSS_LOCAL & 255;
    }
    if (len) {
        u8 *d = nb_put(nb, len);
        ring_read(t->sndbuf, t->snd_head + (seq - t->snd_una), d, len);
    }
    h->sum = l4_cksum(t->laddr, t->raddr, 6, nb->data, nb->len);
    t->segs_out++;
    t->bytes_out += len;
    tcp_stats.segs_out++;
    tcp_stats.bytes_out += len;
    if (flags & TCP_RST)
        tcp_stats.resets_sent++;
    ip_output(t->laddr, t->raddr, 6, nb);
    return 0;
}

static void set_nxt(struct tcb *t, u32 nxt)
{
    t->snd_nxt = nxt;
    if (SEQ_GT(nxt, t->snd_max))
        t->snd_max = nxt;
}

/*
 * A pure ACK carries snd_max, not snd_nxt: after a timeout rewinds snd_nxt
 * the peer may already hold data beyond it and would discard an ACK with an
 * "old" sequence number (found by the randomized host test).
 */
static void send_ack(struct tcb *t)
{
    send_segment(t, t->snd_max, TCP_ACK, 0);
}

/*
 * A duplicate ACK (for out-of-order data) repeats the last advertised
 * window: senders count an ACK as a duplicate only if the window is
 * unchanged (RFC 5681), and ours moves whenever the application reads.
 * Without this, a reading receiver never triggers fast retransmit and every
 * loss costs a full retransmission timeout.
 */
static void send_dupack(struct tcb *t)
{
    send_segment_win(t, t->snd_max, TCP_ACK, 0, true);
}

/* A reset for a segment that matches no connection (RFC 9293 3.10.7.1). */
static void send_rst_reply(u32 laddr, u16 lport, u32 raddr, u16 rport, const struct tcp_hdr *in,
                           u32 seglen)
{
    struct netbuf *nb = nb_alloc();
    if (!nb)
        return;
    struct tcp_hdr *h = nb_put(nb, sizeof(*h));
    memset(h, 0, sizeof(*h));
    h->sport = hton16(lport);
    h->dport = hton16(rport);
    if (in->flags & TCP_ACK) {
        h->seq = in->ack;
        h->flags = TCP_RST;
    } else {
        h->ack = hton32(ntoh32(in->seq) + seglen);
        h->flags = TCP_RST | TCP_ACK;
    }
    h->off = (sizeof(*h) / 4) << 4;
    h->sum = l4_cksum(laddr, raddr, 6, nb->data, nb->len);
    tcp_stats.resets_sent++;
    tcp_stats.segs_out++;
    ip_output(laddr, raddr, 6, nb);
}

static void arm_rexmit(struct tcb *t)
{
    if (!t->rexmit_at)
        t->rexmit_at = net_now_ms() + t->rto;
}

/* Send whatever the windows allow: new data, then the FIN. */
static void output(struct tcb *t)
{
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT &&
        t->state != TCP_FIN_WAIT_1 && t->state != TCP_CLOSING && t->state != TCP_LAST_ACK)
        return;
    for (;;) {
        if (t->fin_sent)
            return;
        u32 sent = t->snd_nxt - t->snd_una;             /* data already in flight */
        u32 avail = t->snd_len - sent;
        u32 wnd = min32(t->snd_wnd, t->cwnd);
        i32 usable = (i32)(t->snd_una + wnd - t->snd_nxt);
        if (avail && usable > 0) {
            u32 n = min32(min32(avail, (u32)usable), t->mss);
            if (!t->rtt_timing) {
                t->rtt_timing = true;
                t->rtt_seq = t->snd_nxt;
                t->rtt_start = net_now_ms();
            }
            if (send_segment(t, t->snd_nxt, TCP_ACK | (n == avail ? TCP_PSH : 0), n))
                return;
            set_nxt(t, t->snd_nxt + n);
            arm_rexmit(t);
            continue;
        }
        if (!avail && t->fin_queued && usable >= 0) {
            if (send_segment(t, t->snd_nxt, TCP_FIN | TCP_ACK, 0))
                return;
            set_nxt(t, t->snd_nxt + 1);
            t->fin_sent = true;
            arm_rexmit(t);
        }
        if (avail && t->snd_wnd == 0)
            arm_rexmit(t);                              /* persist: probe later */
        return;
    }
}

/* ---- the public interface (called by the socket layer) ---- */

struct tcb *tcp_connect(u32 laddr, u16 lport, u32 raddr, u16 rport, void *owner, int *err)
{
    struct tcb *t = tcb_new(laddr, lport, raddr, rport);
    if (!t) {
        *err = -ENOMEM;
        return NULL;
    }
    t->owner = owner;
    set_state(t, TCP_SYN_SENT);
    tcp_stats.active_opens++;
    send_segment(t, t->iss, TCP_SYN, 0);
    set_nxt(t, t->iss + 1);
    arm_rexmit(t);
    return t;
}

struct tcb *tcp_listen(u32 laddr, u16 lport, int backlog, void *owner)
{
    struct tcb *t = tcb_new(laddr, lport, 0, 0);
    if (!t)
        return NULL;
    t->owner = owner;
    t->backlog = backlog < 1 ? 1 : backlog > 64 ? 64 : backlog;
    set_state(t, TCP_LISTEN);
    return t;
}

struct tcb *tcp_accept(struct tcb *l)
{
    struct tcb *c;
    list_for_each_entry(c, &l->children, child_node) {
        if (c->state != TCP_SYN_RCVD) {
            list_del(&c->child_node);
            list_init(&c->child_node);
            c->parent = NULL;
            l->nchildren--;
            tcp_stats.accepts++;
            return c;
        }
    }
    return NULL;
}

void tcp_set_owner(struct tcb *t, void *owner)
{
    t->owner = owner;
}

int tcp_send(struct tcb *t, const void *buf, u32 n)
{
    if (t->error)
        return t->error == -ECONNREFUSED ? -ECONNREFUSED : -EPIPE;
    if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD)
        return -EAGAIN;
    if (t->fin_queued || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT))
        return -EPIPE;
    u32 room = TCP_BUF_SIZE - t->snd_len;
    n = min32(n, room);
    if (!n)
        return -EAGAIN;
    ring_write(t->sndbuf, t->snd_head + t->snd_len, buf, n);
    t->snd_len += n;
    output(t);
    return (int)n;
}

int tcp_recv(struct tcb *t, void *buf, u32 n, bool peek)
{
    if (t->rcv_len) {
        n = min32(n, t->rcv_len);
        ring_read(t->rcvbuf, t->rcv_head, buf, n);
        if (peek)
            return (int)n;
        t->rcv_head = (t->rcv_head + n) % TCP_BUF_SIZE;
        t->rcv_len -= n;
        /* Reopened a window the peer may be stalled on: tell it. */
        if (t->last_adv_wnd < TCP_BUF_SIZE / 4 && rcv_space(t) >= TCP_BUF_SIZE / 2 &&
            (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
             t->state == TCP_FIN_WAIT_2))
            send_ack(t);
        return (int)n;
    }
    if (t->fin_rcvd)
        return 0;
    if (t->error)
        return t->error;
    if (t->state == TCP_CLOSED)
        return 0;
    return -EAGAIN;
}

void tcp_shutdown(struct tcb *t)
{
    switch (t->state) {
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
        t->fin_queued = true;
        set_state(t, TCP_FIN_WAIT_1);
        output(t);
        break;
    case TCP_CLOSE_WAIT:
        t->fin_queued = true;
        set_state(t, TCP_LAST_ACK);
        output(t);
        break;
    default:
        break;
    }
}

static void abort_conn(struct tcb *t, int err, bool send_rst)
{
    if (send_rst && t->state != TCP_CLOSED && t->state != TCP_LISTEN &&
        t->state != TCP_SYN_SENT && t->state != TCP_TIME_WAIT)
        send_segment(t, t->snd_nxt, TCP_RST | TCP_ACK, 0);
    t->error = err;
    t->rexmit_at = 0;
    set_state(t, TCP_CLOSED);
    wake(t);
}

void tcp_close(struct tcb *t)
{
    t->owner = NULL;
    if (t->state == TCP_LISTEN) {
        struct tcb *c, *tmp;
        list_for_each_entry_safe(c, tmp, &t->children, child_node) {
            abort_conn(c, -ECONNABORTED, true);
            tcb_free(c);
        }
        set_state(t, TCP_CLOSED);
    } else if (t->rcv_len && t->state != TCP_CLOSED) {
        abort_conn(t, -ECONNRESET, true);       /* unread data: reset (RFC 2525) */
    } else if (t->state == TCP_SYN_SENT) {
        set_state(t, TCP_CLOSED);
    } else {
        tcp_shutdown(t);
        if (t->state == TCP_FIN_WAIT_2)
            t->fw2_at = net_now_ms() + FIN_WAIT_2_MS;
    }
    maybe_free(t);
}

u32 tcp_poll(struct tcb *t)
{
    u32 m = 0;
    if (t->state == TCP_LISTEN) {
        struct tcb *c;
        list_for_each_entry(c, &t->children, child_node)
            if (c->state != TCP_SYN_RCVD)
                m |= TCP_POLL_IN;
        return m;
    }
    if (t->rcv_len || t->fin_rcvd || t->error || t->state == TCP_CLOSED)
        m |= TCP_POLL_IN;
    if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && !t->fin_queued &&
        t->snd_len < TCP_BUF_SIZE)
        m |= TCP_POLL_OUT;
    if (t->error)
        m |= TCP_POLL_ERR;
    if (t->state == TCP_CLOSED || (t->fin_rcvd && t->fin_queued))
        m |= TCP_POLL_HUP;
    return m;
}

enum tcp_state tcp_get_state(const struct tcb *t) { return t->state; }
int tcp_get_error(const struct tcb *t) { return t->error; }

void tcp_get_addrs(const struct tcb *t, u32 *laddr, u16 *lport, u32 *raddr, u16 *rport)
{
    *laddr = t->laddr;
    *lport = t->lport;
    *raddr = t->raddr;
    *rport = t->rport;
}

bool tcp_port_in_use(u32 laddr, u16 lport)
{
    struct tcb *t;
    list_for_each_entry(t, &tcbs, node)
        if (t->lport == lport && (!laddr || !t->laddr || t->laddr == laddr) &&
            t->state != TCP_TIME_WAIT && t->state != TCP_CLOSED)
            return true;
    return false;
}

/* ---- input ---- */

static void rtt_sample(struct tcb *t, u32 r)
{
    if (!t->srtt) {
        t->srtt = r ? r : 1;
        t->rttvar = r / 2;
    } else {
        u32 diff = r > t->srtt ? r - t->srtt : t->srtt - r;
        t->rttvar = (3 * t->rttvar + diff) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
        if (!t->srtt)
            t->srtt = 1;
    }
    u32 rto = t->srtt + (4 * t->rttvar > 10 ? 4 * t->rttvar : 10);
    t->rto = rto < RTO_MIN_MS ? RTO_MIN_MS : rto > RTO_MAX_MS ? RTO_MAX_MS : rto;
}

static void parse_options(struct tcb *t, const struct tcp_hdr *h)
{
    const u8 *o = (const u8 *)(h + 1), *end = (const u8 *)h + (h->off >> 4) * 4;
    while (o < end) {
        if (o[0] == 0)
            break;
        if (o[0] == 1) {
            o++;
            continue;
        }
        if (o + 1 >= end || o[1] < 2)
            break;
        if (o[0] == 2 && o[1] == 4 && o + 4 <= end) {
            u16 mss = (u16)(o[2] << 8 | o[3]);
            t->mss = mss > TCP_MSS_LOCAL ? TCP_MSS_LOCAL : mss < 64 ? 64 : mss;
        }
        o += o[1];
    }
}

static void init_cwnd(struct tcb *t)
{
    t->cwnd = 10 * t->mss;                      /* RFC 6928 */
    t->ssthresh = 65535;
}

/* An ACK that acknowledges new data (snd_una < ack <= snd_nxt). */
static void ack_advance(struct tcb *t, u32 ack)
{
    u32 acked = ack - t->snd_una;
    u32 data = min32(acked, t->snd_len);
    if (acked > t->snd_len && t->fin_queued)
        t->fin_was_acked = true;                /* the ACK covers our FIN too */
    if (t->rtt_timing && SEQ_GT(ack, t->rtt_seq)) {
        u64 now = net_now_ms();
        rtt_sample(t, (u32)(now - t->rtt_start));
        t->rtt_timing = false;
    }
    t->snd_head = (t->snd_head + data) % TCP_BUF_SIZE;
    t->snd_len -= data;
    t->snd_una = ack;
    if (SEQ_LT(t->snd_nxt, t->snd_una))
        t->snd_nxt = t->snd_una;
    /* congestion window: slow start, then additive increase */
    if (t->cwnd < t->ssthresh)
        t->cwnd += min32(acked, t->mss);
    else
        t->cwnd += t->mss * t->mss / (t->cwnd ? t->cwnd : 1);
    if (t->cwnd > 4 * TCP_BUF_SIZE)
        t->cwnd = 4 * TCP_BUF_SIZE;
    t->retries = 0;
    t->dupacks = 0;
    t->rexmit_at = 0;
    if (t->snd_una != t->snd_max)
        arm_rexmit(t);
    wake(t);                                    /* room in the send buffer */
}

static bool fin_acked(const struct tcb *t)
{
    return t->fin_was_acked;
}

static struct tcb *find_tcb(u32 laddr, u16 lport, u32 raddr, u16 rport)
{
    struct tcb *t, *listener = NULL;
    list_for_each_entry(t, &tcbs, node) {
        if (t->lport != lport)
            continue;
        if (t->state == TCP_LISTEN) {
            if (!t->laddr || t->laddr == laddr)
                listener = t;
        } else if (t->rport == rport && t->raddr == raddr && t->laddr == laddr &&
                   t->state != TCP_CLOSED) {
            return t;
        }
    }
    return listener;
}

static void listen_input(struct tcb *l, const struct tcp_hdr *h, u32 src, u32 dst)
{
    if (h->flags & TCP_RST)
        return;
    if (h->flags & TCP_ACK) {
        send_rst_reply(dst, ntoh16(h->dport), src, ntoh16(h->sport), h, 0);
        return;
    }
    if (!(h->flags & TCP_SYN))
        return;
    if (l->nchildren >= l->backlog) {
        tcp_stats.backlog_full++;
        return;                                 /* the peer retries the SYN */
    }
    struct tcb *c = tcb_new(dst, l->lport, src, ntoh16(h->sport));
    if (!c)
        return;
    c->parent = l;
    list_add_tail(&c->child_node, &l->children);
    l->nchildren++;
    c->irs = ntoh32(h->seq);
    c->rcv_nxt = c->irs + 1;
    c->snd_wnd = ntoh16(h->win);
    parse_options(c, h);
    init_cwnd(c);
    tcp_stats.passive_opens++;
    set_state(c, TCP_SYN_RCVD);
    c->snd_max = c->iss;
    send_segment(c, c->iss, TCP_SYN | TCP_ACK, 0);
    set_nxt(c, c->iss + 1);
    arm_rexmit(c);
}

static void syn_sent_input(struct tcb *t, const struct tcp_hdr *h)
{
    u32 seq = ntoh32(h->seq), ack = ntoh32(h->ack);
    bool ack_ok = (h->flags & TCP_ACK) && ack == t->iss + 1;
    if ((h->flags & TCP_ACK) && !ack_ok) {
        if (!(h->flags & TCP_RST))
            send_rst_reply(t->laddr, t->lport, t->raddr, t->rport, h, 0);
        return;
    }
    if (h->flags & TCP_RST) {
        if (ack_ok) {
            tcp_stats.resets_rcvd++;
            abort_conn(t, -ECONNREFUSED, false);
        }
        return;
    }
    if (!(h->flags & TCP_SYN))
        return;
    t->irs = seq;
    t->rcv_nxt = seq + 1;
    t->snd_wnd = ntoh16(h->win);
    t->snd_wl1 = seq;
    t->snd_wl2 = ack;
    parse_options(t, h);
    init_cwnd(t);
    if (ack_ok) {
        t->snd_una = ack;
        t->rexmit_at = 0;
        t->retries = 0;
        set_state(t, TCP_ESTABLISHED);
        send_ack(t);
        wake(t);
    } else {
        set_state(t, TCP_SYN_RCVD);             /* simultaneous open */
        send_segment(t, t->iss, TCP_SYN | TCP_ACK, 0);
    }
}

/*
 * Out-of-order data: write it where it belongs (after the unread data, at
 * its distance from rcv_nxt) if it fits in the window, and record the range.
 */
static void ooo_store(struct tcb *t, u32 seq, const u8 *payload, u32 len)
{
    u32 off = seq - t->rcv_nxt;
    u32 space = rcv_space(t);
    if (off >= space)
        return;
    len = min32(len, space - off);
    u32 start = seq, end = seq + len;
    /* merge with overlapping or adjacent ranges */
    int i = 0;
    while (i < t->nooo) {
        if (SEQ_LEQ(t->ooo[i].start, end) && SEQ_GEQ(t->ooo[i].end, start)) {
            if (SEQ_LT(t->ooo[i].start, start))
                start = t->ooo[i].start;
            if (SEQ_GT(t->ooo[i].end, end))
                end = t->ooo[i].end;
            t->ooo[i] = t->ooo[--t->nooo];
        } else {
            i++;
        }
    }
    if (t->nooo == OOO_RANGES)
        return;                                 /* too fragmented: let it be resent */
    ring_write(t->rcvbuf, t->rcv_head + t->rcv_len + off, payload, len);
    /* insert keeping the list sorted by start */
    i = t->nooo++;
    while (i > 0 && SEQ_GT(t->ooo[i - 1].start, start)) {
        t->ooo[i] = t->ooo[i - 1];
        i--;
    }
    t->ooo[i].start = start;
    t->ooo[i].end = end;
}

/* rcv_nxt advanced: absorb ranges that now continue the in-order data. */
static void ooo_merge(struct tcb *t)
{
    while (t->nooo && SEQ_LEQ(t->ooo[0].start, t->rcv_nxt)) {
        if (SEQ_GT(t->ooo[0].end, t->rcv_nxt)) {
            u32 n = t->ooo[0].end - t->rcv_nxt;
            t->rcv_len += n;
            t->rcv_nxt += n;
            t->bytes_in += n;
            tcp_stats.bytes_in += n;
        }
        for (int i = 1; i < t->nooo; i++)
            t->ooo[i - 1] = t->ooo[i];
        t->nooo--;
    }
}

/* Data and FIN for a synchronized connection; seg points at the payload. */
static void receive_data(struct tcb *t, const struct tcp_hdr *h, const u8 *payload, u32 len)
{
    u32 seq = ntoh32(h->seq);
    bool fin = h->flags & TCP_FIN;
    bool can_receive = t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
                       t->state == TCP_FIN_WAIT_2;

    if (len && can_receive) {
        if (SEQ_LT(seq, t->rcv_nxt)) {          /* partly old: trim the front */
            u32 skip = t->rcv_nxt - seq;
            if (skip >= len) {
                len = 0;
            } else {
                payload += skip;
                len -= skip;
                seq = t->rcv_nxt;
            }
        }
        if (len && seq != t->rcv_nxt) {         /* a gap before it: keep it, dup ACK */
            tcp_stats.out_of_order++;
            ooo_store(t, seq, payload, len);
            send_dupack(t);
            return;                             /* (an early FIN is resent by the peer) */
        }
        u32 n = min32(len, rcv_space(t));
        if (n) {
            ring_write(t->rcvbuf, t->rcv_head + t->rcv_len, payload, n);
            t->rcv_len += n;
            t->rcv_nxt += n;
            t->bytes_in += n;
            tcp_stats.bytes_in += n;
            ooo_merge(t);
            wake(t);
        }
        if (n < len)
            fin = false;                        /* the FIN comes after what we dropped */
    } else if (len) {
        fin = false;
    }

    if (fin && seq + len == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++;
        t->fin_rcvd = true;
        switch (t->state) {
        case TCP_SYN_RCVD:
        case TCP_ESTABLISHED:
            set_state(t, TCP_CLOSE_WAIT);
            break;
        case TCP_FIN_WAIT_1:
            set_state(t, fin_acked(t) ? TCP_TIME_WAIT : TCP_CLOSING);
            break;
        case TCP_FIN_WAIT_2:
            set_state(t, TCP_TIME_WAIT);
            break;
        default:
            break;
        }
        wake(t);
    }
    /*
     * ACK data and FINs. Not bare ACKs, even in TIME_WAIT: when both ends
     * close at once, both sit in TIME_WAIT, and answering ACKs with ACKs
     * would bounce one between them until the timers expire (an ACK storm
     * found by the SMP stress test; tests/host/test_tcp.c checks it). A
     * retransmitted FIN still gets its ACK, via the window check above.
     */
    if (len || fin)
        send_ack(t);
}

void tcp_input(struct netbuf *nb)
{
    tcp_stats.segs_in++;
    if (nb->len < sizeof(struct tcp_hdr) ||
        l4_cksum(nb->src_ip, nb->dst_ip, 6, nb->data, nb->len) != 0) {
        tcp_stats.bad_cksum++;
        nb_free(nb);
        return;
    }
    const struct tcp_hdr *h = (const struct tcp_hdr *)nb->data;
    u32 hlen = (h->off >> 4) * 4;
    if (hlen < sizeof(*h) || hlen > nb->len) {
        tcp_stats.bad_cksum++;
        nb_free(nb);
        return;
    }
    const u8 *payload = nb->data + hlen;
    u32 len = nb->len - hlen;
    u32 src = nb->src_ip, dst = nb->dst_ip;
    u16 sport = ntoh16(h->sport), dport = ntoh16(h->dport);
    u32 seq = ntoh32(h->seq), ack = ntoh32(h->ack);
    u8 flags = h->flags;
    u32 seglen = len + !!(flags & TCP_SYN) + !!(flags & TCP_FIN);

    struct tcb *t = find_tcb(dst, dport, src, sport);
    if (!t) {
        tcp_stats.no_listener++;
        if (!(flags & TCP_RST))
            send_rst_reply(dst, dport, src, sport, h, seglen);
        goto out;
    }
    t->segs_in++;
    if (t->state == TCP_LISTEN) {
        listen_input(t, h, src, dst);
        goto out;
    }
    if (t->state == TCP_SYN_SENT) {
        syn_sent_input(t, h);
        goto out;
    }

    /* 1. Is the segment in the receive window? */
    u32 wnd = rcv_space(t);
    bool ok;
    /*
     * Zero-length segments (pure ACKs) may sit exactly at the right edge:
     * their seq is the sender's snd_nxt, which is rcv_nxt + wnd when a full
     * window is in flight. RFC 793's strict "<" rejects them, so with full
     * windows both ways neither side would ever see the other's ACKs (found
     * by tests/host/test_tcp.c); Linux's tcp_sequence() accepts the edge too.
     */
    if (seglen == 0)
        ok = SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LEQ(seq, t->rcv_nxt + wnd);
    else
        ok = wnd && ((SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd)) ||
                     (SEQ_GEQ(seq + seglen - 1, t->rcv_nxt) &&
                      SEQ_LT(seq + seglen - 1, t->rcv_nxt + wnd)));
    if (!ok && t->state == TCP_SYN_RCVD && (flags & TCP_SYN) && seq == t->irs) {
        send_segment(t, t->iss, TCP_SYN | TCP_ACK, 0);      /* our SYN-ACK was lost */
        goto out;
    }
    if (!ok) {
        net_log(5, "tcp %u: segment seq %u len %u outside window [%u, +%u): ACK and drop\n",
                t->id, seq, seglen, t->rcv_nxt, wnd);
        /* Say where we are; but not in reply to a bare ACK (no ACK ping-pong). */
        if (!(flags & TCP_RST) && seglen)
            send_ack(t);
        goto out;
    }

    /* 2. Reset */
    if (flags & TCP_RST) {
        tcp_stats.resets_rcvd++;
        if (t->parent) {                        /* an embryonic child: just drop it */
            set_state(t, TCP_CLOSED);
            tcb_free(t);
            goto out;
        }
        abort_conn(t, t->state == TCP_SYN_RCVD ? -ECONNREFUSED : -ECONNRESET, false);
        maybe_free(t);
        goto out;
    }

    /* 3. A SYN in a synchronized state is an error */
    if (flags & TCP_SYN) {
        abort_conn(t, -ECONNRESET, true);
        if (t->parent)
            tcb_free(t);
        else
            maybe_free(t);
        goto out;
    }

    /* 4. ACK */
    if (!(flags & TCP_ACK))
        goto out;
    if (t->state == TCP_SYN_RCVD) {
        if (SEQ_GT(ack, t->snd_una) && SEQ_LEQ(ack, t->snd_nxt)) {
            t->snd_una = ack;
            t->rexmit_at = 0;
            t->retries = 0;
            t->snd_wnd = ntoh16(h->win);
            t->snd_wl1 = seq;
            t->snd_wl2 = ack;
            set_state(t, TCP_ESTABLISHED);
            wake(t);                            /* accept() can take it now */
        } else {
            send_rst_reply(dst, dport, src, sport, h, seglen);
            goto out;
        }
    }
    if (SEQ_GT(ack, t->snd_max)) {
        net_log(5, "tcp %u: ack %u beyond snd_max %u: drop\n", t->id, ack, t->snd_max);
        send_ack(t);                            /* acks something we never sent */
        goto out;
    }
    u32 win = ntoh16(h->win);
    if (SEQ_GT(ack, t->snd_una)) {
        ack_advance(t, ack);
    } else if (ack == t->snd_una && t->snd_max != t->snd_una && !len &&
               !(flags & TCP_FIN) && win == t->snd_wnd) {
        tcp_stats.dup_acks++;
        if (++t->dupacks == 3) {                /* fast retransmit */
            u32 flight = t->snd_max - t->snd_una;
            t->ssthresh = flight / 2 > 2u * t->mss ? flight / 2 : 2u * t->mss;
            t->cwnd = t->ssthresh;
            t->rtt_timing = false;
            u32 n = min32(t->mss, t->snd_len);
            if (n) {
                send_segment(t, t->snd_una, TCP_ACK, n);
                t->retransmits++;
                tcp_stats.retransmits++;
                tcp_stats.fast_retransmits++;
                tcp_trace_rexmit(t, t->snd_una, n);
            }
        }
    }
    /* window update (RFC 9293 3.10.7.4) */
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
        bool opened = t->snd_wnd == 0 && win > 0;
        t->snd_wnd = win;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        if (opened) {                           /* stop probing; resend from snd_una */
            t->rexmit_at = 0;
            t->snd_nxt = t->snd_una;
            t->fin_sent = false;
        }
    }
    if (fin_acked(t)) {
        switch (t->state) {
        case TCP_FIN_WAIT_1:
            set_state(t, TCP_FIN_WAIT_2);
            if (!t->owner)
                t->fw2_at = net_now_ms() + FIN_WAIT_2_MS;
            break;
        case TCP_CLOSING:
            set_state(t, TCP_TIME_WAIT);
            break;
        case TCP_LAST_ACK:
            set_state(t, TCP_CLOSED);
            wake(t);
            maybe_free(t);
            goto out;
        default:
            break;
        }
    }

    /* 5. Data and FIN */
    receive_data(t, h, payload, len);
    if (t->state != TCP_CLOSED)
        output(t);

out:
    nb_free(nb);
}

/* ---- timers ---- */

static void retransmit_timeout(struct tcb *t, u64 now)
{
    t->rexmit_at = 0;
    if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) {
        if (++t->retries > MAX_SYN_RETRIES) {
            tcp_stats.timeouts++;
            bool child = t->parent != NULL;
            abort_conn(t, -ETIMEDOUT, false);
            if (child)
                tcb_free(t);
            else
                maybe_free(t);
            return;
        }
        t->rto = t->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : t->rto * 2;
        t->retransmits++;
        tcp_stats.retransmits++;
        tcp_trace_rexmit(t, t->iss, 0);
        send_segment(t, t->iss, TCP_SYN | (t->state == TCP_SYN_RCVD ? TCP_ACK : 0), 0);
        t->rexmit_at = now + t->rto;
        return;
    }
    if (t->snd_wnd == 0 && t->snd_len) {
        /*
         * Persist: the peer's window is shut. Send one byte to elicit an ACK
         * (with the window); a slow reader is not a failure, so no retry count.
         */
        send_segment(t, t->snd_una, TCP_ACK, 1);
        set_nxt(t, t->snd_una + 1);
        t->fin_sent = false;
        t->rto = t->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : t->rto * 2;
        t->rexmit_at = now + t->rto;
        return;
    }
    if (t->snd_una == t->snd_max)
        return;                                 /* nothing outstanding after all */
    if (++t->retries > MAX_RETRIES) {
        tcp_stats.timeouts++;
        abort_conn(t, -ETIMEDOUT, true);
        maybe_free(t);
        return;
    }
    tcp_stats.timeouts++;
    u32 flight = t->snd_max - t->snd_una;
    t->ssthresh = flight / 2 > 2u * t->mss ? flight / 2 : 2u * t->mss;
    t->cwnd = t->mss;
    t->rto = t->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : t->rto * 2;
    t->rtt_timing = false;                      /* Karn: no samples from resends */
    t->dupacks = 0;
    t->retransmits++;
    tcp_stats.retransmits++;
    tcp_trace_rexmit(t, t->snd_una, flight);
    /* go back to the oldest unacknowledged byte */
    t->snd_nxt = t->snd_una;
    t->fin_sent = false;
    output(t);
    if (t->snd_nxt == t->snd_una && t->snd_len) {
        send_segment(t, t->snd_una, TCP_ACK, 1);    /* window shut meanwhile: probe */
        set_nxt(t, t->snd_una + 1);
    }
    t->rexmit_at = 0;
    arm_rexmit(t);
}

void tcp_timer(void)
{
    u64 now = net_now_ms();
    struct tcb *t, *tmp;
    list_for_each_entry_safe(t, tmp, &tcbs, node) {
        if (t->state == TCP_TIME_WAIT && now >= t->tw_at) {
            set_state(t, TCP_CLOSED);
            wake(t);
            maybe_free(t);
            continue;
        }
        if (t->state == TCP_FIN_WAIT_2 && !t->owner && t->fw2_at && now >= t->fw2_at) {
            abort_conn(t, -ETIMEDOUT, false);
            maybe_free(t);
            continue;
        }
        if (t->rexmit_at && now >= t->rexmit_at)
            retransmit_timeout(t, now);
    }
}

bool tcp_timers_pending(void)
{
    struct tcb *t;
    list_for_each_entry(t, &tcbs, node)
        if (t->rexmit_at || t->state == TCP_TIME_WAIT || t->fw2_at)
            return true;
    return false;
}

/* ---- debugging ---- */

static void print_addr(int (*print)(const char *fmt, ...), u32 a, u16 port)
{
    char s[24];
    int n = 0;
    /* %-21s by hand: print has no padding for composed strings */
    unsigned parts[4] = { a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255 };
    for (int i = 0; i < 4; i++) {
        unsigned v = parts[i];
        char d[4];
        int k = 0;
        do {
            d[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (k)
            s[n++] = d[--k];
        s[n++] = i < 3 ? '.' : ':';
    }
    unsigned v = port;
    char d[6];
    int k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        s[n++] = d[--k];
    s[n] = '\0';
    print("%-22s", s);
}

void tcp_print(int (*print)(const char *fmt, ...))
{
    struct tcb *t;
    print("  ID LOCAL                 REMOTE                STATE        SNDQ   RCVQ  "
          "SND-WND   CWND  RTO(ms) SRTT  REXMIT  OWNER\n");
    list_for_each_entry(t, &tcbs, node) {
        print("  %2u ", t->id);
        print_addr(print, t->laddr, t->lport);
        print_addr(print, t->raddr, t->rport);
        print("%-11s %6u %6u %7u %6u %7u %5u %6lu  %s\n", tcp_state_name(t->state), t->snd_len,
              t->rcv_len, t->snd_wnd, t->cwnd, t->rto, t->srtt, (unsigned long)t->retransmits,
              t->owner ? "socket" : t->parent ? "backlog" : "orphan");
    }
}

void tcp_print_stats(int (*print)(const char *fmt, ...))
{
    const struct tcp_stats *s = &tcp_stats;
    print("tcp: %lu active opens, %lu passive opens, %lu accepted, %lu resets sent, "
          "%lu resets received\n", (unsigned long)s->active_opens,
          (unsigned long)s->passive_opens, (unsigned long)s->accepts,
          (unsigned long)s->resets_sent, (unsigned long)s->resets_rcvd);
    print("tcp: %lu segments in, %lu out, %lu bytes in, %lu out\n", (unsigned long)s->segs_in,
          (unsigned long)s->segs_out, (unsigned long)s->bytes_in, (unsigned long)s->bytes_out);
    print("tcp: %lu retransmits (%lu fast, %lu timeouts), %lu duplicate ACKs, "
          "%lu out-of-order, %lu bad checksums, %lu to no listener, "
          "%lu SYNs over backlog\n", (unsigned long)s->retransmits,
          (unsigned long)s->fast_retransmits, (unsigned long)s->timeouts,
          (unsigned long)s->dup_acks, (unsigned long)s->out_of_order,
          (unsigned long)s->bad_cksum, (unsigned long)s->no_listener,
          (unsigned long)s->backlog_full);
}
