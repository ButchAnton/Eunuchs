/*
 * The Internet checksum (RFC 1071): the one's complement of the one's
 * complement sum of 16-bit big-endian words. Also built into the host tests.
 */
#include <net.h>

u32 cksum_add(u32 sum, const void *data, u32 len)
{
    const u8 *p = data;
    while (len > 1) {
        sum += (u32)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;          /* odd byte: pad with zero */
    return sum;
}

u16 cksum_finish(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return hton16((u16)~sum);           /* ready to store in a header */
}

u16 inet_cksum(const void *data, u32 len)
{
    return cksum_finish(cksum_add(0, data, len));
}

u16 l4_cksum(u32 src, u32 dst, u8 proto, const void *seg, u32 len)
{
    u32 sum = 0;
    sum += src >> 16;
    sum += src & 0xffff;
    sum += dst >> 16;
    sum += dst & 0xffff;
    sum += proto;
    sum += len;
    return cksum_finish(cksum_add(sum, seg, len));
}
