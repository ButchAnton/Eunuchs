/*
 * eboot: the Eunuchs boot loader.
 *
 * QEMU loads it (the -kernel of make run and make test) and OpenSBI enters
 * it in S-mode with the MMU off: a0 = hart id, a1 = the device tree. It reads
 * the virtio disks' partition tables (GPT, kernel/include/gpt_format.h) and
 * reads /kernel.elf from the efs partition named "boot", loads the segments
 * at their physical addresses and jumps to the entry with a0 and a1 as they
 * came: the kernel can't tell it from QEMU loading it. If that kernel is
 * missing or bad, it tries /kernel.elf on "boot.old"; if neither works it
 * says why and powers off. (A disk without boot partitions, such as a bare
 * efs image: /boot/kernel.elf, then /boot/kernel.old.elf, on the root volume,
 * the one labelled "root", else the first that is not a boot slot.)
 *
 * So Eunuchs can run a kernel it built: install it in boot (the kernel there
 * moving to boot.old: kinstall, or eudisk kernel install on the Mac) and
 * reboot.
 *
 * Everything polls and runs at physical addresses. eboot lives at 0x82000000
 * (eboot.ld); a kernel must fit between 0x80200000 (after OpenSBI) and that.
 * Output goes through the SBI console. Reads honour a committed transaction
 * still in the efs log, as the kernel's recovery would replay it.
 */
#include <efs_format.h>
#include <gpt_format.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define KERNEL_MIN 0x80200000UL     /* OpenSBI is below */

extern char __eboot_start[], __eboot_end[];

/* ---- the SBI console, memory, strings ---- */

static long sbi(long ext, long fid, long arg0, long arg1)
{
    register long a0 asm("a0") = arg0;
    register long a1 asm("a1") = arg1;
    register long a6 asm("a6") = fid;
    register long a7 asm("a7") = ext;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    return a0;
}

static void putc_(int c)
{
    sbi(1, 0, c, 0);            /* legacy console putchar */
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_('\r');
        putc_(*s++);
    }
}

static void putnum(u64 v, int base)
{
    char buf[24];
    int n = 0;
    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (base == 16)
        puts_("0x");
    while (n)
        putc_(buf[--n]);
}

static __attribute__((noreturn)) void power_off(void)
{
    puts_("eboot: powering off\n");
    sbi(0x53525354, 0, 0, 1);   /* SRST: shutdown, reason: system failure */
    for (;;)
        asm volatile("wfi");
}

void *memset(void *d, int c, size_t n)
{
    u8 *p = d;
    while (n--)
        *p++ = c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

/* (tcc copies structures with memmove) */
void *memmove(void *d, const void *s, size_t n)
{
    u8 *p = d;
    const u8 *q = s;
    if (p < q) {
        while (n--)
            *p++ = *q++;
    } else {
        while (n--)
            p[n] = q[n];
    }
    return d;
}

/* The volume label (16 bytes, NUL-padded unless all 16 are used) is `want`? */
static int label_is(const char *label, const char *want)
{
    int i = 0;
    for (; i < 16 && want[i]; i++)
        if (label[i] != want[i])
            return 0;
    return !want[i] && (i == 16 || !label[i]);
}

static int memeq(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    while (n--)
        if (*p++ != *q++)
            return 0;
    return 1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ---- the device tree: where the virtio devices are ---- */

#define FDT_MAGIC 0xd00dfeed
#define MAX_VIRTIO 16

static u32 be32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

/* The base address of every node compatible with "virtio,mmio", in tree
   order. Assumes 2 address cells (as under QEMU virt's /soc). */
static int virtio_devices(const void *dtb, uintptr_t *out, int max)
{
    const u8 *base = dtb;
    if (be32(base) != FDT_MAGIC)
        return 0;
    const u8 *p = base + be32(base + 8);
    const char *strings = (const char *)base + be32(base + 12);
    int n = 0, is_virtio = 0;
    uintptr_t reg = 0;
    for (;;) {
        u32 tok = be32(p);
        p += 4;
        if (tok == 1) {                         /* begin node: its name */
            while (*p)
                p++;
            p = base + (((p + 1) - base + 3) & ~3);
            is_virtio = 0;
            reg = 0;
        } else if (tok == 3) {                  /* property */
            u32 len = be32(p), nameoff = be32(p + 4);
            const u8 *v = p + 8;
            const char *name = strings + nameoff;
            if (streq(name, "compatible")) {
                for (u32 i = 0; i < len; ) {
                    if (streq((const char *)v + i, "virtio,mmio"))
                        is_virtio = 1;
                    while (i < len && v[i])
                        i++;
                    i++;
                }
            } else if (streq(name, "reg") && len >= 8) {
                reg = (uintptr_t)((u64)be32(v) << 32 | be32(v + 4));
            }
            p = base + ((v + len - base + 3) & ~3);
        } else if (tok == 2) {                  /* end node */
            if (is_virtio && reg && n < max)
                out[n++] = reg;
            is_virtio = 0;
        } else if (tok == 4) {                  /* nop */
            continue;
        } else {                                /* end (9), or garbage */
            break;
        }
    }
    return n;
}

/* ---- virtio-blk over MMIO (version 2), one request at a time, polled ---- */

#define R(off) (*(volatile u32 *)(dev + (off)))
enum {
    MAGIC = 0x000, VERSION = 0x004, DEVICE_ID = 0x008, DEV_FEATURES = 0x010, DEV_FEATURES_SEL = 0x014,
    DRV_FEATURES = 0x020, DRV_FEATURES_SEL = 0x024, QUEUE_SEL = 0x030, QUEUE_NUM_MAX = 0x034,
    QUEUE_NUM = 0x038, QUEUE_READY = 0x044, QUEUE_NOTIFY = 0x050, INT_STATUS = 0x060, INT_ACK = 0x064,
    STATUS = 0x070, QUEUE_DESC = 0x080, QUEUE_DRIVER = 0x090, QUEUE_DEVICE = 0x0a0,
};
enum { S_ACK = 1, S_DRIVER = 2, S_DRIVER_OK = 4, S_FEATURES_OK = 8 };
#define QN 4

struct desc {
    u64 addr;
    u32 len;
    u16 flags, next;
};

static struct {
    struct desc desc[QN];
    struct { u16 flags, idx, ring[QN], used_event; } avail;
    u8 pad[4096 - QN * 16 - (6 + 2 * QN)];
    struct { u16 flags, idx; struct { u32 id, len; } ring[QN]; u16 avail_event; } used;
} vq __attribute__((aligned(4096)));

static struct { u32 type, reserved; u64 sector; } req;
static volatile u8 req_status;
static u16 used_seen;
static uintptr_t dev;               /* the disk in use */
static u64 vol_base;                /* the first sector of the volume in use */

static void blk_reset(uintptr_t d)
{
    *(volatile u32 *)(d + STATUS) = 0;
}

/* Set up device d for block reads; 0 if it is a version-2 virtio disk. */
static int blk_init(uintptr_t d)
{
    dev = d;
    if (R(MAGIC) != 0x74726976 || R(DEVICE_ID) != 2)
        return -1;
    if (R(VERSION) != 2) {
        puts_("eboot: legacy virtio-mmio device at ");
        putnum(d, 16);
        puts_(" (QEMU needs -global virtio-mmio.force-legacy=false)\n");
        return -1;
    }
    R(STATUS) = 0;
    R(STATUS) = S_ACK;
    R(STATUS) = S_ACK | S_DRIVER;
    R(DRV_FEATURES_SEL) = 0;
    R(DRV_FEATURES) = 0;
    R(DRV_FEATURES_SEL) = 1;
    R(DRV_FEATURES) = 1;                    /* VIRTIO_F_VERSION_1 (bit 32) */
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK;
    if (!(R(STATUS) & S_FEATURES_OK))
        return -1;
    R(QUEUE_SEL) = 0;
    if (R(QUEUE_NUM_MAX) < QN)
        return -1;
    memset(&vq, 0, sizeof(vq));
    used_seen = 0;
    R(QUEUE_NUM) = QN;
    R(QUEUE_DESC) = (u32)(uintptr_t)vq.desc;
    R(QUEUE_DESC + 4) = (u32)((uintptr_t)vq.desc >> 32);
    R(QUEUE_DRIVER) = (u32)(uintptr_t)&vq.avail;
    R(QUEUE_DRIVER + 4) = (u32)((uintptr_t)&vq.avail >> 32);
    R(QUEUE_DEVICE) = (u32)(uintptr_t)&vq.used;
    R(QUEUE_DEVICE + 4) = (u32)((uintptr_t)&vq.used >> 32);
    R(QUEUE_READY) = 1;
    R(STATUS) = S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK;
    return 0;
}

/* Read `count` 512-byte sectors at `sector` into buf. */
static int blk_read(u64 sector, void *buf, u32 count)
{
    req.type = 0;                           /* VIRTIO_BLK_T_IN */
    req.reserved = 0;
    req.sector = sector;
    req_status = 0xff;
    vq.desc[0] = (struct desc){ (uintptr_t)&req, sizeof(req), 1, 1 };            /* NEXT */
    vq.desc[1] = (struct desc){ (uintptr_t)buf, count * 512, 1 | 2, 2 };         /* NEXT|WRITE */
    vq.desc[2] = (struct desc){ (uintptr_t)&req_status, 1, 2, 0 };               /* WRITE */
    vq.avail.ring[vq.avail.idx % QN] = 0;
    asm volatile("fence rw, rw" ::: "memory");
    vq.avail.idx++;
    asm volatile("fence rw, rw" ::: "memory");
    R(QUEUE_NOTIFY) = 0;
    while (*(volatile u16 *)&vq.used.idx == used_seen)
        ;
    asm volatile("fence rw, rw" ::: "memory");
    used_seen++;
    R(INT_ACK) = R(INT_STATUS);
    return req_status == 0 ? 0 : -1;
}

/* ---- efs, read-only ---- */

static u8 sbuf[EFS_BSIZE] __attribute__((aligned(16)));
static struct efs_super sb;
static struct efs_logheader logh;   /* n > 0: a committed transaction not yet installed */

/* Block b of the filesystem: its copy in the log if a committed transaction
   holds it (the last one), else its home. */
static int read_block(u32 b, void *buf)
{
    u32 from = b;
    for (u32 i = 0; i < logh.n && i < EFS_LOGSIZE; i++)
        if (logh.blocks[i] == b)
            from = sb.logstart + 1 + i;
    return blk_read(vol_base + (u64)from * EFS_SECTORS_PER_BLOCK, buf, EFS_SECTORS_PER_BLOCK);
}

/* The efs superblock and log of the volume at sector base of the current
   disk; 0 if there is an efs. */
static int efs_open(u64 base)
{
    logh.n = 0;
    vol_base = base;
    if (blk_read(base + 1 * EFS_SECTORS_PER_BLOCK, sbuf, EFS_SECTORS_PER_BLOCK))
        return -1;
    memcpy(&sb, sbuf, sizeof(sb));
    if (sb.magic != EFS_MAGIC || sb.bsize != EFS_BSIZE)
        return -1;
    if (blk_read(base + (u64)sb.logstart * EFS_SECTORS_PER_BLOCK, sbuf, EFS_SECTORS_PER_BLOCK))
        return -1;
    memcpy(&logh, sbuf, sizeof(logh));
    if (logh.magic != EFS_LOG_MAGIC)
        logh.n = 0;
    return 0;
}

static int read_inode(u32 ino, struct efs_dinode *d)
{
    if (ino == 0 || ino >= sb.ninodes || read_block(sb.inodestart + ino / EFS_IPB, sbuf))
        return -1;
    memcpy(d, sbuf + (ino % EFS_IPB) * sizeof(*d), sizeof(*d));
    return d->type == EFS_T_FREE ? -1 : 0;
}

/* The disk block holding file block fbn (0: a hole, or past the end). */
static u32 bmap(const struct efs_dinode *d, u32 fbn)
{
    static u32 ind[EFS_NINDIRECT];
    if (fbn < EFS_NDIRECT)
        return d->addrs[fbn];
    fbn -= EFS_NDIRECT;
    if (fbn < EFS_NINDIRECT) {
        if (!d->addrs[EFS_NDIRECT] || read_block(d->addrs[EFS_NDIRECT], ind))
            return 0;
        return ind[fbn];
    }
    fbn -= EFS_NINDIRECT;
    if (fbn >= (u32)EFS_NINDIRECT * EFS_NINDIRECT || !d->addrs[EFS_NDIRECT + 1] ||
        read_block(d->addrs[EFS_NDIRECT + 1], ind))
        return 0;
    u32 b = ind[fbn / EFS_NINDIRECT];
    if (!b || read_block(b, ind))
        return 0;
    return ind[fbn % EFS_NINDIRECT];
}

/* Read len bytes at off of a file into dst (anywhere in physical memory). */
static int file_read(const struct efs_dinode *d, u64 off, void *dst, u64 len)
{
    u8 *out = dst;
    if (len > d->size || off > d->size - len)      /* (no wrap-around) */
        return -1;
    while (len) {
        u32 fbn = off / EFS_BSIZE, in = off % EFS_BSIZE;
        u64 n = EFS_BSIZE - in < len ? EFS_BSIZE - in : len;
        u32 b = bmap(d, fbn);
        if (!b)
            memset(out, 0, n);              /* a hole */
        else if (read_block(b, sbuf))
            return -1;
        else
            memcpy(out, sbuf + in, n);
        out += n;
        off += n;
        len -= n;
    }
    return 0;
}

/* The inode of an absolute path, or 0. */
static u32 lookup(const char *path, struct efs_dinode *d)
{
    u32 ino = EFS_ROOT_INO;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        char name[EFS_NAMELEN + 1];
        int n = 0;
        while (*path && *path != '/') {
            if (n == EFS_NAMELEN)
                return 0;                   /* longer than any efs name */
            name[n++] = *path++;
        }
        name[n] = 0;
        if (read_inode(ino, d) || d->type != EFS_T_DIR)
            return 0;
        u32 next = 0;
        for (u64 off = 0; off < d->size && !next; off += sizeof(struct efs_dirent)) {
            struct efs_dirent de;
            if (file_read(d, off, &de, sizeof(de)))
                return 0;
            de.name[EFS_NAMELEN] = 0;
            if (de.ino && streq(de.name, name))
                next = de.ino;
        }
        if (!next)
            return 0;
        ino = next;
    }
    return read_inode(ino, d) ? 0 : ino;
}

/* ---- loading an ELF kernel ---- */

struct ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

#define MAX_PHDRS 16

static int bad(const char *path, const char *why)
{
    puts_("eboot: ");
    puts_(path);
    puts_(": ");
    puts_(why);
    puts_("\n");
    return -1;
}

/* Load the kernel in `file` (called `path` in messages); its physical entry
   address in *entry. */
static int load_kernel(const char *file, const char *path, uintptr_t *entry)
{
    struct efs_dinode d;
    struct ehdr eh;
    struct phdr ph[MAX_PHDRS];
    if (!lookup(file, &d))
        return bad(path, "not found");
    if (d.type != EFS_T_FILE)
        return bad(path, "not a file");
    if (file_read(&d, 0, &eh, sizeof(eh)) || eh.ident[0] != 0x7f || eh.ident[1] != 'E' ||
        eh.ident[2] != 'L' || eh.ident[3] != 'F' || eh.ident[4] != 2 || eh.machine != 243 ||
        eh.type != 2)
        return bad(path, "not a RISC-V 64-bit ELF executable");
    if (eh.phnum > MAX_PHDRS || eh.phentsize != sizeof(struct phdr) ||
        file_read(&d, eh.phoff, ph, eh.phnum * sizeof(struct phdr)))
        return bad(path, "bad program headers");
    /* check everything before touching memory */
    uintptr_t lo = (uintptr_t)__eboot_start;
    int loads = 0;
    *entry = 0;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != 1 || !ph[i].memsz)        /* PT_LOAD */
            continue;
        /* (as subtractions after the bounds, so nothing wraps around) */
        if (ph[i].paddr < KERNEL_MIN || ph[i].paddr >= lo || ph[i].memsz > lo - ph[i].paddr ||
            ph[i].filesz > ph[i].memsz || ph[i].filesz > d.size || ph[i].offset > d.size - ph[i].filesz) {
            puts_("eboot: segment at ");
            putnum(ph[i].paddr, 16);
            puts_(" (");
            putnum(ph[i].memsz, 10);
            puts_(" bytes) is outside ");
            putnum(KERNEL_MIN, 16);
            puts_("..");
            putnum(lo, 16);
            puts_("\n");
            return bad(path, "does not fit below the boot loader");
        }
        if (eh.entry >= ph[i].vaddr && eh.entry - ph[i].vaddr < ph[i].memsz && (ph[i].flags & 1))
            *entry = eh.entry - ph[i].vaddr + ph[i].paddr;
        loads++;
    }
    if (!loads || !*entry)
        return bad(path, "no loadable code, or the entry point is not in it");
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != 1 || !ph[i].memsz)
            continue;
        if (file_read(&d, ph[i].offset, (void *)(uintptr_t)ph[i].paddr, ph[i].filesz))
            return bad(path, "read error");
        memset((void *)(uintptr_t)(ph[i].paddr + ph[i].filesz), 0, ph[i].memsz - ph[i].filesz);
    }
    puts_("eboot: loaded ");
    puts_(path);
    puts_(" (");
    putnum(d.size / 1024, 10);
    puts_(" KiB), entry ");
    putnum(*entry, 16);
    puts_("\n");
    return 0;
}

/* ---- volumes: GPT partitions, or whole disks ---- */

#define MAX_VOLS 16

static struct vol {
    uintptr_t dev;
    u64 base;                       /* first sector */
    char name[GPT_NAME_LEN + 1];    /* the GPT partition name; "" for a whole disk */
    char label[17];                 /* the efs volume label */
} vols[MAX_VOLS];
static int nvols;

static u8 gptbuf[GPT_NENTRIES * GPT_ENTRY_SIZE] __attribute__((aligned(16)));

static void add_vol(uintptr_t d, u64 base, const char *name)
{
    if (nvols == MAX_VOLS || efs_open(base))
        return;                     /* not efs: nothing to boot from */
    struct vol *v = &vols[nvols++];
    v->dev = d;
    v->base = base;
    int i = 0;
    for (; name[i] && i < GPT_NAME_LEN; i++)
        v->name[i] = name[i];
    v->name[i] = 0;
    memcpy(v->label, sb.label, 16);
    v->label[16] = 0;
}

/* The efs volumes of the current disk: its GPT partitions (from the primary
   table; QEMU's disks are never damaged halfway) or the whole disk. */
static void scan_disk(uintptr_t d)
{
    struct gpt_header h;
    if (blk_read(1, sbuf, 1) == 0) {
        memcpy(&h, sbuf, sizeof(h));
        u32 want = h.header_crc;
        h.header_crc = 0;
        if (h.signature == GPT_SIGNATURE && h.header_size >= GPT_HEADER_SIZE && h.entry_size == GPT_ENTRY_SIZE &&
            h.num_entries && h.num_entries <= GPT_NENTRIES && gpt_crc32(&h, GPT_HEADER_SIZE) == want &&
            blk_read(h.entries_lba, gptbuf, GPT_ENTRY_SECTORS) == 0 &&
            gpt_crc32(gptbuf, h.num_entries * GPT_ENTRY_SIZE) == h.entries_crc) {
            static const u8 unused[16];
            for (u32 i = 0; i < h.num_entries; i++) {
                const struct gpt_entry *e = (const void *)(gptbuf + i * GPT_ENTRY_SIZE);
                if (memeq(e->type, unused, 16) || e->last_lba < e->first_lba)
                    continue;
                char name[GPT_NAME_LEN + 1];
                gpt_name(e, name);
                add_vol(d, e->first_lba, name);
            }
            return;
        }
    }
    add_vol(d, 0, "");
}

/* The volume called `want`: by GPT name, else by efs label. */
static struct vol *find_vol(const char *want)
{
    for (int i = 0; i < nvols; i++)
        if (streq(vols[i].name, want))
            return &vols[i];
    for (int i = 0; i < nvols; i++)
        if (label_is(vols[i].label, want))
            return &vols[i];
    return NULL;
}

static int is_boot_vol(const struct vol *v)
{
    return streq(v->name, "boot") || streq(v->name, "boot.old") || label_is(v->label, "boot") ||
           label_is(v->label, "boot.old");
}

/* ---- main ---- */

static const struct {
    const char *vol;                /* (NULL: the root volume) */
    const char *path, *shown;
} kernels[] = {
    { "boot", "/kernel.elf", "boot:/kernel.elf" },
    { "boot.old", "/kernel.elf", "boot.old:/kernel.elf" },
    { NULL, "/boot/kernel.elf", "/boot/kernel.elf" },          /* no boot partitions */
    { NULL, "/boot/kernel.old.elf", "/boot/kernel.old.elf" },
};

__attribute__((noreturn)) void eboot_main(long hartid, void *dtb)
{
    uintptr_t devs[MAX_VIRTIO];
    puts_("eboot: Eunuchs boot loader\n");
    int n = virtio_devices(dtb, devs, MAX_VIRTIO);
    for (int i = 0; i < n; i++) {
        if (blk_init(devs[i]))
            continue;
        scan_disk(devs[i]);
        blk_reset(devs[i]);
    }
    if (!nvols) {
        puts_("eboot: no disk with an efs filesystem\n");
        power_off();
    }
    struct vol *root = find_vol("root");
    for (int i = 0; i < nvols && !root; i++)        /* else the first that is not a boot slot */
        if (!is_boot_vol(&vols[i]))
            root = &vols[i];
    if (!root)
        root = &vols[0];
    int have_boot = find_vol("boot") || find_vol("boot.old");
    for (unsigned k = 0; k < sizeof(kernels) / sizeof(kernels[0]); k++) {
        struct vol *v = kernels[k].vol ? find_vol(kernels[k].vol) : have_boot ? NULL : root;
        if (!v) {
            if (kernels[k].vol && have_boot) {
                puts_("eboot: ");
                puts_(kernels[k].shown);
                puts_(": no such partition\n");
            }
            continue;
        }
        blk_init(v->dev);
        efs_open(v->base);
        if (logh.n) {
            puts_("eboot: ");
            puts_(v->name[0] ? v->name : v->label);
            puts_(": a committed transaction in the log: read through it\n");
        }
        uintptr_t entry = 0;
        int r = load_kernel(kernels[k].path, kernels[k].shown, &entry);
        blk_reset(v->dev);                  /* the kernel finds the disk as QEMU left it */
        if (r)
            continue;
        asm volatile("fence.i" ::: "memory");
        ((void (*)(long, void *))entry)(hartid, dtb);
    }
    puts_("eboot: no kernel to boot\n");
    power_off();
}
