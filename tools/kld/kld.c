/*
 * kld: links the Eunuchs kernel.
 *
 * A static linker for RISC-V 64-bit ELF relocatable objects (from gcc or
 * tcc), laid out by a linker script in the subset of GNU ld syntax that
 * kernel/arch/riscv/kernel.ld uses. It runs on the Mac and on Eunuchs, so
 * that tcc can build the kernel there (tcc's own linker has no scripts).
 *
 *   kld -T script -o kernel.elf [--ksyms] [-M] file.o ...
 *
 * --ksyms  fill the .ksyms output section with the kernel symbol table (the
 *          format tools/gen_ksyms.py writes from a first link): one link
 *          instead of two
 * -M       print a link map (output and input sections, addresses)
 *
 * The output has one PT_LOAD per allocated output section, physical
 * addresses from AT() (as GNU ld: the VMA-LMA difference carries over to the
 * sections after), the symbol table, and the debug sections with their
 * relocations applied. Nothing is relaxed or merged: strings in mergeable
 * sections stay duplicated, and .eh_frame, .comment and notes are dropped.
 * tcc reaches global variables through the GOT (auipc + ld); kld turns
 * that into a direct address (auipc + addi), so there is no GOT.
 *
 * Script subset: ENTRY(sym), OUTPUT_ARCH(...), `sym = expr;` and
 * `PROVIDE(sym = expr);` anywhere, and SECTIONS { `. = expr;`
 * name [(NOLOAD)] : [AT(expr)] { *(pattern ...) KEEP(*(pattern ...))
 * `. = expr;` assignments } }. Expressions: numbers, symbols, `.`, ALIGN(n),
 * + - * / and parentheses. Patterns match section names with `*`; COMMON is
 * the common symbols. An allocated input section no statement matches is an
 * error.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ELF ---- */

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Shdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Phdr;

typedef struct {
    uint32_t name;
    unsigned char info, other;
    uint16_t shndx;
    uint64_t value, size;
} Sym;

typedef struct {
    uint64_t offset, info;
    int64_t addend;
} Rela;

enum { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };
#define ELF_BIND(i) ((i) >> 4)
#define ELF_TYPE(i) ((i) & 15)
#define PAGE 4096

/* ---- utilities ---- */

static const char *prog = "kld";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

#define PUSH(arr, n, cap, v) do { \
    if ((n) == (cap)) { (cap) = (cap) ? 2 * (cap) : 16; (arr) = xrealloc((arr), (cap) * sizeof(*(arr))); } \
    (arr)[(n)++] = (v); } while (0)

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return a > 1 ? (v + a - 1) & ~(a - 1) : v;
}

static char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: cannot open", path);
    size_t cap = 1 << 16, n = 0, got;
    char *buf = xmalloc(cap);
    while ((got = fread(buf + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap)
            buf = xrealloc(buf, cap *= 2);
    }
    fclose(f);
    *size = n;
    return buf;
}

/* glob: `*` matches any run of characters */
static int match(const char *pat, const char *s)
{
    if (!*pat)
        return !*s;
    if (*pat == '*') {
        for (;; s++) {
            if (match(pat + 1, s))
                return 1;
            if (!*s)
                return 0;
        }
    }
    return *pat == *s && match(pat + 1, s + 1);
}

/* ---- inputs ---- */

struct osec;

struct isec {
    struct obj *obj;
    int index;                  /* in its object (-1: a common symbol) */
    const char *name;
    uint32_t type;
    uint64_t flags, size, align;
    const unsigned char *data;  /* NULL for NOBITS */
    struct osec *osec;          /* NULL: not placed (or dropped) */
    uint64_t addr;              /* VMA (offset within its section if not allocated) */
    uint64_t out_off;           /* offset within the output section */
};

struct obj {
    const char *path;
    char *buf;
    size_t size;
    Ehdr *eh;
    Shdr *sh;
    const char *shstr;
    Sym *syms;
    int nsyms;
    const char *strtab;
    struct isec **secs;         /* by section index */
    struct gsym **globals;      /* by symbol index (non-local ones) */
};

struct gsym {
    const char *name;
    struct gsym *next;          /* hash chain */
    struct isec *sec;           /* defining section, or NULL */
    uint64_t value;             /* section offset, or absolute */
    int defined, absolute, weak, script, type, local;
    uint64_t size;
    struct obj *obj;
};

static struct obj **objs;
static int nobjs, cap_objs;
static struct isec **isecs;
static int nisecs, cap_isecs;

#define NHASH 4096
static struct gsym *hash[NHASH];
static struct gsym **gsyms;
static int ngsyms, cap_gsyms;

static unsigned hashstr(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % NHASH;
}

static struct gsym *gsym_find(const char *name, int create)
{
    unsigned h = hashstr(name);
    for (struct gsym *g = hash[h]; g; g = g->next)
        if (!strcmp(g->name, name))
            return g;
    if (!create)
        return NULL;
    struct gsym *g = xmalloc(sizeof(*g));
    g->name = name;
    g->next = hash[h];
    hash[h] = g;
    PUSH(gsyms, ngsyms, cap_gsyms, g);
    return g;
}

static int dropped_section(const char *name)
{
    return !strcmp(name, ".eh_frame") || !strncmp(name, ".note", 5) || !strcmp(name, ".comment")
           || !strcmp(name, ".riscv.attributes") || !strcmp(name, ".group");
}

static void load_object(const char *path)
{
    struct obj *o = xmalloc(sizeof(*o));
    o->path = path;
    o->buf = read_file(path, &o->size);
    o->eh = (Ehdr *)o->buf;
    if (o->size < sizeof(Ehdr) || memcmp(o->eh->ident, "\177ELF", 4) || o->eh->ident[4] != 2)
        die("%s: not a 64-bit ELF file", path);
    if (o->eh->machine != 243 || o->eh->type != 1)
        die("%s: not a RISC-V relocatable object", path);
    if (o->eh->shoff + (uint64_t)o->eh->shnum * sizeof(Shdr) > o->size)
        die("%s: bad section headers", path);
    o->sh = (Shdr *)(o->buf + o->eh->shoff);
    o->shstr = o->buf + o->sh[o->eh->shstrndx].offset;
    o->secs = xmalloc(o->eh->shnum * sizeof(*o->secs));
    for (int i = 1; i < o->eh->shnum; i++) {
        Shdr *s = &o->sh[i];
        if (s->type == SHT_SYMTAB) {
            o->syms = (Sym *)(o->buf + s->offset);
            o->nsyms = s->size / sizeof(Sym);
            o->strtab = o->buf + o->sh[s->link].offset;
            continue;
        }
        if (s->type != SHT_PROGBITS && s->type != SHT_NOBITS && s->type < 14)
            continue;           /* relocations, string tables, ... */
        if (s->type != SHT_PROGBITS && s->type != SHT_NOBITS && !(s->flags & SHF_ALLOC))
            continue;           /* attributes and other metadata */
        const char *name = o->shstr + s->name;
        if (dropped_section(name))
            continue;
        struct isec *is = xmalloc(sizeof(*is));
        is->obj = o;
        is->index = i;
        is->name = name;
        is->type = s->type;
        is->flags = s->flags;
        is->size = s->size;
        is->align = s->addralign ? s->addralign : 1;
        is->data = s->type == SHT_NOBITS ? NULL : (const unsigned char *)o->buf + s->offset;
        o->secs[i] = is;
        PUSH(isecs, nisecs, cap_isecs, is);
    }
    if (!o->syms)
        die("%s: no symbol table", path);
    o->globals = xmalloc(o->nsyms * sizeof(*o->globals));
    for (int i = 1; i < o->nsyms; i++) {
        Sym *s = &o->syms[i];
        if (ELF_BIND(s->info) == STB_LOCAL)
            continue;
        const char *name = o->strtab + s->name;
        struct gsym *g = gsym_find(name, 1);
        o->globals[i] = g;
        if (s->shndx == SHN_UNDEF)
            continue;
        int weak = ELF_BIND(s->info) == STB_WEAK;
        if (s->shndx == SHN_COMMON) {
            if (g->defined)
                continue;       /* a real definition (or an earlier common) wins */
            struct isec *is = xmalloc(sizeof(*is));
            is->obj = o;
            is->index = -1;
            is->name = "COMMON";
            is->type = SHT_NOBITS;
            is->flags = SHF_ALLOC | SHF_WRITE;
            is->size = s->size;
            is->align = s->value ? s->value : 1;
            PUSH(isecs, nisecs, cap_isecs, is);
            g->defined = 1;
            g->weak = 1;        /* a later real definition replaces it */
            g->sec = is;
            g->value = 0;
            g->type = STT_OBJECT;
            g->size = s->size;
            g->obj = o;
            continue;
        }
        if (g->defined && !g->weak) {
            if (weak)
                continue;
            die("%s: %s is also defined in %s", path, name, g->obj->path);
        }
        if (g->defined && g->sec && g->sec->index == -1)
            g->sec->size = 0;   /* drop the common symbol's space */
        g->defined = 1;
        g->weak = weak;
        g->obj = o;
        g->type = ELF_TYPE(s->info);
        g->size = s->size;
        if (s->shndx == SHN_ABS) {
            g->absolute = 1;
            g->sec = NULL;
        } else if (s->shndx < o->eh->shnum && o->secs[s->shndx]) {
            g->sec = o->secs[s->shndx];
        } else {
            die("%s: %s is in a section kld drops (%s)", path, name,
                s->shndx < o->eh->shnum ? o->shstr + o->sh[s->shndx].name : "?");
        }
        g->value = s->value;
    }
    PUSH(objs, nobjs, cap_objs, o);
}

/* ---- the linker script ---- */

enum { E_NUM, E_SYM, E_DOT, E_ALIGN, E_BIN };

struct expr {
    int kind, op;
    uint64_t num;
    const char *name;
    struct expr *a, *b;
};

enum { S_ASSIGN, S_OSEC, S_INPUT };

struct stmt {
    int kind;
    /* S_ASSIGN */
    const char *sym;            /* "." for the location counter */
    int provide;
    struct expr *value;
    /* S_OSEC */
    const char *name;
    int noload;
    struct expr *at;
    struct stmt **body;
    int nbody, cap_body;
    /* S_INPUT */
    const char **pats;
    int npats, cap_pats;
};

static struct stmt **script;
static int nscript, cap_script;
static const char *entry_name = "_start";
static const char *script_path;
static const char *sp;          /* script cursor */
static int sline = 1;

static void serr(const char *msg)
{
    die("%s:%d: %s", script_path, sline, msg);
}

static void skipws(void)
{
    for (;;) {
        while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == '\r') {
            if (*sp == '\n')
                sline++;
            sp++;
        }
        if (sp[0] == '/' && sp[1] == '*') {
            const char *e = strstr(sp + 2, "*/");
            if (!e)
                serr("unterminated comment");
            for (; sp < e; sp++)
                if (*sp == '\n')
                    sline++;
            sp = e + 2;
            continue;
        }
        return;
    }
}

static int is_name_char(int c, int first)
{
    return c == '_' || c == '.' || c == '$' || c == '*' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (!first && c >= '0' && c <= '9');
}

/* the next name, or NULL (and nothing consumed) */
static const char *name_tok(void)
{
    skipws();
    if (!is_name_char(*sp, 1))
        return NULL;
    const char *s = sp;
    while (is_name_char(*sp, 0))
        sp++;
    char *n = xmalloc(sp - s + 1);
    memcpy(n, s, sp - s);
    return n;
}

static int peek(int c)
{
    skipws();
    return *sp == c;
}

static void expect(int c)
{
    skipws();
    if (*sp != c) {
        char msg[32];
        snprintf(msg, sizeof(msg), "expected '%c'", c);
        serr(msg);
    }
    sp++;
}

static int accept(int c)
{
    if (peek(c)) {
        sp++;
        return 1;
    }
    return 0;
}

static struct expr *new_expr(int kind)
{
    struct expr *e = xmalloc(sizeof(*e));
    e->kind = kind;
    return e;
}

static struct expr *parse_expr(void);

static struct expr *parse_primary(void)
{
    skipws();
    if (accept('(')) {
        struct expr *e = parse_expr();
        expect(')');
        return e;
    }
    if (*sp >= '0' && *sp <= '9') {
        char *end;
        struct expr *e = new_expr(E_NUM);
        e->num = strtoull(sp, &end, 0);
        sp = end;
        if (*sp == 'K' || *sp == 'k')
            e->num <<= 10, sp++;
        else if (*sp == 'M' || *sp == 'm')
            e->num <<= 20, sp++;
        return e;
    }
    const char *n = name_tok();
    if (!n)
        serr("expected an expression");
    if (!strcmp(n, "."))
        return new_expr(E_DOT);
    if (!strcmp(n, "ALIGN")) {
        struct expr *e = new_expr(E_ALIGN);
        expect('(');
        e->a = parse_expr();
        expect(')');
        return e;
    }
    struct expr *e = new_expr(E_SYM);
    e->name = n;
    return e;
}

static struct expr *parse_term(void)
{
    struct expr *e = parse_primary();
    while (peek('*') || peek('/')) {
        struct expr *b = new_expr(E_BIN);
        b->op = *sp++;
        b->a = e;
        b->b = parse_primary();
        e = b;
    }
    return e;
}

static struct expr *parse_expr(void)
{
    struct expr *e = parse_term();
    while (peek('+') || peek('-')) {
        struct expr *b = new_expr(E_BIN);
        b->op = *sp++;
        b->a = e;
        b->b = parse_term();
        e = b;
    }
    return e;
}

static struct stmt *new_stmt(int kind)
{
    struct stmt *s = xmalloc(sizeof(*s));
    s->kind = kind;
    return s;
}

/* `sym = expr;` (name already read) */
static struct stmt *parse_assign(const char *name, int provide)
{
    struct stmt *s = new_stmt(S_ASSIGN);
    s->sym = name;
    s->provide = provide;
    expect('=');
    s->value = parse_expr();
    return s;
}

/* PROVIDE(sym = expr) */
static struct stmt *parse_provide(void)
{
    expect('(');
    const char *n = name_tok();
    if (!n)
        serr("expected a symbol");
    struct stmt *s = parse_assign(n, 1);
    expect(')');
    accept(';');
    return s;
}

/* *(pattern ...) */
static struct stmt *parse_input(void)
{
    struct stmt *s = new_stmt(S_INPUT);
    expect('(');
    while (!peek(')')) {
        const char *n = name_tok();
        if (!n)
            serr("expected a section name pattern");
        PUSH(s->pats, s->npats, s->cap_pats, n);
    }
    expect(')');
    return s;
}

static struct stmt *parse_section_item(void)
{
    const char *n = name_tok();
    if (!n)
        serr("expected a statement in an output section");
    if (!strcmp(n, "PROVIDE"))
        return parse_provide();
    if (!strcmp(n, "KEEP")) {
        expect('(');
        const char *f = name_tok();
        if (!f || strcmp(f, "*"))
            serr("expected *(...) in KEEP");
        struct stmt *s = parse_input();
        expect(')');
        return s;
    }
    if (!strcmp(n, "*"))
        return parse_input();
    struct stmt *s = parse_assign(n, 0);
    expect(';');
    return s;
}

static void parse_sections(void)
{
    expect('{');
    while (!accept('}')) {
        const char *n = name_tok();
        if (!n)
            serr("expected a statement in SECTIONS");
        if (!strcmp(n, "PROVIDE")) {
            PUSH(script, nscript, cap_script, parse_provide());
            continue;
        }
        if (peek('=')) {
            struct stmt *s = parse_assign(n, 0);
            expect(';');
            PUSH(script, nscript, cap_script, s);
            continue;
        }
        /* an output section: name [(NOLOAD)] : [AT(expr)] { ... } */
        struct stmt *s = new_stmt(S_OSEC);
        s->name = n;
        if (accept('(')) {
            const char *t = name_tok();
            if (!t || strcmp(t, "NOLOAD"))
                serr("expected (NOLOAD)");
            expect(')');
            s->noload = 1;
        }
        expect(':');
        skipws();
        if (!strncmp(sp, "AT", 2) && !is_name_char(sp[2], 0)) {
            sp += 2;
            expect('(');
            s->at = parse_expr();
            expect(')');
        }
        expect('{');
        while (!accept('}'))
            PUSH(s->body, s->nbody, s->cap_body, parse_section_item());
        PUSH(script, nscript, cap_script, s);
    }
}

static void parse_script(const char *path)
{
    size_t size;
    char *text = read_file(path, &size);
    text = xrealloc(text, size + 1);
    text[size] = 0;
    script_path = path;
    sp = text;
    for (;;) {
        skipws();
        if (!*sp)
            break;
        const char *n = name_tok();
        if (!n)
            serr("syntax error");
        if (!strcmp(n, "SECTIONS")) {
            parse_sections();
        } else if (!strcmp(n, "ENTRY")) {
            expect('(');
            entry_name = name_tok();
            expect(')');
        } else if (!strcmp(n, "OUTPUT_ARCH")) {
            expect('(');
            name_tok();
            expect(')');
        } else if (!strcmp(n, "PROVIDE")) {
            PUSH(script, nscript, cap_script, parse_provide());
        } else {
            struct stmt *s = parse_assign(n, 0);
            expect(';');
            PUSH(script, nscript, cap_script, s);
        }
    }
}

/* ---- layout ---- */

struct osec {
    const char *name;
    uint32_t type;
    uint64_t flags, addr, lma, size, align;
    struct isec **in;
    int nin, cap_in;
    unsigned char *data;        /* contents (PROGBITS) */
    uint64_t offset;            /* in the output file */
    int shndx;
};

static struct osec **osecs;
static int nosecs, cap_osecs;
static uint64_t dot;
static struct osec *cur_osec;

/* a symbol from the script: absolute here, made section-relative later */
static void script_define(const char *name, uint64_t value, int provide)
{
    struct gsym *g = gsym_find(name, 1);
    if (g->defined && !g->script) {
        if (provide)
            return;             /* PROVIDE: only if nothing defines it */
        die("%s: %s is defined by %s too", script_path, name, g->obj->path);
    }
    g->defined = 1;
    g->script = 1;
    g->absolute = 1;
    g->value = value;
    g->sec = NULL;
}

static uint64_t sym_value(struct gsym *g);

static uint64_t eval(struct expr *e)
{
    switch (e->kind) {
    case E_NUM:
        return e->num;
    case E_DOT:
        return dot;
    case E_ALIGN:
        return align_up(dot, eval(e->a));
    case E_SYM: {
        struct gsym *g = gsym_find(e->name, 0);
        if (!g || !g->defined)
            die("%s: %s is not defined (yet)", script_path, e->name);
        return sym_value(g);
    }
    default: {
        uint64_t a = eval(e->a), b = eval(e->b);
        switch (e->op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        default:
            if (!b)
                die("%s: division by zero", script_path);
            return a / b;
        }
    }
    }
}

static uint64_t sym_value(struct gsym *g)
{
    if (!g->defined)
        return 0;
    if (g->absolute || !g->sec)
        return g->value;
    return g->sec->addr + g->value;
}

static void do_assign(struct stmt *s)
{
    uint64_t v = eval(s->value);
    if (!strcmp(s->sym, ".")) {
        if (cur_osec && v < dot)
            die("%s: . moves backwards in %s", script_path, cur_osec->name);
        dot = v;
    } else {
        script_define(s->sym, v, s->provide);
    }
}

static struct osec *find_osec(const char *name)
{
    for (int i = 0; i < nosecs; i++)
        if (!strcmp(osecs[i]->name, name))
            return osecs[i];
    return NULL;
}

static int pattern_matches(struct stmt *in, struct isec *is)
{
    for (int k = 0; k < in->npats; k++) {
        if (!strcmp(in->pats[k], "COMMON") ? is->index == -1 : is->index != -1 && match(in->pats[k], is->name))
            return 1;
    }
    return 0;
}

static struct isec *ksyms_isec;     /* --ksyms: the synthesized table */

/* Assign the input sections to output sections, in script order: the first
   statement that matches a section gets it (as in GNU ld). */
static void assign_sections(void)
{
    for (int i = 0; i < nscript; i++) {
        struct stmt *s = script[i];
        if (s->kind != S_OSEC)
            continue;
        struct osec *o = find_osec(s->name);
        if (o)
            die("%s: output section %s twice", script_path, s->name);
        o = xmalloc(sizeof(*o));
        o->name = s->name;
        o->type = s->noload ? SHT_NOBITS : 0;
        o->align = 1;
        PUSH(osecs, nosecs, cap_osecs, o);
        for (int j = 0; j < s->nbody; j++) {
            struct stmt *in = s->body[j];
            if (in->kind != S_INPUT)
                continue;
            for (int k = 0; k < nisecs; k++) {
                struct isec *is = isecs[k];
                if (is->osec || !(is->flags & SHF_ALLOC) || !pattern_matches(in, is))
                    continue;
                is->osec = o;
            }
        }
        if (ksyms_isec && !strcmp(s->name, ".ksyms"))
            ksyms_isec->osec = o;
    }
    for (int k = 0; k < nisecs; k++) {
        struct isec *is = isecs[k];
        if ((is->flags & SHF_ALLOC) && !is->osec && is->size)
            die("%s: section %s is not placed by %s", is->obj->path, is->name, script_path);
    }
}

/* Lay out the allocated sections by the script: addresses, sizes, symbols. */
static void layout(void)
{
    uint64_t lma_delta = 0;     /* VMA - LMA, carried from section to section */
    dot = 0;
    for (int i = 0; i < nscript; i++) {
        struct stmt *s = script[i];
        if (s->kind == S_ASSIGN) {
            cur_osec = NULL;
            do_assign(s);
            continue;
        }
        struct osec *o = find_osec(s->name);
        o->nin = 0;
        o->align = 1;
        o->flags = SHF_ALLOC;
        int nobits = 1, any = 0;
        /* gather the inputs in statement, then object order */
        for (int j = 0; j < s->nbody; j++) {
            struct stmt *in = s->body[j];
            if (in->kind != S_INPUT)
                continue;
            for (int k = 0; k < nisecs; k++) {
                struct isec *is = isecs[k];
                if (is->osec == o && is != ksyms_isec && pattern_matches(in, is)) {
                    if (o->align < is->align)
                        o->align = is->align;
                }
            }
        }
        if (ksyms_isec && ksyms_isec->osec == o && o->align < ksyms_isec->align)
            o->align = ksyms_isec->align;
        o->addr = align_up(dot, o->align);
        if (s->at)
            lma_delta = o->addr - eval(s->at);
        o->lma = o->addr - lma_delta;
        dot = o->addr;
        cur_osec = o;
        int placed_ksyms = 0;
        for (int j = 0; j < s->nbody; j++) {
            struct stmt *in = s->body[j];
            if (in->kind == S_ASSIGN) {
                do_assign(in);
                continue;
            }
            for (int k = 0; k < nisecs; k++) {
                struct isec *is = isecs[k];
                if (is->osec != o || is == ksyms_isec || !pattern_matches(in, is))
                    continue;
                /* placed by the first statement that matches it */
                int earlier = 0;
                for (int jj = 0; jj < j && !earlier; jj++)
                    if (s->body[jj]->kind == S_INPUT && pattern_matches(s->body[jj], is))
                        earlier = 1;
                if (earlier)
                    continue;
                dot = align_up(dot, is->align);
                is->addr = dot;
                is->out_off = dot - o->addr;
                dot += is->size;
                o->flags |= is->flags & (SHF_WRITE | SHF_EXECINSTR);
                if (is->type != SHT_NOBITS && is->size)
                    nobits = 0;
                if (is->size)
                    any = 1;
                PUSH(o->in, o->nin, o->cap_in, is);
            }
            if (ksyms_isec && ksyms_isec->osec == o && !placed_ksyms) {
                placed_ksyms = 1;
                dot = align_up(dot, ksyms_isec->align);
                ksyms_isec->addr = dot;
                ksyms_isec->out_off = dot - o->addr;
                dot += ksyms_isec->size;
                nobits = 0;
                any = 1;
                PUSH(o->in, o->nin, o->cap_in, ksyms_isec);
            }
        }
        o->size = dot - o->addr;
        if (!s->noload)
            o->type = nobits ? SHT_NOBITS : SHT_PROGBITS;
        if (!any && !o->size)
            o->type = 0;        /* empty: left out, as GNU ld does */
        cur_osec = NULL;
    }
}

/* Debug and other non-allocated sections: one output section per name. */
static void layout_nonalloc(void)
{
    for (int k = 0; k < nisecs; k++) {
        struct isec *is = isecs[k];
        if (is->flags & SHF_ALLOC)
            continue;
        struct osec *o = find_osec(is->name);
        if (!o) {
            o = xmalloc(sizeof(*o));
            o->name = is->name;
            o->type = is->type;
            o->align = 1;
            PUSH(osecs, nosecs, cap_osecs, o);
        }
        if (o->align < is->align)
            o->align = is->align;
        o->size = align_up(o->size, is->align);
        is->osec = o;
        is->out_off = is->addr = o->size;
        o->size += is->size;
        PUSH(o->in, o->nin, o->cap_in, is);
    }
}

/* ---- symbols of objects ---- */

/* the value of symbol `i` of object `o` (for relocations) */
static uint64_t obj_sym_value(struct obj *o, int i, const char **name)
{
    Sym *s = &o->syms[i];
    *name = o->strtab + s->name;
    if (ELF_BIND(s->info) != STB_LOCAL) {
        struct gsym *g = o->globals[i];
        if (!g->defined) {
            if (ELF_BIND(s->info) == STB_WEAK)
                return 0;
            die("%s: undefined reference to %s", o->path, g->name);
        }
        return sym_value(g);
    }
    if (s->shndx == SHN_ABS)
        return s->value;
    if (s->shndx == SHN_UNDEF || s->shndx >= o->eh->shnum)
        return 0;
    struct isec *is = o->secs[s->shndx];
    if (!is || !is->osec) {
        if (ELF_TYPE(s->info) == STT_SECTION && s->shndx < o->eh->shnum)
            *name = o->shstr + o->sh[s->shndx].name;
        die("%s: reference to %s, in a section kld drops", o->path, *name);
    }
    return is->addr + s->value;
}

/* ---- --ksyms: the kernel symbol table ---- */

struct ksym {
    uint64_t addr;
    const char *name;
    int global;
};

static int skip_ksym(const char *n)
{
    return !*n || !strncmp(n, ".L", 2) || n[0] == '$' || !strncmp(n, "__ksym", 6)
           || !strncmp(n, "L..", 3);        /* tcc's local labels */
}

/* better(a, b): a is preferred at one address (as gen_ksyms.py ranks):
   names without leading __, then globals, then by name */
static int ksym_better(struct ksym *a, struct ksym *b)
{
    int ua = !strncmp(a->name, "__", 2), ub = !strncmp(b->name, "__", 2);
    if (ua != ub)
        return ua < ub;
    if (a->global != b->global)
        return a->global > b->global;
    return strcmp(a->name, b->name) < 0;
}

static int cmp_ksym(const void *x, const void *y)
{
    const struct ksym *a = x, *b = y;
    return a->addr < b->addr ? -1 : a->addr > b->addr;
}

static struct ksym *ksyms;
static int nksyms, cap_ksyms;

static void add_ksym(uint64_t addr, const char *name, int global)
{
    struct ksym k = { addr, name, global };
    PUSH(ksyms, nksyms, cap_ksyms, k);
}

/* the text symbols, one per address */
static void collect_ksyms(void)
{
    nksyms = 0;
    for (int i = 0; i < nobjs; i++) {
        struct obj *o = objs[i];
        for (int j = 1; j < o->nsyms; j++) {
            Sym *s = &o->syms[j];
            int t = ELF_TYPE(s->info);
            if (t == STT_SECTION || t == STT_FILE || s->shndx == SHN_UNDEF || s->shndx >= o->eh->shnum)
                continue;
            struct isec *is = o->secs[s->shndx];
            if (!is || !is->osec || !(is->flags & SHF_EXECINSTR))
                continue;
            const char *name = o->strtab + s->name;
            if (skip_ksym(name))
                continue;
            if (ELF_BIND(s->info) != STB_LOCAL) {
                struct gsym *g = o->globals[j];
                if (g->obj != o || g->sec != is)
                    continue;           /* not the definition that won */
            }
            add_ksym(is->addr + s->value, name, ELF_BIND(s->info) != STB_LOCAL);
        }
    }
    qsort(ksyms, nksyms, sizeof(*ksyms), cmp_ksym);
    int n = 0;
    for (int i = 0; i < nksyms; i++) {
        if (n && ksyms[n - 1].addr == ksyms[i].addr) {
            if (ksym_better(&ksyms[i], &ksyms[n - 1]))
                ksyms[n - 1] = ksyms[i];
        } else {
            ksyms[n++] = ksyms[i];
        }
    }
    nksyms = n;
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = v >> (8 * i);
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}

/* ksyms_num, ksyms_addrs[], ksyms_offs[] (into ksyms_names), ksyms_names */
static void build_ksyms(void)
{
    collect_ksyms();
    size_t names = 0;
    for (int i = 0; i < nksyms; i++)
        names += strlen(ksyms[i].name) + 1;
    size_t size = 8 + 8 * (size_t)nksyms + 4 * (size_t)nksyms + names;
    unsigned char *d = xmalloc(size);
    put64(d, nksyms);
    size_t off = 0;
    for (int i = 0; i < nksyms; i++) {
        put64(d + 8 + 8 * i, ksyms[i].addr);
        put32(d + 8 + 8 * (size_t)nksyms + 4 * i, off);
        strcpy((char *)d + 8 + 12 * (size_t)nksyms + off, ksyms[i].name);
        off += strlen(ksyms[i].name) + 1;
    }
    free((void *)ksyms_isec->data);
    ksyms_isec->data = d;
    ksyms_isec->size = size;
    static const char *names_[] = { "ksyms_num", "ksyms_addrs", "ksyms_offs", "ksyms_names" };
    uint64_t offs[] = { 0, 8, 8 + 8 * (uint64_t)nksyms, 8 + 12 * (uint64_t)nksyms };
    for (int i = 0; i < 4; i++) {
        struct gsym *g = gsym_find(names_[i], 1);
        if (g->defined && g->sec != ksyms_isec)
            die("--ksyms: %s is defined by %s (leave out the ksyms object)", names_[i], g->obj->path);
        g->defined = 1;
        g->sec = ksyms_isec;
        g->value = offs[i];
        g->type = STT_OBJECT;
    }
}

/* ---- relocations ---- */

enum {
    R_NONE = 0, R_32 = 1, R_64 = 2, R_BRANCH = 16, R_JAL = 17, R_CALL = 18, R_CALL_PLT = 19,
    R_GOT_HI20 = 20, R_PCREL_HI20 = 23, R_PCREL_LO12_I = 24, R_PCREL_LO12_S = 25, R_HI20 = 26,
    R_LO12_I = 27, R_LO12_S = 28, R_ADD8 = 33, R_ADD16 = 34, R_ADD32 = 35, R_ADD64 = 36,
    R_SUB8 = 37, R_SUB16 = 38, R_SUB32 = 39, R_SUB64 = 40, R_ALIGN = 43, R_RVC_BRANCH = 44,
    R_RVC_JUMP = 45, R_RELAX = 51, R_SUB6 = 52, R_SET6 = 53, R_SET8 = 54, R_SET16 = 55,
    R_SET32 = 56, R_32_PCREL = 57, R_SET_ULEB128 = 60, R_SUB_ULEB128 = 61,
};

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const unsigned char *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint16_t rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void wr16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* the auipc half of a pc-relative pair: its address and target */
struct hi20 {
    uint64_t place, target;
    int got;
    int zero;                   /* GOT access to an undefined weak symbol: 0 */
};

static int undefined_weak(struct obj *o, int i)
{
    Sym *s = &o->syms[i];
    return ELF_BIND(s->info) == STB_WEAK && !o->globals[i]->defined;
}

static void range_error(struct isec *is, uint64_t off, const char *what, const char *sym)
{
    die("%s: %s+%#llx: %s out of range for %s", is->obj->path, is->name, (unsigned long long)off, what, sym);
}

static void check_hi20(struct isec *is, uint64_t off, int64_t v, const char *sym)
{
    if (v + 0x800 < -(1LL << 31) || v + 0x800 >= (1LL << 31))
        range_error(is, off, "a 32-bit pc-relative offset", sym);
}

static void put_hi20(unsigned char *p, int64_t v)
{
    put32(p, (rd32(p) & 0xfff) | ((uint32_t)(v + 0x800) & 0xfffff000));
}

static void put_i12(unsigned char *p, int64_t v)
{
    put32(p, (rd32(p) & 0xfffff) | ((uint32_t)v & 0xfff) << 20);
}

static void put_s12(unsigned char *p, int64_t v)
{
    put32(p, (rd32(p) & 0x1fff07f) | ((uint32_t)v & 0x1f) << 7 | ((uint32_t)v >> 5 & 0x7f) << 25);
}

static void uleb_write(unsigned char *p, uint64_t v, unsigned char *end)
{
    /* keep the field's length: continuation bits as they were */
    while (p < end) {
        int more = *p & 0x80;
        *p = (v & 0x7f) | more;
        v >>= 7;
        p++;
        if (!more)
            break;
    }
}

static uint64_t uleb_read(const unsigned char *p)
{
    uint64_t v = 0;
    int shift = 0;
    do {
        v |= (uint64_t)(*p & 0x7f) << shift;
        shift += 7;
    } while (*p++ & 0x80);
    return v;
}

static void relocate_section(struct isec *is, Shdr *rsh)
{
    struct obj *o = is->obj;
    Rela *rel = (Rela *)(o->buf + rsh->offset);
    int n = rsh->size / sizeof(Rela);
    unsigned char *base = is->osec->data + is->out_off;
    struct hi20 *his = NULL;
    int nhis = 0, cap_his = 0;
    const char *name;

    /* first the auipc halves, which the %pcrel_lo halves refer to */
    for (int i = 0; i < n; i++) {
        int type = rel[i].info & 0xffffffff;
        if (type != R_PCREL_HI20 && type != R_GOT_HI20)
            continue;
        uint64_t s = obj_sym_value(o, rel[i].info >> 32, &name);
        struct hi20 h = { is->addr + rel[i].offset, s + rel[i].addend, type == R_GOT_HI20,
                          type == R_GOT_HI20 && undefined_weak(o, rel[i].info >> 32) };
        PUSH(his, nhis, cap_his, h);
    }
    for (int i = 0; i < n; i++) {
        int type = rel[i].info & 0xffffffff;
        if (type == R_NONE || type == R_RELAX)
            continue;
        if (type == R_ALIGN) {
            /* nops the assembler left for the linker to trim to an
               alignment; kld does not relax, so they must already fit */
            uint64_t a = 1, P = is->addr + rel[i].offset;
            while (a <= (uint64_t)rel[i].addend)
                a <<= 1;
            if ((P + rel[i].addend) % a)
                die("%s: %s+%#llx: alignment padding that needs linker relaxation "
                    "(build with -mno-relax)", o->path, is->name, (unsigned long long)rel[i].offset);
            continue;
        }
        if (rel[i].offset >= is->size)
            die("%s: relocation outside %s", o->path, is->name);
        uint64_t S = obj_sym_value(o, rel[i].info >> 32, &name);
        int64_t A = rel[i].addend;
        uint64_t P = is->addr + rel[i].offset;
        unsigned char *p = base + rel[i].offset;
        int64_t v = S + A - P;
        switch (type) {
        case R_32:
            put32(p, S + A);
            break;
        case R_64:
            put64(p, S + A);
            break;
        case R_32_PCREL:
            put32(p, v);
            break;
        case R_BRANCH:
            if (v < -4096 || v >= 4096 || (v & 1))
                range_error(is, rel[i].offset, "a branch", name);
            put32(p, (rd32(p) & 0x1fff07f) | ((v >> 12 & 1) << 31) | ((v >> 5 & 0x3f) << 25)
                     | ((v >> 1 & 0xf) << 8) | ((v >> 11 & 1) << 7));
            break;
        case R_JAL:
            if (v < -(1 << 20) || v >= (1 << 20) || (v & 1))
                range_error(is, rel[i].offset, "a jump", name);
            put32(p, (rd32(p) & 0xfff) | ((v >> 20 & 1) << 31) | ((v >> 1 & 0x3ff) << 21)
                     | ((v >> 11 & 1) << 20) | ((v >> 12 & 0xff) << 12));
            break;
        case R_CALL:
        case R_CALL_PLT:
            check_hi20(is, rel[i].offset, v, name);
            put_hi20(p, v);
            put_i12(p + 4, v);
            break;
        case R_GOT_HI20:
            if (undefined_weak(o, rel[i].info >> 32)) {
                /* its address is 0, out of pc-relative reach: lui rd, 0 */
                put32(p, (rd32(p) & 0xf80) | 0x37);
                break;
            }
            /* fall through */
        case R_PCREL_HI20:
            check_hi20(is, rel[i].offset, v, name);
            put_hi20(p, v);
            break;
        case R_PCREL_LO12_I:
        case R_PCREL_LO12_S: {
            /* the symbol is the auipc's label */
            struct hi20 *h = NULL;
            for (int k = 0; k < nhis; k++)
                if (his[k].place == S + A)
                    h = &his[k];
            if (!h)
                die("%s: %s+%#llx: %%pcrel_lo without its %%pcrel_hi", o->path, is->name,
                    (unsigned long long)rel[i].offset);
            int64_t lo = h->zero ? 0 : h->target - h->place;
            if (h->got) {
                /* tcc's GOT load (ld rd, lo(rd)) becomes the address itself */
                uint32_t insn = rd32(p);
                if ((insn & 0x707f) != 0x3003)
                    die("%s: %s+%#llx: GOT access is not an ld", o->path, is->name,
                        (unsigned long long)rel[i].offset);
                put32(p, (insn & ~0x707fu) | 0x13);     /* addi */
            }
            if (type == R_PCREL_LO12_I)
                put_i12(p, lo);
            else
                put_s12(p, lo);
            break;
        }
        case R_HI20:
            if ((int64_t)(S + A) + 0x800 >= (1LL << 31) || (int64_t)(S + A) + 0x800 < -(1LL << 31))
                range_error(is, rel[i].offset, "an absolute 32-bit address", name);
            put_hi20(p, S + A);
            break;
        case R_LO12_I:
            put_i12(p, S + A);
            break;
        case R_LO12_S:
            put_s12(p, S + A);
            break;
        case R_ADD8:  p[0] += S + A; break;
        case R_SUB8:  p[0] -= S + A; break;
        case R_ADD16: wr16(p, rd16(p) + S + A); break;
        case R_SUB16: wr16(p, rd16(p) - S - A); break;
        case R_ADD32: put32(p, rd32(p) + S + A); break;
        case R_SUB32: put32(p, rd32(p) - S - A); break;
        case R_ADD64: put64(p, rd64(p) + S + A); break;
        case R_SUB64: put64(p, rd64(p) - S - A); break;
        case R_SUB6:  p[0] = (p[0] & 0xc0) | ((p[0] - (S + A)) & 0x3f); break;
        case R_SET6:  p[0] = (p[0] & 0xc0) | ((S + A) & 0x3f); break;
        case R_SET8:  p[0] = S + A; break;
        case R_SET16: wr16(p, S + A); break;
        case R_SET32: put32(p, S + A); break;
        case R_SET_ULEB128:
            uleb_write(p, S + A, base + is->size);
            break;
        case R_SUB_ULEB128:
            uleb_write(p, uleb_read(p) - (S + A), base + is->size);
            break;
        case R_RVC_BRANCH:
            if (v < -256 || v >= 256 || (v & 1))
                range_error(is, rel[i].offset, "a compressed branch", name);
            wr16(p, (rd16(p) & 0xe383) | ((v >> 8 & 1) << 12) | ((v >> 3 & 3) << 10) | ((v >> 6 & 3) << 5)
                    | ((v >> 1 & 3) << 3) | ((v >> 5 & 1) << 2));
            break;
        case R_RVC_JUMP:
            if (v < -2048 || v >= 2048 || (v & 1))
                range_error(is, rel[i].offset, "a compressed jump", name);
            wr16(p, (rd16(p) & 0xe003) | ((v >> 11 & 1) << 12) | ((v >> 4 & 1) << 11) | ((v >> 8 & 3) << 9)
                    | ((v >> 10 & 1) << 8) | ((v >> 6 & 1) << 7) | ((v >> 7 & 1) << 6)
                    | ((v >> 1 & 7) << 3) | ((v >> 5 & 1) << 2));
            break;
        default:
            die("%s: %s: relocation type %d is not supported", o->path, is->name, type);
        }
    }
    free(his);
}

static void fill_and_relocate(void)
{
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (o->type == SHT_NOBITS || !o->type)
            continue;
        o->data = xmalloc(o->size);
        for (int k = 0; k < o->nin; k++) {
            struct isec *is = o->in[k];
            if (is->data)
                memcpy(o->data + is->out_off, is->data, is->size);
        }
    }
    for (int i = 0; i < nobjs; i++) {
        struct obj *o = objs[i];
        for (int j = 1; j < o->eh->shnum; j++) {
            Shdr *r = &o->sh[j];
            if (r->type != SHT_RELA || r->info >= o->eh->shnum)
                continue;
            struct isec *is = o->secs[r->info];
            if (!is || !is->osec || !is->osec->data)
                continue;       /* dropped, or NOBITS */
            relocate_section(is, r);
        }
    }
}

/* ---- output ---- */

struct strtab {
    char *buf;
    size_t len, cap;
};

static uint32_t str_add(struct strtab *t, const char *s)
{
    size_t n = strlen(s) + 1;
    if (t->len + n > t->cap) {
        t->cap = (t->len + n) * 2 + 256;
        t->buf = xrealloc(t->buf, t->cap);
    }
    if (!t->len) {
        t->buf[0] = 0;
        t->len = 1;
        if (!*s)
            return 0;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    return t->len - n;
}

static void write_output(const char *path, uint32_t eflags)
{
    struct strtab shstr = { 0 }, str = { 0 };
    Sym *syms = NULL;
    int nsyms = 0, cap_syms = 0;
    str_add(&shstr, "");
    str_add(&str, "");
    Sym null = { 0 };
    PUSH(syms, nsyms, cap_syms, null);

    /* output section indices: the kept ones in order */
    int shnum = 1;
    for (int i = 0; i < nosecs; i++)
        if (osecs[i]->type)
            osecs[i]->shndx = shnum++;
    int symtab_ndx = shnum++, strtab_ndx = shnum++, shstrtab_ndx = shnum++;

    /* symbols: locals (file by file), then globals */
    for (int i = 0; i < nobjs; i++) {
        struct obj *o = objs[i];
        for (int j = 1; j < o->nsyms; j++) {
            Sym *s = &o->syms[j];
            int t = ELF_TYPE(s->info);
            if (ELF_BIND(s->info) != STB_LOCAL || t == STT_SECTION || t == STT_FILE)
                continue;
            const char *name = o->strtab + s->name;
            if (!*name || !strncmp(name, ".L", 2) || name[0] == '$')
                continue;
            Sym out = { 0 };
            out.info = s->info;
            if (s->shndx == SHN_ABS) {
                out.shndx = SHN_ABS;
                out.value = s->value;
            } else if (s->shndx && s->shndx < o->eh->shnum && o->secs[s->shndx] && o->secs[s->shndx]->osec) {
                struct isec *is = o->secs[s->shndx];
                out.shndx = is->osec->shndx;
                out.value = is->addr + s->value;
            } else {
                continue;
            }
            out.size = s->size;
            out.name = str_add(&str, name);
            PUSH(syms, nsyms, cap_syms, out);
        }
    }
    int first_global = nsyms;
    for (int i = 0; i < ngsyms; i++) {
        struct gsym *g = gsyms[i];
        if (!g->defined)
            continue;
        Sym out = { 0 };
        out.info = (g->weak && !g->script && !(g->sec && g->sec->index == -1) ? STB_WEAK : STB_GLOBAL) << 4
                   | (g->type & 15);
        out.value = sym_value(g);
        out.size = g->size;
        if (g->sec && g->sec->osec) {
            out.shndx = g->sec->osec->shndx;
        } else if (g->script) {
            /* in the output section that holds it, else absolute */
            out.shndx = SHN_ABS;
            for (int k = 0; k < nosecs; k++) {
                struct osec *o = osecs[k];
                if (o->type && (o->flags & SHF_ALLOC) && out.value >= o->addr && out.value < o->addr + o->size)
                    out.shndx = o->shndx;
            }
        } else {
            out.shndx = SHN_ABS;
        }
        out.name = str_add(&str, g->name);
        PUSH(syms, nsyms, cap_syms, out);
    }

    /* file layout */
    int phnum = 0;
    for (int i = 0; i < nosecs; i++)
        if (osecs[i]->type && (osecs[i]->flags & SHF_ALLOC) && osecs[i]->size)
            phnum++;
    uint64_t off = sizeof(Ehdr) + phnum * sizeof(Phdr);
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (!o->type)
            continue;
        o->shndx = o->shndx;
        if (o->flags & SHF_ALLOC) {
            /* offset congruent to the address, modulo the page size */
            off += (o->addr - off) & (PAGE - 1);
        } else {
            off = align_up(off, o->align);
        }
        o->offset = off;
        if (o->type != SHT_NOBITS)
            off += o->size;
    }
    for (int i = 0; i < nosecs; i++)
        str_add(&shstr, osecs[i]->name);       /* (names found again below) */
    uint64_t symoff = align_up(off, 8);
    uint64_t stroff = symoff + nsyms * sizeof(Sym);
    uint64_t shstroff = stroff + str.len;
    uint32_t n_symtab = str_add(&shstr, ".symtab"), n_strtab = str_add(&shstr, ".strtab"),
             n_shstrtab = str_add(&shstr, ".shstrtab");
    uint64_t shoff = align_up(shstroff + shstr.len, 8);

    FILE *f = fopen(path, "wb");
    if (!f)
        die("%s: cannot create", path);
    Ehdr eh = { .ident = { 0x7f, 'E', 'L', 'F', 2, 1, 1 }, .type = 2, .machine = 243, .version = 1 };
    struct gsym *entry = gsym_find(entry_name, 0);
    if (!entry || !entry->defined)
        die("entry symbol %s is not defined", entry_name);
    eh.entry = sym_value(entry);
    eh.phoff = sizeof(Ehdr);
    eh.shoff = shoff;
    eh.flags = eflags;
    eh.ehsize = sizeof(Ehdr);
    eh.phentsize = sizeof(Phdr);
    eh.phnum = phnum;
    eh.shentsize = sizeof(Shdr);
    eh.shnum = shnum;
    eh.shstrndx = shstrtab_ndx;
    fwrite(&eh, sizeof(eh), 1, f);
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (!o->type || !(o->flags & SHF_ALLOC) || !o->size)
            continue;
        Phdr ph = { 1, 4, o->offset, o->addr, o->lma, o->type == SHT_NOBITS ? 0 : o->size, o->size, PAGE };
        if (o->flags & SHF_WRITE)
            ph.flags |= 2;
        if (o->flags & SHF_EXECINSTR)
            ph.flags |= 1;
        fwrite(&ph, sizeof(ph), 1, f);
    }
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (!o->type || o->type == SHT_NOBITS)
            continue;
        if (fseek(f, o->offset, SEEK_SET) || fwrite(o->data, 1, o->size, f) != o->size)
            die("%s: write error", path);
    }
    fseek(f, symoff, SEEK_SET);
    fwrite(syms, sizeof(Sym), nsyms, f);
    fwrite(str.buf, 1, str.len, f);
    fwrite(shstr.buf, 1, shstr.len, f);
    fseek(f, shoff, SEEK_SET);
    Shdr sh0 = { 0 };
    fwrite(&sh0, sizeof(sh0), 1, f);
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (!o->type)
            continue;
        Shdr sh = { 0 };
        /* the name was added to shstr in order; find it */
        const char *p = shstr.buf + 1;
        while (strcmp(p, o->name))
            p += strlen(p) + 1;
        sh.name = p - shstr.buf;
        sh.type = o->type;
        sh.flags = o->flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR);
        sh.addr = (o->flags & SHF_ALLOC) ? o->addr : 0;
        sh.offset = o->offset;
        sh.size = o->size;
        sh.addralign = o->align;
        fwrite(&sh, sizeof(sh), 1, f);
    }
    Shdr st = { n_symtab, SHT_SYMTAB, 0, 0, symoff, nsyms * sizeof(Sym), strtab_ndx, first_global, 8, sizeof(Sym) };
    Shdr ss = { n_strtab, SHT_STRTAB, 0, 0, stroff, str.len, 0, 0, 1, 0 };
    Shdr sss = { n_shstrtab, SHT_STRTAB, 0, 0, shstroff, shstr.len, 0, 0, 1, 0 };
    fwrite(&st, sizeof(st), 1, f);
    fwrite(&ss, sizeof(ss), 1, f);
    fwrite(&sss, sizeof(sss), 1, f);
    (void)symtab_ndx;
    if (fclose(f))
        die("%s: write error", path);
}

static void print_map(void)
{
    for (int i = 0; i < nosecs; i++) {
        struct osec *o = osecs[i];
        if (!o->type)
            continue;
        printf("%-16s %016llx %08llx  lma %016llx  align %llu\n", o->name, (unsigned long long)o->addr,
               (unsigned long long)o->size, (unsigned long long)o->lma, (unsigned long long)o->align);
        for (int k = 0; k < o->nin; k++) {
            struct isec *is = o->in[k];
            printf("    %016llx %08llx %s(%s)\n", (unsigned long long)is->addr, (unsigned long long)is->size,
                   is->obj ? is->obj->path : "kld", is->name);
        }
    }
}

int main(int argc, char **argv)
{
    const char *out = "a.out", *ldscript = NULL;
    int want_ksyms = 0, map = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-T") && i + 1 < argc)
            ldscript = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--ksyms"))
            want_ksyms = 1;
        else if (!strcmp(argv[i], "-M"))
            map = 1;
        else if (argv[i][0] == '-')
            die("usage: kld -T script -o output [--ksyms] [-M] file.o ...");
        else
            load_object(argv[i]);
    }
    if (!ldscript || !nobjs)
        die("usage: kld -T script -o output [--ksyms] [-M] file.o ...");
    parse_script(ldscript);
    if (want_ksyms) {
        ksyms_isec = xmalloc(sizeof(*ksyms_isec));
        ksyms_isec->index = -2;
        ksyms_isec->name = ".ksyms";
        ksyms_isec->type = SHT_PROGBITS;
        ksyms_isec->flags = SHF_ALLOC;
        ksyms_isec->align = 8;
        ksyms_isec->data = xmalloc(1);
    }
    assign_sections();
    if (want_ksyms && !ksyms_isec->osec)
        die("--ksyms: %s has no .ksyms output section", ldscript);
    layout();
    if (want_ksyms) {
        /* the table depends on the addresses, and its size on nothing but
           itself: .ksyms is last, so laying out again moves no text */
        build_ksyms();
        uint64_t first = nksyms ? ksyms[0].addr : 0, last = nksyms ? ksyms[nksyms - 1].addr : 0;
        layout();
        build_ksyms();
        if (nksyms && (ksyms[0].addr != first || ksyms[nksyms - 1].addr != last))
            die("--ksyms: text moved when the symbol table grew (is .ksyms last?)");
    }
    layout_nonalloc();
    fill_and_relocate();
    write_output(out, objs[0]->eh->flags);
    if (map)
        print_map();
    return 0;
}
