#define LOG_SUBSYS SUBSYS_BOOT
#include <console.h>
#include <fs.h>
#include <virtio.h>
#include <irq.h>
#include <debug.h>
#include <ktest.h>
#include <lockdep.h>
#include <net.h>
#include <profile.h>
#include <mm.h>
#include <platform.h>
#include <proc.h>
#include <printk.h>
#include <sbi.h>
#include <sched.h>
#include <smp.h>
#include <trace.h>
#include <trap.h>

static void print_sbi_info(void)
{
    long spec = sbi_spec_version();
    long impl = sbi_impl_id();
    long ver = sbi_impl_version();
    pr_info("SBI v%ld.%ld, %s v%ld.%ld; extensions:%s%s%s%s\n",
            (spec >> 24) & 0x7f, spec & 0xffffff, sbi_impl_name(impl),
            ver >> 16, ver & 0xffff,
            sbi_probe(SBI_EXT_TIME) ? " TIME" : "",
            sbi_probe(SBI_EXT_IPI) ? " IPI" : "",
            sbi_probe(SBI_EXT_HSM) ? " HSM" : "",
            sbi_probe(SBI_EXT_SRST) ? " SRST" : "");
}

static int boot_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vklog(LOG_INFO, SUBSYS_BOOT, fmt, ap);
    va_end(ap);
    return 0;
}

void cpu_early_init(unsigned long hartid);

/*
 * First kernel thread. Options:
 *   ktest[=prefix]     run the in-kernel tests
 *   utest              run the user-space test suite (init -1 /bin/utests)
 *   tracedump=<path>   after tests, write the trace to the host (semihosting)
 *   init=<path>        program to run as pid 1 (default /bin/init)
 * With ktest/utest the machine powers off afterwards with the combined result.
 */
static void kinit(void *arg)
{
    (void)arg;
    if (blk_init() == 0)
        panic("no disk: the root filesystem lives on a virtio block device "
              "(make run attaches build/fs.img)");
    vfs_init();
    net_init();

    const char *tests = cmdline_get("ktest");
    bool utest = cmdline_has("utest");
    int failed = 0;

    if (tests)
        failed += ktest_run(tests) != 0;
    if (utest) {
        /* utest=<args> passes one argument to utests (e.g. persist-check). */
        const char *uarg = cmdline_get("utest");
        char *argv[] = { "init", "-1", "/bin/utests", *uarg ? (char *)uarg : NULL, NULL };
        struct proc *p = proc_spawn("/bin/init", argv);
        int status = p ? proc_wait_kernel(p) : -1;
        pr_info("user tests finished with status %#x\n", status);
        failed += status != 0;
    }
    if (tests || utest) {
        const char *dump = cmdline_get("tracedump");
        if (dump && *dump) {
            int n = trace_dump_file(dump);
            if (n < 0)
                pr_warn("tracedump: semihosting unavailable\n");
            else
                pr_info("trace written to %s (%d bytes)\n", dump, n);
        }
        platform_exit(failed ? 1 : 0);
    }

    const char *init = cmdline_get("init");
    if (!init || !*init)
        init = "/bin/init";
    char *argv[] = { (char *)init, NULL };
    pr_info("boot complete; starting %s (Ctrl-] enters the debug monitor)\n", init);
    if (!proc_spawn(init, argv))
        panic("cannot start %s", init);
}

void kmain(unsigned long hartid, uintptr_t dtb)
{
    cpu_early_init(hartid);
    trap_init();
    platform.boot_hart = hartid;

    int dt_err = fdt_parse(dtb);
    platform_init();
    console_init();
    cmdline_init(platform.bootargs);
    printk_apply_cmdline();

    pr_info("Eunuchs 0.1 (riscv64), built " __DATE__ " " __TIME__ "\n");
    if (dt_err)
        panic("cannot parse device tree at %#lx", (unsigned long)dtb);
    print_sbi_info();
    platform_print(boot_print);
    pr_info("kernel %#lx-%#lx (%lu KiB), %lu symbols\n",
            (unsigned long)__kernel_start, (unsigned long)__kernel_end,
            (unsigned long)(__kernel_end - __kernel_start) >> 10,
            (unsigned long)ksym_count());
    pr_info("command line: \"%s\"\n", cmdline_raw());
    cmdline_check();
    lockdep_init();

    if (cmdline_has("break"))
        monitor_enter("boot option 'break' (before memory management init)", NULL, true);

    pmm_init();
    vm_init();
    slab_init();
    trace_init();
    sched_init();
    proc_init();
    plic_init();
    tty_init();
    console_enable_irq();
    timer_init();
    smp_init();
    profile_boot_option();

    struct thread *t = kthread_create("kinit", kinit, NULL);
    if (!t)
        panic("cannot create kinit");
    kthread_detach(t);

    /* The boot context becomes cpu0's idle thread. */
    cpu_idle_loop();
}
