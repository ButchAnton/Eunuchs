/*
 * ELF64 (RISC-V, static ET_EXEC) loader and symbolizer for user programs.
 * Load errors come back as a human-readable reason (shown by exec).
 */
#include <fmt.h>
#include <kstring.h>
#include <proc.h>
#include <uapi/errno.h>

#define EM_RISCV  243
#define ET_EXEC   2
#define PT_LOAD   1
#define PF_X      1
#define PF_W      2
#define PF_R      4
#define SHT_SYMTAB 2
#define STT_FUNC  2

struct elf64_ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

struct elf64_shdr {
    u32 name, type;
    u64 flags, addr, offset, size;
    u32 link, info;
    u64 addralign, entsize;
};

struct elf64_sym {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
};

#define FAIL(...) do { ksnprintf(err, errlen, __VA_ARGS__); return -ENOEXEC; } while (0)

int elf_load(struct mm *mm, const u8 *img, size_t size, uintptr_t *entry, uintptr_t *end,
             char *err, size_t errlen)
{
    const struct elf64_ehdr *eh = (const void *)img;
    uintptr_t top = 0;

    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4))
        FAIL("not an ELF file");
    if (eh->ident[4] != 2 || eh->ident[5] != 1)
        FAIL("not a 64-bit little-endian ELF");
    if (eh->machine != EM_RISCV)
        FAIL("wrong machine type %u (want RISC-V)", eh->machine);
    if (eh->type != ET_EXEC)
        FAIL("not a static executable (type %u)", eh->type);
    if (eh->phoff + (u64)eh->phnum * sizeof(struct elf64_phdr) > size)
        FAIL("program headers outside the file");

    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(img + eh->phoff + i * sizeof(*ph));
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size)
            FAIL("segment %d: file data out of range", i);
        if (ph->vaddr < USER_MIN_ADDR || ph->vaddr + ph->memsz > USER_STACK_TOP - USER_STACK_SIZE)
            FAIL("segment %d at %#lx-%#lx outside user program space", i,
                 (unsigned long)ph->vaddr, (unsigned long)(ph->vaddr + ph->memsz));
        if ((ph->flags & PF_W) && (ph->flags & PF_X))
            FAIL("segment %d is writable and executable (W^X)", i);

        uintptr_t start = ALIGN_DOWN(ph->vaddr, PAGE_SIZE);
        uintptr_t stop = PAGE_ALIGN(ph->vaddr + ph->memsz);
        u32 prot = (ph->flags & PF_R ? VM_READ : 0) | (ph->flags & PF_W ? VM_WRITE : 0) |
                   (ph->flags & PF_X ? VM_EXEC : 0);
        int r = vma_add(mm, start, stop, prot, VMA_ELF,
                        ph->flags & PF_X ? "text" : ph->flags & PF_W ? "data" : "rodata");
        if (r == -EEXIST)
            FAIL("segment %d shares a page with another segment (link with page-aligned segments)", i);
        if (r)
            FAIL("segment %d: cannot create mapping (%d)", i, r);

        /* Load eagerly: copy file bytes; the rest (bss) stays zero. */
        for (uintptr_t va = start; va < stop; va += PAGE_SIZE) {
            u8 *page = uvm_populate(mm, va);
            if (!page)
                FAIL("out of memory loading segment %d", i);
            uintptr_t lo = MAX(va, (uintptr_t)ph->vaddr);
            uintptr_t hi = MIN(va + PAGE_SIZE, (uintptr_t)(ph->vaddr + ph->filesz));
            if (lo < hi)
                memcpy(page + (lo - va), img + ph->offset + (lo - ph->vaddr), hi - lo);
        }
        top = MAX(top, stop);
    }
    if (!top)
        FAIL("no loadable segments");
    struct vma *ev = vma_find(mm, eh->entry);
    if (!ev || !(ev->prot & VM_EXEC))
        FAIL("entry point %#lx is not in an executable segment", (unsigned long)eh->entry);
    *entry = eh->entry;
    *end = top;
    return 0;
}

const char *elf_symbolize(const void *image, size_t size, uintptr_t addr, uintptr_t *off)
{
    const u8 *img = image;
    const struct elf64_ehdr *eh = image;
    const char *best = NULL;
    u64 best_val = 0;

    if (!img || size < sizeof(*eh) || eh->shoff + (u64)eh->shnum * sizeof(struct elf64_shdr) > size)
        return NULL;
    const struct elf64_shdr *sh = (const void *)(img + eh->shoff);
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].type != SHT_SYMTAB || sh[i].link >= eh->shnum)
            continue;
        const struct elf64_shdr *strtab = &sh[sh[i].link];
        const struct elf64_sym *syms = (const void *)(img + sh[i].offset);
        for (u64 j = 0; j < sh[i].size / sizeof(*syms); j++) {
            const struct elf64_sym *s = &syms[j];
            /* Functions, plus untyped globals (assembly labels). */
            bool func = (s->info & 0xf) == STT_FUNC || ((s->info & 0xf) == 0 && (s->info >> 4) == 1);
            if (!func || !s->value || s->value > addr)
                continue;
            if (s->size && addr >= s->value + s->size)
                continue;
            if (!best || s->value > best_val) {
                best = (const char *)img + strtab->offset + s->name;
                best_val = s->value;
            }
        }
    }
    if (best && off)
        *off = addr - best_val;
    return best;
}
