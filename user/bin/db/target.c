/*
 * db: the debugged process, through ptrace.
 *
 * Breakpoints are c.ebreak (2 bytes, fits over any instruction). They are in
 * memory only while the process runs: inserted just before resuming and
 * taken out at every stop, so memory reads see the real code. A breakpoint
 * at the pc we resume from is stepped over first.
 *
 * Single-stepping is done in software (the kernel has no trace trap): decode
 * the instruction at pc, put temporary breakpoints on every instruction that
 * can come next (both sides of a branch, a jump's target), run, take them out.
 */
#include "db.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t pid;
int attached;
struct regs regs;
int stop_sig;
int cur_frame;
struct bp *bps;
int nbps;
static int next_bp_id = 1;

#define EBREAK_C 0x9002

/* ---- memory and registers ---- */

int read_mem(u64 addr, void *buf, size_t n)
{
    if (!pid)
        return -1;
    struct ptrace_io io = { addr, (u64)(uintptr_t)buf, n };
    return ptrace(PTRACE_READ, pid, 0, &io) < 0 ? -1 : 0;
}

int write_mem(u64 addr, const void *buf, size_t n)
{
    if (!pid)
        return -1;
    struct ptrace_io io = { addr, (u64)(uintptr_t)buf, n };
    return ptrace(PTRACE_WRITE, pid, 0, &io) < 0 ? -1 : 0;
}

int read_u64(u64 addr, u64 *v)
{
    return read_mem(addr, v, 8);
}

int fetch_regs(void)
{
    struct user_regs ur;
    if (ptrace(PTRACE_GETREGS, pid, 0, &ur) < 0)
        return -1;
    regs.pc = ur.r[0];
    regs.x[0] = 0;
    for (int i = 1; i < 32; i++)
        regs.x[i] = ur.r[i];
    return 0;
}

int store_regs(void)
{
    struct user_regs ur;
    ur.r[0] = regs.pc;
    for (int i = 1; i < 32; i++)
        ur.r[i] = regs.x[i];
    return ptrace(PTRACE_SETREGS, pid, 0, &ur) < 0 ? -1 : 0;
}

int fetch_fpregs(u64 f[32], u64 *fcsr)
{
    struct user_fpregs fr;
    if (ptrace(PTRACE_GETFPREGS, pid, 0, &fr) < 0)
        return -1;
    memcpy(f, fr.f, sizeof(fr.f));
    *fcsr = fr.fcsr;
    return 0;
}

int store_fpregs(const u64 f[32], u64 fcsr)
{
    struct user_fpregs fr;
    memcpy(fr.f, f, sizeof(fr.f));
    fr.fcsr = fcsr;
    return ptrace(PTRACE_SETFPREGS, pid, 0, &fr) < 0 ? -1 : 0;
}

static const char *const abi[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

const char *reg_name(int i)
{
    return i >= 0 && i < 32 ? abi[i] : "?";
}

int reg_number(const char *name)
{
    if (!strcmp(name, "pc"))
        return 32;
    if (!strcmp(name, "fp"))
        return 8;
    for (int i = 0; i < 32; i++)
        if (!strcmp(name, abi[i]))
            return i;
    if (name[0] == 'x' && name[1] >= '0' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 0 && n < 32)
            return n;
    }
    return -1;
}

/* ---- breakpoints ---- */

struct bp *bp_add(u64 addr, const char *where, int temp)
{
    bps = xrealloc(bps, (nbps + 1) * sizeof(struct bp));
    struct bp *b = &bps[nbps++];
    memset(b, 0, sizeof(*b));
    b->id = temp ? 0 : next_bp_id++;
    b->addr = addr;
    b->enabled = 1;
    b->temp = temp;
    b->where = xstrdup(where ? where : "");
    return b;
}

int bp_delete(int id)
{
    for (int i = 0; i < nbps; i++)
        if (bps[i].id == id) {
            free(bps[i].where);
            free(bps[i].cond);
            memmove(&bps[i], &bps[i + 1], (nbps - i - 1) * sizeof(struct bp));
            nbps--;
            return 0;
        }
    return -1;
}

struct bp *bp_at(u64 addr)
{
    for (int i = 0; i < nbps; i++)
        if (bps[i].addr == addr && bps[i].enabled)
            return &bps[i];
    return 0;
}

/* Temporary breakpoints for stepping: not in the user's list. */
struct tbp {
    u64 addr;
    u16 saved;
    int inserted;
};

static int insert_at(u64 addr, u16 *saved)
{
    u16 ins = EBREAK_C;
    if (read_mem(addr, saved, 2) < 0)
        return -1;
    return write_mem(addr, &ins, 2);
}

static void insert_all(u64 skip_pc, int skip)
{
    for (int i = 0; i < nbps; i++) {
        struct bp *b = &bps[i];
        if (!b->enabled || b->inserted || (skip && b->addr == skip_pc))
            continue;
        /* two breakpoints at one address: insert once */
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (bps[j].inserted && bps[j].addr == b->addr)
                dup = 1;
        if (dup)
            continue;
        if (insert_at(b->addr, &b->saved) == 0)
            b->inserted = 1;
        else
            err("cannot insert breakpoint %d at %#llx", b->id, (unsigned long long)b->addr);
    }
}

static void remove_all(void)
{
    /* in reverse, so overlapping saves restore in the right order */
    for (int i = nbps - 1; i >= 0; i--)
        if (bps[i].inserted) {
            write_mem(bps[i].addr, &bps[i].saved, 2);
            bps[i].inserted = 0;
        }
}

/* ---- running ---- */

int target_alive(void)
{
    return pid != 0;
}

static struct stop wait_stop(void)
{
    struct stop st;
    memset(&st, 0, sizeof(st));
    int status;
    for (;;) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR) {
            st.kind = STOP_ERROR;
            return st;
        }
    }
    if (WIFEXITED(status)) {
        st.kind = STOP_EXITED;
        st.status = WEXITSTATUS(status);
        pid = 0;
        return st;
    }
    if (WIFSIGNALED(status)) {
        st.kind = STOP_KILLED;
        st.sig = WTERMSIG(status);
        pid = 0;
        return st;
    }
    st.kind = STOP_SIGNAL;
    st.sig = WSTOPSIG(status);
    stop_sig = st.sig;
    fetch_regs();
    struct ptrace_siginfo si;
    if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0) {
        st.code = si.si_code;
        st.addr = si.si_addr;
    }
    cur_frame = 0;
    return st;
}

int target_run(char **argv)
{
    fflush(stdout);
    pid_t c = fork();
    if (c < 0) {
        err("fork: %s", strerror(errno));
        return -1;
    }
    if (c == 0) {
        signal(SIGINT, SIG_DFL);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
            fprintf(stderr, "db: ptrace(TRACEME): %s\n", strerror(errno));
            _exit(127);
        }
        execv(argv[0], argv);
        fprintf(stderr, "db: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    pid = c;
    attached = 0;
    struct stop st = wait_stop();
    if (st.kind != STOP_SIGNAL || st.sig != SIGTRAP) {
        if (st.kind == STOP_EXITED)
            err("the program exited (status %d) before it started", st.status);
        else
            err("the program did not stop after exec");
        if (pid)
            target_kill();
        return -1;
    }
    return 0;
}

int target_attach(int p)
{
    if (ptrace(PTRACE_ATTACH, p, 0, 0) < 0) {
        err("attach to %d: %s", p, strerror(errno));
        return -1;
    }
    pid = p;
    attached = 1;
    struct stop st = wait_stop();
    if (st.kind != STOP_SIGNAL) {
        err("process %d went away", p);
        pid = 0;
        return -1;
    }
    return 0;
}

void target_kill(void)
{
    if (!pid)
        return;
    remove_all();
    ptrace(PTRACE_KILL, pid, 0, 0);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    pid = 0;
}

void target_detach(void)
{
    if (!pid)
        return;
    remove_all();
    ptrace(PTRACE_DETACH, pid, 0, 0);
    pid = 0;
}

/* ---- instructions ---- */

static int read_insn(u64 pc, u32 *insn)
{
    u16 lo, hi;
    if (read_mem(pc, &lo, 2) < 0)
        return -1;
    if ((lo & 3) != 3) {
        *insn = lo;
        return 2;
    }
    if (read_mem(pc + 2, &hi, 2) < 0)
        return -1;
    *insn = lo | (u32)hi << 16;
    return 4;
}

int insn_length(u64 pc)
{
    u32 insn;
    int n = read_insn(pc, &insn);
    return n < 0 ? 4 : n;
}

static i64 sext(u64 v, int bits)
{
    u64 m = 1ULL << (bits - 1);
    return (i64)((v ^ m) - m);
}

/* The instructions that can run after the one at pc: 1 or 2 addresses. */
static int next_pcs(u64 pc, u64 out[2], int *is_call, u64 *call_ret)
{
    u32 insn;
    int len = read_insn(pc, &insn);
    *is_call = 0;
    if (len < 0)
        return 0;
    *call_ret = pc + len;
    if (len == 2) {
        u16 c = insn;
        int op = c & 3, f3 = c >> 13;
        if (op == 1 && f3 == 5) {                       /* c.j */
            u64 imm = ((c >> 12) & 1) << 11 | ((c >> 11) & 1) << 4 | ((c >> 9) & 3) << 8 |
                      ((c >> 8) & 1) << 10 | ((c >> 7) & 1) << 6 | ((c >> 6) & 1) << 7 |
                      ((c >> 3) & 7) << 1 | ((c >> 2) & 1) << 5;
            out[0] = pc + sext(imm, 12);
            return 1;
        }
        if (op == 1 && (f3 == 6 || f3 == 7)) {          /* c.beqz c.bnez */
            u64 imm = ((c >> 12) & 1) << 8 | ((c >> 10) & 3) << 3 | ((c >> 5) & 3) << 6 |
                      ((c >> 3) & 3) << 1 | ((c >> 2) & 1) << 5;
            out[0] = pc + 2;
            out[1] = pc + sext(imm, 9);
            return 2;
        }
        if (op == 2 && f3 == 4) {
            int rs1 = (c >> 7) & 31, rs2 = (c >> 2) & 31;
            if (rs2 == 0 && rs1 != 0) {                 /* c.jr c.jalr */
                out[0] = regs.x[rs1] & ~1ULL;
                *is_call = (c >> 12) & 1;
                return 1;
            }
        }
        out[0] = pc + 2;
        return 1;
    }
    int opcode = insn & 0x7f, rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31;
    if (opcode == 0x6f) {                               /* jal */
        u64 imm = ((insn >> 31) & 1) << 20 | ((insn >> 21) & 0x3ff) << 1 | ((insn >> 20) & 1) << 11 |
                  ((insn >> 12) & 0xff) << 12;
        out[0] = pc + sext(imm, 21);
        *is_call = rd == 1 || rd == 5;
        return 1;
    }
    if (opcode == 0x67) {                               /* jalr */
        out[0] = (regs.x[rs1] + sext(insn >> 20, 12)) & ~1ULL;
        *is_call = rd == 1 || rd == 5;
        return 1;
    }
    if (opcode == 0x63) {                               /* branches */
        u64 imm = ((insn >> 31) & 1) << 12 | ((insn >> 25) & 0x3f) << 5 | ((insn >> 8) & 15) << 1 |
                  ((insn >> 7) & 1) << 11;
        out[0] = pc + 4;
        out[1] = pc + sext(imm, 13);
        return out[1] == out[0] ? 1 : 2;
    }
    out[0] = pc + 4;
    return 1;
}

/* Run to one of the addresses (temporary breakpoints), with the user's
 * breakpoints in too if `with_bps`. */
static struct stop run_to(u64 *addrs, int n, int with_bps, int pass_sig)
{
    struct tbp t[4];
    int nt = 0;
    for (int i = 0; i < n && nt < 4; i++) {
        int dup = 0;
        for (int j = 0; j < nt; j++)
            if (t[j].addr == addrs[i])
                dup = 1;
        if (dup)
            continue;
        t[nt].addr = addrs[i];
        t[nt].inserted = 0;
        nt++;
    }
    if (with_bps)
        insert_all(0, 0);
    for (int i = 0; i < nt; i++) {
        struct bp *b = bp_at(t[i].addr);
        if (b && b->inserted)
            continue;               /* a user breakpoint is already there */
        if (insert_at(t[i].addr, &t[i].saved) == 0)
            t[i].inserted = 1;
    }
    struct stop st;
    if (ptrace(PTRACE_CONT, pid, 0, (void *)(long)pass_sig) < 0) {
        err("ptrace(CONT): %s", strerror(errno));
        memset(&st, 0, sizeof(st));
        st.kind = STOP_ERROR;
    } else {
        st = wait_stop();
    }
    if (pid) {
        for (int i = nt - 1; i >= 0; i--)
            if (t[i].inserted)
                write_mem(t[i].addr, &t[i].saved, 2);
        remove_all();
    } else {
        for (int i = 0; i < nbps; i++)
            bps[i].inserted = 0;
    }
    if (st.kind == STOP_SIGNAL && st.sig == SIGTRAP) {
        for (int i = 0; i < nt; i++)
            if (regs.pc == t[i].addr) {
                st.kind = STOP_STEP;
                return st;
            }
        struct bp *b = with_bps ? bp_at(regs.pc) : 0;
        if (b) {
            st.kind = STOP_BREAKPOINT;
            st.bp = b;
        }
    }
    return st;
}

/* One instruction, no user breakpoints (they're out of memory while stopped). */
static struct stop step_raw(int pass_sig)
{
    u64 next[2];
    int is_call;
    u64 ret;
    int n = next_pcs(regs.pc, next, &is_call, &ret);
    if (!n) {
        struct stop st;
        memset(&st, 0, sizeof(st));
        st.kind = STOP_ERROR;
        err("cannot read the instruction at %#llx", (unsigned long long)regs.pc);
        return st;
    }
    return run_to(next, n, 0, pass_sig);
}

static int pending_sig(void)
{
    /* pass on what stopped it, except our own traps and ^C */
    return stop_sig == SIGTRAP || stop_sig == SIGINT || stop_sig == SIGSTOP ? 0 : stop_sig;
}

/* Breakpoint hit: count it, apply its condition and ignore count. Returns 1 to stop. */
static int bp_should_stop(struct bp *b)
{
    b->hits++;
    if (b->cond) {
        struct value v;
        i64 x = 1;
        if (eval(b->cond, &v) == 0 && value_as_int(&v, &x) == 0 && x == 0) {
            b->hits--;
            return 0;
        }
    }
    if (b->ignore > 0) {
        b->ignore--;
        return 0;
    }
    return 1;
}

/* A user breakpoint stopped us: 1 to stop there, 0 to go on (condition false,
 * ignore count). A temporary breakpoint is deleted when it stops us. */
static int bp_hit(struct stop *st)
{
    struct bp *b = st->bp;
    if (!bp_should_stop(b))
        return 0;
    if (b->temp) {
        st->temp_id = b->id;
        for (int i = 0; i < nbps; i++)
            if (&bps[i] == b) {
                free(bps[i].where);
                free(bps[i].cond);
                memmove(&bps[i], &bps[i + 1], (nbps - i - 1) * sizeof(struct bp));
                nbps--;
                break;
            }
        st->bp = 0;                 /* still a breakpoint stop, not the step's target */
    }
    return 1;
}

struct stop resume_continue(int pass_sig)
{
    struct stop st;
    int sig = pass_sig;
    for (;;) {
        if (bp_at(regs.pc)) {
            /* off the breakpoint first, with it out of memory */
            st = step_raw(sig);
            sig = 0;
            if (st.kind != STOP_STEP)
                return st;
        }
        st = run_to(0, 0, 1, sig);
        sig = 0;
        if (st.kind == STOP_BREAKPOINT && !bp_hit(&st))
            continue;
        return st;
    }
}

/* ---- frames ---- */

/* Has the function at pc set up its frame pointer (and not yet given it up)? */
static int frame_is_set_up(u64 pc)
{
    struct func *f = func_at(pc);
    u64 start = f ? f->low : 0;
    if (!start) {
        struct sym *s = sym_at(pc);
        if (!s || !s->is_func)
            return 1;               /* unknown: assume a normal frame */
        start = s->addr;
    }
    /* the prologue: look for addi s0, sp, N (or c.addi4spn s0, sp, N) before pc */
    int found = 0;
    for (u64 a = start; a < pc && a < start + 96;) {
        u32 insn;
        int len = read_insn(a, &insn);
        if (len < 0)
            break;
        if ((len == 4 && (insn & 0xfffff) == 0x10413) ||
            (len == 2 && (insn & 0xe003) == 0 && ((insn >> 2) & 7) == 0 && insn != 0)) {
            found = 1;
            break;
        }
        a += len;
    }
    if (!found)
        return 0;
    /*
     * The epilogue: scanning ahead, a load of s0 before the ret means s0 is
     * still ours; a ret first means the load is behind us (s0 is the
     * caller's again, and ra holds the return address).
     */
    for (u64 a = pc; a < pc + 32;) {
        u32 insn;
        int len = read_insn(a, &insn);
        if (len < 0)
            break;
        if ((len == 4 && (insn & 0x7fff) == 0x3403) ||                  /* ld s0, N(rs) */
            (len == 2 && (insn & 0xe003) == 0x6002 && ((insn >> 7) & 31) == 8))   /* c.ldsp s0 */
            return 1;
        if ((len == 4 && insn == 0x00008067) || (len == 2 && insn == 0x8082))
            return 0;               /* ret */
        a += len;
    }
    return 1;
}

int unwind(struct frame *out, int max)
{
    if (!pid || max < 1)
        return 0;
    int n = 0;
    struct frame f;
    f.pc = regs.pc;
    f.sp = regs.x[2];
    u64 next_pc, next_fp;
    if (frame_is_set_up(regs.pc)) {
        f.fp = regs.x[8];
        f.cfa = f.fp;
        u64 ra, pfp;
        if (read_u64(f.fp - 8, &ra) || read_u64(f.fp - 16, &pfp)) {
            out[n++] = f;
            return n;
        }
        next_pc = ra;
        next_fp = pfp;
    } else {
        /* in a prologue or epilogue (or a frameless leaf): ra and s0 are the caller's */
        f.fp = regs.x[8];
        f.cfa = f.sp;
        next_pc = regs.x[1];
        next_fp = regs.x[8];
    }
    out[n++] = f;
    u64 prev_sp = f.cfa;
    while (n < max && next_pc && next_pc >= prog.text_start && next_pc < prog.text_end) {
        f.pc = next_pc;
        f.sp = prev_sp;
        f.fp = next_fp;
        f.cfa = next_fp;
        out[n++] = f;
        if (!next_fp || (next_fp & 7) || next_fp < prev_sp)
            break;
        u64 ra, pfp;
        if (read_u64(next_fp - 8, &ra) || read_u64(next_fp - 16, &pfp))
            break;
        prev_sp = next_fp;
        next_pc = ra;
        next_fp = pfp;
    }
    return n;
}

int frame_get(int k, struct frame *f)
{
    struct frame fr[64];
    int n = unwind(fr, 64);
    if (k < 0 || k >= n)
        return -1;
    *f = fr[k];
    return 0;
}

/* ---- stepping ---- */

struct stop step_insn(int over_calls)
{
    u64 next[2], ret;
    int is_call;
    int sig = pending_sig();
    int n = next_pcs(regs.pc, next, &is_call, &ret);
    if (over_calls && n == 1 && is_call) {
        u64 sp0 = regs.x[2];
        for (;;) {
            struct stop st;
            if (bp_at(regs.pc) || regs.pc == ret) {
                /* off this instruction first: a breakpoint here would trap at once */
                st = step_raw(sig);
                sig = 0;
                if (st.kind != STOP_STEP || (regs.pc == ret && regs.x[2] >= sp0))
                    return st;
            }
            st = run_to(&ret, 1, 1, sig);
            sig = 0;
            if (st.kind == STOP_BREAKPOINT && !bp_hit(&st))
                continue;
            /* a recursive call can hit the return address deeper down */
            if (st.kind == STOP_STEP && regs.x[2] < sp0)
                continue;
            return st;
        }
    }
    return step_raw(sig);
}

struct stop run_until(u64 addr)
{
    int sig = pending_sig();
    for (;;) {
        struct stop st;
        if (bp_at(regs.pc) || regs.pc == addr) {
            st = step_raw(sig);
            sig = 0;
            if (st.kind != STOP_STEP || regs.pc == addr)
                return st;
        }
        st = run_to(&addr, 1, 1, sig);
        sig = 0;
        if (st.kind == STOP_BREAKPOINT && !bp_hit(&st))
            continue;
        return st;
    }
}

struct stop step_out(u64 *a0, u64 *fa0, int *ok)
{
    struct frame fr[2];
    struct stop st;
    memset(&st, 0, sizeof(st));
    *ok = 0;
    int n = unwind(fr, 2);
    if (n < 2) {
        err("\"finish\" not meaningful in the outermost frame");
        st.kind = STOP_ERROR;
        return st;
    }
    u64 ret = fr[1].pc, cfa = fr[0].cfa;
    int sig = pending_sig();
    for (;;) {
        if (bp_at(regs.pc) || regs.pc == ret) {
            /* off this instruction first (the return address, after a recursive
               call came back to it deeper down, would trap at once) */
            st = step_raw(sig);
            sig = 0;
            if (st.kind != STOP_STEP)
                return st;
            if (regs.pc == ret && regs.x[2] >= cfa)
                break;
            continue;
        }
        st = run_to(&ret, 1, 1, sig);
        sig = 0;
        if (st.kind == STOP_BREAKPOINT && !bp_hit(&st))
            continue;
        if (st.kind == STOP_STEP && regs.x[2] < cfa)
            continue;               /* the same function, deeper in a recursion */
        break;
    }
    if (st.kind == STOP_STEP) {
        *ok = 1;
        *a0 = regs.x[10];
        u64 f[32], fcsr;
        *fa0 = fetch_fpregs(f, &fcsr) == 0 ? f[10] : 0;
    }
    return st;
}

/* The first statement after a function's prologue (where gdb puts a breakpoint). */
u64 after_prologue(u64 func_addr)
{
    struct func *f = func_at(func_addr);
    struct line_row *first = line_for(func_addr);
    if (!f || !first)
        return func_addr;
    /* the second line-table row inside the function */
    u64 best = 0;
    for (int i = 0; i < nrows; i++) {
        struct line_row *r = &rows[i];
        if (r->end_seq || r->addr <= func_addr || r->addr >= f->high)
            continue;
        if (r->line != first->line && (!best || r->addr < best))
            best = r->addr;
    }
    return best ? best : func_addr;
}

struct stop step_line(int over_calls)
{
    struct line_row *start = line_for(regs.pc);
    if (!start)
        return step_insn(over_calls);
    int start_line = start->line, start_file = start->file;
    struct func *start_fn = func_at(regs.pc);
    /* the starting frame: when it returns, stop (even into a caller that is
       the same function, in a recursion) */
    struct frame fr[2];
    int nfr = unwind(fr, 2);
    u64 start_ret = nfr == 2 ? fr[1].pc : 0, start_cfa = fr[0].cfa;
    for (int guard = 0; guard < 1000000; guard++) {
        u64 pc0 = regs.pc;
        struct stop st = step_insn(over_calls);
        if (st.kind != STOP_STEP)
            return st;
        struct func *fn = func_at(regs.pc);
        struct line_row *r = line_for(regs.pc);
        if (start_ret && regs.pc == start_ret && regs.x[2] >= start_cfa)
            return st;              /* returned: stop in the caller, mid-line */
        if (!r || !fn) {
            /* into code without line information (the C library): come back out */
            if (fn == start_fn && r)
                continue;
            u64 a0, fa0;
            int ok;
            if (!over_calls && regs.pc != pc0) {
                st = step_out(&a0, &fa0, &ok);
                if (st.kind != STOP_STEP)
                    return st;
                continue;
            }
            continue;
        }
        if (regs.pc == fn->low) {
            /* stepped into a function (the same one too, in a recursion):
               run past its prologue */
            u64 body = after_prologue(fn->low);
            if (body != regs.pc) {
                st = run_until(body);
                if (st.kind != STOP_STEP)
                    return st;
            }
            st.kind = STOP_STEP;
            return st;
        }
        if (fn != start_fn) {
            /* returned into the caller (in the middle of its line), or
               jumped somewhere else: stop here, as gdb does */
            return st;
        }
        if (r->addr == regs.pc && r->is_stmt && (r->line != start_line || r->file != start_file))
            return st;
        if (r->line != start_line && r->file == start_file && regs.pc < pc0)
            return st;              /* jumped back to a loop's head */
    }
    struct stop st;
    memset(&st, 0, sizeof(st));
    st.kind = STOP_STEP;
    return st;
}
