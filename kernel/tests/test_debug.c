#include <debug.h>
#include <kstring.h>
#include <ktest.h>
#include <mm.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <safe.h>
#include <trap.h>

void kmain(unsigned long hartid, uintptr_t dtb);

KTEST(ksyms_lookup)
{
    uintptr_t off = 99;
    KT_ASSERT(ksym_count() > 10);
    KT_EXPECT_STREQ(ksym_lookup((uintptr_t)kmain, &off), "kmain");
    KT_EXPECT_EQ(off, 0);
    KT_EXPECT_STREQ(ksym_lookup((uintptr_t)kmain + 4, &off), "kmain");
    KT_EXPECT_EQ(off, 4);
    KT_EXPECT_EQ(ksym_find("panic"), (uintptr_t)panic);
    KT_EXPECT(ksym_lookup(0x1000, NULL) == NULL);

    char buf[64];
    ksym_format(buf, sizeof(buf), (uintptr_t)panic + 0x10);
    KT_EXPECT_STREQ(buf, "panic+0x10");
}

/* Three real frames so the unwinder has something to walk. */
static int __noinline bt_level3(uintptr_t *pcs, int max)
{
    return backtrace_collect(current_fp(), pcs, max);
}
static int __noinline bt_level2(uintptr_t *pcs, int max)
{
    int n = bt_level3(pcs, max);
    asm volatile("" ::: "memory");   /* defeat tail-call */
    return n;
}
static int __noinline bt_level1(uintptr_t *pcs, int max)
{
    int n = bt_level2(pcs, max);
    asm volatile("" ::: "memory");
    return n;
}

KTEST(backtrace_walks_frames)
{
    uintptr_t pcs[BACKTRACE_MAX];
    int n = bt_level1(pcs, BACKTRACE_MAX);
    KT_ASSERT(n >= 4);
    KT_EXPECT_STREQ(ksym_lookup(pcs[0] - 1, NULL), "bt_level2");
    KT_EXPECT_STREQ(ksym_lookup(pcs[1] - 1, NULL), "bt_level1");
    KT_EXPECT_STREQ(ksym_lookup(pcs[2] - 1, NULL), "ktest_fn_backtrace_walks_frames");
    /* Tests run in the kinit thread: the chain must end at its entry point. */
    KT_EXPECT_STREQ(ksym_lookup(pcs[n - 1] - 1, NULL), "thread_entry");
}

KTEST(safe_access_recovers_from_faults)
{
    u64 v = 0;
    u64 before = exception_counts[EXC_LOAD_PAGE_FAULT];
    /* NULL and firmware memory are unmapped in the kernel page table. */
    KT_EXPECT_EQ(safe_read_u64(0, &v), -1);
    KT_EXPECT_EQ(safe_read_u64((uintptr_t)__va(platform.mem_base), &v), -1);
    KT_EXPECT_EQ(exception_counts[EXC_LOAD_PAGE_FAULT], before + 2);
    KT_EXPECT_EQ(safe_write_u32(0x10, 1), -1);

    u64 x = 0x1122334455667788ULL;
    KT_EXPECT_EQ(safe_read_u64((uintptr_t)&x, &v), 0);
    KT_EXPECT_EQ(v, x);
    KT_EXPECT_EQ(safe_write_u8((uintptr_t)&x, 0xaa), 0);
    KT_EXPECT_EQ(x, 0x11223344556677aaULL);
}

static int bp_hits;
static uintptr_t bp_pc;
static bool bp_hook(struct trap_frame *tf)
{
    bp_hits++;
    bp_pc = tf->sepc;
    tf->regs[10] = 0x5eed;      /* a0 visible to the code after the ebreak */
    return true;
}

KTEST(breakpoint_trap_and_resume)
{
    register unsigned long a0 asm("a0") = 1;
    register unsigned long s1 asm("s1") = 0xfeedface;   /* callee-saved survives */

    trap_breakpoint_hook = bp_hook;
    asm volatile("ebreak" : "+r"(a0), "+r"(s1) :: "memory");
    trap_breakpoint_hook = NULL;

    KT_EXPECT_EQ(bp_hits, 1);
    KT_EXPECT_EQ(a0, 0x5eed);
    KT_EXPECT_EQ(s1, 0xfeedface);
    KT_EXPECT_STREQ(ksym_lookup(bp_pc, NULL), "ktest_fn_breakpoint_trap_and_resume");
}

KTEST(device_tree)
{
    KT_EXPECT(platform.mem_size >= (16UL << 20));
    KT_EXPECT_EQ(platform.mem_base, 0x80000000UL);
    KT_EXPECT_EQ(platform.uart.base, 0x10000000UL);
    KT_EXPECT_EQ(platform.uart.irq, 10);
    KT_EXPECT_EQ(platform.timebase, 10000000);
    KT_EXPECT(platform.plic.base != 0);
    KT_EXPECT(platform.test.base != 0);
    KT_EXPECT_EQ(platform.nvirtio, 8);
    KT_EXPECT(platform.ncpus >= 1);
}

KTEST(cmdline)
{
    /* `make test` boots with "ktest panic=shutdown". */
    KT_EXPECT(cmdline_has("ktest"));
    KT_EXPECT_STREQ(cmdline_get("panic"), "shutdown");
    KT_EXPECT(cmdline_get("no-such-option") == NULL);
    KT_EXPECT(!cmdline_has("pan"));     /* prefix must not match */
}

KTEST(log_ring_and_thresholds)
{
    u8 saved = log_threshold[SUBSYS_TEST];

    u64 head = log_ring_head();
    klog(LOG_INFO, SUBSYS_TEST, "ring-marker-%d\n", 1234);
    KT_EXPECT(log_ring_head() > head);

    log_threshold[SUBSYS_TEST] = LOG_WARN;
    head = log_ring_head();
    klog(LOG_DEBUG, SUBSYS_TEST, "should be filtered\n");
    KT_EXPECT_EQ(log_ring_head(), head);

    log_threshold[SUBSYS_TEST] = saved;
    KT_EXPECT_EQ(log_subsys_lookup("mm"), SUBSYS_MM);
    KT_EXPECT_EQ(log_level_lookup("debug"), LOG_DEBUG);
    KT_EXPECT_EQ(log_level_lookup("3"), LOG_DEBUG);
    KT_EXPECT_EQ(log_level_lookup("bogus"), -1);
}
