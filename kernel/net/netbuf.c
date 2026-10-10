/*
 * Packet buffers: one NB_SIZE allocation per packet, with NB_HEADROOM bytes
 * in front so each layer can prepend its header without copying. Also built
 * into the host TCP test.
 */
#include <net.h>

#ifdef NET_HOST_TEST
#include <string.h>
#else
#include <debug.h>
#include <kstring.h>
#endif

u64 nb_live;    /* allocated and not yet freed (leak checks: /proc/net/stats) */

struct netbuf *nb_alloc(void)
{
    struct netbuf *nb = kmalloc(sizeof(*nb));
    if (!nb)
        return NULL;
    nb->head = kmalloc(NB_SIZE);
    if (!nb->head) {
        kfree(nb);
        return NULL;
    }
    nb->data = nb->head + NB_HEADROOM;
    nb->len = 0;
    nb->dev = NULL;
    nb->src_ip = nb->dst_ip = 0;
    nb->src_port = 0;
    nb->proto = 0;
    nb_live++;
    return nb;
}

void nb_free(struct netbuf *nb)
{
    if (!nb)
        return;
    nb_live--;
    kfree(nb->head);
    kfree(nb);
}

void *nb_put(struct netbuf *nb, u32 n)
{
    u8 *p = nb->data + nb->len;
    if (p + n > nb->head + NB_SIZE)
        return NULL;
    nb->len += n;
    return p;
}

void *nb_push(struct netbuf *nb, u32 n)
{
    if (nb->data - n < nb->head)
        return NULL;
    nb->data -= n;
    nb->len += n;
    return nb->data;
}

void nb_pull(struct netbuf *nb, u32 n)
{
    if (n > nb->len)
        n = nb->len;
    nb->data += n;
    nb->len -= n;
}

struct netbuf *nb_clone(const struct netbuf *nb)
{
    struct netbuf *c = nb_alloc();
    if (!c)
        return NULL;
    c->data = c->head + (nb->data - nb->head);
    memcpy(c->data, nb->data, nb->len);
    c->len = nb->len;
    c->dev = nb->dev;
    c->src_ip = nb->src_ip;
    c->dst_ip = nb->dst_ip;
    c->src_port = nb->src_port;
    c->proto = nb->proto;
    return c;
}
