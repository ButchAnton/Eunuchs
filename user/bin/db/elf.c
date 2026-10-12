/* db: reading the program file (ELF64): sections and symbols. */
#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct program prog;

struct ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct shdr {
    u32 name, type;
    u64 flags, addr, offset, size;
    u32 link, info;
    u64 addralign, entsize;
};

struct esym {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
};

#define SHT_SYMTAB 2
#define SHT_NOBITS 8
#define SHF_EXECINSTR 4
#define STT_OBJECT 1
#define STT_FUNC 2

static int cmp_sym(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    /* functions before other symbols at the same address */
    return y->is_func - x->is_func;
}

int elf_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        err("%s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (long)sizeof(struct ehdr)) {
        err("%s: not an ELF file", path);
        close(fd);
        return -1;
    }
    u8 *img = xmalloc(st.st_size);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        long n = read(fd, img + got, st.st_size - got);
        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    struct ehdr *eh = (struct ehdr *)img;
    if (got != (size_t)st.st_size || memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 2 ||
        eh->machine != 243) {
        err("%s: not a RISC-V 64-bit ELF file", path);
        free(img);
        return -1;
    }
    if (eh->shoff + (u64)eh->shnum * sizeof(struct shdr) > got || eh->shstrndx >= eh->shnum) {
        err("%s: bad section headers", path);
        free(img);
        return -1;
    }
    memset(&prog, 0, sizeof(prog));
    prog.path = xstrdup(path);
    prog.image = img;
    prog.image_size = got;
    prog.entry = eh->entry;
    struct shdr *sh = (struct shdr *)(img + eh->shoff);
    const char *shstr = (const char *)img + sh[eh->shstrndx].offset;
    struct {
        const char *name;
        struct section *s;
    } wanted[] = {
        { ".debug_info", &prog.debug_info },       { ".debug_abbrev", &prog.debug_abbrev },
        { ".debug_line", &prog.debug_line },       { ".debug_str", &prog.debug_str },
        { ".debug_line_str", &prog.debug_line_str }, { ".debug_loclists", &prog.debug_loclists },
        { ".debug_loc", &prog.debug_loc },         { ".debug_ranges", &prog.debug_ranges },
        { ".debug_rnglists", &prog.debug_rnglists }, { ".debug_addr", &prog.debug_addr },
        { ".debug_str_offsets", &prog.debug_str_offsets },
    };
    prog.text_start = ~0ULL;
    struct shdr *symtab = 0;
    for (int i = 0; i < eh->shnum; i++) {
        const char *name = shstr + sh[i].name;
        if (sh[i].type != SHT_NOBITS && sh[i].offset + sh[i].size > got)
            continue;
        for (unsigned k = 0; k < sizeof(wanted) / sizeof(wanted[0]); k++)
            if (!strcmp(name, wanted[k].name)) {
                wanted[k].s->name = wanted[k].name;
                wanted[k].s->data = img + sh[i].offset;
                wanted[k].s->size = sh[i].size;
                wanted[k].s->addr = sh[i].addr;
            }
        if (sh[i].type == SHT_SYMTAB)
            symtab = &sh[i];
        if ((sh[i].flags & SHF_EXECINSTR) && sh[i].addr) {
            if (sh[i].addr < prog.text_start)
                prog.text_start = sh[i].addr;
            if (sh[i].addr + sh[i].size > prog.text_end)
                prog.text_end = sh[i].addr + sh[i].size;
        }
    }
    if (symtab && symtab->link < eh->shnum) {
        struct esym *es = (struct esym *)(img + symtab->offset);
        int n = symtab->size / sizeof(struct esym);
        const char *strs = (const char *)img + sh[symtab->link].offset;
        prog.syms = xcalloc(n ? n : 1, sizeof(struct sym));
        for (int i = 0; i < n; i++) {
            int type = es[i].info & 15;
            if (!es[i].value || !es[i].name || es[i].shndx == 0)
                continue;
            if (type != STT_FUNC && type != STT_OBJECT && type != 0)
                continue;
            const char *name = strs + es[i].name;
            if (!name[0] || name[0] == '$' || !strncmp(name, ".L", 2) || !strncmp(name, "L.", 2))
                continue;               /* mapping symbols and local labels (gcc's .L, tcc's L.) */
            struct sym *s = &prog.syms[prog.nsyms++];
            s->name = xstrdup(name);
            s->addr = es[i].value;
            s->size = es[i].size;
            s->is_func = type == STT_FUNC ||
                         (type == 0 && s->addr >= prog.text_start && s->addr < prog.text_end);
        }
        qsort(prog.syms, prog.nsyms, sizeof(struct sym), cmp_sym);
    }
    return 0;
}

/* The symbol at or before addr that covers it (sized symbols), or the nearest
 * function before it in the text (unsized ones, like _start). */
struct sym *sym_at(u64 addr)
{
    int lo = 0, hi = prog.nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (prog.syms[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    for (int i = best; i >= 0 && i > best - 64; i--) {
        struct sym *s = &prog.syms[i];
        if (addr < s->addr + (s->size ? s->size : 1) || (!s->size && s->is_func && i == best))
            return s->size || (addr >= prog.text_start && addr < prog.text_end) ? s : 0;
    }
    return 0;
}

struct sym *sym_named(const char *name)
{
    for (int i = 0; i < prog.nsyms; i++)
        if (!strcmp(prog.syms[i].name, name))
            return &prog.syms[i];
    return 0;
}
