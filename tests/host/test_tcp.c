/*
 * Host-native test of the kernel's TCP (kernel/net/tcp.c) and checksum code
 * under ASan/UBSan. Two endpoints, A (10.0.0.1) and B (10.0.0.2), talk over
 * a simulated wire with configurable loss, duplication and delay, driven by
 * a simulated millisecond clock. Every scenario checks data integrity byte
 * for byte, the final states, and that no control block or packet buffer
 * leaks.
 *
 *   build/host/test_tcp [-v]        (-v: log every state change)
 */
#define NET_HOST_TEST 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include <uapi/errno.h>

/* ---- the simulated world ---- */

static u64 now_ms;
static int verbose, failures, checks;
static int loss_pct, dup_pct, delay_ms = 1;
static u64 wire_packets, wire_lost;
extern u64 nb_live;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(const void *p) { free((void *)p); }
u64 net_now_ms(void) { return now_ms; }
u32 net_random(void) { return (u32)rand(); }

void net_log(int level, const char *fmt, ...)
{
    if (!verbose || level > verbose + 3)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("  [%6lu ms] ", (unsigned long)now_ms);
    vprintf(fmt, ap);
    va_end(ap);
}

static int wakeups;
void tcp_wake_owner(void *owner) { (void)owner; wakeups++; }
void tcp_trace_state(const struct tcb *t, int from, int to) { (void)t, (void)from, (void)to; }
static u64 rexmit_events;
void tcp_trace_rexmit(const struct tcb *t, u32 seq, u32 len)
{
    (void)t;
    rexmit_events++;
    if (verbose)
        printf("  [%6lu ms] retransmit seq %u len %u\n", (unsigned long)now_ms, seq, len);
}

struct inflight {
    struct netbuf *nb;
    u64 at;
    struct inflight *next;
};
static struct inflight *wire;

int ip_output(u32 src, u32 dst, u8 proto, struct netbuf *nb)
{
    (void)proto;
    wire_packets++;
    if (rand() % 100 < loss_pct) {
        wire_lost++;
        nb_free(nb);
        return 0;
    }
    nb->src_ip = src;
    nb->dst_ip = dst;
    int copies = rand() % 100 < dup_pct ? 2 : 1;
    for (int i = 0; i < copies; i++) {
        struct inflight *f = malloc(sizeof(*f));
        f->nb = i ? nb_clone(nb) : nb;
        f->at = now_ms + delay_ms + (u64)(rand() % 3);
        /* append: delivery is in order of `at` per scan, roughly FIFO */
        struct inflight **pp = &wire;
        while (*pp)
            pp = &(*pp)->next;
        f->next = NULL;
        *pp = f;
    }
    return 0;
}

static void deliver_due(void)
{
    struct inflight **pp = &wire;
    while (*pp) {
        struct inflight *f = *pp;
        if (f->at <= now_ms) {
            *pp = f->next;
            tcp_input(f->nb);
            free(f);
        } else {
            pp = &f->next;
        }
    }
}

static void tick(void)
{
    now_ms++;
    deliver_due();
    if (now_ms % 5 == 0)
        tcp_timer();
}

#define A 0x0a000001u
#define B 0x0a000002u

static int owner_a, owner_b, owner_l;      /* addresses only: owner tokens */

/* ---- scenarios ---- */

static u8 *pattern(size_t n, unsigned seed)
{
    u8 *p = malloc(n);
    for (size_t i = 0; i < n; i++)
        p[i] = (u8)(i * 131 + seed * 7 + (i >> 8));
    return p;
}

/*
 * Connect A -> B, transfer `na` bytes A->B and `nb_` bytes B->A at the same
 * time, then close both ends. Returns 0 if everything checked out.
 */
static void transfer(const char *name, size_t na, size_t nbytes, int loss, int dup,
                     int reader_stall_ms)
{
    loss_pct = loss;
    dup_pct = dup;
    u64 start = now_ms, rex0 = tcp_stats.retransmits;
    u16 port = (u16)(1000 + rand() % 1000);
    struct tcb *l = tcp_listen(0, port, 4, &owner_l);
    int err = 0;
    struct tcb *a = tcp_connect(A, 40000, B, port, &owner_a, &err);
    CHECK(a && l, "%s: open", name);
    struct tcb *b = NULL;
    u8 *src_a = pattern(na, 1), *src_b = pattern(nbytes, 2);
    u8 *dst_a = malloc(nbytes + 1), *dst_b = malloc(na + 1);
    size_t sent_a = 0, sent_b = 0, got_a = 0, got_b = 0;
    bool closed_a = false, closed_b = false, eof_a = false, eof_b = false;

    for (int i = 0; i < 600000 && !(eof_a && eof_b && closed_a && closed_b); i++) {
        tick();
        if (!b && (b = tcp_accept(l)))
            tcp_set_owner(b, &owner_b);
        if (tcp_get_state(a) == TCP_ESTABLISHED || tcp_get_state(a) == TCP_CLOSE_WAIT) {
            if (sent_a < na) {
                int n = tcp_send(a, src_a + sent_a, (u32)(na - sent_a));
                if (n > 0)
                    sent_a += (size_t)n;
            }
        }
        if (b && sent_b < nbytes) {
            int n = tcp_send(b, src_b + sent_b, (u32)(nbytes - sent_b));
            if (n > 0)
                sent_b += (size_t)n;
        }
        if (sent_a == na && !closed_a && tcp_get_state(a) != TCP_SYN_SENT) {
            tcp_shutdown(a);
            closed_a = true;
        }
        if (b && sent_b == nbytes && !closed_b) {
            tcp_shutdown(b);
            closed_b = true;
        }
        /* readers (B can stall to exercise zero windows) */
        u8 buf[3000];
        int n;
        if (b && now_ms - start >= (u64)reader_stall_ms) {
            while ((n = tcp_recv(b, buf, sizeof(buf), false)) > 0) {
                CHECK(got_b + (size_t)n <= na, "%s: B got too much", name);
                if (got_b + (size_t)n <= na)
                    memcpy(dst_b + got_b, buf, (size_t)n);
                got_b += (size_t)n;
            }
            if (n == 0)
                eof_b = true;
        }
        while ((n = tcp_recv(a, buf, sizeof(buf), false)) > 0) {
            if (got_a + (size_t)n <= nbytes)
                memcpy(dst_a + got_a, buf, (size_t)n);
            got_a += (size_t)n;
        }
        if (n == 0 && tcp_get_state(a) != TCP_SYN_SENT)
            eof_a = true;
    }
    if (got_b != na || got_a != nbytes)
        tcp_print((int (*)(const char *, ...))printf);
    CHECK(got_b == na && !memcmp(src_a, dst_b, na), "%s: A->B data (%zu of %zu bytes)", name,
          got_b, na);
    CHECK(got_a == nbytes && !memcmp(src_b, dst_a, nbytes), "%s: B->A data (%zu of %zu bytes)",
          name, got_a, nbytes);
    CHECK(tcp_get_error(a) == 0 && (!b || tcp_get_error(b) == 0), "%s: errors %d %d", name,
          tcp_get_error(a), b ? tcp_get_error(b) : 0);
    printf("  %-34s %7zu+%-7zu bytes, loss %2d%%, dup %2d%%: %6lu ms, %4lu retransmits\n", name,
           na, nbytes, loss, dup, (unsigned long)(now_ms - start),
           (unsigned long)(tcp_stats.retransmits - rex0));

    /* let go: TIME_WAIT and retransmissions drain, everything is freed */
    tcp_close(a);
    if (b)
        tcp_close(b);
    tcp_close(l);
    loss_pct = dup_pct = 0;
    for (int i = 0; i < 40000; i++)
        tick();
    CHECK(!tcp_timers_pending(), "%s: timers still pending after close", name);
    free(src_a), free(src_b), free(dst_a), free(dst_b);
}

static void refused(void)
{
    int err = 0;
    struct tcb *a = tcp_connect(A, 40001, B, 9, &owner_a, &err);
    for (int i = 0; i < 100; i++)
        tick();
    CHECK(tcp_get_state(a) == TCP_CLOSED && tcp_get_error(a) == -ECONNREFUSED,
          "refused: state %s error %d", tcp_state_name(tcp_get_state(a)), tcp_get_error(a));
    CHECK(tcp_poll(a) & TCP_POLL_ERR, "refused: poll shows the error");
    tcp_close(a);
    printf("  %-34s ok\n", "connect to a closed port");
}

static void timeout(void)
{
    struct tcb *l = tcp_listen(0, 7777, 4, &owner_l);
    int err = 0;
    struct tcb *a = tcp_connect(A, 40002, B, 7777, &owner_a, &err);
    for (int i = 0; i < 100; i++)
        tick();
    struct tcb *b = tcp_accept(l);
    CHECK(b && tcp_get_state(a) == TCP_ESTABLISHED, "timeout: connected");
    tcp_set_owner(b, &owner_b);
    loss_pct = 100;                             /* the cable is cut */
    tcp_send(a, "hello", 5);
    u64 t0 = now_ms;
    while (tcp_get_state(a) != TCP_CLOSED && now_ms - t0 < 600000)
        tick();
    CHECK(tcp_get_error(a) == -ETIMEDOUT, "timeout: error %d", tcp_get_error(a));
    printf("  %-34s ETIMEDOUT after %lu ms\n", "data on a dead link",
           (unsigned long)(now_ms - t0));
    loss_pct = 0;
    tcp_close(a);
    tcp_close(b);
    tcp_close(l);
    for (int i = 0; i < 40000; i++)
        tick();
}

static void reset_on_close_with_unread_data(void)
{
    struct tcb *l = tcp_listen(0, 7778, 4, &owner_l);
    int err = 0;
    struct tcb *a = tcp_connect(A, 40003, B, 7778, &owner_a, &err);
    for (int i = 0; i < 50; i++)
        tick();
    struct tcb *b = tcp_accept(l);
    tcp_set_owner(b, &owner_b);
    tcp_send(a, "unread", 6);
    for (int i = 0; i < 50; i++)
        tick();
    tcp_close(b);                               /* closes with 6 unread bytes: RST */
    for (int i = 0; i < 50; i++)
        tick();
    CHECK(tcp_get_state(a) == TCP_CLOSED && tcp_get_error(a) == -ECONNRESET,
          "reset: A state %s error %d", tcp_state_name(tcp_get_state(a)), tcp_get_error(a));
    CHECK(tcp_send(a, "x", 1) == -EPIPE, "reset: send after reset is EPIPE");
    tcp_close(a);
    tcp_close(l);
    for (int i = 0; i < 40000; i++)
        tick();
    printf("  %-34s ok\n", "close with unread data resets");
}

static void backlog(void)
{
    struct tcb *l = tcp_listen(0, 7779, 2, &owner_l);
    struct tcb *c[4];
    int err = 0;
    for (int i = 0; i < 4; i++)
        c[i] = tcp_connect(A, (u16)(40010 + i), B, 7779, &owner_a, &err);
    for (int i = 0; i < 100; i++)
        tick();
    int est = 0;
    for (int i = 0; i < 4; i++)
        est += tcp_get_state(c[i]) == TCP_ESTABLISHED;
    CHECK(est == 2, "backlog 2: %d connections established before accept", est);
    CHECK(tcp_stats.backlog_full > 0, "backlog: SYNs over the backlog were dropped");
    /* accepting makes room; the others get in on a SYN retransmission */
    struct tcb *acc[4] = { 0 };
    int n = 0;
    for (int i = 0; i < 5000 && n < 4; i++) {
        tick();
        struct tcb *x = tcp_accept(l);
        if (x) {
            tcp_set_owner(x, &owner_b);
            acc[n++] = x;
        }
    }
    CHECK(n == 4, "backlog: accepted %d of 4", n);
    for (int i = 0; i < 4; i++)
        tcp_close(c[i]);
    for (int i = 0; i < n; i++)
        tcp_close(acc[i]);
    tcp_close(l);
    for (int i = 0; i < 40000; i++)
        tick();
    printf("  %-34s ok\n", "listen backlog and accept");
}

/* Both ends close at the same moment: FINs cross, CLOSING, TIME_WAIT. */
static void simultaneous_close(void)
{
    struct tcb *l = tcp_listen(0, 7780, 4, &owner_l);
    int err = 0;
    struct tcb *a = tcp_connect(A, 40020, B, 7780, &owner_a, &err);
    for (int i = 0; i < 50; i++)
        tick();
    struct tcb *b = tcp_accept(l);
    tcp_set_owner(b, &owner_b);
    u64 segs0 = tcp_stats.segs_out;
    tcp_shutdown(a);
    tcp_shutdown(b);
    for (int i = 0; i < 1000; i++)
        tick();
    u64 segs = tcp_stats.segs_out - segs0;
    CHECK(segs <= 8, "simultaneous close took %lu segments (an ACK storm?)", (unsigned long)segs);
    CHECK(tcp_get_state(a) == TCP_TIME_WAIT && tcp_get_state(b) == TCP_TIME_WAIT,
          "simultaneous close: states %s %s", tcp_state_name(tcp_get_state(a)),
          tcp_state_name(tcp_get_state(b)));
    tcp_close(a);
    tcp_close(b);
    tcp_close(l);
    for (int i = 0; i < 40000; i++)
        tick();
    printf("  %-34s %lu segments\n", "simultaneous close", (unsigned long)segs);
}

static void checksums(void)
{
    /* RFC 1071 section 3 example: words 0001 f203 f4f5 f6f7 sum to 0xddf2 */
    u8 data[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    u16 c = inet_cksum(data, sizeof(data));
    CHECK(ntoh16(c) == (u16)~0xddf2, "rfc1071 example: %#x", ntoh16(c));
    /* a header with its checksum filled in sums to zero */
    u8 hdr[20] = { 0x45, 0, 0, 20, 0, 0, 0x40, 0, 64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2 };
    u16 s = inet_cksum(hdr, 20);
    memcpy(hdr + 10, &s, 2);
    CHECK(inet_cksum(hdr, 20) == 0, "verify ip header");
    /* odd length */
    u8 odd[3] = { 0x12, 0x34, 0x56 };
    CHECK(ntoh16(inet_cksum(odd, 3)) == (u16)~(0x1234 + 0x5600), "odd length");
    printf("  %-34s ok\n", "checksums");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v") ? 1 : argc > 1 && !strcmp(argv[1], "-vv") ? 2 : 0;
    srand(12345);
    if (argc > 2) {     /* -v LOSS [SEED DUP STALL]: one transfer, for debugging */
        if (argc > 3)
            srand((unsigned)atoi(argv[3]) * 7919);
        transfer("debug", 60000, 60000, atoi(argv[2]), argc > 4 ? atoi(argv[4]) : 0,
                 argc > 5 ? atoi(argv[5]) : 0);
        return failures != 0;
    }
    printf("TCP over a simulated link:\n");
    checksums();
    transfer("handshake, small exchange", 100, 50, 0, 0, 0);
    transfer("bulk both ways", 300000, 200000, 0, 0, 0);
    transfer("bulk, 5% loss", 200000, 100000, 5, 0, 0);
    transfer("bulk, 20% loss", 100000, 50000, 20, 0, 0);
    transfer("bulk, 10% duplicated", 100000, 100000, 0, 10, 0);
    delay_ms = 30;
    transfer("bulk, 30 ms latency, 5% loss", 200000, 0, 5, 0, 0);
    delay_ms = 1;
    transfer("receiver stalls (zero window)", 200000, 0, 0, 0, 3000);
    /* many seeds: loss, duplication and reordering together */
    printf("  randomized: 25 seeds of 60 KB each way, 10%% loss, 5%% duplicates, jitter\n");
    int saved_verbose = verbose;
    for (int seed = 1; seed <= 25; seed++) {
        srand((unsigned)seed * 7919);
        verbose = 0;
        int before = failures;
        int quiet = 1;
        (void)quiet;
        transfer("  seed", 60000, 60000, 10, 5, seed % 3 ? 0 : 500);
        if (failures != before)
            printf("  ^ seed %d failed\n", seed);
    }
    verbose = saved_verbose;
    simultaneous_close();
    refused();
    reset_on_close_with_unread_data();
    backlog();
    timeout();
    CHECK(nb_live == 0, "%lu packet buffers leaked", (unsigned long)nb_live);
    tcp_print_stats((int (*)(const char *, ...))printf);
    printf("wire: %lu packets, %lu lost on purpose\n", (unsigned long)wire_packets,
           (unsigned long)wire_lost);
    printf("tcp: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
