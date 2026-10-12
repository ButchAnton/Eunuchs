/*
 * db: DWARF 4 and 5, as gcc and tcc write it.
 *
 * Read once at startup: the compile units, a function index (subprograms
 * with addresses), the global variables, and the line tables (rows grouped
 * in sequences, sorted by address). Read on demand: types (cached by DIE
 * offset), the variables of a function's scopes, and location lists.
 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tags */
#define TAG_array_type 0x01
#define TAG_enumeration_type 0x04
#define TAG_formal_parameter 0x05
#define TAG_lexical_block 0x0b
#define TAG_member 0x0d
#define TAG_pointer_type 0x0f
#define TAG_compile_unit 0x11
#define TAG_structure_type 0x13
#define TAG_subroutine_type 0x15
#define TAG_typedef 0x16
#define TAG_union_type 0x17
#define TAG_inlined_subroutine 0x1d
#define TAG_subrange_type 0x21
#define TAG_base_type 0x24
#define TAG_const_type 0x26
#define TAG_enumerator 0x28
#define TAG_subprogram 0x2e
#define TAG_variable 0x34
#define TAG_volatile_type 0x35
#define TAG_restrict_type 0x37
#define TAG_atomic_type 0x47
#define TAG_skeleton_unit 0x4a
#define TAG_partial_unit 0x3c

/* attributes */
#define AT_sibling 0x01
#define AT_location 0x02
#define AT_name 0x03
#define AT_byte_size 0x0b
#define AT_bit_offset 0x0c
#define AT_bit_size 0x0d
#define AT_stmt_list 0x10
#define AT_low_pc 0x11
#define AT_high_pc 0x12
#define AT_comp_dir 0x1b
#define AT_producer 0x25
#define AT_const_value 0x1c
#define AT_upper_bound 0x2f
#define AT_abstract_origin 0x31
#define AT_count 0x37
#define AT_data_member_location 0x38
#define AT_declaration 0x3c
#define AT_encoding 0x3e
#define AT_external 0x3f
#define AT_frame_base 0x40
#define AT_specification 0x47
#define AT_type 0x49
#define AT_ranges 0x55
#define AT_data_bit_offset 0x6b
#define AT_str_offsets_base 0x72
#define AT_addr_base 0x73
#define AT_rnglists_base 0x74
#define AT_loclists_base 0x8c

/* forms */
#define FORM_addr 0x01
#define FORM_block2 0x03
#define FORM_block4 0x04
#define FORM_data2 0x05
#define FORM_data4 0x06
#define FORM_data8 0x07
#define FORM_string 0x08
#define FORM_block 0x09
#define FORM_block1 0x0a
#define FORM_data1 0x0b
#define FORM_flag 0x0c
#define FORM_sdata 0x0d
#define FORM_strp 0x0e
#define FORM_udata 0x0f
#define FORM_ref_addr 0x10
#define FORM_ref1 0x11
#define FORM_ref2 0x12
#define FORM_ref4 0x13
#define FORM_ref8 0x14
#define FORM_ref_udata 0x15
#define FORM_indirect 0x16
#define FORM_sec_offset 0x17
#define FORM_exprloc 0x18
#define FORM_flag_present 0x19
#define FORM_strx 0x1a
#define FORM_addrx 0x1b
#define FORM_ref_sup4 0x1c
#define FORM_strp_sup 0x1d
#define FORM_data16 0x1e
#define FORM_line_strp 0x1f
#define FORM_ref_sig8 0x20
#define FORM_implicit_const 0x21
#define FORM_loclistx 0x22
#define FORM_rnglistx 0x23
#define FORM_ref_sup8 0x24
#define FORM_strx1 0x25
#define FORM_strx2 0x26
#define FORM_strx3 0x27
#define FORM_strx4 0x28
#define FORM_addrx1 0x29
#define FORM_addrx2 0x2a
#define FORM_addrx3 0x2b
#define FORM_addrx4 0x2c

/* ---- reading ---- */

struct rd {
    const u8 *p, *end;
    int bad;
};

static u64 rd_n(struct rd *r, int n)
{
    if (r->p + n > r->end) {
        r->bad = 1;
        r->p = r->end;
        return 0;
    }
    u64 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u64)r->p[i] << (8 * i);
    r->p += n;
    return v;
}

static u64 rd_uleb(struct rd *r)
{
    u64 v = 0;
    int shift = 0;
    while (r->p < r->end) {
        u8 b = *r->p++;
        if (shift < 64)
            v |= (u64)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            return v;
    }
    r->bad = 1;
    return v;
}

static i64 rd_sleb(struct rd *r)
{
    i64 v = 0;
    int shift = 0;
    u8 b = 0;
    while (r->p < r->end) {
        b = *r->p++;
        if (shift < 64)
            v |= (i64)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift < 64 && (b & 0x40))
        v |= -((i64)1 << shift);
    return v;
}

static const char *rd_str(struct rd *r)
{
    const char *s = (const char *)r->p;
    while (r->p < r->end && *r->p)
        r->p++;
    if (r->p < r->end)
        r->p++;
    else
        r->bad = 1;
    return s;
}

static const char *sec_str(struct section *s, u64 off)
{
    if (!s->data || off >= s->size)
        return "?";
    return (const char *)s->data + off;
}

/* ---- abbreviations ---- */

struct attrspec {
    u32 at, form;
    i64 implicit;
};

struct abbrev {
    u64 code;
    u32 tag;
    u8 children;
    int nattrs;
    struct attrspec *attrs;
};

struct abbrev_table {
    u64 offset;
    struct abbrev *v;           /* indexed by code when codes are small */
    int n;
    struct abbrev_table *next;
};

static struct abbrev_table *abbrev_tables;

static struct abbrev_table *abbrevs_at(u64 off)
{
    for (struct abbrev_table *t = abbrev_tables; t; t = t->next)
        if (t->offset == off)
            return t;
    struct abbrev_table *t = xcalloc(1, sizeof(*t));
    t->offset = off;
    struct rd r = { prog.debug_abbrev.data + off, prog.debug_abbrev.data + prog.debug_abbrev.size, 0 };
    if (off >= prog.debug_abbrev.size)
        r.p = r.end;
    for (;;) {
        u64 code = rd_uleb(&r);
        if (!code || r.bad)
            break;
        struct abbrev a = { code, (u32)rd_uleb(&r), (u8)rd_n(&r, 1), 0, 0 };
        int cap = 0;
        for (;;) {
            u32 at = rd_uleb(&r), form = rd_uleb(&r);
            i64 imp = form == FORM_implicit_const ? rd_sleb(&r) : 0;
            if ((!at && !form) || r.bad)
                break;
            if (a.nattrs == cap) {
                cap = cap ? cap * 2 : 8;
                a.attrs = xrealloc(a.attrs, cap * sizeof(struct attrspec));
            }
            a.attrs[a.nattrs++] = (struct attrspec){ at, form, imp };
        }
        if (code >= (u64)t->n) {
            int n = code + 16;
            t->v = xrealloc(t->v, n * sizeof(struct abbrev));
            memset(t->v + t->n, 0, (n - t->n) * sizeof(struct abbrev));
            t->n = n;
        }
        if (code < 100000)
            t->v[code] = a;
    }
    t->next = abbrev_tables;
    abbrev_tables = t;
    return t;
}

/* ---- compile units ---- */

struct cu {
    u64 off, end;               /* in .debug_info */
    const u8 *dies;             /* the first DIE */
    int version, addr_size, offset_size;
    struct abbrev_table *ab;
    u64 low_pc;
    u64 str_offsets_base, addr_base, loclists_base, rnglists_base;
    u64 stmt_list;
    int has_stmt_list;
    const char *name, *comp_dir;
    int params_reversed;        /* tcc lists a function's parameters last to first */
    int *file_map;              /* line-program file index -> files[] */
    int nfile_map;
    int line_version;
};

static struct cu *cus;
static int ncus;

/* A DIE's attributes that we care about. */
struct die {
    u64 off;
    u32 tag;
    int children;
    const u8 *after;            /* first child, or next sibling */
    struct cu *cu;
    const char *name;
    u64 type;                   /* absolute .debug_info offset; 0 if none */
    int has_low, has_high, high_is_len;
    u64 low, high;
    const u8 *loc;
    size_t loc_len;
    u64 loclist;
    int has_loclist;
    const u8 *frame_base;
    size_t fb_len;
    u64 byte_size;
    int has_byte_size;
    u64 encoding;
    i64 member_loc;
    int has_member_loc;
    i64 upper_bound;
    int has_upper;
    u64 count;
    int has_count;
    i64 const_value;
    int has_const;
    u64 sibling;
    int external, declaration;
    u64 bit_size, data_bit_offset, bit_offset;
    int has_data_bit_offset, has_bit_offset;
    u64 ranges;
    int has_ranges, ranges_form_index;
    u64 origin;                 /* abstract_origin or specification */
};

static u64 read_addrx(struct cu *cu, u64 index)
{
    struct section *s = &prog.debug_addr;
    u64 off = cu->addr_base + index * cu->addr_size;
    if (!s->data || off + cu->addr_size > s->size)
        return 0;
    struct rd r = { s->data + off, s->data + s->size, 0 };
    return rd_n(&r, cu->addr_size);
}

static const char *read_strx(struct cu *cu, u64 index)
{
    struct section *s = &prog.debug_str_offsets;
    u64 off = cu->str_offsets_base + index * cu->offset_size;
    if (!s->data || off + cu->offset_size > s->size)
        return "?";
    struct rd r = { s->data + off, s->data + s->size, 0 };
    return sec_str(&prog.debug_str, rd_n(&r, cu->offset_size));
}

/* One attribute value: as a number (constants, addresses, offsets, references),
 * a string, or a block. Returns 0 if the form is unknown. */
struct aval {
    u64 u;
    i64 s;
    int is_signed;
    const char *str;
    const u8 *block;
    size_t len;
    int cls;                    /* 'c' constant 'a' address 'r' reference 's' string 'b' block 'o' offset 'f' flag 'x' index */
};

static int read_form(struct rd *r, struct cu *cu, u32 form, i64 implicit, struct aval *v)
{
    memset(v, 0, sizeof(*v));
    switch (form) {
    case FORM_addr: v->u = rd_n(r, cu->addr_size); v->cls = 'a'; break;
    case FORM_addrx: v->u = read_addrx(cu, rd_uleb(r)); v->cls = 'a'; break;
    case FORM_addrx1: v->u = read_addrx(cu, rd_n(r, 1)); v->cls = 'a'; break;
    case FORM_addrx2: v->u = read_addrx(cu, rd_n(r, 2)); v->cls = 'a'; break;
    case FORM_addrx3: v->u = read_addrx(cu, rd_n(r, 3)); v->cls = 'a'; break;
    case FORM_addrx4: v->u = read_addrx(cu, rd_n(r, 4)); v->cls = 'a'; break;
    case FORM_data1: v->u = rd_n(r, 1); v->s = (i8)v->u; v->cls = 'c'; break;
    case FORM_data2: v->u = rd_n(r, 2); v->s = (int16_t)v->u; v->cls = 'c'; break;
    case FORM_data4: v->u = rd_n(r, 4); v->s = (int32_t)v->u; v->cls = 'c'; break;
    case FORM_data8: v->u = rd_n(r, 8); v->s = (i64)v->u; v->cls = 'c'; break;
    case FORM_data16: v->block = r->p; v->len = 16; rd_n(r, 8); rd_n(r, 8); v->cls = 'b'; break;
    case FORM_sdata: v->s = rd_sleb(r); v->u = (u64)v->s; v->is_signed = 1; v->cls = 'c'; break;
    case FORM_udata: v->u = rd_uleb(r); v->s = (i64)v->u; v->cls = 'c'; break;
    case FORM_implicit_const: v->s = implicit; v->u = (u64)implicit; v->is_signed = 1; v->cls = 'c'; break;
    case FORM_string: v->str = rd_str(r); v->cls = 's'; break;
    case FORM_strp: v->str = sec_str(&prog.debug_str, rd_n(r, cu->offset_size)); v->cls = 's'; break;
    case FORM_line_strp: v->str = sec_str(&prog.debug_line_str, rd_n(r, cu->offset_size)); v->cls = 's'; break;
    case FORM_strx: v->str = read_strx(cu, rd_uleb(r)); v->cls = 's'; break;
    case FORM_strx1: v->str = read_strx(cu, rd_n(r, 1)); v->cls = 's'; break;
    case FORM_strx2: v->str = read_strx(cu, rd_n(r, 2)); v->cls = 's'; break;
    case FORM_strx3: v->str = read_strx(cu, rd_n(r, 3)); v->cls = 's'; break;
    case FORM_strx4: v->str = read_strx(cu, rd_n(r, 4)); v->cls = 's'; break;
    case FORM_ref1: v->u = cu->off + rd_n(r, 1); v->cls = 'r'; break;
    case FORM_ref2: v->u = cu->off + rd_n(r, 2); v->cls = 'r'; break;
    case FORM_ref4: v->u = cu->off + rd_n(r, 4); v->cls = 'r'; break;
    case FORM_ref8: v->u = cu->off + rd_n(r, 8); v->cls = 'r'; break;
    case FORM_ref_udata: v->u = cu->off + rd_uleb(r); v->cls = 'r'; break;
    case FORM_ref_addr: v->u = rd_n(r, cu->version <= 2 ? cu->addr_size : cu->offset_size); v->cls = 'r'; break;
    case FORM_ref_sig8: rd_n(r, 8); v->cls = 'x'; break;
    case FORM_ref_sup4: rd_n(r, 4); v->cls = 'x'; break;
    case FORM_ref_sup8: rd_n(r, 8); v->cls = 'x'; break;
    case FORM_strp_sup: rd_n(r, cu->offset_size); v->str = "?"; v->cls = 's'; break;
    case FORM_sec_offset: v->u = rd_n(r, cu->offset_size); v->cls = 'o'; break;
    case FORM_loclistx: case FORM_rnglistx: v->u = rd_uleb(r); v->cls = 'x'; break;
    case FORM_flag: v->u = rd_n(r, 1); v->cls = 'f'; break;
    case FORM_flag_present: v->u = 1; v->cls = 'f'; break;
    case FORM_exprloc: case FORM_block: v->len = rd_uleb(r); v->block = r->p; v->cls = 'b'; break;
    case FORM_block1: v->len = rd_n(r, 1); v->block = r->p; v->cls = 'b'; break;
    case FORM_block2: v->len = rd_n(r, 2); v->block = r->p; v->cls = 'b'; break;
    case FORM_block4: v->len = rd_n(r, 4); v->block = r->p; v->cls = 'b'; break;
    case FORM_indirect: {
        u32 f = rd_uleb(r);
        return read_form(r, cu, f, implicit, v);
    }
    default:
        return 0;
    }
    if (v->cls == 'b') {
        if (r->p + v->len > r->end) {
            r->bad = 1;
            return 0;
        }
        r->p += v->len;
    }
    return !r->bad;
}

/* Read the DIE at p (in cu). Returns 0 at a null entry (end of siblings) or error. */
static int read_die(struct cu *cu, const u8 *p, struct die *d)
{
    memset(d, 0, sizeof(*d));
    struct rd r = { p, prog.debug_info.data + cu->end, 0 };
    d->off = p - prog.debug_info.data;
    d->cu = cu;
    u64 code = rd_uleb(&r);
    if (!code || code >= (u64)cu->ab->n || !cu->ab->v[code].code) {
        d->after = r.p;
        return 0;
    }
    struct abbrev *a = &cu->ab->v[code];
    d->tag = a->tag;
    d->children = a->children;
    for (int i = 0; i < a->nattrs; i++) {
        struct aval v;
        if (!read_form(&r, cu, a->attrs[i].form, a->attrs[i].implicit, &v))
            return 0;
        switch (a->attrs[i].at) {
        case AT_name: if (v.cls == 's') d->name = v.str; break;
        case AT_type: if (v.cls == 'r') d->type = v.u; break;
        case AT_low_pc: d->low = v.u; d->has_low = 1; break;
        case AT_high_pc: d->high = v.u; d->has_high = 1; d->high_is_len = v.cls == 'c'; break;
        case AT_location:
            if (v.cls == 'b') {
                d->loc = v.block;
                d->loc_len = v.len;
            } else if (v.cls == 'o' || v.cls == 'c') {
                d->loclist = v.u;
                d->has_loclist = 1;
            } else if (v.cls == 'x') {
                /* loclistx: an index into the offsets table at loclists_base */
                struct section *s = &prog.debug_loclists;
                u64 off = cu->loclists_base + v.u * cu->offset_size;
                if (s->data && off + cu->offset_size <= s->size) {
                    struct rd t = { s->data + off, s->data + s->size, 0 };
                    d->loclist = cu->loclists_base + rd_n(&t, cu->offset_size);
                    d->has_loclist = 1;
                }
            }
            break;
        case AT_frame_base:
            if (v.cls == 'b') {
                d->frame_base = v.block;
                d->fb_len = v.len;
            }
            break;
        case AT_byte_size: d->byte_size = v.u; d->has_byte_size = 1; break;
        case AT_encoding: d->encoding = v.u; break;
        case AT_data_member_location:
            if (v.cls == 'c') {
                d->member_loc = v.s;
                d->has_member_loc = 1;
            } else if (v.cls == 'b' && v.len >= 1 && v.block[0] == 0x23) {     /* DW_OP_plus_uconst */
                struct rd t = { v.block + 1, v.block + v.len, 0 };
                d->member_loc = rd_uleb(&t);
                d->has_member_loc = 1;
            }
            break;
        case AT_upper_bound: d->upper_bound = v.is_signed ? v.s : (i64)v.u; d->has_upper = v.cls == 'c'; break;
        case AT_count: d->count = v.u; d->has_count = v.cls == 'c'; break;
        case AT_const_value:
            if (v.cls == 'c') {
                d->const_value = v.is_signed ? v.s : (i64)v.u;
                d->has_const = 1;
            }
            break;
        case AT_sibling: d->sibling = v.u; break;
        case AT_external: d->external = v.u != 0; break;
        case AT_declaration: d->declaration = v.u != 0; break;
        case AT_bit_size: d->bit_size = v.u; break;
        case AT_data_bit_offset: d->data_bit_offset = v.u; d->has_data_bit_offset = 1; break;
        case AT_bit_offset: d->bit_offset = v.u; d->has_bit_offset = 1; break;
        case AT_ranges: d->ranges = v.u; d->has_ranges = 1; d->ranges_form_index = v.cls == 'x'; break;
        case AT_abstract_origin:
        case AT_specification: if (v.cls == 'r') d->origin = v.u; break;
        case AT_stmt_list: cu->stmt_list = v.u; cu->has_stmt_list = d->tag == TAG_compile_unit; break;
        case AT_comp_dir: if (v.cls == 's') cu->comp_dir = v.str; break;
        case AT_producer:
            if (v.cls == 's' && d->tag == TAG_compile_unit)
                cu->params_reversed = !strncmp(v.str, "tcc ", 4);
            break;
        case AT_str_offsets_base: cu->str_offsets_base = v.u; break;
        case AT_addr_base: cu->addr_base = v.u; break;
        case AT_loclists_base: cu->loclists_base = v.u; break;
        case AT_rnglists_base: cu->rnglists_base = v.u; break;
        }
    }
    if (d->has_high && d->high_is_len)
        d->high += d->low;
    d->after = r.p;
    return 1;
}

static struct cu *cu_of(u64 off)
{
    for (int i = 0; i < ncus; i++)
        if (off >= cus[i].off && off < cus[i].end)
            return &cus[i];
    return 0;
}

static int die_at(u64 off, struct die *d)
{
    struct cu *cu = cu_of(off);
    return cu && read_die(cu, prog.debug_info.data + off, d);
}

/* The next sibling of d (which has been read). Walks its children if needed. */
static const u8 *skip_children(struct die *d)
{
    if (!d->children)
        return d->after;
    if (d->sibling && d->sibling > d->off)
        return prog.debug_info.data + d->sibling;
    const u8 *p = d->after;
    struct die c;
    for (;;) {
        if (p >= prog.debug_info.data + d->cu->end)
            return p;
        if (!read_die(d->cu, p, &c))
            return c.after;         /* the null entry ending the children */
        p = skip_children(&c);
    }
}

/* Does [low, high) or DW_AT_ranges of d contain pc? */
static int die_contains(struct die *d, u64 pc)
{
    if (d->has_low && d->has_high)
        return pc >= d->low && pc < d->high;
    if (!d->has_ranges)
        return 0;
    struct cu *cu = d->cu;
    if (cu->version >= 5) {
        struct section *s = &prog.debug_rnglists;
        if (!s->data)
            return 0;
        u64 off = d->ranges;
        if (d->ranges_form_index) {
            struct rd t = { s->data + cu->rnglists_base + off * cu->offset_size, s->data + s->size, 0 };
            off = cu->rnglists_base + rd_n(&t, cu->offset_size);
        }
        struct rd r = { s->data + off, s->data + s->size, 0 };
        u64 base = cu->low_pc;
        while (!r.bad && r.p < r.end) {
            u8 kind = rd_n(&r, 1);
            u64 a = 0, b = 0;
            switch (kind) {
            case 0: return 0;                               /* end_of_list */
            case 1: base = read_addrx(cu, rd_uleb(&r)); continue;
            case 2: a = read_addrx(cu, rd_uleb(&r)); b = read_addrx(cu, rd_uleb(&r)); break;
            case 3: a = read_addrx(cu, rd_uleb(&r)); b = a + rd_uleb(&r); break;
            case 4: a = base + rd_uleb(&r); b = base + rd_uleb(&r); break;
            case 5: base = rd_n(&r, cu->addr_size); continue;
            case 6: a = rd_n(&r, cu->addr_size); b = rd_n(&r, cu->addr_size); break;
            case 7: a = rd_n(&r, cu->addr_size); b = a + rd_uleb(&r); break;
            default: return 0;
            }
            if (pc >= a && pc < b)
                return 1;
        }
        return 0;
    }
    struct section *s = &prog.debug_ranges;
    if (!s->data || d->ranges >= s->size)
        return 0;
    struct rd r = { s->data + d->ranges, s->data + s->size, 0 };
    u64 base = cu->low_pc;
    while (!r.bad) {
        u64 a = rd_n(&r, cu->addr_size), b = rd_n(&r, cu->addr_size);
        if (!a && !b)
            return 0;
        if (a == ~0ULL) {
            base = b;
            continue;
        }
        if (pc >= base + a && pc < base + b)
            return 1;
    }
    return 0;
}

/* ---- the indexes ---- */

static struct func *funcs;
static int nfuncs;
static struct var *globals;
static int nglobals;

static int cmp_func(const void *a, const void *b)
{
    const struct func *x = a, *y = b;
    return x->low < y->low ? -1 : x->low > y->low;
}

static const char *die_name(struct die *d)
{
    if (d->name)
        return d->name;
    struct die o;
    for (int depth = 0; d->origin && depth < 4; depth++) {
        if (!die_at(d->origin, &o))
            break;
        if (o.name)
            return o.name;
        d = &o;
    }
    return 0;
}

static void fill_var(struct var *v, struct die *d, int cu_index)
{
    v->name = xstrdup(die_name(d) ? die_name(d) : "?");
    u64 tref = d->type;
    if (!tref && d->origin) {
        struct die o;
        if (die_at(d->origin, &o))
            tref = o.type;
    }
    v->type = type_at(tref);
    v->loc = d->loc;
    v->loc_len = d->loc_len;
    v->loclist = d->has_loclist ? d->loclist : ~0ULL;
    v->loclist_form5 = d->cu->version >= 5;
    v->has_const = d->has_const;
    v->const_value = d->const_value;
    v->is_param = d->tag == TAG_formal_parameter;
    v->cu_low_pc = d->cu->low_pc;
    v->cu_index = cu_index;
}

static void index_cu(int ci)
{
    struct cu *cu = &cus[ci];
    const u8 *p = cu->dies;
    const u8 *end = prog.debug_info.data + cu->end;
    struct die d;
    if (!read_die(cu, p, &d))
        return;
    if (d.tag != TAG_compile_unit && d.tag != TAG_partial_unit && d.tag != TAG_skeleton_unit)
        return;
    cu->name = d.name;
    cu->low_pc = d.has_low ? d.low : 0;
    if (!d.children)
        return;
    /* the CU's direct children: functions and globals (and a level deeper
       for functions inside namespaces etc., which C doesn't have) */
    p = d.after;
    while (p < end) {
        struct die c;
        if (!read_die(cu, p, &c))
            break;
        if (c.tag == TAG_subprogram && c.has_low && c.has_high && c.high > c.low) {
            funcs = xrealloc(funcs, (nfuncs + 1) * sizeof(struct func));
            struct func *f = &funcs[nfuncs++];
            memset(f, 0, sizeof(*f));
            const char *n = die_name(&c);
            f->name = xstrdup(n ? n : "?");
            f->low = c.low;
            f->high = c.high;
            f->frame_base = c.frame_base;
            f->frame_base_len = c.fb_len;
            f->cu_index = ci;
            f->die = c.off;
            f->ret_die = c.type;
        } else if (c.tag == TAG_variable && (c.loc || c.has_const) && die_name(&c)) {
            globals = xrealloc(globals, (nglobals + 1) * sizeof(struct var));
            fill_var(&globals[nglobals++], &c, ci);
        }
        p = skip_children(&c);
    }
}

/* ---- line tables ---- */

struct line_row *rows;
int nrows;
struct src_file *files;
int nfiles;

struct seq {
    u64 low, high;
    int first, n;
};
static struct seq *seqs;
static int nseqs;

static int file_index(const char *dir, const char *name)
{
    char path[512];
    if (name[0] == '/' || !dir || !dir[0])
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s/%s", dir, name);
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i].path, path))
            return i;
    files = xrealloc(files, (nfiles + 1) * sizeof(struct src_file));
    struct src_file *f = &files[nfiles];
    memset(f, 0, sizeof(*f));
    f->path = xstrdup(path);
    char *sl = strrchr(f->path, '/');
    f->base = sl ? sl + 1 : f->path;
    return nfiles++;
}

static void add_row(u64 addr, int file, int line, int is_stmt, int end_seq)
{
    static int cap;
    if (nrows == cap) {
        cap = cap ? cap * 2 : 1024;
        rows = xrealloc(rows, cap * sizeof(struct line_row));
    }
    rows[nrows++] = (struct line_row){ addr, file, line, (u8)is_stmt, (u8)end_seq };
}

/* Read the entry formats of a DWARF 5 directory or file table. */
static void read_v5_entries(struct rd *r, struct cu *cu, char ***names, u64 **dirs, int *count)
{
    int nformats = rd_n(r, 1);
    u64 fmt[16][2];
    for (int i = 0; i < nformats && i < 16; i++) {
        fmt[i][0] = rd_uleb(r);
        fmt[i][1] = rd_uleb(r);
    }
    u64 n = rd_uleb(r);
    *names = xcalloc(n + 1, sizeof(char *));
    *dirs = xcalloc(n + 1, sizeof(u64));
    for (u64 k = 0; k < n && !r->bad; k++) {
        for (int i = 0; i < nformats && i < 16; i++) {
            struct aval v;
            if (!read_form(r, cu, (u32)fmt[i][1], 0, &v)) {
                r->bad = 1;
                break;
            }
            if (fmt[i][0] == 1 && v.cls == 's')         /* DW_LNCT_path */
                (*names)[k] = (char *)v.str;
            else if (fmt[i][0] == 2)                    /* DW_LNCT_directory_index */
                (*dirs)[k] = v.u;
        }
    }
    *count = (int)n;
}

static void read_line_program(struct cu *cu)
{
    struct section *s = &prog.debug_line;
    if (!s->data || cu->stmt_list >= s->size)
        return;
    /* another CU may share it (rare): parse each program once */
    for (struct cu *o = cus; o < cu; o++)
        if (o->has_stmt_list && o->stmt_list == cu->stmt_list && o->file_map) {
            cu->file_map = o->file_map;
            cu->nfile_map = o->nfile_map;
            return;
        }
    struct rd r = { s->data + cu->stmt_list, s->data + s->size, 0 };
    u64 len = rd_n(&r, 4);
    int off_size = 4;
    if (len == 0xffffffff) {
        len = rd_n(&r, 8);
        off_size = 8;
    }
    const u8 *end = r.p + len;
    if (end > r.end)
        return;
    r.end = end;
    int version = rd_n(&r, 2);
    cu->line_version = version;
    if (version >= 5) {
        rd_n(&r, 1);                    /* address_size */
        rd_n(&r, 1);                    /* segment_selector_size */
    }
    u64 hlen = rd_n(&r, off_size);
    const u8 *prog_start = r.p + hlen;
    int min_inst = rd_n(&r, 1);
    if (version >= 4)
        rd_n(&r, 1);                    /* maximum_operations_per_instruction */
    int default_is_stmt = rd_n(&r, 1);
    int line_base = (signed char)rd_n(&r, 1);
    int line_range = rd_n(&r, 1);
    int opcode_base = rd_n(&r, 1);
    u8 std_len[256] = { 0 };
    for (int i = 1; i < opcode_base; i++)
        std_len[i] = rd_n(&r, 1);
    if (!line_range)
        return;

    /* directories and files -> files[] */
    int *map = 0, nmap = 0;
    if (version >= 5) {
        char **dnames, **fnames;
        u64 *ddirs, *fdirs;
        int nd, nf;
        read_v5_entries(&r, cu, &dnames, &ddirs, &nd);
        read_v5_entries(&r, cu, &fnames, &fdirs, &nf);
        map = xcalloc(nf + 1, sizeof(int));
        for (int i = 0; i < nf; i++) {
            const char *dir = fdirs[i] < (u64)nd ? dnames[fdirs[i]] : 0;
            map[i] = file_index(dir, fnames[i] ? fnames[i] : "?");
        }
        nmap = nf;
        free(dnames);
        free(ddirs);
        free(fnames);
        free(fdirs);
    } else {
        const char *dirs[256];
        int nd = 1;
        dirs[0] = cu->comp_dir;
        for (;;) {
            const char *d = rd_str(&r);
            if (!*d || r.bad)
                break;
            if (nd < 256)
                dirs[nd++] = d;
        }
        map = xcalloc(1, sizeof(int));
        nmap = 1;                       /* file numbers start at 1 */
        map[0] = -1;
        for (;;) {
            const char *f = rd_str(&r);
            if (!*f || r.bad)
                break;
            u64 di = rd_uleb(&r);
            rd_uleb(&r);
            rd_uleb(&r);
            map = xrealloc(map, (nmap + 1) * sizeof(int));
            map[nmap++] = file_index(di < (u64)nd ? dirs[di] : 0, f);
        }
    }
    cu->file_map = map;
    cu->nfile_map = nmap;

    /* the state machine */
    r.p = prog_start;
    u64 addr = 0;
    int file = 1, line = 1, is_stmt = default_is_stmt;
    int seq_start = nrows;
#define FILE_OF(f) ((f) >= 0 && (f) < nmap ? map[f] : -1)
#define EMIT(endseq) add_row(addr, FILE_OF(file), line, is_stmt, endseq)
    while (r.p < r.end && !r.bad) {
        u8 op = rd_n(&r, 1);
        if (op >= opcode_base) {
            int adj = op - opcode_base;
            addr += (u64)(adj / line_range) * min_inst;
            line += line_base + adj % line_range;
            EMIT(0);
            continue;
        }
        switch (op) {
        case 0: {                       /* extended */
            u64 n = rd_uleb(&r);
            const u8 *next = r.p + n;
            u8 sub = n ? rd_n(&r, 1) : 0;
            if (sub == 1) {             /* end_sequence */
                EMIT(1);
                if (nrows > seq_start) {
                    seqs = xrealloc(seqs, (nseqs + 1) * sizeof(struct seq));
                    seqs[nseqs++] = (struct seq){ rows[seq_start].addr, addr, seq_start, nrows - seq_start };
                }
                seq_start = nrows;
                addr = 0;
                file = 1;
                line = 1;
                is_stmt = default_is_stmt;
            } else if (sub == 2) {      /* set_address */
                addr = rd_n(&r, (int)(n - 1));
            }
            r.p = next;
            break;
        }
        case 1: EMIT(0); break;                                         /* copy */
        case 2: addr += rd_uleb(&r) * min_inst; break;                  /* advance_pc */
        case 3: line += (int)rd_sleb(&r); break;                        /* advance_line */
        case 4: file = (int)rd_uleb(&r); break;                         /* set_file */
        case 5: rd_uleb(&r); break;                                     /* set_column */
        case 6: is_stmt = !is_stmt; break;                              /* negate_stmt */
        case 7: break;                                                  /* set_basic_block */
        case 8: addr += (u64)((255 - opcode_base) / line_range) * min_inst; break;   /* const_add_pc */
        case 9: addr += rd_n(&r, 2); break;                             /* fixed_advance_pc */
        case 10: case 11: break;                                        /* prologue_end, epilogue_begin */
        case 12: rd_uleb(&r); break;                                    /* set_isa */
        default:
            for (int i = 0; i < std_len[op]; i++)
                rd_uleb(&r);
        }
    }
#undef EMIT
#undef FILE_OF
}

static int cmp_seq(const void *a, const void *b)
{
    const struct seq *x = a, *y = b;
    return x->low < y->low ? -1 : x->low > y->low;
}

struct line_row *line_for(u64 addr)
{
    int lo = 0, hi = nseqs - 1, s = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (seqs[mid].low <= addr) {
            s = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    /* sequences can overlap in odd cases: try a few before this one */
    for (int k = s; k >= 0 && k > s - 8; k--) {
        struct seq *q = &seqs[k];
        if (addr < q->low || addr >= q->high)
            continue;
        struct line_row *best = 0;
        int a = q->first, b = q->first + q->n - 1;
        while (a <= b) {
            int mid = (a + b) / 2;
            if (rows[mid].addr <= addr) {
                best = &rows[mid];
                a = mid + 1;
            } else {
                b = mid - 1;
            }
        }
        /* several rows at one address: the last one wins, as in gdb */
        if (best && !best->end_seq && best->file >= 0)
            return best;
    }
    return 0;
}

u64 addr_for_line(int file, int line, int *actual)
{
    int best_line = 1 << 30;
    u64 best = 0;
    for (int i = 0; i < nrows; i++) {
        struct line_row *r = &rows[i];
        if (r->file != file || r->end_seq || !r->is_stmt || r->line < line)
            continue;
        if (r->line < best_line || (r->line == best_line && r->addr < best)) {
            best_line = r->line;
            best = r->addr;
        }
    }
    if (actual)
        *actual = best_line == 1 << 30 ? 0 : best_line;
    return best_line == 1 << 30 ? 0 : best;
}

int find_file(const char *name)
{
    size_t nl = strlen(name);
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i].path, name))
            return i;
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i].base, name))
            return i;
    for (int i = 0; i < nfiles; i++) {
        size_t pl = strlen(files[i].path);
        if (pl > nl && files[i].path[pl - nl - 1] == '/' && !strcmp(files[i].path + pl - nl, name))
            return i;
    }
    return -1;
}

static void load_source(struct src_file *f)
{
    f->loaded = 1;
    const char *tries[3] = { f->path, f->base, 0 };
    char usr[512];
    snprintf(usr, sizeof(usr), "/usr/src/%s", f->base);
    tries[2] = usr;
    FILE *fp = 0;
    for (int i = 0; i < 3 && !fp; i++)
        fp = fopen(tries[i], "r");
    if (!fp)
        return;
    int cap = 0, c;
    for (;;) {
        /* one line, however long */
        size_t n = 0, bcap = 128;
        char *buf = xmalloc(bcap);
        while ((c = fgetc(fp)) != EOF && c != '\n') {
            if (n + 1 >= bcap)
                buf = xrealloc(buf, bcap *= 2);
            buf[n++] = (char)c;
        }
        buf[n] = 0;
        if (c == EOF && !n) {
            free(buf);
            break;
        }
        if (f->nlines == cap) {
            cap = cap ? cap * 2 : 256;
            f->lines = xrealloc(f->lines, cap * sizeof(char *));
        }
        f->lines[f->nlines++] = buf;
        if (c == EOF)
            break;
    }
    fclose(fp);
}

const char *src_line(int file, int line)
{
    if (file < 0 || file >= nfiles)
        return 0;
    struct src_file *f = &files[file];
    if (!f->loaded)
        load_source(f);
    if (line < 1 || line > f->nlines)
        return 0;
    return f->lines[line - 1];
}

/* ---- loading ---- */

int dwarf_load(void)
{
    struct section *info = &prog.debug_info;
    if (!info->data || !prog.debug_abbrev.data)
        return 0;
    struct rd r = { info->data, info->data + info->size, 0 };
    while (r.p < r.end && !r.bad) {
        struct cu cu;
        memset(&cu, 0, sizeof(cu));
        cu.off = r.p - info->data;
        u64 len = rd_n(&r, 4);
        cu.offset_size = 4;
        if (len == 0xffffffff) {
            len = rd_n(&r, 8);
            cu.offset_size = 8;
        }
        const u8 *end = r.p + len;
        if (end > r.end || len < 7)
            break;
        cu.end = end - info->data;
        cu.version = rd_n(&r, 2);
        u64 abbrev_off;
        if (cu.version >= 5) {
            int unit_type = rd_n(&r, 1);
            cu.addr_size = rd_n(&r, 1);
            abbrev_off = rd_n(&r, cu.offset_size);
            if (unit_type == 2 || unit_type == 6)       /* type / split type units */
                rd_n(&r, 8 + cu.offset_size);
            else if (unit_type == 4 || unit_type == 5)  /* skeleton / split compile */
                rd_n(&r, 8);
        } else {
            abbrev_off = rd_n(&r, cu.offset_size);
            cu.addr_size = rd_n(&r, 1);
        }
        if (cu.version < 2 || cu.version > 5 || cu.addr_size != 8) {
            r.p = end;
            continue;
        }
        cu.dies = r.p;
        cu.ab = abbrevs_at(abbrev_off);
        cus = xrealloc(cus, (ncus + 1) * sizeof(struct cu));
        cus[ncus++] = cu;
        r.p = end;
    }
    for (int i = 0; i < ncus; i++) {
        /* the CU DIE first: it sets the bases that other attributes use */
        struct die d;
        read_die(&cus[i], cus[i].dies, &d);
        cus[i].low_pc = d.has_low ? d.low : 0;
    }
    for (int i = 0; i < ncus; i++)
        index_cu(i);
    for (int i = 0; i < ncus; i++)
        if (cus[i].has_stmt_list)
            read_line_program(&cus[i]);
    qsort(funcs, nfuncs, sizeof(struct func), cmp_func);
    qsort(seqs, nseqs, sizeof(struct seq), cmp_seq);
    return ncus;
}

struct func *func_at(u64 pc)
{
    int lo = 0, hi = nfuncs - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (funcs[mid].low <= pc) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    for (int i = best; i >= 0 && i > best - 4; i--)
        if (pc >= funcs[i].low && pc < funcs[i].high)
            return &funcs[i];
    return 0;
}

struct func *func_named(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(funcs[i].name, name))
            return &funcs[i];
    return 0;
}

/* ---- types ---- */

struct tcache {
    u64 off;
    struct type *t;
};
static struct tcache *tcache;
static int ntcache, captcache;

static struct type *tc_find(u64 off)
{
    for (int i = 0; i < ntcache; i++)
        if (tcache[i].off == off)
            return tcache[i].t;
    return 0;
}

static void tc_add(u64 off, struct type *t)
{
    if (ntcache == captcache) {
        captcache = captcache ? captcache * 2 : 64;
        tcache = xrealloc(tcache, captcache * sizeof(struct tcache));
    }
    tcache[ntcache++] = (struct tcache){ off, t };
}

static struct type *mk(enum tkind k, const char *name, u64 size)
{
    struct type *t = xcalloc(1, sizeof(*t));
    t->kind = k;
    t->name = name ? xstrdup(name) : 0;
    t->size = size;
    return t;
}

static struct type void_type = { T_VOID, "void", 1, 0, 0, 0, 0, 0, 0, 0, 0 };

struct type *type_int(void)
{
    static struct type *t;
    if (!t) {
        t = mk(T_INT, "int", 4);
        t->is_signed = 1;
    }
    return t;
}

struct type *type_long(void)
{
    static struct type *t;
    if (!t) {
        t = mk(T_INT, "long", 8);
        t->is_signed = 1;
    }
    return t;
}

struct type *type_ulong(void)
{
    static struct type *t;
    if (!t)
        t = mk(T_INT, "unsigned long", 8);
    return t;
}

struct type *type_char(void)
{
    static struct type *t;
    if (!t) {
        t = mk(T_INT, "char", 1);
        t->is_char = 1;
    }
    return t;
}

struct type *type_double(void)
{
    static struct type *t;
    if (!t)
        t = mk(T_FLOAT, "double", 8);
    return t;
}

struct type *type_pointer_to(struct type *target)
{
    struct type *t = mk(T_PTR, 0, 8);
    t->target = target ? target : &void_type;
    return t;
}

struct type *type_at(u64 off)
{
    if (!off)
        return &void_type;
    struct type *t = tc_find(off);
    if (t)
        return t;
    struct die d;
    if (!die_at(off, &d))
        return &void_type;
    switch (d.tag) {
    case TAG_base_type: {
        int enc = (int)d.encoding;
        t = mk(enc == 4 ? T_FLOAT : enc == 2 ? T_BOOL : T_INT, d.name, d.byte_size);
        t->is_signed = enc == 5 || enc == 6 || enc == 4;
        t->is_char = enc == 6 || enc == 8 || (d.name && strstr(d.name, "char") && d.byte_size == 1);
        if (d.name && !strcmp(d.name, "void"))
            t->kind = T_VOID;       /* tcc describes void as a 1-byte base type */
        tc_add(off, t);
        return t;
    }
    case TAG_pointer_type:
        t = mk(T_PTR, 0, d.has_byte_size ? d.byte_size : 8);
        tc_add(off, t);
        t->target = type_at(d.type);
        return t;
    case TAG_typedef:
    case TAG_const_type:
    case TAG_volatile_type:
    case TAG_restrict_type:
    case TAG_atomic_type: {
        struct type *under = type_at(d.type);
        if (d.tag != TAG_typedef || !d.name)
            return under;
        if (under->kind == T_STRUCT || under->kind == T_UNION || under->kind == T_ENUM ||
            under->kind == T_ARRAY) {
            /* the aggregate itself: a copy taken now could still be incomplete
               (typedef struct node node; struct node { node *next; }) */
            tc_add(off, under);
            return under;
        }
        /* a typedef: the same type under another name */
        t = xmalloc(sizeof(*t));
        *t = *under;
        t->name = xstrdup(d.name);
        tc_add(off, t);
        return t;
    }
    case TAG_structure_type:
    case TAG_union_type: {
        char nm[128];
        snprintf(nm, sizeof(nm), "%s %s", d.tag == TAG_union_type ? "union" : "struct",
                 d.name ? d.name : "{...}");
        t = mk(d.tag == TAG_union_type ? T_UNION : T_STRUCT, nm, d.byte_size);
        tc_add(off, t);
        if (!d.children)
            return t;
        const u8 *p = d.after;
        struct die m;
        while (read_die(d.cu, p, &m)) {
            if (m.tag == TAG_member) {
                t->members = xrealloc(t->members, (t->nmembers + 1) * sizeof(struct member));
                struct member *mb = &t->members[t->nmembers++];
                mb->name = xstrdup(m.name ? m.name : "");
                mb->offset = m.has_member_loc ? (u64)m.member_loc : 0;
                mb->bit_size = (int)m.bit_size;
                mb->bit_offset = 0;
                if (m.bit_size && m.has_data_bit_offset) {
                    mb->offset = m.data_bit_offset / 8;
                    mb->bit_offset = (int)(m.data_bit_offset % 8);
                } else if (m.bit_size && m.has_bit_offset) {
                    /* DWARF 2-4: from the most significant bit of the storage unit */
                    u64 storage = m.has_byte_size ? m.byte_size : 4;
                    mb->bit_offset = (int)(storage * 8 - m.bit_offset - m.bit_size);
                }
                mb->type = 0;
                u64 mt = m.type;
                mb->type = type_at(mt);
            }
            p = skip_children(&m);
        }
        return t;
    }
    case TAG_array_type: {
        t = mk(T_ARRAY, 0, 0);
        tc_add(off, t);
        struct type *elem = type_at(d.type);
        /* subranges: the first is the outermost dimension */
        u64 dims[8];
        int nd = 0;
        if (d.children) {
            const u8 *p = d.after;
            struct die s;
            while (read_die(d.cu, p, &s)) {
                if (s.tag == TAG_subrange_type && nd < 8)
                    dims[nd++] = s.has_count ? s.count : s.has_upper ? (u64)(s.upper_bound + 1) : 0;
                p = skip_children(&s);
            }
        }
        if (!nd)
            dims[nd++] = 0;
        /* build inner dimensions first */
        struct type *inner = elem;
        for (int i = nd - 1; i >= 1; i--) {
            struct type *a = mk(T_ARRAY, 0, dims[i] * inner->size);
            a->target = inner;
            a->count = dims[i];
            inner = a;
        }
        t->target = inner;
        t->count = dims[0];
        t->size = dims[0] * inner->size;
        return t;
    }
    case TAG_enumeration_type: {
        char nm[128];
        snprintf(nm, sizeof(nm), "enum %s", d.name ? d.name : "{...}");
        t = mk(T_ENUM, nm, d.has_byte_size ? d.byte_size : 4);
        t->is_signed = 1;
        tc_add(off, t);
        if (d.children) {
            const u8 *p = d.after;
            struct die e;
            while (read_die(d.cu, p, &e)) {
                if (e.tag == TAG_enumerator) {
                    t->enums = xrealloc(t->enums, (t->nenums + 1) * sizeof(struct enumerator));
                    t->enums[t->nenums].name = xstrdup(e.name ? e.name : "?");
                    t->enums[t->nenums].value = e.const_value;
                    t->nenums++;
                }
                p = skip_children(&e);
            }
        }
        return t;
    }
    case TAG_subroutine_type:
        t = mk(T_FUNC, 0, 1);
        tc_add(off, t);
        t->target = type_at(d.type);
        return t;
    default:
        return &void_type;
    }
}

const char *type_name(struct type *t, char *buf, size_t size)
{
    if (!t) {
        snprintf(buf, size, "?");
        return buf;
    }
    if (t->name) {
        snprintf(buf, size, "%s", t->name);
        return buf;
    }
    char inner[200];
    switch (t->kind) {
    case T_PTR: {
        type_name(t->target, inner, sizeof(inner));
        size_t n = strlen(inner);
        snprintf(buf, size, "%s%s*", inner, n && inner[n - 1] == '*' ? "" : " ");
        break;
    }
    case T_ARRAY:
        type_name(t->target, inner, sizeof(inner));
        snprintf(buf, size, "%s [%llu]", inner, (unsigned long long)t->count);
        break;
    case T_FUNC:
        type_name(t->target, inner, sizeof(inner));
        snprintf(buf, size, "%s (...)", inner);
        break;
    default:
        snprintf(buf, size, "?");
    }
    return buf;
}

/* ---- scopes ---- */

static struct var *scope_vars;
static int nscope, capscope;
static int *scope_depth;

static void add_scope_var(struct die *d, int depth, int ci)
{
    if (nscope == capscope) {
        capscope = capscope ? capscope * 2 : 32;
        scope_vars = xrealloc(scope_vars, capscope * sizeof(struct var));
        scope_depth = xrealloc(scope_depth, capscope * sizeof(int));
    }
    fill_var(&scope_vars[nscope], d, ci);
    scope_depth[nscope++] = depth;
}

static void collect(struct cu *cu, const u8 *p, u64 pc, int depth, int ci)
{
    struct die d;
    while (p < prog.debug_info.data + cu->end && read_die(cu, p, &d)) {
        if ((d.tag == TAG_variable || d.tag == TAG_formal_parameter) && die_name(&d))
            add_scope_var(&d, depth, ci);
        else if (d.tag == TAG_lexical_block && d.children && (die_contains(&d, pc) || (!d.has_low && !d.has_ranges)))
            collect(cu, d.after, pc, depth + 1, ci);
        p = skip_children(&d);
    }
}

int vars_at(u64 pc, struct var **out, int locals_only)
{
    for (int i = 0; i < nscope; i++)
        free(scope_vars[i].name);
    nscope = 0;
    struct func *f = func_at(pc);
    if (f) {
        struct die d;
        int ok = die_at(f->die, &d);
        if (ok && d.children)
            collect(d.cu, d.after, pc, 1, f->cu_index);
        /* tcc writes parameters last to first: put them in declaration order */
        if (ok && d.cu->params_reversed) {
            int idx[64], np = 0;
            for (int i = 0; i < nscope && np < 64; i++)
                if (scope_vars[i].is_param && scope_depth[i] == 1)
                    idx[np++] = i;
            for (int a = 0, b = np - 1; a < b; a++, b--) {
                struct var t = scope_vars[idx[a]];
                scope_vars[idx[a]] = scope_vars[idx[b]];
                scope_vars[idx[b]] = t;
            }
        }
        /* innermost first; parameters before locals at the same depth */
        for (int i = 1; i < nscope; i++) {
            struct var v = scope_vars[i];
            int dep = scope_depth[i], j = i - 1;
            while (j >= 0 && scope_depth[j] < dep) {
                scope_vars[j + 1] = scope_vars[j];
                scope_depth[j + 1] = scope_depth[j];
                j--;
            }
            scope_vars[j + 1] = v;
            scope_depth[j + 1] = dep;
        }
    }
    if (!locals_only) {
        int ci = f ? f->cu_index : -1;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < nglobals; i++) {
                if ((pass == 0) != (globals[i].cu_index == ci))
                    continue;
                if (nscope == capscope) {
                    capscope = capscope ? capscope * 2 : 32;
                    scope_vars = xrealloc(scope_vars, capscope * sizeof(struct var));
                    scope_depth = xrealloc(scope_depth, capscope * sizeof(int));
                }
                scope_vars[nscope] = globals[i];
                scope_vars[nscope].name = xstrdup(globals[i].name);
                scope_depth[nscope++] = 0;
            }
    }
    *out = scope_vars;
    return nscope;
}

struct var *var_lookup(u64 pc, const char *name)
{
    struct var *v;
    int n = vars_at(pc, &v, 0);
    for (int i = 0; i < n; i++)
        if (!strcmp(v[i].name, name))
            return &v[i];
    return 0;
}

/* ---- locations ---- */

/* The location expression of v that applies at pc (location lists). */
int var_location(struct var *v, u64 pc, const u8 **expr, size_t *len)
{
    if (v->loc) {
        *expr = v->loc;
        *len = v->loc_len;
        return 1;
    }
    if (v->loclist == ~0ULL)
        return 0;
    struct cu *cu = v->cu_index >= 0 && v->cu_index < ncus ? &cus[v->cu_index] : 0;
    if (!cu)
        return 0;
    if (v->loclist_form5) {
        struct section *s = &prog.debug_loclists;
        if (!s->data || v->loclist >= s->size)
            return 0;
        struct rd r = { s->data + v->loclist, s->data + s->size, 0 };
        u64 base = v->cu_low_pc;
        while (!r.bad && r.p < r.end) {
            u8 kind = rd_n(&r, 1);
            u64 a = 0, b = 0;
            int has_range = 1;
            switch (kind) {
            case 0: return 0;                                   /* end_of_list */
            case 1: base = read_addrx(cu, rd_uleb(&r)); continue;
            case 2: a = read_addrx(cu, rd_uleb(&r)); b = read_addrx(cu, rd_uleb(&r)); break;
            case 3: a = read_addrx(cu, rd_uleb(&r)); b = a + rd_uleb(&r); break;
            case 4: a = base + rd_uleb(&r); b = base + rd_uleb(&r); break;
            case 5: has_range = 0; break;                       /* default_location */
            case 6: base = rd_n(&r, 8); continue;
            case 7: a = rd_n(&r, 8); b = rd_n(&r, 8); break;
            case 8: a = rd_n(&r, 8); b = a + rd_uleb(&r); break;
            default: return 0;
            }
            u64 n = rd_uleb(&r);
            if (r.p + n > r.end)
                return 0;
            if (!has_range || (pc >= a && pc < b)) {
                *expr = r.p;
                *len = n;
                return 1;
            }
            r.p += n;
        }
        return 0;
    }
    struct section *s = &prog.debug_loc;
    if (!s->data || v->loclist >= s->size)
        return 0;
    struct rd r = { s->data + v->loclist, s->data + s->size, 0 };
    u64 base = v->cu_low_pc;
    while (!r.bad) {
        u64 a = rd_n(&r, 8), b = rd_n(&r, 8);
        if (!a && !b)
            return 0;
        if (a == ~0ULL) {
            base = b;
            continue;
        }
        u64 n = rd_n(&r, 2);
        if (pc >= base + a && pc < base + b) {
            *expr = r.p;
            *len = n;
            return 1;
        }
        r.p += n;
    }
    return 0;
}

/*
 * Evaluate a DWARF location expression for frame f. Result: a memory
 * address (V_MEM), a register (V_REG/V_FREG, frame 0 only), or a value
 * (V_NONE with bytes). Returns 0, or -1 if it can't be done here.
 */
int dwarf_eval(const u8 *e, size_t n, struct frame *f, int frame_no, u64 frame_base, int has_fb,
               struct value *out)
{
    u64 stack[64];
    int sp = 0;
    struct rd r = { e, e + n, 0 };
    out->loc = V_NONE;
#define PUSH(x) do { if (sp < 64) stack[sp++] = (x); } while (0)
#define POP() (sp ? stack[--sp] : 0)
    while (r.p < r.end && !r.bad) {
        u8 op = rd_n(&r, 1);
        if (op >= 0x30 && op <= 0x4f) {             /* lit0..31 */
            PUSH(op - 0x30);
        } else if (op >= 0x50 && op <= 0x6f) {      /* reg0..31: the value is in a register */
            if (frame_no != 0 && op != 0x58)
                return -1;
            out->loc = V_REG;
            out->reg = op - 0x50;
            if (op == 0x58 && frame_no != 0) {      /* s0 of an outer frame: its fp */
                out->loc = V_NONE;
                memcpy(out->bytes, &f->fp, 8);
            }
            return 0;
        } else if (op >= 0x70 && op <= 0x8f) {      /* breg0..31 */
            int reg = op - 0x70;
            i64 off = rd_sleb(&r);
            u64 base;
            if (reg == 2)
                base = f->sp;
            else if (reg == 8)
                base = f->fp;
            else if (frame_no == 0)
                base = regs.x[reg];
            else
                return -1;
            PUSH(base + off);
        } else {
            switch (op) {
            case 0x03: PUSH(rd_n(&r, 8)); break;                        /* addr */
            case 0x06: {                                                /* deref */
                u64 a = POP(), v;
                if (read_u64(a, &v))
                    return -1;
                PUSH(v);
                break;
            }
            case 0x08: PUSH(rd_n(&r, 1)); break;
            case 0x09: PUSH((u64)(i64)(i8)rd_n(&r, 1)); break;
            case 0x0a: PUSH(rd_n(&r, 2)); break;
            case 0x0b: PUSH((u64)(i64)(int16_t)rd_n(&r, 2)); break;
            case 0x0c: PUSH(rd_n(&r, 4)); break;
            case 0x0d: PUSH((u64)(i64)(int32_t)rd_n(&r, 4)); break;
            case 0x0e: case 0x0f: PUSH(rd_n(&r, 8)); break;
            case 0x10: PUSH(rd_uleb(&r)); break;
            case 0x11: PUSH((u64)rd_sleb(&r)); break;
            case 0x12: { u64 a = POP(); PUSH(a); PUSH(a); break; }      /* dup */
            case 0x13: (void)POP(); break;                              /* drop */
            case 0x14: { u64 b = POP(), a = POP(); PUSH(a); PUSH(b); PUSH(a); break; }
            case 0x16: { u64 b = POP(), a = POP(); PUSH(b); PUSH(a); break; }
            case 0x1a: { u64 b = POP(), a = POP(); PUSH(a & b); break; }
            case 0x1c: { u64 b = POP(), a = POP(); PUSH(a - b); break; }
            case 0x1e: { u64 b = POP(), a = POP(); PUSH(a * b); break; }
            case 0x1f: { u64 a = POP(); PUSH(-a); break; }
            case 0x20: { u64 a = POP(); PUSH(~a); break; }
            case 0x21: { u64 b = POP(), a = POP(); PUSH(a | b); break; }
            case 0x22: { u64 b = POP(), a = POP(); PUSH(a + b); break; }
            case 0x23: { u64 a = POP(); PUSH(a + rd_uleb(&r)); break; }  /* plus_uconst */
            case 0x24: { u64 b = POP(), a = POP(); PUSH(a << b); break; }
            case 0x25: { u64 b = POP(), a = POP(); PUSH(a >> b); break; }
            case 0x26: { u64 b = POP(); i64 a = (i64)POP(); PUSH((u64)(a >> b)); break; }
            case 0x27: { u64 b = POP(), a = POP(); PUSH(a ^ b); break; }
            case 0x90: {                                                /* regx */
                u64 reg = rd_uleb(&r);
                if (frame_no != 0)
                    return -1;
                out->loc = reg >= 32 && reg < 64 ? V_FREG : V_REG;
                out->reg = (int)(reg & 31);
                return 0;
            }
            case 0x91:                                                  /* fbreg */
                if (!has_fb)
                    return -1;
                PUSH(frame_base + rd_sleb(&r));
                break;
            case 0x92: {                                                /* bregx */
                u64 reg = rd_uleb(&r);
                i64 off = rd_sleb(&r);
                if (reg == 2)
                    PUSH(f->sp + off);
                else if (reg == 8)
                    PUSH(f->fp + off);
                else if (frame_no == 0 && reg < 32)
                    PUSH(regs.x[reg] + off);
                else
                    return -1;
                break;
            }
            case 0x93:                                                  /* piece: first piece only */
                r.p = r.end;
                break;
            case 0x9c: PUSH(f->cfa); break;                             /* call_frame_cfa */
            case 0x9e: {                                                /* implicit_value */
                u64 len = rd_uleb(&r);
                memset(out->bytes, 0, sizeof(out->bytes));
                memcpy(out->bytes, r.p, len < 16 ? len : 16);
                out->loc = V_NONE;
                return 0;
            }
            case 0x9f:                                                  /* stack_value */
                if (!sp)
                    return -1;
                memcpy(out->bytes, &stack[sp - 1], 8);
                out->loc = V_NONE;
                return 0;
            default:
                return -1;                                              /* entry_value etc. */
            }
        }
    }
#undef PUSH
#undef POP
    if (!sp)
        return -1;
    out->loc = V_MEM;
    out->addr = stack[sp - 1];
    return 0;
}

/* The frame base of the function at f->pc (for DW_OP_fbreg). */
int frame_base_of(struct func *fn, struct frame *f, int frame_no, u64 *fb)
{
    if (!fn || !fn->frame_base) {
        *fb = f->cfa;
        return 1;
    }
    struct value v;
    if (dwarf_eval(fn->frame_base, fn->frame_base_len, f, frame_no, 0, 0, &v))
        return 0;
    if (v.loc == V_REG) {           /* the base is the register's value */
        *fb = v.reg == 8 ? f->fp : v.reg == 2 ? f->sp : regs.x[v.reg];
        return 1;
    }
    if (v.loc == V_NONE) {
        memcpy(fb, v.bytes, 8);
        return 1;
    }
    *fb = v.addr;                   /* call_frame_cfa and friends */
    return 1;
}
