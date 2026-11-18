/*
 * gzip, gunzip, zcat: deflate compression (RFC 1951) in the gzip format
 * (RFC 1952), chosen by argv[0].
 *
 *   gzip [-cdfklnqtv19] [FILE...]      gunzip [-cfklqtv] [FILE...]      zcat [FILE...]
 *
 * FILE is replaced by FILE.gz (or the reverse) keeping its mode and time,
 * unless -c (to standard output) or -k (keep the input). Without files, or
 * for -, standard input goes to standard output. -l lists, -t tests.
 *
 * The compressor is LZ77 over a 32 KiB window with hash chains and lazy
 * matching (the level sets how hard it looks), emitting a dynamic Huffman
 * block every 16K symbols, or a stored block when that is smaller. The
 * decompressor streams through a 32 KiB window and handles concatenated
 * members.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *prog;
static int to_stdout, decompress, force, keep, list, no_name, quiet, test, verbose, level = 6;

/* ---- CRC-32 (the zlib one) ---- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ---- tables shared by both directions ---- */

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* ================================================================ inflate */

struct inflate {
    FILE *in;
    FILE *out;                          /* NULL: only check (-t, -l) */
    uint32_t bitbuf;
    int bitcnt;
    unsigned char window[32768];
    unsigned wpos;                      /* total bytes out, mod nothing: index & 32767 */
    unsigned long long total;
    uint32_t crc;
    int error;
};

static int in_byte(struct inflate *s)
{
    int c = getc(s->in);
    if (c == EOF) {
        s->error = 1;                   /* unexpected end of data */
        return 0;
    }
    return c;
}

static unsigned bits(struct inflate *s, int n)
{
    while (s->bitcnt < n) {
        s->bitbuf |= (uint32_t)in_byte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    unsigned v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void flush_window(struct inflate *s, unsigned upto)
{
    /* window[0..upto) is new output */
    s->crc = crc_update(s->crc, s->window, upto);
    if (s->out && upto)
        fwrite(s->window, 1, upto, s->out);
}

static void put_out(struct inflate *s, unsigned char c)
{
    s->window[s->wpos++] = c;
    s->total++;
    if (s->wpos == sizeof(s->window)) {
        flush_window(s, s->wpos);
        s->wpos = 0;
    }
}

struct huff {
    uint16_t count[16];                 /* codes of each length */
    uint16_t symbol[288];               /* symbols ordered by code */
};

/* Build a decoding table; 0, or -1 for an over-subscribed set. */
static int huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = i;
    return 0;
}

static int decode(struct inflate *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->error)
            return -1;
    }
    return -1;
}

static int inflate_codes(struct inflate *s, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0 || s->error)
            return -1;
        if (sym < 256) {
            put_out(s, sym);
            continue;
        }
        if (sym == 256)
            return 0;
        sym -= 257;
        if (sym >= 29)
            return -1;
        int len = len_base[sym] + bits(s, len_extra[sym]);
        int ds = decode(s, dist);
        if (ds < 0 || ds >= 30)
            return -1;
        unsigned d = dist_base[ds] + bits(s, dist_extra[ds]);
        if (d > s->total)
            return -1;                  /* before the start of the data */
        while (len--)
            put_out(s, s->window[(s->wpos - d) & 32767]);
    }
}

static int inflate_stored(struct inflate *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;                      /* to a byte boundary */
    unsigned len = in_byte(s);
    len |= in_byte(s) << 8;
    unsigned nlen = in_byte(s);
    nlen |= in_byte(s) << 8;
    if (s->error || len != (~nlen & 0xffff))
        return -1;
    while (len--) {
        int c = in_byte(s);
        if (s->error)
            return -1;
        put_out(s, c);
    }
    return 0;
}

static int inflate_fixed(struct inflate *s)
{
    static struct huff lit, dist;
    static int built;
    if (!built) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dist, l, 30);
        built = 1;
    }
    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(struct inflate *s)
{
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return -1;
    uint8_t lengths[320] = { 0 };
    for (int i = 0; i < ncode; i++)
        lengths[clen_order[i]] = bits(s, 3);
    struct huff lencode, lit, dist;
    if (huff_build(&lencode, lengths, 19) < 0)
        return -1;
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0 || s->error)
            return -1;
        if (sym < 16) {
            lengths[i++] = sym;
            continue;
        }
        int rep, val = 0;
        if (sym == 16) {
            if (!i)
                return -1;
            val = lengths[i - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + bits(s, 3);
        } else {
            rep = 11 + bits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = val;
    }
    if (!lengths[256])
        return -1;                      /* no end-of-block code */
    if (huff_build(&lit, lengths, nlen) < 0 || huff_build(&dist, lengths + nlen, ndist) < 0)
        return -1;
    return inflate_codes(s, &lit, &dist);
}

/* One deflate stream; 0 or -1. */
static int inflate_stream(struct inflate *s)
{
    int last;
    do {
        last = bits(s, 1);
        int type = bits(s, 2);
        int r = type == 0 ? inflate_stored(s) : type == 1 ? inflate_fixed(s) : type == 2 ? inflate_dynamic(s) : -1;
        if (r < 0 || s->error)
            return -1;
    } while (!last);
    flush_window(s, s->wpos);
    s->wpos = 0;
    return 0;
}

static uint32_t le32(const unsigned char *b)
{
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

/* Decompress every member of IN into OUT (NULL: just check). Returns 0, 1
   (error) or 2 (not gzip). *orig_name gets the first member's name. */
static int gunzip_stream(FILE *in, FILE *out, const char *name, unsigned long long *outsize,
                         char *orig_name, size_t name_size, time_t *mtime)
{
    static struct inflate s;
    int member = 0;
    unsigned long long total = 0;
    for (;; member++) {
        int c1 = getc(in);
        if (c1 == EOF && member)
            break;
        int c2 = getc(in);
        if (c1 != 0x1f || c2 != 0x8b) {
            if (member) {
                if (!quiet)
                    eprintf("%s: %s: trailing garbage ignored\n", prog, name);
                break;
            }
            eprintf("%s: %s: not in gzip format\n", prog, name);
            return 2;
        }
        unsigned char h[8];
        int method = getc(in), flags = getc(in);
        if (fread(h, 1, 6, in) != 6 || method != 8) {
            eprintf("%s: %s: unknown method or truncated header\n", prog, name);
            return 1;
        }
        if (mtime && !member)
            *mtime = le32(h);
        if (flags & 4) {                /* FEXTRA */
            int xl = getc(in);
            xl |= getc(in) << 8;
            while (xl-- > 0)
                getc(in);
        }
        size_t nl = 0;
        if (flags & 8) {                /* FNAME */
            int c;
            while ((c = getc(in)) != EOF && c)
                if (orig_name && !member && nl + 1 < name_size)
                    orig_name[nl++] = c;
        }
        if (orig_name && !member)
            orig_name[nl] = 0;
        if (flags & 16)                 /* FCOMMENT */
            while (getc(in) > 0)
                ;
        if (flags & 2) {                /* FHCRC */
            getc(in);
            getc(in);
        }
        memset(&s, 0, sizeof(s));
        s.in = in;
        s.out = out;
        if (inflate_stream(&s) < 0) {
            eprintf("%s: %s: invalid compressed data\n", prog, name);
            return 1;
        }
        unsigned char t[8];
        if (fread(t, 1, 8, in) != 8) {
            eprintf("%s: %s: unexpected end of file\n", prog, name);
            return 1;
        }
        if (le32(t) != s.crc) {
            eprintf("%s: %s: invalid compressed data--crc error\n", prog, name);
            return 1;
        }
        if (le32(t + 4) != (uint32_t)s.total) {
            eprintf("%s: %s: invalid compressed data--length error\n", prog, name);
            return 1;
        }
        total += s.total;
    }
    if (outsize)
        *outsize = total;
    return 0;
}

/* ================================================================ deflate */

#define WSIZE 32768
#define WMASK (WSIZE - 1)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)
#define MAX_DIST (WSIZE - MIN_LOOKAHEAD)
#define HBITS 15
#define HSIZE (1 << HBITS)
#define NIL 0xffff                      /* no position (positions are < 2 * WSIZE) */
#define SYMS 16384

struct deflate {
    FILE *in, *out;
    unsigned char buf[2 * WSIZE];
    unsigned lookahead_end;             /* valid bytes in buf */
    unsigned strstart, block_start;
    int eof;
    uint32_t head[HSIZE];
    uint32_t prev[WSIZE];
    /* the symbols of the current block: literal (dist 0) or (len, dist) */
    uint16_t sym_len[SYMS], sym_dist[SYMS];
    int nsyms;
    uint32_t litfreq[286], distfreq[30];
    /* bit output */
    uint32_t bitbuf;
    int bitcnt;
    unsigned long long in_total, out_total;
    uint32_t crc;
    int max_chain, good, nice, lazy;
};

static void put_byte(struct deflate *d, int c)
{
    putc(c, d->out);
    d->out_total++;
}

static void put_bits(struct deflate *d, uint32_t v, int n)
{
    d->bitbuf |= v << d->bitcnt;
    d->bitcnt += n;
    while (d->bitcnt >= 8) {
        put_byte(d, d->bitbuf & 0xff);
        d->bitbuf >>= 8;
        d->bitcnt -= 8;
    }
}

static void align_bits(struct deflate *d)
{
    if (d->bitcnt > 0)
        put_byte(d, d->bitbuf & 0xff);
    d->bitbuf = 0;
    d->bitcnt = 0;
}

/* Huffman code lengths from FREQ (n symbols), at most LIMIT bits. */
static void build_lengths(const uint32_t *freq_in, int n, int limit, uint8_t *len)
{
    uint32_t freq[288];
    memcpy(freq, freq_in, n * sizeof(uint32_t));
    for (;;) {
        /* simple O(n^2) Huffman: n is at most 286 */
        int parent[2 * 288], nodes = 0, alive[2 * 288];
        uint64_t w[2 * 288];
        int leaf_of[288];
        for (int i = 0; i < n; i++) {
            len[i] = 0;
            leaf_of[i] = -1;
            if (freq[i]) {
                leaf_of[i] = nodes;
                w[nodes] = freq[i];
                parent[nodes] = -1;
                alive[nodes++] = 1;
            }
        }
        if (nodes == 0)
            return;
        if (nodes == 1) {
            for (int i = 0; i < n; i++)
                if (leaf_of[i] >= 0)
                    len[i] = 1;
            return;
        }
        int live = nodes;
        while (live > 1) {
            int a = -1, b = -1;
            for (int k = 0; k < nodes; k++) {
                if (!alive[k])
                    continue;
                if (a < 0 || w[k] < w[a])
                    b = a, a = k;
                else if (b < 0 || w[k] < w[b])
                    b = k;
            }
            alive[a] = alive[b] = 0;
            w[nodes] = w[a] + w[b];
            parent[nodes] = -1;
            alive[nodes] = 1;
            parent[a] = parent[b] = nodes;
            nodes++;
            live--;
        }
        int maxlen = 0;
        for (int i = 0; i < n; i++) {
            if (leaf_of[i] < 0)
                continue;
            int depth = 0;
            for (int k = leaf_of[i]; parent[k] >= 0; k = parent[k])
                depth++;
            len[i] = depth;
            if (depth > maxlen)
                maxlen = depth;
        }
        if (maxlen <= limit)
            return;
        /* too deep: flatten the frequencies and try again */
        for (int i = 0; i < n; i++)
            if (freq[i])
                freq[i] = (freq[i] >> 1) | 1;
    }
}

/* Canonical codes (bit-reversed for output) from lengths. */
static void build_codes(const uint8_t *len, int n, uint16_t *code)
{
    uint16_t count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    int c = 0;
    for (int b = 1; b < 16; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (int i = 0; i < n; i++) {
        if (!len[i])
            continue;
        unsigned v = next[len[i]]++, r = 0;
        for (int k = 0; k < len[i]; k++)
            r = r << 1 | ((v >> k) & 1);
        code[i] = r;
    }
}

static int len_code(int len)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    return i;
}

static int dist_code(int dist)
{
    int i = 29;
    while (dist_base[i] > dist)
        i--;
    return i;
}

/* Emit the block of buffered symbols covering buf[block_start..strstart). */
static void flush_block(struct deflate *d, int last)
{
    memset(d->litfreq, 0, sizeof(d->litfreq));
    memset(d->distfreq, 0, sizeof(d->distfreq));
    for (int i = 0; i < d->nsyms; i++) {
        if (!d->sym_dist[i]) {
            d->litfreq[d->sym_len[i]]++;
        } else {
            d->litfreq[257 + len_code(d->sym_len[i])]++;
            d->distfreq[dist_code(d->sym_dist[i])]++;
        }
    }
    d->litfreq[256] = 1;
    /* two codes at least in each tree, for every decoder's sake */
    if (!d->litfreq[0]) d->litfreq[0] = 1;
    int nd = 0;
    for (int i = 0; i < 30; i++)
        nd += d->distfreq[i] != 0;
    if (nd < 2) {
        if (!d->distfreq[0]) d->distfreq[0] = 1;
        if (!d->distfreq[1]) d->distfreq[1] = 1;
    }
    uint8_t ll[286], dl[30];
    build_lengths(d->litfreq, 286, 15, ll);
    build_lengths(d->distfreq, 30, 15, dl);
    int nlit = 286, ndist = 30;
    while (nlit > 257 && !ll[nlit - 1])
        nlit--;
    while (ndist > 1 && !dl[ndist - 1])
        ndist--;
    /* run-length encode the lengths with 16/17/18 */
    uint8_t all[316], rle[316], rle_extra[316];
    int nall = 0, nrle = 0;
    memcpy(all, ll, nlit);
    memcpy(all + nlit, dl, ndist);
    nall = nlit + ndist;
    uint32_t cfreq[19] = { 0 };
    for (int i = 0; i < nall;) {
        int v = all[i], run = 1;
        while (i + run < nall && all[i + run] == v)
            run++;
        if (v == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11) rle[nrle] = 18, rle_extra[nrle++] = r - 11;
            else rle[nrle] = 17, rle_extra[nrle++] = r - 3;
            cfreq[rle[nrle - 1]]++;
            i += r;
        } else if (v != 0 && run >= 4) {
            rle[nrle] = v, rle_extra[nrle++] = 0;
            cfreq[v]++;
            int r = run - 1 > 6 ? 6 : run - 1;
            rle[nrle] = 16, rle_extra[nrle++] = r - 3;
            cfreq[16]++;
            i += 1 + r;
        } else {
            rle[nrle] = v, rle_extra[nrle++] = 0;
            cfreq[v]++;
            i++;
        }
    }
    uint8_t cl[19];
    build_lengths(cfreq, 19, 7, cl);
    int ncl = 19;
    while (ncl > 4 && !cl[clen_order[ncl - 1]])
        ncl--;
    /* the cost of this block as dynamic, against stored */
    unsigned long long bitsz = 3 + 5 + 5 + 4 + 3 * ncl;
    for (int i = 0; i < nrle; i++)
        bitsz += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    for (int i = 0; i < d->nsyms; i++) {
        if (!d->sym_dist[i])
            bitsz += ll[d->sym_len[i]];
        else {
            int lc = len_code(d->sym_len[i]), dc = dist_code(d->sym_dist[i]);
            bitsz += ll[257 + lc] + len_extra[lc] + dl[dc] + dist_extra[dc];
        }
    }
    bitsz += ll[256];
    unsigned raw = d->strstart - d->block_start;
    if (raw <= 65535 && (unsigned long long)raw * 8 + 3 + 32 + 7 < bitsz) {
        put_bits(d, last, 1);
        put_bits(d, 0, 2);
        align_bits(d);
        put_byte(d, raw & 0xff);
        put_byte(d, raw >> 8);
        put_byte(d, ~raw & 0xff);
        put_byte(d, (~raw >> 8) & 0xff);
        fwrite(d->buf + d->block_start, 1, raw, d->out);
        d->out_total += raw;
    } else {
        uint16_t lcode[286], dcode[30], ccode[19];
        build_codes(ll, 286, lcode);
        build_codes(dl, 30, dcode);
        build_codes(cl, 19, ccode);
        put_bits(d, last, 1);
        put_bits(d, 2, 2);
        put_bits(d, nlit - 257, 5);
        put_bits(d, ndist - 1, 5);
        put_bits(d, ncl - 4, 4);
        for (int i = 0; i < ncl; i++)
            put_bits(d, cl[clen_order[i]], 3);
        for (int i = 0; i < nrle; i++) {
            put_bits(d, ccode[rle[i]], cl[rle[i]]);
            if (rle[i] == 16) put_bits(d, rle_extra[i], 2);
            else if (rle[i] == 17) put_bits(d, rle_extra[i], 3);
            else if (rle[i] == 18) put_bits(d, rle_extra[i], 7);
        }
        for (int i = 0; i < d->nsyms; i++) {
            if (!d->sym_dist[i]) {
                put_bits(d, lcode[d->sym_len[i]], ll[d->sym_len[i]]);
                continue;
            }
            int len = d->sym_len[i], dist = d->sym_dist[i];
            int lc = len_code(len), dc = dist_code(dist);
            put_bits(d, lcode[257 + lc], ll[257 + lc]);
            if (len_extra[lc])
                put_bits(d, len - len_base[lc], len_extra[lc]);
            put_bits(d, dcode[dc], dl[dc]);
            if (dist_extra[dc])
                put_bits(d, dist - dist_base[dc], dist_extra[dc]);
        }
        put_bits(d, lcode[256], ll[256]);
    }
    d->nsyms = 0;
    d->block_start = d->strstart;
}

static void fill(struct deflate *d)
{
    while (!d->eof && d->lookahead_end < sizeof(d->buf)) {
        size_t n = fread(d->buf + d->lookahead_end, 1, sizeof(d->buf) - d->lookahead_end, d->in);
        if (n == 0) {
            d->eof = 1;
            break;
        }
        d->crc = crc_update(d->crc, d->buf + d->lookahead_end, n);
        d->in_total += n;
        d->lookahead_end += n;
    }
}

/* Move the upper half of the buffer down when the lookahead runs short. */
static void slide(struct deflate *d)
{
    if (d->strstart < 2 * WSIZE - MIN_LOOKAHEAD || d->eof)
        return;
    flush_block(d, 0);
    memmove(d->buf, d->buf + WSIZE, WSIZE);
    d->strstart -= WSIZE;
    d->block_start -= WSIZE;
    d->lookahead_end -= WSIZE;
    for (int i = 0; i < HSIZE; i++)
        d->head[i] = d->head[i] >= WSIZE && d->head[i] != NIL ? d->head[i] - WSIZE : NIL;
    for (int i = 0; i < WSIZE; i++)
        d->prev[i] = d->prev[i] >= WSIZE && d->prev[i] != NIL ? d->prev[i] - WSIZE : NIL;
    fill(d);
}

static unsigned hash3(const unsigned char *p)
{
    return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HSIZE - 1);
}

static void insert(struct deflate *d, unsigned pos)
{
    if (pos + MIN_MATCH > d->lookahead_end)
        return;
    unsigned h = hash3(d->buf + pos);
    d->prev[pos & WMASK] = d->head[h];
    d->head[h] = pos;
}

/* The longest match for buf[pos] no shorter than PREV_LEN; its distance in *dist. */
static int longest_match(struct deflate *d, unsigned pos, int prev_len, unsigned *dist)
{
    if (pos + MIN_MATCH > d->lookahead_end)
        return 0;
    int maxlen = d->lookahead_end - pos < MAX_MATCH ? (int)(d->lookahead_end - pos) : MAX_MATCH;
    int best = prev_len, chain = d->max_chain;
    if (prev_len >= d->good)
        chain >>= 2;
    unsigned cur = d->head[hash3(d->buf + pos)];
    const unsigned char *s = d->buf + pos;
    while (cur != NIL && cur < pos && pos - cur <= MAX_DIST && chain-- > 0) {
        const unsigned char *m = d->buf + cur;
        if (m[best] == s[best] && m[0] == s[0] && m[1] == s[1]) {
            int l = 2;
            while (l < maxlen && m[l] == s[l])
                l++;
            if (l > best) {
                best = l;
                *dist = pos - cur;
                if (l >= d->nice || l == maxlen)
                    break;
            }
        }
        unsigned nx = d->prev[cur & WMASK];
        if (nx == NIL || nx >= cur)
            break;
        cur = nx;
    }
    return best > prev_len ? best : 0;
}

static void emit_literal(struct deflate *d, int c)
{
    d->sym_len[d->nsyms] = c;
    d->sym_dist[d->nsyms++] = 0;
}

static void emit_match(struct deflate *d, int len, unsigned dist)
{
    d->sym_len[d->nsyms] = len;
    d->sym_dist[d->nsyms++] = dist;
}

static int deflate_file(FILE *in, FILE *out, unsigned long long *insize, unsigned long long *outsize,
                        uint32_t *crc_out)
{
    static struct deflate d;
    memset(&d, 0, sizeof(d));
    d.in = in;
    d.out = out;
    static const struct { int good, lazy, nice, chain; } cfg[10] = {
        { 0, 0, 0, 0 }, { 4, 0, 8, 4 }, { 4, 0, 16, 8 }, { 4, 0, 32, 32 }, { 4, 4, 16, 16 },
        { 8, 16, 32, 32 }, { 8, 16, 128, 128 }, { 8, 32, 128, 256 }, { 32, 128, 258, 1024 },
        { 32, 258, 258, 4096 } };
    d.good = cfg[level].good;
    d.lazy = cfg[level].lazy;
    d.nice = cfg[level].nice;
    d.max_chain = cfg[level].chain;
    for (int i = 0; i < HSIZE; i++)
        d.head[i] = NIL;
    for (int i = 0; i < WSIZE; i++)
        d.prev[i] = NIL;
    fill(&d);
    /* lazy matching: a match is taken only if the next position has no longer one */
    int prev_len = 0, match_available = 0;
    unsigned prev_dist = 0;
    while (d.strstart < d.lookahead_end) {
        unsigned dist = 0;
        int len = 0;
        if (prev_len < d.lazy || !match_available)
            len = longest_match(&d, d.strstart, prev_len < MIN_MATCH - 1 ? MIN_MATCH - 1 : prev_len, &dist);
        if (len == MIN_MATCH && dist > 4096)
            len = 0;                    /* a far 3-byte match isn't worth it */
        if (match_available && prev_len >= MIN_MATCH && len <= prev_len) {
            /* the previous position's match wins */
            emit_match(&d, prev_len, prev_dist);
            unsigned end = d.strstart - 1 + prev_len;
            insert(&d, d.strstart);
            for (unsigned p = d.strstart + 1; p < end; p++)
                insert(&d, p);
            d.strstart = end;
            match_available = 0;
            prev_len = 0;
        } else if (match_available) {
            emit_literal(&d, d.buf[d.strstart - 1]);
            insert(&d, d.strstart);
            prev_len = len;
            prev_dist = dist;
            d.strstart++;
        } else if (!d.lazy && len >= MIN_MATCH) {
            emit_match(&d, len, dist);
            for (unsigned p = d.strstart; p < d.strstart + len; p++)
                insert(&d, p);
            d.strstart += len;
        } else {
            insert(&d, d.strstart);
            match_available = 1;
            prev_len = len;
            prev_dist = dist;
            d.strstart++;
        }
        if (d.nsyms >= SYMS - 2)
            flush_block(&d, 0);
        slide(&d);
    }
    if (match_available) {
        if (prev_len >= MIN_MATCH)
            emit_match(&d, prev_len, prev_dist);
        else
            emit_literal(&d, d.buf[d.strstart - 1]);
    }
    flush_block(&d, 1);
    align_bits(&d);
    if (ferror(in))
        return -1;
    *insize = d.in_total;
    *outsize = d.out_total;
    *crc_out = d.crc;
    return 0;
}

static void put32(FILE *f, uint32_t v)
{
    putc(v & 0xff, f);
    putc(v >> 8 & 0xff, f);
    putc(v >> 16 & 0xff, f);
    putc(v >> 24, f);
}

static int gzip_stream(FILE *in, FILE *out, const char *name, time_t mtime, unsigned long long *insize,
                       unsigned long long *outsize)
{
    putc(0x1f, out);
    putc(0x8b, out);
    putc(8, out);
    putc(name && !no_name ? 8 : 0, out);
    put32(out, no_name ? 0 : (uint32_t)mtime);
    putc(level == 9 ? 2 : level == 1 ? 4 : 0, out);
    putc(3, out);                       /* Unix */
    unsigned long long hdr = 10;
    if (name && !no_name) {
        const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
        fputs(base, out);
        putc(0, out);
        hdr += strlen(base) + 1;
    }
    uint32_t crc;
    if (deflate_file(in, out, insize, outsize, &crc) < 0)
        return -1;
    put32(out, crc);
    put32(out, (uint32_t)*insize);
    *outsize += hdr + 8;
    return 0;
}

/* ================================================================ the command */

static int has_suffix(const char *name, const char *suf)
{
    size_t n = strlen(name), s = strlen(suf);
    return n > s && !strcmp(name + n - s, suf);
}

static void ratio(unsigned long long in, unsigned long long out, char *b, size_t n)
{
    if (!in)
        snprintf(b, n, "0.0%%");
    else {
        long long r = 1000 - (long long)(out * 1000 / in);
        snprintf(b, n, "%s%lld.%lld%%", r < 0 ? "-" : "", (r < 0 ? -r : r) / 10, (r < 0 ? -r : r) % 10);
    }
}

static int do_list(const char *name, FILE *in)
{
    static int header;
    struct stat st;
    long long csize = fstat(fileno(in), &st) == 0 ? (long long)st.st_size : -1;
    unsigned long long usize = 0;
    char orig[256];
    if (gunzip_stream(in, 0, name, &usize, orig, sizeof(orig), 0))
        return 1;
    if (!header++)
        printf("%19s %19s  ratio uncompressed_name\n", "compressed", "uncompressed");
    char r[16];
    ratio(usize, csize, r, sizeof(r));
    char uname[512];
    if (!strcmp(name, "-"))
        snprintf(uname, sizeof(uname), "%s", orig[0] ? orig : "stdout");
    else if (has_suffix(name, ".gz"))
        snprintf(uname, sizeof(uname), "%.*s", (int)(strlen(name) - 3), name);
    else
        snprintf(uname, sizeof(uname), "%s", name);
    printf("%19lld %19llu %6s %s\n", csize, usize, r, uname);
    return 0;
}

static int process(const char *name)
{
    int is_stdin = !strcmp(name, "-");
    FILE *in = is_stdin ? stdin : fopen(name, "r");
    if (!in) {
        eprintf("%s: %s: %s\n", prog, name, strerror(errno));
        return 1;
    }
    struct stat st;
    if (!is_stdin) {
        if (fstat(fileno(in), &st) < 0 || S_ISDIR(st.st_mode)) {
            eprintf("%s: %s is a directory -- ignored\n", prog, name);
            fclose(in);
            return 1;
        }
    }
    if (list) {
        int r = do_list(name, in);
        if (!is_stdin)
            fclose(in);
        return r;
    }
    if (test) {
        int r = gunzip_stream(in, 0, name, 0, 0, 0, 0) ? 1 : 0;
        if (!is_stdin)
            fclose(in);
        if (verbose && !r)
            eprintf("%s:\t OK\n", name);
        return r;
    }
    /* where the output goes */
    char outname[1024] = "";
    FILE *out = stdout;
    int to_file = !to_stdout && !is_stdin;
    if (to_file) {
        if (decompress) {
            if (has_suffix(name, ".gz") || has_suffix(name, "-gz") || has_suffix(name, ".z"))
                snprintf(outname, sizeof(outname), "%.*s", (int)(strlen(name) - (has_suffix(name, ".z") ? 2 : 3)), name);
            else if (has_suffix(name, ".tgz"))
                snprintf(outname, sizeof(outname), "%.*s.tar", (int)(strlen(name) - 4), name);
            else {
                eprintf("%s: %s: unknown suffix -- ignored\n", prog, name);
                fclose(in);
                return 1;
            }
        } else {
            if (has_suffix(name, ".gz") || has_suffix(name, ".tgz")) {
                eprintf("%s: %s already has .gz suffix -- unchanged\n", prog, name);
                fclose(in);
                return 1;
            }
            snprintf(outname, sizeof(outname), "%s.gz", name);
        }
        struct stat ost;
        if (!force && stat(outname, &ost) == 0) {
            eprintf("%s: %s already exists; not overwritten (use -f)\n", prog, outname);
            fclose(in);
            return 1;
        }
        if (!(out = fopen(outname, "w"))) {
            eprintf("%s: %s: %s\n", prog, outname, strerror(errno));
            fclose(in);
            return 1;
        }
    } else if (!decompress && !force && isatty(fileno(stdout))) {
        eprintf("%s: compressed data not written to a terminal. Use -f to force compression.\n", prog);
        if (!is_stdin)
            fclose(in);
        return 1;
    }
    int r;
    unsigned long long isz = 0, osz = 0;
    if (decompress) {
        r = gunzip_stream(in, out, name, &osz, 0, 0, 0);
        isz = is_stdin ? 0 : (unsigned long long)st.st_size;
    } else {
        r = gzip_stream(in, out, is_stdin ? 0 : name, is_stdin ? time(0) : st.st_mtime, &isz, &osz);
        if (r < 0)
            eprintf("%s: %s: read error\n", prog, name);
    }
    if (!is_stdin)
        fclose(in);
    if (fflush(out) != 0 || ferror(out)) {
        eprintf("%s: write error: %s\n", prog, strerror(errno));
        r = 1;
    }
    if (to_file) {
        fclose(out);
        if (r) {
            unlink(outname);
            return 1;
        }
        chmod(outname, st.st_mode & 07777);
        struct timespec ts[2] = { st.st_mtim, st.st_mtim };
        utimensat(AT_FDCWD, outname, ts, 0);
        if (getuid() == 0)
            chown(outname, st.st_uid, st.st_gid);
        if (!keep)
            unlink(name);
    }
    if (verbose && !r) {
        char rb[16];
        if (decompress)
            ratio(osz, isz, rb, sizeof(rb));
        else
            ratio(isz, osz, rb, sizeof(rb));
        eprintf("%s:\t%s%s%s\n", name, rb, to_file ? " -- replaced with " : "", to_file ? outname : "");
    }
    return r ? 1 : 0;
}

int main(int argc, char **argv)
{
    prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(prog, "gunzip"))
        decompress = 1;
    else if (!strcmp(prog, "zcat"))
        decompress = to_stdout = 1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--stdout")) { to_stdout = 1; continue; }
        if (!strcmp(argv[i], "--decompress")) { decompress = 1; continue; }
        if (!strcmp(argv[i], "--keep")) { keep = 1; continue; }
        if (!strcmp(argv[i], "--force")) { force = 1; continue; }
        for (char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'c': to_stdout = 1; break;
            case 'd': decompress = 1; break;
            case 'f': force = 1; break;
            case 'k': keep = 1; break;
            case 'l': list = 1; break;
            case 'n': no_name = 1; break;
            case 'N': no_name = 0; break;
            case 'q': quiet = 1; break;
            case 't': test = 1; break;
            case 'v': verbose = 1; break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                level = *f - '0';
                break;
            case 'h':
            default:
                eprintf("usage: %s [-cdfklnqtv19] [FILE...]\n", prog);
                return 1;
            }
        }
    }
    crc_init();
    if (i >= argc)
        return process("-");
    int rc = 0;
    for (; i < argc; i++)
        rc |= process(argv[i]);
    return rc;
}
