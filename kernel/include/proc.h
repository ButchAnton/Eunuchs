#ifndef EUNUCHS_PROC_H
#define EUNUCHS_PROC_H

/*
 * User processes and their address spaces.
 *
 * User virtual layout (Sv39 lower half):
 *   0x0000000000000000  unmapped (NULL guard)
 *   0x0000000000010000  program (ELF PT_LOAD segments), then the heap (brk)
 *   0x0000002000000000  mmap area (grows up)
 *   USER_STACK_TOP      stack: USER_STACK_SIZE below it, demand-paged,
 *                       with an unmapped gap under it (stack overflow -> SIGSEGV)
 *   0x0000004000000000  USER_TOP (end of the lower half)
 *
 * Debugging: `procs`, `vm <pid>`, `ubt <pid>`, `strace <pid>` in the monitor;
 * segfaults print a report with the faulting VMA (or lack of one), registers
 * and a symbolized user backtrace (from the program's ELF symbol table).
 */

#include <list.h>
#include <mm.h>
#include <sched.h>
#include <types.h>
#include <uapi/signal.h>

#define USER_TOP         0x0000004000000000UL
#define USER_STACK_TOP   0x0000003ffff00000UL
#define USER_STACK_SIZE  (8UL << 20)
#define USER_MMAP_BASE   0x0000002000000000UL
#define USER_MIN_ADDR    0x10000UL

#define PTE_COW (1UL << 8)      /* RSW bit: shared copy-on-write page */

/* VMA protections and kinds. */
#define VM_READ  1
#define VM_WRITE 2
#define VM_EXEC  4

enum vma_kind { VMA_ELF, VMA_HEAP, VMA_STACK, VMA_ANON };

struct vma {
    struct list_head node;          /* sorted by start */
    uintptr_t start, end;           /* [start, end), page aligned */
    u32 prot;
    enum vma_kind kind;
    char name[16];
};

struct mm {
    pagetable_t pt;
    /*
     * Only the owning process changes its mm. vma_lock serializes those
     * changes to the VMA list against readers from other processes
     * (/proc/<pid>/maps, which also holds proc_lock to keep the mm alive).
     */
    spinlock_t vma_lock;
    struct list_head vmas;
    uintptr_t brk_start, brk;
    uintptr_t mmap_next;
    u64 rss;                        /* resident pages */
    u64 faults, cow_copies;         /* statistics */
    u32 kernel_gen;                 /* kernel_root_gen when the upper half was synced */
};

/* Fault access types */
#define FAULT_READ  1
#define FAULT_WRITE 2
#define FAULT_EXEC  4

/* uvm.c */
struct mm *mm_create(void);
void mm_destroy(struct mm *mm);
struct mm *mm_fork(struct mm *parent);
struct vma *vma_find(struct mm *mm, uintptr_t addr);
int vma_add(struct mm *mm, uintptr_t start, uintptr_t end, u32 prot, enum vma_kind kind,
            const char *name);
int vma_unmap(struct mm *mm, uintptr_t start, uintptr_t end);
int uvm_fault(struct mm *mm, uintptr_t addr, int access);
void *uvm_populate(struct mm *mm, uintptr_t va);   /* map a zeroed page; kernel VA of it */
int uvm_write(struct mm *mm, uintptr_t dst, const void *src, size_t n);
int uvm_read(struct mm *mm, uintptr_t src, void *dst, size_t n);  /* no faulting in */
void vm_activate(struct mm *mm);                    /* NULL: kernel page table */
void mm_print(int (*print)(const char *fmt, ...), struct mm *mm);
const char *vma_kind_name(enum vma_kind k);

/* uaccess.S / uaccess.c: the only way the kernel touches user memory. */
long copy_from_user(void *dst, uintptr_t usrc, size_t n);  /* 0 or -EFAULT */
long copy_to_user(uintptr_t udst, const void *src, size_t n);
long strncpy_from_user(char *dst, uintptr_t usrc, size_t max);  /* length or -errno */
static inline bool user_range_ok(uintptr_t addr, size_t n)
{
    return addr < USER_TOP && n <= USER_TOP - addr;
}

/* ---- processes ---- */

enum proc_state { P_ALIVE, P_ZOMBIE };

struct ksigaction {
    uintptr_t handler;              /* SIG_DFL (0), SIG_IGN (1) or a user address */
    u64 flags;
    uintptr_t restorer;
    u32 mask;
};


struct file;
struct inode;

struct exec_image {
    int refcnt;
    void *data;
    size_t size;
    char path[64];
};

struct proc {
    int pid;
    char name[16];
    enum proc_state state;
    struct thread *thread;          /* single-threaded for now */
    struct mm *mm;
    struct proc *parent;
    struct list_head children;
    struct list_head sibling;
    struct list_head all_node;
    struct waitqueue child_wq;      /* parent sleeps here in wait4 */
    struct waitqueue exit_wq;       /* kernel waiters (proc_wait_kernel) */
    int exit_status;                /* wait4 encoding */
    int strace;                     /* 0 off, 1 on, 2 on + follow forks */
    struct exec_image *exe;         /* program file, for symbolization */
    const void *image;              /* = exe->data */
    size_t image_size;
    struct file *files[32];         /* NOFILE */
    u8 fd_flags[32];                /* FD_CLOEXEC */
    struct inode *cwd;
    u64 syscalls;
    u64 started;                    /* rdtime */
    char cmdline[256];              /* argv, NUL-separated (truncated): /proc/<pid>/cmdline */
    u16 cmdline_len;

    /* Signals (signal.c) */
    u32 sig_pending;
    u32 sig_blocked;
    u32 saved_blocked;              /* sigsuspend: restored after the handler */
    bool restore_blocked;
    struct ksigaction sigactions[32];
    bool stopped;                   /* by SIGSTOP/TSTP/TTIN/TTOU */
    int stop_sig;
    bool stop_reported;             /* to the parent's wait4(WUNTRACED) */
    u32 cont_seq;                   /* SIGCONT/SIGKILL count: a stop signal dequeued
                                       before one of these must not stop us */
    struct waitqueue stop_wq;
    u64 signals_received;
    bool syscall_restart;           /* last syscall returned -ERESTARTSYS */
    u64 restart_a0;
    bool nanosleep_restarting;      /* restarted nanosleep: sleep until the old deadline */
    u64 nanosleep_deadline;         /* rdtime */

    /* Credentials */
    u16 uid, euid, suid;
    u16 gid, egid, sgid;
    u16 umask;

    /* Job control */
    int pgid;
    int sid;
    struct tty *ctty;               /* controlling terminal */
    char strace_buf[192];           /* syscall being traced */
};

#define WEXITSTATUS_ENC(code)  (((code) & 0xff) << 8)
#define WSIGNAL_ENC(sig)       ((sig) & 0x7f)
#define WCOREFLAG              0x80
#define WSTOPPED_ENC(sig)      (((sig) << 8) | 0x7f)


static inline struct proc *current_proc(void)
{
    return current ? current->proc : NULL;
}

struct trap_frame;
struct trap_frame *thread_user_tf(struct thread *t);

/* proc.c; see proc_lock there for what it protects */
extern spinlock_t proc_lock;
extern struct list_head all_procs;
void proc_init(void);
struct proc *proc_spawn(const char *path, char *const argv[]);   /* from the kernel */
int proc_wait_kernel(struct proc *p);
void proc_exit(int status) __noreturn;
struct proc *proc_find(int pid);            /* proc_lock held (or world stopped) */
bool proc_exists(int pid);
int proc_nth_pid(int n);                    /* 0 if fewer processes */
void proc_list_print(int (*print)(const char *fmt, ...));
int proc_user_backtrace(int (*print)(const char *fmt, ...), struct proc *p,
                        uintptr_t pc, uintptr_t fp);
/* A user fault: deliver `sig` if the program handles it, else report and kill. */
void user_fault(struct trap_frame *tf, const char *what, int sig);

/* signal.c */
int signal_send(struct proc *p, int sig, int from_pid);         /* takes proc_lock */
int signal_send_locked(struct proc *p, int sig, int from_pid);  /* proc_lock held */
int signal_send_pgrp(int pgid, int sig, int from_pid);
bool signal_pending(struct proc *p);
void signal_deliver(struct trap_frame *tf);     /* on the way back to user mode */
bool signal_handled(struct proc *p, int sig);   /* has a handler and not blocked */
const char *signal_name(int sig);
void signal_fork(struct proc *child, struct proc *parent);
void signal_exec(struct proc *p);
void signal_notify_parent(struct proc *p, int code);
void syscall_restart_check(struct trap_frame *tf);

long sys_fork(void);
long sys_execve(uintptr_t upath, uintptr_t uargv, uintptr_t uenvp);
long sys_wait4(long pid, uintptr_t ustatus, long options, uintptr_t rusage);

/* exec.c / elf.c */
int elf_load(struct mm *mm, const u8 *img, size_t size, uintptr_t *entry, uintptr_t *end,
             char *err, size_t errlen);
const char *elf_symbolize(const void *img, size_t size, uintptr_t addr, uintptr_t *off);

struct proc *proc_nth(int n);               /* proc_lock held */

/*
 * Kernel-internal: "interrupted by a signal; restart unless a handler without
 * SA_RESTART runs" (then the caller sees EINTR). Never reaches user space.
 */
#define ERESTARTSYS 512
/* syscall.c */
void syscall_dispatch(struct trap_frame *tf);
const char *syscall_name(long nr);
const char *errno_name(long err);

/* trapentry.S */
void user_return(struct trap_frame *tf) __noreturn;

#endif
