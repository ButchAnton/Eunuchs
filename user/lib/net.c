/* libc: sockets, poll/select, and address helpers. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <uapi/syscall.h>
#include <unistd.h>

static long ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define SC(n, a, b, c, d, e, f) \
    ret(__syscall((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f)))

int socket(int domain, int type, int proto) { return (int)SC(SYS_socket, domain, type, proto, 0, 0, 0); }
int bind(int fd, const struct sockaddr *a, socklen_t l) { return (int)SC(SYS_bind, fd, a, l, 0, 0, 0); }
int listen(int fd, int backlog) { return (int)SC(SYS_listen, fd, backlog, 0, 0, 0, 0); }
int accept(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC(SYS_accept, fd, a, l, 0, 0, 0); }
int accept4(int fd, struct sockaddr *a, socklen_t *l, int flags)
{
    return (int)SC(SYS_accept4, fd, a, l, flags, 0, 0);
}
int connect(int fd, const struct sockaddr *a, socklen_t l) { return (int)SC(SYS_connect, fd, a, l, 0, 0, 0); }
int getsockname(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC(SYS_getsockname, fd, a, l, 0, 0, 0); }
int getpeername(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC(SYS_getpeername, fd, a, l, 0, 0, 0); }
ssize_t sendto(int fd, const void *b, size_t n, int fl, const struct sockaddr *to, socklen_t l)
{
    return SC(SYS_sendto, fd, b, n, fl, to, l);
}
ssize_t recvfrom(int fd, void *b, size_t n, int fl, struct sockaddr *from, socklen_t *l)
{
    return SC(SYS_recvfrom, fd, b, n, fl, from, l);
}
ssize_t send(int fd, const void *b, size_t n, int fl) { return sendto(fd, b, n, fl, NULL, 0); }
ssize_t recv(int fd, void *b, size_t n, int fl) { return recvfrom(fd, b, n, fl, NULL, NULL); }
int setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l)
{
    return (int)SC(SYS_setsockopt, fd, lvl, opt, v, l, 0);
}
int getsockopt(int fd, int lvl, int opt, void *v, socklen_t *l)
{
    return (int)SC(SYS_getsockopt, fd, lvl, opt, v, l, 0);
}
int shutdown(int fd, int how) { return (int)SC(SYS_shutdown, fd, how, 0, 0, 0, 0); }

int poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
    struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000 };
    return (int)SC(SYS_ppoll, fds, n, timeout_ms < 0 ? NULL : &ts, 0, 0, 0);
}

int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    struct pollfd p[FD_SETSIZE];
    int n = 0;
    if (nfds > FD_SETSIZE)
        nfds = FD_SETSIZE;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = (short)((r && FD_ISSET(fd, r) ? POLLIN : 0) |
                           (w && FD_ISSET(fd, w) ? POLLOUT : 0));
        if (ev || (e && FD_ISSET(fd, e))) {
            p[n].fd = fd;
            p[n].events = ev;
            p[n].revents = 0;
            n++;
        }
    }
    int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int rc = poll(p, (nfds_t)n, ms);
    if (rc < 0)
        return rc;
    if (r)
        FD_ZERO(r);
    if (w)
        FD_ZERO(w);
    if (e)
        FD_ZERO(e);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (r && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(p[i].fd, r);
            count++;
        }
        if (w && (p[i].revents & (POLLOUT | POLLERR))) {
            FD_SET(p[i].fd, w);
            count++;
        }
        if (e && (p[i].revents & POLLERR)) {
            FD_SET(p[i].fd, e);
            count++;
        }
    }
    return count;
}

int inet_aton(const char *s, struct in_addr *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return 0;
        uint32_t part = 0;
        while (*s >= '0' && *s <= '9')
            part = part * 10 + (uint32_t)(*s++ - '0');
        if (part > 255)
            return 0;
        v = v << 8 | part;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    out->s_addr = htonl(v);
    return 1;
}

uint32_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr a)
{
    static char buf[16];
    uint32_t v = ntohl(a.s_addr);
    char *p = buf;
    for (int i = 3; i >= 0; i--) {
        unsigned part = (v >> (8 * i)) & 255;
        char d[3];
        int k = 0;
        do {
            d[k++] = (char)('0' + part % 10);
            part /= 10;
        } while (part);
        while (k)
            *p++ = d[--k];
        if (i)
            *p++ = '.';
    }
    *p = '\0';
    return buf;
}

int resolve_host(const char *name, struct in_addr *out)
{
    if (!strcmp(name, "localhost")) {
        out->s_addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    if (!strcmp(name, "host") || !strcmp(name, "gateway"))
        return inet_aton("10.0.2.2", out) ? 0 : -1;
    return inet_aton(name, out) ? 0 : -1;
}
