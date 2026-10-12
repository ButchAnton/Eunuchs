/*
 * System call dispatch, the simple system calls, and strace.
 *
 * ABI: ecall with a7 = number, a0..a5 = arguments; result in a0, negative
 * errno on failure. The table is generated from lib/include/uapi/syscall.h.
 *
 * strace (per process: DBG_STRACE from user space, `strace <pid>` in the
 * monitor) logs every call with decoded arguments and the result, e.g.
 *   [3 sh] write(1, "hello\n", 6) = 6
 *   [3 sh] execve("/bin/nope", ["nope"], 0x0) = -1 ENOENT (No such file or directory)
 */
#define LOG_SUBSYS SUBSYS_SYSCALL
#include <console.h>
#include <fs.h>
#include <debug.h>
#include <fmt.h>
#include <irq.h>
#include <kstring.h>
#include <platform.h>
#include <printk.h>
#include <proc.h>
#include <profile.h>
#include <riscv.h>
#include <sbi.h>
#include <trace.h>
#include <trap.h>
#include <uapi/errno.h>
#include <uapi/reboot.h>
#include <uapi/syscall.h>

typedef long (*sys_fn)(const u64 *a);

/* Every handler: long do_<name>(const u64 *args). File syscalls live in fs/file.c. */
#define X(nr, name, nargs, fmt) long do_##name(const u64 *a);
SYSCALLS(X)
#undef X

/* ---- simple system calls ---- */

long do_exit(const u64 *a)
{
    proc_exit(WEXITSTATUS_ENC((int)a[0]));
}

long do_sched_yield(const u64 *a) { (void)a; yield(); return 0; }
long do_getpid(const u64 *a) { (void)a; return current_proc()->pid; }

long do_getppid(const u64 *a)
{
    (void)a;
    unsigned long flags = spin_lock_irqsave(&proc_lock);    /* the parent may be exiting */
    struct proc *pp = current_proc()->parent;
    long ppid = pp ? pp->pid : 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    return ppid;
}

long do_brk(const u64 *a)
{
    struct mm *mm = current_proc()->mm;
    uintptr_t want = a[0], old = mm->brk;

    if (!want || want < mm->brk_start || want >= USER_MMAP_BASE)
        return old;
    uintptr_t old_top = PAGE_ALIGN(old), new_top = PAGE_ALIGN(want);
    if (new_top > old_top) {
        struct vma *heap = old_top > mm->brk_start ? vma_find(mm, old_top - 1) : NULL;
        struct vma *next = vma_find(mm, new_top - 1);
        if (next && next != heap)
            return old;                     /* would run into another mapping */
        if (heap && heap->prot != (VM_READ | VM_WRITE))
            heap = NULL;                    /* mprotect changed the top page: start a new VMA */
        if (heap) {
            unsigned long flags = spin_lock_irqsave(&mm->vma_lock);
            heap->end = new_top;
            spin_unlock_irqrestore(&mm->vma_lock, flags);
        } else if (vma_add(mm, MAX(old_top, mm->brk_start), new_top, VM_READ | VM_WRITE, VMA_HEAP, "heap"))
            return old;
    } else if (new_top < old_top) {
        vma_unmap(mm, new_top, old_top);
    }
    mm->brk = want;
    return want;
}

long do_mmap(const u64 *a)
{
    struct mm *mm = current_proc()->mm;
    size_t len = PAGE_ALIGN(a[1]);
    u64 prot = a[2], flags = a[3];
    if (!len || !(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE))
        return -EINVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EACCES;                     /* W^X for user memory too */
    uintptr_t start = mm->mmap_next;
    if (start + len > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)
        return -ENOMEM;
    u32 vprot = (prot & PROT_READ ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) |
                (prot & PROT_EXEC ? VM_EXEC : 0);
    long r = vma_add(mm, start, start + len, vprot, VMA_ANON, "anon");
    if (r)
        return r;
    mm->mmap_next = start + len + PAGE_SIZE;   /* leave a guard gap between mappings */
    return start;
}

long do_munmap(const u64 *a)
{
    if (!user_range_ok(a[0], a[1]))
        return -EINVAL;
    return vma_unmap(current_proc()->mm, a[0], a[0] + PAGE_ALIGN(a[1]));
}

long do_mprotect(const u64 *a)
{
    u64 prot = a[2];
    if (a[0] & (PAGE_SIZE - 1) || !user_range_ok(a[0], a[1]) || (prot & ~7UL))
        return -EINVAL;
    if (!a[1])
        return 0;                           /* as in Linux */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EACCES;                     /* W^X */
    u32 vprot = (prot & PROT_READ ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) |
                (prot & PROT_EXEC ? VM_EXEC : 0);
    return vma_protect(current_proc()->mm, a[0], a[0] + PAGE_ALIGN(a[1]), vprot);
}

long do_sleep_ms(const u64 *a)
{
    sleep_ms(a[0]);
    return signal_pending(current_proc()) ? -EINTR : 0;
}

/* reboot(how): RB_AUTOBOOT restarts the machine (QEMU runs it again: OpenSBI,
   then the boot loader), RB_POWER_OFF stops it. Root only. Syncs first, so
   every write that has returned is on disk. */
long do_reboot(const u64 *a)
{
    struct proc *p = current_proc();
    if (p->euid != 0)
        return -EPERM;
    if (a[0] != RB_AUTOBOOT && a[0] != RB_POWER_OFF)
        return -EINVAL;
    pr_info("%s requested by pid %d (%s)\n", a[0] == RB_AUTOBOOT ? "reboot" : "power off", p->pid,
            p->name);
    vfs_sync_all();
    if (a[0] == RB_AUTOBOOT)
        sbi_reboot();
    sbi_shutdown(false);
}

long do_uptime_ms(const u64 *a)
{
    (void)a;
    return rdtime() * 1000 / platform.timebase;
}

long do_debug(const u64 *a)
{
    struct proc *p = current_proc();
    switch (a[0]) {
    case DBG_MARK: {
        char msg[25] = { 0 };
        long r = strncpy_from_user(msg, a[1], sizeof(msg));
        if (r < 0 && r != -ENAMETOOLONG)
            return r;
        u64 w[3] = { 0, 0, 0 };
        memcpy(w, msg, 24);
        trace(umark, w[0], w[1], w[2]);
        pr_info("[%d %s] mark: %s\n", p->pid, p->name, msg);
        return 0;
    }
    case DBG_BREAK: {
        char reason[64];
        ksnprintf(reason, sizeof(reason), "debug break from pid %d (%s)", p->pid, p->name);
        monitor_enter(reason, thread_user_tf(current), true);
        return 0;
    }
    case DBG_STRACE:
        p->strace = (int)a[1];
        return 0;
    case DBG_PS:
        proc_list_print(kprintf);
        return 0;
    case DBG_FILES:
        files_print(kprintf, p);
        return 0;
    case DBG_FSCRASH: {
        static char point[16];
        long r = strncpy_from_user(point, a[1], sizeof(point));
        if (r < 0)
            return r;
        return efs_arm_crash(point);
    }
    case DBG_VM:
        mm_print(kprintf, p->mm);
        return 0;
    case DBG_PROFILE:
        switch (a[1]) {
        case 0:
            profile_stop();
            return 0;
        case 1:
            return profile_start((u32)a[2]) ? -ENOMEM : 0;
        case 2:
            profile_clear();
            return 0;
        case 3: {
            char path[128];
            long r = strncpy_from_user(path, a[2], sizeof(path));
            if (r < 0)
                return r;
            return profile_dump_file(path) < 0 ? -EIO : 0;
        }
        }
        return -EINVAL;
    }
    return -EINVAL;
}

long do_fork(const u64 *a) { (void)a; return sys_fork(); }
long do_execve(const u64 *a) { return sys_execve(a[0], a[1], a[2]); }
long do_wait4(const u64 *a) { return sys_wait4((long)a[0], a[1], (long)a[2], a[3]); }

static const struct {
    sys_fn fn;
    const char *name;
    int nargs;
    const char *fmt;
} table[SYSCALL_MAX] = {
#define X(nr, name, nargs, fmt) [nr] = { do_##name, #name, nargs, fmt },
    SYSCALLS(X)
#undef X
};

const char *syscall_name(long nr)
{
    return nr >= 0 && nr < SYSCALL_MAX && table[nr].name ? table[nr].name : NULL;
}

const char *errno_name(long err)
{
    switch (err) {
#define X(name, num, desc) case num: return #name;
        ERRNOS(X)
#undef X
    }
    return "E?";
}

static const char *errno_desc(long err)
{
    switch (err) {
#define X(name, num, desc) case num: return desc;
        ERRNOS(X)
#undef X
    }
    return "unknown error";
}

/* ---- strace ---- */

static size_t put_escaped(char *out, size_t size, const char *s, size_t n, bool more)
{
    size_t len = 0;
    if (len < size) out[len++] = '"';
    for (size_t i = 0; i < n && len + 5 < size; i++) {
        unsigned char c = s[i];
        if (c == '\n') len += ksnprintf(out + len, size - len, "\\n");
        else if (c == '\t') len += ksnprintf(out + len, size - len, "\\t");
        else if (c == '"' || c == '\\') len += ksnprintf(out + len, size - len, "\\%c", c);
        else if (kisprint(c)) out[len++] = c;
        else len += ksnprintf(out + len, size - len, "\\x%02x", c);
    }
    len += ksnprintf(out + len, size - len, "\"%s", more ? "..." : "");
    return MIN(len, size);
}

static void strace_enter(struct proc *p, long nr, const u64 *a)
{
    char *buf = p->strace_buf;
    size_t size = sizeof(p->strace_buf), len;
    const char *fmt = table[nr].fmt;

    len = ksnprintf(buf, size, "%s(", table[nr].name);
    for (int i = 0; fmt[i] && len < size; i++) {
        if (i)
            len += ksnprintf(buf + len, size - len, ", ");
        char tmp[48];
        switch (fmt[i]) {
        case 'd': len += ksnprintf(buf + len, size - len, "%ld", (long)a[i]); break;
        case 'u': len += ksnprintf(buf + len, size - len, "%lu", (unsigned long)a[i]); break;
        case 'x': len += ksnprintf(buf + len, size - len, "%#lx", (unsigned long)a[i]); break;
        case 'p': len += ksnprintf(buf + len, size - len, "%#lx", (unsigned long)a[i]); break;
        case 'k': len += ksnprintf(buf + len, size - len, "%s", signal_name((int)a[i])); break;
        case 'o': len += ksnprintf(buf + len, size - len, "0%lo", (unsigned long)a[i]); break;
        case 's': {
            long r = strncpy_from_user(tmp, a[i], 40);
            if (r < 0 && r != -ENAMETOOLONG)
                len += ksnprintf(buf + len, size - len, "%#lx", (unsigned long)a[i]);
            else
                len += put_escaped(buf + len, size - len, tmp, strlen(tmp), r < 0);
            break;
        }
        case 'b': {
            size_t n = MIN((size_t)a[i + 1], (size_t)32);
            if (copy_from_user(tmp, a[i], n))
                len += ksnprintf(buf + len, size - len, "%#lx", (unsigned long)a[i]);
            else
                len += put_escaped(buf + len, size - len, tmp, n, a[i + 1] > n);
            break;
        }
        case 'n': {
            u8 sa[8];
            if (!a[i] || copy_from_user(sa, a[i], sizeof(sa)))
                len += ksnprintf(buf + len, size - len, "%#lx", (unsigned long)a[i]);
            else
                len += ksnprintf(buf + len, size - len, "{%u.%u.%u.%u:%u}", sa[4], sa[5], sa[6],
                                 sa[7], sa[2] << 8 | sa[3]);
            break;
        }
        case 'a': {
            len += ksnprintf(buf + len, size - len, "[");
            for (int j = 0; j < 6 && len < size; j++) {
                u64 ptr;
                if (copy_from_user(&ptr, a[i] + j * 8, 8) || !ptr)
                    break;
                long r = strncpy_from_user(tmp, ptr, 24);
                if (j)
                    len += ksnprintf(buf + len, size - len, ", ");
                len += put_escaped(buf + len, size - len, tmp, r < 0 ? 0 : strlen(tmp), r < 0);
            }
            len += ksnprintf(buf + len, size - len, "]");
            break;
        }
        }
    }
    if (len < size)
        ksnprintf(buf + len, size - len, ")");
    if (nr == SYS_exit)
        klog(LOG_INFO, SUBSYS_SYSCALL, "[%d %s] %s = ?\n", p->pid, p->name, buf);
}

static void strace_exit(struct proc *p, long nr, long ret)
{
    if (ret < 0 && ret > -4096)
        klog(LOG_INFO, SUBSYS_SYSCALL, "[%d %s] %s = -1 %s (%s)\n", p->pid, p->name,
             p->strace_buf, errno_name(-ret), errno_desc(-ret));
    else if (nr == SYS_brk || nr == SYS_mmap)
        klog(LOG_INFO, SUBSYS_SYSCALL, "[%d %s] %s = %#lx\n", p->pid, p->name, p->strace_buf,
             (unsigned long)ret);
    else
        klog(LOG_INFO, SUBSYS_SYSCALL, "[%d %s] %s = %ld\n", p->pid, p->name, p->strace_buf,
             nr == SYS_execve ? 0 : ret);
}

void syscall_dispatch(struct trap_frame *tf)
{
    struct proc *p = current_proc();
    long nr = (long)tf->regs[17];
    const u64 *a = &tf->regs[10];
    u64 args[6] = { a[0], a[1], a[2], a[3], a[4], a[5] };
    long ret;

    tf->sepc += 4;                      /* resume after the ecall */
    p->syscalls++;
    intr_on();
    trace(syscall_enter, nr, args[0], args[1]);

    if (nr < 0 || nr >= SYSCALL_MAX || !table[nr].fn) {
        pr_warn("[%d %s] unknown system call %ld\n", p->pid, p->name, nr);
        ret = -ENOSYS;
    } else {
        bool traced = p->strace;        /* the call itself may toggle strace */
        if (traced)
            strace_enter(p, nr, args);
        ret = table[nr].fn(args);
        if (traced)
            strace_exit(p, nr, ret);
    }
    trace(syscall_exit, nr, ret, 0);
    if (ret == -ERESTARTSYS) {          /* see signal.c: restart or EINTR */
        p->syscall_restart = true;
        p->restart_a0 = args[0];
    }
    tf->regs[10] = ret;
}
