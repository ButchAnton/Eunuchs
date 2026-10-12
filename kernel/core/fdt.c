/*
 * Flattened device tree (DTB) parser: just enough to discover the platform.
 * Spec: https://devicetree-specification.readthedocs.io (v0.4, chapter 5).
 */
#define LOG_SUBSYS SUBSYS_BOOT
#include <debug.h>
#include <kstring.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

#define MAX_DEPTH 16

struct fdt_header {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
    u32 last_comp_version;
    u32 boot_cpuid_phys;
    u32 size_dt_strings;
    u32 size_dt_struct;
};

struct platform_info platform;

/* (by bytes, not __builtin_bswap32: without Zbb that is a libgcc call) */
static inline u32 be32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static u64 read_cells(const u8 *p, u32 cells)
{
    u64 v = 0;
    for (u32 i = 0; i < cells; i++)
        v = (v << 32) | be32(p + 4 * i);
    return v;
}

/* Properties of the node currently open at each depth. */
struct node_state {
    const char *name;
    u32 addr_cells, size_cells;     /* for this node's children */
    const u8 *reg;
    u32 reg_len;
    const u8 *compat;
    u32 compat_len;
    const char *device_type;
    u32 irq;
    bool has_irq;
};

typedef void (*fdt_visit_fn)(int depth, const char *node, const char *prop,
                             const u8 *val, u32 len, void *ctx);

/*
 * Walk the structure block. Calls visit(depth, node, NULL, ...) on node begin,
 * visit(depth, node, prop, val, len) per property, and visit(-depth-1, node, ...)
 * on node end.
 */
static int fdt_walk(uintptr_t dtb_pa, fdt_visit_fn visit, void *ctx)
{
    uintptr_t dtb = (uintptr_t)__va(dtb_pa);
    const struct fdt_header *h = (const void *)dtb;
    if (be32(&h->magic) != FDT_MAGIC)
        return -1;

    const u8 *p = (const u8 *)dtb + be32(&h->off_dt_struct);
    const u8 *end = p + be32(&h->size_dt_struct);
    const char *strings = (const char *)dtb + be32(&h->off_dt_strings);
    const char *names[MAX_DEPTH];
    int depth = -1;

    while (p < end) {
        u32 tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            p += ALIGN_UP(strlen(name) + 1, 4);
            if (++depth >= MAX_DEPTH)
                return -1;
            names[depth] = name;
            visit(depth, name, NULL, NULL, 0, ctx);
            break;
        }
        case FDT_END_NODE:
            if (depth < 0)
                return -1;
            visit(-depth - 1, names[depth], NULL, NULL, 0, ctx);
            depth--;
            break;
        case FDT_PROP: {
            u32 len = be32(p);
            u32 nameoff = be32(p + 4);
            p += 8;
            if (depth >= 0)
                visit(depth, names[depth], strings + nameoff, p, len, ctx);
            p += ALIGN_UP(len, 4);
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return 0;
        default:
            return -1;
        }
    }
    return 0;
}

static bool compat_has(const struct node_state *n, const char *want)
{
    const char *s = (const char *)n->compat;
    const char *end = s + n->compat_len;
    while (s < end) {
        if (!strcmp(s, want))
            return true;
        s += strlen(s) + 1;
    }
    return false;
}

static bool reg_first(const struct node_state *n, const struct node_state *parent,
                      u64 *base, u64 *size)
{
    u32 ac = parent->addr_cells, sc = parent->size_cells;
    if (!n->reg || n->reg_len < 4 * (ac + sc))
        return false;
    *base = read_cells(n->reg, ac);
    *size = read_cells(n->reg + 4 * ac, sc);
    return true;
}

static void set_dev(struct mmio_dev *d, const struct node_state *n, u64 base, u64 size)
{
    d->base = base;
    d->size = size;
    d->irq = n->has_irq ? n->irq : 0;
}

static void add_reserved(u64 base, u64 size)
{
    if (size && platform.nreserved < (int)ARRAY_SIZE(platform.reserved)) {
        platform.reserved[platform.nreserved].base = base;
        platform.reserved[platform.nreserved].size = size;
        platform.nreserved++;
    }
}

static void finish_node(struct node_state *n, struct node_state *parent)
{
    u64 base = 0, size = 0;
    bool has_reg = reg_first(n, parent, &base, &size);

    if (has_reg && !strcmp(parent->name, "reserved-memory")) {
        add_reserved(base, size);
        return;
    }

    if ((n->device_type && !strcmp(n->device_type, "memory")) ||
        !strncmp(n->name, "memory@", 7)) {
        if (has_reg && !platform.mem_size) {
            platform.mem_base = base;
            platform.mem_size = size;
        }
    }
    if (n->device_type && !strcmp(n->device_type, "cpu")) {
        if (has_reg && platform.ncpus < ARRAY_SIZE(platform.hartids))
            platform.hartids[platform.ncpus] = base;
        platform.ncpus++;
    }
    if (!n->compat || !has_reg)
        return;

    if (compat_has(n, "ns16550a") || compat_has(n, "ns16550")) {
        if (!platform.uart.base)
            set_dev(&platform.uart, n, base, size);
    } else if (compat_has(n, "riscv,plic0") || compat_has(n, "sifive,plic-1.0.0")) {
        set_dev(&platform.plic, n, base, size);
    } else if (compat_has(n, "riscv,clint0") || compat_has(n, "sifive,clint0")) {
        set_dev(&platform.clint, n, base, size);
    } else if (compat_has(n, "sifive,test0")) {
        set_dev(&platform.test, n, base, size);
    } else if (compat_has(n, "google,goldfish-rtc")) {
        set_dev(&platform.rtc, n, base, size);
    } else if (compat_has(n, "virtio,mmio")) {
        if (platform.nvirtio < MAX_VIRTIO)
            set_dev(&platform.virtio[platform.nvirtio++], n, base, size);
    }
}

struct parse_ctx {
    struct node_state stack[MAX_DEPTH];
};

static void parse_visit(int depth, const char *node, const char *prop,
                        const u8 *val, u32 len, void *arg)
{
    struct parse_ctx *ctx = arg;

    if (depth < 0) {                        /* node end */
        int d = -depth - 1;
        if (d > 0)
            finish_node(&ctx->stack[d], &ctx->stack[d - 1]);
        return;
    }

    struct node_state *n = &ctx->stack[depth];
    if (!prop) {                            /* node begin */
        memset(n, 0, sizeof(*n));
        n->name = node;
        n->addr_cells = 2;                  /* spec defaults */
        n->size_cells = 1;
        return;
    }

    if (!strcmp(prop, "#address-cells")) n->addr_cells = be32(val);
    else if (!strcmp(prop, "#size-cells")) n->size_cells = be32(val);
    else if (!strcmp(prop, "reg")) { n->reg = val; n->reg_len = len; }
    else if (!strcmp(prop, "compatible")) { n->compat = val; n->compat_len = len; }
    else if (!strcmp(prop, "device_type")) n->device_type = (const char *)val;
    else if (!strcmp(prop, "interrupts") && len >= 4) { n->irq = be32(val); n->has_irq = true; }
    else if (!strcmp(prop, "timebase-frequency") && len >= 4) platform.timebase = read_cells(val, len / 4);
    else if (!strcmp(prop, "bootargs") && !strcmp(node, "chosen"))
        strlcpy(platform.bootargs, (const char *)val, sizeof(platform.bootargs));
    else if (!strcmp(prop, "model") && depth == 0)
        strlcpy(platform.model, (const char *)val, sizeof(platform.model));
}

/* dtb is a physical address; read through the direct map. */
int fdt_parse(uintptr_t dtb)
{
    static struct parse_ctx ctx;
    const struct fdt_header *h = __va(dtb);

    if (!dtb || be32(&h->magic) != FDT_MAGIC)
        return -1;
    platform.dtb = dtb;
    platform.dtb_size = be32(&h->totalsize);

    /* /memreserve/ block: (address, size) u64 pairs ending with a zero pair. */
    const u8 *rsv = (const u8 *)h + be32(&h->off_mem_rsvmap);
    for (;; rsv += 16) {
        u64 base = read_cells(rsv, 2), size = read_cells(rsv + 8, 2);
        if (!base && !size)
            break;
        add_reserved(base, size);
    }
    return fdt_walk(dtb, parse_visit, &ctx);
}

/* ---- Pretty printer (monitor `dtb`) ---- */

static bool printable_strings(const u8 *val, u32 len)
{
    if (len == 0 || val[len - 1] != '\0' || val[0] == '\0')
        return false;
    for (u32 i = 0; i < len; i++) {
        if (val[i] == '\0') {
            if (i + 1 < len && val[i + 1] == '\0')
                return false;
        } else if (!kisprint(val[i])) {
            return false;
        }
    }
    return true;
}

static void dump_visit(int depth, const char *node, const char *prop,
                       const u8 *val, u32 len, void *arg)
{
    print_fn print = (print_fn)arg;

    if (depth < 0) {
        print("%*s};\n", (-depth - 1) * 2, "");
        return;
    }
    if (!prop) {
        print("%*s%s {\n", depth * 2, "", depth == 0 ? "/" : node);
        return;
    }

    print("%*s%s", (depth + 1) * 2, "", prop);
    if (len == 0) {
        print(";\n");
    } else if (printable_strings(val, len)) {
        print(" = ");
        for (const char *s = (const char *)val; s < (const char *)val + len; s += strlen(s) + 1)
            print("%s\"%s\"", s == (const char *)val ? "" : ", ", s);
        print(";\n");
    } else if (len % 4 == 0) {
        print(" = <");
        for (u32 i = 0; i < len; i += 4)
            print("%s%#x", i ? " " : "", be32(val + i));
        print(">;\n");
    } else {
        print(" = [");
        for (u32 i = 0; i < len; i++)
            print("%s%02x", i ? " " : "", val[i]);
        print("];\n");
    }
}

void fdt_dump(print_fn print)
{
    if (!platform.dtb || fdt_walk(platform.dtb, dump_visit, (void *)print))
        print("no valid device tree\n");
}

void platform_print(print_fn print)
{
    print("  model     %s\n", platform.model[0] ? platform.model : "?");
    print("  harts     %u (boot hart %lu)\n", platform.ncpus, platform.boot_hart);
    print("  memory    %#lx-%#lx (%lu MiB)\n", (unsigned long)platform.mem_base,
          (unsigned long)(platform.mem_base + platform.mem_size),
          (unsigned long)(platform.mem_size >> 20));
    print("  timebase  %lu Hz\n", (unsigned long)platform.timebase);
    print("  dtb       %#lx (%u bytes)\n", (unsigned long)platform.dtb, platform.dtb_size);
    print("  uart      %#lx irq %u\n", (unsigned long)platform.uart.base, platform.uart.irq);
    print("  plic      %#lx\n", (unsigned long)platform.plic.base);
    print("  clint     %#lx\n", (unsigned long)platform.clint.base);
    print("  rtc       %#lx irq %u\n", (unsigned long)platform.rtc.base, platform.rtc.irq);
    print("  test      %#lx\n", (unsigned long)platform.test.base);
    for (int i = 0; i < platform.nvirtio; i++)
        print("  virtio%d   %#lx irq %u\n", i, (unsigned long)platform.virtio[i].base,
              platform.virtio[i].irq);
    for (int i = 0; i < platform.nreserved; i++)
        print("  reserved  %#lx-%#lx\n", (unsigned long)platform.reserved[i].base,
              (unsigned long)(platform.reserved[i].base + platform.reserved[i].size));
}
