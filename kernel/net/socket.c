/*
 * BSD sockets: AF_INET stream (TCP), datagram (UDP) and raw (ICMP) sockets
 * as files, and their system calls.
 *
 * Waiting: every wakeup of a socket (sock_wake, with net_lock held)
 * recomputes its readiness mask and bumps an event counter. Blocking calls
 * remember the counter, drop net_lock and sleep until it changes, so they
 * never read protocol state without the lock; ppoll reads the cached mask.
 *
 * Errors follow Linux: ECONNREFUSED/ETIMEDOUT from connect, ECONNRESET from
 * recv after a reset, EPIPE (+SIGPIPE unless MSG_NOSIGNAL) from send on a
 * connection that can no longer send, EAGAIN on non-blocking sockets.
 *
 * Debugging: /proc/net/{tcp,udp}; strace shows addresses as {a.b.c.d:port};
 * a blocked thread shows "socket" in ps.
 */
#define LOG_SUBSYS SUBSYS_NET
#include <debug.h>
#include <fmt.h>
#include <kstring.h>
#include <mm.h>
#include <net.h>
#include <printk.h>
#include <proc.h>
#include <uapi/errno.h>
#include <uapi/fs.h>

#define MSG_NOSIGNAL 0x4000
#define FIONREAD     0x541b
#define CHUNK        4096

static u64 next_sock_id = 1;
static u16 next_ephemeral = 49152;

/* ---- readiness and waiting (net_lock held) ---- */

struct socket_ext {
    struct socket s;
    u64 events;                 /* bumped by every sock_wake */
    u32 ready;                  /* POLLIN/POLLOUT/... as of the last change */
};

static struct socket_ext *ext(struct socket *s)
{
    return (struct socket_ext *)s;
}

static u32 compute_ready(struct socket *s)
{
    u32 m = 0;
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            return POLLOUT | POLLHUP;
        u32 t = tcp_poll(s->tcb);
        if (t & TCP_POLL_IN)
            m |= POLLIN;
        if (t & TCP_POLL_OUT)
            m |= POLLOUT;
        if (t & TCP_POLL_ERR)
            m |= POLLERR;
        if (t & TCP_POLL_HUP)
            m |= POLLHUP;
    } else {
        if (s->rxq_len)
            m |= POLLIN;
        m |= POLLOUT;
        if (s->error)
            m |= POLLERR;
    }
    if (s->shut_rd)
        m |= POLLIN;
    return m;
}

static void refresh(struct socket *s)
{
    ext(s)->ready = compute_ready(s);
}

void sock_wake(struct socket *s)
{
    refresh(s);
    ext(s)->events++;
    wake_up(&s->wq);
}

/* Sleep until the socket's state changes. Returns 0 or -ERESTARTSYS. */
static int sock_wait(struct socket *s)
{
    u64 seen = ext(s)->events;
    mutex_unlock(&net_lock);
    current->wait_what = "socket";
    int r = wait_event_interruptible(&s->wq, ext(s)->events != seen);
    current->wait_what = NULL;
    mutex_lock(&net_lock);
    return r ? -ERESTARTSYS : 0;
}

/* ---- creation and lookup ---- */

static struct socket *sock_new(int type, int proto)
{
    struct socket_ext *e = kzalloc(sizeof(*e));
    if (!e)
        return NULL;
    struct socket *s = &e->s;
    s->type = type;
    s->proto = proto;
    s->id = next_sock_id++;
    list_init(&s->rxq);
    waitqueue_init(&s->wq, "socket");
    s->owner_pid = current_proc() ? current_proc()->pid : 0;
    list_add_tail(&s->node, &all_sockets);
    refresh(s);
    return s;
}

static void sock_free(struct socket *s)
{
    list_del(&s->node);
    while (!list_empty(&s->rxq)) {
        struct netbuf *nb = list_first_entry(&s->rxq, struct netbuf, node);
        list_del(&nb->node);
        nb_free(nb);
    }
    if (s->tcb)
        tcp_close(s->tcb);
    kfree(ext(s));
}

static int install(struct socket *s, int flags)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->refcnt = 1;
    f->readable = f->writable = true;
    f->cops = &sock_ops;
    f->priv = s;
    f->flags = O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    ksnprintf(f->path, sizeof(f->path), "socket:[%lu] %s", (unsigned long)s->id,
              s->type == SOCK_STREAM ? "tcp" : s->type == SOCK_DGRAM ? "udp" : "raw");
    struct proc *p = current_proc();
    int fd = fd_install(p, f);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    if (flags & SOCK_CLOEXEC)
        p->fd_flags[fd] |= FD_CLOEXEC;
    return fd;
}

static struct socket *sock_of(long fd, struct file **fp, long *err)
{
    struct file *f = fd_get(fd);
    if (!f) {
        *err = -EBADF;
        return NULL;
    }
    if (f->cops != &sock_ops) {
        *err = -ENOTSOCK;
        return NULL;
    }
    if (fp)
        *fp = f;
    return f->priv;
}

#define GET_SOCK(fd, fp)                                                    \
    long __err = 0;                                                         \
    struct socket *s = sock_of((long)(fd), (fp), &__err);                   \
    if (!s)                                                                 \
        return __err

static int get_addr(uintptr_t uaddr, u64 len, u32 *ip, u16 *port)
{
    struct sockaddr_in sin;
    if (len < sizeof(sin))
        return -EINVAL;
    if (copy_from_user(&sin, uaddr, sizeof(sin)))
        return -EFAULT;
    if (sin.sin_family != AF_INET)
        return -EAFNOSUPPORT;
    *ip = ntoh32(sin.sin_addr.s_addr);
    *port = ntoh16(sin.sin_port);
    return 0;
}

static int put_addr(uintptr_t uaddr, uintptr_t ulen, u32 ip, u16 port)
{
    if (!uaddr || !ulen)
        return 0;
    u32 len;
    if (copy_from_user(&len, ulen, sizeof(len)))
        return -EFAULT;
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = hton16(port),
                               .sin_addr = { hton32(ip) } };
    if (copy_to_user(uaddr, &sin, MIN(len, (u32)sizeof(sin))))
        return -EFAULT;
    len = sizeof(sin);
    return copy_to_user(ulen, &len, sizeof(len)) ? -EFAULT : 0;
}

bool sock_port_in_use(int type, u32 laddr, u16 lport)
{
    struct socket *s;
    if (type == SOCK_STREAM && tcp_port_in_use(laddr, lport))
        return true;
    list_for_each_entry(s, &all_sockets, node)
        if (s->type == type && s->bound && s->lport == lport &&
            (!laddr || !s->laddr || s->laddr == laddr) &&
            (type != SOCK_STREAM || !s->tcb))
            return true;
    return false;
}

static u16 ephemeral_port(int type, u32 laddr)
{
    for (int tries = 0; tries < 16384; tries++) {
        u16 p = next_ephemeral++;
        if (next_ephemeral == 0)
            next_ephemeral = 49152;
        if (p >= 49152 && !sock_port_in_use(type, laddr, p))
            return p;
    }
    return 0;
}

static long bind_locked(struct socket *s, u32 ip, u16 port)
{
    if (s->bound)
        return -EINVAL;
    if (ip && !ip_is_local(ip) && ip != INADDR_BROADCAST)
        return -EADDRNOTAVAIL;
    if (port) {
        if (port < 1024 && current_proc()->euid != 0)
            return -EACCES;
        if (s->type != SOCK_RAW && sock_port_in_use(s->type, ip, port))
            return -EADDRINUSE;
    } else if (s->type != SOCK_RAW) {
        port = ephemeral_port(s->type, ip);
        if (!port)
            return -EADDRINUSE;
    }
    s->laddr = ip;
    s->lport = port;
    s->bound = true;
    return 0;
}

/* ---- system calls ---- */

long do_socket(const u64 *a)
{
    int domain = (int)a[0], type = (int)a[1] & 0xf, flags = (int)a[1] & ~0xf, proto = (int)a[2];
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type == SOCK_STREAM && proto && proto != IPPROTO_TCP)
        return -EPROTONOSUPPORT;
    if (type == SOCK_DGRAM && proto && proto != IPPROTO_UDP)
        return -EPROTONOSUPPORT;
    if (type == SOCK_RAW && proto != IPPROTO_ICMP)
        return -EPROTONOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return -EINVAL;
    if (type == SOCK_RAW && current_proc()->euid != 0)
        return -EPERM;
    mutex_lock(&net_lock);
    struct socket *s = sock_new(type, type == SOCK_STREAM ? IPPROTO_TCP :
                                      type == SOCK_DGRAM ? IPPROTO_UDP : proto);
    mutex_unlock(&net_lock);
    if (!s)
        return -ENOMEM;
    int fd = install(s, flags);
    if (fd < 0) {
        mutex_lock(&net_lock);
        sock_free(s);
        mutex_unlock(&net_lock);
    }
    return fd;
}

long do_bind(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    u32 ip;
    u16 port;
    long r = get_addr(a[1], a[2], &ip, &port);
    if (r)
        return r;
    mutex_lock(&net_lock);
    r = bind_locked(s, ip, port);
    mutex_unlock(&net_lock);
    return r;
}

long do_listen(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    mutex_lock(&net_lock);
    long r = 0;
    if (s->listening)
        goto out;
    if (s->tcb) {
        r = -EISCONN;
        goto out;
    }
    if (!s->bound && (r = bind_locked(s, 0, 0)))
        goto out;
    s->tcb = tcp_listen(s->laddr, s->lport, (int)a[1], s);
    if (!s->tcb) {
        r = -ENOMEM;
        goto out;
    }
    s->listening = true;
    refresh(s);
out:
    mutex_unlock(&net_lock);
    return r;
}

long do_accept4(const u64 *a)
{
    struct file *f;
    GET_SOCK(a[0], &f);
    mutex_lock(&net_lock);
    if (!s->listening) {
        mutex_unlock(&net_lock);
        return -EINVAL;
    }
    struct tcb *child;
    while (!(child = tcp_accept(s->tcb))) {
        refresh(s);
        if (f->flags & O_NONBLOCK) {
            mutex_unlock(&net_lock);
            return -EAGAIN;
        }
        if (sock_wait(s)) {
            mutex_unlock(&net_lock);
            return -ERESTARTSYS;
        }
    }
    refresh(s);
    struct socket *ns = sock_new(SOCK_STREAM, IPPROTO_TCP);
    if (!ns) {
        tcp_close(child);
        mutex_unlock(&net_lock);
        return -ENOMEM;
    }
    tcp_get_addrs(child, &ns->laddr, &ns->lport, &ns->raddr, &ns->rport);
    ns->bound = ns->connected = true;
    ns->tcb = child;
    tcp_set_owner(child, ns);
    refresh(ns);
    u32 rip = ns->raddr;
    u16 rport = ns->rport;
    mutex_unlock(&net_lock);

    int fd = install(ns, (int)a[3]);
    if (fd < 0) {
        mutex_lock(&net_lock);
        sock_free(ns);
        mutex_unlock(&net_lock);
        return fd;
    }
    put_addr(a[1], a[2], rip, rport);
    return fd;
}

long do_accept(const u64 *a)
{
    u64 b[4] = { a[0], a[1], a[2], 0 };
    return do_accept4(b);
}

long do_connect(const u64 *a)
{
    struct file *f;
    GET_SOCK(a[0], &f);
    u32 ip;
    u16 port;
    long r = get_addr(a[1], a[2], &ip, &port);
    if (r)
        return r;
    if (!ip)
        ip = INADDR_LOOPBACK;                   /* as Linux: 0.0.0.0 means this host */
    mutex_lock(&net_lock);
    if (s->type != SOCK_STREAM) {
        if (s->type == SOCK_DGRAM && !s->bound && (r = bind_locked(s, 0, 0)))
            goto out;
        s->raddr = ip;
        s->rport = port;
        s->connected = true;
        goto out;
    }
    if (s->listening) {
        r = -EINVAL;
        goto out;
    }
    if (s->tcb) {
        enum tcp_state st = tcp_get_state(s->tcb);
        if (st == TCP_SYN_SENT) {
            r = -EALREADY;
            goto out;
        }
        if (st != TCP_CLOSED) {
            r = -EISCONN;
            goto out;
        }
        tcp_close(s->tcb);                      /* a failed earlier attempt */
        s->tcb = NULL;
    }
    u32 laddr = s->laddr ? s->laddr : ip_source_for(ip);
    if (!laddr) {
        r = -ENETUNREACH;
        goto out;
    }
    if (!s->bound && (r = bind_locked(s, 0, 0)))
        goto out;
    int err = 0;
    s->tcb = tcp_connect(laddr, s->lport, ip, port, s, &err);
    if (!s->tcb) {
        r = err;
        goto out;
    }
    s->laddr = laddr;
    s->raddr = ip;
    s->rport = port;
    refresh(s);
    net_kick();
    if (f->flags & O_NONBLOCK) {
        r = -EINPROGRESS;
        goto out;
    }
    while (tcp_get_state(s->tcb) == TCP_SYN_SENT || tcp_get_state(s->tcb) == TCP_SYN_RCVD) {
        if (sock_wait(s)) {
            r = -ERESTARTSYS;
            goto out;
        }
    }
    if (tcp_get_state(s->tcb) == TCP_CLOSED) {
        r = tcp_get_error(s->tcb) ? tcp_get_error(s->tcb) : -ECONNREFUSED;
        goto out;
    }
    s->connected = true;
out:
    mutex_unlock(&net_lock);
    return r;
}

/* ---- data ---- */

static long copy_in(void *dst, const void *src, size_t n, bool user)
{
    if (user)
        return copy_from_user(dst, (uintptr_t)src, n);
    memcpy(dst, src, n);
    return 0;
}

static long copy_out(void *dst, const void *src, size_t n, bool user)
{
    if (user)
        return copy_to_user((uintptr_t)dst, src, n);
    memcpy(dst, src, n);
    return 0;
}

static long sock_send(struct socket *s, struct file *f, const void *buf, size_t n, bool user,
                      int flags, bool has_dst, u32 dst, u16 dport)
{
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    u8 *k = kmalloc(CHUNK);
    if (!k)
        return -ENOMEM;
    long r = 0;
    size_t done = 0;
    mutex_lock(&net_lock);
    if (s->type == SOCK_STREAM) {
        if (!s->tcb || s->listening) {
            r = -ENOTCONN;
            goto out;
        }
        while (done < n) {
            size_t chunk = MIN(n - done, (size_t)CHUNK);
            if (copy_in(k, (const u8 *)buf + done, chunk, user)) {
                r = -EFAULT;
                break;
            }
            int t = tcp_send(s->tcb, k, (u32)chunk);
            if (t > 0) {
                done += (size_t)t;
                s->tx_bytes += (u64)t;
                continue;
            }
            if (t == -EAGAIN) {
                if (nonblock || done)           /* a partial write returns what it did */
                    break;
                if (sock_wait(s)) {
                    r = -ERESTARTSYS;
                    break;
                }
                continue;
            }
            r = t;
            if (t == -EPIPE && !(flags & MSG_NOSIGNAL) && !done)
                signal_send(current_proc(), SIGPIPE, current_proc()->pid);
            break;
        }
        if (done)
            r = (long)done;
        else if (!r)
            r = -EAGAIN;
        net_kick();
    } else {
        if (!has_dst) {
            if (!s->connected) {
                r = -EDESTADDRREQ;
                goto out;
            }
            dst = s->raddr;
            dport = s->rport;
        }
        if (n > CHUNK) {
            r = -EMSGSIZE;
            goto out;
        }
        if (copy_in(k, buf, n, user)) {
            r = -EFAULT;
            goto out;
        }
        if (s->type == SOCK_DGRAM) {
            if (!s->bound && (r = bind_locked(s, 0, 0)))
                goto out;
            r = udp_output(s, dst, dport, k, (u32)n);
        } else {
            struct netbuf *nb = nb_alloc();
            if (!nb || n > 1480) {
                nb_free(nb);
                r = nb ? -EMSGSIZE : -ENOBUFS;
                goto out;
            }
            memcpy(nb_put(nb, (u32)n), k, n);
            s->tx_bytes += n;
            net_stats.icmp_out++;
            r = ip_output(s->laddr, dst, (u8)s->proto, nb);
            if (r >= 0)
                r = (long)n;
        }
    }
out:
    refresh(s);
    mutex_unlock(&net_lock);
    kfree(k);
    return r;
}

static long sock_recv(struct socket *s, struct file *f, void *buf, size_t n, bool user, int flags,
                      uintptr_t uaddr, uintptr_t ulen)
{
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    bool peek = flags & MSG_PEEK;
    u8 *k = kmalloc(CHUNK);
    if (!k)
        return -ENOMEM;
    long r;
    u32 from = 0;
    u16 from_port = 0;
    mutex_lock(&net_lock);
    if (s->type == SOCK_STREAM) {
        if (!s->tcb || s->listening) {
            r = -ENOTCONN;
            goto out;
        }
        for (;;) {
            if (s->shut_rd) {
                r = 0;
                break;
            }
            r = tcp_recv(s->tcb, k, (u32)MIN(n, (size_t)CHUNK), peek);
            if (r != -EAGAIN)
                break;
            if (nonblock)
                break;
            if (sock_wait(s)) {
                r = -ERESTARTSYS;
                break;
            }
        }
        from = s->raddr;
        from_port = s->rport;
        if (r > 0) {
            s->rx_bytes += (u64)r;
            net_kick();                         /* a window update may be due */
        }
    } else {
        while (list_empty(&s->rxq) && !s->shut_rd) {
            if (nonblock) {
                r = -EAGAIN;
                goto out;
            }
            if (sock_wait(s)) {
                r = -ERESTARTSYS;
                goto out;
            }
        }
        if (list_empty(&s->rxq)) {
            r = 0;
            goto out;
        }
        struct netbuf *nb = list_first_entry(&s->rxq, struct netbuf, node);
        r = (long)MIN(n, (size_t)nb->len);
        if (r > CHUNK)
            r = CHUNK;
        memcpy(k, nb->data, (size_t)r);
        from = nb->src_ip;
        from_port = nb->src_port;
        if (!peek) {
            list_del(&nb->node);
            s->rxq_len--;
            nb_free(nb);
        }
    }
out:
    refresh(s);
    mutex_unlock(&net_lock);
    if (r > 0 && copy_out(buf, k, (size_t)r, user))
        r = -EFAULT;
    kfree(k);
    if (r >= 0 && uaddr)
        put_addr(uaddr, ulen, from, from_port);
    return r;
}

long do_sendto(const u64 *a)
{
    struct file *f;
    GET_SOCK(a[0], &f);
    u32 ip = 0;
    u16 port = 0;
    if (a[4]) {
        long r = get_addr(a[4], a[5], &ip, &port);
        if (r)
            return r;
    }
    return sock_send(s, f, (const void *)a[1], a[2], true, (int)a[3], a[4] != 0, ip, port);
}

long do_recvfrom(const u64 *a)
{
    struct file *f;
    GET_SOCK(a[0], &f);
    return sock_recv(s, f, (void *)a[1], a[2], true, (int)a[3], a[4], a[5]);
}

long do_shutdown(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    int how = (int)a[1];
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -EINVAL;
    mutex_lock(&net_lock);
    long r = 0;
    if (s->type == SOCK_STREAM && !s->tcb)
        r = -ENOTCONN;
    if (!r && (how == SHUT_RD || how == SHUT_RDWR))
        s->shut_rd = true;
    if (!r && (how == SHUT_WR || how == SHUT_RDWR)) {
        s->shut_wr = true;
        if (s->tcb)
            tcp_shutdown(s->tcb);
    }
    sock_wake(s);
    net_kick();
    mutex_unlock(&net_lock);
    return r;
}

long do_getsockname(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    mutex_lock(&net_lock);
    u32 ip = s->laddr;
    u16 port = s->lport;
    mutex_unlock(&net_lock);
    return put_addr(a[1], a[2], ip, port);
}

long do_getpeername(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    mutex_lock(&net_lock);
    bool conn = s->type == SOCK_STREAM ? s->tcb && !s->listening &&
                                         tcp_get_state(s->tcb) != TCP_SYN_SENT
                                       : s->connected;
    u32 ip = s->raddr;
    u16 port = s->rport;
    mutex_unlock(&net_lock);
    if (!conn)
        return -ENOTCONN;
    return put_addr(a[1], a[2], ip, port);
}

long do_setsockopt(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    (void)s;
    int level = (int)a[1], opt = (int)a[2];
    if (level == SOL_SOCKET && (opt == SO_REUSEADDR || opt == SO_RCVBUF || opt == SO_SNDBUF))
        return 0;                               /* accepted; nothing to tune */
    if (level == IPPROTO_TCP && opt == 1)       /* TCP_NODELAY: we never delay anyway */
        return 0;
    return -ENOPROTOOPT;
}

long do_getsockopt(const u64 *a)
{
    GET_SOCK(a[0], NULL);
    int level = (int)a[1], opt = (int)a[2];
    int val;
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    mutex_lock(&net_lock);
    if (opt == SO_ERROR) {
        int e = s->error ? s->error : s->tcb ? tcp_get_error(s->tcb) : 0;
        s->error = 0;
        val = -e;
    } else if (opt == SO_TYPE) {
        val = s->type;
    } else {
        mutex_unlock(&net_lock);
        return -ENOPROTOOPT;
    }
    mutex_unlock(&net_lock);
    u32 len = sizeof(val);
    if (copy_to_user(a[3], &val, sizeof(val)) || (a[4] && copy_to_user(a[4], &len, sizeof(len))))
        return -EFAULT;
    return 0;
}

/* ---- file operations ---- */

static long sock_read(struct file *f, void *buf, size_t n, bool user)
{
    return sock_recv(f->priv, f, buf, n, user, 0, 0, 0);
}

static long sock_write(struct file *f, const void *buf, size_t n, bool user)
{
    return sock_send(f->priv, f, buf, n, user, 0, false, 0, 0);
}

static void sock_close(struct file *f)
{
    mutex_lock(&net_lock);
    sock_free(f->priv);
    net_kick();
    mutex_unlock(&net_lock);
}

static u32 sock_poll(struct file *f, struct poll_table *pt)
{
    struct socket *s = f->priv;
    poll_wait(pt, &s->wq);
    return ext(s)->ready;
}

static long sock_ioctl(struct file *f, unsigned long req, uintptr_t arg)
{
    (void)f;
    struct netif_req r;
    if (req == NET_IOC_GETIF || req == NET_IOC_SETIF) {
        if (copy_from_user(&r, arg, sizeof(r)))
            return -EFAULT;
        mutex_lock(&net_lock);
        struct netif *d = req == NET_IOC_GETIF
                              ? (r.index >= 0 && r.index < nnetifs ? netifs[r.index] : NULL)
                              : netif_by_name(r.name);
        long ret = 0;
        if (!d) {
            ret = -ENODEV;
        } else if (req == NET_IOC_SETIF) {
            if (current_proc()->euid != 0) {
                ret = -EPERM;
            } else if (d != &loopback_if) {
                d->addr = r.addr;
                d->netmask = r.netmask;
                d->gateway = r.gateway;
                d->up = r.up;
            }
        } else {
            int idx = r.index;
            memset(&r, 0, sizeof(r));
            r.index = idx;
            strlcpy(r.name, d->name, sizeof(r.name));
            memcpy(r.mac, d->mac, sizeof(r.mac));
            r.has_mac = d->has_mac;
            r.up = d->up;
            r.addr = d->addr;
            r.netmask = d->netmask;
            r.gateway = d->gateway;
            r.mtu = d->mtu;
            r.rx_packets = d->stats.rx_packets;
            r.rx_bytes = d->stats.rx_bytes;
            r.rx_dropped = d->stats.rx_dropped;
            r.tx_packets = d->stats.tx_packets;
            r.tx_bytes = d->stats.tx_bytes;
            r.tx_dropped = d->stats.tx_dropped;
        }
        mutex_unlock(&net_lock);
        if (!ret && req == NET_IOC_GETIF && copy_to_user(arg, &r, sizeof(r)))
            ret = -EFAULT;
        return ret;
    }
    return -ENOTTY;
}

const struct chr_ops sock_ops = {
    .read = sock_read, .write = sock_write, .close = sock_close, .poll = sock_poll,
    .ioctl = sock_ioctl,
};
