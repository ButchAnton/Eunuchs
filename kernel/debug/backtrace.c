/*
 * Frame-pointer unwinder. With -fno-omit-frame-pointer, GCC stores every
 * frame's record just below its frame pointer:
 *     fp - 8  : return address
 *     fp - 16 : caller's fp
 * The trap entry builds the same record so unwinding crosses traps.
 * All reads go through safe_read_u64, so a corrupt chain cannot fault.
 */
#include <debug.h>
#include <fmt.h>
#include <riscv.h>
#include <proc.h>
#include <safe.h>

int backtrace_collect(uintptr_t fp, uintptr_t *pcs, int max)
{
    int n = 0;
    while (n < max && fp && !(fp & 7)) {
        u64 ra, next;
        if (safe_read_u64(fp - 8, &ra) || safe_read_u64(fp - 16, &next))
            break;
        if (!ra)
            break;
        pcs[n++] = ra;
        if (next <= fp)     /* stacks grow down: callers live higher */
            break;
        fp = next;
    }
    return n;
}

/* A return address follows the call; symbolize ra - 1 so noreturn calls at a
 * function's very end resolve to the caller, then report the offset of ra. */
static void format_return(char *buf, size_t size, uintptr_t ra)
{
    uintptr_t off;
    const char *name = ksym_lookup(ra - 1, &off);
    struct proc *p = current_proc();
    if (!name && ra < USER_TOP && p && p->image) {
        /* A frame in user space (backtraces through a syscall). */
        name = elf_symbolize(p->image, p->image_size, ra - 1, &off);
        if (name) {
            ksnprintf(buf, size, "[user %s] %s+%#lx", p->name, name, (unsigned long)(off + 1));
            return;
        }
    }
    if (name)
        ksnprintf(buf, size, "%s+%#lx", name, (unsigned long)(off + 1));
    else
        ksnprintf(buf, size, "%#lx", (unsigned long)ra);
}

void backtrace_print(print_fn print, uintptr_t pc, uintptr_t fp)
{
    uintptr_t pcs[BACKTRACE_MAX];
    char sym[96];

    ksym_format(sym, sizeof(sym), pc);
    print("  #0  %016lx  %s\n", (unsigned long)pc, sym);

    int n = backtrace_collect(fp, pcs, BACKTRACE_MAX - 1);
    for (int i = 0; i < n; i++) {
        format_return(sym, sizeof(sym), pcs[i]);
        print("  #%-2d %016lx  %s\n", i + 1, (unsigned long)pcs[i], sym);
        /* Collapse runs of identical frames (recursion). */
        int j = i;
        while (j + 1 < n && pcs[j + 1] == pcs[i])
            j++;
        if (j - i >= 2) {
            print("  ... frame repeated %d more times (recursion)\n", j - i);
            i = j;
        }
    }
    if (n == BACKTRACE_MAX - 1)
        print("  ... (truncated)\n");
}

void backtrace_here(print_fn print)
{
    u64 caller_fp = 0;
    safe_read_u64(current_fp() - 16, &caller_fp);
    backtrace_print(print, (uintptr_t)__builtin_return_address(0) - 2, caller_fp);
}
